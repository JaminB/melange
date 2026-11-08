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

**Projects folder.** Each server keeps projects in its own game folder's `[Erg] ProjectsDir` (in that game's
`Melange.ini`), else `Documents\Melange\erg\projects`; `connect`, `list_bases` and `new_project` answer it as
`projectsDir`. **Test** runs in the game and reads the game's folder, so a project made through `Melange.exe --serve`
for another install (or one whose `ProjectsDir` differs) is not seen by that game's Test: build and test against the
same install, or give both the same `ProjectsDir`.

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
| `connect` | Connect or reconnect: `url`, or `gameDir` to start `Melange.exe --serve` (`MELANGE_EXE` names another `Melange.exe`; the tool takes no program path); no arguments uses the order above. Reconnecting closes the open project (refused with unsaved edits unless `discard`). Answers the server's `projectsDir`. |
| `list_bases` | The base levels (the game's multiplayer maps and installed map packs: key, title, theme, source), the projects and `projectsDir`. Filters: `search`, `theme`, `source`. |
| `new_project` | Start a project from a base: `base` (a key such as `Multi.DinerMight`), `slug` (`[a-z0-9]{1,24}`), `title`. Refused when a project with that slug exists (open it, `delete_project` it, or pass `allowRename: true` to take the server's next free id, such as `caldera2`, with a `warning` in the answer). |
| `delete_project` | Delete a project's folder (patch, script, metadata) by `id`. Refused while it is open, here or in any other server (the game, `Melange.exe`): `close` it first. Exported mods are not touched; it cannot be undone. |
| `open_project` | Open a project by `id`: its base and current state with every voxel blob. One project is open at a time. |
| `describe_level` | The open level: voxel frames largest first (id, size, solid count, position and bounds in map and world units; `search`, `minVoxels`, `limit`, `allFrames`), the level's extent, spawns, objects, water, theme, time of day, the patch's usage against the limits, the game's voxel cell budget (`limits.voxelCells`: used, max 65535, free) and `engineWarnings` (blocks over 31 a side, cells over the budget). |
| `save` | Save the open project. Refused, with nothing sent, while the patch breaks a limit or the patch validator objects (`describe_level` lists them under `problems`; undo the edit). Returns the server's warnings. |
| `undo`, `redo` | The editor's undo stack (500 steps), shared by every tool's edits. |
| `close` | Close the open project (refused with unsaved edits unless `discard`). |

### terrain

Sculpt with the Terrain tab's own brushes, add and remove blocks, clear the base map's terrain, write voxels directly
and paint the surround. Each call is one undo step. Strokes use the panel's brush code, so a tool edits exactly as a
mouse stroke does: carve reaches every frame the brush overlaps, fill and paint touch only the frame the stroke started
on (the anchor) unless `allFrames` is set, and a stroke is refused as a whole when a frame would need over 2000 runs or
the patch outgrow its budget. Coordinates are in `units`: `world` (default), `map` or `grid` (voxel cells of `frame`). Brush sizes are voxels
of the anchor frame, 1-32.

| Tool | What it does |
|---|---|
| `sculpt` | One dab (it and `sculpt_path` take `allFrames`: fill and paint also edit every other frame the brush overlaps, rotated and scaled ones at their cell centres, as carve does; default false, the panel's anchor-only brush): `mode` (`carve`, `fill`, `paint`), `shape` (`box`, `sphere`), `size` `[x,y,z]`, `center`, and `material` 0-63 (fill and paint need it; fill also takes `materialColumn`, the material of the highest solid voxel in each column). `frame` picks the anchor, otherwise the frame holding the point. Centres snap to the middle of a voxel unless `snap` is false; `dryRun` counts without applying. Fill places voxels where the brush is, so centre it in empty cells inside a frame. |
| `sculpt_path` | A stroke along `points` (a polyline), a dab every `spacing` map units (default half the smallest brush side), all anchored on the first point's frame (or `frame`). `taper` is the brush size at the end as a multiple of `size` (0.3 narrows to 30 %, linear along the path's length, never under 1 voxel). `followSurface: "downhill"` makes the path from the first point (the ground under it): it walks downhill over the voxel ground by steepest descent (16 directions, a step of `spacing`; level ground is crossed straight on for up to 8 steps; it never climbs) until ground at or under the water, a pit, `maxLength` (map units, default 400), `maxSteps` (default 1000) or the edge of the ground, and dabs along it with each centre `depth` map units above the ground's top (default -0.5, the middle of an unscaled top voxel). The answer's `followSurface` gives the `path` (in `units`), why it stopped (`stop`), its steps and drop. A lava river is a paint stroke with `followSurface`, `allFrames` and a `taper`. Not with units `grid`. |
| `add_block` | A solid block of `size` (each side 1-31, at most 64 blocks) and `material` under the Scene frame, optionally `scale` (0.5-4 map units per voxel, default 1; the game's own maps use frames scaled 1.2-2.0, and 0.75 played in the game: finer voxels for a crater lip or a summit at the same cost per cell). `dryRun` answers the block's cost (the game's cells before and after, its bounds) without adding it. `at` is the point it rests on (x and z its centre, y its bottom) or `center` is its middle, in world or map units; the corner snaps to whole units. Refused when the level's voxel frames would pass the game's 65 535 cells. Answers the new frame id (a negative id; it can change when the project is reopened, so describe_level again), its bounds and the cells left. |
| `remove_block` | Remove a block added in this project, by frame id. |
| `clear_base_terrain` | Carve away the solid voxels of base frames chosen by `frames`, `search` (name) and/or a `box` (voxels whose centre is in `[min, max)`); `all: true` for everything. Added blocks are kept unless `includeBlocks`. `wipe` writes empty words (0) instead of carving, which keeps each voxel's material bits; empty words merge into fewer patch runs. `dryRun` counts. |
| `fill_box_raw` | Write a box of voxels in any voxel frame, including added blocks: `state` `solid` (with `material`) or `empty`, or a raw `word`; `only` empty or solid cells; the box is `min`/`max` in `units` (default `grid`: voxel indices, `max` exclusive). Words must be valid edits of the base word. |
| `paint_surround` | The painted surround, a 100x100 height grid (heights 0-1, row-major, cell `(x, y)`). `raise`, `lower`, `flatten` (to `level`) and `smooth` dab the panel's brush at `points` (`radius` 1-25, `strength`, `passes`); `set_grid` writes `heights` (100 rows of 100, or 10 000 numbers) or a `formula` of `x`, `y`, `u`, `v`, `h` over a `region`, moved `mix` of the way. The first call switches the level to a painted surround. |
| `read_surround` | The surround heights: min, max, mean and block averages (`step`), and the exact cells of a small `region`. |

### structures

Voxel architecture. Each tool plans a structure as a list of shapes and writes it as **one undo step**: voxels of the
frames it overlaps are edited in place (rotated and scaled frames too), and where a solid part reaches outside every
frame it goes into new terrain blocks under the Scene frame (they count against the 64 added blocks and the game's 65 535
voxel cells, and a structure whose new blocks would pass either is refused). The answer lists
the voxels added, removed and repainted, the frames touched, the new blocks, the patch usage, and the structure's own
numbers (resolved sizes, where a ramp ends). `save` then sends the same patch the panel would.

Shared arguments: `material` (0-63; default the most common solid material near the structure), `place` (`auto`
default: frames, then new blocks; `existing`: never add blocks; `new`: every solid voxel into new blocks), `dryRun`
(plan and report, change nothing), `units` (`map` default, or `world`: every position and length of the call is world
units), `blockScale` (map units per voxel of the new blocks, 1-4, default 1: fewer cells for the same structure in
coarser voxels; use `place: "new"` so all of it is on that lattice) and `packing` (`tiles`, the default: tiles of up
to 31 a side, each trimmed to what it holds; `parts`: a block per part of the structure, such as each wall, tower and
the keep, merged where that costs at most an eighth more cells, so the air between parts, a courtyard, is in no block:
a 24 x 24 castle with a keep takes about 40 % of the cells, in more blocks). Positions are `[x, y, z]` with `y` up; the `y` of `at`/`from` is the level of the first solid layer, so the
structure stands on the surface at that height. Sides are map directions: `n` = -z, `s` = +z, `e` = +x, `w` = -x, before
any `rotation` (degrees, counter-clockwise from above). A call that changes nothing fails (for example, outside every
frame in a level whose base has no Scene frame, such as Multi.NiceToSiege, where only `place: "existing"` can work).

| Tool | What it builds |
|---|---|
| `build_castle` | `at`, `size` [x, z], `wallHeight`, `wallThickness`, round or square corner towers (`towerShape`, `towerRadius`, `towerHeight`), a `keep` with `keepSize`, a `gate` on one side (`gateWidth`, `gateHeight`, round-topped from 3 wide), `crenellations`, `foundation` (fills the whole footprint below the floor), `rotation`. `scale` (1-4) lays the castle out in voxels of that many map units (lengths stay map units; walls, merlons and gates keep their rhythm in voxels) and gives its new blocks that scale: about 1/scale^3 of the cells. The answer's `spawnSpots` lists open courtyard positions (between the curtain wall and the keep, never inside a wall passage), the middle of each wall-walk and each tower top; after a build each is checked against the voxels (`ok`, `standOn`: ground within 1.5 map units of the design and 1.5 of air above), ready for `place_spawn_knots` as `pos`. |
| `build_wall` | A wall along `points` (each with its own base y, so it follows a slope), `closed`, `height`, `thickness`, `crenellations` (merlons 2 on, 1 off; on both edge rows of walls 3+ thick, `parapet` picks left, right, both or all), `foundation`. |
| `build_tower` | Round or square, `radius`, `height`, `hollow` with `wallThickness` and a `door` (default south) of `doorWidth` x `doorHeight`, merlons round the rim, `foundation`. |
| `build_keep` | A rectangular building: `size`, `height`, hollow interior at floor level, roof slab, `door`, arrow slits (`windows`), merlons on the roof rim. |
| `build_ramp` | `from` to `to`, `width`, `maxSlope` (default 40 degrees; worms walk about 45). Steeper is refused unless `switchback` folds it into lanes side by side (`turn` left or right); the answer gives the slope, the lane count and where the top ends. `support` solid or slab. `mode: "cut"` cuts the walking surface into the terrain that is there instead of building on it (a path up a cone, at no block cost): the voxels above the surface, over the ramp's width and `headroom` high (default 3 map units), are carved in every frame it crosses, and the support fills only empty cells under it where it passes over air, into existing frames only (`place: "existing"`; any other `place` is refused). A switchback's later lanes run back over the flank, so where they pass over air outside every frame there is no ramp: pick `from` and `to` so the lanes stay in the terrain. |
| `build_stairs` | `from` to `to`, `width`, `rise` per step; the tread is the run divided by the steps and must be at least 1. |
| `dig_crater` | A paraboloid bowl of `radius` and `depth` below the surface level `at.y`, a raised rim (`rimWidth`, `rimHeight`), optional `bowlMaterial` repaint. |
| `sculpt_crater` | A crater shaped into the terrain that is there (a volcano's summit), `at` [x, z]: the ground is read first, the rim crest is its mean height round the `radius` circle plus `rimHeight`, the bowl is `depth` below the crest (flat inside `floorRadius`), and outside the crest the rim falls back to the ground's own slope over `rimWidth` (default radius / 2), so there is no separate wall ring. `bands` paint the new surface `bandDepth` deep (default 2) by signed distance from the crest (`from`/`to`, negative inside the bowl), first match wins. The rim is raised in `material`, by default each column's own top material (no new material change down a column, so no new runs: an added block holds at most 2000, and every band painted adds about one a column); fills above the old ground go into existing frames, else new blocks (`blockScale` gives them the field's voxel size). Refused where under half the rim circle has ground. |
| `dig_tunnel` | Carves a round or square tube of `radius` along `points` through every frame it crosses; optional lining shell (`liningMaterial`, `liningThickness`). `floor: "flat"` digs a walkable tunnel: the points are on its floor (y = the level worms walk on), a flat floor that runs straight between them, `2 x radius` wide and `headroom` high (default 2 x radius, at least 2; round: the top `radius` is a half circle), with rounded bends and a floor slab `floorThickness` thick (default 1, 0 for none) filled in the structure's material where the cells under the floor are empty (across a cave; outside every frame it goes into new blocks unless `place: "existing"`). A segment steeper than `maxSlope` (default 40 degrees) is refused; the answer gives `steepestDegrees`. |
| `build_arch` | A freestanding gateway: opening `span` wide, straight for `rise`, then semicircular; `pier`, `depth`, `crown`, `rotation`. |
| `build_bridge` | A deck from `from` to `to` (`width`, `deckThickness`), rails, and optionally piers every `pierSpacing` down to `downTo`; `arched` fills solid between the piers and carves a round arch through each bay. |

Structures are sampled at the centres of the voxel cells, so in a frame whose position is not on the 1-unit lattice a
surface can land a voxel off from its neighbour frame; new blocks sit on the Scene frame's whole-unit lattice. Sizes are
map units whatever the frame's scale: a tower of radius 5 built (`place: "existing"`) into a heightfield of `voxelSize`
2.5 is 2 of its voxels across, so detail finer than the frame's voxels is lost. The added blocks and the cells are the
limits for large structures: a block is at most 31 voxels a side, its whole box counts against the cells, and only cells
that hold something are given a block.

### heightfield

Terrain from a height per column: islands, hills, volcanoes. Both tools take the same arguments; `heightfield_plan`
answers the plan and changes nothing, `heightfield_region` applies it as **one undo step**.

| Tool | What it does |
|---|---|
| `heightfield_plan` | Samples the field and packs it: which frames take which cells, the blocks it needs (size, bounds in map and world units, voxels, cells, runs), what does not fit and why, and the budget before and after (added blocks, the game's voxel cells, patch bytes, run voxels). |
| `heightfield_region` | The same plan, applied: frame fills and new blocks together. Refused when it does not fit, unless `partial`. Answers the plan, the new blocks' ids and names, and the patch usage. |

Arguments, all lengths and heights in `units` (`map` default, or `world`):

- `voxelSize`: map units per voxel, 1-4 (default 1), always in map units: the new blocks' scale. The rectangle is
  sampled one column per voxel on a lattice of that size from the Scene frame's origin, and heights become whole
  voxels of it.
- `min`, `max`: `[x, z]` corners of the rectangle in level coordinates. They snap to whole voxels of the lattice: at
  `voxelSize` 1 a 96 x 96 map-unit rectangle is 9216 columns, at 2.5 it is 1444 (at most 1024 a side, 262 144 in all).
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
- `materials` rules may also bound `minDepth`/`maxDepth` (the voxel's depth under its column's top, in `units`: rock
  under a skin, lava deep in a crater), `exposed` (true: only voxels with air above or beside them, cliff faces
  included; false: only buried ones) and `jitter` {`amount`, `scale` (default 8), `seed`}: Perlin noise moves the rule's
  `minHeight`/`maxHeight` by up to `amount`, so a band's edge wanders instead of running level round a cone.
- `edges`: how a height becomes whole voxels: `round` (default; a cone becomes concentric terraces), `dither` (a 4 x 4
  ordered pattern) or `noise` (a hash per column): the terrace edges break up into a ragged line at the same cost. For
  smooth slopes use `heightfield_drape` (below).
- `packer`: `tiles` (default, a grid of 31 x 31 columns), `greedy` (tiles that start on the field's own columns: fewer
  blocks for a ring or a sparse field) or `best` (both, keeping the fewer blocks). `costAt`: also plan the same field at
  up to 8 voxel sizes and answer each one's blocks and cells (`costAt`), to choose a size first.
- `reuseFrames`: `aligned` (default), `any` or `none`; see below.
- `maxBlocks` and `maxCells`: add at most this many blocks, and blocks holding at most this many of the game's cells,
  to keep some of each for other edits (castles).
- `partial`: when the blocks do not all fit, place the ones holding the most voxels instead of refusing. A block in a
  stack is kept only when the block under it is, so nothing floats; the answer lists what was dropped.
- `runsHeadroom`: leave this many of each new block's 2000 runs free for later edits (`sculpt_crater`, painted bands,
  structures built into the blocks), which are refused when a block would pass 2000. Default 0; blocks halve sooner.
- `centre`: `[x, z]`, the origin of the expression's `x`, `z`, `r` and `theta` (and the crater's default centre)
  instead of the rectangle's centre, which snaps to the lattice of the voxel size.
- `clip` and `exclude`: areas, each a circle (`x`, `z`, `radius`) or a rectangle (`min`, `max`). `clip` keeps only the
  columns whose centre is inside one; `exclude` leaves out the columns lying wholly inside one.

**Fine voxels where they matter.** The game's voxels are cubes (a word holds solid, a material and a second-material
paint mask, no slope), so a coarse `voxelSize` makes every slope a staircase of its size. Detail costs cells, so spend
it where it shows: two calls with the same rectangle, `expr` and `centre`, one coarse with `exclude: [circle]` and one
fine with `clip: [circle]` (the same circle). They sample the same surface; the coarse field keeps the columns the
circle only partly covers, so the two overlap at the edge and no gap opens. For example (tried on Multi.DinerMight) a
240 x 240 island with a 56-high cone: the lowlands at 3 (16 blocks, 24 756 cells), the cone inside r 40 at 2.25 (9
blocks, 24 479 cells); the whole island at 1.5 would need 195 696 cells. Use `x`, `z`, `r` in such an expression (`u`,
`v`, `W`, `D` follow the snapped rectangle), and cap each call's `maxBlocks` so the two share the 64.

**Packing.** Two budgets bind. A patch can add 64 blocks of at most 31 voxels a side (the game asserts on 32), each
with at most 2000 runs. And the game counts the cells of every voxel frame of the level, base and added, solid or
empty (X x Y x Z of each), in 16 bits: past 65 535 it crashes loading the map, so the whole box of every block counts,
not its solid voxels. The base's frames already take 8 000-46 000 of them (`describe_level` `limits.voxelCells`; clearing
base terrain frees none), which leaves room for roughly 230 x 230 columns one voxel thick, whatever the voxel size: a
larger `voxelSize` covers more ground and height with the same cells, in coarser voxels. Blocks the project already
has count against both. The rectangle is cut into 31 x 31 tiles (four offsets are tried each way and the fewest blocks
kept); each tile is stacked in 31-high slices from its lowest cell, and each slice is trimmed to the box of the cells
it needs. While those boxes hold more cells than are left (or `maxCells`), and blocks are left, the box whose halving
(in x, z or y, each half trimmed again) saves the most cells is halved, so a cone's flanks cost about their slope
rather than their tile's whole height. Inside a block a column of one material is one run, so a 31 x 31 block uses
about 961; each extra material band a column crosses adds a run, and a block that passes 2000 is halved along its longer
side, which costs blocks. When the field does not fit the answer says what binds first: `cell budget exceeded (N needed, M allowed)` when the
cells do (the packer then spends blocks halving boxes, so a block count past 64 is a consequence), with
`suggestedVoxelSize` (the smallest voxelSize, in steps of 0.25, at which the same field fits whole, and its blocks and
cells), else the blocks it needs; then how many voxels would be missing and whether the peak is among them: shrink the
rectangle, lower the heights or `baseDepth`, raise `voxelSize`, use fewer bands, or pass `partial`. For example on Multi.DinerMight (11 652 base cells, 53 883 left)
an island 250 map units across with a cone 60 high is about 25 600 voxels at `voxelSize` 2.5: it packs into 17 blocks of
50 215 cells, or with `maxCells: 40000` into 23 blocks of 39 926; at `voxelSize` 1 it would need over a million cells,
and at 2 about 60 000 even after halving. It was played in the game: it loads, worms walk on it and weapons carve it.

**Materials and the game's landscape mesh.** In that test a `surface: 1` rule (a one-voxel skin of grass over rock on
every column) made the game's landscape combiner run out of vertices (`CombineLandscapeAction` asserts in the log and
holes in the drawn terrain); the same island in height bands, or in one material, had none. The plan adds a `notes`
entry when a surface rule covers more than 2500 columns: on a large field use `minHeight`/`maxHeight` or slope bands,
and Test the level.

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

### drape

| Tool | What it does |
|---|---|
| `heightfield_drape` | The heightfield tools' arguments (the field, `voxelSize`, `materials`, `clip`, `maxBlocks`, ...), raised as a smooth skin instead of voxel terraces: blocks of up to 31 x 31 columns, `thickness` voxels thick (1-8, default 2), whose **corner offsets** put each column on the field's height, over the stepped field cut just under the skin (`core`, default true; `false` leaves a hollow skin for about a fifth of the cells). `dryRun` answers the plan: the skin's blocks (size, columns, the offsets' range), the core's blocks and cells, the budget. One undo step. |

**Corner offsets.** Every voxel frame of a .xan has a HeightMap: a float per corner of its columns ((X+1) x (Z+1), in
voxels). The game moves each column's vertices, its bottom too, up by the bilinear offset of the column's corners, and
uses the moved columns for drawing and for collision (seen in the game on 2026-11-07: a 24 x 4 x 24 block with offsets
rising 3, 6 and 20 voxels across it drew and collided as a sheared slab; worms stood on it, landRay heights matched the
offsets exactly; the game's own maps use -1.9 to 3.6, for curved roofs). Neighbouring columns share a corner, so an
offset cannot smooth a step between two columns of different heights: only a block whose columns are all the same
height can follow a surface, which is what the skin is. A patch carries them on `addFrame` as `corners` (-64..64,
written only when set); a block without them has zeros, as before. Tried on Multi.DinerMight: a cone 120 map units
across, 30 high, at `voxelSize` 2 draped in 4 skin blocks (4608 cells) over 4 core blocks: smooth flanks in the game,
worms placed with `xz` stood on them, and a bazooka dug into the skin.

The skin is read at its sheared height by `query_ground`, the renders, `check_reachability`, `place_*` drops and the
Erg panel's drop to ground. The structure tools and `heightfield_region`'s frame filling leave a draped block alone
(its cells are not where its grid says), and the sculpt brushes edit it as if unsheared: build structures and carve
before draping, or beside the skin.
The Erg panel's 3D view draws a draped block unsheared.

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
| `set_water`, `set_theme`, `set_time_of_day`, `set_surround_mode` | `level` in world units (-1000 to 1000) or `useDefault`; `theme`, and `materialFile` (`"theme"` for the theme's own file, a file name, or `"keep"`, the default: the voxels' materials are indices into the material file, which a theme change otherwise leaves as it is); `timeOfDay` (DAY, EVENING, NIGHT); `mode` copy, none, flat or paint. |
| `get_script`, `put_script` | The project's Lua script. `put_script` writes it to the server at once (not an undo step, not part of `save`) and answers `problems` with line numbers and `syntaxChecked`: the game compiles it with its own Lua 5.0, `Melange.exe` with a stock Lua 5.0.1 parser (older versions: `false`, checked on `test`). `wum.sim.announce(text, seconds)` puts a message on every player's screen (see [erg.md](erg.md#level-scripts)). |
| `export` | `modId`, `name`, `version` (default 1.0.0), `mode` `source` (default, shareable) or `install` (built files, this machine only), `survivor`. Saves the project first when it has unsaved edits. |
| `build` | `modId`: build a source-mode mod's levels into map files (game closed). |
| `test` | `level.test` in the running game: saves first, follows the `erg` channel and waits for `playing` (`waitFor` `armed` stops at the armed state; `timeoutSec`, default 60). Answers every state with its time; `tod` picks the time of day. The match is the game's Quick Game (a human team against a computer team, its own scheme: turn time, team sizes and the scheme's options are not chosen here). Enabled content mods with sim code run in it beside the level script: the answer names them (`simMods`), and `cleanMods: true` refuses to start while there are any (disable them in the game; they apply at the next launch). Needs the game running at its main menu: a standalone `Melange.exe --serve` has no Test. |
| `levels_live` | `modId` and `on`: show a pack's levels without a restart (game running, offline, at the menu). |

### inspect

Read-only looks at the open level; none of these changes the project. Coordinates are map units unless `units` is
`world`; `step` (the size of a sample) is in the same units. A region that leaves out an edge means the level's extent
on that side. Ground is the highest solid voxel of a column over every voxel frame, read at the centre of each sample
cell. A frame that is not rotated (moved, or scaled along its axes, as every added block whatever its scale) is read
cell by cell (exact tops); a rotated one is sampled every half voxel (tops good to 0.25 voxel, and ground thinner than
that can be missed). The renders tint each material with its real colour (`palette: "real"`, the default: the average
of the material's texture, from the server; `"index"` gives a distinct hue per index to tell neighbours apart).

| Tool | What it does |
|---|---|
| `heightmap_ascii` | Top-down text map (x right, z down, at most 200 x 200 cells): a character `0-9a-z` per cell for the top height in equal bands (listed in the key), `.` for no ground, `S` spawns, `C` crates, `T` telepads, `R` triggers, `F` the mine factory, `O` other placed objects (`markers: false` leaves them out). Rows are labelled with z and a ruler gives x. `x0`, `z0`, `x1`, `z1`, `units`, `step`. |
| `render_topdown` | PNG top view as image content: tinted by material, shaded by height (`shading`: `both`, `height`, `material`), ground under the water level blue, markers as in the text map, axis ticks in the margin. The text part says how to turn a pixel into x and z, the material colours (with the record names) and where each marker is. `step`, `scale` (pixels per sample), region as above. |
| `render_side` | PNG cross-section along the line `x0, z0` to `x1, z1`: every solid stretch (overhangs and caves show), water, objects within 1.5 voxels of the line, a y ruler and a distance ruler. `y0` and `y1` clip the height; `scale` is pixels per voxel. Ground of stacked or overlapping frames is one stretch: only its top (air above) gets the light edge, so block boundaries leave no seam. The text part has the top profile along the line. |
| `query_ground` | One column `x`, `z`: the top of the highest solid voxel (map and world y), its frame and material, `standOn` (the y a placed object rests on), whether it is under the water, and every solid stretch of the column. `below` asks for the ground under a height, such as the floor under a bridge. |
| `list_materials` | The level's material file (names in index order, solid voxels of each in the open project, the colour, `textured`: whether the server found its texture) and the theme's object palette (`name`, `resource`, `role`; `search`, `role`, `limit`). `only` leaves one half out. With no project open, pass `theme` (its own file is listed, `ThemeCamelot\ThemeCamelot.txt` for CAMELOT) and/or `materialFile`; with neither, it lists the themes and material files. A `materialFile` written with `/`, in another case, or that lost its backslash (`Mapsdinermight.txt`, a JSON `"\d"` read as `d`) is matched to the listed file, with a `note`; one that is not found answers the listed files. |
| `preview_materials` | A swatch sheet (image content) of a material file: a tile per material in its real colour, labelled with its index, `?` where the texture was not found, and the list of names and colours. Default the open project's file; or `materialFile`, or a `theme`'s own. |
| `check_reachability` | Can each spawn walk to the summit? A worm-walk graph over the voxels: the tops of solid ground with `headroom` (default 1.5 map units) of air, a column every `step` (default 1), joined where a worm steps up at most `maxStep` (0.5) or climbs a riser no steeper than `maxSlope` (45 degrees) over one voxel of the upper ground (the game draws a voxel step as a slope, so a cone at voxel size 2.75 is walked), drops at most `maxDrop` (8), or with `maxGap` jumps a gap to ground no higher than `maxStep`; a diagonal step needs one side column walkable too. From every spawn knot (or `from` points) to the target (`target` x, z, radius: the highest ground there; default the highest ground of the region). Per spawn: `reachable`, the walk and waypoints, or the closest point reached and `blockedAt` (where, and the climb that stops it). Ground at or under the project's water level is not walked. Walking only: no rope, jetpack or girders. |

The water line is drawn only when the project sets a water level; a level that keeps its own water height has none the
editor knows, and the renders say so. The 100 x 100 surround terrain is not part of these views.

### render

A perspective look at the open level, to judge silhouettes (a volcano's cone, a castle's towers) before a Test.
Read-only. Positions are map units unless `units` is `world`.

| Tool | What it does |
|---|---|
| `render_view` | PNG 3/4 view (image content): every exposed face of every voxel frame, rotated and scaled ones through their own matrix, rasterised into a z-buffer in the material's real colour (`palette` as the inspect renders), lit per face by the time of day's sun (or moon) and an ambient term, so tops and each side of a voxel step differ; silhouette edges darkened (`edges`), the project's water level as a translucent plane, spawns and objects as small billboards in the top view's marker colours (hollow when terrain hides them), a sky gradient by `timeOfDay` (default the level's), optional `fog` (the distance that hides about 63 %). Camera: `pos` and `target`, `fov` (vertical, default 60, as `game_camera`); or `view` `nw`, `ne`, `sw`, `se` (default), from that side 35 degrees up at the distance that fits every frame's solid voxels in the level or a `region` (`clip` draws only what is inside it); `orbit` draws the four in a 2 x 2 sheet; `game` mimics the turn camera, 170 world units from `at` and 22 degrees up, on the side away from the level's middle. `azimuth` (degrees clockwise from north, -z), `elevation` and `distance` override a preset; `target` moves its centre. `width` x `height` default 960 x 540, at most 1920 x 1080; `antialias` renders 2 x 2 samples a pixel. The text part gives each camera in map and world units, the time taken, the frames drawn and each marker's pixel. A 960 x 540 view of the 400-frame synthetic level (117 000 cells, 45 000 faces) takes about 0.1 s, 0.2 s antialiased. |

Faces are flat-shaded with no cast shadows, and only a voxel's first material is drawn (not the second-material paint),
nor the base's scenery meshes or its heightmap ground. A face between two frames that touch is still drawn (hidden by
the faces in front of it), so overlapping coplanar frames can flicker between their colours.

### scenery

The level's scenery details (the base map's meshes, glass and neon, grass tufts, lights, sounds, cameras) and copies
of another base's pieces. A copied piece draws in the game but has **no collision** (it is not part of the landscape:
worms and shots pass through it), so copies are decoration only; the Erg panel does not copy scenery for that reason.
Build walls that matter with the structure tools. Removing a base detail is a `remove` op in the patch, as the panel's.

| Tool | What it does |
|---|---|
| `list_scenery` | The open level's details that are not spawns or objects: id, name, resource, role, frame, whether the base has it, position, rotation, scale; counts per resource. Filters: `search`, `role`, `near` {x, z, radius} (nearest first), `box` {min, max}. |
| `remove_scenery` | Remove such details by `ids` or by the same filters (at least one); `dryRun` lists them. One undo step. The surround ring is `set_surround_mode` (`none` removes it). |
| `list_base_scenery` | A base level's scenery pieces (default the open project's base; `Multi.StormTheCastle`, `Multi.NiceToSiege`, `Multi.HardDaysKnight` and the other Camelot maps for castle walls, towers, flags and statues), grouped by resource, with an example of each (its id, name, position, rotation and scale there). |
| `copy_base_scenery` | `copies`: up to 64 pieces, each `id` (a detail of that base) or `resource`, at `pos` or `xz` (dropped onto the voxels), with `rot` (degrees) and `scale` (default the source's). Names keep their engine tags (`VISIBLE_`, `MORTAL_`). Refused from a base of another theme unless `allowOtherTheme` (the game may not load another theme's meshes: Test it). One undo step. |

**What is left of a wiped base.** `clear_base_terrain` empties the voxel frames, but not the base's heightmap ground
(the `.hmp` surround, `copy` by default) or its details. On Multi.DinerMight the heightmap is the green hill the diner
stands on: with the voxels wiped and the sea risen over it, it shows as a translucent green-grey mass under the water
(seen in the game on 2026-11-07; with `set_surround_mode {mode: "none"}` it is gone). The base also keeps 137 details:
cameras and look-at points (65), 27 `VISIBLE_NeonStrip` (`D05_02`, the thin pink lines left floating round the old diner
at y 6-10), 20 `VISIBLE_MORTAL_Grass` tufts (`BUILDING14`) at y 2-4, lights, sounds and the juke box. For an island in
an open sea: `set_surround_mode {mode: "none"}` (or `flat`, or `paint` it lower), then `remove_scenery {search: "Neon"}`
and `{search: "Grass"}`; `list_scenery {near}` finds what else is near a point.

### game

A look at a Test while it plays, through the running game's Oasis: these run Lua in the game's client console
(`lua.eval`), so they need the game running with Oasis writable and offline (the console refuses online matches
unless `[LuaConsole] MatchConsoleOnline=1`). A Test match with an idle human team ends by itself after some minutes:
keep waits short, and stop the game before a match you do not want recorded ends.

| Tool | What it does |
|---|---|
| `game_state` | The scene (menu, loading, match), the turn (index, team, worm, phase, ai), the water level (world and map units), turn and round time left, sudden death, worms alive per team, the sim tick. |
| `game_wait` | Wait until `turn` turns have started, or `seconds` pass (default 120), or the match ends; answers `game_state`. |
| `game_camera` | Draw the frame from `pos` toward `target` (map units unless `units: "world"`; `fov`) for `seconds` (at most 10; call again to hold it), or `clear`. The picture only (`wum.render.cameraOverride`): the game's camera, aiming and the match are untouched. An override belongs to a client mod, so it runs in the console of `mod` (default the first enabled mod with client code; none enabled: refused). |


<!-- end of erg-mcp tools -->

Edits stay in memory until `save`, as in the panel; closing or reconnecting with unsaved edits is refused unless the
tool is told to discard them. A failed tool answers with an error result (`isError`) that says why.

## Limits

The server's patch limits (see [erg.md](erg.md#limits)) apply as in the panel, and `describe_level` reports the usage:
at most 64 added blocks of 1-31 voxels a side (scale 1-4), 2000 runs of voxel changes per frame, 20 000 ops, a 4 MB
patch and 256 objects. Voxel edits that would pass the runs or size limit are refused before they are applied. The game
adds two of its own: a voxel frame side over 31 asserts (a block of 32 from an older project still opens, and is named
in `engineWarnings` and in the save's warnings), and more than 65 535 cells over all voxel frames crashes the game on
load, so every tool that adds blocks refuses past it, `save` sends the server's warning, and `export`, `build` and
`test` are refused by the server. The game's own maps are made of many small voxel frames (a few hundred, none longer
than 31 voxels a side), not one large terrain grid.

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
