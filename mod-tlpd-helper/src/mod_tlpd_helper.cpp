/*
 * mod-tlpd-helper
 *
 * Intercepts TLPD (32491) and Vyragosa (32630) spawns so that the
 * spawn-visibility timer can be configured.  Also provides GM commands:
 *   .tlpd status                                    - see state of each tracked creature
 *   .tlpd reveal [tlpd|vyragosa|both]               - make hidden creature(s) visible now
 *   .tlpd log [tlpd|vyragosa|both]                  - show last-visible + last-death times (persisted in DB across restarts)
 *   .tlpd spawns                                    - list all DB spawn slots with their 1-based indices
 *   .tlpd forcespawn <tlpd|vyragosa> [<slot>] [instant] [forced]
 *                                                   - summon a new creature into the spawn cycle;
 *                                                     <slot> targets a specific slot (see .tlpd spawns);
 *                                                     "instant" skips the hide timer;
 *                                                     "forced" spawns even if one is already active
 *   .tlpd nextspawn [tlpd|vyragosa|both]            - show estimated next spawn/reveal window
 *   .tlpd teleport <guid>                           - teleport to a tracked creature by guid
 */

#include "AllCreatureScript.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Duration.h"
#include "MapMgr.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "RBAC.h"
#include "TemporarySummon.h"
#include "UnitScript.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <ctime>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

using namespace Acore::ChatCommands;

constexpr uint32 NPC_TIME_LOST_PROTO_DRAKE = 32491;
constexpr uint32 NPC_VYRAGOSA             = 32630;

// Matches ACTION_TLPD_REVEAL in zone_storm_peaks.cpp
constexpr int32 ACTION_TLPD_REVEAL = 1;

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
namespace TLPDHelperConfig
{
    bool   Enable               = true;
    bool   AnnounceOnSpawn      = false;
    uint32 SpawnTimerMinSeconds = 0;
    uint32 SpawnTimerMaxSeconds = 0;
    // Must match creature.spawntimesecs for TLPD/Vyragosa so that .tlpd nextspawn
    // can accurately estimate the pool-respawn window after a death.
    uint32 PoolRespawnSeconds   = 21600;

    void Load()
    {
        Enable               = sConfigMgr->GetOption<bool>  ("TLPDHelper.Enable",               true);
        AnnounceOnSpawn      = sConfigMgr->GetOption<bool>  ("TLPDHelper.AnnounceOnSpawn",      false);
        SpawnTimerMinSeconds = sConfigMgr->GetOption<uint32>("TLPDHelper.SpawnTimerMinSeconds", 0);
        SpawnTimerMaxSeconds = sConfigMgr->GetOption<uint32>("TLPDHelper.SpawnTimerMaxSeconds", 0);
        PoolRespawnSeconds   = sConfigMgr->GetOption<uint32>("TLPDHelper.PoolRespawnSeconds",   21600);

        if (SpawnTimerMinSeconds > SpawnTimerMaxSeconds && SpawnTimerMinSeconds > 0)
        {
            LOG_WARN("module", "mod-tlpd-helper: SpawnTimerMinSeconds > SpawnTimerMaxSeconds - swapping values.");
            std::swap(SpawnTimerMinSeconds, SpawnTimerMaxSeconds);
        }
    }
} // namespace TLPDHelperConfig

// Forward declaration - defined after kNamedSpawns below.
static char const* GetSpawnName(float x, float y);

// Spawn slot loaded from the creature + creature_addon tables at startup.
struct TLPDSpawnSlot
{
    Position pos;
    uint32   pathId;
};

// ---------------------------------------------------------------------------
// Creature GUID tracker (thread-safe; guids accessed from map + session threads)
// ---------------------------------------------------------------------------
class TLPDTracker
{
public:
    static TLPDTracker& Instance()
    {
        static TLPDTracker tracker;
        return tracker;
    }

    void Register(ObjectGuid guid, float x, float y)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _guids.insert(guid);
        _spawnPos[guid] = {x, y};
    }

    void Unregister(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _guids.erase(guid);
        _spawnPos.erase(guid);
        _revealTime.erase(guid);
    }

    void SetRevealTime(ObjectGuid guid, time_t t)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _revealTime[guid] = t;
    }

    void ClearRevealTime(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _revealTime.erase(guid);
    }

    Optional<time_t> GetRevealTime(ObjectGuid guid) const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _revealTime.find(guid);
        return (it != _revealTime.end()) ? Optional<time_t>{it->second} : std::nullopt;
    }

    // Returns the nearest named spawn location for the given tracked guid.
    char const* GetSpawnName(ObjectGuid guid) const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _spawnPos.find(guid);
        if (it == _spawnPos.end())
            return "Unknown";
        return ::GetSpawnName(it->second.first, it->second.second);
    }

    void RecordVisible(uint32 entry)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _lastVisible[entry] = time(nullptr);
    }

    void RecordDeath(uint32 entry)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _lastDeath[entry] = time(nullptr);
    }

    // Returns nullopt if the entry has never become visible (even across restarts).
    Optional<time_t> GetLastVisible(uint32 entry) const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _lastVisible.find(entry);
        return (it != _lastVisible.end()) ? Optional<time_t>{it->second} : std::nullopt;
    }

    // Returns nullopt if the entry has never died (even across restarts).
    Optional<time_t> GetLastDeath(uint32 entry) const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _lastDeath.find(entry);
        return (it != _lastDeath.end()) ? Optional<time_t>{it->second} : std::nullopt;
    }

    // Returns a snapshot so callers can iterate without holding the lock.
    std::unordered_set<ObjectGuid> Snapshot() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _guids;
    }

    // Queries creature + creature_addon for all TLPD spawn slots on Northrend.
    // Called once during OnLoadCustomDatabaseTable so the world DB is ready.
    void LoadSpawnSlotsFromDB()
    {
        QueryResult result = WorldDatabase.Query(
            "SELECT c.position_x, c.position_y, c.position_z, c.orientation, ca.path_id "
            "FROM creature c "
            "INNER JOIN creature_addon ca ON c.guid = ca.guid "
            "WHERE c.id1 = {} AND c.map = {} AND c.MovementType = 2 AND ca.path_id > 0 "
            "ORDER BY c.guid",
            NPC_TIME_LOST_PROTO_DRAKE, MAP_NORTHREND);

        std::lock_guard<std::mutex> lock(_mutex);
        _spawnSlots.clear();

        if (!result)
        {
            LOG_WARN("module", "mod-tlpd-helper: no TLPD waypoint spawn slots found in DB - .tlpd forcespawn disabled.");
            return;
        }

        do
        {
            Field* fields = result->Fetch();
            _spawnSlots.push_back({
                Position(fields[0].Get<float>(), fields[1].Get<float>(),
                         fields[2].Get<float>(), fields[3].Get<float>()),
                fields[4].Get<uint32>()
            });
        } while (result->NextRow());

        LOG_INFO("module", "mod-tlpd-helper: loaded {} spawn slot(s) from DB.", _spawnSlots.size());
    }

    std::vector<TLPDSpawnSlot> GetSpawnSlots() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _spawnSlots;
    }

private:
    mutable std::mutex                                        _mutex;
    std::unordered_set<ObjectGuid>                          _guids;
    std::unordered_map<ObjectGuid, std::pair<float, float>> _spawnPos;    // guid → (x, y) at spawn time
    std::unordered_map<ObjectGuid, time_t>                  _revealTime;  // guid → scheduled reveal time (unix epoch)
    std::unordered_map<uint32, time_t>                      _lastVisible; // entry → last time became visible (backed by DB)
    std::unordered_map<uint32, time_t>                      _lastDeath;   // entry → last death time (backed by DB)
    std::vector<TLPDSpawnSlot>                              _spawnSlots;  // loaded from DB at startup
};

// Human-readable names for each spawn point, matched by nearest XY distance.
struct NamedSpawn { float x, y; char const* name; };
static NamedSpawn const kNamedSpawns[] =
{
    { 6821.0693f, -1800.8033f, "Brunnhildar"     },
    { 6455.72f,   -562.87f, "Waterfall (path2)" },
    { 6550.9775f, -671.7839f, "Waterfall (path3)" },
    { 6880.4116f, -398.9924f, "Frozen Lake"     },
    { 8545.776f, -1879.396f, "Ulduar"           },
};

static char const* GetSpawnName(float x, float y)
{
    float bestDist = std::numeric_limits<float>::max();
    char const* bestName = "Unknown";
    for (NamedSpawn const& n : kNamedSpawns)
    {
        float dx = x - n.x, dy = y - n.y;
        float dist = dx * dx + dy * dy;
        if (dist < bestDist)
        {
            bestDist = dist;
            bestName = n.name;
        }
    }
    return bestName;
}

// ---------------------------------------------------------------------------
// AllCreatureScript - intercepts TLPD / Vyragosa world-add events
// ---------------------------------------------------------------------------
class TLPDHelperCreatureScript : public AllCreatureScript
{
public:
    TLPDHelperCreatureScript() : AllCreatureScript("TLPDHelperCreatureScript") {}

    // Fired after the creature is added to the world and its AI has been
    // initialized (i.e. the default hidden-timer event is already queued).
    void OnCreatureAddWorld(Creature* creature) override
    {
        if (!TLPDHelperConfig::Enable)
            return;

        if (creature->GetEntry() != NPC_TIME_LOST_PROTO_DRAKE &&
            creature->GetEntry() != NPC_VYRAGOSA)
            return;

        TLPDTracker::Instance().Register(creature->GetGUID(), creature->GetPositionX(), creature->GetPositionY());

        // Kill the vanilla 6-22 h event queued by npc_time_lost_proto_drakeAI::InitializeAI
        // and replace it with our configured range.
        creature->m_Events.KillAllEvents(false);

        bool   const announce    = TLPDHelperConfig::AnnounceOnSpawn;
        std::string  name        = creature->GetName();
        uint32 const entry       = creature->GetEntry();
        ObjectGuid const cGuid   = creature->GetGUID();

        uint32 minSecs = TLPDHelperConfig::SpawnTimerMinSeconds;
        uint32 maxSecs = TLPDHelperConfig::SpawnTimerMaxSeconds;

        // 0/0 means use the same formula as the core: urand(0, 16h).
        // Combined with the 6h spawntimesecs this gives 6-22h total respawn.
        if (minSecs == 0 && maxSecs == 0)
            maxSecs = 60 * 60 * 16;

        uint32 const delaySecs = (minSecs >= maxSecs) ? minSecs : urand(minSecs, maxSecs);
        Milliseconds const delay    = Seconds(delaySecs);
        time_t const revealAt       = time(nullptr) + static_cast<time_t>(delaySecs);

        LOG_DEBUG("module", "mod-tlpd-helper: {} entered world, will become visible in ~{} second(s).",
            creature->GetName(), delaySecs);

        creature->m_Events.AddEventAtOffset(
            [creature, announce, name, entry, cGuid]()
            {
                if (creature->AI())
                    creature->AI()->DoAction(ACTION_TLPD_REVEAL);

                TLPDTracker::Instance().ClearRevealTime(cGuid);
                TLPDTracker::Instance().RecordVisible(entry);

                if (announce)
                {
                    std::string msg = Acore::StringFormat(
                        "[TLPD Hunter] |cffff6600{}|r has appeared in the Storm Peaks! The hunt is on!",
                        name);
                    sWorldSessionMgr->SendServerMessage(SERVER_MSG_STRING, msg);
                }

                time_t const visibleAt = time(nullptr);
                char timeBuf[32];
                struct tm ltm{};
#ifdef _WIN32
                localtime_s(&ltm, &visibleAt);
#else
                localtime_r(&visibleAt, &ltm);
#endif
                std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &ltm);
                LOG_INFO("module", "mod-tlpd-helper: [{}] {} is now visible.", timeBuf, name);
            },
            delay);

        TLPDTracker::Instance().SetRevealTime(creature->GetGUID(), revealAt);
    }

    void OnCreatureRemoveWorld(Creature* creature) override
    {
        if (creature->GetEntry() != NPC_TIME_LOST_PROTO_DRAKE &&
            creature->GetEntry() != NPC_VYRAGOSA)
            return;

        TLPDTracker::Instance().Unregister(creature->GetGUID());
    }
};

// ---------------------------------------------------------------------------
// UnitScript - records death timestamps for TLPD / Vyragosa
// ---------------------------------------------------------------------------
class TLPDHelperUnitScript : public UnitScript
{
public:
    TLPDHelperUnitScript() : UnitScript("TLPDHelperUnitScript") {}

    void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
    {
        if (!TLPDHelperConfig::Enable)
            return;

        Creature* c = unit->ToCreature();
        if (!c)
            return;

        if (c->GetEntry() != NPC_TIME_LOST_PROTO_DRAKE &&
            c->GetEntry() != NPC_VYRAGOSA)
            return;

        TLPDTracker::Instance().RecordDeath(c->GetEntry());
        LOG_INFO("module", "mod-tlpd-helper: {} has died - death time recorded.", c->GetName());
    }
};

// ---------------------------------------------------------------------------
// CommandScript - .tlpd reveal / .tlpd status / .tlpd log / .tlpd forcespawn
// ---------------------------------------------------------------------------
class TLPDHelperCommandScript : public CommandScript
{
public:
    TLPDHelperCommandScript() : CommandScript("TLPDHelperCommandScript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable tlpdTable =
        {
            { "reveal",     HandleTLPDRevealCommand,     rbac::RBAC_PERM_COMMAND_NPC_ADD, Console::No },
            { "status",     HandleTLPDStatusCommand,     rbac::RBAC_PERM_COMMAND_NPC_ADD, Console::No },
            { "log",        HandleTLPDLogCommand,        rbac::RBAC_PERM_COMMAND_NPC_ADD, Console::No },
            { "nextspawn",  HandleTLPDNextSpawnCommand,  rbac::RBAC_PERM_COMMAND_NPC_ADD, Console::No },
            { "spawns",     HandleTLPDSpawnsCommand,     rbac::RBAC_PERM_COMMAND_NPC_ADD, Console::No },
            { "forcespawn", HandleTLPDForceSpawnCommand, rbac::RBAC_PERM_COMMAND_NPC_ADD, Console::No },
            { "teleport",   HandleTLPDTeleportCommand,   rbac::RBAC_PERM_COMMAND_NPC_ADD, Console::No },
        };
        static ChatCommandTable commandTable =
        {
            { "tlpd", tlpdTable },
        };
        return commandTable;
    }

    // .tlpd reveal [tlpd|vyragosa|both]
    static bool HandleTLPDRevealCommand(ChatHandler* handler, Optional<std::string> which)
    {
        if (!TLPDHelperConfig::Enable)
        {
            handler->SendErrorMessage("mod-tlpd-helper is disabled.");
            return false;
        }

        bool doTLPD     = true;
        bool doVyragosa = true;

        if (which)
        {
            std::string target = *which;
            std::transform(target.begin(), target.end(), target.begin(), ::tolower);

            if (target == "tlpd")
                doVyragosa = false;
            else if (target == "vyragosa")
                doTLPD = false;
            else if (target != "both")
            {
                handler->SendErrorMessage("Usage: .tlpd reveal [tlpd|vyragosa|both]");
                return false;
            }
        }

        Map* northrend = sMapMgr->FindBaseNonInstanceMap(MAP_NORTHREND);
        if (!northrend)
        {
            handler->SendErrorMessage("Northrend map is not loaded.");
            return false;
        }

        auto guids = TLPDTracker::Instance().Snapshot();
        if (guids.empty())
        {
            handler->SendErrorMessage("No TLPD/Vyragosa creatures tracked (are they spawned in the DB?).");
            return false;
        }

        uint32 revealed = 0;
        for (ObjectGuid const& guid : guids)
        {
            Creature* c = northrend->GetCreature(guid);
            if (!c)
                continue;

            if (!doTLPD && c->GetEntry() == NPC_TIME_LOST_PROTO_DRAKE)
                continue;
            if (!doVyragosa && c->GetEntry() == NPC_VYRAGOSA)
                continue;

            if (!c->IsVisible())
            {
                c->m_Events.KillAllEvents(false);
                c->AI()->DoAction(ACTION_TLPD_REVEAL);
                TLPDTracker::Instance().ClearRevealTime(c->GetGUID());
                handler->PSendSysMessage("{} has been revealed!", c->GetName());
                ++revealed;
            }
            else
            {
                handler->PSendSysMessage("{} is already visible.", c->GetName());
            }
        }

        if (revealed == 0)
            handler->SendSysMessage("All matching creatures are already visible (or not on the map).");

        return true;
    }

    // .tlpd status
    static bool HandleTLPDStatusCommand(ChatHandler* handler)
    {
        if (!TLPDHelperConfig::Enable)
        {
            handler->SendErrorMessage("mod-tlpd-helper is disabled.");
            return false;
        }

        auto guids = TLPDTracker::Instance().Snapshot();
        if (guids.empty())
        {
            handler->SendSysMessage("No TLPD/Vyragosa creatures are currently tracked.");
            return true;
        }

        Map* northrend = sMapMgr->FindBaseNonInstanceMap(MAP_NORTHREND);
        if (!northrend)
        {
            handler->SendErrorMessage("Northrend map is not loaded.");
            return false;
        }

        for (ObjectGuid const& guid : guids)
        {
            Creature* c = northrend->GetCreature(guid);
            if (!c)
            {
                handler->PSendSysMessage("GUID {} - not found on Northrend map.", guid.GetCounter());
                continue;
            }

            std::string stateStr;
            if (!c->IsAlive())
                stateStr = "DEAD";
            else if (!c->IsVisible())
            {
                auto revealTime = TLPDTracker::Instance().GetRevealTime(guid);
                if (revealTime)
                {
                    time_t remaining = *revealTime - time(nullptr);
                    if (remaining > 0)
                    {
                        if (remaining < 3600)
                            stateStr = Acore::StringFormat("hidden - reveals in {}m {}s",
                                remaining / 60, remaining % 60);
                        else
                            stateStr = Acore::StringFormat("hidden - reveals in {}h {}m",
                                remaining / 3600, (remaining % 3600) / 60);
                    }
                    else
                        stateStr = "hidden (reveal imminent)";
                }
                else
                    stateStr = "hidden (spawn timer active)";
            }
            else
                stateStr = "VISIBLE \u2013 up for grabs!";

            handler->PSendSysMessage("[{}] entry={} guid={} | {} | {}",
                c->GetName(), c->GetEntry(), guid.GetCounter(),
                TLPDTracker::Instance().GetSpawnName(guid), stateStr);
            handler->PSendSysMessage(".go xyz {:.2f} {:.2f} {:.2f} 571",
                c->GetPositionX(), c->GetPositionY(), c->GetPositionZ());
        }

        return true;
    }

    // .tlpd log [tlpd|vyragosa|both]
    // Shows last-visible and last-death timestamps from the persistent DB log.
    static bool HandleTLPDLogCommand(ChatHandler* handler, Optional<std::string> which)
    {
        if (!TLPDHelperConfig::Enable)
        {
            handler->SendErrorMessage("mod-tlpd-helper is disabled.");
            return false;
        }

        bool doTLPD     = true;
        bool doVyragosa = true;

        if (which)
        {
            std::string target = *which;
            std::transform(target.begin(), target.end(), target.begin(), ::tolower);

            if (target == "tlpd")
                doVyragosa = false;
            else if (target == "vyragosa")
                doTLPD = false;
            else if (target != "both")
            {
                handler->SendErrorMessage("Usage: .tlpd log [tlpd|vyragosa|both]");
                return false;
            }
        }

        auto formatTime = [](Optional<time_t> ts, char const* neverMsg) -> std::string
        {
            if (!ts)
                return neverMsg;

            time_t now     = time(nullptr);
            time_t elapsed = now - *ts;

            char timeBuf[32];
            struct tm ltm {};
#ifdef _WIN32
            localtime_s(&ltm, &*ts);
#else
            localtime_r(&*ts, &ltm);
#endif
            std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &ltm);

            std::string ago;
            if (elapsed < 60)
                ago = Acore::StringFormat("{}s ago", elapsed);
            else if (elapsed < 3600)
                ago = Acore::StringFormat("{}m ago", elapsed / 60);
            else if (elapsed < 86400)
                ago = Acore::StringFormat("{}h {}m ago", elapsed / 3600, (elapsed % 3600) / 60);
            else
                ago = Acore::StringFormat("{}d {}h ago", elapsed / 86400, (elapsed % 86400) / 3600);

            return Acore::StringFormat("{} ({})", timeBuf, ago);
        };

        auto printEntry = [&](char const* label, uint32 entry)
        {
            std::string visible = formatTime(TLPDTracker::Instance().GetLastVisible(entry),
                                             "never visible (no record)");
            std::string died    = formatTime(TLPDTracker::Instance().GetLastDeath(entry),
                                             "never died (no record)");
            handler->PSendSysMessage("|cffff9900{}|r  last visible: {}  |  last death: {}",
                label, visible, died);
        };

        if (doTLPD)
            printEntry("TLPD",     NPC_TIME_LOST_PROTO_DRAKE);
        if (doVyragosa)
            printEntry("Vyragosa", NPC_VYRAGOSA);

        return true;
    }

    // .tlpd nextspawn [tlpd|vyragosa|both]
    // Shows the estimated next spawn/reveal window for TLPD and/or Vyragosa.
    // - Visible: says so immediately.
    // - Hidden with tracked reveal time: shows exact countdown.
    // - Dead / between pool cycles: estimates from last death + PoolRespawnSeconds + reveal range.
    static bool HandleTLPDNextSpawnCommand(ChatHandler* handler, Optional<std::string> which)
    {
        if (!TLPDHelperConfig::Enable)
        {
            handler->SendErrorMessage("mod-tlpd-helper is disabled.");
            return false;
        }

        bool doTLPD     = true;
        bool doVyragosa = true;

        if (which)
        {
            std::string target = *which;
            std::transform(target.begin(), target.end(), target.begin(), ::tolower);
            if (target == "tlpd")
                doVyragosa = false;
            else if (target == "vyragosa")
                doTLPD = false;
            else if (target != "both")
            {
                handler->SendErrorMessage("Usage: .tlpd nextspawn [tlpd|vyragosa|both]");
                return false;
            }
        }

        Map* northrend   = sMapMgr->FindBaseNonInstanceMap(MAP_NORTHREND);
        time_t const now = time(nullptr);

        auto fmtWallClock = [](time_t t) -> std::string
        {
            char buf[32];
            struct tm ltm{};
#ifdef _WIN32
            localtime_s(&ltm, &t);
#else
            localtime_r(&t, &ltm);
#endif
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ltm);
            return buf;
        };

        // Returns "Xh Ym Zs" with seconds precision for countdown displays.
        auto fmtCountdown = [](time_t diff) -> std::string
        {
            if (diff <= 0)
                return "now";
            if (diff < 60)
                return Acore::StringFormat("{}s", diff);
            if (diff < 3600)
                return Acore::StringFormat("{}m {}s", diff / 60, diff % 60);
            return Acore::StringFormat("{}h {}m {}s", diff / 3600, (diff % 3600) / 60, diff % 60);
        };

        auto fmtElapsed = [](time_t elapsed) -> std::string
        {
            if (elapsed < 60)
                return Acore::StringFormat("{}s ago", elapsed);
            if (elapsed < 3600)
                return Acore::StringFormat("{}m ago", elapsed / 60);
            return Acore::StringFormat("{}h {}m ago", elapsed / 3600, (elapsed % 3600) / 60);
        };

        // --- Per-NPC: report visible / hidden creatures that are currently tracked ---
        auto guids      = TLPDTracker::Instance().Snapshot();
        bool tlpdHandled = false;
        bool vyraHandled = false;

        for (ObjectGuid const& guid : guids)
        {
            Creature* c = northrend ? northrend->GetCreature(guid) : nullptr;
            if (!c)
                continue;

            bool const isTLPD = (c->GetEntry() == NPC_TIME_LOST_PROTO_DRAKE);
            bool const isVyra = (c->GetEntry() == NPC_VYRAGOSA);

            if (!doTLPD && isTLPD) continue;
            if (!doVyragosa && isVyra) continue;

            char const* label = isTLPD ? "TLPD" : "Vyragosa";

            if (c->IsVisible())
            {
                handler->PSendSysMessage(
                    "|cff00ff00[{}]|r VISIBLE NOW at '{}' (guid={}). Get in there!",
                    label, TLPDTracker::Instance().GetSpawnName(guid), guid.GetCounter());
            }
            else
            {
                auto revealTime = TLPDTracker::Instance().GetRevealTime(guid);
                if (revealTime && *revealTime > now)
                {
                    handler->PSendSysMessage(
                        "|cffffff00[{}]|r Hidden at '{}' - reveals in {} (at {}).",
                        label, TLPDTracker::Instance().GetSpawnName(guid),
                        fmtCountdown(*revealTime - now), fmtWallClock(*revealTime));
                }
                else
                    handler->PSendSysMessage("|cffffff00[{}]|r Hidden - reveal is imminent.", label);
            }

            if (isTLPD) tlpdHandled = true;
            if (isVyra) vyraHandled = true;
        }

        // --- Dead / between pool cycles: show the spawn window ONCE for the pair ---
        // Skip entirely if any creature is already tracked - the pool is active and the
        // status lines above already tell the full story. Use .tlpd log for death history.
        if (tlpdHandled || vyraHandled)
            return true;

        // Use whichever entry died most recently as the reference death time.
        auto tlpdDeath   = TLPDTracker::Instance().GetLastDeath(NPC_TIME_LOST_PROTO_DRAKE);
        auto vyraDeath   = TLPDTracker::Instance().GetLastDeath(NPC_VYRAGOSA);
        Optional<time_t> lastDeath;
        char const* deadLabel = "TLPD/Vyragosa";

        if (tlpdDeath && vyraDeath)
        {
            if (*tlpdDeath >= *vyraDeath) { lastDeath = tlpdDeath; deadLabel = "TLPD"; }
            else                          { lastDeath = vyraDeath; deadLabel = "Vyragosa"; }
        }
        else if (tlpdDeath) { lastDeath = tlpdDeath; deadLabel = "TLPD"; }
        else if (vyraDeath) { lastDeath = vyraDeath; deadLabel = "Vyragosa"; }

        if (!lastDeath)
        {
            handler->PSendSysMessage(
                "|cffaaaaaa[TLPD/Vyragosa]|r No death on record - creature may be on its initial hidden timer. Try .tlpd status.");
            return true;
        }

        time_t const poolRespawnAt = *lastDeath + static_cast<time_t>(TLPDHelperConfig::PoolRespawnSeconds);

        uint32 revealMinSecs = TLPDHelperConfig::SpawnTimerMinSeconds;
        uint32 revealMaxSecs = TLPDHelperConfig::SpawnTimerMaxSeconds;
        if (revealMinSecs == 0 && revealMaxSecs == 0)
            revealMaxSecs = 60 * 60 * 16;

        time_t const earliestVisible = poolRespawnAt + static_cast<time_t>(revealMinSecs);
        time_t const latestVisible   = poolRespawnAt + static_cast<time_t>(revealMaxSecs);

        if (poolRespawnAt > now)
        {
            handler->PSendSysMessage(
                "|cffff4444[{}]|r died {} | Pool respawns in {} (at {})",
                deadLabel, fmtElapsed(now - *lastDeath),
                fmtCountdown(poolRespawnAt - now), fmtWallClock(poolRespawnAt));
        }
        else
        {
            handler->PSendSysMessage(
                "|cffff9900[{}]|r died {} | Pool respawned at {} - creature not yet tracked.",
                deadLabel, fmtElapsed(now - *lastDeath), fmtWallClock(poolRespawnAt));
        }

        handler->PSendSysMessage(
            "  Visible window: {} to {} (between {} and {} from now)",
            fmtWallClock(earliestVisible), fmtWallClock(latestVisible),
            fmtCountdown(earliestVisible - now), fmtCountdown(latestVisible - now));

        return true;
    }

    // .tlpd teleport <guid>
    // Teleports the GM to the tracked creature with the given guid counter.
    static bool HandleTLPDTeleportCommand(ChatHandler* handler, uint32 guidCounter)
    {
        if (!TLPDHelperConfig::Enable)
        {
            handler->SendErrorMessage("mod-tlpd-helper is disabled.");
            return false;
        }

        Map* northrend = sMapMgr->FindBaseNonInstanceMap(MAP_NORTHREND);
        if (!northrend)
        {
            handler->SendErrorMessage("Northrend map is not loaded.");
            return false;
        }

        auto guids = TLPDTracker::Instance().Snapshot();
        Creature* target = nullptr;
        for (ObjectGuid const& guid : guids)
        {
            if (guid.GetCounter() == guidCounter)
            {
                target = northrend->GetCreature(guid);
                break;
            }
        }

        if (!target)
        {
            handler->PSendSysMessage("No tracked TLPD/Vyragosa found with guid {}.", guidCounter);
            return false;
        }

        Player* player = handler->GetSession()->GetPlayer();
        player->TeleportTo(MAP_NORTHREND,
            target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(),
            target->GetOrientation());

        handler->PSendSysMessage("Teleporting to {} (guid={}).",
            target->GetName(), guidCounter);
        return true;
    }

    // .tlpd spawns
    // Lists all spawn slots loaded from the DB with their index, for use with forcespawn.
    static bool HandleTLPDSpawnsCommand(ChatHandler* handler)
    {
        if (!TLPDHelperConfig::Enable)
        {
            handler->SendErrorMessage("mod-tlpd-helper is disabled.");
            return false;
        }

        auto const slots = TLPDTracker::Instance().GetSpawnSlots();
        if (slots.empty())
        {
            handler->SendErrorMessage("No TLPD spawn slots found in DB.");
            return false;
        }

        handler->PSendSysMessage("TLPD/Vyragosa spawn slots ({} loaded):", slots.size());
        for (size_t i = 0; i < slots.size(); ++i)
        {
            Position const& pos = slots[i].pos;
            handler->PSendSysMessage("  [{}] {}  ({:.1f}, {:.1f}, {:.1f})  path={}",
                i + 1,
                GetSpawnName(pos.GetPositionX(), pos.GetPositionY()),
                pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(),
                slots[i].pathId);
        }
        handler->SendSysMessage("Pass a slot number to '.tlpd forcespawn' to target a specific slot.");
        return true;
    }

    // .tlpd forcespawn <tlpd|vyragosa> [<slot>] [instant] [forced]
    // Summons a new creature at a random slot, or at <slot> (1-based index from '.tlpd spawns').
    // - By default, refuses if any live TLPD/Vyragosa is already tracked.
    // - "instant" skips the hide timer so the creature is immediately visible.
    // - "forced" bypasses the already-active check and spawns an extra one anyway.
    static bool HandleTLPDForceSpawnCommand(ChatHandler* handler, std::string which,
                                            Optional<std::string> opt1, Optional<std::string> opt2,
                                            Optional<std::string> opt3)
    {
        if (!TLPDHelperConfig::Enable)
        {
            handler->SendErrorMessage("mod-tlpd-helper is disabled.");
            return false;
        }

        std::transform(which.begin(), which.end(), which.begin(), ::tolower);

        uint32 entry = 0;
        if (which == "tlpd")
            entry = NPC_TIME_LOST_PROTO_DRAKE;
        else if (which == "vyragosa")
            entry = NPC_VYRAGOSA;
        else
        {
            handler->SendErrorMessage("Usage: .tlpd forcespawn <tlpd|vyragosa> [<slot>] [instant] [forced]");
            return false;
        }

        // Optional args are order-independent; a bare positive number selects a specific spawn slot (1-based).
        auto hasFlag = [&](char const* flag) -> bool
        {
            return (opt1.has_value() && *opt1 == flag) ||
                   (opt2.has_value() && *opt2 == flag) ||
                   (opt3.has_value() && *opt3 == flag);
        };
        auto getSlotArg = [&]() -> Optional<uint32>
        {
            auto tryParse = [](Optional<std::string> const& s) -> Optional<uint32>
            {
                if (!s.has_value() || s->empty())
                    return std::nullopt;
                if (!std::all_of(s->begin(), s->end(), ::isdigit))
                    return std::nullopt;
                uint32 v = static_cast<uint32>(std::stoul(*s));
                if (v == 0)
                    return std::nullopt;
                return v;
            };
            if (auto v = tryParse(opt1)) return v;
            if (auto v = tryParse(opt2)) return v;
            return tryParse(opt3);
        };
        bool makeInstant         = hasFlag("instant");
        bool forceOverride       = hasFlag("forced");
        Optional<uint32> slotArg = getSlotArg();

        Map* northrend = sMapMgr->FindBaseNonInstanceMap(MAP_NORTHREND);
        if (!northrend)
        {
            handler->SendErrorMessage("Northrend map is not loaded.");
            return false;
        }

        // Refuse if any live creature is already in the cycle, unless the GM passed "forced".
        if (!forceOverride)
        {
            auto guids = TLPDTracker::Instance().Snapshot();
            for (ObjectGuid const& guid : guids)
            {
                Creature* c = northrend->GetCreature(guid);
                if (!c || !c->IsAlive())
                    continue;

                handler->PSendSysMessage(
                    "A {} is already active at '{}' (guid={}, {})."
                    " Use '.tlpd forcespawn {} forced' to spawn an additional one anyway.",
                    c->GetName(),
                    TLPDTracker::Instance().GetSpawnName(guid),
                    guid.GetCounter(),
                    c->IsVisible() ? "VISIBLE" : "hidden - timer active",
                    which);
                return true;
            }
        }

        auto const slots = TLPDTracker::Instance().GetSpawnSlots();
        if (slots.empty())
        {
            handler->SendErrorMessage("No TLPD spawn slots found in DB - ensure creature/creature_addon entries exist on map 571 with MovementType=2.");
            return false;
        }

        uint32 spawnIdx;
        if (slotArg)
        {
            if (*slotArg > static_cast<uint32>(slots.size()))
            {
                handler->PSendSysMessage("Invalid slot {} - valid range is 1-{}. Use '.tlpd spawns' to list slots.",
                    *slotArg, slots.size());
                return false;
            }
            spawnIdx = *slotArg - 1;
        }
        else
            spawnIdx = urand(0, static_cast<uint32>(slots.size()) - 1);

        Position const pos    = slots[spawnIdx].pos;
        uint32 const pathId   = slots[spawnIdx].pathId;

        TempSummon* summon  = northrend->SummonCreature(entry, pos);
        if (!summon)
        {
            handler->SendErrorMessage("Failed to summon creature - check server logs.");
            return false;
        }

        summon->SetTempSummonType(TEMPSUMMON_DEAD_DESPAWN);
        // Set the path so DoAction(ACTION_TLPD_REVEAL) can call MoveWaypoint(GetWaypointPath())
        // correctly when the hide timer fires. TempSummons have no creature_addon entry so
        // GetWaypointPath() would otherwise return 0.
        summon->LoadPath(pathId);

        char const* spawnName = GetSpawnName(pos.GetPositionX(), pos.GetPositionY());

        if (makeInstant)
        {
            summon->m_Events.KillAllEvents(false);
            summon->AI()->DoAction(ACTION_TLPD_REVEAL);
            handler->PSendSysMessage("{} force-spawned and made immediately visible at '{}' ({:.1f}, {:.1f}, {:.1f}).",
                summon->GetName(), spawnName, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ());
        }
        else
        {
            handler->PSendSysMessage("{} force-spawned in hidden state at '{}' ({:.1f}, {:.1f}, {:.1f}). "
                "It will become visible after the configured timer.",
                summon->GetName(), spawnName, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ());
        }

        handler->PSendSysMessage(".go xyz {:.2f} {:.2f} {:.2f} 571",
            pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ());

        return true;
    }
};

// ---------------------------------------------------------------------------
// WorldScript - reloads config on .reload config
// ---------------------------------------------------------------------------
class TLPDHelperWorldScript : public WorldScript
{
public:
    TLPDHelperWorldScript() : WorldScript("TLPDHelperWorldScript") {}

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        TLPDHelperConfig::Load();
    }

    void OnLoadCustomDatabaseTable() override
    {
        TLPDTracker::Instance().LoadSpawnSlotsFromDB();
    }
};

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
void Addmod_tlpd_helperScripts()
{
    TLPDHelperConfig::Load();
    new TLPDHelperCreatureScript();
    new TLPDHelperUnitScript();
    new TLPDHelperCommandScript();
    new TLPDHelperWorldScript();
}
