# Caldera's detail layer

The detail layer adds decoration to Caldera v2 (the volcano, its four castles, trails and shore) that changes nothing in the
simulation. It covers rock detail on the volcano (outcrops, crowns, coast stacks, talus fans, strata ledges) and then flora
(clumps zoned by height, from the green shore to the ash rim). It is a separate layer: `detail.mjs` runs **after**
`build.mjs` on the saved project. It never edits `build.mjs` or `terrain.mjs`, so the map and gameplay work (branch
`erg-mcp`) and this layer (branch `erg-detail`) reconcile by running one script after the other.

## What it does

`detail.mjs` runs these passes in order (`PASSES`; `--passes a,b` runs a subset):

| Pass | File | What it places or changes |
|---|---|---|
| flags | detail.mjs | Removes build.mjs's four Camelot flag copies. They stood inside the volcano and the PREHISTORIC theme drew them as purple pods. |
| land | detail-land.mjs | The cone's two basalt texture tints (P05 `#3b3531`, P07 `#46403b`). The pale side-face slivers are gone. |
| lawn | detail-lawn.mjs | Pulls the lawn tiles' textures to a muted olive green (P01-P04). |
| rock | detail-rock.mjs | Hero walls, outcrops, ribs, skirts, scree, crater pieces and breakers (tag `_dtl`). |
| crown | detail-crown.mjs | Clusters on crests and cliff tops. |
| coast | detail-coast.mjs | Shore stacks, steps and the coast talus gradient. |
| offshore | detail-offshore.mjs | Removes offshore masses that do not touch the shore, except big stacks. |
| talus | detail-basalt.mjs | Talus fans at the slope break below cliffs (`plan_slope_break`, tag `_tal`): toe, body, apex and skirt pieces, none on ground over 50 degrees. |
| ledge | detail-basalt.mjs | Off since round 11 (`DTL_LEDGE_MAX=0`): the ledges read as walkable platforms beside the trails. |
| columns | detail-basalt.mjs | Basalt columns on cliff tops (`caldera.Columns`, tag `_col`). On Caldera every candidate falls in a keep-out, so 0 are placed. |
| slab | detail-slab.mjs | Off (`DTL_SLAB_MAX=0`). |
| retint | detail-retint.mjs | One rock tint per elevation band on every rock copy (per-copy `_tint<rrggbb>`, Sunstone `mg_tint`). |
| support | detail-support.mjs | Removes rock clumps hanging off steep faces with no ground or talus under them. |
| flora | detail-flora.mjs | Plant clumps zoned wall / shore / green / lawn / cliff / rock / dry / ash, with a lush-to-hardy height ramp (tag `_flo`). |
| lint | detail-lint.mjs | Removes mine look-alikes, hovering props and buried props. |
| perch | detail-perch.mjs | Removes clumps perched with a visible gap under them from the fixed cameras. |
| alone | detail-alone.mjs | Removes plants that stand alone. |
| basalt | detail-basalt.mjs | Re-points every remaining HORROR5 copy at the matte `caldera.Boulder` and removes the small stones left on faces over 50 degrees. Round 11: the swapped rocks take the faceted variants by proportion, and no `caldera.*` copy keeps a tint tag. |
| strings | detail-r11.mjs | Vertical or climbing strings of rock (a chain that climbs 3 or more, mostly on ground over 44 degrees) keep only their lowest piece. |
| beach | detail-r11.mjs | Removes rocks under 1.3 across from beach ground (under 4.5 high, under 20 degrees). |
| tumble | detail-r11.mjs | The shore's hero stacks (pieces over 2.0 across, under 6 high, near the sea) become graded tumbles on the waterline: one 2.0 block, a ring of 1.4-1.8, an outer ring of 1.0-1.3 and one block on top. No piece is over 2.0, and none stands on ground over 46 degrees. |
| rim | detail-r11.mjs | Crater rim breakup: clusters of shards (scoria), flat slabs (spatter) and small blocks on the crest band, outside the pads, the trail ends and the lava. |
| flora2 | detail-r11.mjs | Flora at the switchbacks' fill side, hedge rows straddling the lawn tiles' edges, scrub in the dry and ash belts, and a shore fringe. |
| lint11 | detail-r11.mjs | The mine look-alike lint again, over the round's rock. |

**Final layer (round 11):** 582 props (`_dtl` 196, `_tal` 61, `_ldg` 0, `_flo` 325), 694 level details in all. The rock is 257
copies of the four faceted meshes, none tinted. Round 10 had 459 props (`_dtl` 160, `_tal` 82, `_ldg` 34, `_flo` 183).

**Keep-out** (`detail-keepout.mjs`): no prop on a trail or its kerbs, a castle court, a spawn, a launcher pad, the mine
factory or lava. Scenery copies have no collision, so nothing may stand where worms walk.

**Caldera's own meshes** (`meshes/gen.mjs`, `meshes/build.mjs`): the rock is four faceted, flat-shaded convex hulls of
78-126 vertices: `caldera.Boulder` (a block), `caldera.Slab`, `caldera.Wedge` and `caldera.Shard` (sections 486, 489, 490
and 491). `caldera.Ledge` and `caldera.Columns` are still built but are not shipped, because no copy uses them. Each mesh
has a 128 x 128 texture, is bundled with the shader of the game's HORROR5, and has its material made matte (specular and
Fresnel set to 0).

Since round 11 the warm albedo is **baked into the texture**. Block and Slab are a dark warm brown near P07, Wedge and Shard
are near P05. The top faces carry ash dust, and the sides grow dustier toward their tops. Earlier rounds used a neutral grey
texture with a per-copy tint (`mg_tint`). That tint needs the `sunstone-tint` shaders, and the released Sunstone (2.1.0) does
not have them, so the stones drew blue-grey. A baked albedo draws the same with or without any tint.

The MCP tools the layer uses are in new files under `tools/erg-mcp/src/` (dress, scatter, cluster, artpass, tint, landtint,
ledge, meshbank, propcheck, kitbash, budget, fingerprint and their `lib/` helpers). Each is registered in one line.
`docs/erg-mcp.md` documents them at its end. No existing tool behaves differently:
- `place.ts` `export` gained an optional `melangeRange` and the ship gate.
- `render_view` gained an optional `scenery` flag.

## How to run it

The game in the target folder must be closed. MELANGE_GAME_DIR defaults to testenv D.

```
set MELANGE_EXE=<checkout>\build\x86-release\Melange.exe
tools\node\node.exe tools\erg-mcp\bench\caldera\build.mjs --out <buildOut>          # the undressed map (erg-mcp's work)
tools\node\node.exe tools\erg-mcp\bench\caldera\detail.mjs --out <dir> --build-out <buildOut>
tools\node\node.exe tools\erg-mcp\bench\caldera\detail-verify.mjs                   # keep-out and sunk-prop check of the saved project
tools\node\node.exe tools\erg-mcp\bench\caldera\meshes\build.mjs <bankDir>          # caldera.Boulder / Ledge / Columns banks
tools\node\node.exe tools\erg-mcp\bench\caldera\detail-capture.mjs --mode game --url <test game url> --prefix <p> --out <dir> --start
```

- `detail.mjs` refuses a project that already holds the layer. Rebuild it with `build.mjs` first.
- Env knobs: `DTL_BUDGET` (default 820 details), `DTL_CLIFF=1` (round 5-9 cliff piles), `DTL_SLAB_MAX`.

**Shipping:** without the mesh banks, the copies of the `caldera.*` meshes draw nothing. The level still loads, but the rock
layer is missing. Since round 11, `ship.mjs` writes the dressed pack. Run it after `detail.mjs`, with the game closed:

```
tools\node\node.exe tools\erg-mcp\bench\caldera\ship.mjs [--out <dir>] [--version 2.1.0]
```

It does four things:
1. It exports the dressed project. The export's ship gate raises `melange.range` to `>=0.14.0`, because 694 details are
   over the 450 that the vanilla 550-instance mesh pool loads. The pool is raised by `[Fixes] MeshInstances`.
2. It builds the banks and merges the meshes the level names into ONE bank, `assets/meshes/caldera.Rocks.xom`
   (`meshes/merge.mjs`; until 2026-11-10 it shipped one bank per mesh, 8 in all). It refuses to continue if the level
   names a mesh the pack would not ship.
3. It lists that bank under `meshes` in `spice.json`.

One bank per map: every bank a pack lists takes one of the 44 mod mesh sections all enabled mods share, and Caldera's 8
banks with Kindjal's 38 left Kindjal's SlugGun and StoneDonkey unloaded. The mesh names (`caldera.Boulder`, ...) are
unchanged, so the level's copies still name them. `ship.mjs --banks-only` re-ships only the banks into the committed
pack (no export; the patch stays byte-identical, so `sim_fingerprint` cannot change).
4. It writes `.gitignore` as `assets/levels/`, so the banks are committed and the level builds are not.

The committed `pack/` on this branch is now the dressed pack. A merge from `erg-mcp` that touches `pack/` is resolved by
running `build.mjs`, `detail.mjs` and `ship.mjs` again.

Two things a fresh install needs, both checked in testenv D on 2026-11-10:
- **Melange 0.14.0 or later.** On a Melange before 0.14.0 the compat check moves the pack to `Mods\.incompatible`.
  The in-game checks before the version bump used a copy of the pack with only `melange.range` lowered.
- **A built level.** A source pack whose `assets/levels` is missing is refused ("not built"). A refused pack is
  Incompatible, so its mesh banks do not load either. After `build.ps1`, the log shows `4 bank(s) loaded from 1 mod(s)`
  and the rock draws, with both the released Sunstone 2.1.0 and `sunstone-tint`.

With the released Sunstone, the landscape and flora tints (`mg_landTint`, `mg_tint`) do nothing, so the lawns and plants
show their own greens. The rock does not depend on either tint.

## Neutrality proofs

What counts as SIM: voxels, drape corners, objects, spawn knots, water, the level script's sim behaviour, the mine factory,
and anything with collision. The layer only writes through these channels:
- scenery copies, which draw but have no collision
- per-copy tint tags
- the landscape texture tints, which are a visual table outside the voxels

`sim_fingerprint` (`src/lib/sim-fingerprint.ts`) hashes the sim and visual parts separately.

1. **Per pass.** `detail.mjs` fingerprints the project before the first pass and compares after every pass. A changed sim
   throws and nothing is saved. The project is then saved, reopened and fingerprinted again.
2. **Final reconcile run** (2026-11-10). `erg-mcp` at `3b84b13` is fully merged: the merge was "Already up to date", and
   `build.mjs` and `terrain.mjs` are byte-identical to `erg-mcp`. The map was rebuilt from scratch in testenv D.

| | sim | visual |
|---|---|---|
| undressed (build.mjs only) | `7b640e2825c535f3658cbbd5fcc875ad398db81170ecee2f1015c0198d2f0668` | `f46b5162aec90e9c…` |
| after all 18 passes | `7b640e2825c535f3658cbbd5fcc875ad398db81170ecee2f1015c0198d2f0668` | `8c0d3f6a35ae84e9…` |
| saved and reopened | `7b640e2825c535f3658cbbd5fcc875ad398db81170ecee2f1015c0198d2f0668` | as saved |

   - Counts are identical before and after (voxelFrames 593, solidVoxels 30394, simDetails 91, light 8, emitter 0,
     sound 7). Only `scenery` changes, from 4 to 465.
   - The saved project hash is `aab463b74b42c580`, the same as round 10's run: the layer is deterministic.
3. **The undressed level is erg-mcp's.** The rebuild re-exported `pack/`, and `git diff` of `pack/src/caldera.ergpatch.json`
   against `erg-mcp`'s committed export is empty. The only other differences were line endings in three pack files, which
   were reverted. So the fingerprint above is the fingerprint of `erg-mcp`'s undressed build.
4. **Channel proofs from earlier rounds** (docs/erg-mcp.md "Detail layer"):
   - Copies of base pieces carry no collision.
   - The material and texture tints do not change how voxels break: the landscape tint is a per-level texture table read
     only by Sunstone's shaders, and the voxels' material indices are untouched.
   - No emitters or lights are added.
5. **Readability:** `detail-verify.mjs` on the saved project reports 459 props, 0 keep-out violations and 0 flora
   violations.

**Round 11 run** (2026-11-10). `erg-mcp` at `92df935` (the scheme's sea, no level script or text, the courts' flicker fix) is
merged. `build.mjs` was rebuilt from scratch in testenv D, then `detail.mjs` ran all 24 passes:

| | sim | visual |
|---|---|---|
| undressed (build.mjs only) | `926ed2c04a9a4791d0d65d388b0739edb97873e610b93ae477797d083c696d2e` | `e1da4f8362e88316…` |
| after all 24 passes | `926ed2c04a9a4791d0d65d388b0739edb97873e610b93ae477797d083c696d2e` | `6fd4e87caad6d513…` |
| saved and reopened | `926ed2c04a9a4791d0d65d388b0739edb97873e610b93ae477797d083c696d2e` | as saved |

- Counts before and after: voxelFrames 595, solidVoxels 30505, simDetails 91, light 8, emitter 0, sound 7. Only `scenery`
  changes, from 4 to 588.
- `detail-verify.mjs` reports 582 props, 0 violations and 0 flora violations.
- The rebuilt undressed export differs from `erg-mcp`'s committed `pack/src` in 16 small values: a few detail heights by
  0.01-0.09, four voxel runs one or two cells longer, and some drape corners by up to 0.03. `build.mjs` and `terrain.mjs`
  are byte-identical to `erg-mcp`, so `build.mjs` is not exactly deterministic between runs.
- Castle flicker: the layer puts no plane or tint on a court. Rock and flora keep out of the castles, and the land tint is a
  texture colour, not geometry. `erg-mcp`'s `92df935` fixed the flicker in the skin under the courts.
- Perf (testenv D, Sunstone on, the same Test, testenv B's game running in both, so the numbers carry an overlap):

| | GPU frame p50 / p95 | fps p50 |
|---|---|---|
| undressed | 5.26 / 6.17 ms | 36.1 |
| dressed | 7.52 / 8.44 ms | 36.1 |

  The game is CPU-bound, so the frame rate is the same with and without the layer.

## Tests (final reconcile)

| Check | Result |
|---|---|
| `tools/erg-mcp/test.ps1` | 266 / 266 pass |
| `build.ps1 -PrivateDir` | Built |
| C++ selftests: erg_level, erg_pack, erg_preview, erg_scene, erg_voxels, meshbank, shaders, textags, xom_convert | All pass |
| `scripts/web/test.ps1` unit | Pass |
| `scripts/web/test.ps1 -Erg` | 34 / 34 pass |
| `scripts/web/test.ps1 -Panels` | 86 / 87 pass |

The one panel failure is `shell: layout and theme remembered`. `web/` is byte-identical to `erg-mcp`
(`git diff erg-mcp -- web` is empty). The test toggles to `light` and expects that to persist across a reload. The machine's
Windows theme is now light (AppsUseLightTheme=1), which most likely makes the toggle store "system" instead. The suite
passed 87 / 87 in rounds 6-8. This looks environmental and was not caused by this branch.

## The judge's final scores

A professional game-asset designer judged each round in game, with Sunstone on and the same cameras every round.

**Final, after round 14** (the polish run's goal was overall 6 or more; see "Final reconcile and check" at the end):

| rock | flora | cohesion | readability | technical | overall | sim neutral |
|---|---|---|---|---|---|---|
| 5.5 | 5.5 | 6 | 7 | 8 | 6 | yes |

The earlier verdict, after round 10, re-checked on the round-11 reconcile run (`caldet_r11_*`):

| rock | flora | cohesion | readability | technical | overall | sim neutral |
|---|---|---|---|---|---|---|
| 5 | 5 | 4 | 7 | 8 | 4 | yes |

What the judge said the layer does well:
- The pipeline: per-pass sim checks, keep-outs, and lint for contact, perch and support.
- The first in-game mod-mesh scenery.
- Clean courts, spawns, pads and trails.
- Good flora picks up close.

What is still open (the judge's next focus, not done here):
- **Rock:** the smooth 642-vertex `caldera.Boulder` does not match the faceted low-poly terrain and reads blue-grey.
  Rework it as 3-4 faceted, flat-shaded variants in the warm P05/P07 albedo.
- **Ledges:** the `_ldg` ledges read as walkable platforms or grates beside the trails. This is a readability defect:
  remove them, or rebuild them as thin flush strata.
- **Rock groups:** recompose the shore stack and Seawall's upper-right string as graded tumbles, with no piece over 2.0.
- **Talus and crater:** the talus fans must read from the flank camera, and the crater rim needs breakup.
- **Flora:** plant the fill side of the switchback bends and run hedge rows along the lawn checker edges. The dry and ash
  belts need 15-20+ clumps, for about 350-450 pieces in all.
- **Perf:** measure with testenv B idle. Round 9 measured +1.8 ms at 544 details.

Tool gaps the judge listed:
- `render_view` cannot draw mod meshes.
- There is no vertex colour or AO bake for mod meshes.
- There is no terrain-side edge blend for lawn cells.
- The trail keep-out has no cut-side or fill-side awareness.
- There is no perf sampler that is safe to run while another game runs.
- There is no faceted-rock generator.
- There is no emitter tool.

Evidence:
- Final run: scratchpad `caldet/r11/`:
  - `caldet_r11_contact_sheet.png`
  - `caldet_r11_pairs_before_after.png` (r10 undressed vs r11 dressed: the undressed level is the same)
  - `detail.log`, `dressed/detail.json`, `verify.json`
- Per-round evidence: `caldet/r0` to `caldet/r10`.

## Round 12 (dress)

Changes, from the round-11 judge's list (`detail-r12.mjs`, passes `chips`, `beachtum`, `scree`, `accent`, `lint12` after `lint11`):
- **Rock albedo:** the baked texture is about 1.8x brighter and warmer (`meshes/gen.mjs`: basalt7 #6c533c, basalt5 #5b4633, dust #988264). Round 11's values drew near-black in game. A first try at 2.3x drew pale grey-beige, so it was pulled back.
- **chips:** removes lone shore chips and a big flat slab. On this build it removed 0: the black chips were the outer ring of the round-11 tumbles, which now read as graded stones with the lighter albedo.
- **beachtum:** seated 7-piece graded tumbles (1.8-2.2 / 1.1-1.5 / 0.6-0.9) on beach ground: only 3 sites pass the keep-outs (4 pieces).
- **scree:** mid-scale rock groups (anchor 2.4-3.2 on steep ground, low flat-lying 1.0-1.8 stones on walkable ash) outside the trail band: 12 sites, 70 pieces. Almost all non-steep cone ground lies inside a trail's keep-out (1422 of ~4100 samples), so the flanks can hold few groups without breaking the readability rule.
- **accent:** 11 scrub and accent plants beside the scree groups (bush cores capped at 2.4 tall, only on slopes over 18 degrees).
- `columns` still places 0 (every candidate is in a keep-out); the talus placer places 61 pieces.

Run (testenv D, erg-mcp `92df935` rebuilt from scratch with `build.mjs --no-export`): sim `926ed2c04a9a4791d0d65d388b0739edb97873e610b93ae477797d083c696d2e` before, after all 29 passes and after save and reopen (31 `simSame` true, 0 false). `detail-verify`: 667 props, 0 violations, 0 flora violations. 779 details. Perf (testenv B running, so overlapped): GPU frame p50 / p95 8.70 / 9.97 ms (round 11: 7.52 / 8.44), 36 fps.
Shipping: `ship.mjs` rewrote the committed `pack/`; an install of the pack (range lowered to 0.13 as in round 11) logs `4 bank(s) loaded from 1 mod(s)` and draws the rock. Evidence: scratchpad `caldet/r12/`.

## Round 13 (dress: cohesion at the material level)

From the round-12 judge's next focus (`detail-r13.mjs`, passes `ground` after `lawn`; `toe`, `debris`, `darken`, `lint13` after `lint12`):
- **ground** (primary paint only, bits 2-7 of solid voxels: the fingerprint's visual `paint` part; never the second material or the corner mask).
  - *steep*: a draped skin cell whose steepest top triangle is over 50 degrees and whose material has a pale side tile (path02, grass01/02, rock02, GreySlateFloor: P05 / P07) takes VolcanoRock. These were the grey-blue wedge faces. The round-5 tint of P05 / P07 never reached players: it needs the `sunstone-tint` shaders, which the released Sunstone lacks. 344 cells. At 40 degrees the flank's warm dust aprons went dark too and the dust broke into square islands, so the threshold is 50.
  - *shore*: the checker under the castles goes. Flat basalt squares on the beach (under 6.5 high, under 30 degrees) become sand (49 cells). Grass and sand in the shore band are repainted from one fbm field: sand, olive grass02 patches, grass01 cores (57 to sand, 58 to grass02, 3 to grass01).
  - erg-mcp `paint_region` / `paint_path` masks gained the variable `s` (the column's steepest-triangle slope from its frame's corner offsets) for this.
- **toe**: rock groups (an anchor of 1.7-2.6 plus 2-4 smaller pieces) at the foot of cliff faces on 42-66 degree ground just outside the trail kerbs: 13 groups, 40 pieces. Few sites qualify because the keep-outs cover most of the cone.
- **darken**: four DARK variants of the faceted rocks (`caldera.BoulderDark`, `SlabDark`, `WedgeDark`, `ShardDark`, sections 492-495: the same hulls, a darker basalt albedo, 30 % of the dust). Every rock not lying on walkable dust, ash, sand or grass takes its dark variant: the crater (70), bare basalt and lava rock (133) and steep ground (10). The shore's graded tumbles (names `tumble` / `btum`) and the beach keep the warm stone (156).
- **debris**: removes lone or tipped-up small blocks on beach ground (2 on this build).

Run (testenv D, the round-12 undressed project = `erg-mcp` `92df935`): sim `926ed2c04a9a4791d0d65d388b0739edb97873e610b93ae477797d083c696d2e` before, after all 35 passes and after save and reopen. `detail-verify`: 745 props (`_dtl` 308, `_tal` 61, `_flo` 376), 0 violations, 0 flora violations; 857 details. The pack ships 8 banks. An install of the pack (range lowered to 0.13), built with its `build.ps1`, logs `8 bank(s) loaded from 1 mod(s)`, and a match on the installed `Multi.caldera_caldera` draws the rock as the project does.

Perf (testenv D, Sunstone on, the oblique, testenv B running in all three runs): dressed 8.80 / 8.71 ms GPU p50, undressed 5.88, 36 fps in all. Round 12 measured 8.70 the same way with 779 details.

## Round 14 (dress: the beaches' tile grid, Highwatch's plinth, the edges)

From the round-13 judge's next focus (`detail-r14.mjs`; passes `shore`, `plinth`, `smooth` after `ground`; `beach14`, `seam`, `mass`, `meadow`, `lint14` after `lint13`). `erg-mcp` `b73f35b` is merged; it changed only `level.test.selectWeapon` (C++ and `game.ts`), not the map, so the undressed project is still `92df935`'s build (fingerprint `926ed2c0…`).

The paint passes work on the skin's own cells: a 2.5-unit grid whose edges lie on multiples of 2.5. Each cell is repainted through a small rectangle round its centre, with `only` limited to the cell's old material.
- **shore** (primary paint):
  - Every dark or lava-rock square on the flat shore becomes sand: VolcanoRock, HotRocks and rock01/02. On the team castles' shelves this applies up to 3.2 over the court, and elsewhere under 5.5 high and under 40 degrees. Magma, the flows, stays. 139 cells.
  - Every grass square under 10 high becomes sand, or sand01blend (P12: the same sand with pebbles, its sides P11) inside 11 round, wobbly patches on the flat shelves. No grass tile is left on the shore band, so there are no lime rectangles.
  - A sweep then paints any grass voxel left under 10 high (31), and any dark voxel on a team castle's shelf (482), to sand. These were cells whose sample point fell under a wall and answered the wall's material. The sweep goes 20 deep, because those columns also hold a tower.
  - The castles' own materials (12, 16, 27) are never in `only`.
  - A first try used path01 (P09, a dry earth) for the patches. In game it still read as a beige checker, so the patches now use P12.
- **plinth** (primary paint): Highwatch's ring walls are flint, like its plinth, so only voxels under the court's top are painted.
  - Inside the ring the court takes GreySlateFloor (P23): 57 voxels.
  - The square's margin outside the ring takes the trails' dust (path02): 16.
  - Its steep sides take VolcanoRock: 16.
  - The white slab is gone. A first try painted by cell and darkened the walls; that was fixed before any shot was kept.
- **smooth** (primary paint):
  - A majority filter over the cone's cells off the trail bed and shoulder, the castles, pads and lava tongues: a dust, basalt or hot-rock cell with 3 or fewer of its 8 neighbours alike, and 5 or more of one other, takes that material. It runs twice, and dust never goes onto ground over 46 degrees. 41 cells.
  - Then the confetti: HotRocks clusters of 3 cells or fewer take VolcanoRock. 32 cells.
  - 2348 of the 3255 cone cells lie in the trail band. The zigzag the judge saw on trail_NE is mostly the trail's own ash shoulder over the banks between legs, so it stays.
- **beach14**: dark variants on beach ground (under 7.5, rho over 76) go back to the warm stone (31). Lone small pieces are removed (1).
- **seam**: rock groups along light/dark ground boundaries outside the keep-outs: 6 sites, 18 pieces. On the cone 496 of 564 edge points are inside the trail band (the dust is the trail beds), so most sites are at the cone's foot.
- **mass**: gameplay-scale outcrops: a warm Wedge anchor of 3.2-4.4 with a block and a short talus on 28-68 degree shoulders. 3 sites, 5 pieces. Almost no ground qualifies: 2220 points fail on slope, 958 on the trail and 212 on existing rock.
- **meadow**: low plants (grass, tussock, weed, flowers, all 0.8 tall or less) on a jittered grid over each shore patch, dense at its rim: 58 plants.

Run (testenv D, the round-12 undressed project = `erg-mcp` `92df935`):
- The sim fingerprint was `926ed2c04a9a4791d0d65d388b0739edb97873e610b93ae477797d083c696d2e` before, after each of the 43 passes and after save and reopen (43 "sim unchanged", 0 changed).
- `detail-verify`: 740 props (`_dtl` 330, `_tal` 61, `_flo` 349), 0 violations, 0 flora violations. The level holds 852 details.
- The pack ships the same 8 banks. An install of the pack (range lowered to 0.13), built with its `build.ps1`, logs `8 bank(s) loaded from 1 mod(s)`. A match on the installed `Multi.caldera_caldera` (852 details) draws the rock as the project Test does.

Perf (testenv D, Sunstone on, testenv B idle: `game_overlap` clear; the same procedure for both, the 12 shots then 30 s):

| | GPU frame p50 / p95 | fps p50 |
|---|---|---|
| undressed | 4.39 / 4.80 ms | 36.5 |
| dressed | 8.93 / 10.18 ms | 35.9 |

Tool gaps left: the trail beds and their ash shoulders make up the cone's material edges, and the readability keep-outs cover almost all the ground beside them. A sim-neutral edge blend, a terrain-side decal or a texture blend in the released Sunstone is still the only way to soften those edges.

## Final reconcile and check (2026-11-10, after round 14)

`erg-mcp` `30c4996` (the cone's inner shell) is merged; the merge touched only `build.mjs`, `rubric.md` and the pack.
- **Rebuild** (testenv B, project id `calfinal` so the shared `caldera` project was not touched mid-build): `build.mjs`,
  then `detail.mjs` (43 passes), then `ship.mjs` from the `caldera` project. The undressed export equals `erg-mcp`'s
  committed `pack/src/caldera.ergpatch.json` except its `stem`, so it is `erg-mcp`'s map.
- **Sim neutral:** fingerprint `7e3c5c6b629225dafb2473946d06e8ab718b329bc9d2c815df7579212ca3e20c` before the layer, after
  each of the 43 passes and after save and reopen (43 "sim unchanged"). Counts before: voxelFrames 594, solidVoxels 30664,
  simDetails 91. The level holds 860 details; the 8 mesh banks are byte-identical to round 14's.
- **Tests:** `tools/erg-mcp/test.ps1` 266 / 266. Gate 1 on the merged tree: `build.ps1` built; web unit pass; `-Erg`
  34 / 34; `-Panels` 86 / 87 (the same environmental `shell: layout and theme remembered` as above; `web/` is unchanged
  from `erg-mcp`); `selftest.ps1` all C++ selftests pass except `launcher_selftest` 260 / 261, which needs
  `tools\ual\dinput8.dll`, missing in this worktree only.

In game (testenv B, private build of this tree, Sunstone on, the final pack installed fresh with only `melange.range`
lowered to 0.13, `8 bank(s) loaded from 1 mod(s)`):
- **Versus > Deathmatch on the installed `Caldera`**, four human teams of four, a temporary game style (150 s round,
  60 s turns, Raise Water, fast). The sea stood at 20 until the round clock ran out, then sudden death began at turn 4
  and the sea rose 20, 36, 52, 68 (+16 a turn, the fast speed). No on-screen text from the map in any turn
  (`announcements` empty every turn).
- **Courts:** a 10-frame burst from the follow camera of a worm walking in its court shows no stripes or patches.
- **Air Strike** (Test match on the same project, closed-loop aim): across the volcano at the summit side and at the
  crater floor. The helicopter flies at y 77, at least 8 over the ground under its path; all 12 bombs exploded on the
  cone or the crater floor, none fell through (round 3 saw 3 of 6 at the crater on `erg-mcp` before the inner shell).
- **Perf:** 35.4 fps, GPU frame p50 8.5 ms (Versus, installed pack); 34.7 fps, 7.3 ms (Test).
- The Versus match drew in a dark blue evening light; the same level in a Test match draws in daylight.

Evidence: scratchpad `caldera/polish/final/` (`final_01..08_*.png`, `sheet_*.png`, `match/rows.json`,
`airsum_final.jsonl`, `fin_as_*_track.json`, `detail.log`, `dressed/detail.json`).

## Reconcile: weapons round 4 (2026-11-10)

`erg-mcp` `195539e` (the `LevelLandMask` engine fix: no-ground columns and bombs through the crater floor) and `b3388f6`
(a 12 x 4 x 12 slab of 3-unit voxels under the crater floor's inner shell, so a Concrete Donkey stops on the floor) are
merged. Rebuild in testenv B with this tree's Melange.exe: `build.mjs` (its undressed export equals `erg-mcp`'s
committed `pack/src/caldera.ergpatch.json`), `detail.mjs` (43 passes, 43 "sim unchanged", reopened unchanged), `ship.mjs`.
- **Sim neutral:** fingerprint `fd3c3765a2e214ae37ce00829d9c4f9e3b163223df8d27559589c1d5481416b9` before the layer, after
  every pass and after save and reopen. Counts before: voxelFrames 595, solidVoxels 31240, simDetails 91. `spice.json`
  (range `>=0.14.0`) and the 8 mesh banks are unchanged; only the level patch changed.
- **Tests:** `tools/erg-mcp/test.ps1` 266 / 266.
