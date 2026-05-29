# mod-tlpd-helper

An [AzerothCore](https://www.azerothcore.org/) module for WoW 3.3.5a (WotLK) that improves the
**Time-Lost Proto-Drake** (TLPD) and **Vyragosa** spawn experience. It replaces the vanilla
hidden-timer system with a configurable one, persists spawn history across server restarts, and
adds a suite of GM commands for tracking and managing the creatures.

---

## Features

- **Configurable spawn visibility timer** – replaces the hardcoded vanilla 6–22 hour hidden timer with values from your config file.
- **Server-wide announce** – optionally broadcasts a message when TLPD or Vyragosa becomes visible.
- **Persistent spawn log** – last-visible and last-death timestamps are written to the world database and survive server restarts.
- **DB-driven spawn slots** – patrol paths and spawn positions are loaded from `creature` / `creature_addon` at startup; adding new paths requires only SQL, no recompile.
- **GM commands** – status, reveal, force-spawn (at a random or specific slot), teleport, and spawn log.

---

## Requirements

- AzerothCore (WotLK 3.3.5a), C++20 build
- The companion SQL file applied to `acore_world` (creates the `mod_tlpd_helper_spawn_log` table and adds the Ulduar patrol path)

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
| `TLPDHelper.AnnounceOnSpawn` | `1` | Broadcast a server message when TLPD or Vyragosa becomes visible. |
| `TLPDHelper.SpawnTimerMinHours` | `6` | Minimum hours before the creature becomes visible after entering the world. |
| `TLPDHelper.SpawnTimerMaxHours` | `22` | Maximum hours before the creature becomes visible after entering the world. |

If `SpawnTimerMinHours > SpawnTimerMaxHours` the values are swapped automatically and a warning is logged.

---

## GM Commands

All commands require the `npc add` RBAC permission (GM level 2+).

### `.tlpd status`
Shows the current state of every tracked TLPD/Vyragosa creature: visible, hidden with time remaining, or dead. Includes a `.go xyz` teleport line for each.

### `.tlpd reveal [tlpd|vyragosa|both]`
Immediately reveals hidden creature(s) by cancelling their timer. Defaults to both if no argument is given.

### `.tlpd log [tlpd|vyragosa|both]`
Displays the persistent last-visible and last-death timestamps for TLPD and/or Vyragosa, including a human-readable "X hours ago" suffix. Data survives server restarts.

### `.tlpd spawns`
Lists all spawn slots loaded from the database with their 1-based index, human-readable location name, coordinates, and waypoint path ID.
