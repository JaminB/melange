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
