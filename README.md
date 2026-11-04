

# Input Buffer

**A click-prediction buffer for Geometry Dash — delivered as a standalone DLL.**

[![Version](https://img.shields.io/badge/version-0.2-blue)]()
[![Game](https://img.shields.io/badge/Geometry%20Dash-2.2081-ff4757)]()
[![Framework](https://img.shields.io/badge/Geode-5.11.2-9b59b6)]()
[![Platform](https://img.shields.io/badge/platform-Windows-0078D6)]()
[![Build](https://img.shields.io/badge/build-RelWithDebInfo-2ecc71)]()

*Every click, simulated before it happens.*

</div>

---

## Demo

<img width="960" height="540" alt="input-buffer-demo" src="https://github.com/user-attachments/assets/3572d7cd-bed2-4d24-99fc-a1ff6425aa05" />
<div align="center">
    
## Features

- **Click lookahead** — when you press (or release) a button, a hidden *ghost* copy of the player is simulated forward with your input. If the ghost dies, the input is retried with 1..N ticks of delay and delivered the moment it provably survives.
- **Smart prediction** — the ghost accounts for orbs, pads, gravity portals and speed portals; mode / size / dual / mirror / teleport portals end the prediction as *safe*.
- **Input queue** — clicks that arrive during a delay are shifted by the same amount, so taps and hold lengths are preserved. Your rhythm stays intact.
- **Level memory** — the mod remembers which delays saved you and where you died, and reuses that knowledge (files under `memory/`).
- **Zero UI, zero footprint** — nothing is drawn on screen; the DLL hooks into the already-loaded Geode runtime and stays out of your way.
- **Fully configurable** — 30+ settings via launch arguments or a plain JSON file, no menu needed.

## Installation

**Requirements:** Geometry Dash **2.2081** (Steam) with [Geode](https://geode-sdk.org) installed. The DLL attaches to the `Geode.dll` already loaded by the game — nothing is installed into Geode itself.

1. Launch the game normally and wait for the main menu (let Geode fully load).
2. Open **Process Hacker 2** → find the `GeometryDash.exe` process → right-click → *Miscellaneous → Inject DLL* → pick `bin/input-buffer.dll`.
3. That's it — the buffer is live. Nothing appears on screen (the UI is stripped); verify it in the Geode log (`geode/logs/geode.log` — look for `[input-buffer] injected, starting up` / `[input-buffer] ready`).

## Usage

| Key | Action |
|---|---|
| `6` | cycle the buffer window: 0.5f → 1f → 2f → 4f → 6f → 8f → custom → off |
| `F11` | forget the current level's memory |

Keys only work inside a level and while the Geometry Dash window is focused.

## Configuration

There is no UI — everything is configured via **game launch arguments** (Steam → game Properties → "Launch Options") or the `geode\mods\input-buffer\saved.json` file.

Format: `--ib:<key>=<value>`; booleans can be passed as a bare `--ib:<key>` (= true). Example:

```
--ib:stage=6 --ib:custom-ticks=10 --ib:custom-wave-ticks=10 --ib:ship-max-delay=1
```

Precedence: **launch args → `saved.json` → built-in defaults.**

### Key settings

| Key | Type | Default | Description |
|---|---|---|---|
| `stage` | int | 0 | buffer stage 0..7 (0.5f → … → custom → off) |
| `custom-ticks` | int | 40 | max delay for the custom stage (240 ticks/s; 4 ticks = 1 frame @ 60 fps) |
| `custom-wave-ticks` | int | 20 | same, for the wave |
| `ship-max-delay` | int | 4 | separate max delay for the ship |
| `smart-prediction` | bool | true | ghost accounts for orbs, pads, portals |
| `practice-only` | bool | false | buffer only in practice mode |
| `sim-budget-ms` | float | 8.0 | time budget for predicting a single click |
| `wave-auto-release` | bool | false | auto-release in the wave (when holding = death) |
| `auto-click` | bool | false | auto-click on the last safe tick (practice) |
| `level-memory` | bool | true | remember which delays worked on a level |
| `low-latency` | bool | true | low-latency mode: window capped at 4 ticks |
| `performance-mode` | bool | false | lightweight prediction mode |
| `tps` | int | 240 | physics steps per second (above 240 changes physics) |
| `debug-console` | bool | false | dedicated console window with a live log |

> **Note:** `low-latency` (on by default) caps the window at 4 ticks — for `custom-ticks` above 4, add `--ib:low-latency=no`, and for big windows (from ~16 ticks) raise the search budget with `--ib:sim-budget-ms=30`. Example with a 64-tick window:
>
> ```
> --ib:stage=6 --ib:custom-ticks=64 --ib:custom-wave-ticks=64 --ib:low-latency=no --ib:sim-budget-ms=30
> ```

### Per-mode buffer windows

Per-mode frames give each of the 8 game modes its own slot — Cube, Ship, Ball, UFO, Wave, Robot, Spider, Swing (`mode-ticks-0` … `mode-ticks-7`). They override both the stage and the low-latency cap. Example — 10 ticks for every mode, 1 for the ship:

```
--ib:per-mode-frames=true --ib:mode-ticks-0=10 --ib:mode-ticks-1=1 --ib:mode-ticks-2=10 --ib:mode-ticks-3=10 --ib:mode-ticks-4=10 --ib:mode-ticks-5=10 --ib:mode-ticks-6=10 --ib:mode-ticks-7=10
```

The same, permanently (no launch args), in `saved.json`:

```json
{
    "stage": 6,
    "per-mode-frames": true,
    "mode-ticks-0": 10,
    "mode-ticks-1": 1,
    "mode-ticks-2": 10,
    "mode-ticks-3": 10,
    "mode-ticks-4": 10,
    "mode-ticks-5": 10,
    "mode-ticks-6": 10,
    "mode-ticks-7": 10
}
```

`saved.json` lives at `%LOCALAPPDATA%\GeometryDash\geode\mods\input-buffer\saved.json`.

## How It Works

- On every click, a hidden copy of the player (the "ghost") is created in a separate layer — it draws nothing and never touches the real level (object state is snapshotted and restored).
- The ghost replays your input forward; if the click leads to death, delays of 1..N ticks are tried (1 tick = 1/240 s) and the first one that clearly survives wins.
- Clicks arriving during a delay are shifted by the same amount — the rhythm of your inputs is preserved.
- Level memory stores which delays saved you and where you died, and feeds that into future decisions (files under `memory/` in the save folder).

## Reading the log (`--ib:debug-console`)

| Line | Meaning |
|---|---|
| `SAVED press +4t (1f) 15->27 [max 64]` | click saved: delayed by 4 ticks (1 frame); without the delay the ghost lived 15 ticks, with it — 27; the window is 64 |
| `no save: press dies in 2t` | no delay helps: the click went to the game as-is |
| `too late: waiting only makes it crash sooner` | the click was **after** the ideal tick — the buffer can only hold early clicks back, it can't give time back |
| `dies in 93t ... same crash at any timing (next obstacle)` | the click itself is fine; the ghost dies later (on the next obstacle) at any delay — not your click's fault |
| `ghost killed at tN by obj X (type T)` | diagnostics: what killed the ghost (object, type, hitboxes); "no object" = crushed / out of bounds / slope |
| `prediction budget hit at +N ticks` | the search didn't fit into `sim-budget-ms` — it continues over the next ticks (the window shows up reduced: `[max 62]`) |
| `attempt: 5 saves \| avg 5.4t early` | attempt summary: how many clicks were saved and how early they were |
| `memory: N spots learned` | level memory: how many spots are learned |

## Troubleshooting

- **Nothing happens after injection** — check `geode/logs/geode.log` for the `[input-buffer]` lines; if they're missing, Geode wasn't fully loaded when you injected.
- **Big window feels laggy** — raise `sim-budget-ms`, enable `performance-mode`, or drop the window size.
- **Settings don't apply** — launch args take precedence over `saved.json`; check your Steam launch options..

## Building from Source

**Requirements:** Visual Studio 2022 Build Tools (C++ workload), CMake, Ninja, and [Geode SDK 5.11.2](https://github.com/geode-sdk/geode/releases) with the `GEODE_SDK` environment variable pointing at the SDK folder.

```cmd
set GEODE_SDK=C:\path\to\geode-sdk\v1
configure.cmd     & one-time setup
build.cmd         & rebuild -> build\input-buffer.dll
```

---

<div align="center">

**Input Buffer v0.2** · Windows · Geometry Dash 2.2081 · [Geode](https://geode-sdk.org)

</div>
