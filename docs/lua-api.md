# Lua API for client mods (`wum.*`)

A mod's `entry.client` script (from its `spice.json`) runs in Melange's Lua 5.4 VM, the Sandbox. This page lists everything a
client script can use. The offline self-test (`sandbox_selftest`) fails if a `wum.*` function is missing from this page.

Sim scripts (`entry.sim`) run in the game's own Lua 5.0 match VM and have a different API (`wum.sim.*`); they are not covered here. They are documented in the [developer guide](developer-guide.md#sim-scripts), including the Melange 0.11
additions `wum.sim.worm(slot)` (worm state and the anchor), `wum.sim.worms()`, `wum.sim.land()` and the turn-start hold
`wum.sim.holdTurn(ticks)`, `wum.sim.releaseTurn()` and `wum.sim.turnHold()`. A client mod sees a hold through the events
`melange.turnHold` and `melange.turnHoldEnd` below.

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
  - `melange.turnHold` `{ticks, tick, roundMsLeft}` and `melange.turnHoldEnd` `{ticks, tick, roundMsLeft}`: a sim mod
    held the next turn start with `wum.sim.holdTurn` (`ticks` is the hold's length, or at the end the ticks held;
    `tick` the sim tick; `roundMsLeft` the round clock, which does not run during the hold). See the developer guide's
    [sim scripts](developer-guide.md#the-turn-start-hold).
  - `melange.turn` `{index, team, worm, mine, ai, online}`: a turn started (after the game state snapshot of that
    frame); see `wum.game.turn()`. `melange.ui` `{weaponPanel, pause, readyIntro}`: one of `wum.game.ui()`'s flags
    changed. (Melange 0.12)
  - `melange.lobby.enter` `{isHost}`, `melange.lobby.leave`, `melange.lobby.members`, `melange.lobby.owner`
    `{owner, isHost}`, `melange.lobby.phase` `{phase}`: see [`wum.lobby`](#wumlobby). (Melange 0.12)
  - `melange.media` `{type, ...}`, `melange.input.mouse`, `melange.input.wheel`, `melange.input.key`,
    `melange.input.char`, `melange.input.paste`: delivered **only to the mod they concern**; see
    [`wum.media`](#wummedia) and [`wum.input`](#wuminput). (Melange 0.12)
  - Events that C++ modules post with `melange::lua::PostEvent` (to every listener) or `melange::lua::PostEventTo`
    (to one mod's listeners, Melange 0.12).
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
| `wum.game.online()` | A net session exists: the online lobby, its loading screens, the match and its results (Melange 0.12; before 0.12 it was true only once the online match was running). |
| `wum.game.turn()` | `{index, team, worm, ai, mine, phase}`: `index` = turns started in this match; `team` = the current team's index and `worm` = the active worm's index, both absent between turns; `ai` = the current team is computer-controlled; `mine` = the current turn is played on this machine (online: the turn owner's player is local, kept across the gap between turns; offline: the team is local and not AI); `phase` = `"intro"` (the hot-seat timer), `"play"`, `"retreat"`, `"settle"` or `"none"`. The fields after `index` are Melange 0.12. |
| `wum.game.ui()` | `{weaponPanel, pause, readyIntro, feCursor, attract}` booleans: the weapon panel is open, a pause menu or popup is up, the "Ready?" turn intro shows, the frontend's own menu cursor is active, the attract demo runs. `nil` on an unrecognised build. Changes are also sent as `melange.ui`. (Melange 0.12) |
| `wum.game.tick()` | Simulation ticks (50 per second) since the match started, when the sim bridge is running; otherwise 0. |
| `wum.game.worms()` | The worms of the current match, an array of `{slot, team, name, health, alive, pos={x,y,z}, vel={x,y,z}, yaw, weapon}` (`vel` is the engine's own velocity in world units per second: the game keeps it per millisecond of game time and Melange multiplies by 1000; it is the worm's physics velocity, so it is about zero while the worm stands or walks and large while it flies, falls or is knocked back; `weapon` is the weapon id, absent when none; `yaw` is the facing angle in radians about +Y, the facing direction is `(sin yaw, 0, cos yaw)`, it is not wrapped to a range and it spins while a worm is thrown). Empty outside a match; `nil, "unavailable"` on an unrecognised game build or with `[GameState] Enabled=0`. |
| `wum.game.teams()` | The teams of the current match, an array of `{slot, name, active, ai, local}`; empty outside a match, `nil, "unavailable"` as above. |
| `wum.game.activeWorm()` | The slot of the worm whose turn it is, or `nil`. |
| `wum.game.theme()` | The level theme of the current match as the game names it (for example `"SPACE"`), or `nil` outside a match. |
| `wum.game.match()` | The current match's clocks and levels, or `nil` outside a match (and on an unrecognised build or with `[GameState] Enabled=0`). See [below](#wumgamematch). |
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

### `wum.game.match`

`wum.game.match()` returns a table read from the game's own data, from the same snapshot as `wum.game.worms()` (read
once per frame), or `nil` outside a match. Detect it with `if wum.game.match then ... end` (Melange 0.11 and later).

| Field | Meaning |
|---|---|
| `roundMs`, `roundMsLeft` | The game's `RoundTime` and `RoundTimeRemaining`, integer milliseconds. |
| `turnMs`, `turnMsLeft` | `TurnTime` and `TurnTimeRemaining`, integer milliseconds. |
| `waterLevel` | `Water.Level`, the water surface height in world units. |
| `suddenDeath` | `true` once sudden death started in this match: the versus script sends `Comment.SuddenDeath` when the round clock runs out (`GameLogic.ActivateSuddenDeath` counts too, for scripts that send it). |
| `turnsStarted` | Turns started in this match (`GameLogic.Turn.Started`). |
| `landMin`, `landMax` | `{x, y, z}`: the landscape's bounding box (`Land.MinBounds`, `Land.MaxBounds`). Absent until the level has reported a valid box. |

The land box is the game's own. `landMin.y` is the lowest point of the land, which is usually under the water (-76 and
-61 on the two quick-match maps tried, with the water at 0), and `landMin.x`/`z` can sit at the arena's edge rather
than at the land (-1500 on both). `roundMs` is the round's full length from the scheme, so the time played is
`roundMs - roundMsLeft`.

These are this machine's values, for display. A client script must not feed them into anything that has to match
between players; sim scripts read deterministic state through `wum.sim`.

## `wum.ui`

| Name | Description |
|---|---|
| `wum.ui.panel(id, title, fn[, open])` | Adds an overlay panel. `fn()` draws its contents every frame while it is open. Returns a handle. The overlay's *View* menu lists it as *<mod name> > <title>*; a `/` in `title` (`"Tools/Grid"`) adds a submenu level. |
| `wum.ui.menu(path, fn[, opts])` | Adds a menu item `Mods/<mod name>/<path>` (for example `"Reset"`). `opts.checked` is an optional function asked every frame the menu is open: the item shows a check mark while it returns true, for an item that switches something on and off (`wum.ui.menu("Grid", toggleGrid, {checked = function() return gridOn end})`). An error in it counts as a fault (the getter is disabled after 3, the item still works) and reads as unchecked. Returns a handle. |
| `wum.ui.hotkey(keys, fn)` | Calls `fn()` on a hotkey such as `"Ctrl+Shift+H"`, whether or not the overlay is shown. The key is kept from the game. A hotkey needs at least one of Ctrl, Shift or Alt, except that a bare function key (`"F1"` to `"F12"`) is accepted; any other key without a modifier raises an error, because it would take an ordinary gameplay key away from the game. |
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
| `wum.draw.hudText(x, y, text, color[, size[, frames]])` | HUD text. Melange 0.12 also takes `wum.draw.hudText(x, y, text, color, size, opts)` with `opts`: `font` (a `wum.draw.font` id; UTF-8, kerning, a missing glyph draws `?`), `shadow` (a colour, or `false`), `shadowOffset = {dx, dy}`, `align = "left"` \| `"center"` \| `"right"`, `maxWidth` (pixels) and `ellipsis` (cut with an ellipsis at `maxWidth`). |
| `wum.draw.hudImage(x0, y0, x1, y1, texture[, tint[, frames]])` | HUD image from `wum.draw.texture`. |
| `wum.draw.texture(rel[, opts])` | Loads a PNG from the mod folder; returns a texture id, or `nil, err`. Freed when the mod unloads or reloads. A mod can hold 256 at a time. Textures get a full mip chain and trilinear filtering. With `opts`, the texture is for a shader (see [Shader textures](#shader-textures)): `{wrap = "repeat"\|"clamp", filter = "linear"\|"nearest", layers = n}`. `{mips = false}` (Melange 0.12) loads a HUD texture without mips (linear filtering), which saves a quarter of its memory. From Melange 0.12, `hudImage`, `hudImageUV` and `hudNine` take only the calling mod's own textures. |
| `wum.draw.sprite(tex, x, y, z, halfW, halfL, ax, ay, az[, color[, mode]])` | Textured world sprite; only inside a `"world"` or `"worldLate"` callback. Returns `true`, or `false` when dropped by the per-frame cap. See [Sprites](#sprites). |
| `wum.draw.mesh(verts, idx, tex, model[, opts])` | Triangle mesh from vertex and index arrays; only inside a `"world"` or `"worldLate"` callback. Returns `true`, or `false` when over the per-frame cap. See [Meshes](#meshes). |
| `wum.draw.mesh(handle, tex, model[, color[, opts]])` | The same for a mesh kept with `wum.draw.newMesh`. |
| `wum.draw.newMesh(verts, idx[, format])` | Keeps a mesh in Melange so it is not sent every frame; returns an integer handle, or `nil, "too many meshes"`. Freed when the mod unloads or reloads. |
| `wum.draw.freeMesh(handle)` | Frees a kept mesh; `true` if the handle was one of this mod's live meshes. |
| `wum.draw.on(stage, fn)` | Calls `fn(stage)` every frame at `"world"`, `"worldLate"`, `"hud"` or `"screen"`; returns a handle. `"screen"` (Melange 0.12) runs at present time in window pixels, after the game's composite and every post effect, before the overlay, in the frontend, the lobby, loading screens and matches alike; it is never tinted or post-processed. The `hud*` primitives work there. |
| `wum.draw.hudImageUV(x0, y0, x1, y1, tex, u0, v0, u1, v1[, tint])` | A HUD image showing only the texture rectangle `(u0, v0)`-`(u1, v1)` (0..1), for atlases. (Melange 0.12) |
| `wum.draw.hudNine(x0, y0, x1, y1, tex, l, t, r, b[, opts])` | A nine-slice panel: `l, t, r, b` are the source insets in texture pixels; the corners are drawn at `scale * inset`, the edges and the centre are stretched. `opts`: `tint`, `scale` (default 1), `uv = {u0, v0, u1, v1}` to take the slice from part of an atlas. (Melange 0.12) |
| `wum.draw.font(atlasPng, metricsJson)` | Loads a bitmap font from two files in the mod folder (format `holo-font-1`, see [Mod fonts](#mod-fonts)); returns a font id, or `nil, reason`. At most 4 per mod; freed on unload and reload. (Melange 0.12) |
| `wum.draw.textSize(text, size[, opts])` | `w, h` in pixels of `text` drawn by `hudText` at `size`, with `opts.font` for a mod font. (Melange 0.12) |
| `wum.draw.hudClip(x0, y0, x1, y1)` | Clips the following HUD primitives of this callback to the rectangle (nests 4 deep, intersecting); `wum.draw.hudClipEnd()` ends the innermost clip. Clips still open end with the callback. (Melange 0.12) |
| `wum.draw.media(player, x0, y0, x1, y1[, opts])` | Draws a [`wum.media`](#wummedia) player's current frame into the rectangle and returns the picture rectangle `px0, py0, px1, py1`, or `nil` when there is no frame yet. `opts`: `fit = "contain"` (default, letterboxed) \| `"cover"` \| `"stretch"`, `alpha` 0..1. Only the mod that opened the player can draw it; drawing it is what keeps its frames uploading. (Melange 0.12) |
| `wum.draw.off(handle)` | Removes a draw callback. |

### Mod fonts

`wum.draw.font` (Melange 0.12) reads a PNG atlas (at most 2048x2048: white glyphs with their outline baked in black, so
the `hudText` colour tints the fill and the outline stays black) and a JSON metrics file:

```json
{"format": "holo-font-1", "size": 28, "scale": 2, "lineHeight": 34, "ascent": 26, "outline": 2,
 "glyphs": {"65": [0, 0, 30, 36, 1, -26, 29]}, "kern": {"65,86": -2}}
```

`size` is the pixel size at scale 1, and the atlas is baked at `scale` times that. Glyph keys are Unicode code points in
decimal, each `[x, y, w, h, xoff, yoff, adv]` in atlas pixels; `kern` pairs adjust the advance between two code points.

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

### Meshes

`wum.draw.mesh` draws an indexed triangle mesh in the world, for shapes sprites cannot make: a mound of sand, a cast,
a shell. Detect it with `if wum.draw.mesh then ... end` (Melange 0.11 and later).

- `verts`: a flat array of numbers, 8 per vertex: `x, y, z, nx, ny, nz, u, v` (position, normal, texture
  coordinate). With the format `"pntc"` (the array form's `opts.format`, or `newMesh`'s third argument) it is 12 per
  vertex, adding `r, g, b, a` in 0..1, multiplied with everything else. Normals need not be unit length (a zero normal
  reads as +Y). At most 16384 vertices.
- `idx`: a flat array of 1-based vertex numbers, three per triangle (at most 49152). Front faces wind
  counter-clockwise as seen from outside.
- `tex`: a texture id from this mod's `wum.draw.texture`, or `nil` (or `0`) for none. A texture loaded with
  `{wrap = "repeat"}` tiles when `u, v` go past 0..1; a texture array cannot texture a mesh. The texture is multiplied
  with the vertex colour.
- `model`: where the mesh goes. `nil` (the vertices are world coordinates), 16 numbers (a column-major matrix with the
  translation in 13..15, as `glLoadMatrixf` takes it), or `{pos = {x, y, z}, yaw = radians, scale = s or {x, y, z}}`:
  scaled, then turned about +Y by `yaw` (+Z turns to `(sin yaw, 0, cos yaw)`, a worm's facing at that `yaw`), then
  moved to `pos`.
- `color` (handle form) or `opts.color` (array form): a tint in any colour form; its alpha fades the mesh.
- `opts`:

| Option | Default | Meaning |
|---|---|---|
| `lit` | `true` | Shades each vertex `ambient + (1 - ambient) * max(0, N . light)` with its world-space normal. `false`: colours as given. |
| `light` | `{0, 0.936, -0.351}` | The world direction toward the light (normalised); the default is the game's landscape light. |
| `ambient` | `0.6` | 0..1. |
| `blend` | `"alpha"` | `"alpha"` (`SRC_ALPHA, ONE_MINUS_SRC_ALPHA`) or `"additive"` (`SRC_ALPHA, ONE`). |
| `depthWrite` | `true` | Writes depth, so later world draws (particles, sprites) behind the mesh are hidden. Turn it off for see-through meshes. |
| `cull` | `true` | Skips back faces. `false` draws both sides. |
| `xray` | `false` | `true` draws without the depth test, over everything. |
| `color`, `format` | | The array form only, see above. |

The mesh is depth-tested against the scene, so land and worms in front of it hide it. Lighting is computed on the CPU
for each draw and follows `model`'s rotation: a mesh turned with `yaw` keeps its light from the same world direction.
Meshes draw at the callback's stage before that stage's sprites, in call order (they are not sorted), so draw
see-through meshes after opaque ones.

A mod can keep 64 meshes with 262144 vertices in all. Each frame it can make 256 `wum.draw.mesh` calls drawing up to
131072 vertices across its callbacks; past that, calls do nothing and return `false`. Bad arguments (an index out of
range, a vertex array that is not 8 or 12 numbers per vertex, a non-finite number, another mod's texture or handle,
an unknown option) raise an error.

```lua
-- A dome of sand at each worm's feet, built once and drawn every frame.
local sand = wum.draw.texture("fx/sand.png", {wrap = "repeat"})
local function dome(rings, segs, radius, height)
  local v, idx = {}, {}
  for r = 0, rings do
    local t = r / rings
    local rr, y = radius * math.sin(t * math.pi / 2), height * math.cos(t * math.pi / 2)
    for s = 0, segs do
      local a = s / segs * 2 * math.pi
      local x, z = rr * math.cos(a), rr * math.sin(a)
      for _, n in ipairs({x, y, z, x / radius, y / height, z / radius, s / segs * 4, t * 2}) do v[#v + 1] = n end
    end
  end
  for r = 0, rings - 1 do
    for s = 0, segs - 1 do
      local a, b = r * (segs + 1) + s + 1, (r + 1) * (segs + 1) + s + 1
      for _, i in ipairs({a, a + 1, b, a + 1, b + 1, b}) do idx[#idx + 1] = i end
    end
  end
  return wum.draw.newMesh(v, idx)
end
local mound = dome(8, 24, 14, 6)
wum.draw.on("world", function()
  for _, w in ipairs(wum.game.worms() or {}) do
    if w.alive then wum.draw.mesh(mound, sand, {pos = w.pos}, "#e8c890ff") end
  end
end)
```

## `wum.render`

| Name | Description |
|---|---|
| `wum.render.camera()` | `{pos, fwd, up, near, far, fov, overridden}` of the main camera as it is drawn (with a camera override, the override's), or `nil`. `fov` is the vertical field of view in degrees; `overridden` is `true` while a camera override is set. |
| `wum.render.gameCamera()` | The same for the game's own camera, as it would be drawn without an override (to blend back into it), or `nil`. |
| `wum.render.cameraOverride(spec)` | Draws the frame from a camera of the mod's choosing; `nil` clears it. See [Camera override](#camera-override). |
| `wum.render.worldToScreen(pos)` | `x, y, depth` in window pixels, or `nil` behind the camera. |
| `wum.render.windowSize()` | `width, height`. |
| `wum.render.timing()` | `{busyMsP50, busyMsP95, frameMsP50, fps, frames}`. |

### Camera override

`wum.render.cameraOverride({pos = {x, y, z}, target = {x, y, z}[, up = {x, y, z}][, fov = degrees][, frames = n]})`
replaces the camera the frame is drawn with, for cut-scenes, tours and replays. Detect it with
`if wum.render.cameraOverride then ... end` (Melange 0.11 and later).

- `pos` is the eye and `target` the point it looks at (not the same point). `up` is the up direction (default
  `{0, 1, 0}`; when it is parallel to the view direction the game's own fallback applies). `fov` is the vertical field
  of view, 5 to 150 degrees (default: the game's); the aspect ratio and depth range stay the game's.
- `frames`, 1 to 600 (default 30): the override ends by itself when it has not been set again for that many frames, so
  a mod that stops (an error, a disabled callback) gives the camera back. Set it every frame while it should last. A
  value set in a frame is drawn from the next frame on.
- It also ends at the end of the match, when the mod unloads or reloads, and with `wum.render.cameraOverride(nil)`,
  which returns `true` if the mod's own override was cleared.
- Returns `true`; `false, "busy"` while another mod's override is set (one mod at a time); `false, "unavailable"`
  outside a match or on an unrecognised game build. Bad fields raise an error.

The override only changes the picture. The game keeps its own camera: aiming, the turn camera, the camera keys and
the logical camera that online games and Wormsign compare between machines (reasons 6 to 10 in `docs/wormsign.md`)
are untouched, so an override on one machine changes nothing on another. Melange puts the override into the engine's
render camera only while the frame is drawn, and the whole frame agrees with it: the land, sea, worms, particles and
labels, the engine's culling, `wum.draw` primitives, sprites and meshes, post-FX (`mg_proj`, `mg_invProj` and depth
reconstruction from them) and GLSL replacements. `wum.render.camera()`, `wum.render.worldToScreen()` and positional
`wum.audio` follow it everywhere, so HUD labels placed with them stay on their world points.
`wum.render.gameCamera()` reads the game's camera, which keeps moving underneath.

Two things still follow the game's camera, because the game decides them before drawing: the land the game fades
out when it stands between its camera and the active worm, and the picture-in-picture view of a computer player's aim.

```lua
-- Orbit the active worm for five seconds, then hand the camera back.
local t0 = os.clock()
wum.events.on("melange.frame", function()
  local t, slot = os.clock() - t0, wum.game.activeWorm()
  if t > 5 or not slot then return end
  for _, w in ipairs(wum.game.worms() or {}) do
    if w.slot == slot then
      local a, p = t * 0.8, w.pos
      wum.render.cameraOverride{pos = {p.x + 140 * math.sin(a), p.y + 60, p.z + 140 * math.cos(a)},
                                target = {p.x, p.y + 10, p.z}, fov = 45}
    end
  end
end)
```

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
| `wum.shaders.setTexture(file, entry, sampler, tex)` | Binds a texture from this mod's `wum.draw.texture` (or `nil` to unset) to a sampler of the mod's own `shaders\<File>.<Entry>.glsl` replacements, e.g. `("FixedFunction.cg", "FFFragmentMainTexLit*", "materials", tex)`. `entry` is a glob. Returns `true`, or `false` while GLSL replacements are off (`[MirageShaders] GlslReplace=0`). Errors on another mod's replacement or texture, or a name starting with `gl_` or `mg_`. Melange 0.10.0 and later; see [Shader textures](#shader-textures). |

### Shader textures

A GLSL replacement can sample a mod's own tileable textures, for example a set of material detail maps:

```lua
if wum.shaders.setTexture then  -- Melange 0.10.0 and later
  local mats, err = wum.draw.texture("textures/materials.png", {wrap = "repeat", layers = 8})
  if mats then
    wum.shaders.setTexture("FixedFunction.cg", "FFFragmentMainTexLit*", "materials", mats)
  end
end
```

```glsl
#version 130
uniform sampler2D texture0;         // the engine's texture
uniform sampler2DArray materials;   // the mod's texture array
// ...
vec4 m = texture(materials, vec3(uv * 8.0, layer));
```

- `opts.layers = n` (1 to 64): the PNG is a vertical strip of `n` equal tiles, layer 0 at the top, and loads as one
  texture array (`sampler2DArray`). Without `layers` it is an ordinary 2D texture (`sampler2D`). Arrays need GL 3.0
  and cannot be sprites. The image height must be a multiple of `n`.
- `opts.filter`: `"linear"` (the default) is trilinear, with a full mip chain per layer and the anisotropy from
  `[MirageTextures]` (the merged mod requests, see the developer guide). `"nearest"` keeps only the base level, for data
  tables read with `texelFetch` or at texel centres. Mips average every channel as stored: alpha is data here.
- `opts.wrap`: `"clamp"` (the default, to the edge) or `"repeat"`.
- A replacement's sampler that has no Cg parameter gets its own texture unit (8 to 13, six per program at most) and
  reads a neutral 1x1 texture, 0.502 in every channel, until a texture is set. A sampler with a Cg parameter's name
  (`texture0`, `shadowMap`, ...) is the engine's and cannot be set.
- `sampler2DArray` and `texture()` need `#version 130` in the replacement. A `#version 130` fragment replacement links
  with the game's vertex programs, which Cg compiles to GLSL 1.10 writing `gl_TexCoord[]`: keep reading
  `gl_TexCoord[n]` and writing `gl_FragColor`, which GLSL 1.30 still has.
- On an older Melange, `wum.shaders.setTexture` is `nil`, and `wum.draw.texture` ignores `opts` and returns a clamped
  2D texture. The extra sampler then shares unit 0 with `texture0`, and a `sampler2DArray` there makes the program's
  draws fail. Require this Melange in `spice.json`, or pause the replacement with
  `wum.shaders.enableGlsl(file, entry, false)` when `wum.shaders.setTexture` is `nil`. See the developer guide's
  Shaders section.

Per-texture values for a replacement (`uniform vec4 mg_texTag[2]`) come from a table the mod declares in `spice.json`
(`graphics.textureTags`), not from Lua: see [Texture tags](developer-guide.md#texture-tags).

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

## `wum.input`

Mouse look and aim options, and what the game's controls are bound to. Everything here changes the numbers the game
puts into its own mouse messages on this machine, before they are sent, so other players and Wormsign recordings
receive the same final values as for any other input: no option can desynchronise a match. The namespace needs build
#1077 and `[Controls] Enabled=1` (the default); otherwise `groups()` returns `nil, "unavailable"` and the other
functions do nothing useful (`binding()` returns `nil`, `setOptions` is accepted and has no effect, though `options()` reports what it set).

| Name | Description |
|---|---|
| `wum.input.groups()` | An array of the active control-group names in index order, for example `{"InGame", "WormFirstPersonAiming"}`. The names are `Menu`, `InGame`, `WormAiming`, `WormFirstPersonAiming`, `WormMoving`, `WormRoping`, `UtilityGirder`, `Flying`, `CameraSelect`, `Fire`, `UtilityFire`, `CrateChuteRelease`, `Spectator`, `NetworkSpectator`, `AttractMode`, `ControllerRemoved`, `ManualCam`, `EFMVMovie` and `XboxSignIn`. An empty table when the game has no input service yet; `nil, "unavailable"` on an unknown build or with `[Controls] Enabled=0`. |
| `wum.input.binding(messageName)` | A short label of the key mapped to an engine message (`"Space"`, `"Q"`), or of the mouse button (`"LMB"`, `"MMB"`, `"RMB"`) when no key is, or `nil` when neither is (joypad bindings are not reported). Names are the game's message names, for example `"Input.JumpPressed"` or `"Input.FirePressed"`. |
| `wum.input.setOptions(t)` | Sets the options in `t` and returns `true`. `t` fields are all optional, and a missing one means the game's own behaviour: `cameraInvertY` = `"game"`, `"standard"` (mouse up looks up) or `"inverted"`; `aimInvertY` = the same for aiming; `blimpInvert` = a boolean, applies the camera invert in the Blimp view too, which the game skips; `cameraSensitivity` = a number from 0.25 to 3.0 (a number outside the range is clamped), scales camera and spectator-camera motion; `aimSensitivity` = the same for aiming. A wrong type or an unknown string raises an error. `setOptions(nil)` clears everything. The last caller wins, and a mod's options are cleared when that mod unloads or is disabled. |
| `wum.input.options()` | A table of the effective values: the five fields above (`"game"`, `false` and `1.0` when unset) and `smoothMouse`, a boolean owned by Melange's `[Controls] SmoothMouse` setting that Lua cannot change. |

Scaled motion keeps its fractional part from one mouse event to the next, so a low sensitivity does not drop slow
movements.

### Mouse regions, pointer mode and keyboard focus

Melange 0.12 lets a client mod take mouse clicks and typing for its own on-screen controls, without them reaching the
game. This is local input filtering only: a click or key a mod takes never becomes a game input, so nothing is sent to
other players and the match cannot desynchronise. `[Input] AllowModInput=0` turns all of it off (every call then
returns `false, "unavailable"` or `nil`).

| Name | Description |
|---|---|
| `wum.input.cursor()` | `x, y, source` in window pixels: `source` is `"frontend"` (the game's own menu cursor, in menus and the lobby) or `"pointer"` (this mod's pointer mode in a match). `nil` in a match without pointer mode, since the game hides and re-centres the mouse there. |
| `wum.input.region(id, x0, y0, x1, y1[, opts])` | Claims a rectangle for the next `opts.frames` frames (default 2), so renew it every frame while the control shows. While the cursor is inside, button presses, double-clicks and the wheel go to this mod instead of the game (mouse moves still reach the game, and a button release whose press the game saw is always passed on). `opts.cursor`: `"arrow"`, `"hand"` or `"ibeam"`. Returns `true`, or `false, "busy"` when another mod's region or focus has priority. `id` is a string up to 64 bytes. |
| `wum.input.hover()` | The id of this mod's topmost region under the cursor, or `nil`. |
| `wum.input.pointer(on[, opts])` | Pointer mode for a match: the game stops re-centring the mouse and gets no mouse input, and Melange draws a cursor (`opts.cursor` = a texture of this mod, or the default arrow). The keyboard still reaches the game. `opts.release`: `"click-outside"` (default; a click outside every region of this mod ends it and is not passed on) or `"manual"`. It also ends on `pointer(false)`, Esc (consumed), the overlay taking input, the end of the match, focus loss, unload, and when a turn of this machine's player starts. On exit the cursor goes back to the centre and the game sees no mouse jump. Returns `true`, or `false, "busy"` / `false, "unavailable"` (outside a match). |
| `wum.input.pointerActive()` | `true` while this mod's pointer mode is on. |
| `wum.input.focus(on)` | Keyboard focus for a text box of this mod: keys and characters go to this mod (`melange.input.key`, `melange.input.char`, `melange.input.paste`) and not to the game. Hotkeys with Ctrl, Shift or Alt still work; bare F-key hotkeys do not while focused. Ends on `focus(false)`, Esc (delivered first), the overlay taking input, focus loss or unload. Returns `true` or `false, "busy"`. |

Priority: the overlay first, then the mod with focus or pointer mode, then regions (the most recently loaded mod
first), then the game. Events, delivered at the next frame to the mod that owns the region, focus or pointer only:

- `melange.input.mouse` `{region, button, down, x, y, double}`: `button` is `"left"`, `"right"`, `"middle"`, `"x1"` or `"x2"`.
- `melange.input.wheel` `{region, delta, x, y}`: `delta` in notches (positive away from the user).
- `melange.input.key` `{key, down, repeat, ctrl, shift, alt}`, `melange.input.char` `{text}` (UTF-8) and
  `melange.input.paste` `{text}` (Ctrl+V: the clipboard text, at most 4096 bytes, control characters removed): focus only.

A click that Melange delivered to a mod also counts as that mod's user gesture, which `wum.media.install` requires.

## `wum.audio`

Plays sounds from the mod folder on this machine. It is client-only: the library exists in the client VM and not in the
match VM, the sound is mixed and played locally, and nothing about it reaches the simulation, the wire, the content
handshake or other players. Each player hears only what their own copy of the mod plays. The backend is XAudio2 2.9
(`xaudio2_9.dll`, part of Windows 10 and later); where it or an output device is missing, `ready()` is `false` and
`play` returns `nil, "audio unavailable"` (`load` still works, since it only reads the file). It is also unavailable with `[Audio] Enabled=0`.

Files must be WAV: PCM (format 1, or `WAVE_FORMAT_EXTENSIBLE` with a PCM subformat), 16-bit, 1 or 2 channels, 8000 to
48000 Hz, at most 2 MiB. Other RIFF chunks are skipped. Anything else (24-bit, float, ADPCM, a truncated file) returns
`nil` and a short reason such as `"not 16-bit"` or `"too large"`. Convert other formats first.

| Name | Description |
|---|---|
| `wum.audio.ready()` | `true` when sound can play. The first call starts the audio backend, so a mod can check once at start-up. |
| `wum.audio.load(rel)` | Reads and decodes a WAV from the mod folder (`rel` is relative to it, with no `..`, drive or leading slash) and returns a handle, an integer, or `nil, reason`. Loading the same path again returns the same handle without reading the file; paths that differ only in case, in `/` against `\`, or by a `.` segment count as the same path. `load` does not need the audio backend, so a `load` at the top of the mod works even if the output device comes up later. A mod holds up to 64 sounds and 16 MiB of decoded audio at a time; past that `load` returns `nil, "too many sounds loaded"` or `nil, "sound memory limit reached"`. Handles are valid only in the mod that loaded them and are freed when the mod unloads, reloads or is disabled. |
| `wum.audio.play(handle[, opts])` | Starts the sound and returns a voice id, an integer, or `nil, reason` (`"audio unavailable"`, `"unknown sound"`, `"too many voices"`). `opts` is a table, all fields optional: `volume` = 0 to 2, default 1; `pitch` = a frequency ratio from 0.5 to 2.0, default 1 (2.0 is an octave up and half as long); `loop` = a boolean, default `false`, a looping voice plays until `stop` or `stopAll`; `pos` = `{x=, y=, z=}` (or `{x, y, z}`) in world units, which makes the sound positional. Numbers outside a range, infinities included, are clamped; a `NaN` volume or pitch is replaced by the default, and a `NaN` `pos` component makes the sound silent. Without `pos` the sound is plain 2D. |
| `wum.audio.stop(voice)` | Stops a voice and returns `true`, or `false` if it is unknown, has already ended, or belongs to another mod. |
| `wum.audio.stopAll()` | Stops every voice this mod has playing and returns how many. |

A positional sound is attenuated by its distance to the render camera: full volume within `[Audio] Near` (150 world
units by default), falling linearly to silence at `[Audio] Far` (1500), recalculated every frame, so a moving camera
and a long or looping sound follow each other. There is no panning. If the camera is not available the sound plays at
its own volume. The `Volume` setting (0 to 1) scales everything the mod plays, and at most `[Audio] MaxVoices` (default
32, up to 64) voices play at once across all mods; a new `play` over that returns `nil, "too many voices"` and does not
cut off a playing one. A voice stopped with `stop` or `stopAll` frees its slot at once, so a `play` right after it has room.

Voices end by themselves when the sound finishes. Every voice that started before the match ended is also stopped when
it does, and a mod's voices when that mod unloads, reloads or is disabled. A sound started from a `melange.match.end`
handler is not stopped (the handler runs in the same frame as the stop), so it can be used for a victory sound; it plays
on into the menu. This is expected from the code and not yet tried in game. Errors are raised only for a wrong argument type (a handle that is not a
number, `opts` that is not a table, a `volume` or `pitch` that is not a number, `loop` that is not a boolean, `pos`
that is not a table); everything else returns `nil, reason`.

```lua
local boom = wum.audio.load("sfx/boom.wav")      -- nil, reason if it is missing or not a usable WAV
wum.events.on("melange.match.start", function()
  if boom then wum.audio.play(boom, {volume = 0.8}) end                  -- plain 2D
end)
-- positional, a little random pitch so repeats do not sound identical:
--   wum.audio.play(boom, {pos = {x = 120, y = 40, z = -300}, pitch = 0.9 + math.random() * 0.2})
```

### `wum.audio.music`

Melange 0.12 lets a client mod silence the game's own music (the menu and match themes, sudden death, victory and the
frontend music, and replacement music from mods such as Kanly's sudden-death bank) while it plays something else.
It mutes the game's FMOD music category; sound effects, speech and ambience are untouched, the music keeps running
silently (so nothing that waits on it changes), and the player's saved music volume is never written. Detect it with
`if wum.audio.music then ... end`.

| Name | Description |
|---|---|
| `wum.audio.music.available()` | `true` once the game's music category was found (a request made earlier is kept and applied then). |
| `wum.audio.music.suppress(on[, opts])` | Asks for the game music to be silenced (`true`) or released (`false`). `opts.fade` = seconds (default 0.4, 0 = at once). Returns `true`, or `nil, "unavailable"` / `nil, "disabled"` (`[Audio] AllowMusicSuppress=0`). Each mod has one request; the music is silent while any mod asks. A mod's request is released when it unloads, reloads or is disabled. |
| `wum.audio.music.suppressed()` | `effective, mine`: whether the music is silenced (by any mod), and whether this mod asks for it. |

`wum.audio.play` also takes `{kind = "music"}` in Melange 0.12: such a sound is silenced together with the game music.

## `wum.media`

Melange 0.12 plays YouTube videos and playlists and torrent (magnet) videos for client mods, for watch parties such as
the Holo plugin. Everything runs on this machine for presentation only: it is not available to sim scripts, it is not
part of the content handshake, and nothing about it reaches the match or Wormsign. Detect it with
`if wum.media then ... end`; a mod that needs it declares `"melange": {"range": ">=0.12.0"}`.

The decoder runs in a separate 64-bit process, `holo-media.exe`, from the **media pack**: a download of about 95 MB
from Melange's own GitHub releases, installed once per machine into `Documents\Melange\media` and shared by every mod.
Melange checks every file of it against hashes compiled into `melange.asi`, and the helper's signature, before each
start. Nothing is downloaded until the user clicks the mod's download button (see `wum.media.install`). YouTube items
use yt-dlp, which Melange keeps up to date from the same releases (at most one check a day). See
[the media helper](media-helper.md) for the details, and the developer guide for the `[Media]` settings.

| Name | Description |
|---|---|
| `wum.media.available()` | `true`, or `false, reason`: `"disabled"` (`[Media] Enabled=0`), `"unsupported-os"` (32-bit Windows), `"unavailable"` (the module did not start). |
| `wum.media.pack()` | The media pack: `{state, version, progress, bytes, total, downloadMB, diskMB, ytdlp, error}`. `state` is `"missing"`, `"downloading"`, `"verifying"`, `"installing"`, `"ready"`, `"updating"`, `"error"`, `"unsupported"` or `"disabled"`; `progress` 0..1 and `bytes`/`total` while downloading; `downloadMB`/`diskMB` for the consent text; `ytdlp` the installed yt-dlp version; `error` (state `"error"` only) `"hash"`, `"disk-space"`, `"network"`, `"cancelled"` or `"blocked"`. |
| `wum.media.install()` | Starts the pack download. It must be called within 2 seconds of a click Melange delivered to this mod (a `melange.input.mouse` press and release inside one of its regions; the region must cover at most a quarter of the window and have existed for half a second): that click is the user's consent, so show the size, the source and the folder next to the button. Returns `true`, or `nil, reason`: `"gesture"`, `"busy"`, `"ready"`, `"disabled"`, `"unsupported-os"`, `"blocked"` (`[Media] AllowDownload=0`). Allowed in the menus, the lobby and matches; in a match the download is throttled (`[Media] MatchDownloadKBps`). |
| `wum.media.cancelInstall()` | Stops a running download; returns `true`. |
| `wum.media.classify(text)` | Checks a link the user typed or pasted. Returns `{kind = "youtube", id, playlist, live, url}` or `{kind = "magnet", infohash, name, url}`, or `nil, "unsupported-url"`. `url` is the canonical form, the only one to store or send to other players. Accepted: `https://` links to `youtube.com`, `www.`, `m.` or `music.youtube.com` (`/watch?v=`, `/playlist?list=`, `/shorts/`, `/live/`) and `youtu.be`, and `magnet:` links with a `btih` (40 hex or 32 base32 characters) or `btmh` infohash. Everything else is refused. A magnet keeps only its infohash, its name (`dn`, at most 200 bytes) and up to 8 `udp`, `http` or `https` trackers, and is at most 1024 bytes. Text is trimmed; more than 4096 bytes is refused. |
| `wum.media.resolve(url, fn)` | Lists a YouTube playlist (or one video): calls `fn({items = {{url, id, title, duration}, ...}, title})` or `fn({error, message})` with `error` = `"ytdlp"`, `"network"`, `"private"` or `"timeout"` (30 s). Returns a request id, or `nil, reason`. At most 4 in flight per mod and 200 items per result. |
| `wum.media.torrent(magnet[, fn])` | Starts a torrent from a magnet link (every file paused until one is played) and returns a torrent handle, or `nil, reason`: `"unsupported-url"`, `"pack"`, `"busy"` (4 open per mod), `"first-use-in-match"` (the first torrent of a session must start in the menus or the lobby, because Windows may ask to allow network access). `fn({files, name})` runs when the file list arrives, or `fn({error})`. |
| `wum.media.open(opts)` | Opens this mod's player (one per mod) and returns it, or `nil, reason`: `"busy"`, `"pack"`, `"unsupported-url"`, `"cache-cap"`, `"helper"`. `opts`: exactly one source, `url` (a YouTube video) or `torrent` + `file` (a file index); `start` (seconds), `paused` (default `true`), `volume` 0..100 (default 70), `mute`, `quality` = 360, 480, 720 or 1080 (default 720, capped by `[Media] MaxHeight`), `speed` 0.25..4, `audioLang` and `subLang` (language codes such as `"en"` or `"pt-BR"`). |

A torrent handle `t`:

| Name | Description |
|---|---|
| `t:files()` | `{{index, path, size, video}, ...}` once the file list arrived, else `nil`. `video` is `true` for mkv, mp4, m4v, webm, avi, mov, ts and ogv files; playlists and other reference files are never playable. |
| `t:status()` | `{down, up, peers, progress, have, metadata}`: bytes per second, peers, the played file's progress 0..1 and its bytes on disk. |
| `t:close()` | Stops the torrent and deletes its downloaded data. Torrents are also closed when the mod unloads or reloads. The data lives in a cache folder (`[Media] TorrentCacheMB`, 8 GB by default, under `AppData\LocalLow\Melange\media-cache`) that is swept at the next start if the game exits early. While a torrent is open it uploads to other peers (`[Media] TorrentUploadKBps`), and the swarm sees this machine's IP address. |

A player `p` (its methods return `nil, "closed"` after `p:close()`):

| Name | Description |
|---|---|
| `p:play()`, `p:pause()` | |
| `p:seek(t[, mode])` | Seeks to `t` seconds. `mode`: `"exact"` (default), `"keyframes"` (fast, while scrubbing) or `"relative"`. |
| `p:speed(x)` | Playback rate 0.25..4, pitch kept. |
| `p:volume(v)`, `p:mute(b)` | Volume 0..100 (already a perceptual curve), mute. |
| `p:track(kind, id)` | Selects an audio (`kind = "audio"`) or subtitle (`"sub"`) track by its id from `p:state().tracks`; `false` turns subtitles off. |
| `p:quality(h)` | Reopens the item at the current time with another height cap (360, 480, 720, 1080), keeping pause and speed. |
| `p:size(w, h)` | Tells the decoder how large the picture is drawn, so it renders no more pixels than needed (at most 1280x720). |
| `p:follow(st, hostId)` | Keeps this player in step with another machine's: `st = {e, i, s, p, r, a}` (epoch, item id, `"play"` or `"pause"`, position in seconds, rate, and the host's `wum.peer.now()` in ms at which `p` holds), `hostId` the host's SteamID string. Melange corrects the drift with the peer bus's clock: under 0.1 s nothing, up to 1.5 s a 5 % speed nudge, beyond that a seek slightly ahead; it never pauses the host. An older epoch or timestamp is ignored. `p:follow(nil)` stops following. |
| `p:state()` | `{time, duration, paused, buffering, cacheAhead, speed, title, eof, w, h, aspect, tracks, following, drift, syncAction, helper}`: `tracks = {audio = {{id, lang, title, selected}, ...}, sub = {...}}`; `drift` is this player's position minus the followed target in seconds (`nil` if not following); `syncAction` is `"none"`, `"nudge"` or `"seek"`; `helper` is `"starting"`, `"ready"`, `"restarting"` or `"dead"`. Read it every frame; it is not pushed as events. |
| `p:close()` | Closes the player. Players are also closed when the mod unloads or reloads. |

Draw a player with `wum.draw.media`. Events come as `melange.media` to the mod they concern only, with `type`:
`"loaded"` `{title, duration}`, `"ended"` `{reason}` (`"eof"`, `"error"` or `"stop"`), `"error"` `{code, message}`,
`"buffering"` `{on}`, `"tracks"`, `"helper"` `{state}` (the helper restarts by itself at most once a minute, reopening
the item where it was), `"pack"` `{state, progress}` (at most 4 times a second) and `"torrent"` `{infohash, files}`.
Errors are raised only for a wrong argument type; everything else returns `nil, reason`.

## `wum.lobby`

The Steam lobby this machine is in, as client mods see it (Melange 0.12). Detect it with `if wum.lobby then ... end`.

| Name | Description |
|---|---|
| `wum.lobby.state()` | `nil` outside a net session, else `{lobby, me, owner, isHost, phase, members}`: SteamIDs as strings; `phase` = `"lobby"`, `"loading"`, `"match"` or `"results"`; `members = {{id, name, me, host, melange, peer}, ...}` where `melange` is the member's Melange version (`nil` without Melange) and `peer` is `true` when the member has the calling mod's peer channel open. |
| `wum.lobby.isHost()` | `true` when this machine owns the lobby; `false` outside a session. |

Events (every listener): `melange.lobby.enter` `{isHost}`, `melange.lobby.leave`, `melange.lobby.members` (a member
joined or left), `melange.lobby.owner` `{owner, isHost}` (the lobby changed hands), `melange.lobby.phase` `{phase}`.

## `wum.peer`

Messages between copies of the same mod on the machines of one lobby (Melange 0.12), over Steam P2P channel 6, which
the game never uses. A mod only ever talks to its own copies: a channel reaches only members that opened the same mod's
channel with the same version, and only once the game already has its own connection to that member, so Melange's
traffic is never the first between two machines. None of it touches the match, the game's own channel or Wormsign.

| Name | Description |
|---|---|
| `wum.peer.open{version = n}` | Opens this mod's channel (one per mod) and advertises `<mod id>@<version>` to the lobby; returns a channel, or `nil, reason`. |
| `wum.peer.now()` | A monotonic clock in milliseconds (a float), the clock of every timestamp below. |

A channel `ch`:

| Name | Description |
|---|---|
| `ch:send(id, tbl[, opts])` | Sends a table (nil, booleans, numbers, strings up to 1024 bytes, tables up to 6 deep and 256 keys) to one member. `opts.reliable` (default `true`): reliable messages up to 8 KB, unreliable ones up to 900 bytes. Returns `true`, or `false, reason`: `"size"`, `"rate"` (over 20 messages or 8 KB a second to that member), `"no-peer"` (the member has not opened this channel), `"no-session"` (no game connection to the member yet), `"offline"`, `"closed"`. |
| `ch:broadcast(tbl[, opts])` | Sends to every member that passes the same checks; returns how many. |
| `ch:on(fn)` | `fn(from, tbl, meta)` for each message; `meta = {reliable, at}` with `at` the receive time in `wum.peer.now()` ms. |
| `ch:onPeer(fn)` | `fn(id, up)` when a member becomes reachable on this channel (`true`) or stops being reachable (`false`). |
| `ch:peers()` | The SteamIDs reachable now. |
| `ch:clock(id)` | `{offset, rtt, samples}` for a member (`offset` = their clock minus mine, in ms), or `nil` before the first sample. |
| `ch:setPresence(tbl)` | Publishes a small table (at most 256 bytes as JSON) as this machine's lobby member data for the mod, visible to every member even before any connection exists; written at most every 2 s. |
| `ch:presence(id)` | Another member's presence table for this mod, or `nil`. |
| `ch:close()` | Closes the channel (also on unload and reload). |

Everything is reset when the lobby is left, when it changes hands and when a new lobby is joined.
`[PeerBus] Enabled=0` turns the channel off (`wum.peer.open` then returns `nil, "unavailable"`).

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

C++ modules add their own contributors with `melange::wormsign::AddContributor` (`melange/wormsign.h`). A contributor that takes over 20 µs at the 95th percentile over 500 ticks is logged once as slow and keeps being hashed every tick (a contributor hashed on some ticks only would make the mods hash depend on the machine's speed; see [wormsign.md](wormsign.md), "For modules").

## Extending the API from C++

C++ modules can add namespaces with `melange::lua::AddLibrary("name", open)` (`melange/lua.h`). They appear as `wum.<name>` in
environments created afterwards. `melange::lua::CurrentMod()` tells a library function which mod is calling it.
