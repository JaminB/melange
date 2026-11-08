// Caldera: an Erg MCP benchmark map, built only through the erg-mcp server's tools (stdio JSON-RPC, no hand-written
// patches). A volcanic island on Multi.DinerMight at night: a tall round cone (a smooth drape skin, so the flanks are slopes
// and not stairs) with a crater and a breach facing south, lava tongues that follow gullies down the flanks, three roads cut
// into the cone that run through seven castles of pale stone (a shelf under each, a gate at each end), a mine factory in the
// crater, and a level script (script.lua) that raises the sea every turn from round 3 until only the upper cone is dry.
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

// Materials of Diner Might's file (Building theme): grass01 0, rock01 4 (tan), path01 8 (pale sand), sand01 10, stone 12 (pale
// grey), Leaves 13 (orange-brown), Road 14 (slate), Concrete 16 (warm white), JCB Yellow Metal 17 (the hot core of the lava),
// Red Metal 21, Tyre 24 (near black, the ash), Rubber matting 36 (dark blue-grey), concrete and bricks 55 (warm).
const MAT = { grass: 0, rock: 4, path: 8, sand: 10, ember: 13, slate: 14, hot: 17, lava: 21, ash: 24, rubber: 36 };
const VOXEL = +(process.env.VOXEL || 2.5);       // map units per voxel of the island's skin
const THICK = +(process.env.THICK || 3);         // voxels of skin (hollow under it: a fifth of the cells of a solid core)
const SCALE = +(process.env.CASTLE_SCALE || 1.5);   // voxels of the castle blocks
const WATER_START = -20;                          // world units: the beaches are dry at the start

// Materials of the skin, first rule that holds wins: lava, the crater, the rim's glow, the roads and shelves (flat ground on the
// flank is pale), then bands by height from the dark ash of the summit down to tan scree, grass and sand at the shore.
const FLOOR = T.FLOOR_Y;
const MATERIALS = [
  { material: MAT.hot, mask: T.tonguesMask(T.TONGUES.filter((t) => t.core), 0.42) },
  { material: MAT.lava, mask: T.tonguesMask(T.TONGUES) },
  { material: MAT.hot, maxHeight: FLOOR + 2, mask: "1.9 - abs(r - 8.5)" },               // a ring of yellow round the factory
  { material: MAT.lava, maxHeight: FLOOR + 2, mask: "13 - r" },                          // the lava lake on the crater floor
  { material: MAT.hot, minHeight: 76 },                                                  // the rim's lip glows
  { material: MAT.ember, minHeight: 71 },
  { material: MAT.path, minHeight: 10.6, maxHeight: 64, maxSlope: 30 },                  // roads and shelves
  { material: MAT.ash, minHeight: 56 },
  { material: MAT.rubber, minHeight: 41 },
  { material: MAT.slate, minHeight: 28 },
  { material: MAT.rock, minHeight: 15 },
  { material: MAT.sand, maxHeight: 3.4 },
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

  // Setting: the base's own material set (Building) at night (dusk made the sea the colour of sand, night a clear teal against
  // the dark cone); the base's terrain wiped (its frames stay, so its 11 652 cells stay spent); the heightmap hill and the
  // diner's neon and grass tufts removed (they show through the water); the sea starts below the beaches.
  await call("set_theme", { theme: "BUILDING" });
  await call("set_time_of_day", { timeOfDay: "NIGHT" });
  await call("set_water", { level: WATER_START });
  await call("set_surround_mode", { mode: "none" });
  for (const search of ["Neon", "Grass"]) say("remove", search, brief(await call("remove_scenery", { search }, { soft: true }), 120));
  say("clear", brief(await call("clear_base_terrain", { all: true, wipe: true }), 200));
  const d0 = await call("describe_level", { limit: 1 });
  say("cells before", JSON.stringify(d0.json.limits.voxelCells));

  // ---- the island: one drape skin over the whole rectangle (the height is terrain.mjs's island()) ----
  const field = { min: [-T.BOX, -T.BOX], max: [T.BOX, T.BOX], centre: [0, 0], floor: 0, voxelSize: VOXEL, grid: T.gridOf(-T.BOX, T.BOX, 1),
    thickness: THICK, core: false, material: MAT.grass, materials: MATERIALS, maxBlocks: 16 };
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
      towerHeight: p.towerHeight, keep: true, keepSize: p.keep, keepHeight: p.keepHeight, gate: c.arrive, gateWidth: 5, gateHeight: 5, crenellations: true,
      material: p.mat, place: "new", scale: SCALE, packing: "tiles" });
    // the second gate, in the wall on the other side (a box carved through it)
    const [gx, gz] = c.depart === "e" ? [cx + sx / 2 - 1, cz] : c.depart === "w" ? [cx - sx / 2 + 1, cz] : c.depart === "s" ? [cx, cz + sz / 2 - 1] : [cx, cz - sz / 2 + 1];
    const along = c.depart === "e" || c.depart === "w";
    // (fill_box_raw in the castle's own blocks: a sculpt carve would also cut the skin, which it reads unsheared)
    const hw = along ? [2.5, 2.5] : [2.5, 2.5];
    for (const b of r.json.newBlocks ?? [])
      await call("fill_box_raw", { frame: b.id, state: "empty", min: [round1(gx - (along ? 2.5 : 2.5)), y, round1(gz - 2.5)], max: [round1(gx + 2.5), y + 5, round1(gz + 2.5)], units: "map" }, { soft: true });
    spots[c.name] = r.json.spawnSpots?.spots ?? [];
    for (const s of spots[c.name]) if (s.kind === "tower" && s.ok) towers.push(s.at);
    say("castle", c.name, c.road, "floor", y, "ground", round1(g), "at", [cx, cz].join(","), "axis", c.axis, "gates", c.arrive, c.depart, "cells", r.json.usage?.voxelCells, "blocks", r.json.usage?.newBlocks);
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

  // ---- the mine factory in the crater; mines come from it alone (the scheme's start mines are off) ----
  say("factory", brief(await call("place_object", { type: "minefactory", units: "map", xz: [0, 0] }), 300));
  say("factory settings", brief(await call("set_mine_factory", { activation: 12, inactiveTurns: 1, schemeMines: false }), 300));

  // ---- the spawns: 8 knots over the seven castles, courtyards and wall-walks, low and high ----
  const pick = (name, kind, i = 0) => {
    const l = (spots[name] || []).filter((s) => s.kind === kind && s.ok);
    if (!l[i]) throw new Error(`no ${kind} spot ${i} in ${name}: ${JSON.stringify(spots[name]).slice(0, 300)}`);
    return l[i].standOn ?? l[i].at;
  };
  const SPAWNS = [["Ashgate", "courtyard", 0], ["Cinder", "rampart", 0], ["Harbour", "rampart", 0], ["Highwatch", "courtyard", 0],
                  ["Ember", "courtyard", 1], ["Seawall", "rampart", 1], ["Strand", "rampart", 0], ["Ashgate", "rampart", 2]];
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
