# mod-tlpd-helper

An [AzerothCore](https://www.azerothcore.org/) module for WoW 3.3.5a (WotLK) that improves the
**Time-Lost Proto-Drake** (TLPD) and **Vyragosa** spawn experience. It intercepts the vanilla
hidden-timer system, replaces it with a configurable seconds-based timer, and adds a suite of
GM commands for tracking and managing the creatures.

---

## Features

- **Configurable spawn visibility timer** – replaces the core hidden timer with a seconds-based range from your config file. Set both values to `0` to use the server default (matching the core's 0–16h window, which combined with the 6h pool `spawntimesecs` gives a 6–22h total respawn range).
- **Server-wide announce** – optionally broadcasts a message when TLPD or Vyragosa becomes visible.
- **In-memory spawn history** – last-visible and last-death timestamps are tracked in memory during the session. History is intentionally reset on restart (a fresh server start gives each creature a new random timer anyway).
- **DB-driven spawn slots** – patrol paths and spawn positions are loaded from `creature` / `creature_addon` at startup; adding new paths requires only SQL, no recompile.
- **GM commands** – status, reveal, next spawn window, force-spawn, teleport, and spawn log.

---

## Requirements

- AzerothCore (WotLK 3.3.5a), C++20 build
- No custom database table required

---

## Installation

1. Clone or copy this module into `modules/mod-tlpd-helper/` inside your AzerothCore source tree.
2. Apply the SQL files from `data/sql/updates/pending_db_world/` to your `acore_world` database (the DB updater does this automatically on the next worldserver start).
3. Copy `conf/mod_tlpd_helper.conf.dist` to your server's config directory as `mod_tlpd_helper.conf` and adjust the values.
4. Rebuild the server (`cmake` + `make`).

---

## Configuration

`mod_tlpd_helper.conf`

| Option | Default | Description |
|---|---|---|
| `TLPDHelper.Enable` | `1` | Enable or disable the entire module. |
| `TLPDHelper.AnnounceOnSpawn` | `0` | Broadcast a server message when TLPD or Vyragosa becomes visible. |
| `TLPDHelper.SpawnTimerMinSeconds` | `0` | Minimum seconds before the creature becomes visible after spawning. Set to `0` with Max also `0` to use the server default (0–16h). |
| `TLPDHelper.SpawnTimerMaxSeconds` | `0` | Maximum seconds before the creature becomes visible after spawning. |
| `TLPDHelper.PoolRespawnSeconds` | `21600` | Must match `creature.spawntimesecs` for TLPD/Vyragosa in the DB. Used by `.tlpd nextspawn` to estimate the next visible window after a death. |

If `SpawnTimerMinSeconds > SpawnTimerMaxSeconds` the values are swapped automatically and a warning is logged.

---

## GM Commands

All commands require the `npc add` RBAC permission (GM level 2+).

### `.tlpd status`
Shows the current state of every tracked TLPD/Vyragosa creature: visible, hidden with a countdown to reveal, or not yet tracked. Includes the spawn location name and a `.go xyz` teleport command.

### `.tlpd reveal [tlpd|vyragosa|both]`
Immediately reveals hidden creature(s) by cancelling their timer and starting their patrol. Defaults to both if no argument is given.

### `.tlpd nextspawn [tlpd|vyragosa|both]`
Shows the estimated next visibility window. If a creature is currently tracked it shows the exact reveal countdown. If both are dead/between pool cycles it shows the estimated pool respawn time and the full visibility window (e.g. `between 4h 12m 30s and 20h 12m 30s from now`).

### `.tlpd log [tlpd|vyragosa|both]`
Displays the in-session last-visible and last-death timestamps for TLPD and/or Vyragosa, including a human-readable "X hours ago" suffix. Note: this data is not persisted across server restarts.

### `.tlpd spawns`
Lists all spawn slots loaded from the database with their 1-based index, human-readable location name, coordinates, and waypoint path ID. Use the slot index with `.tlpd forcespawn`.

### `.tlpd forcespawn <tlpd|vyragosa> [<slot>] [instant] [forced]`
Summons a creature into the spawn cycle at a random slot or at a specific slot (1-based index from `.tlpd spawns`).
- `instant` – skips the hidden timer and makes the creature immediately visible.
- `forced` – bypasses the check for an already-active creature and spawns an additional one.

### `.tlpd teleport <guid>`
Teleports you to a tracked creature by its GUID counter (shown in `.tlpd status`).

## Example usage to test
1. `.tlpd forcespawn vyragosa 5`
2. `.tlpd status` (you can teleport to the spawn using the commadn this prints out)
3. `.tlpd reveal`

## Considerations to speed up testing