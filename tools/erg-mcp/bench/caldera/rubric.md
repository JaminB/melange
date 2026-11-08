# Caldera benchmark

The brief (the user's words): "an extremely complex map with a volcano in the center, and castles scattered
throughout. It is of course surrounded by an ocean that will rise and the worms must move toward the center of the map
up the volcano to survive. The volcano is where the mines are spawned from."

The map is built only through the erg-mcp server's tools, by `build.mjs` here (see its header for how to run it); the
level script is `script.lua`, and `pack/` is the source export of the last build.

Each item passes or fails, with evidence.

| | Item |
|---|---|
| R1 | Builds via the MCP from `tools/erg-mcp/bench/caldera/build.mjs` reproducibly; `level.save` has no warnings; export (source) succeeds; within 65 535 cells. |
| R2 | Loads and plays in game with no asserts or errors in `Melange.log`; fps not worse than vanilla maps (about 36 in the harness). |
| R3 | The volcano clearly reads as a volcano from the default (overview) camera: a tall central cone, a crater at the summit, volcanic materials (dark rock or ash; a lava-like colour if a material allows), distinctly taller than everything else. |
| R4 | Castles: at least 5 recognisable castles (walls, towers, crenellations, keep or gate) scattered around the island at varied heights, made of voxels so they collide and are destructible; optionally decorative Camelot scenery copies near them. |
| R5 | Ocean all around; the water starts low and rises (level script) so that over about 10-15 turns the lowlands and the castles near the shore flood and only the upper volcano stays dry; the rise is announced (a message) and is deterministic sim code. |
| R6 | The mine factory is at or inside the crater, and mines are seen coming from it in play. |
| R7 | Every spawn (8 knots, spread over the castles) can reach the volcano summit on foot or with standard utilities; ramps, switchbacks or tunnels exist; no floating debris, no unreachable or underwater spawns. |
| R8 | It looks good and complex: varied terrain (ridges, gullies, beaches, cliffs), not boxy; theme and time of day chosen deliberately. |

## Iteration 4 design (builder's notes)

A single `heightfield_drape` skin (voxel 2.5, 3 thick, hollow) over a 230 x 230 rectangle gives a smooth cone: summit rim at
71-80, castles at 11-34, a breach notch facing south with a lava flow to the shore (`terrain.mjs`). Three roads climb at
26.6 degrees through seven pale-stone castles (shelves cut and filled; a gate at each end); `check_reachability` at
maxSlope 38 and `check_flood` (the script's literal `waterSchedule`) pass 8 of 8; `check_islets` finds no detached
piece. Water: first rise turn 5 (round 3), 62 world units a turn, cap 740 at turn 16. Mines: the crater factory (target 12,
one idle turn, no start mines). Cells 49 859 of 65 535, 16 blocks.

## Iteration 5 design (builder's notes)

- **Setting**: Prehistoric theme and its own material file (`set_theme PREHISTORIC`, `materialFile: "theme"`), NIGHT. The
  lava is the theme's LiquidHotMagma (50) with HotRocks (51) margins and crater walls, the summit VolcanoRock (49), roads
  path01 (8), courtyards flint (16), castles stone/flint/chipped stone. The base's diner scenery is removed.
- **Island**: one `heightfield_drape` (voxel 2.5, skin 3 thick, clipped to a circle of 117) on a floor at -9, so the
  beach slopes under the sea instead of ending in a slab (a coast of 79-111 units radius: headlands, coves, rocks);
  `skirtTo`/`skirtTop` 0 fills the shore solid so the rising sea shows no hollow underside. The flank climbs in treads
  (27 degrees) between three cliff bands (about 70 degrees, 10 high, wobbled by angle), so the roads that cut through
  them are the way up. Grid heights are limited to 2.9 a unit (`relax`), so the 3-thick skin never folds.
- **Materials**: distance-field masks for the roads (`sd`: exact distance to each polyline) and for the lava tongues (they
  widen downslope), `feather` (dither) and `jitter` on every band edge: ash -> grey rock -> rock -> olive/grass -> sand.
- **Castles** (7, `build_castle` at scale 1.5 with `foundation`, a second gate carved with `fill_box_raw`): shore 3
  (floor 4), lower treads 2 (19, 20), high treads 2 (37, 41). Each runs along x or z, straight, and the road runs
  through both gates; between castles the road climbs at no more than 32 degrees, in switchbacks when the way is short.
- **Spawns** on courtyards (five) and wall-walks (three), one castle each, two on Cinder.
- **Mine factory** on a dais in the crater, target 16, no idle turns, no start mines: it fires from the first turns.
- **Water**: first rise turn 7 (round 4), 56 world units a turn, cap 820 (41 map units) at turn 21; announcements at turns
  1, 7, then every 4, and at the cap. The shore castles drown around turns 11-12, the lower treads around 17-18; the two
  high castles and the cone stay dry.
- **Checks in the build**: `check_reachability` (45 and 38 degrees, to the rim), road only (every spawn reaches the rim
  where its own road ends over road, flagstone and castle stone: 8 of 8) and off road (2 of 8: only the two high Cinder
  spawns, one cliff band from the rim; the checker judges the skin's cliff as a stair so it passes at 55 degrees),
  `check_flood` with the script, `check_islets` (no detached piece).
- **Cells** 62 552 of 65 535 (24 of 64 blocks): the skin 28 100, the castles about 22 800, the base 11 652.
- **Reproducibility**: `build.mjs` twice and `diff -r pack/` (see the iteration's notes).

## Iteration 7 design (builder's notes)

- **Cone**: profile = soft foot (10 units of beach) + a tread of 0.52 + two cliff bands (shore bluff at 78, 13 high at 47) +
  a boost of 20 that steepens the top; rim 68-80, crater floor 58 (a wide bowl: floor level to r 26, walls to r 36), castles
  at 30 and under, so the cone is the tallest thing from every side. Radial ribs (7 lobes), 4 dry gullies and 5 lava gullies.
- **Mine factory** on a basalt plug (HotRocks, undraped blocks) standing on the crater floor at r 19 on the side the
  south camera sees over the breach, `keepKnot: true` so it stays on its knot if the game's drop misses. Not played in game
  this iteration.
- **Roads** march up the cone in spirals (heading leans 41 degrees or more from uphill, climb 0.6 a unit); a castle stands
  where the road reaches its floor and the road runs through it along a chord of the contour, through gates placed where
  that line meets the walls. 7 castles of 6 plans: classic keep (Strand), square-towered (Ashgate), tower house (Cinder),
  L-shaped harbour fort with a wing and a sea gate (Harbour), 38 x 12 long wall with no keep (Ember), big bailey (Seawall),
  round keep on a crag inside a 16-sided ring (Highwatch); floors 4, 17, 30, 3, 23, 4, 26; 3 outposts (towers).
- **Lava**: tongues of magma with a hot margin, perlin edges, stopping at the beach (`minHeight`), from the crater to the sea.
- **Sea**: from turn 5 +15 world a turn (turn 8: 96, shore castles awash; turn 11: 141), from turn 13 +50 a turn, cap 500 in
  turn 19 (25 map): the mid castles (17, 23) go under turns 16-19, the two high castles (26, 30) and the cone stay dry.
  EVENING (dusk) for the lava's glow.
- **Checks**: reach 8 of 8 (45 and 38 degrees), road only 8 of 8, flood 8 of 8 dry, islets 0 detached, save with no warnings.
- **Cells** 63 251 of 65 535 (31 of 64 blocks): the skin 24 117 (11 blocks), the castles about 22 000, the base 11 652.

## Iteration 10 design (builder's notes)

Castles are 22 across with a keep sized so that a lane 4.5-5.5 map units clear lies between the keep and the corner towers
(`terrain.mjs` `laneOf`): both gates open into it (width = the lane, height = wall - 1 = 5), so no keep or pillar is in the
gate line; `check_reachability` with `bodyWidth: 3.5` reaches the rim from all 8 spawns. Seawall is a small gatehouse fort
(the cells paid for the lanes). Nobody spawns on a rampart or on the shore: all 8 are in courtyards at 16.5-29 map (Ashgate 3,
Cinder 3, Ember, Highwatch). The flood schedule takes S = the lowest spawn less 2.5 (check_flood: dry by 1.5 at the spawn's
own turn 2 = game turn 11 = round 6), so the shore castles drown at rounds 5-8 before anyone is there. The crater floor is
at 49 (was 58) with the inner rim raised 3 and the floor 18 wide, a 9-radius lava lake, lava pools and hot-rock patches
on the ash; the factory plug is 5 radius and a solid 18 x 18 block of 3-unit voxels joins it to the skin underneath
(launch height 16). Vegetation is limited to slopes under 22 degrees below 9 map units (no green squares on the cone); roads
are 7.2 wide with a flat bed of 3.4. 64 029 of 65 535 cells, 35 of 64 blocks, 97 MCP calls.

## Iteration 11 notes

- Spawns: 8 knots over 7 places (Ashgate 2, Cinder 2, Highwatch, Ember, the Tower house and Gullywatch outposts 1 each), never more than 2 in a courtyard; early movers on the low floors.
- Flood: `schedule.mjs` derives the sea from the spawns' floors and the engine's rotation (4, 1, 5, 2, 6, 3, 7, 0): every spawn dry (1.5 map) at its worm's 3rd own turn (`check_flood` minOwnTurns 3 passes), the castle walls drowned by round 13.
- Terrain: roads painted 1.8 wider than their bed with a dark ash shoulder, an organic lava lake with thin rivulets, an ash apron round the factory plug, no vegetation inside the rim or above 9 map units, five shallow gullies between the switchbacks. Camelot flags on tower tops (decoration, no collision; `--no-scenery` leaves them out).
