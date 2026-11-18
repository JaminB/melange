# Grave Spade RE: carving terrain from the sim

Reverse-engineering notes for a deterministic `wum.sim.carve` (Melange 0.16.0, the Kindjal Grave Spade). Build #1077
(`WormsMayhem.exe`, 5 713 408 bytes). Every address is a VA in that build. This page covers the land side only:
how the game removes voxels, why the result is the same on every peer, what it costs, what debris comes with it,
and the API it suggests. No code was changed for it.

How it was found: static analysis only (string and message-ID cross-references, then x86 disassembly with
capstone). Nothing was probed in a running game: the shared test instances and this worktree were in use by other
steps of the workflow at the time. The open items that need a live run are listed in
[Still to measure in the game](#still-to-measure-in-the-game), with the probe that settles each one.

## Summary

- **The game has no drill, blowtorch or digging weapon.** The container names in the exe are the 26 weapons and 12
  utilities Melange already knows. Every hole in the land is made by the landscape's **Explosion** handler. The
  only other land edit is `Land.ClearVoxel` (one voxel, addressed by globals a collision test leaves behind) and the
  Girder, which adds land.
- **The carve primitive is `LandscapeLogicEntity::OnExplosion` (0x473530).** It is a thiscall on the one
  landscape (`*(0x955638)`) taking an `ExplosionMessage*`, and it reads two fields only: the damage epicentre
  (+0x08) and the land damage radius (+0x38). It removes every voxel whose centre is inside that sphere from every
  destructible voxel frame and from the heightmap. Then it spawns the vanilla debris and posts `Land.NewShape`, so
  resting worms, mines, crates and drums re-check their footing.
- **Melange can call it directly, without putting an Explosion on the bus.** Worms, mines, oil drums, crates, the
  camera (the post-explosion FlyCam hold, the cause of the net time-sync aborts), the AI and the scripts never see
  an Explosion, so nothing but the land changes. The vanilla payload path posts the message instead (0x57f140 ->
  0x518ce0 -> 0x6910e4), and that is why `Explosion.Construct` and clone `explode()` are the wrong tools here.
- **Water is immune by construction.** The sea is a plane, not voxels. The carve never touches it. A hole dug
  below the waterline is open sea to a worm, which drowns there as usual. That is what the owner asked for.
- **It is deterministic.** Land state is never sent between peers. Every machine replays the same explosions from
  the same inputs. The carve is x87 arithmetic over the land's own stored state (frame matrices, voxel bits), and
  its only random draws are for cosmetic debris, made from the game's second RNG (`0x68c07b`/`0x68c0aa`), which
  Wormsign already treats as non-sim (`rng2`, never hashed). Called from the sim at the same tick on every peer,
  it gives the same land everywhere.
- **The cost is small. The holes are voxel-sized.** A carve walks only the voxels in the sphere's box, in the
  frames whose AABB the sphere touches. A Grave Spade scoop is about 1 to 8 voxels. Map voxels are 1.2 to 2.0 map
  units (24 to 40 world units) a side on the game's maps (Erg: scale 0.5 to 4). So a scoop removes whole cubes, not
  a smooth bite. A worm-sized cave is roughly 6 to 20 voxels, which takes 10 to 30 scoops: well inside a 30 s turn
  at a scoop every 0.3 to 0.5 s.

## How the game removes land

### The landscape's message handler

`LandscapeLogicEntity` (vtable `0x81c810`, the instance at `*(uint32*)0x955638`; Melange's `gamestate.cpp`
already checks both) handles its messages in **0x478780**. It compares the message id at `msg+4` with each message
id object in turn (`0x68bcad`):

| Message | ID object | Handler |
|---|---|---|
| `Land.CheckVoxel` | 0x9556a8 | 0x473350: is the voxel the last sweep hit solid? |
| `Land.ClearVoxel` | 0x9556b8 | 0x4733b0: clear the voxel in globals `0x955ad4` (frame) / `0x955ad0` (voxel index) |
| `Explosion` | 0x9556c0 | **0x473530**: the sphere carve |
| `Land.ClearCoded` | 0x9556f8 | 0x475170 |
| `Land.GetNormal`, `Land.SpawnPiece`, `Land.GetLandRemaining`, ... | 0x9556d8, 0x9556e8, 0x9556f0 | |

Other ids used here: `Land.NewShape` 0x953334 (the landscape's own copy; other entities have their own copies of the
same id), and `Explosion` 0x95c364 (the copy the ExplosionMessage constructor uses). The ids come from the
registration stubs at 0x7dd000 to 0x7ed000 (`push "<name>"; mov ecx, <id object>; call 0x68bb9f`).

### `OnExplosion` (0x473530), step by step

`thiscall(LandscapeLogicEntity* this, ExplosionMessage* m)`, `ret 4`. Prologue
`6a ff 68 ab 45 7d 00 64 a1 00 00 00`.

1. **`Land.Indestructable`** (bool data, handle at `0x95574c`): if it is set, the handler does nothing (the level
   or scheme makes the land indestructible).
2. **Detail objects**: for each entry of the vector at `this+0xb0..+0xb4`, it calls `0x5cc960(m)`. An entry that
   answers true is removed, which is how props in the blast go.
3. **`Land.Changed`** (bool data, handle at `0x95573c`) is set to 0 (`0x69e100`).
4. **Epicentre and radius**: `0x518ee0(m, &pos)` copies `m+0x08..+0x10`, and `0x527980(m)` returns `m+0x38`
   (`fld [ecx+0x38]`).
5. **Broadphase**: `0x473060(this, &pos, radius)` tests the sphere against each frame's AABB (an array of min/max
   pairs at `this+0x98..+0x9c`). It writes one bit per frame into the mask at `this+0xc8` (0x80 bytes, the mask
   `wum.game.landRay` saves and restores).
6. **Frames**: for each frame entity in the global vector `0x955740..0x955744` whose mask bit is set and whose
   flags byte `+0x16` has bit `0x08`, it calls **`0x46d290(frame, m)`**. That is the per-frame voxel carve
   described below.
7. **Heightmap**: if `*(0x952ad4)` (the `HeightmapLogicEntity`) exists, it calls **`0x463c90(heightmap, m)`**, which
   lowers the height field inside the sphere. The heightmap is a height field, so it can be dug into a pit but can
   never hold a cave.
8. **If `Land.Changed` became 1**:
   - it spawns the `weapons/Debris` emitter (the resource is cached at `this+0x3178`; vtable `+0x40` sets the
     position and `+0x38` fires it with 1.0);
   - it allocates a `LandNewShapeMessage` (0x18 bytes from the message pool `0x96d14c` / `0x691705`, ctor
     **0x46fb00**: vtable `0x81c444`, id, `+0x08` radius, `+0x0c..+0x14` position) and posts it with
     **0x6910e4**, the bus post, which delivers it at the next dispatch.

### The per-frame carve (0x46d290)

- **PERM frames.** A frame with flag `+0x16 & 0x10` is never carved. A hit on it only sets
  `Explosion.HitIndestructible` (through `0x46c1b0` and `0x50ba00`). These are Erg's PERM frames.
- **Local space.** The epicentre goes into the frame's local voxel space. The inverse of the frame matrix is at
  frame data `+0x60` (`0x455d30`), and the per-axis scale is at `+0x12c/+0x130/+0x134`. The radius is divided per
  axis by that scale, so a scaled block is carved in its own voxels.
- **Voxel grid.** The frame data (`*frame`, an XContainer) holds the grid dimensions as bytes at `+0x1c0/+0x1c1`
  (with `+0x1c3..+0x1c5`). The solid bits are a bitset under `+0x1b0/+0x1b8`, cleared with `0x43e0d0`. The grid is
  cut into blocks of 7 voxels per axis for meshing (the divides by 7 in `0x46c680`).
- **Per removed voxel**, the carve does three things:
  1. decrements that frame's voxel count, a u16 at `0x955570 + 2*frameId` (frameId = frame `+0x14`);
  2. marks the neighbouring mesh blocks dirty in the 5x5x... table at `0x952fb1..0x952fe4`, so the
     `LandscapeGraphicEntity` re-meshes only those blocks;
  3. spawns a material debris piece (next section).
- **`Land.Changed` = 1** when any voxel went.

`0x46c680(frame, voxelIndex)` is the single-voxel version, used by `Land.ClearVoxel`. It refuses PERM frames,
clears one bit, decrements the count and marks the block dirty. `0x46c630(frame)` then refreshes the frame's
attached details. That pair is a possible later "exact voxel" mode (see the API section). The sphere carve is the
simple, proven path.

### Who reacts to `Land.NewShape`

The subscribers, found by the id copies' cross-references:

| Subscriber | Function | What it does |
|---|---|---|
| AI scene graph | 0x4b2d10 | `AISceneGraphService.cpp`; logs `MsgLandNewShape rad=%f, pos=`: the AI's route graph is updated |
| Payloads | 0x570920, 0x577f40 | resting projectiles and mines re-check their contact |
| Crates, oil drums, mines, mine factory | 0x5b37c0, 0x5bc070, 0x5be2e0, 0x5c9bd0, 0x5d2200 | objects re-check their footing |
| Worms | 0x5b07c0 (`WXWormLogicEntity`) | only a **passive** worm (worm container `+0xf0 == 6`, not the active one) whose support frame (`+0x120/+0x122`) is gone or is the heightmap "goes ballistic" (`0x5acb80(7, 0)`), so it falls if the ground under it was removed |

The active worm checks its ground every update on its own, so a worm digging under itself just falls into its hole.
`Land.NewShape` has **no radius test** on the worm side. Any carve makes every passive worm whose support frame
vanished fall, which is the right behaviour.

## Debris and dust (vanilla)

A carve already makes the vanilla land debris, on every machine, with nothing extra to do:

- **Per-voxel material debris.** For each removed voxel, the carve looks up the particle resource of the voxel's
  material: the material table `*(0x953344)`, the entry's `+0xc3c`, through `0x69f7e0`. It then spawns a piece
  through the land particle manager `*(0x952bb4)`, `0x465ea0`. Dirt throws dirt and a castle wall throws stone,
  because the material picks the effect. The scatter is drawn from the **second** RNG (`0x68c07b` /
  `0x68c0aa`), which is cosmetic: Wormsign records it as `rng2` and never hashes it.
- **The `weapons/Debris` burst**, once per carve that changed anything, at the epicentre.

Neither can be switched off without a patch, and the spade doesn't need that. Whatever the scoop should add (dirt
clods along the swing, a dust puff, sparks on stone, a thrown shovel-load) is Kindjal client sprites. The client
learns about a carve in one of two ways:

- **Now:** the sim sends it, as `wum.sim.sendString("Kindjal.Dig", "x,y,z,r,removed")`, the same way Kindjal's
  `Kindjal.Burst` works today.
- **Proposed for 0.16.0:** a bus decoder for `LandNewShapeMessage` (vtable `0x81c444`: `radius` at +0x08, `pos`
  at +0x0c). Then `wum.events.on("Land.NewShape", fn)` gives the client every carve, with its position and radius,
  including vanilla ones. It is a two-line addition to `src/core/bus_decoders.cpp`, where the class is already
  listed without a decoder.

## Why it is the same on every peer

- **Land is derived, never sent.** The only things that cross the wire are the active player's inputs (the input
  sender at `0x505990`). Each peer simulates the explosions, and so the land, from them. There is no land-sync
  message to keep in step.
- **Inputs to the carve.** The carve reads the landscape's own stored state: frame AABBs, matrices, scales and
  voxel bits. All of these are themselves products of the same deterministic history. The only other inputs are the
  epicentre and radius, which come from the sim script. Nothing is read from the camera, the renderer, LOD or the
  frame rate. The gates are two data bools, `Land.Indestructable` and `Land.Changed`, both of which are logic
  state.
- **Floating point.** The carve is x87 code, the same code the vanilla explosions run. It gives the same bits if
  the FPU control word is the same on every peer. Wormsign already hashes `fpucw` per tick, and the sim's Lua runs
  at the engine's control word. The carve must run on the main thread inside the sim tick, like every other
  `wum.sim` call. It must never run from the client VM, a render callback or another thread.
- **Random draws.** Only the debris uses the second RNG, which is cosmetic. The logic RNG (`0x68c015`/`0x68c024`)
  is not drawn by the carve path. The live check for this is listed below.
- **Ordering.** A direct call runs at the point the Lua calls it, inside the sim tick, which is the same point on
  every peer. The `Land.NewShape` it posts is delivered at the next bus dispatch, also the same everywhere.
- **Proof in Wormsign.** The engine hash does not cover the land today (it covers time, RNG, turn, worms, tasks,
  projectiles and teams). A carve that went differently would only show up later, through worms. So the carve
  should add a contributor, `melange.land`. Each carve folds its tick, the raw float bits of its position and
  radius, the removed count, and the 16-bit voxel counts at `0x955570` of the frames in the broadphase mask, into a
  running digest. The contributor hashes that digest every tick: 8 bytes, well under the 5 us budget. A difference
  in the land then becomes a desync report that names `melange.land`, at the tick of the carve.

Rejected routes:

- **`Explosion.Construct` through the match VM's `SetData`/`SendMessage`** (what `level.test.explode` does). It
  needs a named level knot, not an x, y, z. It posts a real Explosion, which the worms, mines, drums, crates, the
  camera (the FlyCam hold that broke turn-end time sync, see `docs/multiplayer-fixes.md`) and the AI all react to.
  Its constructor also writes the global `Explosion.*` data values that level scripts read.
- **A clone's `wum.sim.weapons.explode(dx, dy, dz)`.** It needs a projectile clone in flight (Kindjal's 3 clone
  cells are taken, and no melee base can be cloned), and it runs inside an explosion with that explosion's damage.
- **`Land.ClearVoxel` as a message.** It is addressed through the globals a collision test leaves behind, not
  through a position. The function under it (0x46c680) is usable directly, see "exact voxel mode" below.

## Cost

From the code (to be confirmed in the game, below):

- **Logic.** The broadphase is a loop over the frame AABBs: a level has at most a few hundred frames, and Erg's
  cell budget is 65 535 cells. The carve then walks the voxels inside the sphere's box, in the touched frames only.
  For a spade scoop of radius 0.6 to 1.0 voxel, that is at most 3^3 to 5^3 voxel tests per frame, plus one debris
  spawn and one dirty mark per removed voxel. That is microseconds, not milliseconds.
- **Re-meshing** is done by the landscape graphic side, for the dirty 7^3 blocks only (`CombineLandscapeAction`).
  It is the same work a bazooka hole costs. Vanilla already does 5 to 10 carves within a second (cluster bombs, an
  air strike, the Holy Hand Grenade's cluster) without a hitch.
- **`Land.NewShape` consumers.** The AI scene graph update is the heaviest listener. It only matters with CPU
  teams in the match.

The proposed limits keep the spade far below the vanilla bursts: **at most 4 carves a tick** and **at most 900 a
turn** (`[Land] CarvesPerTick`, `CarvesPerTurn`). A held dig at one scoop every 15 to 25 ticks is 60 to 100 carves
in a 30 s turn. A refused carve answers `nil, "budget"`, so a mod cannot hitch or flood the game.

## Proposed API (Melange 0.16.0)

### Lua (sim VM, Lua 5.0)

```lua
-- Remove every destructible voxel whose centre is within `radius` world units of (x, y, z), from every
-- voxel frame and the heightmap. Water is never touched. PERM frames and an indestructible level are never
-- carved. No damage, no impulse, no Explosion message; the vanilla land debris plays and Land.NewShape is
-- posted, so resting worms and objects re-check their footing.
-- radius: 1..200 world units. Returns the number of voxels removed (0 when nothing was solid there),
-- or nil, reason: "not in a match", "bad arguments", "indestructible", "budget", "unavailable".
local removed = wum.sim.carve(x, y, z, radius)

-- A dig stroke: spheres of `radius` from (x0,y0,z0) to (x1,y1,z1), one every `step` world units
-- (default radius/2, at least radius/4; at most 16 spheres). Counts as that many carves for the budget.
-- Returns the total removed, or nil, reason.
local removed = wum.sim.carveLine(x0, y0, z0, x1, y1, z1, radius [, step])
```

- Content mods only. The call is refused (`"denied"`) where sim mods are not allowed, as weapon clones are.
- Numbers are taken as Lua doubles and stored as float32 before use, so every peer passes identical bits.
- `removed` is deterministic. It is the sum of the per-frame voxel-count drops (`0x955570`), plus 1 if the
  heightmap changed. Mods may branch on it, for example "the scoop hit rock, so play the clang".
- Both calls run synchronously. The land is gone when they return, and a `wum.game.landRay` on the client sees it
  from the next frame.

Kindjal's use, for illustration: tap = one `carve` at the blade, at worm centre + aim x 22, radius about 0.7 voxel.
Hold = a `carve` every 20 ticks, stepping the blade forward by the removed depth. The sim then sends `Kindjal.Dig`
to the client for the dust and the hands.

### C++ plan

New module `src/gameplay/land_carve.{h,cpp}`. The Lua side goes in `src/lua/sim/sim_land.cpp`, registered through
`simbridge::AddSimFunction`, as `sim_worms.cpp` is.

```cpp
namespace melange::land {
struct CarveResult { int removed; bool changed; const char* refused; };
CarveResult Carve(const float pos[3], float radius);   // main thread, inside the sim tick only
}
```

- **Addresses** (each one checked with `mem::Expect` at install, and the feature refused otherwise):

  | Address | What |
  |---|---|
  | `0x473530` | `OnExplosion`, prologue `6a ff 68 ab 45 7d 00 64 a1 00 00 00` |
  | `0x46d290` | frame carve, `6a ff 68 0b 82 7c 00 64 a1 00 00 00` |
  | `0x463c90` | heightmap carve, `81 ec b4 00 00 00 57 8d 84 24 84 00` |
  | `0x473060` | broadphase, `83 ec 1c 53 55 8b a9 9c 00 00 00 56` |
  | `0x518ee0` / `0x527980` | the epicentre and radius getters |
  | `0x955638` | the landscape instance (vtable `0x81c810`) |
  | `0x955740` | the frame vector |
  | `0x955570` | the voxel counts |
  | `0x95574c` | the `Land.Indestructable` handle |
  | `0x95573c` | the `Land.Changed` handle |
  | `0x854184` | the ExplosionMessage vtable |

- **The message.** A zeroed 0x4c-byte `ExplosionMessage` on the stack:

  | Offset | Value |
  |---|---|
  | +0x00 | vtable `0x854184` |
  | +0x04 | the `Explosion` id (`bus::IdOf("Explosion")`) |
  | +0x08 | damage epicentre = pos |
  | +0x14 | impulse epicentre = pos |
  | +0x20 | dir = 0 |
  | +0x2c | worm damage 0 |
  | +0x30 | impulse 0 |
  | +0x34 | worm radius 0 |
  | +0x38 | land radius = radius |
  | +0x3c | impulse radius 0 |
  | +0x40..+0x48 | 0 |

  Do **not** call the constructor `0x518ce0`. It writes the global `Explosion.*` data values and applies
  DoubleDamage. The handler keeps no pointer to the message, so a stack message is fine. Wrap the call in
  `__try`/SEH, as `gamestate::LandRay` does.
- **The call sequence:**
  1. Refuse if `Land.Indestructable` is set (`"indestructible"`).
  2. Snapshot the voxel counts of the frames the broadphase will touch, and the `Explosion.HitIndestructible`
     value.
  3. Call `OnExplosion(landscape, &msg)`.
  4. Restore `HitIndestructible`. A spade hitting a PERM frame must not leak a "hit indestructible" into the next
     real explosion's scripts.
  5. Diff the counts to get `removed`.
  6. Fold the carve into the `melange.land` digest.
- **Budget**: the `[Land]` ini (`CarvesPerTick` = 4, `CarvesPerTurn` = 900, `Log` = 0), reset at
  `sim.turnStarted`. With `Log=1`, each carve logs `[land] carve (x,y,z) r=.. removed=.. us=..`, timed with QPC.
- **Wormsign**: `AddContributor("melange.land", ...)`, version 1. A replay re-runs the sim Lua, so it carves again
  by itself, and nothing extra goes into the recording.
- **Client**: add the `LandNewShapeMessage` decoder (`radius` +0x08, `pos` +0x0c) to `bus_decoders.cpp`.
- **Docs**: `docs/lua-api.md` (the sim table) and `docs/developer-guide.md` (a "Carving the land" section with
  the voxel-size caveat).

### Later, if needed

- **Exact voxel mode**, `wum.sim.carveVoxel(x,y,z)`: find the frame whose AABB holds the point, transform the point
  with that frame's inverse matrix and scale, and call `0x46c680(frame, index)` then `0x46c630(frame)`. One whole
  voxel goes every time, whatever the radius maths, but Melange then has to post `Land.NewShape` itself. Only worth
  it if the sphere scoops feel wrong in play.
- **A sim land query**, `wum.sim.landRay(...)`: the engine sweep `0x466ae0` that `wum.game.landRay` already wraps
  (it saves and restores the mask and ray cache). It is deterministic, because payloads use it in the sim. The
  spade's sim could then stop the dig at a wall or tell air from rock before carving. Optional, since `carve`'s
  `removed` already answers "was anything there".
- **Material of the removed voxels**, so the client can choose sparks or dirt: read the voxel's material before
  clearing it (the material index the debris lookup uses). Not needed while the vanilla per-material debris plays.

## Still to measure in the game

To do once `Carve` exists, with a private build (`build.ps1 -Out gsp`, testenv B, the runbook's traps). These are
the claims above that came from reading the code only:

1. **Cost.** With `[Land] Log=1`, hold-dig for 30 s on a scale-1 and a scale-2 map. Read the per-carve `us` and
   the `gltrace` frame CPU and GPU time for the 3 frames after each carve. Expect under 0.2 ms logic per carve and
   no frame spike beyond a bazooka's.
2. **RNG.** Run `wormsign.stats` around a single carve. The logic RNG's draw count must not change, only `rng2`.
3. **Footing.** A passive worm standing on the carved voxel falls. The digging worm, and worms nearby on intact
   ground, do not move.
4. **PERM, indestructible and water.** A carve on a PERM castle frame removes 0. A level with `Land.Indestructable`
   answers `"indestructible"`. A carve below the waterline opens a hole the worm drowns in, with no change to the
   water.
5. **The heightmap**, on the base surround of a game map: the pit forms, `removed` counts it, and passive worms on
   it fall.
6. **Net.** On LocalNet A+B, dig on A's turn (with B as spectator) and on B's turn. Both logs must show equal
   `melange.land` digests, and there must be no `Desync at tick` and no time-sync abort.
