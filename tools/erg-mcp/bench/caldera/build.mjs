// Caldera: an Erg MCP benchmark map, built only through the erg-mcp server's tools (stdio JSON-RPC, no hand-written
// patches). A volcanic island on Multi.DinerMight, in the Prehistoric theme's materials (volcano rock, hot rocks, liquid magma)
// at dusk: a tall round cone (the summit rim about 72-80 map units, the castles at 30 and under) with a cliff band half way up,
// ribs and gullies down its flanks, a wide crater with a breach facing south, a basalt plug on the crater floor that carries the
// mine factory, a coast of wide beaches, headlands and coves, lava tongues of real magma texture that run from the crater to
// the shore, three roads that climb the cone in spirals through seven castles of different plans (a classic keep, a tower
// house, an L-shaped harbour fort with a sea gate, a long wall on a cliff edge, a big bailey, a round keep on a crag) and a
// level script (script.lua) that raises the sea from round 5 until only the cone and the two high castles are dry.
//
//   tools\node\node.exe tools\erg-mcp\bench\caldera\build.mjs [--out <dir>] [--plan] [--no-export] [--keep-mod] [--no-scenery]
//
// It connects like erg-mcp does: MELANGE_GAME_DIR (default C:\Users\Jamin\Desktop\WUMFix\testenv\C) with the game
// closed, starting Melange.exe --serve (MELANGE_EXE names another Melange.exe; default this checkout's
// build\x86-release\Melange.exe). The project id is `caldera`; an old one is deleted first (delete_project), so
// every run starts from the base. Renders go to --out (default out\ next to this file); the source export is copied to
// pack\ next to this file. --plan stops after the heightfield plan (nothing is applied or saved). The terrain itself
// (the height, the roads, the castle sites, the lava tongues) is terrain.mjs; the MCP client is mcpclient.mjs.
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
const GAME = process.env.MELANGE_GAME_DIR || "C:/Users/Jamin/Desktop/WUMFix/testenv/C";
process.env.MELANGE_EXE ??= path.join(ROOT, "build/x86-release/Melange.exe");
const ID = process.env.CALDERA_ID || "caldera";
const MOD = "caldera";

// Materials of the Prehistoric theme's file (set_theme PREHISTORIC, materialFile "theme"): grass01 0, grass02 2 (olive), rock01 4,
// rock02 6 (grey-brown: cliff faces), path01 8 (the roads), sand01 10, stone 12, flint 16, chippedstone 27 (the castles),
// VolcanoRock 49 (near black: the ash of the summit), LiquidHotMagma 50 (the lava), HotRocks 51 (dark red: its margins,
// the crater walls and the plug), Metal 53.
const MAT = { grass: 0, olive: 2, rock: 4, grey: 6, path: 8, sand: 10, stone: 12, flint: 16, chipped: 27, ash: 49, magma: 50, hot: 51, metal: 53 };
const VOXEL = +(process.env.VOXEL || 2.5);       // map units per voxel of the island's skin
const THICK = +(process.env.THICK || 3);         // voxels of skin (hollow under it: a fifth of the cells of a solid core)
const SCALE = +(process.env.CASTLE_SCALE || 1.5);   // voxels of the castle blocks
const WATER_START = 36;                           // world units (1.8 map): over the diner backdrop's ground planes, so the island stands in a sea from turn 1; the beaches' sand is cut to it
// the rising sea (script.lua), world units: from turn 5 +15 a turn to 156 (turn 12), from turn 13 +50 a turn to the cap 500 (25 map: turn 19)
const SEA = { first: 5, step: 15, cap1: 156, second: 13, step2: 50, cap: 500 };
const FL = -T.SEA_FLOOR;                          // the skin's floor is SEA_FLOOR: a rule's heights are above it
const hh = (v) => v + FL;

// Materials of the skin, first rule that holds wins: the castles' courtyards, the roads (a distance field to each polyline),
// the lava (a molten channel inside a margin of hot rock; both widen downslope, run from the crater to the beach and no further),
// the crater (a magma lake, hot patches on an ash floor, hot walls), the black ash of the summit, the cliff faces, then bands by
// height from grey rock down to grass and sand; every band edge is wandered by noise (feather perlin, jitter), so there are no
// straight steps and no dithered speckle.
const FLOOR = T.FLOOR_Y;
const courtyard = (c) => { const [x0, z0, x1, z1] = T.footRect(c, -0.5); return `min(${(x1).toFixed(1)} - x, x - ${x0.toFixed(1)}, ${z1.toFixed(1)} - z, z - ${z0.toFixed(1)})`; };
const MATERIALS = [
  { material: MAT.flint, masks: T.CASTLES.map(courtyard) },
  ...T.ROADS.map((rd) => ({ material: MAT.path, mask: T.roadMask(rd, 2.9, 7), feather: { width: 1.2, pattern: "perlin", cell: 4 } })),
  { material: MAT.magma, minHeight: hh(2.4), mask: T.tonguesMask(T.TONGUES, 0.62), feather: { width: 1.6, pattern: "perlin", cell: 5 } },
  { material: MAT.hot, minHeight: hh(2.4), mask: T.tonguesMask(T.TONGUES, 1.0), feather: { width: 3, pattern: "perlin", cell: 5 } },
  { material: MAT.magma, maxHeight: hh(FLOOR + 1.5), mask: "12 - r", feather: { width: 2, pattern: "perlin", cell: 5 } },                // the magma lake on the crater floor
  { material: MAT.hot, minHeight: hh(FLOOR - 1), maxHeight: hh(FLOOR + 2.5), mask: `${T.FLAT_R + 2} - r`, noise: { threshold: 0.15, scale: 8, seed: 21 } },   // hot patches on the floor
  { material: MAT.hot, minHeight: hh(FLOOR + 2.5), mask: `${T.RC + 1.5} - r`, feather: { width: 3, pattern: "perlin", cell: 6 } },    // the crater's walls and rim
  { material: MAT.ash, minHeight: hh(FLOOR - 3), mask: `${T.RC + 14} - r`, feather: { width: 5, pattern: "perlin", cell: 8 }, jitter: { amount: 2.5, scale: 9, seed: 3 } },   // the floor and the summit's ash
  { material: MAT.grey, minSlope: 46, feather: { width: 3, pattern: "perlin", cell: 6 } },                          // cliff faces
  // ash and dark rock in irregular patches down the lower flanks, so the cone reads dark against the castles
  { material: MAT.ash, minHeight: hh(14), maxHeight: hh(60), noise: { threshold: 0.2, scale: 14, seed: 11 } },
  { material: MAT.grey, minHeight: hh(8), maxHeight: hh(40), noise: { threshold: 0.3, scale: 10, seed: 12 } },
  { material: MAT.grey, minHeight: hh(36), feather: { width: 4, pattern: "perlin", cell: 9 }, jitter: { amount: 3, scale: 11, seed: 4 } },
  { material: MAT.rock, minHeight: hh(15), feather: { width: 4, pattern: "perlin", cell: 9 }, jitter: { amount: 3, scale: 12, seed: 6 } },
  { material: MAT.olive, minHeight: hh(8.5), feather: { width: 3, pattern: "perlin", cell: 7 }, noise: { threshold: 0.05, scale: 9, seed: 5 } },
  { material: MAT.grass, minHeight: hh(5.5), feather: { width: 2.5, pattern: "perlin", cell: 6 }, jitter: { amount: 1.5, scale: 7, seed: 8 } },
];

// ---- the MCP client ----
const say = (...a) => console.log(...a);
const brief = (r, n = 300) => r.text.slice(0, n).replace(/\s+/g, " ");
const round1 = (v) => Math.round(v * 10) / 10;

fs.mkdirSync(OUT, { recursive: true });
const { call, stop, log, stats } = await startServer();
try {
  say("connect", brief(await call("connect", { gameDir: GAME }), 160));
  if ((await call("list_bases", { search: "dinermight" })).json.projects.some((p) => p.id === ID))
    say("delete the old project", brief(await call("delete_project", { id: ID }), 200));
  const np = await call("new_project", { base: "Multi.DinerMight", slug: ID, title: "Caldera" });
  if (np.json?.created?.id !== ID) throw new Error(`new_project gave id ${np.json?.created?.id}, not ${ID}`);
  await call("open_project", { id: ID });

  // Setting: the Prehistoric theme (its volcano rock, hot rocks and liquid magma materials) at dusk (EVENING: the lava glows
  // against a warm sky); the base's terrain wiped (its frames stay, so its 11 652 cells stay spent); the diner's buildings, neon,
  // signs, grass tufts and jukebox removed (they would show through the sea or poke through the cone); the sea starts below the beaches.
  await call("set_theme", { theme: "PREHISTORIC", materialFile: "theme" });
  await call("set_time_of_day", { timeOfDay: process.env.TOD || "EVENING" });
  await call("set_water", { level: WATER_START });
  await call("set_surround_mode", { mode: "none" });
  const sc0 = await call("list_scenery", { limit: 1 }, { soft: true });
  say("scenery of the base", JSON.stringify(sc0.json?.counts ?? sc0.text.slice(0, 300)).slice(0, 600));
  for (const search of ["Neon", "Grass", "BUILDING", "D05", "MusicNotes", "Applause", "JukeBox"]) say("remove", search, brief(await call("remove_scenery", { search }, { soft: true }), 120));
  say("clear", brief(await call("clear_base_terrain", { all: true, wipe: true }), 200));
  const d0 = await call("describe_level", { limit: 1 });
  say("cells before", JSON.stringify(d0.json.limits.voxelCells));

  // ---- the island: one drape skin over the whole rectangle (the height is terrain.mjs's island()) ----
  const grid = T.relax(T.gridOf(-T.BOX, T.BOX, 1));
  const field = { min: [-T.BOX, -T.BOX], max: [T.BOX, T.BOX], centre: [0, 0], floor: T.SEA_FLOOR, voxelSize: VOXEL, grid, clip: [{ x: 0, z: 0, radius: T.CLIP }],
    thickness: THICK, core: false, material: MAT.sand, materials: MATERIALS, maxBlocks: 20,
    // the shore's columns (the skin dips to the sea's zero there) are solid from zero up, so the sea never shows the skin's hollow underside
    skirtTo: +(process.env.SKIRT_TO ?? 0), skirtTop: +(process.env.SKIRT_TOP ?? 0) };
  const plan = await call("heightfield_drape", { ...field, dryRun: true });
  say("plan fits", plan.json.fits, "skin", JSON.stringify({ blocks: plan.json.skin?.blocks, cells: plan.json.skin?.cells }), "budget", JSON.stringify(plan.json.budget?.cells), plan.json.why ?? "", JSON.stringify(plan.json.notes ?? []));
  fs.writeFileSync(path.join(OUT, "plan-drape.json"), plan.text);
  if (flag("--plan")) { await call("close", { discard: true }, { soft: true }); throw new Error("--plan: stopped"); }
  const hf = await call("heightfield_drape", field);
  say("island", JSON.stringify(hf.json.budget?.cells ?? hf.json.limits?.voxelCells), "notes", JSON.stringify(hf.json.notes ?? []));
  const groundOf = async (x, z) => {
    const g = await call("query_ground", { x, z, units: "map" });
    const top = g.json?.standOn?.map;
    if (typeof top !== "number") throw new Error(`no ground at ${x}, ${z}: ${g.text.slice(0, 300)}`);
    return top;
  };
  const lvl = async () => (await call("describe_level", { limit: 1 })).json.limits;

  // ---- the castles, each into new blocks of SCALE-unit voxels, a gate where its road comes in and one where it leaves ----
  const spots = {};                           // castle -> spawn spots [{kind, at, standOn, ok}]
  const towers = [];                          // [x, y, z] of tower tops, for the flags
  const common = { place: "new", scale: SCALE, packing: "tiles" };
  const sizeOf = (c, w, a) => (c.axis === "x" ? [w, a] : [a, w]);        // [along the road's axis, across it] as [x, z]
  const sideOf = (d) => (d[0] > 0 ? "e" : d[0] < 0 ? "w" : d[1] > 0 ? "s" : "n");
  const opp = { n: "s", s: "n", e: "w", w: "e" };
  const gateOf = (g, wall) => ({ side: g.side, offset: round1(g.offset), width: 4, height: Math.min(5, wall - 1.5) });
  const carve = async (blocks, y, [x0, z0, x1, z1], up = 6) => {
    for (const b of blocks ?? []) await call("fill_box_raw", { frame: b.id, state: "empty", min: [round1(x0), y - 1, round1(z0)], max: [round1(x1), y + up, round1(z1)], units: "map" }, { soft: true });
  };
  for (const c of T.CASTLES) {
    const [cx, cz] = c.at.map(round1);
    const y = c.floor, p = c.plan;
    const g = await groundOf(cx, cz);
    let usage = null;
    if (p.kind === "castle") {
      const r = await call("build_castle", { at: [cx, y - 1, cz], size: sizeOf(c, c.along, c.across), wallHeight: p.wallHeight, wallThickness: 2, towerShape: p.towerShape, towerRadius: p.towerRadius,
        towerHeight: p.towerHeight, keep: !!p.keep, keepSize: p.keep ?? undefined, keepHeight: p.keep ? p.keepHeight : undefined, gates: [gateOf(c.gin, p.wallHeight), gateOf(c.gout, p.wallHeight)], gateBelow: 0, crenellations: true, foundation: 2, material: p.mat, ...common });
      spots[c.name] = r.json.spawnSpots?.spots ?? [];
      usage = r.json.usage;
    } else if (p.kind === "lfort") {
      // an L: the main court on the road and a wing on the sea side, joined by a door in the shared wall; a sea gate in the wing's far wall
      const ws = sideOf(c.wdir), [wa, wc] = p.wing;
      const r = await call("build_castle", { at: [cx, y - 1, cz], size: sizeOf(c, c.along, c.across), wallHeight: p.wallHeight, wallThickness: 2, towerShape: p.towerShape, towerRadius: p.towerRadius,
        towerHeight: p.towerHeight, keep: !!p.keep, keepSize: p.keep ?? undefined, keepHeight: p.keep ? p.keepHeight : undefined, gates: [gateOf(c.gin, p.wallHeight), gateOf(c.gout, p.wallHeight), { side: ws, width: 4, height: 4 }], gateBelow: 0, crenellations: true, foundation: 2, material: p.mat, ...common });
      const wx = cx + c.wdir[0] * (c.across / 2 + wc / 2 - 2), wz = cz + c.wdir[1] * (c.across / 2 + wc / 2 - 2);
      const w = await call("build_castle", { at: [round1(wx), y - 1, round1(wz)], size: sizeOf(c, wa, wc), wallHeight: p.wallHeight, wallThickness: 2, towerShape: p.towerShape, towerRadius: p.towerRadius - 0.4,
        towerHeight: p.towerHeight - 2, keep: false, gates: [{ side: opp[ws], width: 4, height: 4 }, { side: ws, width: 6, height: 4.5, below: "auto" }], crenellations: true, foundation: 2, material: p.mat, ...common });
      spots[c.name] = [...(r.json.spawnSpots?.spots ?? []), ...(w.json.spawnSpots?.spots ?? [])];
      usage = w.json.usage;
    } else {
      // a round keep on a crag: a ring wall of 16 sides round a round keep set uphill of the centre, the road passing between them
      const R = p.ringRadius, U = [-c.at[0] / Math.hypot(...c.at), -c.at[1] / Math.hypot(...c.at)];
      const pts = Array.from({ length: 16 }, (_, i) => [round1(cx + R * Math.cos(i * Math.PI / 8)), y - 1, round1(cz + R * Math.sin(i * Math.PI / 8))]);
      const wl = await call("build_wall", { points: pts, closed: true, height: p.wallHeight, thickness: 2.5, crenellations: true, foundation: 2, material: p.mat, place: "new", blockScale: SCALE, packing: "tiles" });
      const kp = [round1(cx + U[0] * 6), round1(cz + U[1] * 6)];
      const kt = await call("build_tower", { at: [kp[0], y - 1, kp[1]], radius: p.keepRadius, height: p.keepHeight, hollow: true, wallThickness: 2, door: "s", doorWidth: 3, doorHeight: 4, foundation: 2, material: p.mat, place: "new", blockScale: SCALE });
      // the gates: where the road line meets the ring
      for (const s of [-1, 1]) await carve(wl.json.newBlocks, y, [cx + c.T[0] * R * s - 2.5, cz + c.T[1] * R * s - 2.5, cx + c.T[0] * R * s + 2.5, cz + c.T[1] * R * s + 2.5], p.wallHeight + 2);
      const gt = [round1(cx - U[0] * 5), round1(cz - U[1] * 5)], wt = [round1(cx - U[0] * (R - 0.5)), round1(cz - U[1] * (R - 0.5))];
      spots[c.name] = [];
      for (const [kind, [sx, sz]] of [["courtyard", gt], ["rampart", wt]]) {
        const q = await call("query_ground", { x: sx, z: sz, units: "map" }, { soft: true });
        const top = q.json?.standOn?.map;
        spots[c.name].push({ kind, at: [sx, top, sz], standOn: [sx, top, sz], ok: typeof top === "number" });
      }
      usage = kt.json?.usage ?? wl.json?.usage;
      towers.push([kp[0], y + p.keepHeight, kp[1]]);
    }
    for (const s of spots[c.name]) if (s.kind === "tower" && s.ok) towers.push(s.at);
    say("castle", c.name, p.kind, "floor", y, "ground", round1(g), "at", [cx, cz].join(","), "axis", c.axis, "gates", c.arrive, c.depart, "cells", usage?.voxelCells, "blocks", usage?.newBlocks);
  }
  const dc = await lvl();
  say("cells after castles", JSON.stringify(dc.voxelCells), "blocks", JSON.stringify(dc.newFrames));

  // ---- utility crates: a rope or a girder on the road just outside each castle's far gate ----
  const crates = [];
  for (const [rn, i, contents] of [["R1", 0, "kUtilityNinjaRope"], ["R1", 1, "kUtilityGirder"], ["R2", 0, "kUtilityGirder"], ["R2", 1, "kUtilityNinjaRope"], ["R3", 0, "kUtilityNinjaRope"], ["R3", 1, "kUtilityJetpack"]]) {
    const road = T.ROADS.find((r) => r.name === rn), k = road.castles[i];
    const q = k.run[k.run.length - 1];
    const r = await call("place_object", { type: "crate", kind: "utility", contents, units: "map", xz: [round1(q[0]), round1(q[1])] }, { soft: true });
    crates.push(brief(r, 120));
  }
  say("crates", crates.join(" | "));

  // ---- the mine factory: on a basalt plug standing on the crater floor (the drop never lands on a draped skin, docs/erg-mcp.md,
  // "The mine factory's drop", so the plug is a solid tower of undraped blocks). Mines are fired from it up and out over the rim.
  // The scheme's start mines are off. ----
  const [fx, fz] = T.pointAt(T.PLUG_RHO, T.PLUG_TH).map(round1);
  const fg = await groundOf(fx, fz);
  const PLUG_H = +(process.env.PLUG_H ?? 9);
  const plug = await call("build_tower", { at: [fx, round1(fg - 3), fz], radius: +(process.env.PLUG_R ?? 4), height: PLUG_H, hollow: false, material: MAT.hot, foundation: 2, place: "new", blockScale: 1.5 }, { soft: true });
  say("plug", fx, fz, "floor", round1(fg), brief(plug, 300));
  const topG = await groundOf(fx, fz);
  say("plug top", round1(topG));
  say("factory", brief(await call("place_object", { type: "minefactory", units: "map", xz: [fx, fz] }), 500));
  say("factory settings", brief(await call("set_mine_factory", { activation: 16, inactiveTurns: 0, schemeMines: false, keepKnot: true }), 300));

  // ---- outposts off the roads, so the island is not a ring of castles: a watchtower on the north-east headland, a tower house on
  // the western bluff, a lookout on the northern gully's lip (only while the cells last) ----
  const OUTPOSTS = [["Pharos", -0.62, 84, 3.4, 13], ["Tower house", 3.62, 74, 3.0, 11], ["Gullywatch", -1.62, 68, 2.8, 9]];
  for (const [name, th, rho, rad, hgt] of OUTPOSTS) {
    if ((await lvl()).voxelCells.free < 1200) { say("outpost", name, "skipped: the cells are spent"); continue; }
    const [ox, oz] = T.pointAt(rho, th).map(round1);
    const og = await groundOf(ox, oz);
    const o = await call("build_tower", { at: [ox, round1(og - 1), oz], radius: rad, height: hgt, hollow: false, foundation: 3, material: MAT.chipped, place: "new", blockScale: SCALE }, { soft: true });
    say("outpost", name, ox, oz, "ground", round1(og), o.err ? o.text.slice(0, 200) : "ok " + JSON.stringify(o.json?.usage ?? {}).slice(0, 160));
    if (!o.err) towers.push([ox, round1(og - 1 + hgt), oz]);
  }

  // ---- the spawns: 8 knots over the seven castles, courtyards and wall-walks, low and high ----
  const pick = (name, kind, i = 0) => {
    const l = (spots[name] || []).filter((s) => s.kind === kind && s.ok);
    if (!l[i]) throw new Error(`no ${kind} spot ${i} in ${name}: ${JSON.stringify(spots[name]).slice(0, 300)}`);
    return l[i].standOn ?? l[i].at;
  };
  fs.writeFileSync(path.join(OUT, "spots.json"), JSON.stringify(spots, null, 1));
  // WORM0-3 are team 0, WORM4-7 team 1, and a worm moves first in turn 1 + team + 2 * index; so each team's first mover (WORM0, WORM4)
  // stands on a shore castle (its two moves come before the sea reaches it), then low, then two high ones: one shore worm and two high per team
  const SPAWNS = [["Strand", "courtyard", 0], ["Ashgate", "courtyard", 0], ["Cinder", "courtyard", 0], ["Highwatch", "courtyard", 0],
                  ["Harbour", "courtyard", 0], ["Ember", "courtyard", 0], ["Highwatch", "rampart", 0], ["Cinder", "courtyard", 1]];
  const spawns = SPAWNS.map(([n, k, i], worm) => ({ worm, pos: pick(n, k, i).map(round1) }));
  say("spawns", brief(await call("place_spawn_knots", { mode: "knots", units: "map", spawns }), 800));

  // Camelot flags on the tower tops (no collision; Nice To Siege's pieces, another theme's meshes)
  if (!flag("--no-scenery")) {
    const copies = towers.slice(0, 40).map((t, i) => ({ resource: i % 2 ? "CAMELOT19" : "CAMELOT16", pos: [round1(t[0]), round1(t[1]), round1(t[2])], rot: [0, 0, 0], scale: 2.4 }));
    const sc = await call("copy_base_scenery", { base: "Multi.NiceToSiege", copies, allowOtherTheme: true, units: "map" }, { soft: true });
    say("scenery", copies.length, brief(sc, 300));
  }

  // The rising sea.
  const script = fs.readFileSync(path.join(HERE, "script.lua"), "utf8");
  say("script", brief(await call("put_script", { text: script }), 300));

  const sv = await call("save");
  say("save", brief(sv, 600));
  const d = await call("describe_level", { limit: 100 });
  fs.writeFileSync(path.join(OUT, "describe.json"), d.text);
  say("cells", JSON.stringify(d.json.limits.voxelCells), "blocks", JSON.stringify(d.json.limits.newFrames), "engineWarnings", JSON.stringify(d.json.engineWarnings), "problems", JSON.stringify(d.json.problems ?? []));
  const lo = await call("list_objects", {});
  fs.writeFileSync(path.join(OUT, "objects.json"), lo.text);

  // Checks: reachability from every spawn to the summit rim at two maxSlopes, the flood, islets.
  const rim = T.ROADS[0].end;
  for (const [label, args] of [["rim45", { maxSlope: 45, target: { x: rim[0], z: rim[1], radius: 5 } }],
                               ["rim38", { maxSlope: 38, target: { x: rim[0], z: rim[1], radius: 5 } }]]) {
    const rc = await call("check_reachability", args, { soft: true });
    fs.writeFileSync(path.join(OUT, `reach-${label}.json`), rc.text);
    say("reach", label, brief(rc, 700));
  }
  // The roads are the way up: on road and castle stone alone, every spawn reaches the rim where its own road ends (each road is
  // checked to its own end: the rim ring itself is not road); never touching them, how many do (the cliff band is the barrier).
  const ROUTE = [MAT.path, MAT.stone, MAT.flint, MAT.chipped];
  const roadOf = (name) => T.CASTLES.find((c) => c.name === name).road;
  const tally = { only: 0, avoid: 0 };
  for (const [label, mode] of [["roadonly", "only"], ["offroad", "avoid"]]) {
    for (const rd of T.ROADS) {
      const rc = await call("check_reachability", { maxSlope: 45, target: { x: rd.end[0], z: rd.end[1], radius: 4 }, via: { materials: ROUTE, mode, near: 4 }, render: rd.name === "R1" }, { soft: true });
      if (rc.images[0]) fs.writeFileSync(path.join(OUT, `reach-${label}.png`), Buffer.from(rc.images[0].data, "base64"));
      fs.writeFileSync(path.join(OUT, `reach-${label}-${rd.name}.json`), rc.text);
      (rc.json?.spawns ?? []).forEach((sp, i) => { if (roadOf(SPAWNS[i][0]) === rd.name && sp.reachable) tally[mode]++; });
    }
    say("reach", label, `${tally[mode]} of 8 spawns reach their own road's rim end`);
  }
  // the script's two phases (script.lua), as levels in the call's units (map), since the "script" schedule reads only its top-level call
  const levels = [{ turn: 1, level: WATER_START / 20 }];
  for (let t = SEA.first, w = WATER_START; w < SEA.cap; t++) { w = t < SEA.second ? Math.min(SEA.cap1, w + SEA.step) : Math.min(SEA.cap, w + SEA.step2); levels.push({ turn: t, level: w / 20 }); }
  const fl = await call("check_flood", { levels, start: WATER_START / 20, target: { x: rim[0], z: rim[1], radius: 5 }, maxSlope: 38 }, { soft: true });
  fs.writeFileSync(path.join(OUT, "flood.json"), fl.text);
  say("flood", brief(fl, 900));
  const is = await call("check_islets", {}, { soft: true });
  fs.writeFileSync(path.join(OUT, "islets.json"), is.text);
  say("islets", brief(is, 500));

  // Renders.
  const shots = [
    ["topdown", "render_topdown", { step: 1, scale: 3 }],
    ["side", "render_side", { x0: -T.BOX, z0: 0, x1: T.BOX, z1: 0, scale: 4 }],
    ["side-ns", "render_side", { x0: 0, z0: -T.BOX, x1: 0, z1: T.BOX, scale: 4 }],
    ["view-s", "render_view", { view: "se", fog: 900 }],
    ["view-n", "render_view", { view: "nw", fog: 900 }],
    ["view-crater", "render_view", { pos: [0, 150, 160], target: [0, 55, 0], fov: 50, fog: 900 }],
    ["view-shore", "render_view", { pos: [-40, 20, 140], target: [-60, 12, 20], fov: 55, fog: 900 }],
  ];
  for (const [name, tool, args] of shots) {
    const r = await call(tool, args, { soft: true });
    if (r.images[0]) fs.writeFileSync(path.join(OUT, `${name}.png`), Buffer.from(r.images[0].data, "base64"));
    say(name, r.err ? r.text.slice(0, 300) : "ok");
  }
  fs.writeFileSync(path.join(OUT, "heightmap.txt"), (await call("heightmap_ascii", { step: 3 })).text);

  // Source export, copied next to this script.
  if (!flag("--no-export")) {
    const ex = await call("export", { modId: MOD, name: "Caldera", version: "1.0.0", mode: "source", survivor: false });
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
