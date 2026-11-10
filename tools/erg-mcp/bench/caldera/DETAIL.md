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
| ledge | detail-basalt.mjs | Strata ledges on 45-75 degree faces (`caldera.Ledge`, tag `_ldg`). |
| columns | detail-basalt.mjs | Basalt columns on cliff tops (`caldera.Columns`, tag `_col`). On Caldera every candidate falls in a keep-out, so 0 are placed. |
| slab | detail-slab.mjs | Off (`DTL_SLAB_MAX=0`). |
| retint | detail-retint.mjs | One rock tint per elevation band on every rock copy (per-copy `_tint<rrggbb>`, Sunstone `mg_tint`). |
| support | detail-support.mjs | Removes rock clumps hanging off steep faces with no ground or talus under them. |
| flora | detail-flora.mjs | Plant clumps zoned wall / shore / green / lawn / cliff / rock / dry / ash, with a lush-to-hardy height ramp (tag `_flo`). |
| lint | detail-lint.mjs | Removes mine look-alikes, hovering props and buried props. |
| perch | detail-perch.mjs | Removes clumps perched with a visible gap under them from the fixed cameras. |
| alone | detail-alone.mjs | Removes plants that stand alone. |
| basalt | detail-basalt.mjs | Re-points every remaining HORROR5 copy at the matte `caldera.Boulder` and removes the small stones left on faces over 50 degrees. |

**Final layer:** 459 props (`_dtl` 160, `_tal` 82, `_ldg` 34, `_flo` 183), 450 of them tinted, and 571 level details in all.

**Keep-out** (`detail-keepout.mjs`): no prop on a trail or its kerbs, a castle court, a spawn, a launcher pad, the mine
factory or lava. Scenery copies have no collision, so nothing may stand where worms walk.

**Caldera's own meshes** (`meshes/gen.mjs`, `meshes/build.mjs`): `caldera.Boulder`, `caldera.Ledge` and `caldera.Columns`.
Each is a procedural model with a 128 x 128 basalt texture, bundled with the shader of the game's HORROR5. The material is
made matte (specular and Fresnel set to 0). The banks are built output and are not committed.

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
layer is missing. A pack with the layer needs three things:
1. Export the dressed project (`export`). Its ship gate raises `melange.range` to `>=0.14.0`, because 571 details are over
   the 450 that the vanilla 550-instance mesh pool loads. The pool is raised by `[Fixes] MeshInstances`.
2. Copy the three banks to `pack/assets/meshes/`.
3. Add `"meshes": [{"file": "assets/meshes/caldera.Boulder.xom"}, {"file": "assets/meshes/caldera.Ledge.xom"}, {"file": "assets/meshes/caldera.Columns.xom"}]` to `pack/spice.json`.

The committed `pack/` is still `build.mjs`'s undressed export, so `erg-mcp` stays the owner of the map. The dressed pack is
the release step, done after `erg-mcp` lands.

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

A professional game-asset designer judged each round in game, with Sunstone on and the same cameras every round. The
final verdict, after round 10, was re-checked on the reconcile run (`caldet_r11_*`):

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
