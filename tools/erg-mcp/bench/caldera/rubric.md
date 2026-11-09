# Caldera benchmark

The brief (the user's words): "an extremely complex map with a volcano in the center, and castles scattered
throughout. It is of course surrounded by an ocean that will rise and the worms must move toward the center of the map
up the volcano to survive. The volcano is where the mines are spawned from."

The map is built only through the erg-mcp server's tools, by `build.mjs` here (see its header for how to run it); the
level script is `script.lua`, and `pack/` is the source export of the last build.

Each item passes or fails, with in-game evidence. The rubric of v2 (the user's directions of 2026-11-09: obvious
switchback trails from every castle to the summit, mines from launchers on the caldera's sides, four human teams each
spawning in its own castle, every worm steered, checks with Sunstone on):

| | Item |
|---|---|
| R1 | Builds via the MCP from `tools/erg-mcp/bench/caldera/build.mjs` reproducibly; `level.save` has no warnings; source export succeeds; within 65 535 cells. |
| R2 | Loads and plays with Sunstone ON and no asserts beyond the vanilla baseline (a vanilla Multi.DinerMight run in the same build with Sunstone on, bazooka fire, counted in the same session); fps not worse than that vanilla run. |
| R3 | The volcano clearly reads as a volcano from the overview: tall central cone, crater, volcanic materials, distinctly taller than everything. |
| R4 | At least 5 recognisable voxel castles at varied heights, destructible; EVERY team spawns all its worms inside its OWN castle (4 teams in 4 different castles); no worm spawns outside a castle. |
| R5 | Ocean all round; the water rises (deterministic level script) so over about 10-15 rounds the lowlands and shore castles flood and only the upper volcano stays dry; the rise is announced on screen. |
| R6 | Mines come from the volcano and land ON the volcano/caldera, not all over the island: either the central factory with a constrained launch range or several launchers on the caldera sides. Screenshot mines in flight. |
| R7 | An OBVIOUS switchback trail (zig-zag, readable from the overview, distinct material, wide enough for a worm with margin, walkable gradient) runs from EVERY castle to the summit. In the test every worm is steered up its trail; at least one worm per team is screenshotted climbing it (start/middle/end) and reaches at least the upper cone. No unreachable or underwater spawns. |
| R8 | It looks good and complex: varied terrain, not boxy; theme/time of day deliberate. |
| R9 | The test used 4 HUMAN teams (no CPU teams) and the tester steered every worm on every turn it took. |

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

## v2 round 1 (builder's notes)

- **Teams and castles**: four team castles near the shore, each turned to its contour (`build_castle` `rotation`, local x along
  the contour, local +z to the sea), court at 4.5 map: Strand (team 0, NE), Ashgate (team 1, SE), Harbour (team 2, SW, the L fort
  with a sea gate), Seawall (team 3, NW). `place_spawn_knots {mode: "teams"}`: team t's knots WORM4t..4t+3 are four courtyard spots
  of its own castle (16 knots; the test plays two worms a team). Three higher castles beside the trails: Cinder (court 21, by the NE
  trail), Ember (a small square fort, court 25.5, by the SW trail), Highwatch (the ring castle, 32, by the NW trail), each with a
  level branch from its gate to its trail.
- **Trails** (`terrain.mjs`, laid out, not marched): from each team castle's far gate a column of five legs and four hairpins up
  its diagonal of the cone to the rim crest; hairpins spread so every leg climbs alike and at least 9.5 apart radially, each leg as
  wide (8-17 off the column's line, 20 for the last) as climbing it at 0.5 a unit needs; heights straight between hairpins, taken
  on the column's line. Path stone over the 5.2-wide bed with a dark ash shoulder; the cliff bands are notched where a column
  crosses them. The lava runs down the cardinals (east, north, west tongues) and the breach faces south.
- **Checks in the build**: trail and castle stone alone, at 38 degrees and the worm's 0.6 width, take all 16 team spawns to their
  own trail's rim end; never touching them, none (the trails are the way up); any route, 16 of 16 at 45 and 38; no detached
  piece (a rotated merlon left joined by a corner is emptied with `fill_box_raw` after `check_islets` finds it).
- **Sea** (`schedule.mjs`, four teams of two): dry under every spawn through its worm's 3rd own turn (round 6, at most 55
  world), the shore castles' wall-walks + 1 by round 10, every castle's by round 15, the cap 860 (6 under the crater floor, under
  the launchers' pads) from round 16.
- **Mines**: the mines report's setup (four launchers at 23 from the crater's centre on 6 x 6 hot-rock pads, launchHeight 14,
  launchRadius 2, everyNTurns 3, no scheme mines, keepKnot); the crater is unchanged (its rim is raised from the unnotched profile).
- **Cells** 64 348 of 65 535 (37 of 64 blocks): the base's frames removed (`remove_base_frames`: 11 652 cells to 610), the skin
  about 29 000, the seven castles about 33 000. 135 MCP calls; two builds give the same projectHash and pack.

## v2 round 2 harness (2026-11-09)

- `play.mjs` is the in-game test, through the erg-mcp tools only: the vanilla baseline (asserts, its overview, its GPU time
  per frame with `game_perf`), a probe match that drops a mine along every trail's centre and shoulders (`game_mine_drop
  {trails}`), then the Test: spawns, a wide labelled shot of each team castle with its worms (`game_shots` preset castle),
  the announcement shot (`game_state {announceShot}`), one wall of each team castle blasted with shots before and after
  (`test_explosion {game}`, its eye picked clear of the ground, at the build's blast points, `out/blasts.json`), the GPU time against the baseline, every
  worm walked up its trail (`game_play_turns`: a planned approach to the trail, a replan when stuck, a shot of each
  announcement), the asserts against the baseline.
- `build.mjs` places the blast points, checks every trail column by column (`check_trail`, `out/trails.json`, with the
  skin corners near each trail) and checks the flood with the Test's two worms a team (team knots).

## v2 round 2 (builder's notes)

The round-1 critique: both SW worms fell 31 units through the trail beside Ember, one Ashgate spawn was boxed against the
keep, and the trails had no lip on their downhill side.

- **The fall**: it was at a corner where four draped skin blocks meet. The skin is cut into 31-column tiles from the field's
  corner, so the four corners of the middle tile are at (+-40 or 37.5, +-40 or 37.5), on the diagonals the trails climb. The
  game lets a worm or a mine through within about 0.4 of such a corner (docs/erg.md). The build lists the corners within 12 of
  any trail or branch (`check_trail` `skinCorners`) and puts a plug under each: 3 x 4 x 3 undraped 1-unit voxels whose top is
  flush with the skin at the corner or under it, so whatever falls through lands at most a unit lower (4 plugs, 144 cells).
  There is no solid fill under the whole trail: the skin is one continuous sheet away from those corners, and a core under
  four 160-unit diagonal trails would cost axis-aligned boxes the 65 535 cells cannot hold.
- **Trails** (`terrain.mjs`):
  - Each column starts `lead` 24 along the contour beyond its castle's out gate (was 9). Round 1's second legs passed 3-5 units
    from the uphill walls, over a 15-unit drop into the court; now every leg above the court stays 9.7 or more from its castle.
  - The climb is evened: each hairpin's height moves 0.7 of the way to an even grade over the whole column, and each leg
    eases into and out of a hairpin (half its mean grade there). The steepest stretch is about 31 degrees, down from 40.
  - Every leg has a kerb on its downhill side and round each hairpin tip. It stands 1.2 over the bed from 3.1 to 5.6 off
    the centreline, on a berm at the bed's height out to 7.5. Between two legs it is the upper leg's kerb. `relax()` keeps
    the kerb cells (and the courts) pinned, so its ramp eats the berm, not the kerb.
- **Spawns**: each team's four knots are on the line between its castle's two gates (the lane beside the keep, 2.7 clear of it
  on either side): 3.5 and 8 either side of the court's middle, the two played worms on the out gate's side first.
  `query_ground` checks the floor and the headroom of each. `waypoints.json` gives each spawn its own `routes`: the spot, the
  court's middle when the spot is behind it, the out gate, the point outside, then the trail.
- **Checks in the build**:
  - Trail-only at 38 degrees and the worm's width: 16 of 16 team spawns reach their own trail's rim end. Off the trail none
    do, and any route takes 16 of 16 at 45 and 38.
  - `check_trail` finds no drop over 3 off any bed's outer side. What is left are 3-4.6 drops where a leg's 3.6 margin
    overlaps the leg below it near a hairpin (onto the trail).
  - Its "no ground" and "hole" columns at exactly x or z = -40 are the skin tiles' seam as the reader samples it:
    `query_ground` reads solid ground on both sides.
  - Flood PASS. 0 detached pieces. Save with no warnings.
- **Sea** and **mines**: unchanged (the same schedule from the same castle floors; the cap 860 world, 43 map, is under the
  launcher pads at 48+).
- **Cells** 62 670 of 65 535 (41 of 64 blocks); 162 MCP calls. Two builds give the same projectHash (f5dc0f39...).
