# Grave Spade RE: the worm animation system and the render-only root transform

> **Status:** built and shipped in Melange 0.16.0. These are the research notes written before and during the
> work; the API as built is documented in [lua-api.md](lua-api.md), and where the two differ lua-api.md is right.

These notes are for the Grave Spade execution: the victim lies down and the attacker mounts it. They cover the worm's
animation system on build #1077 and a render-only way to override it, which was **proven live in instance D on
2026-11-18**.

Background: [gravespade-re-worm.md](gravespade-re-worm.md) found that #1077 skins worms on the CPU, so the GPU palette
route (MiragePose) cannot work. The route below does not need GPU skinning. It works one level higher: it changes the
**animation weights** the CPU skinner reads, and the worm mesh's **root transform**.

Confidence tags follow [meshes.md](meshes.md): **[V]** read in the disassembly, **[L]** seen live in game (instance D,
probe module), **[I]** inferred, **[U]** unknown.

## Verdict

| Question | Answer |
|---|---|
| Can a worm be laid flat for rendering only? | **Yes [L].** Write an Euler rotation to the worm's `XMeshInstance` (vtable slot 21, `+0x54`) right after the worm's graphic entity update. The body, hands, hat, accessories and the shadow all follow. |
| Can the attacker be placed on top of the victim? | **Yes [L].** Use the same instance's `SetPosition` (slot 20, `+0x50`), with a delta from the drawn position, a yaw that faces the victim and a forward pitch. |
| Can a chosen clip be played or held at a frame? | **Yes [L].** Rewrite the `XAnimInstance` per-clip `{weight, time, rate}` table around `Evaluate` (`0x7ad0ef`), then restore it. The game's scheduler, its anim events and the sim never see the change. |
| Do the hands follow the dig motion? | **Yes, the engine has a handle for it [V]/[L]:** the procedural pose-slider clips `LeftArmRotX/Y`, `RightArmRotX/Y`, `HeadRotX/Y`, `TailAngle` and `TipAngle`, held at a time in 0..2 (1 = neutral). |
| Are sim positions changed? | **No.** Only the drawn mesh is changed. The container position `+0x38` and the logic entity are never written. |

Proof screenshots (instance D, `Melange\m1t\gsm_*.png`; copies are in `docs/images/gravespade-pose/`):

- `root-sidecam.jpg`: a vanilla worm; then pitch X = 90 (it falls forward); then roll Z = 90 (it lies on its side);
  then `RecoverBack1` held at t = 0, solo.
- `euler-topdown.jpg`: a top-down camera that fixes the Euler convention.
- `mount-r1.jpg` and `mount-r2.jpg`: the victim on its back (pitch -90). The attacker is moved onto its belly, faces
  the victim's head and is pitched forward, holding `Fire2Bat`, `HoldBat` or `Cower` layered over the game's clips.
  After `ap.off` both worms return to vanilla at once.
- `held-clips.jpg`: `RecoverBack1/2`, `RecoverFront1`, `Blastflight2`, `Nailed` and `Squish`, held with weight 20 over
  `Base`. They show that the recovery and flight clips carry **root motion** (see [Clips with root motion](#clips-with-root-motion)).

## How a worm is drawn

### Objects [V]

```
WXWormGraphicEntity (vtable 0x85e474, one per worm, client side of the worm)
  +0x24  XMeshInstance* "W4.Worm" (vtable 0x888074), the drawn worm
  +0x64  u8 slot (passed to 0x50bdf0 -> WormDataContainer)
  +0x94..+0x10c  clip ids resolved by name at init 0x5a49a0 (table below)
  +0x110..+0x124 / +0x124.. / +0x138..  5 manual overlay clips {id, time, length} advanced by 0x59fe00
  +0x14c  target yaw   +0x150  body yaw (smoothed)   +0x154  head yaw
  +0x158  rotation X   +0x15c  rotation Z    (both 0 in every state seen)
  +0x160..+0x170  turn-hop state (HopLeft/HopRight, 0x59fe80)
  +0x174  graphic state (0 ambulatory, 1 override, 2 flight, 3/4 slide, 5, 6, 7 death throes, 8 drown)
  +0x178/+0x17c, +0x184  anim-event queue: ids and params, drained by 0x5a3620
  +0x194  last update time (ms)   +0x198  frame delta in 20 ms ticks

XMeshInstance (vtable 0x888074)
  +0x10  position (vec3)   +0x1c  rotation (Euler, radians)   +0x28  scale
  +0x7c  instance transform node (pos +0x48, rot +0x54, scale +0x60, dirty bit 2 at +0xb)
  +0xc0  XAnimClipLibrary   +0xc4  XAnimInstance   +0xc8  XAnimScheduler   +0xcc  scheduler time
  slot 11 Update(dt) 0x6a02b0      slot 20 SetPosition(this, vec3*, force) 0x6b6b90
  slot 21 SetRotation 0x6b6c20     slot 22 SetScale 0x6b6cb0      slot 23 SetVisible(this, bool) 0x6b49c0
  thiscall helpers (ecx = mesh): FindClip(name, &id) 0x6a0350, PlayClip(id, &handle, speed, ...) 0x6a03d0,
  PlayClipByName 0x6a0730, SetClipTime(id, time, weight) 0x6a04c0, SetClipWeight(id, w) 0x6a0530,
  ClipLength(id) 0x6a0600, ClipLengthByName 0x6a0990
```

Every XOM interface method is `__stdcall` with `this` first, for example `SetRotation(mesh, float[3], force)`. The
graphic entity update is `__stdcall(this, timeMs)` and returns with `ret 8`.

### The per-frame update [V]

`WXWormGraphicEntity` slot 6 is `0x5a4740`. In order, it:

1. reads the container (`0x50bdf0(slot)`);
2. runs `0x5a2710`, which smooths the yaw (`+0x14c` → `+0x150` → `+0x154`) and plays HopLeft/HopRight on large turns;
3. runs `0x5a3620`, which drains the anim-event queue `+0x178` (see below);
4. runs the state function for `+0x174` (`0x5a2a00` ambulatory, `0x5a01a0` flight, and so on);
5. runs `0x5a2420`, which places the mesh:
   - `0x5a00b0`: `SetPosition(mesh, container.pos + (0, 3, 0), 0)`;
   - `SetRotation(mesh, {+0x158, +0x150, +0x15c}, 0)`.

The setters skip the write when the value is unchanged and `force` is 0. So a post-hook on `0x5a4740` that calls them
with `force = 1` wins for that frame, and the next frame's vanilla call restores the vanilla transform by itself **[L]**.

### Euler convention [L]

The probe wrote `(rx, ry, rz)` in degrees, with a top-down camera (`euler-topdown.jpg`):

- `ry` is the yaw about +Y: the facing is `(sin ry, 0, cos ry)`. It matches `wum.game.worms()[i].yaw`.
- `rx` is applied **in the worm's own frame after the yaw**. It is a pitch about the worm's right axis:
  - `+90` tips the worm forward onto its face (prone, head toward the facing);
  - `-90` lays it on its back, head behind it.
  
  The lab check was yaw 90 + `rx` 90: the body lay along world X, the facing direction.
- `rz` is a roll about the worm's forward axis: `+90` lies it on its side.
- The order between X and Z, when both are set, is **[I]** (probably `R = Ry·Rx·Rz`). The spade needs only yaw plus
  pitch.
- The pivot is the mesh origin, which is the feet: container position + 3 in Y. Laid on its back, the worm's back
  surface is about 5.5 below the pivot, so raise it by about +4 to sit on the ground. Its belly is then about 9.5 above
  the container Y.

Hats, glasses, the held weapon and the Pack/Name locators are children of the instance skeleton, so they follow the
root. The shadow follows too: it is cast from the same CPU-skinned mesh (`gsm_flat90x` shows a prone silhouette on a
rock) **[L]**. The floating name and health labels are HUD and stay at the sim position; hide them with
`wum.render.hideHud`.

## The animation system

### Classes [V]

| Class | vtable | Role |
|---|---|---|
| `XAnimClipLibrary` | `0x8b5fb4` | The clips of a mesh. Slot 6 (`+0x18`) returns `{.., u16 count at +4}`. Slot 7 (`+0x1c`, `(lib, id)`) returns a clip record `{char* name, float length, ...}`. |
| `XAnimInstance` | `0x8b592c` | The per-instance blend state. `+0x14` = global time. `+0x48` = an XArray of **12-byte entries `{float weight, float time, float rate}`**, one per clip id (begin `+0x54`, end `+0x58`). `+0xa4` = "active set dirty". |
| `XAnimScheduler` | `0x8b5714` | Timed playback: queues, events and fades. It writes the weights and times into the instance each update. |

`XAnimInstance` methods (stdcall):

- slot 11 `SetGlobalTime` `0x7aa738` and slot 12 `GetGlobalTime` `0x7aa748`;
- slot 13 `GetEntry(id)` `0x7ab8a8`;
- slot 14 `SetWeight(id, w)` `0x7ab8bb`, which sets `+0xa4` when a weight crosses zero;
- slot 15 `SetTime(id, time, rate)` `0x7ab900`;
- slot 9 **`Evaluate` `0x7ad0ef`**.

### Evaluate [V]

For each clip id with `weight != 0`, `Evaluate` samples the clip at `rate * globalTime + time` and accumulates it per
channel through `0x7ac1a0` (`value*weight`, and the weights). `0x7acc6f` then normalises each channel by its weight
sum. So it is a **per-channel weighted average**: a channel that only one clip drives takes that clip's value, at any
weight. Channels flagged `0x10` take the maximum instead. When `+0xa4` is set, `0x7ace12` first rebuilds the active
channel set.

### The update chain [V]

`XMeshInstance::Update` `0x6a02b0` adds `dt` to `+0xcc` and calls scheduler slot 8 `SetTime`. It then calls scheduler
slot 7 `0x7a9ed4`, which processes events and queues, sets the instance's global time and calls `Evaluate`. The CPU
skinner then blends the evaluated joints.

Anim events (`XExpandedAnimInfo`) live in the **scheduler**, not in `Evaluate`. An override that only changes what
`Evaluate` sees fires no events.

### How a worm's clips are chosen [V], with live values [L]

Every frame a worm has these clips at a non-zero weight (from `ap.dump`):

| Kind | Clips | How they are driven |
|---|---|---|
| Base layer | `Base` (id 20), `w 1, t 0, rate 0` | always on |
| **Procedural pose sliders** | `HeadRotX/Y`, `LeftArmRotX/Y`, `RightArmRotX/Y`, `Eyes_LR/UD`, `TailAngle`, `TailLag`, `TipAngle`, `Teeth` | Set every frame through `SetClipTime(id, t, w)` with **rate 0**. The time is the parameter: **t = 1 is neutral and the range is 0..2** (clip length 2.0). The idle head and arm look-around showed t = 0.87..1.28. |
| Timed one-shots | emotes, taunts, weapon draw/fire/hold (`Taunt3`, `Angry`, `Frown`...) | `PlayClip` through the scheduler: `rate 1`, with `time = -startTime` so they run on the global clock |
| Locomotion | `Walk`, `WalkTail`, `Jump*`, `Fall`, `Land`, `Skid*`, `Blastflight2..5`, `Recover*`, `FallDrown`, `HopLeft/Right`, `FP*` | The state functions of `+0x174`, through the ids cached at init (table below) |

The anim-event queue: the logic side pushes `(id, param)` (the logic entity's `0x5ac4d0` and `0x5acb80`, see
gravespade-re-worm). `0x5a3620` drains it every frame:

- `0x12` override → state 1;
- `0x11` → state 6;
- `0x13` death → state 7, and posts a message `0x4d3410(0xe, slot)`;
- `0x15` drown → state 8;
- `0x16` → state 4, with the angle 2π stored at `+0x1c0`;
- `0x17` → the "weapons/Thud" sound;
- cases 2..7 → flight (state 2), with the Blastflight clips;
- 8 and 9 → play the `+0xb8`/`+0xbc` (Land/Vault) clips.

Clip ids cached at init `0x5a49a0`, written by `FindClip` into the graphic entity:

| Offset | Clip | Offset | Clip | Offset | Clip |
|---|---|---|---|---|---|
| `+0x94` | Walk | `+0xbc` | Vault | `+0xe4` | Blastflight5 |
| `+0x98` | WalkTail | `+0xc0` | TailAngle | `+0xe8` | RecoverFront1 |
| `+0x9c` | Jump | `+0xc4` | TailLag | `+0xec` | RecoverBurried1 |
| `+0xa0` | Backflip | `+0xc8` | TipAngle | `+0xf0` | RecoverBack1 |
| `+0xa4` | Fwdflip | `+0xcc` | HopLeft | `+0xf4` | RecoverBack2 |
| `+0xa8` | Jump_Start | `+0xd0` | HopRight | `+0xf8` | SkidArms |
| `+0xac` | AT_FB | `+0xd4` | Skid | `+0xfc` | "Death" (**not in the library**: FindClip fails, so the id is 0) |
| `+0xb0` | AT_LR | `+0xd8` | Blastflight2 | `+0x100` | FallDrown |
| `+0xb4` | Fall | `+0xdc` | Blastflight3 | `+0x104..10c` | FPX, FPY, FPZ |
| `+0xb8` | Land | `+0xe0` | Blastflight4 | | |

### The worm clip library (329 clips, live dump of slot 0) [L]

Lengths are in seconds. The ones useful for the spade:

- **Lying or fallen:**
  - `RecoverBack1` 6.0 and `RecoverBack2` 6.0 (t = 0 = on its back, but with root motion);
  - `RecoverFront1` 9.5 (t = 0 = flat on its front, raised);
  - `RecoverFront2` 1.667, `RecoverBurried1` 2.625;
  - `Blastflight2` 1.0, `Blastflight3` 0.5, `Blastflight4` 1.0, `Blastflight5` 1.0;
  - `Fall` 0.25, `Squish` 1.0, `Nailed` 2.5 (sinks into the ground);
  - `Hit_Back` 3.167, `Hit_Front` 0.833, `Hit_Left/Right` 1.333, `NailedHitBack/Front/Left/Right` 0.917;
  - `FallDrown` 2.0.
- **Victim reactions:** `Cower` 3.833, `Terror` 2.0, `Scared` 2.0, `Cover_Head` 3.125, `Pray` 4.417,
  `ClutchChest` 7.083, `Wounded` 2.0, `Shriek` 1.25, `Gasp` 2.5, `CantLook` 2.0, plus the `*Mouth` face layers.
- **Attacker (bend and chop):**
  - `WindupBat` 0.25, `Fire1Bat` 0.583, `Fire2Bat` 1.042 (overhead), `HoldBat` 2.0, `DrawBat` 1.333, `TauntBat` 2.0;
  - `FireProd` 0.667, `HoldProd` 2.0, `Poke` 2.0;
  - `FireNMN` 2.125, `HoldNMN` 2.0;
  - `WindupFirepunch` 0.333, `Fire1Firepunch` 0.167;
  - `LobThrown` 1.0, `InspectHands` 6.458;
  - `EvilGrin` 2.0, `Victorious_Grin` 2.0, `PointAndLaugh` 5.833, `Shake_Fist` 2.083.
- **Pose sliders, for the hands following the dig:** `LeftArmRotX/Y`, `RightArmRotX/Y`, `HeadRotX/Y`, `TailAngle`,
  `TailLag`, `TipAngle`, `Eyes_LR/UD` (all 2.0 long, t = 1 neutral), and the `Aim*` clips (2.0: `AimBazooka`,
  `AimGrenade`... t probably maps to the aim pitch **[I]**).
- **There is no "Death", "Sleep" or "Idle" clip.** The death look is state 7 with `Wounded` and `FallDrown`-style
  layers; `Inactive` 2.0 is the inactive face; `Bored` 12.79 and `Yawn` 3.6 are idles.

The full list is in the session log (`[ap] clip N name length` lines) and can be dumped again with the probe's
`ap.clips <slot>`.

### Clips with root motion [L]

Held over `Base` with weight 20, `RecoverBack1/2`, `RecoverFront1`, `Blastflight*`, `Nailed` and `Squish` all move the
whole worm far from its pivot. At t = 0:

- `RecoverBack1` puts the worm above or off camera;
- `RecoverFront1` puts it flat but raised about 20 units;
- `Nailed` puts it below the ground.

The flight and recovery clips are authored relative to the flight arc, so **use the root rotation for "prone", not
these clips**. If one of them is wanted for its limb pose, cancel its root offset with a measured position delta per
clip and frame.

### Weights and modes [L]

- **Solo** (every other weight 0, ours 1): the channels that only `Base` or the sliders drive go to an unnormalised or
  zero state. Fingers and hands splay (`rb1` in `root-sidecam.jpg`). Do not use it.
- **Base + ours** (only `Base` kept): fine for body clips. Pose sliders reset to the bind pose.
- **Keep all + ours at weight 1..3** (layer on top): this looks best. `Fire2Bat`, `HoldBat` and `Cower` read clearly
  on the mounted pair (`mount-r2.jpg`). At weight 20 some scale or translation channels get exaggerated (`Poke` showed
  a stretched body in `mount-r1.jpg`), so keep the weight in about 1..3.

## Render-only override: hook plan (proven by the probe)

Two inline hooks (safetyhook), both only on the client and both reverting by themselves:

1. **`0x5a4740` post-hook** (prologue `83 ec 0c 56 8b 74 24 14`, `__stdcall(gfx, ms)`):
   - read `slot = *(u8*)(gfx+0x64)`, `mesh = *(gfx+0x24)` and `anim = *(mesh+0xc4)`, and record them per slot. This
     is also the **slot → mesh map**, with no list walk;
   - if the slot has a pose, call `SetRotation(mesh, {rx, ry or *(gfx+0x150), rz}, 1)` and
     `SetPosition(mesh, drawnPos + delta, 1)`, where `drawnPos` is the `mesh+0x10` the vanilla code just wrote.
2. **`0x7ad0ef` wrap** (prologue `55 8b ec 83 ec 2c 56 8b 75 08`, `__stdcall(anim)`): if `anim` belongs to a posed
   slot:
   - copy the `{w, t, rate}` array;
   - apply the clip edits: set the target clip's weight and time with rate 0, optionally zero the others, and set the
     pose-slider times;
   - set `+0xa4 = 1`, call the original, restore the array and set `+0xa4 = 1` again.

The probe was `src/render/mirage/animprobe.cpp`, reverted. A copy is in that session's scratchpad. Its verbs were:

- `ap.set <slot> <clip|-> <time> [weight] [solo|base|keep] [rx|-] [ry|keep] [rz] [dx dy dz]` (degrees, world units);
- `ap.off`;
- `ap.clips <slot>`;
- `ap.dump [slot]`.

Example from the run (victim slot 0 on its back; attacker slot 1 on its belly, facing its head):

```
ap.set 0 - 0 1 solo -90 <vyaw> 0 0 4 0
ap.set 1 Fire2Bat 0.5 1 keep 30 <vyaw+180> 0 <dx> <dy> <dz>
  where (dx,dz) = victimPos - 8*facing - attackerPos, dy = (victim.y + 9.5) - (attacker.y + 3)
```

### Net safety

- Both hooks write only to the **drawn** `XMeshInstance` and to a temporary change of the `XAnimInstance` weights,
  which is restored before `Evaluate` returns. The container (`+0x38` position, `+0x90` yaw), the logic entity, the
  physics state and the anim-event queue are never written. The scheduler and its events are untouched. Wormsign hashes
  sim state, so it cannot see this.
- **[U] to check in a LocalNet run:** whether any sim code reads a skeleton node's world transform from the drawn
  instance, for example the `WeaponLocator` as a projectile origin, or `NameLocator`/`HatLocator` for gameplay rather
  than FX. FX spawns (`0x5c1470` with "NameLocator", "WXP_WormThinking") are visual. During the execution hold neither
  worm fires, so the window is small. Still, keep poses limited to the hold and run the LocalNet check from
  `melange-ingame-test-harness` with a hash comparison.
- The graphic entity update posts some messages itself, such as the death case `0x13` and the `+0x1f8` timer message
  in `0x5a2420`. These are vanilla and are not affected by a post-hook.
- Whether `0x5a4740` runs per render frame or per 20 ms tick: it scales by `(now - last)/20`, so it is frame-driven
  **[I]**. A post-hook re-applies the pose on every call either way.

### The API (proposed here for the implementer, built in Melange 0.16.0)

This replaces the GPU-palette `wormPose` back end on CPU-skinned builds. Same Lua shape, new back end:

```lua
wum.render.wormPose(slot, {
  frames = 3,                                   -- expires like cameraOverride
  root   = {pitch = deg, yaw = deg|nil, roll = deg, move = {x,y,z}},  -- Euler on the drawn instance (yaw nil = game's)
  clip   = {name = "Fire2Bat", time = s, weight = 1..3, mode = "layer"|"base"},  -- held (rate 0) or time advanced by the mod
  sliders = {RightArmRotX = t, RightArmRotY = t, LeftArmRotX = t, HeadRotX = t, TailAngle = t},  -- 0..2, 1 = neutral
})
```

- `hide = {head = ...}` (the decap) is **not** covered by this route.
  - The bones cannot be scaled through the clip table without a clip that scales the head. There is none, though
    there is a head-only face layer.
  - Candidates: a render-only post-`Evaluate` write to the head joint's scale in the instance node graph. The joint
    nodes are reachable through `XAnimInstance::Attach` `0x7ad37f`, which binds each channel to a node field (`+0x60`
    list: node, field id, flags). The other candidate is a client head mesh. That is the third research angle
    (post-skin vertex deform) **[U]**.
- `wormBones` on this route: the joint world matrices after `Evaluate` would come from those same bound nodes **[U]**.

## Addresses (build #1077)

| What | Address |
|---|---|
| `WXWormGraphicEntity` vtable / Update (slot 6) / init (clip names) | `0x85e474` / `0x5a4740` / `0x5a49a0` |
| Yaw smoothing + hop / anim-event queue / mesh placement / SetPosition wrapper | `0x5a2710` / `0x5a3620` / `0x5a2420` / `0x5a00b0` |
| Manual overlay clips / hop clip driver | `0x59fe00` / `0x59fe80` |
| `XMeshInstance` vtable / Update / SetPosition / SetRotation / SetScale / SetVisible | `0x888074` / `0x6a02b0` / `0x6b6b90` / `0x6b6c20` / `0x6b6cb0` / `0x6b49c0` |
| FindClip / PlayClip(id) / PlayClip(name) / SetClipTime / SetClipWeight / ClipLength | `0x6a0350` / `0x6a03d0` / `0x6a0730` / `0x6a04c0` / `0x6a0530` / `0x6a0600` |
| `XAnimInstance` vtable / Evaluate / SetWeight / SetTime / accumulate / normalise / Attach | `0x8b592c` / `0x7ad0ef` / `0x7ab8bb` / `0x7ab900` / `0x7ac1a0` / `0x7acc6f` / `0x7ad37f` |
| `XAnimScheduler` vtable / Play / Update+Evaluate / SetTime | `0x8b5714` / `0x7a9cf8` / `0x7a9ed4` / `0x7a81dc` |
| `XAnimClipLibrary` vtable | `0x8b5fb4` |
| Active worm's graphic entity (global) | `0x95fb04` |
