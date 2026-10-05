# Creating a plugin

Melange runs two kinds of plugins:

- **Mods** are folders under `<game>\Mods\<id>\` with a `spice.json` manifest. They hold Lua scripts, shaders, post-processing effects, weapon clones and assets. No compiler needed.
- **Modules** are C++ files compiled into `melange.asi`. Use one when you need direct access to the engine.

## A Lua mod

1. Create `<game>\Mods\my-mod\spice.json`:

   ```json
   {
     "spiceVersion": 1,
     "id": "my-mod",
     "version": "1.0.0",
     "name": "My Mod",
     "melange": { "range": ">=0.3.0" },
     "kind": "client-only",
     "entry": { "client": "client/init.lua" }
   }
   ```

2. Create `client/init.lua`:

   ```lua
   local turn = 0
   wum.events.on("GameLogic.Turn.Started", function()
     turn = turn + 1
     wum.log.info("turn", turn)
   end)
   ```

3. Start the game and enable the mod on the overlay's *Thumper/Mods* page (press `` ` ``). A mod you made is a local plugin: tick *Show local plugins* to see it. Saving the script reloads it while the game runs.

A mod whose `spice.json` does not parse, or whose `melange.range` the running Melange does not satisfy, is moved to `Mods\.incompatible\` when the game or Melange.exe starts (see [Compatibility sweep](spice.md#compatibility-sweep)). While you work on one, set `[Thumper] SweepIncompatible=0` in `Melange.ini`: it then only shows as incompatible.

`kind` decides how the mod behaves online:

- `client-only` mods only change your own screen (UI, drawing, effects) and never affect other players.
- `content` mods change the game itself, through a sim script (`entry.sim`), weapon clones or maps. Everyone in an online match needs the same content mods.

A content mod that ships only maps can be enabled at the main menu, offline, without a restart. One with scripts (including level scripts), weapons, messages or file overrides needs a restart.

Next steps:

| To | Read |
|---|---|
| Use the full Lua API: events, timers, settings, storage, UI, drawing | [lua-api.md](lua-api.md) |
| Run code inside the match simulation | [Sim scripts](developer-guide.md#sim-scripts) |
| Change the game's shaders or add post-processing | [Graphics layer](developer-guide.md#graphics-layer-mirage) |
| Add a weapon | [weapons.md](weapons.md) |
| Make a map with objects, a level script (`wum.level`) or a painted surround | [erg.md](erg.md) |
| Add a web panel to Oasis | [oasis.md](oasis.md) |
| Declare dependencies, settings and permissions | [spice.md](spice.md) |
| Check a content mod stays in sync online | [wormsign.md](wormsign.md) |

The samples in `dist\Mods\` (all shipped disabled) are complete examples: `hello-spice` (client Lua), `sim-sampler` (sim script), `mega-bazooka` (weapon clone), `mirage-samples` (post-FX) and `mirage-landscape` (shaders).

## A C++ module

Modules need a source build (see [Building from source](developer-guide.md#building-from-source)). A module is one `.cpp` file anywhere under `src/`; the build picks it up automatically.

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

The public SDK headers are listed in the [developer guide](developer-guide.md#sdk-headers). To keep your modules outside the Melange source tree, put them in `<dir>\modules\*.cpp` and build with `.\build.ps1 -PrivateDir <dir>`.
