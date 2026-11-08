// Caldera: an Erg MCP benchmark map, built only through the erg-mcp server's tools (stdio JSON-RPC, no hand-written
// patches). A volcanic island on Multi.DinerMight at night: a concave cone with a crater at the summit holding the mine
// factory, gullies and three lava channels down its flanks, six castles on shelves round it, three lava tubes from the
// castles into the crater, and a level script (script.lua) that raises the sea once per round until only the upper
// cone is dry.
//
//   tools\node\node.exe tools\erg-mcp\bench\caldera\build.mjs [--out <dir>] [--plan] [--no-export] [--keep-mod]
//
// It connects like erg-mcp does: MELANGE_GAME_DIR (default C:\Users\Jamin\Desktop\WUMFix\testenv\C) with the game
// closed, starting Melange.exe --serve (MELANGE_EXE names another Melange.exe; default this checkout's
// build\x86-release\Melange.exe). The project id is `caldera`; an old one is deleted first (delete_project), so
// every run starts from the base. Renders go to --out (default out\ next to this file); the source export is copied to
// pack\ next to this file. --plan stops after the heightfield plans (nothing is applied or saved).
import { spawn } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, "../../../..");
const argv = process.argv.slice(2);
const flag = (f) => argv.includes(f);
const opt = (f, d) => { const i = argv.indexOf(f); return i >= 0 ? argv[i + 1] : d; };
const OUT = path.resolve(opt("--out", path.join(HERE, "out")));
const GAME = process.env.MELANGE_GAME_DIR || "C:/Users/Jamin/Desktop/WUMFix/testenv/C";
process.env.MELANGE_EXE ??= path.join(ROOT, "build/x86-release/Melange.exe");
const ID = "caldera";
const MOD = "caldera";

// ---- the design (map units: 1 voxel of the base = 1 map unit = 20 world units; y up, floor 0) ----
//
// Budget (heightfield_plan and dry-run castles): Diner Might's frames hold 11 652 of the game's 65 535 cells, leaving
// 53 883. Two heightfield calls share the island: a coarse one (voxel 3) for the lowlands and the cone's foot, and a
// fine one (voxel 2) inside r 46, where the flanks, the crater and the gullies show. The castles take what is left, in
// scale-1 blocks packed per wall.

const VC = 3;                      // lowland voxels, map units
const VF = 2;                      // cone voxels
const SLAB1 = 12, R1 = 58;         // slab under the cone's foot (height, radius)
const SLAB2 = 24, R2 = 30;         // slab under the cone's upper flanks
const ISLAND = 108;                // half side of the coarse heightfield rectangle
const CELLS_COARSE = +(process.env.CC || 14000);
const CELLS_MID = +(process.env.CM || 14000);
const CELLS_FINE = +(process.env.CF || 14000);
const WATER_START = -20;           // world units: the beaches are dry at the start
const TUBE_MOUTH = 31;             // r of the lava tubes' mouths
const CASTLE_SCALE = 1.5;         // castle voxels: a castle in scale-1 blocks packed per wall needs 9 blocks of the 64

// The cone: h = H (1 - r/R)^P is steepest at the top and runs out in a wide skirt; the crater is a bowl inside r = RC
// with a flat floor, its rim blending into the slope (no separate collar).
const H = 74, R = 100, P = 1.5, RC = 17, FLOOR_Y = 42, RIM = 2;
const coneAt = (r) => H * Math.pow(Math.max(0, 1 - r / R), P);

// Castle sites round the volcano: centre, shelf height (map units), outer size, wall material, style. The heightfield
// flattens a shelf under each; the gate faces the summit.
const CASTLES = [
  { name: "Seawall",  at: [-74, 12],  h: 10,   size: [16, 16], mat: 12, keep: true },
  { name: "Harbour",  at: [52, -56],  h: 12.5, size: [16, 16], mat: 37, keep: true },
  { name: "Headland", at: [8, 84],    h: 14,   size: [14, 14], mat: 55 },
  { name: "Ashgate",  at: [-44, -42], h: 15,   size: [14, 14], mat: 36, towerShape: "square" },
  { name: "Cinder",   at: [46, 20],   h: 19.5, size: [14, 14], mat: 37 },
  { name: "Ember",    at: [-30, 28],  h: 26,   size: [16, 16], mat: 14, keep: true, towerShape: "square" },
];
// Two worms in each large castle (either side of the keep), one in each of the others.
const SPAWNS = [[0, 0], [1, 1], [2, 1], [3, 2], [4, 3], [5, 4], [6, 5], [7, 5]];

// Diner Might's material file (Building theme): grass01 0, rock01 4, rock02 6, sand01 10, stone 12, Road 14 (slate),
// Red Metal 21 (lava), Tyre 24 (near black, for ash), Rubber matting 36, Brick colour 37, concrete and bricks 55.
const MAT = { rubber: 36, grass: 0, rock: 4, darkRock: 6, sand: 10, road: 14, lava: 21, ash: 24 };

// Lava gullies: angles (radians about the summit, as atan2(z, x)) of three channels cut into the flanks from the rim
// down to r 70, painted red afterwards.
const LAVA = [0.5, 2.5, 4.4];

// The height of the island at (x, z), map units above the floor. heightfield's expr language is limited to 500 parts,
// too few for coast, cone, gullies and six shelves, so the build sends the same surface as a grid (a height per map
// unit, sampled bilinearly): this function is the design.
function hash(i, j, seed) {
  let h = (Math.imul(i, 374761393) + Math.imul(j, 668265263) + Math.imul(seed, 2147483647)) | 0;
  h = Math.imul(h ^ (h >>> 13), 1274126177);
  return ((h ^ (h >>> 16)) >>> 0) / 4294967295 * 2 - 1;
}
function noise(x, z, seed) {
  const i = Math.floor(x), j = Math.floor(z), fx = x - i, fz = z - j;
  const u = fx * fx * (3 - 2 * fx), v = fz * fz * (3 - 2 * fz);
  const a = hash(i, j, seed), b = hash(i + 1, j, seed), c = hash(i, j + 1, seed), d = hash(i + 1, j + 1, seed);
  return (a + (b - a) * u) + ((c + (d - c) * u) - (a + (b - a) * u)) * v;
}
function fbm(x, z, seed, oct) {
  let sum = 0, amp = 1, norm = 0;
  for (let o = 0; o < oct; o++) { sum += amp * noise(x, z, seed + o * 31); norm += amp; x *= 2; z *= 2; amp *= 0.5; }
  return sum / norm;
}
const smooth = (e0, e1, v) => { const t = Math.min(1, Math.max(0, (v - e0) / (e1 - e0))); return t * t * (3 - 2 * t); };
const gauss = (x, z, cx, cz, s) => Math.exp(-((x - cx) ** 2 + (z - cz) ** 2) / (2 * s * s));
const HEADS = [[8, 92, 15, 14], [-70, -62, 13, 12]];     // headlands: centre, spread, how far they push the coast out
const COVE = [88, -24, 12, 26];                          // a cove beside Harbour
function height(x, z) {
  const r = Math.hypot(x, z), theta = Math.atan2(z, x);
  let coast = 88 + 8 * fbm(x / 45, z / 45, 11, 3) - COVE[3] * gauss(x, z, COVE[0], COVE[1], COVE[2]);
  let cliff = 0;
  for (const [cx, cz, s, a] of HEADS) { const w = gauss(x, z, cx, cz, s); coast += a * w; cliff = Math.max(cliff, w); }
  const land = smooth(coast + 2, coast - 3 - 12 * (1 - cliff), r);
  const lowland = (5 + 2.5 * fbm(x / 25, z / 25, 4, 3) + cliff * 7) * land;
  // The cone: ridges and gullies grow with r from the rim; three lava channels are cut deeper.
  const ridges = 1 + 0.13 * Math.cos(9 * theta + 1.5 * noise(x / 40, z / 40, 9)) * smooth(RC + 2, RC + 22, r);
  let dip = 0;
  for (const t of LAVA) { const q = (r * Math.atan2(Math.sin(theta - t), Math.cos(theta - t))) / 4; dip += 2.2 * Math.exp(-q * q); }
  dip *= smooth(RC + 3, RC + 10, r) * (1 - smooth(50, 66, r));
  const outer = coneAt(Math.max(r, RC)) * ridges - dip + RIM * Math.exp(-(((r - RC) / 2.5) * ((r - RC) / 2.5)));
  const bowl = FLOOR_Y + (coneAt(RC) - FLOOR_Y) * smooth(5, RC, r);
  const t = smooth(RC - 0.5, RC + 0.5, r);
  const cone = bowl + (outer - bowl) * t;
  let h = Math.max(lowland, cone) + 1.2 * fbm(x / 9, z / 9, 21, 2) * land;
  for (const c of CASTLES) {
    const s = Math.max(c.size[0], c.size[1]) / 2 + 3;
    h += (c.h - h) * smooth(s + 8, s, Math.hypot(x - c.at[0], z - c.at[1]));
  }
  return Math.max(0, h);
}
const gridOf = (min, max, base = 0) => {
  const rows = [];
  for (let z = min[1]; z <= max[1]; z++) {
    const row = [];
    for (let x = min[0]; x <= max[0]; x++) row.push(Math.round(Math.max(0, height(x, z) - base) * 100) / 100);
    rows.push(row);
  }
  return rows;
};

// Materials by height above the level's floor (first rule that holds wins); the rules of a field standing on SLAB or
// deeper are shifted by its floor.
const MATERIALS = [
  { material: MAT.lava, minHeight: FLOOR_Y - 2, maxHeight: FLOOR_Y + 1.5, maxSlope: 20 },   // the crater floor
  { material: MAT.darkRock, minSlope: 55, maxHeight: 14 },                  // sea cliffs
  { material: MAT.ash, minHeight: 46 },                                     // the summit and the crater wall: black
  { material: MAT.ash, minHeight: 22, minSlope: 52 },                       // black ash down the steepest gullies
  { material: MAT.rubber, minHeight: 32 },                                  // the upper cone, dark blue-grey
  { material: MAT.road, minHeight: 20 },                                    // the middle cone, slate
  { material: MAT.rock, minHeight: 14 },                                    // the scree at the foot
  { material: MAT.sand, maxHeight: 3 },                                     // beaches
];
const matsAt = (floor) => MATERIALS.map((m) => ({ ...m, ...(m.minHeight != null ? { minHeight: m.minHeight - floor } : {}), ...(m.maxHeight != null ? { maxHeight: m.maxHeight - floor } : {}) }));
const disc = (radius) => ({ x: 0, z: 0, radius });
// Layers, bottom up, so every voxel size is spent where it shows: 4-unit slabs under the cone (to SLAB1 within r R1, to
// SLAB2 within r R2), 3-unit voxels for the flanks between R2 and R1 above SLAB1 and for the lowlands, 2-unit voxels
// for the cone above SLAB2.
const box = (r) => ({ min: [-r - 6, -r - 6], max: [r + 6, r + 6] });
const FIELDS = {
  slab1: { ...box(R1), floor: 0, baseDepth: 0, voxelSize: 4, centre: [0, 0], clip: [disc(R1 + 3)], expr: String(SLAB1), material: MAT.rock, runsHeadroom: 0 },
  slab2: { ...box(R2), floor: SLAB1, baseDepth: 0, voxelSize: 4, centre: [0, 0], clip: [disc(R2 + 3)], expr: String(SLAB2 - SLAB1), material: MAT.rock, runsHeadroom: 0 },
  coarse: { min: [-ISLAND, -ISLAND], max: [ISLAND, ISLAND], floor: 0, baseDepth: 0, voxelSize: VC, maxCells: CELLS_COARSE,
            centre: [0, 0], exclude: [disc(R1)], material: MAT.grass, materials: matsAt(0), runsHeadroom: 300 },
  mid: { ...box(R1), floor: SLAB1, baseDepth: 0, voxelSize: VC, maxCells: CELLS_MID, centre: [0, 0], clip: [disc(R1)], exclude: [disc(R2)],
         material: MAT.grass, materials: matsAt(SLAB1), runsHeadroom: 300 },
  fine: { ...box(R2), floor: SLAB2, baseDepth: 0, voxelSize: VF, maxCells: CELLS_FINE, centre: [0, 0], clip: [disc(R2)],
          material: MAT.grass, materials: matsAt(SLAB2), runsHeadroom: 300 },
};
for (const f of Object.values(FIELDS)) if (!f.expr) f.grid = gridOf(f.min, f.max, f.floor);

// ---- the MCP client ----

let calls = 0;
const log = [];
async function startServer() {
  const node = path.join(ROOT, "tools/node/node.exe");
  const p = spawn(node, [path.join(ROOT, "tools/erg-mcp/dist/erg-mcp.mjs")], { stdio: ["pipe", "pipe", "inherit"] });
  let buf = "", id = 0;
  const pend = new Map();
  p.stdout.on("data", (d) => {
    buf += d;
    let i;
    while ((i = buf.indexOf("\n")) >= 0) {
      const l = buf.slice(0, i); buf = buf.slice(i + 1);
      if (!l.trim()) continue;
      const m = JSON.parse(l);
      if (m.id != null && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); }
    }
  });
  const rpc = (method, params) => new Promise((r) => { const i = ++id; pend.set(i, r); p.stdin.write(JSON.stringify({ jsonrpc: "2.0", id: i, method, params }) + "\n"); });
  await rpc("initialize", { protocolVersion: "2025-06-18", capabilities: {}, clientInfo: { name: "caldera-bench", version: "1" } });
  p.stdin.write(JSON.stringify({ jsonrpc: "2.0", method: "notifications/initialized" }) + "\n");
  const call = async (name, args = {}, { soft = false } = {}) => {
    calls++;
    const r = await rpc("tools/call", { name, arguments: args });
    const res = r.result || { isError: true, content: [{ type: "text", text: JSON.stringify(r.error) }] };
    const text = (res.content || []).filter((c) => c.type === "text").map((c) => c.text).join("\n");
    let json = null;
    try { json = JSON.parse(text); } catch { /* text answer */ }
    const out = { err: !!res.isError, text, json, images: (res.content || []).filter((c) => c.type === "image") };
    log.push({ name, args: name === "put_script" ? { text: `<${args.text.length} chars>` } : args, err: out.err, text: text.slice(0, 4000) });
    if (out.err && !soft) throw new Error(`${name}: ${text.slice(0, 1500)}`);
    return out;
  };
  const stop = () => new Promise((r) => { p.on("exit", r); p.stdin.end(); setTimeout(() => { p.kill(); r(); }, 5000); });
  return { call, stop };
}

const say = (...a) => console.log(...a);
const brief = (r, n = 300) => r.text.slice(0, n).replace(/\s+/g, " ");

// ---- the build ----

fs.mkdirSync(OUT, { recursive: true });
const { call, stop } = await startServer();
try {
  say("connect", brief(await call("connect", { gameDir: GAME }), 160));
  if ((await call("list_bases", { search: "dinermight" })).json.projects.some((p) => p.id === ID))
    say("delete the old project", brief(await call("delete_project", { id: ID }), 200));
  const np = await call("new_project", { base: "Multi.DinerMight", slug: ID, title: "Caldera" });
  if (np.json?.created?.id !== ID) throw new Error(`new_project gave id ${np.json?.created?.id}, not ${ID}`);
  await call("open_project", { id: ID });

  // Setting: the base's own material set (Building) at night; the base's terrain wiped (its frames stay, so its 11 652
  // cells stay spent); the heightmap hill and the diner's neon and grass tufts removed (they show through the water);
  // the sea starts below the beaches.
  await call("set_theme", { theme: "BUILDING" });
  await call("set_time_of_day", { timeOfDay: "NIGHT" });
  await call("set_water", { level: WATER_START });
  await call("set_surround_mode", { mode: "none" });
  for (const search of ["Neon", "Grass"]) say("remove", search, brief(await call("remove_scenery", { search }, { soft: true }), 120));
  say("clear", brief(await call("clear_base_terrain", { all: true, wipe: true }), 200));

  // Island and volcano: the two heightfield calls.
  for (const [name, f] of Object.entries(FIELDS)) {
    const plan = await call("heightfield_plan", f);
    const pj = plan.json;
    say("plan", name, "fits", pj.fits, "blocks", JSON.stringify(pj.blocks?.placed), "/", JSON.stringify(pj.blocks?.needed), "cells", JSON.stringify(pj.budget?.cells),
      "voxels", JSON.stringify(pj.voxels), "notes", JSON.stringify(pj.notes ?? []), pj.why ?? "");
    fs.writeFileSync(path.join(OUT, `plan-${name}.json`), plan.text);
    if (!pj.fits) throw new Error(`the ${name} field does not fit: ${pj.why ?? ""}`);
  }
  if (flag("--plan")) { await call("close", { discard: true }, { soft: true }); throw new Error("--plan: stopped"); }
  for (const [name, f] of Object.entries(FIELDS)) {
    const hf = await call("heightfield_region", f);
    say("island", name, JSON.stringify(hf.json.limits.voxelCells), "blocks", JSON.stringify(hf.json.limits.newFrames));
  }

  // Castles, in fine voxels (scale 1, new blocks, packed per wall) on their shelves, gates towards the summit. Each
  // stands on the shelf's voxel top rounded down to a whole unit.
  const spots = {};
  for (const c of CASTLES) {
    const [x, z] = c.at;
    const g = await call("query_ground", { x, z, units: "map" });
    const top = g.json?.standOn?.map;
    if (typeof top !== "number") throw new Error(`no ground at ${c.name}: ${g.text.slice(0, 300)}`);
    const y = Math.floor(top + 1e-6);
    c.floor = y;
    const gate = Math.abs(x) > Math.abs(z) ? (x > 0 ? "w" : "e") : (z > 0 ? "n" : "s");
    const big = !!c.keep;
    const r = await call("build_castle", {
      at: [x, y, z], size: c.size, wallHeight: big ? 5 : 4, wallThickness: 2, towerHeight: big ? 8 : 7, towerRadius: big ? 2.5 : 2,
      towerShape: c.towerShape ?? "round", keep: big, ...(big ? { keepSize: [6, 6], keepHeight: 9 } : {}),
      gate, gateWidth: 3, gateHeight: big ? 4 : 3, crenellations: true, material: c.mat, place: "new", scale: CASTLE_SCALE,
    });
    spots[c.name] = r.json?.spawnSpots;
    say("castle", c.name, "shelf", top, "floor", y, "gate", gate, "cells", JSON.stringify(r.json?.usage?.voxelCells ?? r.text.slice(0, 200)), "spots", JSON.stringify(r.json?.spawnSpots));
  }
  fs.writeFileSync(path.join(OUT, "spots.json"), JSON.stringify(spots, null, 1));

  // Three lava tubes through the rim, each opening on the line from the summit through a castle (Ember, Cinder, Ashgate):
  // square bores 6 wide, their floor rising from the flank's ground at the mouth to the crater floor, so a worm can
  // walk from the castle's gate to the crater without climbing the steepest 15 units of the cone.
  const crater = (await call("query_ground", { x: 0, z: 0, units: "map" })).json.standOn.map;
  for (const name of ["Ember", "Cinder", "Ashgate"]) {
    const c = CASTLES.find((k) => k.name === name), a = Math.atan2(c.at[1], c.at[0]);
    const at = (r) => [Math.round(r * Math.cos(a) * 10) / 10, Math.round(r * Math.sin(a) * 10) / 10];
    const [mx, mz] = at(TUBE_MOUTH), [ix, iz] = at(5);
    const mouth = (await call("query_ground", { x: mx, z: mz, units: "map" })).json.standOn.map;
    const t = await call("dig_tunnel", { points: [[mx, mouth + 2.5, mz], [ix, crater + 3, iz]], radius: 3, shape: "square" });
    say("tube", name, "mouth", mx, mz, mouth, "crater", crater, "removed", t.json?.voxels?.removed);
  }

  // Lava: red channels painted down the three gullies, from the rim to r 70. Each point is painted in the frame that
  // holds it (a stroke touches only its anchor's frame), so the path is cut where the frame changes.
  for (const t of LAVA) {
    let run = [], frame = null;
    const flush = async () => {
      if (run.length) await call("sculpt_path", { mode: "paint", shape: "sphere", size: [2, 2, 2], material: MAT.lava, units: "map", frame, points: run, snap: false }, { soft: true });
      run = [];
    };
    for (let r = RC - 1; r <= 62; r += 2) {
      const x = Math.round(r * Math.cos(t) * 10) / 10, z = Math.round(r * Math.sin(t) * 10) / 10;
      const q = (await call("query_ground", { x, z, units: "map" }, { soft: true })).json;
      if (!q?.standOn) continue;
      const f = q.ground?.frame;
      if (f !== frame) { await flush(); frame = f; }
      run.push([x, q.standOn.map - 0.5, z]);
    }
    await flush();
    say("lava", t);
  }

  // The mine factory in the crater bowl, the spawns in the castles' courtyards (the castle tool's own spawn spots).
  say("factory", brief(await call("place_object", { type: "minefactory", units: "map", xz: [0, 0] }), 300));
  const spawns = SPAWNS.map(([worm, ci]) => {
    const c = CASTLES[ci];
    const yard = (spots[c.name]?.spots ?? []).filter((s) => s.kind === "courtyard" && s.ok);
    const first = SPAWNS.find((s) => s[1] === ci)[0] === worm;
    const spot = yard[c.keep && !first ? 1 : 0];
    if (!spot) throw new Error(`no courtyard spot at ${c.name}`);
    return { worm, pos: spot.at };
  });
  const sp = await call("place_spawn_knots", { mode: "knots", units: "map", spawns });
  say("spawns", brief(sp, 900));

  // The rising sea.
  const script = fs.readFileSync(path.join(HERE, "script.lua"), "utf8");
  const ps = await call("put_script", { text: script });
  say("script", brief(ps, 300));

  const sv = await call("save");
  say("save", brief(sv, 600));
  const d = await call("describe_level", { limit: 100 });
  fs.writeFileSync(path.join(OUT, "describe.json"), d.text);
  say("cells", JSON.stringify(d.json.limits.voxelCells), "blocks", JSON.stringify(d.json.limits.newFrames), "engineWarnings", JSON.stringify(d.json.engineWarnings), "problems", JSON.stringify(d.json.problems ?? []));
  const lo = await call("list_objects", {});
  fs.writeFileSync(path.join(OUT, "objects.json"), lo.text);
  const rc = await call("check_reachability", {}, { soft: true });
  fs.writeFileSync(path.join(OUT, "reach.json"), rc.text);
  say("reach", brief(rc, 1500));

  // Renders.
  const shots = [
    ["topdown", "render_topdown", { step: 1, scale: 3 }],
    ["side", "render_side", { x0: -ISLAND, z0: 0, x1: ISLAND, z1: 0, scale: 4 }],
    ["side-ns", "render_side", { x0: 0, z0: -ISLAND, x1: 0, z1: ISLAND, scale: 4 }],
  ];
  for (const [name, tool, args] of shots) {
    const r = await call(tool, args, { soft: true });
    if (r.images[0]) fs.writeFileSync(path.join(OUT, `${name}.png`), Buffer.from(r.images[0].data, "base64"));
    say(name, r.err ? r.text.slice(0, 300) : "ok");
  }
  const ascii = await call("heightmap_ascii", { step: 3 });
  fs.writeFileSync(path.join(OUT, "heightmap.txt"), ascii.text);

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
  say("MCP tool calls:", calls);
  await stop();
}
