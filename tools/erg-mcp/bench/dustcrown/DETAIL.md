# Dustcrown's detail layer

Decoration that changes nothing the simulation reads, added after `build.mjs` on the saved project (as Caldera's
DETAIL.md describes for its volcano). `detail.mjs` never edits `build.mjs`; `ship.mjs` writes the dressed pack. Round 1
(2026-11-12): 400 scenery copies, the level at 627 details in all (the mesh pool holds 2 000: `detail_budget`). Round 2 (the judge's 5/10: one saturated lemon hue, blue and stair-stepped domes, 30 identical banners, gold plastic trim, boxy walls, no dryness cues): 826 details, the palace recoloured by tint, smooth domes, 8 banners, pilasters, a window rhythm, a tall Crown, pylons, wells, bones and denser dead scrub.

## Passes (`detail-passes.mjs`, in order)

| Pass | What it places |
|---|---|
| mesas | A graded tumble at each mesa's foot (a block, two middling stones, a heap, two small ones): 5 a refuge, 3 an outcrop, on ground under 30 degrees that is not the cliff. Eight low boulders and heaps on each refuge's top, along its rim (the middle stays clear: it is where a worm stands). |
| hoodoos | Eight wind-carved groups in the open desert, 34 or more apart, on gentle ground: a balanced rock (a Boulder stretched into a stem and a flat Boulder cap wider than it), a pillar, a leaning block and a heap. |
| litter | Sixteen pairs and threes of loose boulders and heaps, 18 or more apart. |
| palette | `tint_landscape` on the palace's own textures (a visual carrier): stone warm ochre, sandy plaster burnt sienna, Gold (merlons, caps) pale limestone, GoldEdging a matte darker course, BlueMosaic copper green, StarMosaic tan, darkyellowplaster (the mesas' strata) rust. The colours ASKED were calibrated in the game (`docs/erg-mcp.md` round 2): the grade file's preview is wrong off sand. |
| flora | Up to forty clumps of six to nine Spikes (thin shards in a dead-scrub grey brown) in the dune troughs, 9 or more apart: thorn scrub. |
| dry | Bone piles (three to five lying Bones crossed) round the pads and in the open desert, dry wells (a collar of eight Boulders round a flat dark Slit, a clay urn beside it), clear of the hoodoos. Cracked hardpan and skulls were tried and dropped (see docs/erg-mcp.md). |
| palace | 8 faded-crimson banners (ARABIAN15 tinted `a04a38`): one on each gate's flank and one on each of the keep's faces over the wings' roofs, none on the plinths. Smooth Dome meshes over the four towers' voxel domes and a tall one (x 1.12, 1.8) over the Crown's merlons. 12 pilasters (stretched Boulders) on the curtain's faces, clear of the gates. A window rhythm (Slit): four a face on the keep, two heights on the Crown, three on each tower face. Eight squat stone pylons (Boulders) at the corners of plinths 1 and 2. |
| lint | `lint_contact` (ground over 50 degrees under a piece) and `lint_support`, on the layer's own `_dtl1` tag: 16 and 1 pieces out. The palace's pieces (`_pal1`) stand on vertical faces on purpose and are not linted. |

**Keep-out** (`keepOut`): nothing within 14 of a pad, 5 of the straight walk in from each pad, 53 (Chebyshev) of the
palace's centre (no stair of rocks up a riser: round 4), 9 of a boulder field's centre, 4 of a mesa, or past the coast
(93). Scenery has no collision, so nothing a worm walks through reads as a bug.

## The bank (`meshes/bank.mjs`)

One bank, `dustcrown.Rocks.xom`, six meshes (the most a bank holds on section 514): Boulder and Heap (burnt-ochre sandstone `[232, 184, 126]`),
Spike (dead scrub), Dome (a `lathe` onion in copper green), Slit (near black) and Bone (a `lathe` with knobbed ends). Round 1's
gold Obelisk is gone. `build.mjs` imports the same file, so the undressed map's boulder fields and the layer cannot drift. A game
keeps the bank it loaded first: restart it after any bank change.

## How to run it

The game in the target folder (default testenv C) must be closed. `MELANGE_EXE` defaults to this checkout's
`build\x86-release\Melange.exe`; the private build in `dist-private` is newer.

```
tools\node\node.exe tools\erg-mcp\bench\dustcrown\build.mjs --out <dir>            # the undressed map (round 6's work)
tools\node\node.exe tools\erg-mcp\bench\dustcrown\detail.mjs --out <dir> [--render <dir>] [--dry]
tools\node\node.exe tools\erg-mcp\bench\dustcrown\ship.mjs --check                  # the neutrality proof from the files
tools\node\node.exe tools\erg-mcp\bench\dustcrown\ship.mjs                          # the dressed pack into pack\
```

`detail.mjs` removes its own earlier pieces first (`_dtl1`, `_pal1`), so it can be run twice: the saved project hashes
the same (`cf9161545b876af7`, three runs). The layer's pack differs from the undressed one in its level patch (400 `add`
ops), its mesh bank and its menu picture (drawn from the level camera with the layer's meshes).

## Neutrality proofs

1. `sim_fingerprint` after every pass equals the one before the first (`86d695189ee979d0...`), with the per-part hashes
   (`base`, `frames`, `voxels`, `spawnKnots`, `objects`, `script`, `water`, `cameras` ...) unchanged; the saved project,
   opened again, hashes the same. `detail.mjs` throws, and saves nothing, when it does not.
2. `ship.mjs --check` exports the dressed project and compares its level patch, without the `add` ops of the layer's
   details (names ending `_dtl1` or `_pal1`), with the committed pack's patch as `build.mjs` wrote it: identical.

## What the game showed (testenv B, Sunstone 2.4.0 with the sea and haze, 2026-11-12)

See docs/erg-mcp.md "Dustcrown detail layer, round 1". In short: the layer reads (balanced rocks, tapestries, slits, gold
obelisks, spiky clumps), the Test loads (the one assert of a run after a fresh launch is the game's own `Language not recognised` prompt; the runs after it had none), and the dressed level costs 1.5 ms of GPU time over the undressed
one with the CPU the bound in both.

## Not done (rounds 1 and 2)

- Round 2 left to the builder (build.mjs, not a detail): the dunes' hard facets and white crests and their lemon body tone
  (tint calibration above), the cream haze (dd2_01 is still washed out), vertical seam stripes and the uniform strata band on the mesas,
  a recessed gate arch and battered base tiers (the gate's lintel sits at 30; a banner cannot hang over it), the courts' visibility
  from the overview.
- Cracked hardpan decals need a flat-decal feature the bank cannot give (six meshes).

- No flora of the other themes (they draw as ARABIAN props).
- No keep or court ornament (the keep's windows are voxels), no statues beyond the obelisks.
- Colours were judged by eye in the game from a handful of poses; the next pass can tune the Spike's green cast.
