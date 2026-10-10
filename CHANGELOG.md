# Changelog

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
