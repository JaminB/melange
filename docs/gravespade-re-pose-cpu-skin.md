# Grave Spade RE: posing worms on the CPU skinning path

These are research notes for the "cpu-skin" angle of the Grave Spade execution (prone victim, mounted attacker, hands
on the spade). Build #1077 skins worms on the CPU ([gravespade-re-worm.md](gravespade-re-worm.md), update r1), so the
GPU palette route of MiragePose 0.16.0 is dead there. This page finds the CPU route, plans the hook, and records a
live proof in test instance C (2026-11-18).

**Verdict: the pose works on the CPU path.** One inline hook on the GL draw action's `XSkinShape` method (`0x790cd0`)
swaps the 34 bone matrices of the chosen worm for the call and puts them back afterwards. The existing pure maths in
`pose_logic.h` (`Identify`, `Compose`, `ComposeCopy`) apply unchanged, because the CPU matrices are the same thing the
Cg `BlendMat` palette would have held: `inverse bind x world`, row vectors, translation in row 3. A throwaway probe
built on that idea bent heads, tails and hands, pitched whole worms prone, and cut a head off and drew it beside the
body, all in a live match. The remaining gap is the rigid attachments (hat, glasses, held weapon). They do not follow
the posed bones on this route; see [Attachments](#attachments-and-the-held-weapon).

Confidence tags follow [meshes.md](meshes.md): **[V]** verified in the disassembly or in game, **[I]** inferred,
**[P]** proposal, **[U]** unknown.

## How the engine skins a worm [V]

XOM actions dispatch by node class. Each node carries its class id at `node+6` (u16). An action's interface pointer
(`action+4`) holds a method table at `+0x18`, and the dispatcher calls `table[id](iface, node)` as cdecl (see
`0x6df3c7`, the generic "visit children" loop). The static registration records are `{class descriptor, function,
1, 0}`, 16 bytes each. The descriptor is `{GUID, name*}`. The table address is at `name_ref + 8` in the `*_Methods`
type records. For `XOglDrawAction_Methods` the records start at `0x8b26f0`. The ones that matter:

| Class | XOglDrawAction method | What it does |
|---|---|---|
| `XSkinShape` | **`0x790cd0`** | `action+0xc4 = bones.count`, `action+0xc0 = &bones.data` (`node+0x48` is the bone-list container: count `+0x18`, `XBone*` array `+0x20`). Clears bit 0 of `action+0x308`, calls the `XShape` method `0x790af0`, then sets the bit again and zeroes `+0xc0`. |
| `XShape` | `0x790af0` | Binds the shaders (`0x793240`, the vertex program by index, with no skinned variant on this build) and loads the model matrix (`glLoadMatrixf(action+0x2c)` when `action+0x6c` is set). Then it visits the geometry. |
| `XGeoSet` / `XIndexedGeoSet` | `0x799800` / `0x799980` | Uses a display list when `geo+0x34` is set. Otherwise it visits the weights (`+0x5c`), coords (`+0x4c`), normals (`+0x50`), colours (`+0x54`) and texcoords (`+0x58`), then the index set (`+0x60`). |
| `XWeightSet` (and `XPaletteWeightSet`) | `0x7993f0` | `action+0xbc` = the weight set, or 0 when it is empty. `0x799800` zeroes `+0xbc` when a geo set has no weights. |
| **`XCoord3fSet`** | **`0x79a930`** | **The CPU skin for positions.** When `action+0xbc` is set, it builds a stack array `mats[i] = action.bones[i] + 0x70` for `i < action+0xc4`, then calls the blender. For an `XPaletteWeightSet` (the cast is `0x799350`) that is `0x7c57e0(src, influencesPerVertex = ws+0x20 (u16), indices = ws[+0x1c]+0x20 (u8), weights = ws[+0x24]+0x20 (float), mats, out, vertexCount, 12, 12)`. For a plain `XWeightSet` it is `0x7c5e40(src, boneCount, weights, mats, out, n)`. `out` is a per-action scratch vector (`action+0x3d4`, `0x7a4310`). It then loads the view matrix only (`glLoadMatrixf(vtbl+0x3c())`, `action+0x6c = 1`), so the blended positions are **world space**, and calls `glVertexPointer(3, GL_FLOAT, 0, out)`. |
| **`XNormal3fSet`** | **`0x79ac70`** | The same for normals: `0x7c5990` with the same `bone+0x70` matrices (3x3 part). |
| `XIndexedTriangleSet` | `0x79a460` | `glDrawElements(GL_TRIANGLES, ...)` through `0x815434`. |

The blender `0x7c57e0` is x87 code. For each vertex it computes `Σ_k w_k · (v · M[idx_k])` with
`x' = x·m[0] + y·m[4] + z·m[8] + m[12]` (row vector, row-major 4x4). It skips influences with zero weight. **[V]**

### Where the bone matrices come from [V]

`bone+0x70` is written by the **cull and sort pass**, not by the draw. The `XCullSortAction` method for `XBone` is
`0x6ebede`: `0x69c6ad(this = bone+0x70, a = bone+0x30, b = *(action+0x34))`, and `0x69c6ad` is `out = a x b`. So:

- `bone+0x30` is the XBone's inverse bind matrix (`PoseMatrix`);
- `action+0x34` is the current world matrix on the cull pass's matrix stack (the owning joint's world);
- `bone+0x70 = inverseBind x jointWorld`, the CPU twin of `BlendMat[i]`.

The joints are pushed by the cull pass's `XMatrix` method `0x6ebe38`, which every transform class inherits unless a
more specific method is registered. It computes `world = 0x69c6ff(node+0x14 local, parent world)` into a stack
local, points `action+0x34` at it, visits the children (`0x6df3c7` on `node+0x18`), and restores the previous
pointer. It also XOR-hashes the matrix into `action+0x2c`. `XViewPlaneAlignTransform` (`0x972398`) gets a billboard
matrix instead.

### Measured in game (instance C, quick match) [V]

- **Bones are per worm.** Each worm has its own `XBone` objects: `bone0` (`main`) differed for every worm, and the
  translation of `bone0+0x70` matched that worm's position. Every `n=34` skin shape of one worm (the body and the
  hand shapes) points at the same `XBone` objects. One logged frame had 42 `n=34` shape visits, about two per worm.
  So the matrices of one worm can change without touching another.
- **The palette order is the 0.16 table.** `pose_logic.h`'s `kBoneNames` and `kPivot` are the order of
  `XSkinShape.bones`. `Identify` on the CPU matrices learned a centre height of **3.0** units (main pivot over the
  container position) from 36,000 unambiguous samples. Identification and the per-slot poses worked from the first
  frame after it.
- Other skinned props use `n=2` bone lists, so the `n == 34` check skips them.

## The worm rig [V]

The order is the `XSkinShape` bone list, which is also the matrix index on the CPU path (from
[gravespade-re-worm.md](gravespade-re-worm.md); `pose_logic.h` has the bind pivots):

| idx | bone | group | idx | bone | group |
|---|---|---|---|---|---|
| 0 | `main` (root, pivot y -0.8) | root | 17 | `wrist_left` | left hand pivot |
| 1 | `tail` | tail pivot | 18..24 | left fingers and thumb | left hand |
| 2, 3 | `tail2`, `tail3` | tail | 25 | `shoulder_right` | right hand |
| 4 | `head` (pivot y 1.0) | head pivot | 26 | `wrist_right` | right hand pivot, `WeaponLocator` |
| 5..11 | lips, mouth corners | head | 27..33 | right fingers and thumb | right hand |
| 12..15 | eyelids, eyebrows | head | 16 | `shoulder_left` | left hand |

The neck is the skin-weight blend band at bind y = -6..+2, with the cut at y = -2 (`kNeck`). The worm has no spine
chain: "body segments" means `main` (the whole worm) and `tail` -> `tail2` -> `tail3` (the lower body, down to
y = -12.8). `head` carries everything above the neck. A bend at the waist is a `tail` rotation, and a bend at the neck
is a `head` rotation.

## Hook plan [P], proven by the probe

Replace the "unsupported" branch of MiragePose with a CPU backend:

1. **Install.** Keep the existing slot-60 check (`0x790880`). When it reports CPU skinning, `mem::Expect(0x790cd0,
   8b 54 24 04 53 56 57 85 d2)` and `safetyhook::create_inline(0x790cd0, HkSkinShape)`. The function is cdecl
   `(iface, node)` and its return value is passed through, since `0x799980`-style callers test `eax`.
2. **Per call** (main thread, inside the frame's draw):
   - When nothing is live (`g_live == 0`), call the original at once: the cost is one branch.
   - Read `cont = *(node+0x48)`, `n = cont[+0x18]` and `bone[] = cont+0x20`. Skip unless `n == 34`.
   - Copy `bone[i]+0x70` into `pal[34*16]`. Run `RefreshWorms` (gamestate), `g_centre.Sample` and
     `lg::Identify(pal, feet, n, 26, centre)`. Go straight through unless that slot has a live pose or copy.
   - For a pose: `lg::Compose(pal, spec, out)`, write `out` into the 34 `bone+0x70`, call the original, record
     `wormBones`. For a copy (the severed head): `lg::ComposeCopy(pal, head, out)`, write it, call the original
     again (a second draw of the same shape). Always write `pal` back before returning.
   - The writes go to render-only memory that the cull pass rewrites every frame. Restoring the matrices keeps every
     other reader (a second shape of the same worm, a shadow or reflection pass, the next frame) exactly vanilla.
3. **Cache** (optional): remember `bone[0] -> slot` for the frame, so the other shapes of a worm skip `Identify`.
   Bones are per worm, so pointer identity is a stronger key than the 26-unit nearest-feet match once a frame has
   resolved it.
4. **Status.** `wum.render.wormPoseStatus()` returns `"untested"` until a draw is identified, then `"ok"`, the same
   as the GPU route. `wormPose`, `wormCopy` and `wormBones` keep their 0.16 Lua shapes.

**Net safety.** Only `bone+0x70` changes, inside one draw call, and it is restored before the call returns.
`bone+0x70` is a render matrix that the cull pass rebuilds from the animation each frame. The simulation, physics,
Wormsign hashes and the animation scheduler never read it, so the pose cannot desync. Prone and mount stay purely
visual. The sim keeps `worm:anchor` as it is.

### Probe used for the proof (reverted)

The probe was about 70 lines added to `src/render/mirage/pose.cpp`: the hook above, the install branch and a status
tweak. Melange was built with `-Out gsmcpu` (private), deployed to instance C, and then reverted. The diff is kept at
`<session scratchpad>/gsm_shots/cpu-skin-probe.diff`. Its core:

```cpp
int __cdecl HkSkinShape(uintptr_t iface, uintptr_t node) {
    uintptr_t cont = *(uintptr_t*)(node + 0x48);
    if (!g_live || *(uint32_t*)(cont + 0x18) != 34) return g_hSkin.ccall<int>(iface, node);
    uintptr_t* bone = (uintptr_t*)(cont + 0x20);
    float pal[34 * 16], out[34 * 16];
    for (int b = 0; b < 34; ++b) memcpy(pal + b * 16, (void*)(bone[b] + 0x70), 64);
    // Identify -> slot; Compose(pal, spec, out); write out; r = original(iface, node);
    // ComposeCopy for a severed head and a second original call; write pal back; return r.
}
```

### Live proof (instance C, 2026-11-18) [V]

The screenshots are `gsm_*.png` in `WUMFix\testenv\C\Melange\m1t\`, with copies in the session scratchpad
`gsm_shots\`. Every pose was set for slots 0..7 through `pose.test` or
`lua.eval @kindjal wum.render.wormPose(...)`:

| Shot | Pose | What is on screen |
|---|---|---|
| `gsm_base` | none | The active worm stands behind a ledge (camera behind it). |
| `gsm_prone` | `pose.test prone` (root pitch -90) | The body tips over backwards about the main pivot. The hat and glasses stay where they were. |
| `gsm_mount` | `pose.test mount` (root pitch 35 and +6 up, both hands pitched -60) | The worm leans forward and up, and the hands swing. |
| `gsm_copy` | `pose.test copy` (stump + head copy) | The body is a flat-topped stump. The worm's own head (real texture, eyes and mouth) is drawn again 30 units to the side, rolled 90 degrees. |
| `gsm_face_base` / `gsm_bend` | `wormPose{head={rot={0,0,55}}, tail={rot={40,0,0}}, rightHand={move={0,22,0}}, leftHand=...}` | The worm faces the camera. The head rolls 55 degrees to the side and the lower body bends. |

`wormPoseStatus()` returned `ok`. Nothing else in the frame changed (other worms with no pose, props, n=2 skins).
The Steam save and the replays folder were byte-identical afterwards. The C build, ini and pdb were restored.

## Attachments and the held weapon

Moving the bones does **not** move rigid attachments: the hat, moustache and glasses on `HatLocator` and
`TashLocator`, and the weapon mesh on `WeaponLocator` (under `wrist_right`). The screenshots show this: the hat stays
where the unposed head was. They are ordinary `XShape`s under locator groups under the joints, placed by the cull
pass's matrix stack (`0x6ebe38`), not by `bone+0x70`. On the draw side the `XShape` method loads `action+0x2c` as
the model-view **[I]**. Three options:

1. **Spade (needed) [P]:** keep the 0.16 plan. The Grave Spade's held mesh is invisible, the client draws the spade
   with `wum.draw.mesh` at the posed `wrist_right` frame from `wum.render.wormBones(slot)`, and the hand edits grip
   it. That needs no engine work beyond this hook.
2. **Hat on a posed or decapitated worm, quick [P]:** hide it. For the execution the victim's hat can come off with
   the head copy, and on the attacker a hat that stays in place reads as a "follow" error only under large root
   pitches. Use the engine mask `ActiveWorm_UnmaskAccessories` / `m_nHatMask` (`0x5a43d0`, `0x5a17d0`) **[U]**, or
   find the hat's `XShape` under the head joint (below) and skip its draw.
3. **Full follow, the complete route [P][U]:** move the pose up one level, into the cull pass. Replace the
   `XCullSortAction` `XMatrix` method `0x6ebe38` for a posed worm's joints with `world' = local x parentWorld x D`,
   where `D` is the joint's world delta. Children inherit it through the stack, so the bones (`bone+0x70`), the hat,
   the glasses and the engine-held weapon all follow, and the draw hook becomes unnecessary. What is open:
   - mapping joint nodes to bone indices. In the XBone method `0x6ebede`, `action+0x34` points at the owning joint's
     world, which is the stack local of that joint's `0x6ebe38` frame, so a replacement can record "joint J owns bone
     b" on the first pass;
   - whether the `XAction` base methods for `XTransform` (`0x6e00f5`) and `XJointTransform` (`0x6dfce0`) or
     `0x6ebe38` win the dispatch for joints in the cull pass;
   - whether the hat matrix is captured at cull time (the sorted render list) or recomputed in the draw.

   Deltas composed at joint level are the same `D_i` that `Compose` builds, so `pose_logic.h` still applies.

## Addresses (build #1077)

| What | Address |
|---|---|
| XOglDrawAction method records (`XOglDrawAction_Methods`) | `0x8b26f0` (16-byte records) |
| `XSkinShape` draw method (**hook**) | `0x790cd0` (cdecl `(iface, node)`; prologue `8b 54 24 04 53 56 57 85 d2`) |
| `XShape` draw method | `0x790af0` |
| `XGeoSet` / `XIndexedGeoSet` / `XWeightSet` draw methods | `0x799800` / `0x799980` / `0x7993f0` |
| `XCoord3fSet` / `XNormal3fSet` draw methods (CPU skin) | `0x79a930` / `0x79ac70` |
| Palette blend: positions / normals; plain weight-set blend | `0x7c57e0` / `0x7c5990`; `0x7c5e40` |
| `XPaletteWeightSet` cast | `0x799350` |
| XCullSort `XBone` method (writes `bone+0x70`) | `0x6ebede` |
| XCullSort `XMatrix` method (joint matrix stack) | `0x6ebe38` (world at `ebp-0x40`, stack pointer at `action+0x34`) |
| Matrix multiply `out = a x b` (this = out) / local x parent | `0x69c6ad` / `0x69c6ff` |
| Child visitor | `0x6df3c7` |
| Draw action fields | `+0xbc` weight set, `+0xc0` `XBone**`, `+0xc4` bone count, `+0x2c` model-view, `+0x6c` view-only flag, `+0x3d4` blend scratch vector |
| `XSkinShape` fields | `+0x48` bone-list container (`+0x18` count, `+0x20` `XBone*` array) |
| `XBone` fields | `+0x30` inverse bind (`PoseMatrix`), `+0x70` skin matrix (`inverseBind x world`) |
| Class objects: `XSkinShape` / `XSkin` / `XBone` / `XPaletteWeightSet` / `XJointTransform` | `0x972810` / `0x9727b8` / `0x972868` / `0x971f78` / `0x972340` |
| Class id | `class+0x20 & 0x3ff` (assigned at start), mirrored at `node+6` |
