# WUMFix

WUMFix is a modular fix and mod framework for **Worms Ultimate Mayhem**: Steam app 70600, exe build #1077.

Its main target is the long-standing multiplayer bug where **a second online match started back-to-back in the same lobby freezes**. Other players report the same bug as *"This session is no longer available"* when the second player's turn begins.

WUMFix is a single `WUMFix.asi` plugin loaded by [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (`dinput8.dll`). It uses the same mechanism as WUMPatch and Renewation HD, so it can be installed alongside them.

## Install (players)

1. Copy `dinput8.dll`, `WUMFix.asi` and `WUMFix.ini` into the game folder (`...\steamapps\common\WormsXHD`).
   - If you already use WUMPatch or Renewation HD, you already have `dinput8.dll`. Just add `WUMFix.asi` and `WUMFix.ini`.
2. Play as normal.
   - Logs and crash or hang dumps go to `WormsXHD\WUMFix\`.
   - If something goes wrong, send `WUMFix\WUMFix.log`, and `WUMFix.prev.log` from the previous run.

To uninstall, delete `WUMFix.asi` and `WUMFix.ini`, and also `dinput8.dll` if no other `.asi` mods remain. Or run `uninstall.ps1`.

## Modules

Each module is one file under `src/` (`core/`, `render/`, `gameplay/`, `net/` or `tools/`, by what it touches) with its own `[Section]` in `WUMFix.ini`.

| Module | Default | What it does |
|---|---|---|
| **NetTransport** | on | Fixes the game's reliable-UDP layer. The original retransmits only the first two packets of a connection. After that, a lost packet stalls both machines forever, which is the freeze. Also re-ACKs duplicates so a lost ACK can't stall the peer. |
| **NetSession** | on | Clears per-match state the game never resets between matches: stale surrender flags, the throttle mask, the viability offset, and a network pause left over from the previous match. Also traces the whole match lifecycle: state machine, turns, surrenders, aborts with call site, and throttle/pause changes. |
| **Diagnostics** | on | Crash handler and hang watchdog: stack trace plus minidump. `Ctrl+Shift+F12` takes a manual snapshot. |
| **SteamTrace** | on | Logs every Steam lobby, P2P and callback call. |
| **EngineLog** | on | Mirrors the engine's own log into `WUMFix.log`. |
| **EventBus** | on | Hooks the engine's message Post and Deliver so modules can subscribe to engine messages by name (`wumfix/bus.h`). Changes nothing in the game. `DumpRegistry=1` writes every message name to `WUMFix\messages.tsv`. Replaces the Probe's message hooks, so only one of the two can hook them. |
| **NetTrace** | on | Logs raw Winsock usage. |
| **WindowTag** | on | Shows `[WUMFix x.y.z]` in the window title. |
| **FrameInterval** | off | Engine frame cap in ms (example of a fixed-address patch). |
| **LocalNet** | off | *Test only.* Emulates Steam lobbies and P2P over localhost so two instances on one PC can play each other. `LossPercent` simulates packet loss. See `docs/localnet.md`. |
| **Automation** | off | *Test only.* Keeps the game running while unfocused and injects input from `WUMFix\automation[.<pid>].txt`. See `scripts/auto.ps1`. |
| **Probe** | off | *Test only (M0 scouting).* Logs GL state at Present, draws a test quad, counts engine messages by name, and toggles input capture (F10). See `docs/re-notes.md` §15. |

The fixes only change local state and the sender side of the protocol; nothing on the wire changes. They therefore help even when only one player has WUMFix, although both players should install it.

## Building

Requirements:
- Visual Studio 2022+ Build Tools with the C++ x86 toolset.
- CMake 3.25+ and Ninja. The portable copies in `tools/` are used automatically.

```powershell
.\build.ps1          # -> dist\WUMFix.asi (x86, static CRT)
.\deploy.ps1         # installs UAL + WUMFix into the Steam game folder
```

SafetyHook (with Zydis) is fetched by CMake.

`.\scripts\selftest.ps1` builds and runs the offline self-tests (`tests/`), which need no game.

## Writing a module

```cpp
// src/gameplay/my_fix.cpp  (any .cpp under src/ is picked up automatically by the build)
#include "core/module.h"
#include "core/mem.h"
#include "core/log.h"

namespace {
class MyFix final : public wf::Module {
public:
    const char* Name() const override { return "MyFix"; }            // = ini section
    const char* Description() const override { return "what it does"; }
    bool RequiresKnownBuild() const override { return true; }         // uses fixed addresses
    bool Install() override {
        int v = Int("SomeValue", 42);                                  // ini key, default auto-written
        if (!wf::mem::Expect(0x4D919B, {0x10})) return false;          // verify bytes before patching
        wf::mem::Put<uint8_t>(0x4D919B, static_cast<uint8_t>(v));
        return true;
    }
};
}
WUMFIX_MODULE(MyFix);
```

Building blocks:

| Header | Provides |
|---|---|
| `core/mem.h` | Patching, pattern scan, IAT and vtable hooks |
| `<safetyhook.hpp>` | Inline hooks and mid-function hooks (register context) |
| `wumfix/bus.h` | Engine message bus: subscribe to engine messages by name, registry names, payload decoders |
| `core/events.h` | Per-frame callback, plus MatchStart/MatchEnd/LobbyEnter/LobbyLeave events fired by NetSession |
| `net/steam.h` | Steam callback base and callback ids |
| `net/net.h` | Typed accessors for NetService, NetThrottle, the session and players |
| `core/debug.h` | Stack scans, minidumps, RTTI names |
| `core/game.h` | Exe identity guard, paths |

Modules that return `RequiresKnownBuild() == true` are skipped automatically on any exe other than #1077, which is checked by SHA-256.

## Repository layout

| Path | Contents |
|---|---|
| `src/core/` | Plugin entry, logging, ini, exe guard, memory/hook helpers, events, debug, testcmd registry |
| `src/render/` | Overlay, GL state guard, input capture (component A; a stub until it lands) |
| `src/gameplay/` | Gameplay-facing tweaks and fixes |
| `src/net/` | Game structure knowledge for networking (build #1077), Steam and net modules (LocalNet in its own subfolder) |
| `src/tools/` | In-plugin dev/test tools: Automation, Probe, log export (component D; a stub until it lands) |
| `src/sdk/wumfix/` | Frozen public headers for mods and other components (`wumfix/<name>.h`) |
| `docs/re-notes.md` | Reverse-engineering reference: classes, Steam usage, match lifecycle, input |
| `docs/netcode.md` | Root-cause writeup of the back-to-back match bug |
| `docs/localnet.md` | The two-instance test harness |
| `re/` | Ghidra scripts, helper tools and RE notes. The decompiled corpus `re/out` is regenerated with `re/ghidra.sh ExportAll.java`. |
| `tests/` | Offline self-tests, built by `scripts/selftest.ps1` (not part of the plugin) |
| `scripts/` | Test automation: `auto.ps1` (input), `ui.ps1` (screenshots), `e2e.ps1` |
| `tools/` | Downloaded toolchain and RE tools; gitignored |
