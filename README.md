# Melange

**A modding framework for Worms Ultimate Mayhem.**

Melange is a single `melange.asi` plugin loaded by [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader). It targets the Steam version of the game, build #1077, and refuses to patch any other build.

## Features

- **Netcode fixes** targeting back-to-back online matches that freeze or end with *"This session is no longer available"*: lost packets are always retransmitted, and match state left over from the previous game is reset.
- **Crash and hang diagnostics.** Stack traces and minidumps on a crash or a hang. `Ctrl+Shift+F12` takes a snapshot by hand.
- **In-game overlay.** Press `` ` `` to show or hide it. Modules add their own panels, menus and hotkeys to it.
- **Event bus.** Subscribe to the engine's own messages by name.
- **Structured session logs.** Every session is logged as JSONL: log lines, engine messages and game events such as turns, shots and damage.
- **Save logs.** `Ctrl+Shift+F11` saves one zip with everything a bug report needs. User names are redacted, and Steam IDs and IP addresses are hashed.
- **Steam and network tracing.** Logs lobby, P2P and socket activity.
- **Works alongside WUMPatch.**

## Install

1. Build `melange.asi` (see [Building from source](#building-from-source)); prebuilt releases are coming. `Melange.ini` is in `dist/`.
2. Get `dinput8.dll` from the x86 build of [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/latest) (`Ultimate-ASI-Loader.zip`). If you already use WUMPatch, you have it already.
3. Copy these files into the game folder, next to `WormsMayhem.exe` (`...\steamapps\common\WormsXHD`). Start the game. The window title now shows `[Melange x.y.z]`.

Both players should install Melange, but the fixes also help when only one of them has it.

## Uninstall

Delete `melange.asi`, `Melange.ini` and the `Melange` folder from the game folder. Delete `dinput8.dll` too, unless other `.asi` mods still need it.

## Configuration

Every module has its own section in `Melange.ini`, and `Enabled=0` turns a module off. Missing keys are added with their default values the first time the game runs.

| Module | Default | What it does |
|---|---|---|
| `NetTransport` | on | Retransmits any lost packet, and re-acknowledges duplicates so a lost ACK can't stall the peer |
| `NetSession` | on | Resets the match state the game leaves behind, and traces the match lifecycle |
| `Diagnostics` | on | Crash handler, hang watchdog and minidumps |
| `Overlay` | on | The in-game overlay (`ToggleKey`, `PassthroughKey`) |
| `EventBus` | on | The engine message bus for modules |
| `Logging` | on | Structured JSONL session logs |
| `LogExport` | on | "Save logs" zip export (`Hotkey`) |
| `EngineLog` | on | Copies the engine's own log into `Melange.log` |
| `SteamTrace` | on | Logs Steam lobby, P2P and callback activity |
| `NetTrace` | on | Logs raw Winsock calls |
| `WindowTag` | on | Shows the Melange version in the window title |
| `FrameInterval` | off | Sets the engine frame interval (`IntervalMs=16` is about 60 fps) |

## Logs and bug reports

| Where | What |
|---|---|
| `<game>\Melange\Melange.log` | Plain-text log of the current run (`Melange.prev.log` is the run before) |
| `<game>\Melange\dumps\` | Crash and hang minidumps |
| `Documents\Melange\logs\<session>\` | Structured session log (`events.jsonl`) |

To report a bug, press `Ctrl+Shift+F11` in the game, or choose *File > Save logs as...* in the overlay, and attach the zip. In fullscreen, the zip goes to `Documents\Melange\exports` instead of opening a save dialog.

## Writing a module

A module is one `.cpp` file anywhere under `src/`. The build picks it up automatically.

```cpp
#include <imgui.h>

#include "core/log.h"
#include "core/module.h"
#include "melange/bus.h"
#include "melange/overlay.h"

namespace {
int g_shots = 0;

class ShotCounter final : public melange::Module {
public:
    const char* Name() const override { return "ShotCounter"; }  // [ShotCounter] in Melange.ini
    const char* Description() const override { return "counts weapon shots"; }
    bool Install() override {
        bool announce = Bool("Announce", true);  // added to Melange.ini with its default
        melange::bus::SubscribeName("Weapon.Fired", melange::bus::Path::Post,
            [](const melange::bus::MessageView&, void* user) {
                ++g_shots;
                if (*static_cast<bool*>(user)) LOG_INFO("[shots] %d", g_shots);
            }, new bool(announce));
        melange::overlay::AddPanel("shots", "Shots", [](void*) { ImGui::Text("Shots: %d", g_shots); }, nullptr);
        return true;
    }
};
}  // namespace

MELANGE_MODULE(ShotCounter);
```

A module reads its settings with `Int`, `Bool` and `Float`. Each key is written to its section in `Melange.ini` on the first run, so every option can be found there. Override `DefaultEnabled()` to ship a module switched off. A module that patches fixed addresses must return `true` from `RequiresKnownBuild()`, so that it is skipped on unknown builds.

The public SDK headers are in `src/sdk/melange/`:

| Header | Purpose |
|---|---|
| `melange/bus.h` | Subscribe to engine messages by name or id, read their payloads, and register payload decoders |
| `melange/overlay.h` | Add overlay panels, menu items and hotkeys |
| `melange/jlog.h` | Write structured records to the session log, and read the in-memory tail |
| `melange/export.h` | Start a "Save logs" export, or write one to a given path |
| `melange/testcmd.h` | Register named text commands for scripted testing |

## Building from source

You need:

- Visual Studio 2022 or later, or the Build Tools, with the C++ x86 toolset;
- CMake 3.25 or later;
- Ninja.

```powershell
.\build.ps1                      # builds dist\melange.asi (-Config x86-debug for a debug build)
.\deploy.ps1                     # installs into the Steam game folder (-GameDir <path> for another folder)
.\uninstall.ps1                  # removes it again (-Purge also deletes logs and dumps)
.\scripts\selftest.ps1           # offline self-tests, no game needed
```

`deploy.ps1` keeps an existing `dinput8.dll`. If there is none, it downloads the latest Ultimate ASI Loader, or uses the one you give with `-LoaderPath <dinput8.dll>`. `build.ps1 -PrivateDir <dir>` also compiles the modules in `<dir>\modules\*.cpp`.

## Roadmap

Coming next: a graphics layer, Lua mods, a mod loader and a map editor.

## License

[MIT](LICENSE).

Melange is an unofficial fan project. It is not affiliated with or endorsed by Team17. You need your own copy of Worms Ultimate Mayhem to use it.
