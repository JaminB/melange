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
| `SmoothSixty` | on (`On=0`) | "Smooth 60": lifts the engine's frame limiter and uses vsync. Toggle in the overlay menu *Game* |
| `Mirage` | on | Graphics layer core: renderer access, scene stages for mods, mod folders |
| `MirageTrace` | on | OpenGL call statistics (`Mode=count`), frame capture (`CaptureHotkey`), texture dumper, GPU compatibility report |
| `MirageShaders` | on | Shader mods: replacements, patches, live reload, sliders; fixes the game's FXAA on AMD and Intel (`FixFxaa`) |
| `MiragePostFX` | on | Post-processing effects from mods, applied to the world or the whole frame (`ToggleKey` bypasses them) |
| `MirageDraw` | on | Drawing API for modules: world-space lines, boxes, spheres and text, and HUD shapes, text and images |
| `MirageDebug` | off | OpenGL debug context: driver errors and warnings go to the logs and the *Mirage/GL debug* panel |

## Logs and bug reports

| Where | What |
|---|---|
| `<game>\Melange\Melange.log` | Plain-text log of the current run (`Melange.prev.log` is the run before) |
| `<game>\Melange\dumps\` | Crash and hang minidumps |
| `Documents\Melange\logs\<session>\` | Structured session log (`events.jsonl`) |

To report a bug, press `Ctrl+Shift+F11` in the game, or choose *File > Save logs as...* in the overlay, and attach the zip. In fullscreen, the zip goes to `Documents\Melange\exports` instead of opening a save dialog.

The zip includes a GPU compatibility report (`gpu/compat.txt`): graphics card, driver, OpenGL version and extensions, the Cg shader profiles your card supports, and which shaders and effects loaded or were skipped and why. The overlay panel *Mirage/GPU* shows the same report.

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
| `melange/render.h` | Renderer access: camera, window size, frame timing, scene stages, GL state save and restore |
| `melange/gltrace.h` | OpenGL call statistics, frame capture and texture dumps |
| `melange/compat.h` | Report what your module loaded or skipped on this GPU, for the compatibility report |
| `melange/postfx.h` | List, enable, order and tune post-processing effects; add a full-screen pass from C++ |
| `melange/shaders.h` | List the game's shader programs, reload them, set their parameters, add shader folders |
| `melange/draw.h` | Draw lines, boxes, spheres, meshes and text in the world, and shapes, text and images on the HUD |
| `melange/gldebug.h` | Whether the debug context is on, its message counts, and debug groups and labels for your GL work |

## Graphics layer (Mirage)

Mirage lets modules and mods draw inside the game's own frame. `melange/render.h` gives the main camera, window size and frame timing, and runs callbacks at fixed points of the engine's draw list:

| Stage | Where it runs |
|---|---|
| `World` | after the landscape, sea and worms, before particles (the depth buffer holds the world) |
| `WorldLate` | after particles, before worm labels and the HUD |
| `PostWorld` | same point, after the `WorldLate` callbacks: post-processing that leaves labels and HUD alone |
| `Hud` | after the HUD, before the final copy to the screen |
| `Final` | just before the final copy to the screen |

Callbacks run in the main render pass only, with the game's framebuffer bound. Wrap your GL work in `render::PushState()` / `PopState()`. With no stage callbacks or mods asking for one, the scene stages themselves patch nothing and the frame is pixel-identical to the game without Mirage. The GL trace hub is separate and installs its call-counting thunks whenever `[MirageTrace] Mode` is `count` (the default) or `log`; set it to `off` for a frame with no Mirage hooks at all.

Mods live in `<game>\Mods\<id>\`. When two mods provide the same file, the later folder name wins. `[Mirage] DisabledMods=a,b` switches mods off, and `ModsDir` moves the folder.

### GL trace and frame capture

`MirageTrace` counts every OpenGL call the game and its Cg runtime make. The overlay panel *Mirage/GL* shows calls, draw calls, shader switches and frame time per frame, and the busiest functions. `[MirageTrace] Mode` is `count` (the default: one counter per call), `log` (records every call), or `off` (no hooks at all; switching back needs a restart).

*Capture frame* (or `Ctrl+Shift+F9`) records one frame into `Documents\Melange\captures\*.mcap`: every call with decoded arguments, the GL state, the compiled shaders, the bound textures and the final image. The format is a plain zip, described in [docs/capture-format.md](docs/capture-format.md). *Dump next 50 textures* writes the next textures the game loads to `Documents\Melange\textures\` as PNG. Captures and dumps contain the game's textures, so they stay on your PC and are never part of a logs export.

### Shader mods

The game's shaders are the Cg files in `<game>\CG\`. A mod changes them from its `shaders\` folder, without shipping the game's files:

- `shaders\<file>` replaces a whole file, such as `Landscape.cg`, or a file it includes, such as `Fxaa3_9.h`.
- `shaders\<file>.patch` edits the game's file with find/replace blocks. Each `find` block must match exactly once; otherwise the patch is skipped and the error is logged. An optional first line `@@ entry <pattern>` limits the patch to some programs (`*` and `?` wildcards).

  ```
  @@ entry *FragmentMain
  @@ find
  	const float specularPower = 20.0f;
  @@ replace
  	const float specularPower = 40.0f;
  @@ end
  ```
- `shaders\params.ini` turns uniforms into sliders in the overlay's *Mirage/Shaders* panel. A section names the file and the programs, `[Landscape.cg:*FragmentMain]`, and each line is `name=type,default,min,max,label` with the type `float`, `vec2`, `vec3`, `vec4` or `color`. Modules set the same values with `melange::shaders::SetParam`, and add folders of shader files with `AddOverrideRoot`.

Saving a file reloads the shaders that use it while the game runs. The new source is compiled first: if it has errors, the game keeps the running shader, and the errors go to the log and the panel.

`[MirageShaders] FixFxaa=1` (the default) fixes the game's own FXAA pass (the `/FXAA` launch option), which does not compile on AMD and Intel GPUs. The panel also switches FXAA on and off while the game runs.

`Mods\mirage-landscape\` is a sample: it adds tunables to the landscape lighting and softens the shadow edges. It is listed in `DisabledMods` by default; remove it from that list to try it. Experimental: a file `shaders\<File>.<Entry>.glsl` replaces one program with GLSL, keeping the Cg parameter names (`GlslReplace=1`, read at start). `Mods\mirage-landscape\extras\` has a GLSL version of the landscape pixel shader.

### Post-processing effects

An effect is a folder `<game>\Mods\<id>\postfx\<effect>\` with an `effect.ini` and GLSL fragment shaders. Its id is `<id>/<effect>`. Effects run at one of two stages:

- `PostWorld` changes the world only: worm labels and the HUD are drawn on top afterwards.
- `Final` changes the whole frame, just before the game copies it to the screen. The game's own FXAA and sepia still apply afterwards.

Open the overlay's *Mirage/Post-FX* panel to switch effects on, change their order, drag their parameters and see what each one costs on the GPU. *Split compare* shows the left half of the screen without the effects. `Ctrl+Shift+F8` (`[MiragePostFX] ToggleKey`) bypasses the whole stack. Your choices are saved in `[MiragePostFX]` in `Melange.ini`. Editing an effect's files while the game runs reloads it. A shader that fails to compile is reported in the panel and the log, and the other effects keep running.

```ini
[effect]
title=Bloom
stage=PostWorld          ; PostWorld | Final
order=300                ; lower runs first; ties by id
enabled=0                ; the default until the player changes it

[param.threshold]        ; uniform float p_threshold
type=float               ; float | vec2 | vec3 | color | int | bool
default=0.8
min=0
max=2
label=Threshold

[texture.lut]            ; uniform sampler2D t_lut, a PNG from the effect folder
file=lut.png
filter=linear            ; linear | nearest
wrap=clamp               ; clamp | repeat

[pass.extract]
shader=extract.frag
scale=0.5                ; size relative to the scene
format=rgba16f           ; rgba8 | rgba16f | r8 | rg8
inputs=scene             ; scene, depth, prev, pass.<name>, texture.<name>

[pass.blurh]
shader=blur.frag
defines=HORIZONTAL=1     ; prepended as #define lines
scale=0.5
inputs=prev

[pass.combine]           ; the last pass writes the effect's output at scene size
shader=combine.frag
inputs=scene,pass.blurh
```

Shader rules:

- Fragment shaders only. The file's own `#version` is used, or `#version 120` if there is none. Mirage supplies a full-screen vertex shader that writes `vec2 mg_uv` (0..1, origin bottom-left). Declare it as `varying vec2 mg_uv;` in GLSL 1.20, or `in vec2 mg_uv;` in 1.30 and later.
- Samplers are named after the inputs: `mg_scene`, `mg_depth`, `mg_prev` (the previous pass, or the scene for the first pass), `mg_pass_<name>` and `t_<name>`. Every sampler a shader uses must be listed in that pass's `inputs=`.
- `mg_depth` is the game's depth buffer, in [0,1] as stored.
- Parameters are `uniform <type> p_<name>`. Optional built-in uniforms:
  - `vec4 mg_resolution`: width, height, 1/width and 1/height of this pass's target;
  - `vec4 mg_sceneResolution`: the same for the scene;
  - `float mg_time`, `float mg_frame`;
  - `mat4 mg_proj`, `mat4 mg_invProj`, `mat4 mg_view`: the main camera;
  - `vec2 mg_nearFar`: the near and far clip distances.
- `#include "file"` pastes a file from the effect folder.
- Textures are uploaded top row first, so `v = 0` is the top row of the PNG.

C++ modules can add a pass of their own with `postfx::AddCodePass`. It draws a full-screen pass that reads `ctx.srcColor` (and `ctx.srcDepth`) into the framebuffer already bound.

The `mirage-samples` mod in `dist\Mods\` has five example effects, all switched off by default:

| Effect | Stage | What it does |
|---|---|---|
| `smaa` | Final | SMAA 1x anti-aliasing ([iryoku/smaa](https://github.com/iryoku/smaa), MIT). Use it instead of `/FXAA`, not with it. |
| `sharpen` | Final | AMD FidelityFX Contrast Adaptive Sharpening ([FidelityFX-CAS](https://github.com/GPUOpen-Effects/FidelityFX-CAS), MIT) |
| `ssao` | PostWorld | Ambient occlusion from the depth buffer |
| `bloom` | PostWorld | Glow around bright areas |
| `tonemap` | PostWorld | A filmic curve plus exposure, contrast, saturation and a colour-grading LUT (`lut.png`) |

The SMAA and CAS folders carry their licence files.

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

Shipped: the Mirage graphics layer (GL trace and frame capture, shader overrides and hot reload, a post-FX stack, and a world/HUD draw API). Coming next: Lua mods, a mod loader and a map editor.

## License

[MIT](LICENSE).

Melange is an unofficial fan project. It is not affiliated with or endorsed by Team17. You need your own copy of Worms Ultimate Mayhem to use it.
