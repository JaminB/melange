# Caldera benchmark

The brief (the user's words): "an extremely complex map with a volcano in the center, and castles scattered
throughout. It is of course surrounded by an ocean that will rise and the worms must move toward the center of the map
up the volcano to survive. The volcano is where the mines are spawned from."

The map is built only through the erg-mcp server's tools, by `build.mjs` here (see its header for how to run it), and
`pack/` is the source export of the last build. The map has no level script (since 2026-11-10, below).

**The sea is the standard sudden death (the owner, 2026-11-10).** "It is important that the water rises with whatever the
time is configured to just like any other map when sudden death begins." The scripted rising sea (`script.lua`, generated
by `schedule.mjs`: a level a round and on-screen announcements) is gone, and with it every text the map showed. The level
only sets the sea's starting level, 20 world units (1 map unit, under every beach and spawn), in its chunk as vanilla levels
do; the pack has no scheme and nothing else touches the water. The sea rises when the scheme's sudden death begins, at the
scheme's round time and water speed (tested in testenv B on the Quick Game scheme: 20 world units through the round, then
+8 a turn from the first sudden-death turn: 28, 36, ... 92 at turn 11). The rounds' notes below that describe a schedule,
announcements or `check_flood` against the script are history.

Each item passes or fails, with in-game evidence. The rubric of v2 (the user's directions of 2026-11-09: obvious
switchback trails from every castle to the summit, mines from launchers on the caldera's sides, four human teams each
spawning in its own castle, every worm steered, checks with Sunstone on):

| | Item |
|---|---|
| R1 | Builds via the MCP from `tools/erg-mcp/bench/caldera/build.mjs` reproducibly; `level.save` has no warnings; source export succeeds; within 65 535 cells. |
| R2 | Loads and plays with Sunstone ON and no asserts beyond the vanilla baseline (a vanilla Multi.DinerMight run in the same build with Sunstone on, bazooka fire, counted in the same session); fps not worse than that vanilla run. |
| R3 | The volcano clearly reads as a volcano from the overview: tall central cone, crater, volcanic materials, distinctly taller than everything. |
| R4 | At least 5 recognisable voxel castles at varied heights, destructible; EVERY team spawns all its worms inside its OWN castle (4 teams in 4 different castles); no worm spawns outside a castle. |
| R5 | Ocean all round, starting under every beach and spawn; the water rises as on any other map, by the scheme's own sudden death (its round time and water speed), so the lowlands and shore castles flood first and the volcano last. The map shows no instruction or announcement text and sets no water beyond its start level. (Until 2026-11-10: a deterministic level script raised it over 10-15 rounds with on-screen announcements.) |
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
- **The cone's inner shell** (weapons round 2, 2026-11-10): the 7.5-thick skin over a void was breached by an Air Strike's
  first bombs, and the later bombs, the Fatkins payload and a Gas Canister fell 40-50 units into the hollow cone with the
  follow camera after them. A second `heightfield_drape` (one block of 31 x 31 columns, 4-unit voxels, 4 thick, `keepFloor`,
  VolcanoRock) follows the same surface 7 units down over x, z in [-60, 64], so the skin and the shell overlap by half a unit
  and the slope is 23.5 units of rock; near the block's edge the grid is eroded (the least height within 4) so the coarse edge
  never rises into the skin (build.mjs checks the ground at 1 520 probes before and after: unchanged). Its 3 844 cells come
  from the sea floor: the skin's columns under -2.4 (the sea's floor is -3, the sea starts at 1) are left out, so the skin ends
  in the shallows and the tiling is pinned (`tileOrigin` -40, -42.5) where `cornersOff` had put it. Cells 64 187 of 65 535.
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

## v2 round 4 (builder's notes)

The round-3 critique applied (`terrain.mjs`, `build.mjs`):

- **Landings**: every hairpin is a level landing about 9 across. Each leg runs level for 3 either side of the hairpin and
  the bed widens to 4.6 half width round it (`TRAIL.flat`, `TRAIL.land`). The legs are sized for the climb plus the level ends.
- **Berms**: inside every hairpin's V, wherever the two legs' centrelines are less than 17 apart, the ground is raised to
  the upper leg's height (`BERM`). Its side over the lower leg is a bank of at most 2.4 a unit from just off that bed, so
  the lower bed is never lifted. Across every V the relaxed grid now has no dip deeper than 0.6 (the kerb's lip).
- **NW**: legs at a 0.44 grade (the others 0.5). The steepest 3-unit stretch is 0.55-0.62 on every trail, and the NW is no
  steeper than the rest.
- **The bed**: level 2 past its painted edge before the bank or the kerb starts (`TRAIL.shoulder`). Pinned in `relax()`.
  Each column takes the height at its foot on the polyline, not the nearest point's (round 3's beds were stairs of a
  map unit's tread). The material is GreySlateFloor (19), a warm grey slate floor, with the dark VolcanoRock kerb line kept.
- **relax()** now runs to convergence and then unpinned until no step is over 2.9. Round 3's 60 passes left about 2 400
  steps over the limit, up to 17 high: skin cells sheared across themselves, the kind check_trail names as mine-leak
  suspects.
- **Branches** ramp at up to 0.3 to the nearest point of their trail (round 3: level only, so the SW branch ran 58 units
  through the V of the SW trail's second hairpin). The last leg ends 0.6 of its width off the column's line (was all of it),
  so the rim end no longer stands over the hairpin two below.
- **Mines**: the launcher pads are 2-unit voxels (5 x 5 with a 3 x 3 dish), not 1.5. The leak plugs go in after the
  castles: the NE one is now under the castle block's top at 10.5 (plug top 10). Plugs were added at (54.44, -58.17) and
  (-44.1, -34.3), each under the lowest ground of its footprint.
- **Cameras**: `out/castles.json` gives a pose on each castle from the sea, about 49 degrees down, whose line of sight clears
  the ground. It replaces the castle preset, which framed the team's worms too and put the eye in the cone.

Checks:

- `check_trail`: no trap or hole on any trail. The centreline walk is clean on all four team trails and on the Cinder
  branch.
- Walks 1.5 either side of the centreline are logged, not gated. They have 4-39 rises of 0.5-0.7 over half a unit, on the
  uphill side of the upper legs. There, two legs stack on the steep upper cone, and the bank between them leans into the
  bed's edge: a drape corner is the mean of the four columns round it, and relax() has no room for the step.
  `TRAIL_GATE=all` fails the build on them as round 3 did.
- Trail-only at 38 degrees: 16 of 16 team spawns reach their own rim end. Flood PASS. 0 detached pieces. Save with no
  warnings beyond the factory's known shell note.
- Cells 63 126 of 65 535 (43 of 64 blocks). 205 MCP calls. Two builds give the same projectHash (4cb20bb4...) and a
  byte-identical pack.

## v2 round 5 (builder's notes)

The round-4 critique applied (`terrain.mjs`, `build.mjs`, `schedule.mjs`):

- **Summit legs**: every trail's last leg is 6.6 wide (`TRAIL.top`, 3.3 half width) past its landing. It is widened on its
  uphill side only, so the leg under it in the hairpin's V keeps its room. The leg ends on a level summit terrace about 9
  across (`TRAIL.summit`, 4.4 half width).
- **Summit walls**: a low wall of dark basalt runs along the downhill edge of each last leg. It starts past the hairpin's V,
  where the leg under it no longer lies beside the wall's spot, and runs round the terrace from the downhill side, over the
  far end, to the uphill side. It is built from 2-unit voxels, one voxel thick, with its inner face on the bed's edge and a
  foot 8 deep down the cut bank. The wall's top stands about 1.0-2.2 over the bed. 1-unit walls cost 2 400 cells and left
  slots under the wall that check_trail called traps. Round 4's SW summit leg had a 5-wide bed between drops, and a worm
  walked off it from 63 to 43.5.
- **Trail material**: path02 (9), a warm brown worn-path texture. In the round-4 game, GreySlateFloor came out pale
  off-white with green blotches, close to the white castle stone.
  The in-game look was checked in testenv B with Sunstone on, using `game_material_shots` (shots `calv2r5b_mat_*`,
  `calv2r5b_close_*`, `calv2r5b_summit_*`, `calv2r5b_ov_*`). Mean colour of each material in the game:

  | Material | In game |
  |---|---|
  | path02 (the trail) | #b9913f, golden ochre |
  | chippedstone (castles) | #6d7974 |
  | flint (castles) | #5a6c7e |
  | stone (castles) | #718181 |
  | rock02 | #303c4a |
  | VolcanoRock | #1e2427 |
  | HotRocks | #683624 |

  The trail is the only warm, light ground on the cone.
- **Banks**: the banks beside the beds, within 4 past the level shoulder, take three 3 x 3 smoothing passes after relax()
  (`smoothBanks`), then relax() again.
- **Launcher pad at (9.6, -21.4)** (the tester's "south" pad): two voxels were added on its west side, their top one voxel
  over the lip. A catch slab of 1-unit voxels sits a unit under the dish's west half.
- **Leak plugs**: 2-unit voxels (3 x 2 x 3), each under the lowest ground of its footprint. The plug at (-44.1, -34.3) was
  rebuilt, and plugs were added under round 4's trail-probe misses at (54.44, -58.17), (36.95, 34.75) and (-45.21, -34.45).
  The probes' y is 8 over the ground, so query_ground atOrBelow finds the trail's surface.
- **Announcements**: at most 46 characters (`MSG_MAX`), so each fits the banner. Turn 1 says "Mines erupt! The sea rises in 3
  rounds.", and the four shore castles are called "the shore castles".

Checks:

- `check_trail`: no trap or hole. The centreline walk is clean on all four team trails.
- SW with the full gate (`TRAIL_GATE=SW`): it **fails**. The 1.5-left line has 4 refused rises, all on leg 4. The 1.5-right
  line has 7: one at the castle exit (along 34), two on leg 2 (85, 88) and four on leg 4 (146-150). Each is a rise of
  0.50-0.63 over half a unit (45-52 degrees).
- **Why SW still fails**: on the upper cone the trail climbs about 1.3 a unit of radius, so stacked legs need about 15
  between centrelines for two level 5-wide beds and a bank of 2.9 a unit between them. Near each hairpin they are closer.
  Things tried with no gain on SW, or a loss on another trail: shoulder 1.5 or 2.5, no berm, both relax() pin rules, no
  easing, an even climb, a 0.42 design grade, leg spacing 10.2 or 10.8, and 3 or 4 legs.
- Trail-only at 38 degrees: 16 of 16 team spawns reach their own rim end. Flood PASS, and the cap of 860 sits under the
  launcher pads. No detached pieces. The save warns only the factory's known shell note.
- Cells 63 968 of 65 535 (59 of 64 blocks). 228 MCP calls. Two builds give the same projectHash (42f8d388...) and a
  byte-identical pack.
