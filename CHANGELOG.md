# Changelog

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
