# Lua API for client mods (`wum.*`)
| `wum.shaders.enableGlsl(file, entry, on)` | Pauses (`false`) or resumes one of the mod's own `shaders<File>.<Entry>.glsl` replacements, e.g. `("Landscape.cg", "LandscapeFragmentMain", false)`, so the game's own program draws again. Takes effect on the next frame, is not saved, and needs `[MirageShaders] GlslReplace=1`. Errors if the mod ships no such file. |

A mod's `entry.client` script (from its `spice.json`) runs in Melange's Lua 5.4 VM, the Sandbox. This page lists everything a
client script can use. The offline self-test (`sandbox_selftest`) fails if a `wum.*` function is missing from this page.

Sim scripts (`entry.sim`) run in the game's own Lua 5.0 match VM and have a different API (`wum.sim.*`); they are not covered here.

## The environment

Every mod gets its own global table. Globals a mod defines are invisible to other mods. `_G` is the mod's own table.

Available:
- Base functions: `assert error ipairs next pairs pcall xpcall rawequal rawget rawlen rawset select tonumber tostring type getmetatable setmetatable collectgarbage print _VERSION`.
  - `print(...)` writes to the mod log (`wum.log.info`).
  - `collectgarbage` accepts only `"collect"`, `"count"` and `"step"`.
  - `setmetatable` refuses metatables with `__gc`.
  - `rawset` refuses the shared read-only tables.
- Libraries: `string` (without `string.dump`), `table`, `math`, `utf8`, `coroutine`, and `os.time`, `os.clock`, `os.date`.
- `wum`, described below.

Not available: `load`, `loadstring`, `dofile`, `loadfile`, `require`, `package`, `io`, `debug`, the rest of `os`, and precompiled
(binary) chunks. The shared tables (`string`, `math`, `wum`, `wum.log`, ...) are read-only; assigning to them raises an error.

## Limits

Set in `[Sandbox]` of `Melange.ini`:

| Key | Default | Meaning |
|---|---|---|
| `InstrPerCall` | 500000 | Lua instructions per call from Melange into a mod (a callback, the top-level chunk, a panel function). Coroutines share the caller's budget. A mod's own `pcall` cannot catch the stop. |
| `ModMemoryMB` | 16 | Memory per mod. Over the limit, Lua runs one full garbage collection, then the allocation fails ("not enough memory"). |
| `TotalMemoryMB` | 96 | Memory for the whole VM. |
| `HotReload` | 1 | Reload a mod when a `.lua` file in its `entry.client` folder changes. |

**Faults.** An error in a callback, a spent instruction budget or an out-of-memory is a fault. After 3 faults the callback is
disabled for the rest of the session (it is logged; a disabled panel shows a notice). If the top-level chunk fails, the mod is not
loaded and the error, with its line number, is shown on the Mods page.

**Hot reload.** The new script is compiled and run first. If it fails, the previous version keeps running and the error is shown.
If it succeeds, every handle of the previous version (event subscriptions, timers, panels, menu items, hotkeys, draw callbacks,
textures) is revoked, then `wum.mod.onReload(prev)` is called with the previous version's `wum.mod.keep` table. `wum.storage` is
not affected.

## `wum.mod`

| Name | Description |
|---|---|
| `wum.mod.id` | The mod id. |
| `wum.mod.name` | Display name. |
| `wum.mod.version` | Version from `spice.json`. |
| `wum.mod.dir` | The mod folder (UTF-8). |
| `wum.mod.readFile(rel)` | Contents of a file inside the mod folder as a string, or `nil, err`. Needs `permissions.filesystem: "own-folder"`. At most 8 MB. |
| `wum.mod.keep` | A table that is handed to the next version on hot reload. |
| `wum.mod.onReload` | Set it to `function(prevKeep) ... end` to receive the previous version's `keep` after a hot reload. |

## `wum.log`

`wum.log.debug(...)`, `wum.log.info(...)`, `wum.log.warn(...)`, `wum.log.error(...)`: arguments are converted with `tostring` and
joined with tabs. Records go to the Melange log (category `mod`, tagged with the mod id). At most 200 records per second per mod.

## `wum.events`

| Name | Description |
|---|---|
| `wum.events.on(name, fn)` | Subscribes `fn(payload, name)`; returns a handle. |
| `wum.events.off(handle)` | Unsubscribes; returns `true` if the handle was the mod's. |
| `wum.events.emit(name, table)` | Sends `mod.<your id>.<name>` to every subscriber (a name that already starts with `mod.<your id>.` is kept). The table is copied (strings, numbers, booleans and nested tables only). |

Events are delivered at the next frame, in mod load order, then subscription order, at most 256 per frame.

Names:
- **Engine messages** by name, for example `GameLogic.Turn.Started` or `Weapon.Fired`. The payload table holds the decoded
  message fields (vectors are `{x, y, z}` arrays); messages without a decoder give an empty table.
- **Melange events:**
  - `melange.frame` `{frame}`: every frame.
  - `melange.scene` `{scene}`: `boot`, `menu`, `loading` or `match`.
  - `melange.match.start` `{online}`, `melange.match.end`.
  - `melange.mods.changed`: the mod list changed.
  - `melange.reload` `{id}`: a mod was hot-reloaded.
  - Events that C++ modules post with `melange::lua::PostEvent`.
- **Mod events:** `mod.<id>.<name>` from `wum.events.emit`.

## `wum.timers`

| Name | Description |
|---|---|
| `wum.timers.after(seconds, fn)` | Calls `fn()` once after `seconds` (wall clock); returns a handle. |
| `wum.timers.every(seconds, fn)` | Calls `fn()` every `seconds` (0 = every frame); returns a handle. |
| `wum.timers.cancel(handle)` | Stops a timer; returns `true` if the handle was the mod's. |

## `wum.config`

Settings declared in the `settings` array of `spice.json`. They are shown on the Mods page and stored in `[Mod.<id>]` of
`Melange.ini`.

| Name | Description |
|---|---|
| `wum.config.get(key)` | The value (boolean, integer, number or string, by the declared `type`), or the default. |
| `wum.config.set(key, value)` | Checks the type, `min`/`max` and `options`, then stores it. Raises on an undeclared key or a bad value. |

## `wum.storage`

A per-mod JSON file (`Melange\mods\<id>\storage.json` next to `melange.asi`), at most 1 MB. Values are strings, numbers, booleans
and tables of those (sequences become arrays, other tables need string keys). Writes are saved within 2 seconds, and when the mod
is unloaded or the game exits.

| Name | Description |
|---|---|
| `wum.storage.get(key)` | A copy of the stored value, or `nil`. |
| `wum.storage.set(key, value)` | Stores a copy; `nil` removes the key. Raises when the file would exceed 1 MB. |
| `wum.storage.remove(key)` | Removes the key. |
| `wum.storage.keys()` | The keys, sorted. |

## `wum.game`

Read-only game state.

| Name | Description |
|---|---|
| `wum.game.scene()` | `"boot"`, `"menu"`, `"loading"` or `"match"`. |
| `wum.game.inMatch()` | A match (including the attract demo) is running. |
| `wum.game.online()` | The current match is an online match. |
| `wum.game.turn()` | `{index}`: turns started in this match (`team` is not available yet). |
| `wum.game.tick()` | Simulation ticks (50 per second) since the match started, when the sim bridge is running; otherwise 0. |
| `wum.game.worms()` | The worms of the current match, an array of `{slot, team, name, health, alive, pos={x,y,z}, vel={x,y,z}, yaw, weapon}` (`vel` is the engine's own velocity in world units per second: the game keeps it per millisecond of game time and Melange multiplies by 1000; it is the worm's physics velocity, so it is about zero while the worm stands or walks and large while it flies, falls or is knocked back; `weapon` is the weapon id, absent when none; `yaw` is the facing angle in radians about +Y, the facing direction is `(sin yaw, 0, cos yaw)`, it is not wrapped to a range and it spins while a worm is thrown). Empty outside a match; `nil, "unavailable"` on an unrecognised game build or with `[GameState] Enabled=0`. |
| `wum.game.teams()` | The teams of the current match, an array of `{slot, name, active, ai, local}`; empty outside a match, `nil, "unavailable"` as above. |
| `wum.game.activeWorm()` | The slot of the worm whose turn it is, or `nil`. |
| `wum.game.theme()` | The level theme of the current match as the game names it (for example `"SPACE"`), or `nil` outside a match. |
| `wum.game.landRay(x0, y0, z0, x1, y1, z1)` | Casts the segment from `(x0, y0, z0)` to `(x1, y1, z1)` against the land with the game's own collision code. On a hit it returns `t, nx, ny, nz`: `t` from 0 to 1 along the segment (the point is `p0 + t * (p1 - p0)`) and the unit outward surface normal there. A miss returns `nil`. See below for the details and the other results. |

**`wum.game.landRay` in detail.** It runs the straight sweep the game's camera uses for its own ray casts, against the
landscape (including terrain that weapons have changed) and the heightmap around it; it does not see water, worms, crates,
barrels or other objects. The hit is the last point outside the land the sweep found, refined by bisection, so it sits on
or just in front of the surface; a segment that starts inside land hits at `t = 0`. The game's collision state is saved
and restored around the call, so a ray never changes the match, online or offline.

- `nil` alone: no land along the segment, the segment has zero length, or no match is running.
- `nil, "budget"`: more than 256 rays this frame (all mods together); try again next frame.
- `nil, "invalid"`: a coordinate is not finite or is beyond ±1e6.
- `nil, "unavailable"`: the game build is not #1077, `[GameState] Enabled=0`, the call is not on the main thread, the
  loaded landscape is not one the sweep can take (1024 or more land frames, or its objects do not look right), or a
  sweep faulted earlier this session (logged; the function then stays off until the game restarts). Only the build,
  setting and fault causes are permanent: the thread and landscape causes can clear on a later call or the next match,
  so a mod should try again later rather than give up for the session.
- Segments longer than 4096 units are searched over their first 4096 units only; `t` is still measured along the whole
  segment.
- Arguments that are not numbers (or strings that convert to numbers) raise an error.

Check for the function before using it, since older Melange versions do not have it: `if wum.game.landRay then ... end`.
Melange logs the number of rays and their average and longest time at the end of each match.

## `wum.ui`

| Name | Description |
|---|---|
| `wum.ui.panel(id, title, fn[, open])` | Adds an overlay panel. `fn()` draws its contents every frame while it is open. Returns a handle. The overlay's *View* menu lists it as *<mod name> > <title>*; a `/` in `title` (`"Tools/Grid"`) adds a submenu level. |
| `wum.ui.menu(path, fn[, opts])` | Adds a menu item `Mods/<mod name>/<path>` (for example `"Reset"`). `opts.checked` is an optional function asked every frame the menu is open: the item shows a check mark while it returns true, for an item that switches something on and off (`wum.ui.menu("Grid", toggleGrid, {checked = function() return gridOn end})`). An error in it counts as a fault (the getter is disabled after 3, the item still works) and reads as unchecked. Returns a handle. |
| `wum.ui.hotkey(keys, fn)` | Calls `fn()` on a hotkey such as `"Ctrl+Shift+H"`, whether or not the overlay is shown. The key is kept from the game. |
| `wum.ui.remove(handle)` | Removes a panel, menu item or hotkey handle. |

Widgets, only inside a panel function:

| Name | Returns |
|---|---|
| `wum.ui.text(s)` | |
| `wum.ui.button(label)` | `true` when clicked |
| `wum.ui.checkbox(label, value)` | `value, changed` |
| `wum.ui.sliderFloat(label, value, min, max)` | `value, changed` |
| `wum.ui.sliderInt(label, value, min, max)` | `value, changed` |
| `wum.ui.inputText(label, text[, maxLen])` | `text, changed` |
| `wum.ui.combo(label, index, items)` | `index, changed` (1-based) |
| `wum.ui.collapsingHeader(label)` | `true` when open |
| `wum.ui.progressBar(fraction[, text])` | |
| `wum.ui.separator()`, `wum.ui.sameLine()`, `wum.ui.spacing()` | |

## `wum.draw`

Positions are tables `{x, y, z}` or `{x = , y = , z = }` in world units. Colours are `0xRRGGBBAA`, `"#RRGGBB"`, `"#RRGGBBAA"` or
`{r, g, b[, a]}` in 0..1. `frames` (default 1) keeps a primitive for that many frames. Called outside a draw callback, primitives are
queued for the next frame; inside `wum.draw.on`, they are drawn at that stage.

| Name | Description |
|---|---|
| `wum.draw.line(a, b, color[, width[, frames]])` | World line. |
| `wum.draw.box(min, max, color[, width[, frames]])` | World box outline. |
| `wum.draw.sphere(center, radius, color[, width[, frames]])` | World sphere outline. |
| `wum.draw.axes(origin[, length[, width[, frames]]])` | X/Y/Z axes. |
| `wum.draw.quad(p1, p2, p3, p4, color[, frames])` | Filled quad. |
| `wum.draw.text(pos, text, color[, size[, frames]])` | Screen-aligned text above a world point. |
| `wum.draw.hudLine(x0, y0, x1, y1, color[, width[, frames]])` | HUD line in window pixels (origin top-left). |
| `wum.draw.hudRect(x0, y0, x1, y1, color[, filled[, width[, frames]]])` | HUD rectangle. |
| `wum.draw.hudText(x, y, text, color[, size[, frames]])` | HUD text. |
| `wum.draw.hudImage(x0, y0, x1, y1, texture[, tint[, frames]])` | HUD image from `wum.draw.texture`. |
| `wum.draw.texture(rel)` | Loads a PNG from the mod folder; returns a texture id, or `nil, err`. Freed when the mod unloads or reloads. A mod can hold 256 at a time. Textures get a full mip chain and trilinear filtering. |
| `wum.draw.sprite(tex, x, y, z, halfW, halfL, ax, ay, az[, color[, mode]])` | Textured world sprite; only inside a `"world"` or `"worldLate"` callback. Returns `true`, or `false` when dropped by the per-frame cap. See [Sprites](#sprites). |
| `wum.draw.on(stage, fn)` | Calls `fn(stage)` every frame at `"world"`, `"worldLate"` or `"hud"`; returns a handle. |
| `wum.draw.off(handle)` | Removes a draw callback. |

### Sprites

`wum.draw.sprite(tex, x, y, z, halfW, halfL, ax, ay, az[, color[, mode]])` draws a textured quad for particles, sparks,
smoke and similar effects. Detect it with `if wum.draw.sprite then ... end` (Melange 0.6 and later).

- `tex`: a texture id from this mod's `wum.draw.texture`. Any other number raises an error. The PNG can use straight or
  premultiplied alpha (pick the matching `mode`). It is sampled with bilinear filtering and mipmaps. Mips are averaged
  weighted by alpha, unless every pixel has r, g and b no greater than its alpha (a premultiplied image), in which case
  they are averaged as stored.
- `x, y, z`: the world centre. `halfW`: half the width, in world units. `halfL`: half the length along the axis.
- `ax, ay, az`: the world-space stretch axis (any length; it is normalised).
  - Zero axis: a round billboard that faces the camera. It is `2*halfW` square, `halfL` is ignored, and the image is
    upright (texture top at the top of the screen).
  - Otherwise: a view-facing quad `2*halfW` wide whose long side follows the screen projection of the axis, as for a
    velocity-stretched particle. Texture `v` runs from the tail (`v = 0`, the PNG's top row) at `centre - axis*halfL` to
    the head (`v = 1`, the bottom row) at `centre + axis*halfL`, so draw streak textures with the head at the bottom.
    `u` runs 0..1 across the width. The tail and head keep their real depths, so the streak is in perspective.
  - When the axis points nearly at the camera, the sprite is not allowed to look shorter than `min(halfW, halfL)`. It
    shows as that much quad in the axis's screen direction (screen up if it has none), so a particle flying straight
    at the camera does not vanish.
- `color`: a tint multiplied with the texture, in any colour form above. The default is opaque white.
- `mode`: `"alpha"` (default, straight alpha blending), `"premul"` (premultiplied: `ONE, ONE_MINUS_SRC_ALPHA`; the
  tint is premultiplied by its own alpha, so its alpha still fades the sprite) or `"additive"` (`SRC_ALPHA, ONE`).

Sprites are drawn at the callback's stage after that stage's other primitives. They are depth-tested against the
scene, with no depth writes and no face culling. All sprites of a stage, from every mod, are sorted back to front by
view depth, and sprites at equal depth keep their call order. Additive sprites blend the same in any order, so each
unbroken stretch of additive sprites in that order is regrouped by texture.

Sprites are batched into one vertex array per stage, with one draw call per run of consecutive sprites (in that order)
that share a texture and mode. One texture costs one draw call however many sprites use it. Alpha-blended textures
interleaved in depth cost a draw call for every switch.

A mod can draw 4096 sprites per frame across all its callbacks. Past that, calls do nothing and return `false`. They
also return `false` if the stage already holds 65536 sprites from all mods.

The arguments are checked. A non-number position or size, a bad colour or mode, or calling outside a world callback
raises an error. `halfL` and the axis may be `nil` (0). Non-finite values (NaN, infinity) are ignored: nothing is
drawn and the call returns `true`. The same happens for `halfW <= 0`, and for a non-zero axis with `halfL == 0`.

```lua
local spark = wum.draw.texture("fx/spark.png")
wum.draw.on("worldLate", function()
  for _, p in ipairs(particles) do
    local v = p.vel
    wum.draw.sprite(spark, p.x, p.y, p.z, 0.4, 0.4 + p.speed * 0.05, v.x, v.y, v.z, 0xffd080ff, "additive")
  end
end)
```

Measured cost, x86 release build on a Ryzen 7 7800X3D with a Radeon RX 7800 XT:

- Lua: about 0.18 ms per 1000 `wum.draw.sprite` calls.
- Melange's side (expand, sort, runs, GL submission), sharing one texture: about 0.04 to 0.06 ms per 1000.
- Worst case: 4096 sprites with three textures shuffled at random in depth make about 3100 draw calls. That costs
  about 0.28 ms of CPU per 1000 sprites, plus a few milliseconds of GPU time per frame. Prefer few textures.

## `wum.render`

| Name | Description |
|---|---|
| `wum.render.camera()` | `{pos, fwd, up, near, far}` of the main camera, or `nil`. |
| `wum.render.worldToScreen(pos)` | `x, y, depth` in window pixels, or `nil` behind the camera. |
| `wum.render.windowSize()` | `width, height`. |
| `wum.render.timing()` | `{busyMsP50, busyMsP95, frameMsP50, fps, frames}`. |

## `wum.postfx`

Every effect can be read; only the mod's own effects (`<mod id>/<effect>`) can be changed.

| Name | Description |
|---|---|
| `wum.postfx.list()` | Array of `{id, title, stage, order, enabled, failed, own}`. |
| `wum.postfx.enable(id, on)` | Turns one of the mod's effects on or off. |
| `wum.postfx.setParam(id, param, v1[, v2...])` | Sets a uniform (up to 16 floats, or one table of them). |
| `wum.postfx.setTransient(id, param, v1[, v2...])` | Like `setParam`, but the value is not saved to `Melange.ini` and not logged. For values a mod sets every frame. An effect reload keeps it, unless the edit removes the param or changes its size: then it returns to its saved value. |
| `wum.postfx.getParam(id, param[, n])` | `n` floats (default 1). |

## `wum.graphics`

| Name | Description |
|---|---|
| `wum.graphics.setShadowMapSize(n)` | Replaces the mod's `graphics.shadowMapSize` request: 512, 1024, 2048 or 4096, 0 for none, `nil` to go back to the manifest's. The shadow map is rebuilt on the next frame. `[MirageShadows]` in `Melange.ini` still has the last word. |
| `wum.graphics.shadowMap()` | `{size, effective, vanilla, modRequest, available}`: the engine's current size, the merged request (0 = vanilla), the size from the game's own cfg files. |
| `wum.graphics.setSupersample(n)` | Asks for supersampling: 2 (1x2) or 4 (2x2) samples per pixel, 0 or `nil` for none. The scene targets are rebuilt on the next frame; with no request left, the game's own `/SSAA` setting comes back. `[MirageSupersample]` in `Melange.ini` still has the last word. |
| `wum.graphics.supersample()` | `{x, y, effective, sceneWidth, sceneHeight, multisampled, modRequest, available}`: the engine's current factors, the merged request in samples (0 = vanilla), the scene's size, and whether `/SSAA` is running as multisampling instead. |

## `wum.shaders`

| Name | Description |
|---|---|
| `wum.shaders.setParam(file, entry, name, v1[, v2...])` | Sets a shader parameter the mod's own `shaders\params.ini` declares, e.g. `("Landscape.cg", "*FragmentMain", "softness", 1.5)`. `entry` is the section's glob. Errors on anything else. |
| `wum.shaders.enableGlsl(file, entry, on)` | Pauses (`false`) or resumes one of the mod's own `shaders\<File>.<Entry>.glsl` replacements, e.g. `("Landscape.cg", "LandscapeFragmentMain", false)`, so the game's own program draws again. Takes effect on the next frame and is not saved. Errors if the mod ships no such file. |

## `wum.unsafe` (Deep Desert)

Only for mods whose `spice.json` has `permissions.unsafe: true`. Until the player allows it in the consent dialog, every function
raises "Deep Desert not granted". Every refusal and every fault is logged. A mod only counts as a content mod online once its
Deep Desert grant is actually allowed; declining ("Keep sandboxed") keeps it out of the online content hash.
Libraries that C++ modules add with the Deep Desert flag are also visible only to granted mods.

| Name | Description |
|---|---|
| `wum.unsafe.read(addr, type[, n])` | Reads memory. Types: `u8 i8 u16 i16 u32 i32 f32 f64 ptr cstr bytes`. For numeric types `n` is a count (a table is returned when `n > 1`); for `cstr` it is the maximum length (default 256); for `bytes` it is the length. |
| `wum.unsafe.write(addr, type, value)` | Writes memory (`cstr` adds the terminating zero). |
| `wum.unsafe.call(addr, conv, ...)` | Calls native code. `conv` is `"cdecl"`, `"stdcall"` or `"thiscall"` (object pointer first), optionally with `":f32"` or `":f64"` for a floating-point return. Up to 8 arguments: integers, floats (passed as 32-bit floats), booleans, strings (as pointers) or `nil`. Returns `eax` or the float. |
| `wum.unsafe.base()` | Load address of `WormsMayhem.exe`. |
| `wum.unsafe.build()` | The game build name and whether Melange knows it. |

A memory fault inside these functions becomes a Lua error instead of a crash. Writes that change the simulation are the mod's
responsibility.

## `wum.web` (Oasis)

Lets a client mod push its own data into [Oasis](oasis.md) and add a panel to it. Everything a mod adds is named
`mod.<its id>.<x>`, so mods never collide with each other or with the core channels and methods.

| Name | Description |
|---|---|
| `wum.web.channel(name)` | Registers `mod.<id>.<name>` and returns `ch`. `ch:publish(tbl)` sends one JSON value to every subscriber (a no-op when nobody is subscribed); `ch:subscribers()` is the current subscriber count. Removed automatically on hot reload or disable. |
| `wum.web.method(name, fn)` | Registers `mod.<id>.<name>` as an RPC method. `fn(params)` gets the call's parameters as a table and must return a table (or raise, which the caller sees as an error); it runs on the main thread under the same instruction budget as any other callback, so a runaway `fn` is stopped and the call fails, it does not hang the page. |
| `wum.web.panel{title=, entry="web/index.html"}` | Serves the folder containing `entry` (so its assets travel with it) in a sandboxed iframe at `/ext/<id>/`, listed in the page's tab bar. One panel per mod. Removed on hot reload or disable, which also closes its channels. |

A panel page has no cookie and cannot reach `/ws` or `document` outside its own frame (`sandbox="allow-scripts"`, no
`allow-same-origin`); it talks to the shell only through `/app/ext.js`, which exposes `OasisExt.call(method, params)`
and `OasisExt.subscribe(channel, filter, fn)` restricted to the mod's own `mod.<id>.*` names plus read-only `state`
and `log`.

## `wum.wormsign`

The match's tick clock and state hash. A tick is one 20 ms step of the game's simulation (50 per second); its hash
covers the logic RNG, the turn, the worms, the scheduled tasks, projectiles and teams, so two machines (or a match and
its replay) that agree on a tick's hash agree on the game state at that tick.

| Name | Description |
|---|---|
| `wum.wormsign.tick()` | `{tick, engine, mods}` for the last completed tick: `tick` is a number, `engine` and `mods` are 16-digit hex strings (`mods` is `"0000000000000000"` when no mod contributes). `nil` outside a match or before the first tick. |
| `wum.wormsign.library()` | An array of the recorded matches (newest first): `{path, bytes, ticks, land, online, complete, pinned, flagged}` per entry. Empty when `[Wormsign] Record=0` or nothing has been recorded yet. |
| `wum.wormsign.onDivergence(fn)` | Calls `fn(payload, name)` when a tick's hash differs from another player's (or from a recording being replayed); returns a handle for `wum.events.off`. `payload` is `{source, tick, serial, comps, contrib, peer}`: `source` is `"peer"` or `"replay"`, `comps` an array of the engine parts that differ (`"time+rng"`, `"turn"`, `"worms"`, `"tasks"`, `"projectiles"`, `"teams"`), `contrib` the first differing mod contributor or `""`, `peer` the other player's SteamID as a string. The same payload is the client event `wormsign.divergence`. |

`mods` covers every sim mod of the match. Sim scripts are not covered by this page, but two things feed `mods` from them:

| Name (sim VM) | Description |
|---|---|
| automatic, `mod.<id>.env` | A digest of the mod's globals and `wum.sim.storage`, three tables deep and independent of key order. Numbers count by their stored bits, strings and booleans by value, functions, userdata and deeper tables by type only. `[Wormsign] EnvDigest` chooses when it runs: `changed` (default, after the mod's code ran), `always` or `off`. |
| `wum.sim.hash(v, ...)`, `mod.<id>.hash` | Adds numbers (their stored bits), strings, booleans or `nil` to this tick's hash; any other type raises an error. For state kept in locals, which the digest cannot see. |

C++ modules add their own contributors with `melange::wormsign::AddContributor` (`melange/wormsign.h`). A contributor that takes over 20 µs at the 95th percentile over 500 ticks is hashed only every 10 ticks from then on, and the log says so.

## Extending the API from C++

C++ modules can add namespaces with `melange::lua::AddLibrary("name", open)` (`melange/lua.h`). They appear as `wum.<name>` in
environments created afterwards. `melange::lua::CurrentMod()` tells a library function which mod is calling it.
