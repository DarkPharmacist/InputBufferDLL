# Input Buffer (DLL)

This mod predicts the consequences of every click: on press (or release) a hidden "ghost" — a copy of your player — is simulated forward with your input. If the ghost dies, the input is delayed by 1..N physics ticks and re-checked; if the delayed version clearly survives longer, the click is delivered later, saving you from a premature press.

---

## Quick Start

1. Requirements: **Geometry Dash 2.2081 (Steam) with [Geode](https://geode-sdk.org) installed** — the DLL attaches to the `Geode.dll` already loaded into the game.
2. Start the game the usual way and wait for the main menu (Geode must be fully loaded).
3. Open **Process Hacker 2** → find the `GeometryDash.exe` process → right-click → *Miscellaneous → Inject DLL* → select `input-buffer.dll`.
4. Done — the buffer is running. Nothing appears on screen (the UI has been cut); you can verify it in the Geode log (`geode/logs/geode.log` — look for the `[CBF-dll] injected, starting up` / `[CBF-dll] ready` lines).

## In-game controls

| Key | Action |
|---|---|
| `6` | cycle the buffer window: 0.5f → 1f → 2f → 4f → 6f → 8f → custom → off |
| `F11` | forget the "memory" of the current level |

They only work inside a level and while Geometry Dash is the active window.

## Settings

There is no UI, so everything is configured via **game launch options** (Steam → game Properties → "Launch Options") or the file `geode\mods\basu.input-buffer\saved.json`.

Format: `--cbf:<key>=<value>`; for bools you can use a bare `--cbf:<key>` (= true). Example:

```
--cbf:stage=6 --cbf:custom-ticks=10 --cbf:custom-wave-ticks=10 --cbf:ship-max-delay=1
```

Priority: launch arguments → `saved.json` → defaults from the original mod.json.

### Main keys

| Key | Type | Default | Description |
|---|---|---|---|
| `stage` | int | 0 | buffer stage 0..7 (0.5f→…→custom→off) |
| `custom-ticks` | int | 40 | max delay, custom stage (ticks at 240/s; 4 ticks = 1 frame @ 60 fps) |
| `custom-wave-ticks` | int | 20 | same, for the wave |
| `ship-max-delay` | int | 4 | separate max delay for the ship |
| `smart-prediction` | bool | true | the ghost accounts for orbs, pads, portals |
| `practice-only` | bool | false | only buffer in practice mode |
| `sim-budget-ms` | float | 8.0 | time budget for predicting a single click |
| `wave-auto-release` | bool | false | auto-release in the wave (when holding = death) |
| `auto-click` | bool | false | autoclick at the last safe tick (practice) |
| `level-memory` | bool | true | the mod remembers which delays worked on a level |
| `low-latency` | bool | true | low-latency mode: window ≤ 4 ticks |
| `performance-mode` | bool | false | lean prediction mode |
| `tps` | int | 240 | physics, steps per second (above 240 changes the physics) |
| `debug-console` | bool | false | separate console window with a live log |

### A buffer window per game mode

**Per-mode frames**: 8 separate slots — Cube, Ship, Ball, UFO, Wave, Robot, Spider, Swing (`mode-ticks-0` … `mode-ticks-7`). They override both the stage and the low-latency limit. Example: 10 ticks for every mode, 1 for the ship:

```
--cbf:per-mode-frames=true --cbf:mode-ticks-0=10 --cbf:mode-ticks-1=1 --cbf:mode-ticks-2=10 --cbf:mode-ticks-3=10 --cbf:mode-ticks-4=10 --cbf:mode-ticks-5=10 --cbf:mode-ticks-6=10 --cbf:mode-ticks-7=10
```

The same thing permanently (without launch arguments) — in `saved.json`:

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

Path to saved.json: `%LOCALAPPDATA%\GeometryDash\geode\mods\basu.input-buffer\saved.json`.

## How it works

- On every click a hidden copy of the player (the "ghost") is created in a separate layer — it draws nothing and never touches the real level (object state is snapshotted and restored).
- The ghost plays your input forward; if the current click leads to death, delays of 1..N ticks are tried (1 tick = 1/240 s) and the first clearly surviving one is chosen.
- Clicks arriving during a delay are shifted by the same amount — the press rhythm is preserved.
- "Level memory" stores which delays saved you and where you died, and is taken into account when choosing (the `memory/` files in the save folder).


## Building from source

Required: **Visual Studio 2022 Build Tools (C++), CMake, Ninja** and the **[Geode SDK 5.11.2](https://github.com/geode-sdk/geode/releases)** (the `GEODE_SDK` environment variable must point to the SDK folder).

```cmd
set GEODE_SDK=C:\path\to\geode-sdk\v1
configure.cmd     & :: once
build.cmd         & :: rebuild -> build\input-buffer.dll
```

Repository layout:

```
bin/input-buffer.dll   ready-to-use DLL (RelWithDebInfo)
src/main.cpp           the mod port (buffer, ghosts, prediction)
src/shim.hpp           settings: command line + saved.json instead of Geode settings
CMakeLists.txt         build via the Geode SDK
```

---


