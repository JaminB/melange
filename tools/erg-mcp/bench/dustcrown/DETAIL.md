# Dustcrown's detail layer

Decoration that changes nothing the simulation reads, added after `build.mjs` on the saved project (as Caldera's
DETAIL.md describes for its volcano). `detail.mjs` never edits `build.mjs`; `ship.mjs` writes the dressed pack. Round 1
(2026-11-12): 400 scenery copies, the level at 627 details in all (the mesh pool holds 2 000: `detail_budget`).

## Passes (`detail-passes.mjs`, in order)

| Pass | What it places |
|---|---|
| mesas | A graded tumble at each mesa's foot (a block, two middling stones, a heap, two small ones): 5 a refuge, 3 an outcrop, on ground under 30 degrees that is not the cliff. Eight low boulders and heaps on each refuge's top, along its rim (the middle stays clear: it is where a worm stands). |
| hoodoos | Eight wind-carved groups in the open desert, 34 or more apart, on gentle ground: a balanced rock (a Boulder stretched into a stem and a flat Boulder cap wider than it), a pillar, a leaning block and a heap. |
| litter | Sixteen pairs and threes of loose boulders and heaps, 18 or more apart. |
| flora | Twenty-six clumps of five or six Spikes (a thin shard in a dry straw palette) in the dune troughs, 11 or more apart: sparse, the few tough plants of a dune desert. |
| palace | 28 tapestries (ARABIAN15's red cushion on its edge): three a curtain face away from its gate, four a face on the third plinth's riser off the niches. 40 arrow slits (the Slit mesh, a dark block squashed flat) on the corner towers' outward faces at three heights and the Crown's four faces at two. Eight gold obelisks at the corners of plinths 1 and 2. |
| lint | `lint_contact` (ground over 50 degrees under a piece) and `lint_support`, on the layer's own `_dtl1` tag: 16 and 1 pieces out. The palace's pieces (`_pal1`) stand on vertical faces on purpose and are not linted. |

**Keep-out** (`keepOut`): nothing within 14 of a pad, 5 of the straight walk in from each pad, 53 (Chebyshev) of the
palace's centre (no stair of rocks up a riser: round 4), 9 of a boulder field's centre, 4 of a mesa, or past the coast
(93). Scenery has no collision, so nothing a worm walks through reads as a bug.

## The bank (`meshes/bank.mjs`)

One bank, `dustcrown.Rocks.xom`, five meshes: Boulder and Heap (the round-4 ones, in a warmer stone: `[228, 184, 116]`),
Obelisk (gold), Spike (straw) and Slit (near black). At most six: see docs/erg-mcp.md "Dustcrown detail layer, round 1"
(`build_rock_bank` converts mesh *i* on section 514 + *i*, and xomtool stops at 519). `build.mjs` imports the same file, so
the undressed map's boulder fields and the layer cannot drift.

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

## Not done in round 1

- No flora of the other themes (they draw as ARABIAN props).
- No keep or court ornament (the keep's windows are voxels), no statues beyond the obelisks.
- Colours were judged by eye in the game from a handful of poses; the next pass can tune the Spike's green cast.
