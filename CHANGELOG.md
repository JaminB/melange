# Changelog

## 0.11.0

- New: `wum.sim.worm(slot)` for sim scripts: `:pos()`, `:vel()`, `:alive()`, `:active()`, `:team()`, `:health()`,
  `:state()`, `:yaw()`, `:weapon()` and `:name()` read the worm's data container (the simulation's own state, the
  bytes Wormsign hashes), and `:anchor(on)` pins the worm where it stands for the rest of the match: it still takes
  turns, turns, aims and fires, but cannot walk, jump, backflip, rope, jetpack, parachute or teleport, and explosions
  damage it without moving it. It drowns like any other worm when the water rises over it, and the pin is released as
  soon as it dies or starts drowning. `:anchored()` and `:pin()` query it. Also `wum.sim.worms()` (the slots with a
  container) and `wum.sim.land()` (the land bounds). The new `WormAnchor` module holds the three engine hooks
  (`WXWormLogicEntity::Update`, `ChangeState` and the worm's input handler, build #1077) and reuses the engine's own
  nailed-worm flags; the anchor set is the Wormsign contributor `melange.wormanchor`. Sim-script calls only, so every
  machine anchors at the same tick.
- New: `wum.sim.holdTurn(ticks)`, `wum.sim.releaseTurn()` and `wum.sim.turnHold()` for sim mods: a deterministic hold
  of the next turn start, 1 to 3000 ticks (60 s). The game's `StartTurn` is deferred on every machine from the sim's
  own tick, the round clock is paused with the engine's `GameLogic.RoundTime.Pause`/`Resume` meanwhile, and the sim
  events `sim.turnHold`/`sim.turnHoldEnd`, the client events `melange.turnHold`/`melange.turnHoldEnd` and
  `melange::sim::GetTurnHold()` report it (`SimTurnHold` module; `docs/developer-guide.md`, "The turn-start hold").
  Online, `SimTurnHoldNet` repeats the turn-end `TimeSync(-1)` right before the deferred turn start runs, so the
  spectators run to their own deferred turn start instead of pausing behind the old active player's clock.
- New: `wum.render.cameraOverride(spec | nil)` for client mods: draws the frame from a camera of the mod's choosing
  (`pos`, `target`, optional `up`, `fov` and a refresh lifetime `frames`), one mod at a time. It is render-only: the
  new `MirageCamera` module swaps the render XCamera for the duration of `XOpenGLRenderManager::Render` and puts it
  back, so the logical camera Wormsign checks is never written; culling, every pass, post-FX, sprites, meshes, the
  HUD and `wum.render.worldToScreen` all see the override. `wum.render.camera()` gains `fov` and `overridden`, and
  `wum.render.gameCamera()` returns the game's own camera underneath, to blend back into it. C++:
  `melange::render::SetCameraOverride`/`ClearCameraOverride`/`CameraOverridden`/`GetGameCamera`.
- New: `wum.game.match()`: the round and turn clocks (`roundMs`, `roundMsLeft`, `turnMs`, `turnMsLeft`),
  `waterLevel`, `suddenDeath`, `turnsStarted` and the land bounds `landMin`/`landMax` (also in `gamestate::Match`).
- New: `wum.draw.mesh` for world draws, in a vertex-array form (`verts`, `idx`, `tex`, `model`, `opts`) and a handle
  form over meshes kept with `wum.draw.newMesh`/`wum.draw.freeMesh`: 8 floats per vertex (or 12 with `"pntc"` vertex
  colours), a model matrix or `{pos, yaw, scale}`, optional CPU lighting, alpha or additive blending, culling and
  x-ray, depth-tested against the scene.

## 0.10.0

- New: texture tags. A client-only mod can declare `"graphics": {"textureTags": "<file>.json"}` in `spice.json`: a
  table of up to 8 numbers per texture, keyed by a 64-bit digest of the texture's base level (scheme
  `gl-level0-sha256-64`, documented in `docs/spice.md`; `tools/tag_digest.py` is the reference implementation and
  computes the keys from the game's XOM files or PNGs). A GLSL replacement that declares `uniform vec4 mg_texTag[2]`
  gets the row of the texture bound on its `texture0` unit before each draw, all zeros when the texture has no tag.
  The new `MirageTexTags` module hashes the engine's texture uploads, follows deletes and name reuse and the texture
  units' bindings, and reads a texture back once if it never saw it uploaded. Tables of several mods merge, the first
  declaring mod winning a digest. Nothing is hooked without a table. The `tags.stats` verb logs how many textures and
  draws were tagged and untagged.
- New: shader textures. A GLSL replacement can sample a mod's own textures. `wum.shaders.setTexture(file, entry,
  sampler, tex)` binds a texture from the mod's `wum.draw.texture` to a sampler of the mod's own replacement (`entry`
  is a glob, `nil` unsets), and `wum.draw.texture(rel, {wrap = "repeat", filter = "linear"|"nearest", layers = n})`
  loads a PNG for it: with `layers`, a vertical strip of `n` tiles becomes one texture array (`sampler2DArray`,
  `#version 130`) with a full mip chain per layer. Linear textures are trilinear and follow `[MirageTextures]`
  anisotropy. Detect the feature with `if wum.shaders.setTexture then`.
- Fixed: a replacement's sampler with no Cg parameter now gets its own texture unit (8 to 13) and reads a neutral 1x1
  texture until one is set. Before, it silently read unit 0, the model's own texture.
- Fixed: texture tags follow `glBindMultiTextureEXT`, the call the Cg runtime uses to bind a draw's texture to a
  unit other than 0 (the landscape's tiles are bound to unit 1 that way). Before, those draws kept a stale tag.
- The GL hub has `hub::RealCaller`, so two interposers of the same GL function (texture clarity and texture tags on
  `glTexImage2D`) both see the engine's call site.

## 0.9.1

- Fixed the crash, or the "Runtime Error! ... terminate it in an unusual way" freeze, at the end of a match. The cause
  was a `std::bad_alloc` thrown by Melange when the address space of `WormsMayhem.exe` ran out: it is not
  large-address-aware, so it has 2 GB, and on one machine at 1920x1080 the game used 1282 MB at the main menu, 1319 MB
  with Melange, 1381 MB with a set of mods and about 1560 MB in a match, with the largest free block only about 186 MB
  at the menu. The exception escaped Melange's frame hook into the game, which has no handler for it, or reached a
  `noexcept` helper and ended the process through `abort()`. Melange now catches its own allocation failures at the
  frame and event dispatch and in the sim bridge (subscribing, timers, the mod log, level spawns), logs them as `out of
  memory` with the address-space state (the first three, then every thousandth), and carries on.
- The log now shows the address space (free, largest free block, used, large-address-aware or not) at startup and in
  every `CRASH` report, and warns once when the largest free block drops below 128 MB. A heavy mod set at 1080p or above
  can still exhaust the address space inside the game itself, where Melange cannot catch it.
- A crash that ended the process through `abort()`, a pure virtual call, an invalid-parameter report or `std::terminate`
  left no `CRASH` line, no minidump and no stack. Melange now logs and dumps every such path like any other crash.
  `SelfTestCrashKind` 2, 3, 4 and 5 (an allocation failure that is caught) try them on purpose.
- The background worker threads (recording close and library indexing, mod handshake hashing, the Store, the update
  check, the trace panel, the detector's bundle export and the launcher's workers) now catch and log an error instead of
  ending the game.
- New opt-in 4 GB mode (large-address-aware) for `WormsMayhem.exe`, the way out of the address-space limit described
  above. Turn it on in `Melange.exe` under *Settings › Memory* ("Use up to 4 GB of memory"), or set `[Game]
  LargeAddressAware=1` in `Melange.ini`: `Melange.exe` then sets one bit in the game's PE header (on a checked copy that
  is swapped in, only for the supported build #1077, and not while the game runs) at every launch, and clears it again
  when the setting is turned off or on Restore vanilla. It is off by default and only changes this PC. It does not
  affect multiplayer: the game-files hash that Melange shows other players ignores that bit, so patched and unpatched
  players publish the same hash (a peer on an older Melange may show the amber "Game files differ" lobby banner, and
  nothing else happens). Steam's "Verify files" restores the stock exe; the next launch from `Melange.exe` applies it
  again, and a game started straight from Steam keeps whatever state the file was left in (the log warns when
  `Melange.ini` asks for it and the exe does not have it). `Melange.log` and the system report now show whether the exe
  is large-address-aware, and the "address space low" warning says 2 GB or 4 GB accordingly.

## 0.9.0

- Fixed the mouse camera not responding (only the arrow keys worked) when Raw Input stopped arriving, for example
  after another program replaced the mouse registration. Melange now falls back to the game's own mouse input and
  registers Raw Input again every few seconds. On older versions, set `[Controls] SmoothMouse=0`.
- New `wum.audio` Lua API (client VM): `ready()`, `load(rel)`, `play(handle, {volume, pitch, pos, loop})`, `stop(voice)`
  and `stopAll()`. A mod plays its own WAV files (PCM 16-bit, mono or stereo, 8-48 kHz, up to 2 MiB each, 64 sounds
  and 16 MiB per mod) through a new `Audio` module (`[Audio] Enabled`, `Volume`, `MaxVoices`, `Near`, `Far`) built on
  XAudio2, with distance attenuation from the render camera for positional sounds. Sound is local to the player:
  nothing reaches the simulation, the wire or the content handshake. Not yet tried in the game by the author.
- `wum.sim.weapon(name):set` now accepts `u32` and `u16` fields (`NumBomblets`, `NumberOfBullets`, `LaunchDelay`, ...).
  A `u32` above 16777216 is refused, because the sim's float numbers cannot name every integer above it.
- New `weaponText` manifest field for content mods: new panel names and help text for vanilla weapons, applied through
  the same hooks as weapon clones, only inside a match that allows sim mods, and part of the content identity so
  peers with different renames do not play together. A weapon renamed by two mods refuses the later one in load order.
- New `weaponIcons` manifest field for content mods: a replacement panel icon (PNG) and/or HUD icon (`<modId>.*.tga`)
  for vanilla weapons. The panel icon is written over the weapon's own slot in the panel's icon sheet and the HUD file
  is swapped when the game loads the weapon's vanilla HUD icon, both only inside a match that allows sim mods; the
  vanilla icons come back when it ends. Part of the content identity; a weapon claimed by two mods refuses the later
  one in load order. Not yet tried in the game by the author.
- New `meshes` manifest field for content mods: mesh banks (`.xom`, built with `xomtool convert --bundle`) under the
  assets root, loaded at the main menu through the game's own graphical resource manager (new `Meshes` module,
  `[Meshes] Enabled`) so `wum.sim.weapon(name):set("WeaponGraphicsResourceID", "<modId>.<Name>")` or a clone's `set`
  can use a mod's own 3D model. Every resource name in a bank must start with `<modId>.`; a bank that fails to load
  is logged and skipped. Loading a bank and holding its mesh was tried in the game by the author; the automatic load
  and projectile meshes were not. A mod may list up to 64 banks, but the engine has only 44 free mesh sections for all
  mods together: banks past that in load order are skipped, and the log says how many before loading starts.
- New `vehicleMeshes` manifest field for content mods with a `meshes` bank: the mesh the Airstrike (`BomberHelicopter`)
  and Super Airstrike (`SuperAirstrike`) helicopters are drawn with, `"<modId>.<Name>"`. The game's two bomber graphic
  entities read their mesh name from a built-in string, which is swapped for the match and put back at its end; nothing
  in the game's code is patched. Only inside a match that allows sim mods, part of the content identity, and a vehicle
  set by two mods refuses the later one in load order. The "Bomber" plane mesh in the game's files is never drawn
  and is refused. Found in the game's code and unit-tested; not yet tried in the game by the author.

## 0.8.0

- New `wum.input` Lua API: `groups()`, `binding(message)`, `setOptions{...}` and `options()`. Mods can set the
  camera and aim Y-invert ("game", "standard" or "inverted"), apply the camera invert in the Blimp view (which the game
  skips), and scale camera and aim mouse sensitivity (0.25 to 3.0). Options are cleared when the mod unloads. They only
  change the numbers the game puts into its own mouse messages on the sending side, so they cannot affect lockstep
  netplay or Wormsign recordings.
- New `Controls` module (`[Controls] Enabled`, `SmoothMouse`, `Probe`; build #1077): `SmoothMouse` (on by default)
  reads the mouse with Raw Input and feeds the game's own mouse code, removing the aiming and camera jitter caused by
  the per-frame cursor re-centre.
- `wum.ui.hotkey` now also accepts a bare function key (`"F1"` to `"F12"`) without Ctrl, Shift or Alt.
