# Grave Spade RE: killing, pinning and posing a worm

These are research notes for the Kindjal Grave Spade execution (Melange 0.16.0). They cover three questions:

- (a) a deterministic sim-side instant kill;
- (b) a prone victim with the attacker mounted on it;
- (c) a client-side pose or bone override: hands that follow a held object, a bent body, and a hidden head.

The notes propose an API and a hook plan. Nothing here is implemented yet.

Everything refers to build #1077 (`WormsMayhem.exe`, Steam/GOG). Decompiles are `re/out/decomp/decomp_<chunk>.c` in the private research checkout. Message-object names were resolved by scanning the static initialisers `push "<name>"; mov ecx, <obj>; call 0x68bb9f` (2082 objects).

Confidence follows [meshes.md](meshes.md):

| Tag | Meaning |
|---|---|
| **[V]** | Read from the decompile or disassembly, and consistent with the data files |
| **[I]** | Inferred from strong indirect evidence |
| **[P]** | A proposal not yet run in game |
| **[U]** | Unknown |

## Verdicts

| Question | Verdict | Route |
|---|---|---|
| (a) Instant kill from the sim | **Feasible today, and cleanly in 0.16.** | `Worm.KillNow` (an IntMessage carrying the slot) is a vanilla, script-only "die now" message the worm already subscribes to. 0.16 should wrap it as `worm:kill{...}`, with hooks that skip the death explosion and grave and shorten the throes. |
| (b) Prone victim, mounted attacker | **Feasible as a visual only. No engine prone state exists.** | The sim anchors both worms (`worm:anchor`, 0.11). "Prone" and "mounted" are client pose overrides of the rendered skeleton (c). Sim positions never change, so this cannot desync. |
| (c) Client pose or bone override | **Feasible: the GL palette patch is the primary route.** | Worms are GPU-skinned with a 34-bone world-space palette (`BlendMat[35]`) that the game uploads as ARB program-local registers. Melange already mirrors those registers and hooks `glDrawElements`. The work is: patch the palette per draw for one worm, restore it after, and optionally draw a second copy with another palette to get a severed head. Hats and the held weapon need extra handling (see [Attachments](#attachments)). |
| Fallback | Available | Vanilla clips by name through `XMeshInstance::PlayClip` `0x6a0730` (speed scalable), plus client meshes. See the [clip list](#route-c-fallback-retimed-vanilla-clips). |

## (a) Killing a worm

### How a worm dies [V]

Worm logic is `WXWormLogicEntity` (one per slot):

- the slot is the byte at `+0x30`;
- `HandleMessage` is `0x5b07c0`, and its second-level handler is `0x5adcf0`;
- the per-tick update is `0x5b1d60`, which is 20 ms per sim tick.

The worm's data is the `WormDataContainer` described in [worm_anchor.cpp](../src/gameplay/worm_anchor.cpp): health is a `u16` at `+0x11e`, state `+0xf0`, flags `+0xec`, active `+0x124`.

| Step | Function | What it does |
|---|---|---|
| Damage accrues | `AddDamage` `0x5ab7e0` (thiscall on the logic entity: `amount, type`) | Adds to a per-type pending array at container `+0x118` (types 0..7). Types 2, 3 and 4 are capped at 75, type 6 is uncapped and skips stats, type 7 is fire. It also sets data `DamagedWorm.Id` and `DamageTypeTaken`, updates the `Turn.*Damage` stats and posts `Worm.Damaged`. Callers are Explosion `0x5ae4f0`, `Damage.Impulse` `0x5ae320`, poison `0x5ac060`, Vapourize `0x5ac160`, and `0x5ac3e0`/`0x5ca1f0`. |
| Turn-end apply | `GameLogic.ApplyDamage` → `0x5abc50` | Sums the pending array. If the sum is at least the health, it sets flag 4 and health 0 (`0x5a70e0`, "Worm Waiting To Die") and posts `GameLogic.AddMeToDeathQueue`. Otherwise it subtracts. It also posts `GSS.CreateDamageGraphic` and `Worm.DamageComplete`. |
| Death queue | GameLogic → `Worm.TimeToDie` → `0x5adbf0` | Unless the worm is drowning (state 8), it posts `Worm.LandDeath(slot)`, runs `ChangeState(container, 7 = DeathThroes)` and queues anim id `0x13` ("Death" clip). `0x5a7190` ("Worm Dying") sets health 0, records stats, and posts `WormManager.SurrenderTeamById` if flag `0x100`. The throes timer at **logic entity `+0x128` = 3000 ms**. |
| Throes tick | `0x5aa080` (state 7, from Update `0x5b1fe1`) | The timer drops by 20 each tick. At 0 or below it sets active = 0 and runs the **death explosion** `0x5a9400`, which uses data `Worm.DeathWormDamageMagnitude`, `...ImpulseMagnitude`, `...WormDamageRadius`, `...LandDamageRadius` and `...ImpulseRadius` through `0x518ce0`, plus FX `WXP_Explosion_Small`. It then makes the **gravestone** `0x5a9310` (team grave style, at feet + 20 y) and posts `WXWormManager.UnspawnWorm(slot)` (handled by `WXWormManagerService` `0x5b5e70` → `0x5b4af0`). |
| Drowning | `0x5ad640` → state 8, `0x5aa130` | The same end through the drowning branch. |

### Script-reachable "die now" messages [V]

Message objects are named via the static-initialiser scan. Subscriptions are in the worm entity's init `0x5a5ae0`.

- **`Worm.KillNow`** (object `0x95fcd8`, subscribed at `0x5a5c3c`). Its handler is in `0x5adcf0`: `if (IntMessage.value == slot) { 0x5a70e0(container); 0x5adbf0(); }`. That means flag 4, health 0, then straight into the throes (the `Worm.TimeToDie` path) **without** the death queue. Nothing in the exe sends it, so it is a script-facing message. The engine's own `SendIntMessage` (`0x69676a`) builds exactly this `IntMessage` (`0x691951`) and Posts it (`0x6910e4`), and the only effective deny is `GameLogic.PauseGame` (framework-feasibility U12).
  - **So `wum.sim.sendInt("Worm.KillNow", slot)` already kills a worm on Melange 0.15**, at the same tick on every machine. Not yet run in game: **[I]**.
- **`Worm.Vapourize`** (subscribed at `0x5a5bc4`, handler `0x5ac160`). It is a 16-byte two-int message `{slot, 20}` built by `0x691985`; the only sender is `0x5885f0`. It runs ApplyDamage, AddDamage(health, 0), ApplyDamage, sets active = 0, posts UnspawnWorm and records stats `0x613080`. The worm vanishes at once: **no throes, no explosion, no grave.** There is no Lua binding for two-int messages, so this route is C++ only.
- **`Worm.TimeToDie`** is the death-queue message. On its own it skips `0x5a70e0` (health stays as it was), so it must not be sent alone.
- `Worm.DieQuietly` goes to `WXWormManagerService` (`0x5b5e70`) and acts on a global or the active worm, not on a slot. Not useful.
- **Do not write health alone.** `ApplyDamage` returns early when health is 0, and nothing then moves the worm into the throes, so it stays "alive" at 0 hp.

### Kill options

1. **K1, no core change (0.15):** `wum.sim.sendInt("Worm.KillNow", slot)`. The full vanilla death follows: 3 s death anim, small death explosion, grave, unspawn.
   - The explosion hits the adjacent attacker and the terrain. To suppress the damage without core work, `wum.sim.setData("Worm.DeathWormDamageMagnitude"/"ImpulseMagnitude"/"LandDamageRadius", 0)` before the kill and restore after the throes. That is deterministic, but the data ids still need a [U] check against `DataPrecheck`.
   - The `WXP_Explosion_Small` FX and an `Explosion` message still fire. Bloodsand treats an `Explosion` at a dying worm as `dieNow` and blows the corpse apart.
2. **K2, damage accrual:** call `AddDamage(health, 6)` through the logic entity at strike time. The engine kills the worm in the normal turn-end death queue, which is the most "vanilla" for stats.
   - Rejected for the spade: the death would run at turn end, which is **before** the holdTurn cinematic, so the victim would be a gravestone before the camera orbits. The exact order of the death queue against `GameLogic.Turn.Ended` is [U].
3. **K3, recommended for 0.16 [P]:** `worm:kill{explode=false, grave=true, throesMs=N}`. It posts `Worm.KillNow` from C++ (the `levels/factory.cpp` message-post pattern: alloc `0x691705`, `IntMessage` ctor `0x691951`, Post `0x6910e4`) and records per-slot kill options. Three hooks, active only while a record exists, read those options:
   - after `0x5adbf0` (thiscall, `ecx` = logic entity): set entity `+0x128 = throesMs` (default 3000; 20..3000);
   - `0x5a9400` (death explosion, `__stdcall(container)`): skip when `explode=false`. That means no explosion, no FX and no `Explosion` message;
   - `0x5a9310` (gravestone, thiscall `ecx` = entity, arg container): skip when `grave=false`.

   The records are sim state, set by sim scripts at the same tick on every machine, so they are hashed in a Wormsign contributor (`melange.wormkill`) and cleared at UnspawnWorm or match end.

Net safety for every route: the sim script issues the kill. The engine's message processing and the 20 ms throes timer are sim-tick driven, so every machine dies at the same tick. Nothing reads the camera or the local player.

Open risks [U], to check in a LocalNet run:

- A kill during a `holdTurn` (between `Turn.Ended` and the swallowed `StartTurn`): team-elimination and game-over checks, and the next-worm selection when the victim was the next team's chosen worm (`Worm.RemoveFromTeamQueue` exists).
- Killing the last worm of a team.
- `Worm.LandDeath` commentary playing mid-hold.

Bloodsand also needs to know about the kill. With K3 and `explode=false` it sees no `Explosion`. It takes the "state flipped or worm vanished" death branch and would throw organ gibs on a decapitated corpse. Kindjal sends `mod.kindjal.decap` first (see the Bloodsand recon) so Bloodsand marks the slot `decap`.

## (b) Prone victim and mounted attacker

- **There is no prone or knocked-down state [V].** The physics states (`kWPS_*` table `0x90d374`) are Ambulatory 0, DetectJump 1, Ballistic 2, Sliding 3, Vaulting 4, Override 5, Passive 6, DeathThroes 7 and DrownFloat 8. "Knocked" worms are Ballistic flights with `Blastflight2..5` clips that recover with `RecoverBack1/2`, `RecoverFront1` and `RecoverBurried1` (resolved by the graphic entity init `0x5a49a0`).
- `Worm.OverridePhysics` (handler in `0x5adcf0`):
  - a positive int puts the worm in state 5, ORs the int into `+0xe8`, sets `+0xec` bit 1 and queues anim `0x12`;
  - zero or below clears the bits, goes Ballistic and queues anim 7.
  
  The handler **does not check a slot**, so it reaches every worm entity that receives it. Do not use it from a script.
- **Recommendation [P]:**
  - **The sim pins and kills.** At the strike, in the sim hit handler, it calls `victim:anchor(true)` and `attacker:anchor(true)`. The anchor refuses Ballistic and Sliding, swallows walk and jump, and is released automatically when the victim enters state 7 (`Repin`, [sim_worms.cpp](../src/lua/sim/sim_worms.cpp)). It sends `Kindjal.Execute` (`victim|attacker|tick`), arms `holdTurn` at `Turn.Ended`, and issues `worm:kill{explode=false}` at the decap beat. The beat must be at least `throesMs + 0.5 s` before the hold ends.
  - **The client poses.** The victim is rendered rotated onto its back and the attacker translated and bent onto it, both through the pose override (c). Sim positions stay where they were, so Wormsign, physics and AI never see the cinematic.
- A sim-side placement API (for example `anchor(true, x,y,z[,yaw])` writing container `+0x38` and yaw `+0x90` through `Backend::Write`) is possible and deterministic, but the spade does not need it. It is left out to keep the risk at zero.
- Engine anim ids can be queued on a logic entity, deterministically, through `0x5ac4d0(id, param)`, which pushes to the queues at entity `+0x17c`/`+0x188` (`0x5acb80` also sets the current id `+0x244`). Known ids are 7 (fly), `0x12` (override), `0x13` (death) and `0x15` (drown). The id-to-clip table lives in the graphic entity or data and is **[U]**, so this is not proposed.

## (c) Pose override

### The worm skeleton [V]

The data comes from `xomtool clone W4.Worm --from Bundl474.xom --tree` and `inspect`. `W4.Worm` (`w4worm.xom`, section 474) has three `XSkin`s on one skeleton:

- the body, `boggybodyShape`, 567 vertices, plus eye and teeth shapes;
- `boggyhand_leftShape`;
- `boggyhand_rightShape`.

Every `XSkinShape` has the **same 34-entry bone palette**, in this order. The palette index is the `BlendMat` index:

| idx | bone | parent | idx | bone | parent |
|---|---|---|---|---|---|
| 0 | main (root) | - | 17 | wrist_left | 16 |
| 1 | tail | 0 | 18 | pinkyknuckle_left | 17 |
| 2 | tail2 | 1 | 19 | pinky_left | 18 |
| 3 | tail3 | 2 | 20 | indexknuckle_left | 17 |
| 4 | **head** | 0 | 21 | index_left | 20 |
| 5 | upperlip_right | 4 | 22 | foreknuckle_left | 17 |
| 6 | upperlip_left | 4 | 23 | fore_left | 22 |
| 7 | bottomlip_left | 4 | 24 | thumb_left | 17 |
| 8 | bottomlip_right | 4 | 25 | shoulder_right | 0 |
| 9 | bottomlip | 4 | 26 | **wrist_right** | 25 |
| 10 | mouthmiddle_left | 4 | 27 | pinkyknuckle_right | 26 |
| 11 | mouthmiddle_right | 4 | 28 | pinky_right | 27 |
| 12 | eyetop_left | 4 | 29 | indexknuckle_right | 26 |
| 13 | eyebrow_left | 4 | 30 | index_right | 29 |
| 14 | eyetop_right | 4 | 31 | foreknuckle_right | 26 |
| 15 | eyebrow_right | 4 | 32 | fore_right | 31 |
| 16 | shoulder_left | 0 | 33 | thumb_right | 26 |

Locators (unskinned `XGroup`s):

- `HatLocator`, `TashLocator`, `BubbleSpawn` and `VomitLocator` under head;
- **`WeaponLocator` under `wrist_right`** (the held weapon mesh hangs here);
- `Pack_Locator` and `NameLocator` under main.

The hands are separate skins, weighted only to the shoulder, wrist and finger bones, so moving a hand subtree moves the hand rigidly.

The body's vertex weights against bind-space height (y) are what make a clean cut possible:

- head-subtree weight (bones 4..15) is 0 below y = -6;
- it rises through 0.2..0.5 between y = -6 and y = -2, and 0.74 at y = 0;
- it is at least 0.94 from y = 2 up to the top (y = 20).

The "head" is the upper half of the worm, and the blend band at y = -6..+2 is the neck.

### How the game skins [V]

`CG/FixedFunction.cg` declares `float4x4 BlendMat[35] : register(c8)`. The `FFVertex*Skinned` entries, which include `FFVertexMainTexLitDirectionalSkinned`, blend up to 4 bones per vertex (`numBonesPerVertex`, a uniform). They treat the skinned position as **world space**: the comment says "Object-space vertex coord (or world space for skinned objects)", and only `view` and `projection` follow.

The game binds Cg through cgGL, so each palette entry is `World(bone) x PoseMatrix(bone)`, flushed into the bound ARB vertex program's local parameters. Melange's GLSL replacement layer ([shaders_glsl.cpp](../src/render/mirage/shaders_glsl.cpp)) already:

- mirrors every `glProgramLocalParameter*` per ARB program (`Store`);
- hooks `glDrawElements`, `glDrawRangeElements` and `glDrawArrays`;
- resolves any Cg parameter to its register (`GetParameterResourceIndex`).

Sunstone's replaced fragment stage links with the Cg-compiled `FFVertexMainTexLitDirectionalSkinned` and draws skinned worms correctly, which shows that `BlendMat` reaches that path through the mirrored local registers **[I]**. A one-frame `gltrace` capture with `cgGetNamedParameter(prog, "BlendMat")` logged would confirm the register base (expected local 8) **[U]**.

### Route A: patch the palette at the draw [P], recommended

This is a new client module, "MiragePose", next to `shaders_glsl.cpp`. It shares the mirror and the draw hooks, and installs them even when no GLSL replacement file exists.

1. **Find the skinned programs.** When a Cg program is first bound, check whether its entry matches `FFVertex*Skinned`. If so, resolve `BlendMat[i]` → ARB local index (`cgGetArrayParameter` plus `GetParameterResourceIndex`) and keep `{arbId, base, count=34}`. Also keep the `model` register of non-skinned programs, which tints already resolves as `modelIndex`, for the attachments.
2. **Work out which worm a draw belongs to.** Before each skinned draw, while any override is live:
   - read the 34 matrices from the mirror;
   - compute `pivot0 = BlendMat[0] x bindPivot(main)`, where `bindPivot(i) = inverse(PoseMatrix_i) x origin`, a 34-entry table generated from `Bundl474.xom` and self-tested;
   - pick the nearest live worm by container position plus a fixed centre offset, read on the main thread from the same frame's game state, within 14 units.
   
   The body, eye, teeth and hand shapes of one worm all carry the same palette, so they classify alike. The worms in a mount are at least the melee reach apart in the sim, because the mount is only visual, so the unpatched palette never confuses them.
3. **Build the patched palette.** Each bone gets a world delta `D_i`, giving `P'_i = D_i x P_i`. A child inherits its parent's delta first, so `D_i = D_parent x L_i`, where `L_i` is the bone's own local edit about its world pivot `P_i x bindPivot(i)`. The edits:
   - **Whole-worm transform:** `D_root`, for example "prone on back" is a 90-degree rotation about the feet, and "mount" is a translation plus pitch over the victim.
   - **Bend:** rotate `head` or `tail*` about their pivots.
   - **Hands:** rigid wrist targets. `D = T(target) x R x T(-currentWristPivot)` is applied to the whole hand subtree (16..24 left, 25..33 right). This is the "hands follow the spade" IK: the client spade mesh gives the two grip points every frame.
   - **Hide head as a stump:** `D_head = T(neck) x S(sx, 0, sz) x T(-neck)` in the bone's frame, applied to the head subtree. Vertices weighted fully to the head flatten into a disk at the neck plane, and the blend band at y = -6..+2 tapers into it. The result is a flat-topped stump, not a spike, because nothing collapses to a distant point.
4. **Draw.**
   - On the plain ARB path: upload `P'` with the driver's own `glProgramLocalParameters4fvEXT` (`n_ls4fv`) for the 136 registers, call the real draw, then re-upload the original 136 so cgGL's shadow state stays true.
   - On the GLSL-replacement path (Sunstone and similar): patch the mirror vector, bump `version`, run `Upload()`, draw, then restore and bump again.
5. **Severed head (ghost draw).** For a live "copy" request, call the same draw a second time with palette `G`:
   - head subtree: `G_i = H x P_i`, where `H` is the client's rolling-head transform relative to the original neck;
   - every other bone: `T(neckGhost) x S(0)`, so the body collapses into the neck point and the blend band forms a second stump on the head side.
   
   This draws the worm's real head (texture, eyes, Sunstone skin and team colours) with no client head mesh. The cost is one extra draw per skinned shape of one worm.
6. **Expiry.** Like `cameraOverride`, a pose lasts `frames` frames (default 3, range 1..600) unless it is re-set, so a crashed script cannot leave a worm bent.

Cost: about 8 skinned draws per worm, 136 registers each, only for posed worms. That is negligible. This route is **render-only**: it changes no sim state and no Wormsign input, so it cannot desync.

#### Attachments

Rigid meshes on locators (the held weapon on `WeaponLocator`, the hat on `HatLocator`, the moustache on `TashLocator`, glasses) are separate non-skinned draws placed by their `model` matrix. They do **not** follow a patched palette. There are three options, in order:

- **Held spade:** give the Grave Spade an invisible held mesh (a tiny or empty `kindjal.GraveSpadeHidden` bank through `WeaponGraphicsResourceID`). The client draws the animated spade itself with `wum.draw.mesh`, and the hand IK grips it.
- **Hat and accessories on a decapitated or bent worm:** in the same draw hook, for a non-skinned draw whose `model` translation equals (within 0.5 units) the original world position of an overridden worm's `HatLocator`/`TashLocator`/`WeaponLocator` this frame, apply that bone's `D` (or the zero-scale stump delta) to the `model` register for the draw. **[P]**
- The engine's accessory mask: `WXWormGraphicEntity` keeps `m_nHatMask` and calls `ActiveWorm_UnmaskAccessories` (`0x5a43d0`, `0x5a17d0`). **[U]**, a backup way to hide the hat.

#### Risks for Route A

- **Shadow pass [U].** If the engine draws worms into its shadow map with a skinned program, the patch applies there too, because it works per draw. If the shadow map uses another path, a prone worm would cast a standing shadow. Check with `[MirageShadows]` on.
- **Culling.** The engine culls the worm by its real instance bounds. A pose offset of about 20 units over the victim is well inside the bounds, so this is not a problem in practice.
- **Identification.** It fails only if two worms' `main` pivots are within a few units, and two sim worms cannot overlap that closely. Use frame-to-frame continuity as the tie-break.
- **Unexpected programs.** If `BlendMat` turns out not to be in program locals (an env binding), Melange must also hook `glProgramEnvParameter*`. Unlikely given Sunstone.

### Route B: CPU joint override [U], not pursued

`XMeshInstance` (vtable `0x888074`) advances its `XAnimScheduler` in `Update` (`0x6a02b0`, slot 11):

- the time is at `+0xcc`;
- the scheduler is at `+0xc8`, slots `+0x20`/`+0x1c`/`+0x24`;
- the clip library is at `+0xc0`.

Overriding the joint transforms after the scheduler runs would also move the hats and the held weapon, because those are children of joints. The research gap is where the per-instance joint output lives, and whether the skeleton graph is shared between instances. Route A covers the spade without it.

### Route C: fallback retimed vanilla clips

The client plays a clip on a worm's mesh instance:

- `XMeshInstance::PlayClip(name, &handle, speed, ...)` is `0x6a0730`. It finds the clip through `FindClip` `0x6a0350`, then calls the scheduler's `+0x14` with a duration scale of `1/speed`.
- `GetClipLength(name)` is `0x6a0990`.
- `0x5ce7e0` plays `Nailed` with speed 1.0.

The worm's instance should be `WXWormGraphicEntity+0x30` (it gets the `HatLocator` attachment in `0x5a49a0`) **[I]**. The slot-to-graphic-entity path is **[U]**.

**Desync risk.** Clips can carry anim events (`XExpandedAnimInfo`). If an event posts a logic message, playing a clip on one machine only would desync. So only play clips during the hold, only clips without events, or play them from the sim.

Useful clips (329 in `XCULLEDboggyb`):

| Use | Clips |
|---|---|
| Dig | `WindupBat`, `Fire1Bat`, `Fire2Bat`, `FireNMN`, `HoldNMN`, `FireProd`, `Poke`, `LobThrown`, `InspectHands` |
| Victim knocked down | `Hit_Back`, `Hit_Front`, `Blastflight2`..`5`, `Fall`, `Squish`, `Nailed`, `NailedHitBack`, and the first frame of `RecoverBack1` (lying on the back) |
| Victim | `Cower`, `Terror`, `Pray`, `Cover_Head`, `ClutchChest`, `Wounded`, `Scared` |
| Attacker | `Fire1Bat`, `Fire2Bat` (overhead chop), `FireProd`, `Shake_Fist`, `EvilGrin`, `PointAndLaugh`, `Victorious_Grin`, `TauntBat` |

## Proposed API (Melange 0.16.0) [P]

### Sim (Lua 5.0, deterministic, hashed)

```lua
-- Kills through the engine death path (Worm.KillNow). Options (all optional):
--   explode  (default true)  false: no death explosion, no WXP FX, no Explosion message
--   grave    (default true)  false: no gravestone
--   throes   (ticks, 1..150, default 150 = 3 s) length of the DeathThroes state before unspawn
-- Returns true, or nil and "no worm" | "dead" | "drowning" | "not in a match".
ok, why = wum.sim.worm(slot):kill({explode = false, grave = true, throes = 25})
```

The C++ side:

- `simworms::Backend` gains `Kill(slot, opts)`, which posts `Worm.KillNow` (`IntMessage` `0x691951`, Post `0x6910e4`).
- `WormAnchor` gains three inline hooks, each enabled only while a kill record exists:
  - `0x5adbf0` (post: entity `+0x128 = throes*20`);
  - `0x5a9400` (skip if `!explode`);
  - `0x5a9310` (skip if `!grave`).
  
  Each hook gets its prologue checked with `mem::Expect`, and the records are cleared at `WXWormManager.UnspawnWorm` and at VM close.
- A Wormsign contributor `melange.wormkill` hashes the records.
- The sim event `sim.wormKilled` (`slot`, `tick`) fires when the unspawn happens.

`worm:damage(n[, type])` (`AddDamage` `0x5ab7e0` on the logic entity, uncapped type 6) is a cheap addition for other mods. The spade does not need it.

### Client (Lua 5.4, render-only)

```lua
-- Pose one worm for `frames` frames (re-set every frame, like cameraOverride). One mod per slot ("busy").
ok, why = wum.render.wormPose(slot, {
  frames = 3,
  root   = {pos = {x,y,z}, yaw = deg, pitch = deg, roll = deg},   -- whole-worm transform about the feet (world)
  bones  = { head = {rot = {x,y,z}}, tail = {rot = {...}}, main = {move = {...}} },  -- local edits, children inherit
  hands  = { left = {pos = {x,y,z}, dir = {x,y,z}}, right = {...} },  -- rigid wrist targets (IK grip)
  hide   = { head = "stump" },                                       -- "stump" (flat neck) or "point"
  attach = true,                                                     -- hats / accessories follow their bones
})
wum.render.wormPose(slot, nil)                -- release

-- Draw a second copy of the worm's head subtree (real texture) at `head` (a 16-number matrix or {pos,yaw,pitch,roll}).
ok, why = wum.render.wormCopy(slot, {bones = "head", model = m, frames = 3})

-- Last frame's rendered (posed) bone frames, for gore and for attaching client meshes: name -> {x,y,z, 9 axis floats}.
bones = wum.render.wormBones(slot)
```

These can never desync: they read only render state and change only draws. The C++ plan for them:

- `src/render/mirage/pose.cpp`, with its logic in `pose_logic.h` (pure maths, unit-tested: the palette composition, stump scale and identification);
- the 34-bone bind-pivot and parent table generated from `Bundl474.xom` (the self-test compares it with the bundle when `--game` is available);
- a pre-draw and post-draw filter added to `shaders_glsl.cpp`'s `HkDrawElements`/`HkDrawRange`/`HkDrawArrays`;
- the Lua bindings in `wum_render.cpp`.

Skinned programs are found by entry name. If `BlendMat` cannot be resolved, the feature reports "unsupported" and the mod falls back to Route C or client meshes.

## Verification plan (in game, before building on it)

1. **K1 probe.** In a guarded match, run `lua.eval` against the sim: `wum.sim.sendInt("Worm.KillNow", <enemy slot>)`. Expect the worm to die with the death anim, explosion, grave and unspawn, and the `Melange.log` worm lines to agree. Then repeat during a `holdTurn`, and on the last worm of a team.
2. **Palette probe.**
   - Log `BlendMat` resolution for each `*Skinned` program (base register).
   - Take one `gltrace` frame and confirm the 136 `ProgramLocalParameter` floats per worm draw.
   - Write a debug `pose.test <slot> prone` that rotates the root by 90 degrees and `pose.test <slot> stump` for the head stump. Screenshot both with Sunstone on and off.
3. **Shadow probe.** Check that a prone worm's shadow matches the pose with `[MirageShadows]` on.
4. **LocalNet.** Run a spade execution on both A and B with a full Wormsign hash run (`melange-ingame-test-harness` runbook §8): no desync, equal hold, equal unspawn tick.

## Addresses (build #1077)

| What | Address |
|---|---|
| `WXWormLogicEntity::HandleMessage`, second level | `0x5b07c0`, `0x5adcf0` |
| Worm subscription list (`Worm.KillNow` at `0x5a5c3c`) | `0x5a5ae0` |
| `AddDamage(amount, type)` thiscall | `0x5ab7e0` |
| ApplyDamage handler / "Waiting To Die" | `0x5abc50` / `0x5a70e0` |
| TimeToDie / "Worm Dying" | `0x5adbf0` / `0x5a7190` |
| Throes tick / drown tick / water check | `0x5aa080` / `0x5aa130` / `0x5ad640` |
| Death explosion / gravestone | `0x5a9400` / `0x5a9310` |
| Vapourize handler / sender | `0x5ac160` / `0x5885f0` |
| `WXWormManagerService::HandleMessage` | `0x5b5e70` |
| Queue anim id / set current | `0x5ac4d0` / `0x5acb80` (entity `+0x17c`, `+0x188`, `+0x244`) |
| Damage.Impulse ctor / melee hit (`MeleeWeaponLogicEntity`) | `0x518c20` / `0x567a40` |
| `IntMessage` ctor / Post / engine `SendIntMessage` | `0x691951` / `0x6910e4` / `0x69676a` |
| Message objects: KillNow / Vapourize / TimeToDie / DieQuietly | `0x95fcd8` / `0x95fcb0` / `0x95fcc8` / `0x95ff40` |
| `XMeshInstance` vtable / Update / FindClip / PlayClip / ClipLength | `0x888074` / `0x6a02b0` / `0x6a0350` / `0x6a0730` / `0x6a0990` |
| `WXWormGraphicEntity` init (clip names) | `0x5a49a0` |
| Cg `CGParam` vtable (slot 9 `cgGLSetMatrixParameterfr` `0x796ab0`) | `0x8b3c30` |

Damage.Impulse layout (`0x518c20`): `+8` damage, `+0xc` impulse direction, `+0x18` position, `+0x24` impulse magnitude, `+0x28` target slot (u8), `+0x29` (u8), `+0x2c` type (5 has a 200 ms cooldown).

The melee hit at `0x567a40` builds it for the struck worm. That is the hook point for a melee "hit" event with a veto, which the weapons recon owns.
