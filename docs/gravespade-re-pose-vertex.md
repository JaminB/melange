# Grave Spade RE: the CPU-skinned vertex path (angle "vertex")

> **Status:** the route recommended here (the `0x790cd0` palette detour) shipped in Melange 0.16.0 (option M); options V and C
> were not built. These are the research notes written before and during the
> work; the API as built is documented in [lua-api.md](lua-api.md), and where the two differ lua-api.md is right.

These notes follow [gravespade-re-worm.md](gravespade-re-worm.md). That document found that build #1077 skins worms
on the CPU (`XOglDrawAction` slot 60 `0x790880` drops every `Skinned` vertex program). This angle was the fallback:
if the bone matrices cannot be reached, find the CPU-skinned output vertex buffer per worm, and work out whether we can
deform it after skinning (bend the body, hide the head) or draw a full second copy of the worm with our own transforms.
It also measures the cost per frame.

The work was static only: disassembly with capstone, the Ghidra export, `xomtool` on `Bundl474.xom`, and an offline
benchmark of the game's skinning loop on the real `W4.Worm` data. The game was not launched.

Tags follow [meshes.md](meshes.md): **[V]** read from the disassembly and consistent with the data, **[I]** inferred,
**[P]** proposal, **[U]** unknown.

## Verdict

- **The bone matrices *are* reachable, at draw time, per skinned shape [V].** On this build the "palette" is a list of
  34 `XBone*` on the `XSkinShape` node. Each bone's skinning matrix is a plain 4x4 float at `bone+0x70`, which equals
  `PoseMatrix x World` (row vectors), so it is exactly the `BlendMat[i]` that `pose_logic.h` already composes. The GPU
  route's maths carries over unchanged. Only the hook point moves from the ARB register upload to `0x790cd0`. That is
  the route to build: the other angle's probe (`HkSkinShape` in `pose.cpp`) is on it already.
- **A post-skin output buffer exists too [V].** It is one shared scratch `vector<float3>` per draw action for
  positions and one for normals. It is filled per skinned shape and handed to GL as a client-memory array
  (`glVertexPointer`/`glNormalPointer`, no VBO), and the draw follows inside the same handler. It can be edited after
  skinning by wrapping the two skinning functions, which have clean cdecl signatures and get the per-vertex weights and
  bone indices as arguments.
- **A full second copy of the worm is cheap and uses the engine's own draw [V]/[I].** We call the `XSkinShape` method
  (or its body `0x790af0`) a second time with another palette. That draws the real worm: textures, eyes, team colours,
  Sunstone. A client-side reimplementation is not needed.
- **Cost:** composing a palette is about 1.5 us per posed worm. The vertex post-pass is about 22 us per worm. A full copy
  is about 15 to 45 us of skinning plus about 8 draw calls. The engine already spends about 15 to 45 us per worm per
  frame skinning on the CPU. All of these are below 0.1 ms per frame (see [Cost](#cost-per-frame)).

So the mount is feasible on #1077. Use **matrix patching at `0x790cd0`** for prone, mount, bend, hands and the head stump.
Use the **second draw** for the severed head (wormCopy). Keep the **vertex wrappers** as an optional extra, for effects that
matrices cannot express, such as a cut plane that follows the blade instead of the bind-pose neck band.

## How #1077 skins a worm [V]

### Draw-action dispatch

`XOglDrawAction` (vtable `0x8b319c`) dispatches per node class through a static `{class GUID*, handler, 1, 0}` table at
`0x8b26f0`. A class's GUID is the 16 bytes just before its `XClassDescriptor` (for example, `XSkinShape`: descriptor
`0x88db2c`, GUID `0x88db1c`). The entries that matter:

| Class | Handler | What it does |
|---|---|---|
| `XShape` | `0x790af0` | Draws one shape: releases cached state refs (`+0x7bc`, `+0x1c0/+0x1c4`), reloads the modelview `+0x2c` if `+0x6c` is set, runs the generic shape traversal `0x6efde7` (shader, then geometry), and calls the `+0x39c` callback |
| `XSkinShape` | **`0x790cd0`** | Sets up skinning, then calls `0x790af0` (below) |
| `XWeightSet` (and `XPaletteWeightSet`) | `0x7993f0` | `action+0xbc = weightSet` (0 if it is empty) |
| `XCoord3fSet` | **`0x79a930`** | Skins the positions if `+0xbc != 0`, otherwise binds the static VBO |
| `XNormal3fSet` | **`0x79ac70`** | Skins the normals the same way |
| `XIndexedTriangleSet` | `0x79a460` | Walks the geo-set children (`0x799980`), applies state, then **`glDrawElements`** |

The `XCullSortAction` method table is at `0x8951e8` (RTTI name `XCullSortAction_Methods` at `0x8951cc`). Its
**`XBone` method `0x6ebede`** runs `bone+0x70 = Mul(bone+0x30, *action+0x34)`. `0x69c6ad` is `out[r][c] = sum A[r][k]
B[k][c]`. So `bone+0x70` = `PoseMatrix x currentWorld`, recomputed every cull pass. The bin entries made by
`0x6ee179` keep the shape node and a snapshot of the world matrix, not the bones. The draw therefore reads the bone
matrices live from the nodes. That only works if every worm has its own skeleton nodes, so the scene graph is
per-instance (**[I]**, strongly implied, since all worms animate independently).

### `0x790cd0`: the XSkinShape method (cdecl `(iface, node)`, returns `0x790af0`'s result)

```
self = iface ? iface - 4 : 0
arr  = *(node + 0x48)                 ; XSkinShape "Bones" (34 XBone* for every W4.Worm shape)
saved = self[+0x7d0]; self[+0x308] &= ~1
self[+0xc4] = *(arr + 0x18)           ; bone count
self[+0xc0] = arr + 0x20              ; XBone* array
self[+0x7d0] = 0
r = 0x790af0(iface, node)
self[+0x308] |= 1; self[+0x7d0] = saved; self[+0xc0] = 0
return r
```

The prologue is `8b 54 24 04 53 56 57 85 d2`, a clean 5-byte boundary for an inline detour. `XSkinShape` objects are
0x4c bytes (create function `0x6d12ef`, ctor `0x6cc6cc`, vtable `0x88f670`), so `+0x48` is the last field.
`XBone` objects are 0xb4 bytes, with `+0x30` PoseMatrix (4x4) and `+0x70` the skin matrix (4x4, to `+0xb0`).

### `0x79a930`: skinning the positions

When `action+0xbc` is an `XPaletteWeightSet` (the `0x799350` cast, class `0x971f78`), which is true for every worm shape:

1. `action+0x3d4` is a `vector<float3>` scratch pool. `0x79a740(count)` reserves `count` entries and `0x7a4310`
   returns its begin. **That pointer is the CPU-skinned output buffer.** It is shared by every skinned shape the action
   draws, and it is only valid until the next skinned shape.
2. A `const float*[count]` array is built on the stack (`_alloca` `0x6fee20`) as `action+0xc0[i] + 0x70`.
3. `0x7c57e0(coords, wpv, weights, indices, mats, out, count, 12, 12)` is called with:
   - `coords` = coordSet`+0x1c`→`+0x20`, with the count at `+0x18`;
   - `wpv` = u16 weightSet`+0x20` (`WeightCount`, which is 3 for the worm);
   - `weights` = weightSet`+0x1c`→`+0x20` (float[wpv] per vertex);
   - `indices` = weightSet`+0x24`→`+0x20` (u8[wpv] per vertex);
   - the last two are the input and output strides.
4. `glLoadMatrixf(action+0x70)` loads the **view** matrix (vtable slot 15 `0x791020` returns `action+0x70`), because the
   output is in world space. It sets `+0x6c = 1` so `0x790af0` reloads the modelview `+0x2c` afterwards.
5. `glVertexPointer(3, GL_FLOAT, 0, out)` (IAT `0x815444`) is called **with no buffer bound**, so GL reads the client
   memory at the draw.

With a plain `XWeightSet` the dense variant `0x7c5e40(coords, nMat, weights, mats, out, count)` runs instead. Worms do
not use it.

`0x79ac70` is the same for normals: pool `action+0x3ec`, `0x7c5990` with the identical 9-argument signature, and
`glNormalPointer(GL_FLOAT, 0, out)` (IAT `0x815448`). Without a palette weight set it disables the normal array.

### The skinning loop (`0x7c57e0`)

This is x87 and per vertex:

```
o = (v * M[i0]) * w0                       ; the first weight is always applied
for k in 1..wpv-1: if w_k != 0: o += (v * M[i_k]) * w_k
v * M = (m0 x + m4 y + m8 z + m12, m1 x + m5 y + m9 z + m13, m2 x + m6 y + m10 z + m14)   ; row vector, row 3 = translation
```

This is the same convention as `pose_logic.h` (`BlendMat'[i] = BlendMat[i] x D_i`), so `Compose`/`ComposeCopy`
produce matrices that this loop consumes as they are. `0x7c5990` uses the upper 3x3 and loads a constant from
`0x8c1578`, probably to renormalise (**[I]**).

## The worm's skinned geometry [V]

The data comes from `xomtool unpack Bundl474.xom`, `W4.Worm`. The three `XSkin`s share one skeleton (`#70 main`). Every
`XSkinShape` lists the same 34 `XBone`s in the palette order of `pose_logic.h`, and every weight set is an
`XPaletteWeightSet` with `WeightCount = 3`.

| Shape | Verts | Tris | Non-zero weights per vertex |
|---|---|---|---|
| `boggybody_shader` | 567 | 1012 | 1.61 |
| `boggyhand_left` / `_right` | 260 each | 428 each | 1.51 / 1.49 |
| eyes (`Boggylefteye`, `boggyrighteye`, with colour/anim-texture variants under `XBinModifier`/`$animTex`) | 25 each | 32 | 1.16 |
| teeth (`boggyteeth_shader_0..2`, anim-texture frames) | 25 each | 28 | 1.96 |

A drawn worm is about **1162 skinned vertices in 6 to 8 skinned draws**: the body, two hands, the visible eye variants and
one teeth frame. Which eye and teeth variants are drawn in a given frame is **[U]**.

## Option M: matrix patching at `0x790cd0` [V] hook point, [P] use

This is the recommended route. Detour `0x790cd0`. For a shape whose `node+0x48` count is 34, read the 34 matrices,
identify the worm, then compose with `pose_logic`. There are two variants:

- **In place** (the current probe in `pose.cpp`): write the posed matrices into `bone+0x70`, call the original, and
  write the saved ones back. This is safe because the cull pass rewrites `+0x70` every frame and nothing between this
  call and the draw reads them for anything else. All three skins of a worm share the bones, so restore after every
  shape.
- **Without touching engine memory**: do not call the original. Run its eight instructions ourselves, but set
  `self+0xc0` to our own array of `const void*` = `&posed[i] - 0x70`. Both `0x79a930` and `0x79ac70` add `0x70`, so
  positions and normals both pick up the pose, then call `0x790af0(iface, node)`. Nothing needs restoring, and an
  exception in the draw leaves no bent bones behind.

Either variant gives everything the mount needs. The body, eyes, teeth and hands all follow, and the normals follow
because the same matrices feed `0x7c5990`:

- the whole-body prone or mount transform (a root delta);
- the bend (head and tail chain);
- rigid hand targets (the hand subtrees);
- the head stump (`S(sx,0,sz)` about the neck, applied to the head subtree).

**Identification.** Each worm has its own `XBone` nodes (**[I]**), so `node+0x48` (or `bone[0]`) is a stable key per
worm instance. Identify once by `PivotWorld(pal, 0)` against the worm positions, then cache key to slot and re-check it
every few frames.

**Hide the original.** For a shape of a hidden worm, return 0 without calling `0x790af0`. Nothing is drawn, and no
degenerate triangles are sent.

**wormCopy (the severed head).** After the normal draw, call `0x790af0(iface, node)` again with the `ComposeCopy`
palette: the head subtree at the rolling-head transform, and every other bone collapsed onto the neck. `0x790af0` is the
ordinary per-shape method and re-applies the shader and state on each call, so a second call is just one more shape.
Every pool and pointer it uses is rebuilt per call (**[I]**: it runs once per shape for every shape in the frame
anyway). A **full** copy (the whole worm with our own transform, while the original is hidden) is the same call with
`P'_i = P_i x T` on all 34 bones.

**What does not follow.** Rigid attachments are `XShape`s under locator `XGroup`s (hat on `HatLocator`, weapon on
`WeaponLocator`, moustache on `TashLocator`). The cull pass places them with their own world snapshot, and they are
drawn by `0x790af0` with the modelview at `action+0x2c`. To move or hide them, hook `0x790af0` for shapes where
`self+0xc0 == 0` and the modelview translation is within 0.5 units of a posed worm's locator. Then hide them, or
reload `glLoadMatrixf(M x V^-1 x D x V)` with the view `V` at `action+0x70`. The spade plan already hides the held
weapon and draws the spade itself.

## Option V: post-skin vertex deformation [V] hook points, [P] use

This is the angle's original question, and it is available. Wrap the two skinning functions, which have only one caller
each (`0x79aa46` and `0x79ad86`):

- `0x7c57e0` (positions): the prologue is `83 ec 14 83 7c 24 30 00`;
- `0x7c5990` (normals): the prologue is `83 ec 18 83 7c 24 34 00`.

Both are cdecl, 9 arguments, no return value. The wrapper calls the original, then edits `out[0..count)` in place. At
that point it has:

- **which worm**: `mats[0] - 0x70` is `bone[0]`, the same per-instance key as in Option M;
- **world-space positions or normals** of every vertex of the shape;
- **per-vertex weights and bone indices** (`weights`, `indices`, `wpv`), so "how much of this vertex is head" is exact:
  `sum w_k` for `i_k` in 4..15. A height threshold would only approximate it.

The buffer is safe to edit because the draw (`glDrawElements` in `0x79a460`) follows with nothing skinned in between.
The next skinned shape reuses the pool, so do not keep the pointer.

What it is good for, beyond matrices:

- a **cut plane** in world space at the blade (the chop line follows the spade's angle, not the bind-pose neck band).
  Project the vertices on the head side onto the plane for the stump. The ghost draw projects the other side, which
  makes the head's own cap;
- a smooth **arc bend** by distance along the body axis. The spine has only `main`, `tail`, `tail2`, `tail3` and
  `head`, so per-bone bends kink a little;
- squash on impact, and wobble or gore pulses.

What it is worse at: lighting. Normals must be rotated by the same per-vertex rotation, which is cheap for a bend but
wrong for a projection, so the stump normals look flat-shaded. It also needs both wrappers to agree on the same
per-vertex function. Prone, mount and the hands should stay in Option M, because a matrix moves everything consistently,
normals included.

## Option C: our own full client copy [I], not recommended

Melange could skin `W4.Worm` itself: the bind coordinates, weights and PoseMatrices are all in `Bundl474.xom`, and the
live `bone+0x70` gives this frame's pose. It would then draw with its own GL. But it would have to reproduce the shaders,
team colours, the animated eye and teeth textures, and Sunstone, so it is strictly worse than calling `0x790af0` a second
time.

## Cost per frame

An offline benchmark (a throwaway, not committed) reimplements `0x7c57e0`/`0x7c5990` exactly, x87 build `/arch:IA32`
like the game. It runs on the real `W4.Worm` data: the body, two hands, two eyes and one teeth set, 1162 vertices, with
real weights and indices and the 34 real PoseMatrices x a world matrix. Ryzen 7 7800X3D, one core, averaged over
20000 runs after a 1 s warm-up.

| Work (one worm, one draw pass) | x87 (the game's code) | SSE2 (Melange's build) |
|---|---|---|
| The engine's own CPU skin (positions + normals), which it already pays every frame | 45 to 48 us | 15 us |
| Option M: compose 34 bones | 3.5 to 4 us | 0.7 to 1.4 us |
| Option M, in place: swap and restore 34 x 64 B | 0.1 us | 0.05 us |
| Option V: post-skin bend + neck collapse on positions + normals (a sin/cos per vertex) | 63 to 76 us | 22 us |
| wormCopy: a second engine skin of the whole worm (the ghost head or a full copy) | 45 to 54 us | 15 us |

Each draw also has GL overhead. A copy adds 6 to 8 `glDrawElements` calls on client arrays (about 1162 x (12 + 12 + 8)
B, roughly 37 KB per worm), a few microseconds per call on a desktop driver. That is **[I]**: it can be checked with
`gltrace.gpu` in game.

For the mount shot, that is two posed worms plus one ghost head: about 2 x 1.5 us + 1 x (15 to 45 us + about 8 draws),
**well under 0.1 ms per frame**. Option V adds about 22 us per worm that uses it. A vertex route without sin/cos (with
the angle per bind vertex precomputed once) would be about half that. The base game spends about 0.5 to 0.7 ms per frame
skinning a 16-worm match, so none of the options is significant.

## Net safety

Every hook here is in the GL draw action, on the render thread, during the draw:

- `0x790cd0`, `0x790af0`, `0x7c57e0` and `0x7c5990` change only the matrices handed to the draw or the client vertex
  array GL reads.
- The in-place variant writes `bone+0x70`. It is restored before return, and the cull pass recomputes it every frame
  from the joints anyway.
- Nothing in the sim reads `XBone+0x70` or the skin scratch pools. The only writer of `+0x70` is `0x6ebede`, and the
  only readers are the two skin handlers, through `action+0xc0` (**[V]** for the handlers in the draw-action table;
  **[I]** that no other code path reads them, by the class-method structure).
- Positions, physics, Wormsign and AI never see a pose.
- Sim-side positioning (`worm:anchor`) is unchanged by this route.

## Open points [U]

- Which eye and teeth variants are drawn per frame. They all carry the same palette, so the pose does not care.
- Whether the worms are also drawn in another pass (water reflection, shadow) through the same draw action. If so, the
  hooks apply there too, which is what we want.
- The exact draw order of a worm's shapes relative to its attachments, which matters for the locator-matching hook.
- In game: confirm per-instance `XBone` nodes. Two worms must show different `node+0x48` and `bone[0]` addresses; the
  `[pose.probe]` log in `pose.cpp` prints exactly this.

## Addresses (build #1077)

| What | Address |
|---|---|
| `XOglDrawAction` method table `{GUID*, fn, 1, 0}` | `0x8b26f0` |
| `XShape` / `XSkinShape` / `XWeightSet` methods | `0x790af0` / `0x790cd0` / `0x7993f0` |
| `XCoord3fSet` / `XNormal3fSet` methods (CPU skin) | `0x79a930` / `0x79ac70` |
| `XIndexedTriangleSet` method (`glDrawElements`) / geo-set walk | `0x79a460` / `0x799980` |
| Palette skin, positions / normals / dense positions | `0x7c57e0` / `0x7c5990` / `0x7c5e40` |
| `XPaletteWeightSet` cast / class | `0x799350` / `0x971f78` |
| Scratch reserve / begin | `0x79a740` / `0x7a4310` |
| Draw action: modelview / reload flag / view / weight set / bone array / bone count | `+0x2c` / `+0x6c` / `+0x70` / `+0xbc` / `+0xc0` / `+0xc4` |
| Draw action: position pool / normal pool / flags | `+0x3d4` / `+0x3ec` / `+0x308` bit 0, `+0x7d0` |
| `XCullSortAction` methods / `XBone` method (`+0x70 = +0x30 x world`) | `0x8951e8` / `0x6ebede` |
| `XSkinShape`: class / create / ctor / vtable / Bones array | `0x972810` / `0x6d12ef` / `0x6cc6cc` / `0x88f670` / `+0x48` (count `+0x18`, data `+0x20`) |
| `XBone`: class / size / PoseMatrix / skin matrix | `0x972868` / `0xb4` / `+0x30` / `+0x70` |
| Matrix multiply (row-major, `A x B`) | `0x69c6ad` |
| IAT `glVertexPointer` / `glNormalPointer` / `glLoadMatrixf` | `0x815444` / `0x815448` / `0x8153f0` |
