# erg-mcp: Erg for AI agents

erg-mcp is a [Model Context Protocol](https://modelcontextprotocol.io) server in front of Erg (see [erg.md](erg.md)).
It lets an MCP client such as Claude Code open an Erg project, look at the level and edit it, with the same model the
Erg panel uses: the same commands, the same undo stack, and the same patch on save. It is a developer tool in this
repository (`tools/erg-mcp/`), not part of the release zip.

It talks to the same local server the Erg panel does (`melange.asi` with the game running, or `Melange.exe` with it
closed), over the Oasis protocol (see [oasis.md](oasis.md)). It never reads `Data\` or writes `Mods\` itself.

## Running it

It needs the portable toolchain (`.\scripts\web\fetch.ps1`, which also fetches the web app's TypeScript). Build it
with:

```powershell
.\tools\erg-mcp\build.ps1
```

That writes `tools\erg-mcp\dist\erg-mcp.mjs`. `tools\erg-mcp\erg-mcp.cmd` runs it with the pinned Node from
`tools\node` (and builds it first when `dist` is missing). The server speaks MCP over stdin and stdout, one JSON-RPC
message per line; it logs to stderr only.

For Claude Code, add it to `.mcp.json` in the project you work from:

```json
{
  "mcpServers": {
    "erg": {
      "command": "C:\\path\\to\\melange\\tools\\erg-mcp\\erg-mcp.cmd",
      "env": { "MELANGE_GAME_DIR": "C:\\Games\\Worms Ultimate Mayhem" }
    }
  }
}
```

Leave out `env` to use the game while it runs (the next section). `erg-mcp.cmd --list-tools` prints every tool and its
group.

## Connecting

The first tool that needs the server connects by the first of these that is set:

1. `--url <launch url>` on the command line;
2. the `ERG_MCP_URL` environment variable;
3. `Documents\Melange\oasis_url.txt`, which the game writes while its Oasis runs (a file left by a game that has since
   closed is skipped);
4. `MELANGE_GAME_DIR`: erg-mcp starts `Melange.exe --serve --game <dir>` for that game folder (`MELANGE_EXE` names
   another `Melange.exe` than `<dir>\Melange.exe`) and stops it when it exits. Use this with the game closed.

The `connect` tool does the same, or connects to an explicit `url` or `gameDir`. A launch URL has the form
`http://127.0.0.1:8765/?k=<token>`; erg-mcp opens `ws://127.0.0.1:8765/ws?k=<token>` with no `Origin` header, which
the server accepts with the token. The token is never echoed back in a tool result.

**Test** (`level.test`) is offered only by the game's server, as in the panel.

## Units

Positions in tool results are **map units**, the level file's own units, in which one voxel is one unit, unless a
field says `world`. World units are map units x 20: what the Erg panel shows, and the unit of the water level. Voxel
cells are counted in a frame's grid: cell `(x, y, z)` with `y` up, centred on the frame's position.

## Tools

<!-- erg-mcp tools: each group adds its own subsection below. -->

### project

| Tool | What it does |
|---|---|
| `connect` | Connect or reconnect: `url`, or `gameDir` (and `exe`) to start `Melange.exe --serve`; no arguments uses the order above. Reconnecting closes the open project (refused with unsaved edits unless `discard`). |
| `list_bases` | The base levels (the game's multiplayer maps and installed map packs: key, title, theme, source) and the projects. Filters: `search`, `theme`, `source`. |
| `new_project` | Start a project from a base: `base` (a key such as `Multi.DinerMight`), `slug` (`[a-z0-9]{1,24}`), `title`. The server picks another id when the slug is taken; the result has the id. |
| `open_project` | Open a project by `id`: its base and current state with every voxel blob. One project is open at a time. |
| `describe_level` | The open level: voxel frames largest first (id, size, solid count, position and bounds in map and world units; `search`, `minVoxels`, `limit`, `allFrames`), the level's extent, spawns, objects, water, theme, time of day, and the patch's usage against the limits. |
| `save` | Save the open project. Returns the server's warnings and anything the patch validator objects to. |
| `undo`, `redo` | The editor's undo stack (500 steps), shared by every tool's edits. |
| `close` | Close the open project (refused with unsaved edits unless `discard`). |

### terrain

Sculpt with the Terrain tab's own brushes, add and remove blocks, clear the base map's terrain, write voxels directly
and paint the surround. Each call is one undo step. Strokes use the panel's brush code, so a tool edits exactly as a
mouse stroke does: carve reaches every frame the brush overlaps, fill and paint touch only the frame the stroke started
on (the anchor), and a stroke is refused as a whole when a frame would need over 2000 runs or the patch outgrow its
budget. Coordinates are in `units`: `world` (default), `map` or `grid` (voxel cells of `frame`). Brush sizes are voxels
of the anchor frame, 1-32.

| Tool | What it does |
|---|---|
| `sculpt` | One dab: `mode` (`carve`, `fill`, `paint`), `shape` (`box`, `sphere`), `size` `[x,y,z]`, `center`, and `material` 0-63 (fill and paint need it; fill also takes `materialColumn`, the material of the highest solid voxel in each column). `frame` picks the anchor, otherwise the frame holding the point. Centres snap to the middle of a voxel unless `snap` is false; `dryRun` counts without applying. Fill places voxels where the brush is, so centre it in empty cells inside a frame. |
| `sculpt_path` | A stroke along `points` (a polyline), a dab every `spacing` map units (default half the smallest brush side), all anchored on the first point's frame (or `frame`). |
| `add_block` | A solid block of `size` (each side 1-32, at most 64 blocks) and `material` under the Scene frame. `at` is the point it rests on (x and z its centre, y its bottom) or `center` is its middle, in world or map units; the corner snaps to whole units. Answers the new frame id (a negative id; it can change when the project is reopened, so describe_level again) and its bounds. |
| `remove_block` | Remove a block added in this project, by frame id. |
| `clear_base_terrain` | Carve away the solid voxels of base frames chosen by `frames`, `search` (name) and/or a `box` (voxels whose centre is in `[min, max)`); `all: true` for everything. Added blocks are kept unless `includeBlocks`. `wipe` writes empty words (0) instead of carving, which keeps each voxel's material bits; empty words merge into fewer patch runs. `dryRun` counts. |
| `fill_box_raw` | Write a box of voxels in any voxel frame, including added blocks: `state` `solid` (with `material`) or `empty`, or a raw `word`; `only` empty or solid cells; the box is `min`/`max` in `units` (default `grid`: voxel indices, `max` exclusive). Words must be valid edits of the base word. |
| `paint_surround` | The painted surround, a 100x100 height grid (heights 0-1, row-major, cell `(x, y)`). `raise`, `lower`, `flatten` (to `level`) and `smooth` dab the panel's brush at `points` (`radius` 1-25, `strength`, `passes`); `set_grid` writes `heights` (100 rows of 100, or 10 000 numbers) or a `formula` of `x`, `y`, `u`, `v`, `h` over a `region`, moved `mix` of the way. The first call switches the level to a painted surround. |
| `read_surround` | The surround heights: min, max, mean and block averages (`step`), and the exact cells of a small `region`. |

### structures

Voxel architecture. Each tool plans a structure as a list of shapes and writes it as **one undo step**: voxels of the
frames it overlaps are edited in place (rotated and scaled frames too), and where a solid part reaches outside every
frame it goes into new terrain blocks under the Scene frame (they count against the 64 added blocks). The answer lists
the voxels added, removed and repainted, the frames touched, the new blocks, the patch usage, and the structure's own
numbers (resolved sizes, where a ramp ends). `save` then sends the same patch the panel would.

Shared arguments: `material` (0-63; default the most common solid material near the structure), `place` (`auto`
default: frames, then new blocks; `existing`: never add blocks; `new`: every solid voxel into new blocks), `dryRun`
(plan and report, change nothing), `units` (`map` default, or `world`: every position and length of the call is world
units). Positions are `[x, y, z]` with `y` up; the `y` of `at`/`from` is the level of the first solid layer, so the
structure stands on the surface at that height. Sides are map directions: `n` = -z, `s` = +z, `e` = +x, `w` = -x, before
any `rotation` (degrees, counter-clockwise from above). A call that changes nothing fails (for example, outside every
frame in a level whose base has no Scene frame, such as Multi.NiceToSiege, where only `place: "existing"` can work).

| Tool | What it builds |
|---|---|
| `build_castle` | `at`, `size` [x, z], `wallHeight`, `wallThickness`, round or square corner towers (`towerShape`, `towerRadius`, `towerHeight`), a `keep` with `keepSize`, a `gate` on one side (`gateWidth`, `gateHeight`, round-topped from 3 wide), `crenellations`, `foundation` (fills the whole footprint below the floor), `rotation`. |
| `build_wall` | A wall along `points` (each with its own base y, so it follows a slope), `closed`, `height`, `thickness`, `crenellations` (merlons 2 on, 1 off; on both edge rows of walls 3+ thick, `parapet` picks left, right, both or all), `foundation`. |
| `build_tower` | Round or square, `radius`, `height`, `hollow` with `wallThickness` and a `door` (default south) of `doorWidth` x `doorHeight`, merlons round the rim, `foundation`. |
| `build_keep` | A rectangular building: `size`, `height`, hollow interior at floor level, roof slab, `door`, arrow slits (`windows`), merlons on the roof rim. |
| `build_ramp` | `from` to `to`, `width`, `maxSlope` (default 40 degrees; worms walk about 45). Steeper is refused unless `switchback` folds it into lanes side by side (`turn` left or right); the answer gives the slope, the lane count and where the top ends. `support` solid or slab. |
| `build_stairs` | `from` to `to`, `width`, `rise` per step; the tread is the run divided by the steps and must be at least 1. |
| `dig_crater` | A paraboloid bowl of `radius` and `depth` below the surface level `at.y`, a raised rim (`rimWidth`, `rimHeight`), optional `bowlMaterial` repaint. |
| `dig_tunnel` | Carves a round or square tube of `radius` along `points` through every frame it crosses; optional lining shell (`liningMaterial`, `liningThickness`). |
| `build_arch` | A freestanding gateway: opening `span` wide, straight for `rise`, then semicircular; `pier`, `depth`, `crown`, `rotation`. |
| `build_bridge` | A deck from `from` to `to` (`width`, `deckThickness`), rails, and optionally piers every `pierSpacing` down to `downTo`; `arched` fills solid between the piers and carves a round arch through each bay. |

Structures are sampled at the centres of the voxel cells, so in a frame whose position is not on the 1-unit lattice a
surface can land a voxel off from its neighbour frame; new blocks sit on the Scene frame's whole-unit lattice. The
number of added blocks is the limit for large structures: a block is at most 32 voxels a side, and only cells that
hold something are given a block.

### heightfield

Terrain from a height per column: islands, hills, volcanoes. Both tools take the same arguments; `heightfield_plan`
answers the plan and changes nothing, `heightfield_region` applies it as **one undo step**.

| Tool | What it does |
|---|---|
| `heightfield_plan` | Samples the field and packs it: which frames take which cells, the blocks it needs (size, bounds in map and world units, voxels, runs), what does not fit and why, and the budget before and after (added blocks, patch bytes, run voxels). |
| `heightfield_region` | The same plan, applied: frame fills and new blocks together. Refused when it does not fit, unless `partial`. Answers the plan, the new blocks' ids and names, and the patch usage. |

Arguments, all lengths and heights in `units` (`map` default, or `world`):

- `min`, `max`: `[x, z]` corners of the rectangle in level coordinates. They snap to the Scene frame's whole map units,
  one column per voxel: a 96 x 96 map-unit rectangle is 9216 columns (at most 1024 a side, 262 144 in all).
- `floor`: the level `y` the columns stand on. Each column is filled from `floor` (less `baseDepth`, for a skirt under
  the water line) up to `floor` + its height; a height under half a voxel leaves the column empty. `maxHeight` caps it.
- The height, as either `expr` or `grid`:
  - `expr` is a small expression language, parsed here (never run as code): numbers, `+ - * / ^` (right associative)
    and parentheses; the variables `x`, `z` (from the rectangle's centre), `u`, `v` (0 to 1 across it), `r`, `theta`
    (polar about the centre), `W`, `D` (its size), `R` (half the shorter side), `pi`, `e`; the functions `min`, `max`,
    `abs`, `sqrt`, `sin`, `cos`, `tan`, `exp`, `log`, `floor`, `ceil`, `round`, `pow`, `atan2`, `clamp(v, lo, hi)`,
    `lerp(a, b, t)`, `step(edge, v)`, `smoothstep(e0, e1, v)`, `noise2(x, z, seed)` (Perlin noise, about -1 to 1) and
    `fbm(x, z, seed, octaves)`. Division by 0 gives 0. For example a volcano 44 high with a rough skin:
    `max(0, 44*exp(-(r/26)^2) + 3*fbm(x/10, z/10, 7, 3))`.
  - `grid` is rows of heights along `z` (row 0 at the `min` corner), columns along `x`, stretched over the rectangle
    corner to corner and sampled bilinearly.
- `crater`: `radius` and `depth` lower the heights inside by `depth * (1 - (d / radius)^2)`; `rim` adds a ridge at the
  edge; `x`, `z` move its centre off the rectangle's.
- `material` (0-63, default 0) and `materials`, rules tried in order, the first whose bounds all hold wins:
  `minHeight`/`maxHeight` (the voxel's height above `floor`), `minSlope`/`maxSlope` (the column's slope in degrees) and
  `surface` (only the top n voxels of a column). For example snow above 30, grass on the top voxel where the slope is
  under 40 degrees, rock elsewhere: `[{material: 3, surface: 1, maxSlope: 40}, {material: 2, minHeight: 30}]` with
  `material: 1`.
- `reuseFrames`: `aligned` (default), `any` or `none`; see below.
- `maxBlocks`: add at most this many blocks, to keep some for other edits.
- `partial`: when the blocks do not all fit, place the ones holding the most voxels instead of refusing. A block in a
  stack is kept only when the block under it is, so nothing floats; the answer lists what was dropped.

**Packing.** A patch can add 64 blocks of at most 32 voxels a side (2 097 152 cells in all), each with at most 2000
runs, and blocks the project already has count against the 64. The rectangle is cut into 32 x 32 tiles (four offsets
are tried each way and the fewest blocks kept); each tile is stacked in 32-high slices from its lowest cell, and each
slice is trimmed to the box of the cells it needs, so the slices near a volcano's peak are small and its flanks are a
single layer. A 256 x 256 footprint 32 high takes all 64 blocks. Inside a block a column of one material is one run, so
a 32 x 32 block uses about 1024; each extra material band a column crosses adds a run, and a block that passes 2000 is
halved along its longer side, which costs blocks. When the field needs more blocks than are left the answer says how
many it needs, how many voxels would be missing and whether the peak is among them: shrink the rectangle, lower the
heights or `baseDepth`, use fewer bands, or pass `partial`.

**Frames as canvas.** The server accepts a `voxels` op on any base frame for any of its cells, empty or not, as long as
each word is valid, so filling an empty base cell costs no block, only runs (2000 per frame) and patch bytes. Before
adding blocks the plan fills the frames that overlap the field: a frame cell is filled when its centre is in the field,
and a field cell is done when its centre is in a frame cell that is solid, before or after (cells already solid count
as done, whatever their material). Frames named with SLIPPY, PERM or TEAMBASE are left alone, since the engine reads
those names as tags, and so is a frame whose runs would pass 2000. A patch cannot move a base frame, so a frame helps
only where it already lies. With `aligned` only frames on the Scene frame's lattice take part (translation-only, unit
scale, whole-unit offset), whose cells are exactly the field's: in practice blocks added earlier, since none of the
2234 voxel frames of Multi.DinerMight, Multi.NiceToSiege, Multi.StormTheCastle, Multi.HardDaysKnight and
Multi.W3DIceIceMaybe is (all but 55 are rotated or scaled, and those 55 sit off the whole units). `any` also fills
rotated and scaled frames that are not scaled under 0.9, which follows the field's surface only to within one of the
frame's voxels; the answer marks them `approximate`. On a level with no Scene frame (Multi.W3DIceIceMaybe) no block can
be added, so only frames can be filled.

### place

Spawns, level objects, the level's settings, the script, and shipping. Placing is the Erg panel's own code
(`model/placing.ts`, `model/ground.ts`), so a placed object is what the palette places, and every edit is **one undo
step**. Positions are in `units` (`world` default, or `map`); they are positions in the level, not in the detail's
frame, and the answers repeat them in the same units (rotations in degrees). A point is `pos` `[x, y, z]`, or `xz`
`[x, z]`, which starts above the whole level and drops straight onto the highest solid terrain below it (the panel's
Drop to ground; under a roof, give `pos` and `drop` from below it instead). Drops use the voxels only: over empty space,
or over a base map's non-voxel ground, they fail. An edit that would make the patch invalid is refused before it
touches the undo stack.

| Tool | What it does |
|---|---|
| `place_spawn_knots` | `mode` `knots` (default) or `random`, and `spawns`: `[{worm: 0-7, pos or xz, drop}]`. An existing `WORMn` is moved, a missing one added. Answers the spawns and `missingKnots` (a match in `knots` mode needs all eight). |
| `place_object` | `type` `crate` (`kind` weapon, health or utility; `contents`, `count`, `amount`, `hitpoints`, `parachute`), `health` (a health crate), `telepad` (a pair in a new group; `pos2` for the second pad, default 2 map units along x), `trigger` (`index`, `radius`, `teamCollect`, `teamDestroy`, `hitpoints`, `wormCollect`), `mine`, `oildrum`, `minefactory` (one per level). Crate contents are the install's names (`kWeaponBazooka`); a short name that matches one is replaced by it, anything else is kept with a warning. Crates, telepads, triggers and the mine factory make the project format v2. |
| `move` | `id` or `ids`, and `pos` (one detail) or `by` (an offset); `drop` drops the result. |
| `rotate`, `scale` | `rot` (absolute) or `by`, in degrees about x, y, z; `scale` `[x,y,z]`, `uniform` or `by` (a factor). Refused for details in frames that swing in the game. |
| `remove` | Delete details; an added level object's knot takes its object with it. |
| `drop_to_ground` | Drop details onto the terrain below them. |
| `list_objects` | Spawns, level objects with their settings, placed mines and drums (`role` and `search` pick other details), the object count, and the editor's checks (missing knots, objects under water, single telepads). |
| `set_water`, `set_theme`, `set_time_of_day`, `set_surround_mode` | `level` in world units (-1000 to 1000) or `useDefault`; `theme`; `timeOfDay` (DAY, EVENING, NIGHT); `mode` copy, none, flat or paint. |
| `get_script`, `put_script` | The project's Lua script. `put_script` writes it to the server at once (not an undo step, not part of `save`) and answers `problems` with line numbers and `syntaxChecked` (false when the server cannot compile Lua; the game checks it on `test`). |
| `export` | `modId`, `name`, `version` (default 1.0.0), `mode` `source` (default, shareable) or `install` (built files, this machine only), `survivor`. Saves the project first when it has unsaved edits. |
| `build` | `modId`: build a source-mode mod's levels into map files (game closed). |
| `test` | `level.test` in the running game: saves first, follows the `erg` channel and waits for `playing` (`waitFor` `armed` stops at the armed state; `timeoutSec`, default 60). Answers every state with its time; `tod` picks the time of day. Needs the game running at its main menu: a standalone `Melange.exe --serve` has no Test. |
| `levels_live` | `modId` and `on`: show a pack's levels without a restart (game running, offline, at the menu). |

### inspect

Read-only looks at the open level; none of these changes the project. Coordinates are map units unless `units` is
`world`; `step` (the size of a sample) is in the same units. A region that leaves out an edge means the level's extent
on that side. Ground is the highest solid voxel of a column over every voxel frame, read at the centre of each sample
cell. A frame that is only moved is read cell by cell (exact tops); a rotated or scaled one is sampled every half voxel
(tops good to 0.25 voxel, and ground thinner than that can be missed).

| Tool | What it does |
|---|---|
| `heightmap_ascii` | Top-down text map (x right, z down, at most 200 x 200 cells): a character `0-9a-z` per cell for the top height in equal bands (listed in the key), `.` for no ground, `S` spawns, `C` crates, `T` telepads, `R` triggers, `F` the mine factory, `O` other placed objects (`markers: false` leaves them out). Rows are labelled with z and a ruler gives x. `x0`, `z0`, `x1`, `z1`, `units`, `step`. |
| `render_topdown` | PNG top view as image content: tinted by material, shaded by height (`shading`: `both`, `height`, `material`), ground under the water level blue, markers as in the text map, axis ticks in the margin. The text part says how to turn a pixel into x and z, the material colours (with the record names) and where each marker is. `step`, `scale` (pixels per sample), region as above. |
| `render_side` | PNG cross-section along the line `x0, z0` to `x1, z1`: every solid stretch (overhangs and caves show), water, objects within 1.5 voxels of the line, a y ruler and a distance ruler. `y0` and `y1` clip the height; `scale` is pixels per voxel. The text part has the top profile along the line. |
| `query_ground` | One column `x`, `z`: the top of the highest solid voxel (map and world y), its frame and material, `standOn` (the y a placed object rests on), whether it is under the water, and every solid stretch of the column. `below` asks for the ground under a height, such as the floor under a bridge. |
| `list_materials` | The level's material file (names in index order, solid voxels of each in the open project, the render colour) and the theme's object palette (`name`, `resource`, `role`; `search`, `role`, `limit`). `only` leaves one half out. With no project open, pass `theme` and/or `materialFile`; with neither, it lists the themes and material files. |

The water line is drawn only when the project sets a water level; a level that keeps its own water height has none the
editor knows, and the renders say so. The 100 x 100 surround terrain is not part of these views.

<!-- end of erg-mcp tools -->

Edits stay in memory until `save`, as in the panel; closing or reconnecting with unsaved edits is refused unless the
tool is told to discard them. A failed tool answers with an error result (`isError`) that says why.

## Limits

The server's patch limits (see [erg.md](erg.md#limits)) apply as in the panel, and `describe_level` reports the usage:
at most 64 added blocks of 1-32 voxels a side, 2000 runs of voxel changes per frame, 20 000 ops, a 4 MB patch and 256
objects. Voxel edits that would pass the runs or size limit are refused before they are applied. The game's own maps
are made of many small voxel frames (a few hundred, none longer than 32 voxels a side), not one large terrain grid.

## Adding tools

A tool group is one file in `tools/erg-mcp/src/tools/` that exports `tools`, an array of tools:

```ts
import type { Tool } from "../registry";

export const tools: Tool[] = [{
  name: "my_tool",                       // snake_case, unique across every group
  description: "What it does, for the model.",
  inputSchema: { type: "object", additionalProperties: false, required: ["frame"], properties: { frame: { type: "integer" } } },
  handler(session, args) { /* ... */ },
}];
```

`build.ps1` registers every file there; nothing else needs editing. The arguments are checked against `inputSchema`
(types, `required`, `enum`, ranges, unknown keys) before the handler runs. A handler returns a string, an object (sent
as JSON), or `{ content: [...] }` with text and `image(bytes)` parts; it throws `ToolError` for a failure the model
should read. It edits only through the open project's `EditorStore` (`session.store`): `store.exec(command)` with the
editor's commands, so every edit is one undo step and `save` sends the panel's patch.

- `src/session.ts`: `session.store`, `session.requireProject()`, `session.levels()`, `session.client()`.
- `src/core.ts`: the editor's model, imported from the web app's sources (`web/src/sdk/erg` and the pure modules of
  `web/src/panels/erg`). Import a single module there, never a barrel that also exports components; the build refuses
  a bundle that pulls in three.js or Preact.
- `src/lib/voxel.ts`: frame grids (`grids`, `grid`), cells (`getVoxel`, `setVoxel`, `voxelIndex`, `cellAt`, `cellCentre`), map,
  world and grid coordinates (`toWorld`, `toMap`, `gridToMap`, `mapToGrid`, `gridToWorld`, `worldToGrid`), voxel words
  (`isSolid`, `filled`, `carved`, `painted`, `withSecond`), the patch's runs (`columnRuns`, `encodeRuns`, `runBytes`),
  limits (`usage`, `limitProblems`, `newFrameProblem`), and `VoxelEdit`, a batch of voxel writes committed as one
  undoable step after the panel's checks.

Tests: `.\tools\erg-mcp\test.ps1` builds the bundle and runs `tools/erg-mcp/test/`: the MCP protocol, the tools
against the mock Oasis server (`web/test/e2e/mock-server.mjs`) and the helpers on the synthetic levels.
