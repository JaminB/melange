// Caldera: an Erg MCP benchmark map, built only through the erg-mcp server's tools (stdio JSON-RPC, no hand-written
// patches). A volcanic island on Multi.DinerMight, in the Prehistoric theme's materials (volcano rock, hot rocks, liquid magma)
// at night: a tall round cone whose flank climbs in treads between three bands of cliffs (a smooth drape skin, so the treads
// are slopes and not stairs), a crater with a breach facing south and a dais for the mine factory, a coast of beaches, headlands
// and coves that slopes under the sea, a few lava tongues of real magma texture that widen downslope, three roads that cut
// through the cliffs and run through seven castles (three on the shore, two on the lower treads, two high up), and a level
// script (script.lua) that raises the sea from round 4 until only the upper cone is dry.
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
// the crater walls), Metal 53 (the factory's dais).
const MAT = { grass: 0, olive: 2, rock: 4, grey: 6, path: 8, sand: 10, stone: 12, flint: 16, chipped: 27, ash: 49, magma: 50, hot: 51, metal: 53 };
const VOXEL = +(process.env.VOXEL || 2.5);       // map units per voxel of the island's skin
const THICK = +(process.env.THICK || 3);         // voxels of skin (hollow under it: a fifth of the cells of a solid core)
const SCALE = +(process.env.CASTLE_SCALE || 1.5);   // voxels of the castle blocks
const WATER_START = -20;                          // world units: the beaches are dry at the start
const FL = -T.SEA_FLOOR;                          // the skin's floor is SEA_FLOOR: a rule's heights are above it
const hh = (v) => v + FL;

// Materials of the skin, first rule that holds wins: the roads (a distance field to each polyline), the lava (a narrow molten
// channel inside a margin of hot rock; both widen downslope), the crater (dais, magma lake, hot walls), the black ash of the
// summit, the cliff faces, then bands by height from grey rock down to grass and sand; every band edge is dithered and
// wandered by noise (feather, jitter), so there are no straight steps.
const FLOOR = T.FLOOR_Y;
const MATERIALS = [
  // the castles' courtyards are flagstones (one rule: the footprint of each, a box's distance field)
  { material: MAT.flint, masks: T.CASTLES.map((c) => { const hx = (c.axis === "x" ? c.along : c.across) / 2 - 0.5, hz = (c.axis === "x" ? c.across : c.along) / 2 - 0.5;
      return `min(${hx.toFixed(1)} - abs(x - ${c.at[0].toFixed(1)}), ${hz.toFixed(1)} - abs(z - ${c.at[1].toFixed(1)}))`; }) },
  ...T.ROADS.map((rd) => ({ material: MAT.path, mask: T.roadMask(rd, 2.7, 7), feather: { width: 1.4 } })),
  { material: MAT.magma, mask: T.tonguesMask(T.TONGUES, 0.6), feather: { width: 1.2 } },
  { material: MAT.hot, mask: T.tonguesMask(T.TONGUES, 1.0), feather: { width: 2.2 } },
  { material: MAT.metal, minHeight: hh(FLOOR + 1), mask: "6.4 - r" },                    // the factory's dais
  { material: MAT.magma, maxHeight: hh(FLOOR + 2.2), mask: "14.5 - r", feather: { width: 1.5 } },   // the magma lake on the crater floor
  { material: MAT.hot, minHeight: hh(FLOOR + 2.2), mask: "30 - r", feather: { width: 2 } },         // the crater's walls and rim
  { material: MAT.ash, minHeight: hh(37), feather: { width: 4 }, jitter: { amount: 2.5, scale: 9, seed: 3 } },
  { material: MAT.grey, minSlope: 46, feather: { width: 3 } },                          // cliff faces
  { material: MAT.grey, minHeight: hh(31), feather: { width: 4 }, jitter: { amount: 3, scale: 11, seed: 4 } },
  { material: MAT.rock, minHeight: hh(15), feather: { width: 4 }, jitter: { amount: 3, scale: 12, seed: 6 } },
  { material: MAT.olive, minHeight: hh(8), feather: { width: 3 }, noise: { threshold: 0.05, scale: 9, seed: 5 } },
  { material: MAT.grass, minHeight: hh(2.8), feather: { width: 2.5 }, jitter: { amount: 1.5, scale: 7, seed: 8 } },
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

  // Setting: the Prehistoric theme (its volcano rock, hot rocks and liquid magma materials) at night, so the ember sky sets off the cone; the base's terrain wiped (its frames stay, so its 11 652 cells stay spent); the diner's buildings, neon, signs, grass tufts and jukebox removed (they would show through the sea or poke through the cone); the sea starts below the beaches.
  await call("set_theme", { theme: "PREHISTORIC", materialFile: "theme" });
  await call("set_time_of_day", { timeOfDay: "NIGHT" });
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

  // ---- the castles: build_castle into new blocks of SCALE-unit voxels; a gate on the side the road comes from, a second one
  // carved in the opposite wall so the road runs through; towers taller than the walls; a keep ----
  const spots = {};                           // castle -> spawnSpots
  const towers = [];                          // [x, y, z] of tower tops, for the flags
  for (const c of T.CASTLES) {
    const [cx, cz] = c.at.map(round1);
    const y = c.floor;
    const g = await groundOf(cx, cz);
    const sx = c.axis === "x" ? c.along : c.across, sz = c.axis === "x" ? c.across : c.along;
    const p = c.plan;
    const r = await call("build_castle", { at: [cx, y - 1, cz], size: [sx, sz], wallHeight: p.wallHeight, wallThickness: 2, towerShape: p.towerShape, towerRadius: p.towerRadius,
      towerHeight: p.towerHeight, keep: true, keepSize: p.keep, keepHeight: p.keepHeight, gate: c.arrive, gateWidth: 5, gateHeight: 5, crenellations: true, foundation: 2,
      material: p.mat, place: "new", scale: SCALE, packing: "tiles" });
    // the second gate, in the wall on the other side (a box carved through it)
    const [gx, gz] = c.depart === "e" ? [cx + sx / 2 - 1, cz] : c.depart === "w" ? [cx - sx / 2 + 1, cz] : c.depart === "s" ? [cx, cz + sz / 2 - 1] : [cx, cz - sz / 2 + 1];
    const along = c.depart === "e" || c.depart === "w";
    // (fill_box_raw in the castle's own blocks: a sculpt carve would also cut the skin, which it reads unsheared)
    const hw = along ? [2.5, 2.5] : [2.5, 2.5];
    for (const b of r.json.newBlocks ?? [])
      await call("fill_box_raw", { frame: b.id, state: "empty", min: [round1(gx - 2.5), y - 1, round1(gz - 2.5)], max: [round1(gx + 2.5), y + 5, round1(gz + 2.5)], units: "map" }, { soft: true });
    spots[c.name] = r.json.spawnSpots?.spots ?? [];
    for (const s of spots[c.name]) if (s.kind === "tower" && s.ok) towers.push(s.at);
    say("castle", c.name, c.road, "floor", y, "ground", round1(g), "at", [cx, cz].join(","), "axis", c.axis, "gates", c.arrive, c.depart, "cells", r.json.usage?.voxelCells, "blocks", r.json.usage?.newBlocks);
  }
  if (process.env.DEBUG_GATE) {
    const c = T.CASTLES.find((k) => k.name === process.env.DEBUG_GATE);
    for (const [dx, dz] of (c.axis === "z") !== !!process.env.DEBUG_CROSS ? Array.from({ length: 35 }, (_, i) => [+(process.env.DEBUG_OFF ?? 0), i - 17]) : Array.from({ length: 35 }, (_, i) => [i - 17, +(process.env.DEBUG_OFF ?? 0)])) {
      const g = await call("query_ground", { x: round1(c.at[0] + dx), z: round1(c.at[1] + dz), units: "map" }, { soft: true });
      say("gate-probe", dx, dz, JSON.stringify(g.json?.standOn), JSON.stringify((g.json?.stretches ?? g.json?.solid ?? []).slice(0, 4)));
    }
  }
  const dc = await call("describe_level", { limit: 1 });
  say("cells after castles", JSON.stringify(dc.json.limits.voxelCells), "blocks", JSON.stringify(dc.json.limits.newFrames));

  // ---- utility crates: a rope or a girder on the road just after each castle's stretch ----
  const crates = [];
  for (const [rn, i, contents] of [["R1", 0, "kUtilityNinjaRope"], ["R1", 1, "kUtilityGirder"], ["R2", 0, "kUtilityGirder"], ["R2", 1, "kUtilityNinjaRope"], ["R3", 0, "kUtilityNinjaRope"], ["R3", 1, "kUtilityJetpack"]]) {
    const road = T.ROADS.find((r) => r.name === rn), k = road.castles[i];
    const run = k.run, q = run[run.length - 3];
    const px = q[0], pz = q[1];
    const r = await call("place_object", { type: "crate", kind: "utility", contents, units: "map", xz: [round1(px), round1(pz)] }, { soft: true });
    crates.push(brief(r, 120));
  }
  say("crates", crates.join(" | "));

  // ---- the mine factory on the crater's dais; mines come from it alone (the scheme's start mines are off), from the first turn ----
  say("factory", brief(await call("place_object", { type: "minefactory", units: "map", xz: [0, 0] }), 300));
  say("factory settings", brief(await call("set_mine_factory", { activation: 16, inactiveTurns: 0, schemeMines: false }), 300));

  // ---- the spawns: 8 knots over the seven castles, courtyards and wall-walks, low and high ----
  const pick = (name, kind, i = 0) => {
    const l = (spots[name] || []).filter((s) => s.kind === kind && s.ok);
    if (!l[i]) throw new Error(`no ${kind} spot ${i} in ${name}: ${JSON.stringify(spots[name]).slice(0, 300)}`);
    return l[i].standOn ?? l[i].at;
  };
  fs.writeFileSync(path.join(OUT, "spots.json"), JSON.stringify(spots, null, 1));
  const SPAWNS = [["Harbour", "courtyard", 0], ["Seawall", "courtyard", 0], ["Strand", "rampart", 0], ["Ashgate", "courtyard", 0],
                  ["Ember", "courtyard", 0], ["Cinder", "courtyard", 0], ["Highwatch", "courtyard", 0], ["Cinder", "rampart", 0]];
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
  // checked to its own end: the rim ring itself is not road); never touching them, how many do (the cliff bands are the barrier).
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
  const fl = await call("check_flood", { schedule: "script", target: { x: rim[0], z: rim[1], radius: 5 }, maxSlope: 38 }, { soft: true });
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
    ["view-s", "render_view", { view: "se", fog: 600 }],
    ["view-n", "render_view", { view: "nw", fog: 600 }],
    ["view-crater", "render_view", { pos: [0, 120, 150], target: [0, 45, 0], fov: 50, fog: 800 }],
    ["view-shore", "render_view", { pos: [-40, 20, 120], target: [-60, 12, 20], fov: 55, fog: 800 }],
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
