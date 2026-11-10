// Caldera: an Erg MCP benchmark map, built only through the erg-mcp server's tools (stdio JSON-RPC, no hand-written
// patches). A volcanic island on Multi.DinerMight, in the Prehistoric theme's materials (volcano rock, hot rocks, liquid magma)
// in DAY light: a tall round cone (the rim about 64-72 map units) with a cliff band half way up, ribs and gullies down its flanks,
// a wide crater with a breach facing south, a basalt plug on the crater floor that carries the mine factory, a coast of wide
// beaches, headlands and coves, lava tongues of real magma texture down the cardinals from the crater to the shore.
// v2 (the user's directions of 2026-11-09): four teams, each spawning in its own castle near the shore (Strand, Ashgate,
// Harbour, Seawall, turned to the contour), and from each castle an obvious SWITCHBACK trail of pale path stone up its diagonal
// of the cone to the rim (five legs, four hairpins, a 5.2-wide bed at about 0.5 a unit); three higher castles (Cinder,
// Ember, Highwatch) beside the trails, each with a level branch to its trail. The sea is the game's own: it starts low (WATER_START)
// and rises only in the scheme's SUDDEN DEATH, at the scheme's round time and water speed, as on any other map (the owner,
// 2026-11-10: the map has no level script, sets no scheme and shows no text of its own). The mine factory fires
// volleys from four launchers on the crater's inner walls (round 5: erupting 14 up onto their vent pads; since 2026-11-09 thrown
// out over the island, see "mines" below). Sea stacks and islets with lights ring the coast.
// v2 round 2: a kerb about a unit high on the downhill side of every leg and round every hairpin's tip (terrain.mjs KERB); a
// solid plug under each corner where four skin blocks meet near a trail (round 1's SW worms and the mine probes fell through
// the skin there, docs/erg.md); the team spawns on the line between the castle's gates, and each spawn's own lead-in waypoints.
// v2 round 3: the trails' bed in pale cracked flagstone (dinoegg02, far paler than the beach sand) with a dark VolcanoRock kerb line on
// the downhill edge of every leg (terrain.mjs kerbLines), so each leg reads as a stripe from the oblique overview; the kerb a
// rounded ridge (softened) instead of a 1-unit step the skin's corners drew as a sawtooth; the skin's tiling shifted off the trails
// (cornersOff); ledges and scree on the flanks between the trails and a wider inner cliff band; plugs under the round-2 NE
// probe misses; a lip round every launcher pad (one mine ran off the north pad into the crater in round 2).
// v2 round 4: a level landing 9 across at every hairpin and a berm filling each hairpin's V up to the upper leg (terrain.mjs TRAIL.flat,
// TRAIL.land, BERM: round 3's NW worms stuck in a trough between legs 3 and 4), the NW legs at a gentler grade; the bed in grey slate
// flagstone (GreySlateFloor: the pale dinoegg02 read as snow up close) with the dark kerb line kept; the launcher pads of 2-unit voxels
// (the pads and plugs that let mines through were all 1.5-unit blocks, docs/erg-mcp.md) and the leak plugs put in after the castles,
// so the NE one comes up under the castle block's top; per-castle camera poses from the sea (out/castles.json) that clear the cone.
// v2 round 5: the trails' bed in path02 (a warm brown path texture: GreySlateFloor came out pale off-white with green blotches in the game,
// close to the white castle stone, game_material_shots); the last leg of every trail 6.6 wide (widened on its uphill side) to a level
// summit terrace about 9 across, with a low wall (1 over the bed) of dark basalt on its downhill edge and round the terrace (round 4's SW
// summit leg was a 5-wide bed between drops where a worm walked off); the banks beside the beds smoothed (the sheared teeth along the bed
// edges up close); the north launcher pad (by the tester's names the south one, at 9.6, -21.4) widened west with a higher rim and a
// catch slab under its dish (3 mines in 40 volleys left it to the west); the leak plugs in 2-unit voxels under the trail's own surface,
// with round 4's three trail probe misses.
// mines (2026-11-09/10, the owner: "the mines need to shoot forward, they should land all over the map even extending to the
// castles"): the launchers THROW their mines out over the whole island (set_mine_factory throw, below), and since mines round 3
// the factory keeps a target of mines lying about (activation MINES_TARGET, a fire every 3rd turn start while fewer lie) instead of
// a volley every 3 turns whatever the island holds: the engine's payload pool has 32 slots, and a turn-count factory filled it, so
// later volleys were trimmed to nothing and a worm's cluster shot deleted mines (docs/erg.md, The payload pool).
// Default game dir is testenv B (the tester's).
//
//   tools\node\node.exe tools\erg-mcp\bench\caldera\build.mjs [--out <dir>] [--plan] [--trails] [--no-export] [--keep-mod] [--no-scenery]
//
// It connects like erg-mcp does: MELANGE_GAME_DIR (default C:\Users\Jamin\Desktop\WUMFix\testenv\B) with the game
// closed, starting Melange.exe --serve (MELANGE_EXE names another Melange.exe; default this checkout's
// build\x86-release\Melange.exe). The project id is `caldera`; an old one is deleted first (delete_project), so
// every run starts from the base. Renders go to --out (default out\ next to this file); the source export is copied to
// pack\ next to this file. --plan stops after the heightfield plan (nothing is applied or saved); --inner after the cone's inner shell and
// its checks (the shell's top against the skin's along the block's edge); --trails after check_trail
// on the bare skin (out\trails-skin.json; nothing saved). The terrain itself
// (the height, the trails, the castle sites, the lava tongues) is terrain.mjs; the MCP client is mcpclient.mjs.
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { startServer, ROOT } from "./mcpclient.mjs";
import * as T from "./terrain.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const argv = process.argv.slice(2);
const flag = (f) => argv.includes(f);
const opt = (f, d) => { const i = argv.indexOf(f); return i >= 0 ? argv[i + 1] : d; };
const OUT = path.resolve(opt("--out", path.join(HERE, "out")));
const GAME = process.env.MELANGE_GAME_DIR || "C:/Users/Jamin/Desktop/WUMFix/testenv/B";
process.env.MELANGE_EXE ??= path.join(ROOT, "build/x86-release/Melange.exe");
const ID = process.env.CALDERA_ID || "caldera";
const MOD = "caldera";
const LAUNCH_R = +(process.env.LAUNCH_R || 2);    // map units: a launcher's mines land within this of it, on its 6 x 6 pad
const TEAMS = 4, WORMS_PER_TEAM = +(process.env.WORMS_PER_TEAM || 2);   // the test: four human teams of two (team knots for four each)
const WORM_W = 0.6;                               // a worm's body, map units (check_reachability's bodyWidth)

// Materials of the Prehistoric theme's file (set_theme PREHISTORIC, materialFile "theme"): grass01 0, grass02 2 (olive), rock01 4,
// rock02 6 (grey-brown: cliff faces), path01 8 (the trails), sand01 10, stone 12, flint 16, chippedstone 27 (the castles),
// VolcanoRock 49 (near black: the ash of the summit), LiquidHotMagma 50 (the lava), HotRocks 51 (dark red: its margins,
// the crater walls and the plug), Metal 53.
// dinoegg02 26 (pale cream, cracked like flagstones: the trails' bed in v2 round 3, far paler than path01's yellow, the beach sand's hue),
// GreySlateFloor 19 (a warm grey slate floor of laid flags: the trails' bed since v2 round 4, darker than dinoegg02 and still far paler than the basalt)
// path02 9 (warm brown worn path, #a26b2a: the trails' bed since v2 round 5)
const MAT = { grass: 0, olive: 2, rock: 4, grey: 6, path: 8, path2: 9, sand: 10, stone: 12, flint: 16, slate: 19, flag: 26, chipped: 27, ash: 49, magma: 50, hot: 51, metal: 53 };
const TRAIL_MAT = +(process.env.TRAIL_MAT ?? MAT.path2);
const VOXEL = +(process.env.VOXEL || 2.5);       // map units per voxel of the island's skin
const THICK = +(process.env.THICK || 3);         // voxels of skin (hollow under it: a fifth of the cells of a solid core)
const SCALE = T.CASTLE_SCALE;                    // voxels of the castle blocks (CASTLE_SCALE, read by terrain.mjs: the courts' height depends on it)
const WATER_START = +(process.env.WATER_START || 20);   // world units (1 map): the sea at the start is low; the beaches' sand is cut to it.
// Nothing else sets the water: sudden death raises it at the scheme's speed (2026-11-10: the scripted rising sea is gone)
const FL = -T.SEA_FLOOR;                          // the skin's floor is SEA_FLOOR: a rule's heights are above it
const hh = (v) => v + FL;

// Materials of the skin, first rule that holds wins: the castles' courtyards, the trails (a distance field to each polyline:
// pale path stone over the 5.2-wide bed (a band 2.5-voxel columns always meet, so the trail is one piece of path stone), a dark ash
// shoulder 1.8 past the bed's edge, so two legs never run
// together and the switchbacks read from the overview), the lava (a
// molten channel inside a margin of hot rock; both widen downslope, run from the crater to the beach and no further), the
// crater (a magma lake, hot patches on an ash floor, hot walls), the black ash of the summit, the cliff faces, then bands by
// height from grey rock down to grass and sand; every band edge is wandered by noise (feather perlin, jitter), so there are no
// straight steps and no dithered speckle.
const FLOOR = T.FLOOR_Y;
const [PFX, PFZ] = T.pointAt(T.PLUG_RHO, T.PLUG_TH).map((v) => Math.round(v * 10) / 10);     // the factory's plug (placed below)
const CRATER_EXCL = [{ x: 0, z: 0, radius: T.RC + 8 }];      // vegetation never grows inside the rim
// the lava lake: a radius that wanders with the angle (an organic round shore, not a disc), and thin rivulets that leave it
const lakeD = (R, pad = 0) => `fn a(cx, cz) = atan2(z - cz, x - cx); ${(R + pad).toFixed(1)} + 1.6 * sin(3 * a(${T.LAKE.c[0]}, ${T.LAKE.c[1]}) + 0.7) + 1.0 * sin(5 * a(${T.LAKE.c[0]}, ${T.LAKE.c[1]}) + 2.1) + 0.9 * noise2(x / 4, z / 4, 14) - hypot(x - ${T.LAKE.c[0]}, z - ${T.LAKE.c[1]})`;
const RIVULETS = [
  { points: [[-11, 5], [-15, 9], [-19, 8], [-24, 13]], width: 2.6, taper: 0.45 },
  { points: [[-6, 12], [-8, 17], [-7, 22]], width: 2.4, taper: 0.4 },
  { points: [[-1, 11], [2, 17], [5, 23], [6, 29]], width: 2.6, taper: 0.5 },
];
const MATERIALS = [
  { material: MAT.flint, masks: T.CASTLES.map((c) => T.courtMask(c)) },
  // the trails: the pale band is the bed, edged with a dark ash shoulder (also over the bank between two legs) that hides the voxel staircase
  ...T.TRAILS.map((rd) => ({ material: TRAIL_MAT, mask: T.roadMask(rd, T.TRAIL.bed, 5), feather: { width: 0.4, pattern: "perlin", cell: 3 }, edge: { depth: 1 } })),
  // the kerb line: dark VolcanoRock along the downhill edge of every leg (over the kerb's ridge), then a narrower dark shoulder on both sides
  { material: MAT.ash, paths: T.kerbLines().map((points) => ({ points, width: 3 })), feather: { width: 0.6, pattern: "perlin", cell: 3 }, edge: { depth: 1 } },
  ...T.TRAILS.map((rd) => ({ material: MAT.ash, mask: T.roadMask(rd, T.TRAIL.bed + 1.4, 5), feather: { width: 1, pattern: "perlin", cell: 3 }, exclude: [{ x: 0, z: 0, radius: T.RC - 2 }] })),
  { material: MAT.magma, minHeight: hh(2.4), mask: T.tonguesMask(T.TONGUES, 0.62), feather: { width: 1.6, pattern: "perlin", cell: 5 } },
  { material: MAT.hot, minHeight: hh(2.4), mask: T.tonguesMask(T.TONGUES, 1.0), feather: { width: 3, pattern: "perlin", cell: 5 } },
  // the crater floor: dry ash apron round the plug first (the mines land on it), the lava lake (organic shore) with thin rivulets and an ash ring round it
  { material: MAT.ash, maxHeight: hh(FLOOR + 3), mask: `${T.APRON_R} - hypot(x - ${PFX}, z - ${PFZ})`, feather: { width: 2, pattern: "perlin", cell: 4 } },
  { material: MAT.magma, maxHeight: hh(FLOOR + 1.5), mask: lakeD(T.LAKE.R), edge: { depth: 1 } },
  { material: MAT.magma, maxHeight: hh(FLOOR + 4), paths: RIVULETS, edge: { depth: 1 } },
  { material: MAT.ash, maxHeight: hh(FLOOR + 1.5), mask: lakeD(T.LAKE.R, 3.6), feather: { width: 1.5, pattern: "perlin", cell: 4 } },
  { material: MAT.hot, minHeight: hh(FLOOR - 1), maxHeight: hh(FLOOR + 2.5), mask: `${T.FLAT_R + 2} - r`, noise: { threshold: 0.1, scale: 5, seed: 21 } },   // hot rock: patches on the ash floor
  { material: MAT.hot, minHeight: hh(FLOOR + 2.5), mask: `${T.RC + 1.5} - r`, feather: { width: 3, pattern: "perlin", cell: 6 } },    // the crater's walls and rim
  { material: MAT.ash, minHeight: hh(FLOOR - 3), mask: `${T.RC + 14} - r`, feather: { width: 5, pattern: "perlin", cell: 8 }, jitter: { amount: 2.5, scale: 9, seed: 3 } },   // the floor and the summit's ash
  // the cone is basalt: cliffs over 56 degrees are HotRocks (dark red-brown: hides the stretched texture of a steep face), the flanks above the
  // foothills are VolcanoRock (the darkest ground material of the theme) with patches of HotRocks; the pale stone is left to the castles and trails
  // (v2 round 3) the steep faces in three rocks, not one slab: grey rock in streaks, dark basalt in patches, hot rock the rest
  { material: MAT.grey, minHeight: hh(14), minSlope: 48, mask: `r - ${T.RC + 3}`, noise: { threshold: 0.4, scale: 6, seed: 33 }, feather: { width: 0.5, pattern: "perlin", cell: 5 } },
  { material: MAT.ash, minSlope: 56, mask: `r - ${T.RC + 3}`, noise: { threshold: 0.25, scale: 10, seed: 34 }, feather: { width: 0.5, pattern: "perlin", cell: 5 } },
  { material: MAT.hot, minSlope: 56, feather: { width: 3, pattern: "perlin", cell: 6 } },
  // scree (v2 round 3): grey rock in patches on the steeper flank under the cliffs and on the ledges' risers, between the trails
  { material: MAT.grey, minHeight: hh(12), minSlope: 30, maxSlope: 56, mask: `r - ${T.RC + 6}`, noise: { threshold: 0.25, scale: 7, seed: 31 }, feather: { width: 0.5, pattern: "perlin", cell: 5 } },
  { material: MAT.hot, minHeight: hh(16), noise: { threshold: 0.55, scale: 11, seed: 12 } },
  { material: MAT.ash, minHeight: hh(11), feather: { width: 4, pattern: "perlin", cell: 9 }, jitter: { amount: 3, scale: 11, seed: 4 } },
  // vegetation: only on gentle lowland ground (a green skirt of olive and grass, under 22 degrees and under 9 map units), in patches; steeper ground at the
  // foot is the same dark rock as the cone, so no bright square sits on a black slope
  { material: MAT.olive, minHeight: hh(6.5), maxHeight: hh(9), exclude: CRATER_EXCL, maxSlope: 22, feather: { width: 3, pattern: "perlin", cell: 7 }, noise: { threshold: 0.1, scale: 9, seed: 5 } },
  { material: MAT.grass, minHeight: hh(4), maxHeight: hh(7), exclude: CRATER_EXCL, maxSlope: 22, feather: { width: 2.5, pattern: "perlin", cell: 6 }, noise: { threshold: -0.1, scale: 12, seed: 8 } },
  { material: MAT.hot, minHeight: hh(5), feather: { width: 3, pattern: "perlin", cell: 7 }, jitter: { amount: 1.5, scale: 7, seed: 8 } },
];

// ---- the MCP client ----
const say = (...a) => console.log(...a);
const brief = (r, n = 300) => r.text.slice(0, n).replace(/\s+/g, " ");
const round1 = (v) => Math.round(v * 10) / 10;
const round2 = (v) => Math.round(v * 100) / 100;

fs.mkdirSync(OUT, { recursive: true });
const { call, stop, log, stats } = await startServer();
try {
  say("connect", brief(await call("connect", { gameDir: GAME }), 160));
  if ((await call("list_bases", { search: "dinermight" })).json.projects.some((p) => p.id === ID))
    say("delete the old project", brief(await call("delete_project", { id: ID }), 200));
  const np = await call("new_project", { base: "Multi.DinerMight", slug: ID, title: "Caldera" });
  if (np.json?.created?.id !== ID) throw new Error(`new_project gave id ${np.json?.created?.id}, not ${ID}`);
  await call("open_project", { id: ID });

  // Setting: the Prehistoric theme (its volcano rock, hot rocks and liquid magma materials) in DAY light; the base's terrain
  // wiped and its frames removed (emptied and trimmed to one cell: the 11 652 cells a wiped base still spends come back for the
  // castles, turned to the contour, which cost more); the diner's buildings, neon, signs, grass tufts and jukebox removed (they
  // would show through the sea or poke through the cone); no surround (its ground would stand over the shore castles' courts);
  // the sea starts below the beaches.
  await call("set_theme", { theme: "PREHISTORIC", materialFile: "theme" });
  await call("set_time_of_day", { timeOfDay: process.env.TOD || "DAY" });
  await call("set_water", { level: WATER_START });
  await call("set_surround_mode", { mode: "none" });
  const sc0 = await call("list_scenery", { limit: 1 }, { soft: true });
  say("scenery of the base", JSON.stringify(sc0.json?.counts ?? sc0.text.slice(0, 300)).slice(0, 600));
  for (const search of ["Neon", "Grass", "BUILDING", "D05", "MusicNotes", "Applause", "JukeBox"]) say("remove", search, brief(await call("remove_scenery", { search }, { soft: true }), 120));
  say("clear", brief(await call("clear_base_terrain", { all: true, wipe: true }), 200));
  say("remove base frames", brief(await call("remove_base_frames", { all: true }, { soft: true }), 300));
  const d0 = await call("describe_level", { limit: 1 });
  say("cells before", JSON.stringify(d0.json.limits.voxelCells));

  // ---- the island: one drape skin over the whole rectangle (the height is terrain.mjs's island()) ----
  const grid = T.smoothBanks(T.relax(T.soften(T.gridOf(-T.BOX, T.BOX, 1))));
  // (weapons round 2, 2026-11-10) the offshore sea floor is left out: a column whose island() height is under OFFSHORE_CUT (map y; the
  // sea stands at 1 at the start and only rises, and its floor is -3) is emptied (height 0), so the skin ends in the shallows round the
  // island and the islets instead of running out over the whole -3 floor to CLIP. Those columns were about 8 000 cells of the skin's
  // 24 000 and showed only as the sea's pale disc; the cells pay for the cone's inner shell below. The grid is cut after relax and
  // smoothBanks, so every column kept is exactly as before. OFFSHORE_CUT=-99 keeps the whole floor.
  const OFFSHORE_CUT = +(process.env.OFFSHORE_CUT ?? -2.4);
  let offshoreCut = 0;
  for (const row of grid) for (let i = 0; i < row.length; i++) if (row[i] + T.SEA_FLOOR < OFFSHORE_CUT) { row[i] = 0; offshoreCut++; }
  say("offshore floor under", OFFSHORE_CUT, "cut", offshoreCut, "grid cells of", grid.length * grid[0].length);
  // trails and branches as check_trail's polylines ([x, y, z]; y on a switchback, so the leg over or under is not taken for it)
  const trailLines = T.TRAILS.flatMap((t) => [t.pts, ...t.branches.map((b) => b.pts)].map((pl) => pl.map((p) => [round1(p[0]), round1(p[2]), round1(p[1])])));
  const trailNames = T.TRAILS.flatMap((t) => [t.name, ...t.branches.map((b) => `${t.name}-${b.castle.name}`)]);
  // round 3: the skin's tiling shifted so the corners where four skin blocks meet keep off the trails (heightfield_drape
  // cornersOff, 2.5 map units: 4 needs a 13th skin block and the skirt no longer fits); a corner still near one gets its plug
  // below. CORNERS_OFF=0 keeps the tiling at the min corner.
  const cornersOff = +(process.env.CORNERS_OFF ?? 2.5);
  // (weapons round 2) the tiling is pinned where cornersOff put it with the whole sea floor (shift [0, 30]: a tile corner at x -40, z -42.5),
  // so cutting the offshore columns moves no skin block and no meeting corner (the plugs and the mine probes stay valid).
  // TILE_ORIGIN=off lets cornersOff choose again.
  const tileOrigin = (process.env.TILE_ORIGIN ?? "-40,-42.5") === "off" ? null : (process.env.TILE_ORIGIN ?? "-40,-42.5").split(",").map(Number);
  const field = { min: [-T.BOX, -T.BOX], max: [T.BOX, T.BOX], centre: [0, 0], floor: T.SEA_FLOOR, voxelSize: VOXEL, grid, clip: [{ x: 0, z: 0, radius: T.CLIP }],
    thickness: THICK, core: false, material: MAT.sand, materials: MATERIALS, maxBlocks: 20,
    // the shore's columns (the skin dips to the sea's zero there) are solid from zero up, so the sea never shows the skin's hollow underside
    skirtTo: +(process.env.SKIRT_TO ?? 0), skirtTop: +(process.env.SKIRT_TOP ?? 0),
    ...(tileOrigin ? { tileOrigin } : cornersOff > 0 ? { cornersOff: { paths: trailLines, distance: cornersOff } } : {}) };
  // ---- (weapons round 2, 2026-11-10) the cone's inner shell. The skin is 3 voxels (7.5 map units) over nothing: in the weapons test an
  // Air Strike's first bombs holed the slope, the later ones and the Fatkins payload fell 40-50 units into the hollow cone, the game's
  // follow camera went in after them (a dark cavern with the sea at its floor) and 11 columns were left with no ground at all. A second,
  // coarser drape (INNER_VOXEL-unit voxels, INNER_THICK of them) follows the same surface INNER_DROP under it: one block of 31 x 31
  // columns over the cone (x, z in [-60, 64]: the summit, the crater and the flanks to past the cliff bands; a solid core at any voxel size
  // would be about four times the cells of the skin and there are 1 400 left). Its top is the skin's underside plus half a unit (the
  // two overlap, so no air lies between them; the coarse surface sags under the fine one at a crest by a unit or two and rises into
  // the skin at a gully, never above the skin's top: checked below). Breaching the slope now takes 7.5 + 16 units of rock, and a
  // payload that gets through the skin lands on the shell a unit or two down instead of in the void. keepFloor: the heights count from
  // the floor as given (the skin's own floor snapped a unit lower: its surface is grid - 1). INNER_THICK=0 leaves it out. ----
  const INNER = { voxel: +(process.env.INNER_VOXEL ?? 4), thick: +(process.env.INNER_THICK ?? 4), lo: +(process.env.INNER_LO ?? -60), hi: +(process.env.INNER_HI ?? 64), drop: +(process.env.INNER_DROP ?? THICK * VOXEL - 0.5) };
  // At the block's edge a corner takes the mean of the two columns inside it (sampled 2 units in), so on a slope rising inward the
  // edge stands up to twice the slope too high (at x 20, z -60 the lower cliff band rises 2.9 a unit and the shell's top came out
  // 0.6 over the skin). Within INNER_EDGE of the edge the grid is eroded first (the least height within INNER_ERODE each way), so the
  // shell's top there is under the skin by the band's relief, never over it.
  const EDGE = { width: +(process.env.INNER_EDGE ?? 6), erode: +(process.env.INNER_ERODE ?? 4) };
  const innerField = () => {
    const a = INNER.lo + T.BOX, b = INNER.hi + T.BOX;    // grid indices (1-unit grid from -BOX)
    const n = b - a + 1;
    const raw = grid.slice(a, b + 1).map((row) => row.slice(a, b + 1));
    const sub = raw.map((row, j) => row.map((v, i) => {
      if (!(v > 0)) return 0;
      const d = Math.min(i, j, n - 1 - i, n - 1 - j);
      let m = v;
      if (d < EDGE.width) for (let dj = -EDGE.erode; dj <= EDGE.erode; dj++) for (let di = -EDGE.erode; di <= EDGE.erode; di++) { const r = grid[a + j + dj]?.[a + i + di]; if (r > 0 && r < m) m = r; }
      return Math.max(0, round2(m - 1 - INNER.drop));
    }));
    return { min: [INNER.lo, INNER.lo], max: [INNER.hi, INNER.hi], centre: [0, 0], floor: T.SEA_FLOOR, keepFloor: true, voxelSize: INNER.voxel, grid: sub,
      thickness: INNER.thick, core: false, material: MAT.ash, maxBlocks: 1, tileOrigin: [INNER.lo, INNER.lo] };
  };
  const plan = await call("heightfield_drape", { ...field, dryRun: true });
  say("skin tiling", JSON.stringify(plan.json.skin?.tiling ?? null), "meeting corners", JSON.stringify(plan.json.skin?.meetingCorners ?? []));
  say("plan fits", plan.json.fits, "skin", JSON.stringify({ blocks: plan.json.skin?.blocks, cells: plan.json.skin?.cells }), "budget", JSON.stringify(plan.json.budget?.cells), plan.json.why ?? "", JSON.stringify(plan.json.notes ?? []));
  fs.writeFileSync(path.join(OUT, "plan-drape.json"), plan.text);
  if (flag("--plan")) {
    if (INNER.thick > 0) {
      const ip = await call("heightfield_drape", { ...innerField(), dryRun: true }, { soft: true });
      say("inner shell plan", ip.err ? ip.text.slice(0, 400) : JSON.stringify({ fits: ip.json.fits, why: ip.json.why, skin: { blocks: ip.json.skin?.blocks, cells: ip.json.skin?.cells, list: ip.json.skin?.list }, floor: ip.json.region?.floor, peak: ip.json.region?.peak }));
    }
    await call("close", { discard: true }, { soft: true }); throw new Error("--plan: stopped");
  }
  const hf = await call("heightfield_drape", field);
  say("island", JSON.stringify(hf.json.budget?.cells ?? hf.json.limits?.voxelCells), "notes", JSON.stringify(hf.json.notes ?? []));
  if (INNER.thick > 0) {
    // the ground before and after the inner shell, read as the game reads it: on the trails (every point of their lines) and on a
    // 5-unit lattice over the cone; the inner shell must change none of it (a point where it did would be a bump a worm meets)
    const probes = [];
    for (const pl of trailLines) for (const p of pl) probes.push([p[0], p[2]]);
    for (let z = INNER.lo; z <= INNER.hi; z += 5) for (let x = INNER.lo; x <= INNER.hi; x += 5) probes.push([x, z]);
    const groundRead = async () => { const out = []; for (const [x, z] of probes) { const g = await call("query_ground", { x, z, units: "map" }, { soft: true }); out.push(g.json?.standOn?.map ?? null); } return out; };
    const before = await groundRead();
    const inner = await call("heightfield_drape", innerField());
    const innerName = inner.json?.added?.[0]?.name ?? null;
    say("inner shell frame", innerName);
    say("inner shell", JSON.stringify({ budget: inner.json.budget, skin: { blocks: inner.json.skin?.blocks, cells: inner.json.skin?.cells, list: inner.json.skin?.list }, floor: inner.json.region?.floor, peak: inner.json.region?.peak, notes: inner.json.notes ?? [] }));
    const after = await groundRead();
    const moved = probes.map((p, i) => ({ at: p, before: before[i], after: after[i] })).filter((q) => q.before !== q.after && !(q.before == null && q.after == null) && (q.before == null || q.after == null || Math.abs(q.before - q.after) > 0.01));
    // under the skin: the inner shell's span at a few points of the cone (the holes of the weapons test and the crater), from the
    // column's spans (the skin's, the shell's): gap = the skin's underside less the shell's top (negative: they overlap)
    const under = [];
    for (const [x, z] of [[-40.8, -15], [-47, -9], [13.9, -29], [-46, -34], [0, 0], [30, 30], [-20, 45], [20, -60], [30, -60], [0, 62]]) {
      const q = (await call("query_ground", { x, z, units: "map", maxSpans: 8 }, { soft: true })).json;
      const sp = (q?.spans?.listed ?? []).map((s) => ({ frame: s.frameName, top: s.top?.map, bottom: s.bottom?.map }));
      const skin = sp.find((s) => /^ergdrape_\d+$/.test(s.frame) && s.frame !== innerName), shell = sp.find((s) => s.frame === innerName);
      under.push({ at: [x, z], standOn: q?.standOn?.map ?? null, skin: skin ? [skin.bottom, skin.top] : null, shell: shell ? [shell.bottom, shell.top] : null, gap: skin && shell ? round2(skin.bottom - shell.top) : null, spans: sp.length });
    }
    say("inner shell under the skin (gap = the skin's underside less the shell's top; negative: overlap):", JSON.stringify(under));
    if (moved.length) {
      say("inner shell MOVED the ground at", moved.length, "of", probes.length, "probes:", JSON.stringify(moved.slice(0, 20)));
      for (const m of moved.slice(0, 3)) say("moved probe", JSON.stringify(m.at), brief(await call("query_ground", { x: m.at[0], z: m.at[1], units: "map", maxSpans: 8 }, { soft: true }), 1500));
      if (!flag("--inner")) throw new Error("the inner shell rises above the skin: lower INNER_DROP or raise INNER_VOXEL");
    } else say("inner shell: the ground is the same at all", probes.length, "probes");
    if (flag("--inner")) {
      // the shell's top along two lines across the block's edge, against the skin's
      for (const [name, pts] of [["x=20, z -62..-44", Array.from({ length: 19 }, (_, i) => [20, -62 + i])], ["z=-60, x 10..34", Array.from({ length: 13 }, (_, i) => [10 + 2 * i, -60])], ["z=-59, x 10..34", Array.from({ length: 13 }, (_, i) => [10 + 2 * i, -59])]]) {
        const row = [];
        for (const [x, z] of pts) {
          const q = (await call("query_ground", { x, z, units: "map", maxSpans: 8 }, { soft: true })).json;
          const sp = q?.spans?.listed ?? [], sh = sp.find((s) => s.frameName === innerName), sk = sp.find((s) => s.frameName !== innerName);
          row.push(`${x},${z}: skin ${sk?.top?.map ?? "-"} shell ${sh?.top?.map ?? "-"}`);
        }
        say("profile", name, row.join(" | "));
      }
      await call("close", { discard: true }, { soft: true }); throw new Error("--plan: --inner stopped");
    }
  }
  // --trails: check_trail on the bare skin and stop (nothing saved): a quick loop on the trails' shape
  if (flag("--trails")) {
    const tq = await call("check_trail", { trails: trailLines, names: trailNames, width: T.TRAIL.bed * 2, margin: 1 }, { soft: true });
    fs.writeFileSync(path.join(OUT, "trails-skin.json"), tq.text);
    for (const t of tq.json?.trails ?? []) say("trail", t.name, t.summary, JSON.stringify((t.walk ?? []).map((w) => [w.offset, w.blockedCount ?? 0])));
    await call("close", { discard: true }, { soft: true });
    throw new Error("--plan: --trails stopped");
  }
  const groundOf = async (x, z) => {
    const g = await call("query_ground", { x, z, units: "map" });
    const top = g.json?.standOn?.map;
    if (typeof top !== "number") throw new Error(`no ground at ${x}, ${z}: ${g.text.slice(0, 300)}`);
    return top;
  };
  const lvl = async () => (await call("describe_level", { limit: 1 })).json.limits;

  // ---- the skin's corners: where four draped skin blocks meet, the game lets a worm or a mine through the skin within about 0.4
  // map units of the corner (round 1: both SW worms fell 31 units there, beside Ember; the mine probes at the other three).
  // Each corner within 12 of a trail gets a plug of undraped 1-unit voxels, 3 x 4 x 3, its top flush with the skin at the corner
  // or under it (a quarter unit, rounded down): whatever falls through the skin there lands on it a unit lower at most ----
  const cornerScan = await call("check_trail", { trails: trailLines, names: trailNames, width: T.TRAIL.bed * 2, margin: 1, holeRadius: 11.4 }, { soft: true });
  const corners = new Map();
  for (const tr of cornerScan.json?.trails ?? []) for (const c of tr.skinCorners ?? []) corners.set(c.at.join(","), { at: c.at, off: Math.min(c.off, corners.get(c.at.join(","))?.off ?? 1e9), trail: tr.name });
  const plugs = [];
  // (v2 round 3: the tiling is shifted off the trails, cornersOff, so only a corner within 1.5 of a bed's edge still gets one, 2 x 3 x 2 and
  // its top under the lowest skin round the corner: round 2's 3 x 4 x 3 plugs on a steep flank stood out of the slope as dark cubes)
  for (const c of corners.values()) {
    if (c.off > T.TRAIL.bed + 1.5) continue;
    const [cx, cz] = c.at, gs = [];
    for (const [dx, dz] of [[-1, -1], [1, -1], [-1, 1], [1, 1], [0, 0]]) gs.push(await groundOf(cx + dx, cz + dz));
    const top = Math.min(...gs), y0 = Math.floor(top - 0.25) - 3;
    const r = await call("add_block", { size: [2, 3, 2], scale: 1, at: [cx, y0, cz], units: "map", fill: "solid", material: MAT.ash }, { soft: true });
    plugs.push({ at: [cx, cz], trail: c.trail, off: round2(c.off), skin: round2(top), plugTop: y0 + 3, block: r.json?.name ?? r.text.slice(0, 160) });
  }
  say("skin corner plugs", JSON.stringify(plugs));

  // ---- the castles, each into new blocks of SCALE-unit voxels, turned to the contour (local x along it, local +z to the sea),
  // a gate in each end wall where its trail comes in and goes out ----
  const spots = {};                           // castle -> spawn spots [{kind, at, standOn, ok}]
  const keepTop = {};                         // a round keep's top, for its flag
  const common = { place: "new", scale: SCALE, packing: "tiles" };
  // a gate is the lane's width (4.5 or more clear, terrain.mjs laneOf) and the wall's height less 1 (5 under a 6 wall): a worm walks through
  const gateOf = (g, wall, pl) => ({ side: g.side, offset: round1(g.offset), width: pl.keep ? T.laneOf(pl).w : 4.5, height: Math.min(5, wall - 1) });
  const carve = async (blocks, y, [x0, z0, x1, z1], up = 6) => {
    for (const b of blocks ?? []) await call("fill_box_raw", { frame: b.id, state: "empty", min: [round1(x0), y - 1, round1(z0)], max: [round1(x1), y + up, round1(z1)], units: "map" }, { soft: true });
  };
  for (const c of T.CASTLES) {
    const [cx, cz] = c.at.map(round1);
    const y = c.floor, p = c.plan, yaw = c.f.yaw;
    const g = await groundOf(cx, cz);
    let usage = null;
    if (p.kind === "castle") {
      const r = await call("build_castle", { at: [cx, y - 1, cz], rotation: yaw, size: [c.along, c.across], wallHeight: p.wallHeight, wallThickness: 2, towerShape: p.towerShape, towerRadius: p.towerRadius,
        towerHeight: p.towerHeight, keep: !!p.keep, keepSize: p.keep ?? undefined, keepHeight: p.keep ? p.keepHeight : undefined, gates: [gateOf(c.gin, p.wallHeight, p), gateOf(c.gout, p.wallHeight, p)], gateBelow: 0, crenellations: true, foundation: 2, material: p.mat, ...common });
      spots[c.name] = r.json.spawnSpots?.spots ?? [];
      usage = r.json.usage;
    } else if (p.kind === "lfort") {
      // an L: the main court on the trail and a wing on the sea side (local +z), joined by a door in the shared wall; a sea gate in the wing's far wall
      const [wa, wc] = p.wing;
      const r = await call("build_castle", { at: [cx, y - 1, cz], rotation: yaw, size: [c.along, c.across], wallHeight: p.wallHeight, wallThickness: 2, towerShape: p.towerShape, towerRadius: p.towerRadius,
        towerHeight: p.towerHeight, keep: !!p.keep, keepSize: p.keep ?? undefined, keepHeight: p.keep ? p.keepHeight : undefined, gates: [gateOf(c.gin, p.wallHeight, p), gateOf(c.gout, p.wallHeight, p), { side: "s", width: 4.5, height: 4.5 }], gateBelow: 0, crenellations: true, foundation: 2, material: p.mat, ...common });
      const [wx, wz] = T.toWorld(c, 0, c.across / 2 + wc / 2 - 2);
      const w = await call("build_castle", { at: [round1(wx), y - 1, round1(wz)], rotation: yaw, size: [wa, wc], wallHeight: p.wallHeight, wallThickness: 2, towerShape: p.towerShape, towerRadius: p.towerRadius - 0.4,
        towerHeight: p.towerHeight - 2, keep: false, gates: [{ side: "n", width: 4.5, height: 4.5 }, { side: "s", width: 6, height: 4.5, below: "auto" }], crenellations: true, foundation: 2, material: p.mat, ...common });
      spots[c.name] = [...(r.json.spawnSpots?.spots ?? []), ...(w.json.spawnSpots?.spots ?? [])];
      usage = w.json.usage;
    } else {
      // a round keep on a crag: a ring wall of 16 sides round a round keep set uphill of the centre, the trail passing between them
      const R = p.ringRadius, U = [-c.f.ez[0], -c.f.ez[1]];
      const pts = Array.from({ length: 16 }, (_, i) => [round1(cx + R * Math.cos(i * Math.PI / 8)), y - 1, round1(cz + R * Math.sin(i * Math.PI / 8))]);
      const wl = await call("build_wall", { points: pts, closed: true, height: p.wallHeight, thickness: 2.5, crenellations: true, foundation: 2, material: p.mat, place: "new", blockScale: SCALE, packing: "tiles" });
      const kp = [round1(cx + U[0] * 6), round1(cz + U[1] * 6)];
      const kt = await call("build_tower", { at: [kp[0], y - 1, kp[1]], radius: p.keepRadius, height: p.keepHeight, hollow: true, wallThickness: 2, door: "s", doorWidth: 3, doorHeight: 4, foundation: 2, material: p.mat, place: "new", blockScale: SCALE });
      // the gates: where the trail's line meets the ring
      for (const s of [-1, 1]) await carve(wl.json.newBlocks, y, [cx + c.T[0] * R * s - 2.5, cz + c.T[1] * R * s - 2.5, cx + c.T[0] * R * s + 2.5, cz + c.T[1] * R * s + 2.5], p.wallHeight + 2);
      const gt = [round1(cx - U[0] * 5), round1(cz - U[1] * 5)], wt = [round1(cx - U[0] * (R - 0.5)), round1(cz - U[1] * (R - 0.5))];
      spots[c.name] = [];
      for (const [kind, [sx, sz]] of [["courtyard", gt], ["rampart", wt]]) {
        const q = await call("query_ground", { x: sx, z: sz, units: "map" }, { soft: true });
        const top = q.json?.standOn?.map;
        spots[c.name].push({ kind, at: [sx, top, sz], standOn: [sx, top, sz], ok: typeof top === "number" });
      }
      usage = kt.json?.usage ?? wl.json?.usage;
      keepTop[c.name] = [kp[0], y - 1 + p.keepHeight, kp[1]];
    }
    say("castle", c.name, p.kind, "team", c.team, "floor", y, "ground", round1(g), "at", [cx, cz].join(","), "yaw", yaw, "gates", c.arrive, c.depart, "cells", usage?.voxelCells, "blocks", usage?.newBlocks);
  }
  // (v2 round 3) plugs under the round-2 trail probes that missed (game_mine_drop on the NE trail: a mine fell to 4.74 at (44.7, -70.44)
  // near the start, and was gone at (58.97, -60.73) on the right shoulder about 30 along): 4 x 3 x 4 of 1-unit voxels, the top a
  // quarter unit under the ground there, so whatever goes through it lands on them. (v2 round 4) after the castles: (44.7, -70.44) is
  // the top of a castle block at 10.5 over the skin at 4.5 (round 3's plug went under the skin and caught nothing), so its plug now
  // comes up under that top; and the round-3 misses (54.44, -58.17) and (-44.1, -34.3), skin cells sheared across themselves
  // (v2 round 5) in 2-unit voxels (3 x 2 x 3: 6 x 4 x 6 map; the 1.5-unit blocks were the ones that let mines through, the 2-unit pads held),
  // and round 4's trail probe misses [x, z, y]: y is the trail's height there, so the plug goes under the surface at or below it (a
  // switchback's legs lie over each other), not under the highest ground at the point
  const LEAKS = (process.env.LEAKS ?? "44.7,-70.44;58.97,-60.73;54.44,-58.17,18.39;-44.1,-34.3;36.95,34.75,57.73;-45.21,-34.45,37.39").split(";").filter(Boolean).map((q) => q.split(",").map(Number));
  const leakPlugs = [];
  const surfAt = async (x, z, yHint) => {
    if (yHint === undefined) return groundOf(x, z);
    const q = await call("query_ground", { x: round2(x), z: round2(z), units: "map", mode: "atOrBelow", y: yHint + 1 }, { soft: true });
    const y = q.json?.standOn?.map;
    return typeof y === "number" ? y : groundOf(x, z);
  };
  for (const [lx, lz, ly] of LEAKS) {
    // (v2 round 4) under the lowest ground over its footprint (round 3's plugs, set by the middle, poked out of a sloping skin),
    // or on a castle (its block's top over the skin) under the top at the point
    const gs = [];
    for (const [dx, dz] of [[-3, -3], [3, -3], [-3, 3], [3, 3]]) gs.push(await surfAt(lx + dx, lz + dz, ly));
    const onCastle = T.CASTLES.some((c) => T.footDist(c, lx, lz, 0) < 1);
    const top = await surfAt(lx, lz, ly), y0 = round1((onCastle ? top : Math.min(top, ...gs)) - 0.25 - 4);
    const r = await call("add_block", { size: [3, 2, 3], scale: 2, at: [lx, y0, lz], units: "map", fill: "solid", material: MAT.ash }, { soft: true });
    leakPlugs.push({ at: [lx, lz], trailY: ly ?? null, skin: round2(top), plugTop: round1(y0 + 4), block: r.json?.name ?? r.text.slice(0, 160) });
  }
  say("leak plugs", JSON.stringify(leakPlugs));
  // (mines round 4) a solid core under the SW trail's fourth leg: in round 3's match a thrown mine's blast and the blasts after it
  // holed the 7.5-thick skin there (bed about 31.6 at (-43, 38), hollow underneath), a worm fell 26 units into the cone and
  // drowned and a second died at the hole. The cells left (about 1 500 of 65 535, 5 blocks of 64) do not stretch to a fill under
  // every trail, so this leg, the one proven to fail, gets two blocks of 2-unit voxels, 8 along the leg, 4 across the bed's middle
  // and 16 deep, their flat top a quarter unit under the lowest bed height over each footprint (the skin and the block overlap, so
  // the bed stays solid 16 and more below it); the other trails are kept clear of landing mines by the throw's avoid lines
  // instead. CORE_FILLS: "x,z,y,halfAlong,halfAcross" per block (map units; y the bed's height there, for the leg's surface).
  const CORE_FILLS = (process.env.CORE_FILLS ?? "-52.9,36.6,29,4,2;-46.6,38.2,31.5,4,2").split(";").filter(Boolean).map((q) => q.split(",").map(Number));
  const coreFills = [];
  for (const [fx, fz, fy, ha, hc] of CORE_FILLS) {
    const gs = [];
    for (const [dx, dz] of [[-ha, -hc], [ha, -hc], [-ha, hc], [ha, hc], [0, 0], [-ha, 0], [ha, 0]]) gs.push(await surfAt(fx + dx, fz + dz, fy));
    const top = Math.min(...gs), depth = 16, y0 = round1(top - 0.25 - depth);
    const r = await call("add_block", { size: [ha, depth / 2, hc], scale: 2, at: [fx - ha, y0, fz - hc], units: "map", fill: "solid", material: MAT.ash }, { soft: true });
    coreFills.push({ at: [fx, fz], bed: round2(top), fillTop: round1(y0 + depth), fillBottom: y0, block: r.json?.name ?? r.text.slice(0, 160) });
  }
  say("core fills under the SW trail", JSON.stringify(coreFills));

  // ---- (v2 round 5) the summit walls: a low wall of dark basalt, one 2-unit voxel thick and about 1 over the bed, along the downhill edge
  // of every trail's last leg from past its landing (its inner face on the bed's edge) and on round the summit terrace from the downhill
  // side over the far end to the uphill side, so a worm on the top leg or the terrace cannot walk off it (round 4: a worm walked off the
  // SW summit leg, 63 to 43.5). Its foot runs SUMMIT_FOUND down the cut bank's drop, so no wall hangs over a slot a worm falls into (round 5's
  // first walls, 1-unit voxels with a 3-unit foot, left two such slots on the NW that check_trail named traps, and cost 2 400 cells:
  // in 2-unit voxels the four cost about 200, and the wall's top stands 0.6-1.9 over the bed, out/summit-walls.json). SUMMIT_WALLS=0
  // leaves them out ----
  const WSCALE = +(process.env.SUMMIT_SCALE ?? 2), WALL_OFF = +(process.env.WALL_OFF ?? WSCALE / 2);
  const summitWalls = [];
  if (+(process.env.SUMMIT_WALLS ?? 1)) {
    const plans = T.TRAILS.map((t) => {
      const iL = t.pinAt[t.pinAt.length - 1], n = t.pts.length, end = t.pts[n - 1], a = t.pts[iL];
      const L = Math.hypot(end[0] - a[0], end[1] - a[1]), d = [(end[0] - a[0]) / L, (end[1] - a[1]) / L];
      let nd = [-d[1], d[0]];
      const m = t.pts[Math.round((iL + n - 1) / 2)];
      if (T.islandBase(m[0] + nd[0] * 5, m[1] + nd[1] * 5) > T.islandBase(m[0] - nd[0] * 5, m[1] - nd[1] * 5)) nd = [-nd[0], -nd[1]];
      const base = (y) => round1(y - 1 - 1.5), pts = [];
      // the wall starts past the last point whose wall spot stands on or beside a lower leg (the hairpin's V: round 5's first build put
      // the SE wall's foot on the leg under it, a 4-high step on that leg's centreline)
      const spot = (p) => { const o = T.TRAIL.bed + (p[3] ?? 0) + WALL_OFF; return [p[0] + nd[0] * o, p[1] + nd[1] * o]; };
      const lower = [...t.pts.slice(0, iL - 1), ...t.branches.flatMap((b) => b.pts)];
      let i0 = iL + T.TRAIL.flat + 3;
      for (let i = i0; i < n - 4; i++) { const [qx, qz] = spot(t.pts[i]); if (lower.some((q) => Math.hypot(q[0] - qx, q[1] - qz) < T.TRAIL.bed + 2.5)) i0 = i + 1; }
      for (let i = i0; i < n - 4; i += 3) { const p = t.pts[i], [qx, qz] = spot(p); pts.push([round1(qx), base(p[2]), round1(qz)]); }
      const R = T.TRAIL.summit + WALL_OFF;
      for (let k = 0; k <= 6; k++) { const g = Math.PI * k / 6, v = [nd[0] * Math.cos(g) + d[0] * Math.sin(g), nd[1] * Math.cos(g) + d[1] * Math.sin(g)]; pts.push([round1(end[0] + v[0] * R), base(end[2]), round1(end[1] + v[1] * R)]); }
      return { trail: t.name, from: i0, of: n - 1 - iL, points: pts };
    });
    for (const pl of plans) {
      const w = await call("build_wall", { points: pl.points, height: 2.5, thickness: WSCALE, crenellations: false, foundation: +(process.env.SUMMIT_FOUND ?? 8), material: MAT.ash,
        place: "new", blockScale: WSCALE, packing: "fit", maxBlocks: +(process.env.SUMMIT_WALL_BLOCKS ?? 3), units: "map" }, { soft: true });
      summitWalls.push({ trail: pl.trail, scale: WSCALE, points: pl.points.length, from: pl.from, frames: (w.json?.newBlocks ?? []).map((q) => q.id), cells: w.json?.cells?.newBlocks ?? null, err: w.err ? w.text.slice(0, 200) : undefined });
    }
    fs.writeFileSync(path.join(OUT, "summit-walls.json"), JSON.stringify(plans, null, 1));
  }
  say("summit walls built", JSON.stringify(summitWalls));
  const dc = await lvl();
  say("cells after castles", JSON.stringify(dc.voxelCells), "blocks", JSON.stringify(dc.newFrames));

  // ---- utility crates: a rope, a girder or a jetpack on each trail's second hairpin ----
  const crates = [];
  for (const [t, contents] of [[0, "kUtilityNinjaRope"], [1, "kUtilityGirder"], [2, "kUtilityNinjaRope"], [3, "kUtilityJetpack"]]) {
    const q = T.TRAILS[t].hairpins[1];
    const r = await call("place_object", { type: "crate", kind: "utility", contents, units: "map", xz: [round1(q[0]), round1(q[1])] }, { soft: true });
    crates.push(brief(r, 120));
  }
  say("crates", crates.join(" | "));

  // ---- the mine factory: on a basalt plug standing on the crater floor (the drop never lands on a draped skin, docs/erg-mcp.md,
  // "The mine factory's drop", so the plug is a solid tower of undraped blocks). The scheme's start mines are off. ----
  const [fx, fz] = [PFX, PFZ];
  const fg = await groundOf(fx, fz);
  const PLUG_H = +(process.env.PLUG_H ?? 9);
  const plug = await call("build_tower", { at: [fx, round1(fg - 3), fz], radius: +(process.env.PLUG_R ?? 5), height: PLUG_H, hollow: false, material: MAT.hot, foundation: 2, place: "new", blockScale: 1.5 }, { soft: true });
  say("plug", fx, fz, "floor", round1(fg), brief(plug, 300));
  // The crater floor is a hollow skin (7.5 thick) over nothing down to the sea floor: a solid block of 3-unit voxels under and round the plug
  // (18 x 18 map, from 4 up into the skin) so nothing a mine or a blast does can open the bottom of the crater.
  // its top reaches 4-5 into the skin (the floor less 7.5 is the underside), so it is joined to the island, not a floating piece
  const fill = await call("add_block", { size: [6, Math.ceil((fg - 9) / 3), 6], scale: 3, at: [fx, 4, fz], units: "map", fill: "solid", material: MAT.hot }, { soft: true });
  say("crater fill", fill.err ? fill.text.slice(0, 300) : brief(fill, 260));
  const topG = await groundOf(fx, fz);
  say("plug top", round1(topG));
  say("factory", brief(await call("place_object", { type: "minefactory", units: "map", xz: [fx, fz] }), 500));
  // Launchers on the crater's inner walls (the engine runs one factory, so these are its launch points): the volley's mines
  // are shared out over them, erupt LAUNCH_H up and land within LAUNCH_R of their launcher. Mines fall through a draped
  // skin (in the Tests 2026-11-08 every mine that landed on the crater's skin dropped to the sea; arcs thrown across the
  // crater lost most of theirs: docs/erg.md, the mine factory), so each launcher stands on a pad of undraped blocks, a
  // 6 x 6 vent of hot rock proud of the wall (its top half a unit over the highest ground under it), and the mines land on it.
  const LAUNCHERS = [];
  for (const [rho, th] of T.LAUNCHERS) {
    const [lx, lz] = T.pointAt(rho, th).map(round1);
    const gs = [];
    for (const dx of [-3, 0, 3]) for (const dz of [-3, 0, 3]) gs.push(await groundOf(round1(lx + dx), round1(lz + dz)));
    const bottom = round1(Math.min(...gs) - 1.5), top = Math.max(...gs) + 0.5;
    // (v2 round 3) a lip: a ring one voxel over the pad's dish, so a mine that bounces stays on it (round 2: one ran off the north pad
    // and fell through the crater's skin). (v2 round 4) 5 x 5 voxels of 2 map units (10 x 10 map, a 6 x 6 dish): the 1.5-unit pads
    // held mines at four bearings only and lost one in ten off the north pad's edge, the 1-unit blocks held every mine (docs/erg-mcp.md:
    // mines through the ground); PAD_SCALE=1.5 is round 3's pad
    const PS = +(process.env.PAD_SCALE ?? 2), PN = PS === 2 ? 5 : 6;
    const ny = Math.ceil((top - bottom) / PS);
    const pad = await call("add_block", { size: [PN, ny + 1, PN], scale: PS, at: [lx, bottom, lz], units: "map", fill: "solid", material: MAT.hot }, { soft: true });
    const lip = pad.err ? null : await call("fill_box_raw", { frame: pad.json.frameId, state: "empty", units: "grid", min: [1, ny, 1], max: [PN - 1, ny + 1, PN - 1] }, { soft: true });
    say("launcher pad", lx, lz, "ground", round1(Math.min(...gs)), "-", round1(Math.max(...gs)), pad.err ? pad.text.slice(0, 200) : brief(pad, 160), "lip", lip ? brief(lip, 120) : "-");
    // (v2 round 5) the pad whose mines leaked west in round 4 (3 of 40 volleys' mines left the pad at 9.6, -21.4 by its west edge, to the
    // crater floor at 43): 2 voxels more on the west, their top one voxel over the lip (a rim 4 over the dish there), and a catch slab of
    // 1-unit voxels a unit under the dish's floor over its west half, so a mine that goes through the dish lands on it
    if (!pad.err && T.pointAt(rho, th)[0] > 0 && T.pointAt(rho, th)[1] < -15) {
      const dish = bottom + ny * PS, wx = round1(lx - PN * PS / 2 - PS);
      const ext = await call("add_block", { size: [2, ny + 2, PN], scale: PS, at: [wx, bottom, lz], units: "map", fill: "solid", material: MAT.hot }, { soft: true });
      const cat = await call("add_block", { size: [4, 1, 6], scale: 1, at: [round1(lx - 1.5), round1(dish - 2), lz], units: "map", fill: "solid", material: MAT.hot }, { soft: true });
      say("pad west rim", wx, ext.err ? ext.text.slice(0, 200) : brief(ext, 200), "catch slab", cat.err ? cat.text.slice(0, 200) : brief(cat, 200));
    }
    LAUNCHERS.push([lx, lz]);
  }
  // (mines round 3, 2026-11-10) the cadence: a fire at every VOLLEY_EVERY-th turn start while fewer than MINES_TARGET mines lie about,
  // topping up to it (at most 10 a fire), instead of round 5's everyNTurns (activation 255). The engine keeps 32 slots for every
  // flying or lying payload (mines, shells, bomblets) and the 33rd deletes the oldest: a turn-count factory filled the island to
  // about 30 mines, Melange trimmed every later volley to the pool's room (0-3 mines), and a worm's cluster or airstrike shot
  // still took lying mines (docs/erg.md, The payload pool). A target of 24 leaves 8 slots for the worms' shots and keeps the
  // factory firing to the end of the match as mines go off (MINES_TARGET=0 or VOLLEY_MODE=every gives round 5's everyNTurns).
  const MINES_TARGET = +(process.env.MINES_TARGET ?? 24), VOLLEY_EVERY = +(process.env.VOLLEY_EVERY || 3);
  const cadence = MINES_TARGET > 0 && process.env.VOLLEY_MODE !== "every" ? { activation: MINES_TARGET, inactiveTurns: VOLLEY_EVERY } : { everyNTurns: VOLLEY_EVERY };
  say("factory settings", brief(await call("set_mine_factory", { ...cadence, schemeMines: false, keepKnot: true, launchHeight: +(process.env.LAUNCH_H || 14), launchRadius: LAUNCH_R,
    units: "map", launchers: LAUNCHERS }), 900));
  // (mines outward, 2026-11-09: the owner wants the mines all over the island, castles included) the throw: the launchers lob
  // their mines 20-115 map units out, a share of each volley aimed within 12 of a team castle's court, the rest anywhere over the
  // land (a landing point in the sea is drawn again), arcs topping out 25 over the higher end; Melange lands each where its
  // flight meets the land (docs/erg.md, Where a thrown mine lands). MINES_THROW=0 keeps round 5's straight-up eruptions.
  // (mines round 4) the castle share down from 40 to 25 % (round 3's landing map crowded the four castles and left the inner
  // and NW slopes thin); a volley of MINES_VOLLEY (4) a fire, so the 24 come in 6 volleys over the first rounds and the top-ups
  // after that stay small (round 3's three volleys of 10 filled the target by turn 9 and the island saw no fire until mines went);
  // the mines keep MINES_AVOID (5) map units off the trails and their branches (round 3: mine blasts on the SW trail holed the
  // 7.5-thick shell over the hollow cone and two worms fell through it and drowned; a mine's blast reaches about 2.5, the bed's
  // half-width is 2.6). Each trail's polyline is thinned to at most 32 points for the throw's avoid lines.
  const AVOID_R = +(process.env.MINES_AVOID ?? 5), VOLLEY = +(process.env.MINES_VOLLEY ?? 4), CASTLE_SHARE = +(process.env.MINES_CASTLE_SHARE ?? 25);
  const thin = (pts, max = 32) => { if (pts.length <= max) return pts; const out = []; for (let i = 0; i < max; i++) out.push(pts[Math.round(i * (pts.length - 1) / (max - 1))]); return out; };
  const avoidLines = AVOID_R > 0 ? trailLines.map((pl) => thin(pl.map((p) => [p[0], p[2]]))) : [];
  if (process.env.MINES_THROW !== "0")
    say("throw", brief(await call("set_mine_factory", { units: "map", throw: { minRange: +(process.env.THROW_MIN || 20), maxRange: +(process.env.THROW_MAX || 115), apex: +(process.env.THROW_APEX || 25),
      pattern: "targets", targets: T.CASTLES.filter((c) => c.team >= 0).map((c) => c.at.map(round1)), scatter: 12, targetShare: CASTLE_SHARE,
      ...(VOLLEY > 0 ? { volley: VOLLEY } : {}), ...(avoidLines.length ? { avoidRadius: AVOID_R, avoid: avoidLines } : {}) } }), 600));

  // ---- lights on three of the islets (only while the cells last) ----
  for (const [i, q] of T.ISLETS.slice(0, 3).entries()) {
    const name = "Islet light " + (i + 1);
    if ((await lvl()).voxelCells.free < 600) { say("outpost", name, "skipped: the cells are spent"); continue; }
    const [ox, oz] = T.pointAt(q.rho * T.scaleAt(q.th), q.th).map(round1);
    const og = await groundOf(ox, oz);
    const o = await call("build_tower", { at: [ox, round1(og - 1), oz], radius: 2.4, height: 8, hollow: false, foundation: 3, material: MAT.chipped, place: "new", blockScale: SCALE }, { soft: true });
    say("outpost", name, ox, oz, "ground", round1(og), o.err ? o.text.slice(0, 200) : "ok " + JSON.stringify(o.json?.usage ?? {}).slice(0, 160));
  }

  // ---- floating bits: a rotated castle's merlon can end up joined to nothing but a corner; check_islets finds them and each is
  // emptied out of its block (fill_box_raw over the piece's box) ----
  const isl = await call("check_islets", {}, { soft: true });
  // (v2 round 5) never a summit wall's own frames: a wall over a cut bank's drop stands on the bed's edge, joined to the skin only there
  const wallFrames = new Set(summitWalls.flatMap((w) => w.frames ?? []));
  for (const pc of (isl.json?.list ?? []).filter((q) => q.debris && q.frames?.length)) {
    if (pc.frames.some((fr) => wallFrames.has(fr))) { say("debris kept (a summit wall)", JSON.stringify(pc.min), JSON.stringify(pc.max), pc.frames.join(",")); continue; }
    for (const fr of pc.frames) say("debris", JSON.stringify(pc.min), JSON.stringify(pc.max), "frame", fr, brief(await call("fill_box_raw", { frame: fr, state: "empty", units: "map", min: pc.min.map((v) => v - 0.5), max: pc.max.map((v) => v + 0.5) }, { soft: true }), 160));
  }

  // ---- the spawns: team t starts all its worms in its own castle's court (team knots WORM4t..4t+3, place_spawn_knots mode
  // teams: the engine deals no team spawns, so the level's script chunk puts each worm on its team's knot at match start) ----
  fs.writeFileSync(path.join(OUT, "spots.json"), JSON.stringify(spots, null, 1));
  const spawns = [];
  // (v2 round 2) each spot on the line between the castle's gates (terrain.mjs laneSpawns: never against the keep), checked here:
  // the floor a worm at the court's height stands on, with room for it (query_ground atOrBelow, passableAt)
  const spawnCheck = [];
  for (const [t, name] of T.TEAM_CASTLES.entries()) {
    const c = T.CASTLES.find((k) => k.name === name);
    for (const [i, [x, z]] of T.laneSpawns(c).entries()) {
      const q = await call("query_ground", { x: round2(x), z: round2(z), units: "map", mode: "atOrBelow", y: c.court + 1 }, { soft: true });
      const y = q.json?.standOn?.map, pass = q.json?.passableAt?.passable;
      spawnCheck.push({ worm: 4 * t + i, castle: name, at: [round2(x), round2(z)], standOn: y, passable: pass, material: q.json?.material });
      if (typeof y !== "number" || Math.abs(y - c.court) > 0.75) throw new Error(`${name} spawn ${i} at ${round1(x)}, ${round1(z)}: no court floor (${q.text.slice(0, 300)})`);
      spawns.push({ worm: 4 * t + i, pos: [round2(x), round2(y), round2(z)], group: name });
    }
  }
  say("spawn spots", JSON.stringify(spawnCheck));
  const ps = await call("place_spawn_knots", { mode: "teams", units: "map", spawns, snap: "tread" });
  say("spawns", brief(ps, 1200));
  const placed = new Map((ps.json?.placed ?? []).map((p) => [p.worm, p.pos]));
  const spawnAt = (w) => placed.get(w) ?? spawns.find((s) => s.worm === w).pos;

  // ---- decoration (R4 polish): Camelot flags and banners on tower tops of Highwatch, Cinder and Strand. Copies draw but have NO collision.
  // Another theme's meshes: a base level of the Camelot theme is allowed with allowOtherTheme (`--no-scenery` leaves them out if the game refuses) ----
  if (!flag("--no-scenery")) {
    const tp = (n) => (spots[n] || []).filter((q) => q.kind === "tower" && q.ok).map((q) => q.at);
    const flags = [];
    const add = (at, res, sc, rot) => { if (at) flags.push({ resource: res, pos: [round1(at[0]), round1(at[1]), round1(at[2])], rot, scale: sc }); };
    add(keepTop.Highwatch, "CAMELOT16", 1.6, [0, 20, 0]);
    tp("Cinder").slice(0, 2).forEach((a, i) => add(a, i % 2 ? "CAMELOT19" : "CAMELOT16", 1.4, [0, 45 + 90 * i, 0]));
    tp("Strand").slice(0, 1).forEach((a) => add(a, "CAMELOT19", 1.3, [0, 100, 0]));
    const dec = await call("copy_base_scenery", { base: "Multi.NiceToSiege", allowOtherTheme: true, copies: flags }, { soft: true });
    say("scenery flags", flags.length, dec.err ? "REFUSED " + dec.text.slice(0, 400) : brief(dec, 300));
  }

  // No level script: the sea is the scheme's sudden death, and the map shows no text of its own (an old project's script is cleared)
  say("script", brief(await call("put_script", { text: "" }), 300));

  // ---- R4: a blast point on each team castle's uphill wall (local -z), for the test's destructibility step (test_explosion
  // {game: true, from} sets an explosion off there with shots before and after from outside the wall) ----
  const blasts = T.CASTLES.filter((c) => c.team >= 0).sort((a, b) => a.team - b.team).map((c) => {
    const [x, z] = T.toWorld(c, 0, -(c.across / 2 - 1)), [ex, ez] = T.toWorld(c, 0, -(c.across / 2 + 16));
    return { castle: c.name, team: c.team, at: [round1(x), round1(c.court + 2.5), round1(z)], from: [round1(ex), round1(c.court + 11), round1(ez)], court: c.court, centre: c.at.map(round1) };
  });
  const bp = await call("place_blast_points", { points: blasts.map((b) => b.at), units: "map" });
  (bp.json?.placed ?? []).forEach((p, i) => { blasts[i].knot = p.name; });
  say("blast points", brief(bp, 300));

  const sv = await call("save");
  say("save", brief(sv, 900));
  const d = await call("describe_level", { limit: 100 });
  fs.writeFileSync(path.join(OUT, "describe.json"), d.text);
  say("cells", JSON.stringify(d.json.limits.voxelCells), "blocks", JSON.stringify(d.json.limits.newFrames), "engineWarnings", JSON.stringify(d.json.engineWarnings), "problems", JSON.stringify(d.json.problems ?? []));
  const lo = await call("list_objects", { outsideFrames: "summary" });
  fs.writeFileSync(path.join(OUT, "objects.json"), lo.text);

  // ---- the tester's waypoints: per team, from its castle's court (the lane beside the keep) out of the far gate and up its trail,
  // every hairpin and the middle of every leg, [x, y, z] in map units (y on the switchbacks, whose legs lie over each other) ----
  const waypoints = T.TRAILS.map((t) => {
    const c = t.castles[0], wp = [];
    const yAt = (x, z) => { let best = null, bd = 1e9; for (const p of t.pts) { const dd = Math.hypot(p[0] - x, p[1] - z); if (dd < bd) { bd = dd; best = p; } } return best[2]; };
    const push = (x, z, y) => wp.push([round1(x), round1(y ?? yAt(x, z)), round1(z)]);
    const mid = T.toWorld(c, 0, c.gin.offset);
    push(mid[0], mid[1], c.court); push(c.gout.G[0], c.gout.G[1], c.court); push(c.gout.O[0], c.gout.O[1], c.court);
    const hp = t.col.hairpins;
    for (let j = 1; j < hp.length; j++) {
      const a = hp[j - 1], b = hp[j];
      // (v2 round 5) the last leg's middle and end from the trail's own points (its line moved uphill when it widened): the end is the
      // middle of the summit terrace
      if (j === hp.length - 1) { const iL = t.pinAt[t.pinAt.length - 1], m = t.pts[Math.round((iL + t.pts.length - 1) / 2)]; push(m[0], m[1], m[2]); push(t.end[0], t.end[1], t.end[2]); continue; }
      push((a[0] + b[0]) / 2, (a[1] + b[1]) / 2);
      push(b[0], b[1], t.hairpins[j - 1]?.[2]);
    }
    // each spawn's own route: its lead-in (the spot, the court's middle when the spot is behind it, the out gate, the point outside
    // it) and then the trail from its first leg
    const routes = [0, 1, 2, 3].map((i) => {
      const w = 4 * t.team + i, sp = spawnAt(w);
      const lead = T.leadIn(c, [sp[0], sp[2]]).map(([x, z]) => [round1(x), round1(c.court), round1(z)]);
      return { worm: w, waypoints: [...lead, ...wp.slice(3)] };
    });
    return { team: t.team, castle: c.name, trail: t.name, worms: [4 * t.team, 4 * t.team + 1, 4 * t.team + 2, 4 * t.team + 3], spawns: [0, 1, 2, 3].map((i) => spawnAt(4 * t.team + i)), summit: wp[wp.length - 1], waypoints: wp, routes };
  });
  fs.writeFileSync(path.join(OUT, "waypoints.json"), JSON.stringify(waypoints, null, 1));
  fs.writeFileSync(path.join(OUT, "blasts.json"), JSON.stringify(blasts, null, 1));
  // (v2 round 4) a camera pose on each castle for the Test's castle shots (round 3's castle preset, framing the team's worms too, put
  // the eye inside the cone once they had climbed): from the sea on the castle's own seaward axis, looking down into its court, the
  // nearest of a few distances whose line of sight clears the ground (terrain.mjs island(), 2 under the line) until the last sixth,
  // about 50 degrees down so the court shows over the seaward wall
  const castleCams = T.CASTLES.map((c) => {
    const tgt = [c.at[0], c.court + 2, c.at[1]];
    for (const [D, H] of [[24, 30], [28, 36], [32, 42], [38, 50], [44, 58], [50, 68]]) {
      const pos = [c.at[0] + c.f.ez[0] * D, c.court + H, c.at[1] + c.f.ez[1] * D];
      let clear = true;
      for (let k = 1; k < 50 && clear; k++) { const q = pos.map((v, i) => v + (tgt[i] - v) * k / 50); if (k < 42 && T.island(q[0], q[2]) > q[1] - 2) clear = false; }
      if (!clear) continue;
      const dx = pos[0] - tgt[0], dz = pos[2] - tgt[2];
      return { castle: c.name, team: c.team, court: c.court, centre: c.at.map(round1), pos: pos.map(round1), target: tgt.map(round1), fov: 55, units: "map",
        azimuth: Math.round(Math.atan2(dx, -dz) * 180 / Math.PI), elevation: Math.round(Math.atan2(H - 2, Math.hypot(dx, dz)) * 180 / Math.PI) };
    }
    return { castle: c.name, team: c.team, court: c.court, centre: c.at.map(round1), pos: null, note: "no clear pose from the sea" };
  });
  fs.writeFileSync(path.join(OUT, "castles.json"), JSON.stringify(castleCams, null, 1));
  say("castle cams", JSON.stringify(castleCams.map((k) => [k.castle, k.pos, k.azimuth, k.elevation])));
  // every column across each trail's bed and a margin: no drop over 3 and no hole a worm falls through (check_reachability's
  // graph keeps to the ground it stands on, so it passes a broken edge); skin corners near a trail are named to probe in game
  const tc = await call("check_trail", { trails: trailLines, names: trailNames, width: T.TRAIL.bed * 2, margin: 1 }, { soft: true });
  fs.writeFileSync(path.join(OUT, "trails.json"), tc.text);
  say("trails", brief(tc, 900));
  // (v2 round 3) a trap beside a trail (a drop with no walkable way back onto it: team 3 stayed in the NW trough) or a move
  // the walk refuses (a ridge between legs) fails the build; a V-drop onto the leg under is told and passes. TRAIL_SOFT=1 logs only.
  // (v2 round 4) on the four team trails the centreline walk is the gate (the tester steers the centreline); the walks 1.5 to either
  // side are logged: where two legs stack on the steep upper cone the bank between them leans into the edge of the lower bed (relax()
  // has no room for the step at 2.9 a unit), rises of 0.5-0.7 over half a unit there. TRAIL_GATE=all fails on any line, as round 3.
  // (v2 round 5) TRAIL_GATE=SW,NE (trail names) is the full gate on those trails only
  const gateAll = process.env.TRAIL_GATE === "all", team = new Set(T.TRAILS.map((t) => t.name)), gateOn = new Set((process.env.TRAIL_GATE ?? "").split(",").filter(Boolean));
  const trailFails = (tc.json?.trails ?? []).filter((t) => (t.drops?.traps ?? 0) > 0 ||
    (t.walk ?? []).some((w) => !w.ok && (gateAll || gateOn.has(t.name) || (w.offset === 0 && team.has(t.name)))));
  say("trail walks", JSON.stringify((tc.json?.trails ?? []).map((t) => [t.name, (t.walk ?? []).map((w) => [w.offset, w.blockedCount ?? 0])])));
  if (trailFails.length && !process.env.TRAIL_SOFT) throw new Error(`check_trail: ${trailFails.map((t) => `${t.name}: ${t.summary}`).join("; ")} (TRAIL_SOFT=1 to build anyway)`);
  say("waypoints", JSON.stringify(waypoints.map((w) => ({ team: w.team, castle: w.castle, n: w.waypoints.length, summit: w.summit }))));

  // Checks: every spawn reaches a trail's rim end on foot with the worm's real width (any trail), then each team's spawns
  // reach their own trail's rim end on trail and castle stone alone at 38 degrees (the trail is a way up, all of it) and never touching them
  const ends = T.TRAILS.map((t) => ({ x: round1(t.end[0]), z: round1(t.end[1]), radius: 4, name: t.name }));
  for (const [label, ms] of [["rim45", 45], ["rim38", 38]]) {
    const rc = await call("check_reachability", { maxSlope: ms, bodyWidth: WORM_W, targets: ends, render: ms === 45 }, { soft: true });
    if (rc.images[0]) fs.writeFileSync(path.join(OUT, `reach-${label}.png`), Buffer.from(rc.images[0].data, "base64"));
    fs.writeFileSync(path.join(OUT, `reach-${label}.json`), rc.text);
    say("reach", label, brief(rc, 700));
  }
  const ROUTE = [TRAIL_MAT, MAT.stone, MAT.flint, MAT.chipped];
  const tally = { only: 0, avoid: 0 };
  for (const [label, mode] of [["trailonly", "only"], ["offtrail", "avoid"]]) {
    for (const [k, t] of T.TRAILS.entries()) {
      const e = ends[k];
      const rc = await call("check_reachability", { maxSlope: 38, bodyWidth: WORM_W, target: { x: e.x, z: e.z, radius: 4 }, via: { materials: ROUTE, mode, near: 4 }, render: mode === "only" }, { soft: true });
      if (rc.images[0]) fs.writeFileSync(path.join(OUT, `reach-${label}-${t.name}.png`), Buffer.from(rc.images[0].data, "base64"));
      fs.writeFileSync(path.join(OUT, `reach-${label}-${t.name}.json`), rc.text);
      (rc.json?.spawns ?? []).forEach((sp) => { const w = +(String(sp.knot ?? sp.name ?? "").match(/\d+/)?.[0] ?? -1); if (Math.floor(w / 4) === t.team && sp.reachable) tally[mode]++; });
    }
    say("reach", label, `${tally[mode]} of 16 team spawns reach their own trail's rim end`);
  }
  const is = await call("check_islets", {}, { soft: true });
  fs.writeFileSync(path.join(OUT, "islets.json"), is.text);
  say("islets", brief(is, 500));

  // Renders: the overview and the topdown, the cuts, and each trail from below its column (the camera out on its bearing)
  const shots = [
    ["topdown", "render_topdown", { step: 1, scale: 3 }],
    ["side", "render_side", { x0: -T.BOX, z0: 0, x1: T.BOX, z1: 0, scale: 4 }],
    ["side-ns", "render_side", { x0: 0, z0: -T.BOX, x1: 0, z1: T.BOX, scale: 4 }],
    ["view-s", "render_view", { view: "se", fog: 900 }],
    ["view-n", "render_view", { view: "nw", fog: 900 }],
    ["view-crater", "render_view", { pos: [0, 150, 160], target: [0, 55, 0], fov: 50, fog: 900 }],
    ...T.TRAILS.map((t) => {
      const th = t.col.thc, c = [Math.cos(th), Math.sin(th)];
      return [`view-trail-${t.name}`, "render_view", { pos: [round1(c[0] * 150), 85, round1(c[1] * 150)], target: [round1(c[0] * 58), 30, round1(c[1] * 58)], fov: 55, fog: 900 }];
    }),
    // (v2 round 4) close views of each trail's middle legs (the landings and berms), and the castle cameras of out/castles.json
    ...T.TRAILS.map((t) => {
      const h = t.hairpins[1], th = t.col.thc, c = [Math.cos(th), Math.sin(th)];
      return [`close-trail-${t.name}`, "render_view", { pos: [round1(h[0] + c[0] * 45), round1(h[2] + 30), round1(h[1] + c[1] * 45)], target: [round1(h[0] - c[0] * 6), round1(h[2] + 4), round1(h[1] - c[1] * 6)], fov: 55, fog: 900 }];
    }),
    ...castleCams.filter((k) => k.pos).map((k) => [`cam-${k.castle}`, "render_view", { pos: k.pos, target: k.target, fov: k.fov, fog: 900 }]),
    // (v2 round 5) each summit terrace and its wall, from out on the trail's bearing and above
    ...T.TRAILS.map((t) => {
      const e = t.end, th = Math.atan2(e[1], e[0]), c = [Math.cos(th), Math.sin(th)];
      return [`summit-${t.name}`, "render_view", { pos: [round1(e[0] + c[0] * 26), round1(e[2] + 20), round1(e[1] + c[1] * 26)], target: [round1(e[0] - c[0] * 6), round1(e[2] - 2), round1(e[1] - c[1] * 6)], fov: 55, fog: 900 }];
    }),
  ];
  for (const [name, tool, args] of shots) {
    const r = await call(tool, args, { soft: true });
    if (r.images[0]) fs.writeFileSync(path.join(OUT, `${name}.png`), Buffer.from(r.images[0].data, "base64"));
    say(name, r.err ? r.text.slice(0, 300) : "ok");
  }
  fs.writeFileSync(path.join(OUT, "heightmap.txt"), (await call("heightmap_ascii", { step: 3 })).text);

  // Source export, copied next to this script.
  if (!flag("--no-export")) {
    const ex = await call("export", { modId: MOD, name: "Caldera", version: "2.0.0", mode: "source", survivor: false });
    say("export", brief(ex, 600));
    const modDir = path.join(GAME, "Mods", MOD);
    const pack = path.join(HERE, "pack");
    fs.rmSync(pack, { recursive: true, force: true });
    fs.cpSync(modDir, pack, { recursive: true });
    if (!flag("--keep-mod")) fs.rmSync(modDir, { recursive: true, force: true });
    say("pack", pack);
  }
  await call("close", { discard: true }, { soft: true });
} catch (e) {
  if (!String(e.message).startsWith("--plan")) throw e;
  say(e.message);
} finally {
  fs.writeFileSync(path.join(OUT, "calls.json"), JSON.stringify(log, null, 1));
  say("MCP tool calls:", stats.calls);
  await stop();
}
