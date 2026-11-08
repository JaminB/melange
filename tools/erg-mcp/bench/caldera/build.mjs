// Caldera: an Erg MCP benchmark map, built only through the erg-mcp server's tools (stdio JSON-RPC, no hand-written
// patches). A volcanic island on Multi.DinerMight at night: a round, irregular cone with a wide crater (a breach on its
// south side, jagged rim spurs, a lava lake round the mine factory), gullies with lava streaks and lava fans, three
// switchback roads cut into the flanks that run from the shore through the castles to the rim, six castles of six
// different plans on the roads and the headland, and a level script (script.lua) that raises the sea every round until
// only the upper cone is dry.
//
//   tools\node\node.exe tools\erg-mcp\bench\caldera\build.mjs [--out <dir>] [--plan] [--no-export] [--keep-mod] [--no-scenery]
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
// Budget: Diner Might's frames hold 11 652 of the game's 65 535 cells, leaving 53 883. Five heightfield calls share the
// island (slabs under the cone, 3-unit voxels for the skirt and shore, 2-unit voxels for the cone), the castles take
// the rest in blocks of 1.5-unit voxels packed per part.

const VC = +(process.env.VC || 3.5);   // skirt voxels, map units
const VF = +(process.env.VF || 2);   // cone voxels
const SLAB1 = +(process.env.S1 || 12), R1 = +(process.env.R1 || 58);         // slab under the cone's foot (height, radius)
const SLAB2 = +(process.env.S2 || 32), R2 = +(process.env.R2 || 34);         // slab under the cone's upper flanks
const ISLAND = 108;                // half side of the coarse heightfield rectangle
const CELLS_COARSE = +(process.env.CC || 14000);
const CELLS_MID = +(process.env.CM || 11000);
const CELLS_FINE = +(process.env.CF || 11000);
const WATER_START = -20;           // world units: the beaches are dry at the start
const CASTLE_SCALE = +(process.env.CS || 2);     // voxels of the castle blocks (one block per castle, built into)

// The cone: h = H (1 - rr/R)^P with rr the radius shaken by angular noise (+-8 %, so the outline is irregular, not a
// polygon of voxel rings); a crater of radius RC with a sloped bowl down to a flat floor, a breach (the rim lowered to a
// notch) facing south, and rim spurs of varied height.
const H = 84, R = 100, P = 1.5, RC = 26, FLOOR_Y = 40, RIM = 2;
const coneAt = (r) => H * Math.pow(Math.max(0, 1 - r / R), P);
const gradAt = (r) => (H * P / R) * Math.pow(Math.max(0, 1 - r / R), P - 1);
const rOfH = (h) => R * (1 - Math.pow(h / H, 1 / P));
const BREACH = 0;                   // +x: east, clear of the castles and roads (which run north, south and west)

// Lava gullies: angles (atan2(z, x)) of four channels from the rim down to r 70: three on the flanks and the breach.
const LAVA = [BREACH, 2.35, 3.9, 5.2];
const FANS = [[2.35, 66], [3.9, 64], [5.2, 68]];   // lava fans pooling at the foot: angle, radius

// Materials of Diner Might's file (Building theme): grass01 0, rock01 4, rock02 6, sand01 10, stone 12, Leaves 13
// (orange-brown), Road 14 (slate), JCB Yellow Metal 17 (the hot core of the lava), Red Metal 21, Tyre 24 (near black, ash),
// Rubber matting 36, Brick colour 37, concrete and bricks 55.
const MAT = { rubber: 36, grass: 0, rock: 4, darkRock: 6, sand: 10, road: 14, lava: 21, hot: 17, ember: 13, ash: 24, path: 9 };

// ---- the height of the island at (x, z), map units above the floor ----
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
const angd = (a, b) => Math.atan2(Math.sin(a - b), Math.cos(a - b));
// The cone's radius, shaken by angular noise: the same function for the cone, the bowl and the roads.
const shake = (x, z) => {
  const th = Math.atan2(z, x);
  return Math.hypot(x, z) * (1 - 0.17 * fbm(Math.cos(th) * 1.7 + 9, Math.sin(th) * 1.7 + 9, 5, 3));
};
const surf0 = (x, z) => coneAt(Math.max(shake(x, z), RC));

// ---- roads: switchbacks cut into the flanks, marched on the cone's surface at a fixed climb ----
// A road spirals round the cone climbing SP per unit of length (the radius closes by SP / gradient per unit, the rest is
// tangential), runs level for `flat` units through a castle, and ends on the rim. Each is built backwards from its
// castle down to `startH`, then forwards to the rim; the terrain is cut and filled to the road's own height across a
// 5-wide bed with a short blend (the "cut-and-fill" of a mountain road).
const SP = 0.55;                   // 29 degrees
const ROADS = [
  { name: "R1", dir: 1, theta: Math.PI / 2, h: 30, flat: 30, startH: 6 },        // Seawall ... Cinder (h 30)
  { name: "R2", dir: -1, theta: -Math.PI / 2, h: 36, flat: 30, startH: 10 },     // Harbour ... Ember (h 36)
  { name: "R3", dir: 1, theta: Math.PI, h: 22, flat: 34, startH: 8 },            // lowland ... Ashgate (h 22)
];
function marchRoad(spec) {
  const rc = rOfH(spec.h), at = (r, th) => [r * Math.cos(th), r * Math.sin(th)];
  const back = [];
  let r = rc, th = spec.theta - spec.dir * (spec.flat / 2) / rc;
  while (coneAt(r) > spec.startH && r < 96) {
    back.push([r, th]);
    const dr = Math.min(0.95, SP / Math.max(gradAt(r), 1e-3));
    r += dr; th -= spec.dir * Math.sqrt(1 - dr * dr) / r;
  }
  back.reverse();
  const pts = back.map(([rr, tt]) => at(rr, tt));
  const flatFrom = pts.length;
  for (let i = 0; i <= spec.flat; i++) pts.push(at(rc, spec.theta - spec.dir * (spec.flat / 2) / rc + spec.dir * i / rc));
  const flatTo = pts.length - 1;
  r = rc; th = spec.theta + spec.dir * (spec.flat / 2) / rc;
  while (r > RC + 3) {
    const dr = Math.min(0.95, SP / Math.max(gradAt(r), 1e-3));
    r -= dr; th += spec.dir * Math.sqrt(1 - dr * dr) / r;
    pts.push(at(r, th));
  }
  // The bed's height: the surface under the line, but never climbing more than SP a unit (and never falling).
  const ys = [];
  let y = surf0(pts[0][0], pts[0][1]);
  for (let i = 0; i < pts.length; i++) {
    if (i >= flatFrom && i <= flatTo) { ys.push(y); continue; }
    const t = surf0(pts[i][0], pts[i][1]);
    y = Math.min(y + SP * 0.98, Math.max(y, t));
    ys.push(y);
  }
  const m = Math.round((flatFrom + flatTo) / 2);
  return { ...spec, pts: pts.map((p, i) => [p[0], p[1], ys[i]]), flatFrom, flatTo, mid: pts[m], midY: ys[flatFrom], end: pts[pts.length - 1] };
}
const ROAD = Object.fromEntries(ROADS.map((s) => [s.name, marchRoad(s)]));
const ROAD_END_ANGLES = Object.values(ROAD).map((rd) => Math.atan2(rd.end[1], rd.end[0]));

// ---- castles ----
// Plan, centre and floor from the roads (the through-road castles stand on a level stretch of their road). `axis` is the
// road's direction at the castle (along x or z).
const tangentAt = (rd, i) => { const a = rd.pts[Math.max(0, i - 2)], b = rd.pts[Math.min(rd.pts.length - 1, i + 2)]; return Math.atan2(b[1] - a[1], b[0] - a[0]); };
const round1 = (v) => Math.round(v * 10) / 10;
const CASTLES = [
  { name: "Seawall", plan: "square", at: ROAD.R1.pts[0], h: ROAD.R1.pts[0][2], size: [28, 22], mat: 12, road: "R1" },
  { name: "Harbour", plan: "round", at: ROAD.R2.pts[0], h: ROAD.R2.pts[0][2], rad: 13, mat: 37, road: "R2" },
  { name: "Headland", plan: "cliff", at: [6, 93], h: 16, mat: 55 },
  { name: "Ashgate", plan: "ell", at: ROAD.R3.mid, h: ROAD.R3.midY, mat: 36, road: "R3", axis: "z" },
  { name: "Cinder", plan: "gatehouse", at: ROAD.R1.mid, h: ROAD.R1.midY, mat: 37, road: "R1", axis: "x" },
  { name: "Ember", plan: "ring", at: ROAD.R2.mid, h: ROAD.R2.midY, mat: 14, road: "R2", axis: "x" },
];
// The castle block of each (map units: x, z, height) and where its middle is.
const PADS = { square: [36, 30, 14], round: [32, 32, 12], cliff: [44, 36, 16], ell: [32, 32, 10], gatehouse: [38, 32, 14], ring: [32, 32, 16] };
for (const c of CASTLES) c.pad = PADS[c.plan];
CASTLES.find((c) => c.plan === "ell").padAt = [CASTLES.find((c) => c.plan === "ell").at[0] + 4, CASTLES.find((c) => c.plan === "ell").at[1]];   // the L reaches 8 further to +x
CASTLES.find((c) => c.plan === "cliff").padAt = [6, 96];
// Half sizes (x, z) of each castle's footprint, for the shelf under it.
const footprint = (c) => ({ square: [19, 16], round: [17, 17], cliff: [23, 20], ell: [20, 18], gatehouse: [20, 17], ring: [17, 17] }[c.plan]);

const HEADS = [[6, 100, 15, 14], [-70, -62, 13, 12]];     // headlands: centre, spread, how far they push the coast out
const COVE = [88, -24, 12, 26];                          // a cove on the east shore
function island(x, z) {
  const r = Math.hypot(x, z), theta = Math.atan2(z, x), rr = shake(x, z);
  let coast = 88 + 8 * fbm(x / 45, z / 45, 11, 3) - COVE[3] * gauss(x, z, COVE[0], COVE[1], COVE[2]);
  let cliff = 0;
  for (const [cx, cz, s, a] of HEADS) { const w = gauss(x, z, cx, cz, s); coast += a * w; cliff = Math.max(cliff, w); }
  const land = smooth(coast + 2, coast - 3 - 12 * (1 - cliff), r);
  const lowland = (5 + 3 * fbm(x / 25, z / 25, 4, 3) + cliff * 9) * land;
  // The cone: ridges and gullies grow with r from the rim; four lava channels are cut deeper and wider at the rim.
  const ridges = 1 + 0.13 * Math.cos(9 * theta + 1.5 * noise(x / 40, z / 40, 9)) * smooth(RC + 2, RC + 22, rr);
  let dip = 0;
  for (const t of LAVA) {
    const wd = 5.2 - 3.4 * Math.min(1, Math.max(0, (rr - RC) / 45));
    const q = (rr * angd(theta, t)) / wd;
    dip += 2.6 * Math.exp(-q * q);
  }
  dip *= smooth(RC + 1, RC + 6, rr) * (1 - smooth(52, 68, rr));
  // The rim: spurs of varied height (no regular teeth), none where a road reaches it.
  let roadMask = 0;
  for (const a of ROAD_END_ANGLES) roadMask = Math.max(roadMask, Math.exp(-((angd(theta, a) / 0.22) ** 2)));
  const spur = Math.max(0, noise(Math.cos(theta) * 3.3 + 4, Math.sin(theta) * 3.3 + 4, 17) + 0.12) * 7 * (1 - roadMask);
  const crest = (RIM + spur) * Math.exp(-(((rr - RC) / 3.4) ** 2));
  const outer = coneAt(Math.max(rr, RC)) * ridges - dip + crest;
  const crestH = coneAt(RC) + RIM;
  const bowl = FLOOR_Y + (crestH - FLOOR_Y) * smooth(7, RC, rr);
  const cone = bowl + (outer - bowl) * smooth(RC - 3, RC + 1, rr);
  // The breach: the rim falls away in a notch facing south, so the bowl and its mines are seen from the default camera.
  const bw = Math.exp(-((angd(theta, BREACH) / 0.3) ** 2));
  const ramp = FLOOR_Y + 3 + Math.max(0, rr - (RC - 9)) * 0.55;
  const c = cone - bw * smooth(RC + 18, RC + 8, rr) * smooth(5, 12, rr) * Math.max(0, cone - ramp);
  let h = Math.max(lowland, c) + 1.3 * fbm(x / 9, z / 9, 21, 2) * land * smooth(40, 70, r);
  // Shelves under the castles, the roads cut and filled into the flank.
  for (const k of CASTLES) {
    const [fx, fz] = footprint(k);
    const d = Math.hypot(Math.max(0, Math.abs(x - k.at[0]) - fx), Math.max(0, Math.abs(z - k.at[1]) - fz));
    h += (k.h - h) * smooth(11, 0, d);
  }
  for (const rd of Object.values(ROAD)) {
    let best = 1e9, by = 0;
    for (const p of rd.pts) {
      const dx = x - p[0], dz = z - p[1];
      if (Math.abs(dx) > 7 || Math.abs(dz) > 7) continue;
      const d = Math.hypot(dx, dz);
      if (d < best) { best = d; by = p[2]; }
    }
    if (best < 7) h += (by - h) * (1 - smooth(2.7, 6.5, best));
  }
  return Math.max(0, h);
}
const gridOf = (min, max, base = 0) => {
  const rows = [];
  for (let z = min[1]; z <= max[1]; z++) {
    const row = [];
    for (let x = min[0]; x <= max[0]; x++) row.push(Math.round(Math.max(0, island(x, z) - base) * 100) / 100);
    rows.push(row);
  }
  return rows;
};

// Materials by height above the level's floor (first rule that holds wins); the rules of a field standing on SLAB or
// deeper are shifted by its floor. The lava skin is a one-voxel layer on the bowl floor only (the fine field, under 2500
// columns; a wider skin broke the game's mesh combiner).
const MATERIALS = [
  { material: MAT.lava, surface: 1, minHeight: FLOOR_Y - 2, maxHeight: FLOOR_Y + 3, maxSlope: 30 },   // the bowl's floor skin
  { material: MAT.ash, minHeight: FLOOR_Y - 2, maxHeight: FLOOR_Y + 3, maxSlope: 30 },              // under it: black ash
  { material: MAT.darkRock, minSlope: 55, maxHeight: 14 },                  // sea cliffs
  { material: MAT.ash, minHeight: 46 },                                     // the summit and the crater wall: black
  { material: MAT.ash, minHeight: 22, minSlope: 50 },                       // black ash down the steepest gullies
  { material: MAT.rubber, minHeight: 32 },                                  // the upper cone, dark blue-grey
  { material: MAT.road, minHeight: 20 },                                    // the middle cone, slate
  { material: MAT.rock, minSlope: 24, maxHeight: 22 },                      // scree where it is steep
  { material: MAT.rock, minHeight: 14 },                                    // the scree at the foot
  { material: MAT.sand, maxHeight: 3 },                                     // beaches
];
const matsAt = (floor) => MATERIALS.map((m) => ({ ...m, ...(m.minHeight != null ? { minHeight: m.minHeight - floor } : {}), ...(m.maxHeight != null ? { maxHeight: m.maxHeight - floor } : {}) }));
const disc = (radius) => ({ x: 0, z: 0, radius });
const box = (r) => ({ min: [-r - 6, -r - 6], max: [r + 6, r + 6] });
const FIELDS = {
  slab1: { ...box(R1), floor: 0, baseDepth: 0, voxelSize: 4, centre: [0, 0], clip: [disc(R1 + 3)], expr: String(SLAB1), material: MAT.rock, runsHeadroom: 0 },
  slab2: { ...box(R2), floor: SLAB1, baseDepth: 0, voxelSize: 4, centre: [0, 0], clip: [disc(R2 + 3)], expr: String(SLAB2 - SLAB1), material: MAT.rock, runsHeadroom: 0 },
  coarse: { min: [-ISLAND, -ISLAND], max: [ISLAND, ISLAND], floor: 0, baseDepth: 0, voxelSize: VC, maxCells: CELLS_COARSE, edges: "noise", packer: "best",
            centre: [0, 0], exclude: [disc(R1)], material: MAT.grass, materials: matsAt(0), runsHeadroom: 300 },
  mid: { ...box(R1), floor: SLAB1, baseDepth: 0, voxelSize: VC, maxCells: CELLS_MID, edges: "noise", packer: "best", centre: [0, 0], clip: [disc(R1)], exclude: [disc(R2)],
         material: MAT.grass, materials: matsAt(SLAB1), runsHeadroom: 300 },
  fine: { ...box(R2), floor: SLAB2, baseDepth: 0, voxelSize: VF, maxCells: CELLS_FINE, edges: "noise", packer: "best", centre: [0, 0], clip: [disc(R2)],
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
    log.push({ name, args: name === "put_script" ? { text: `<${args.text.length} chars>` } : (args.grid ? { ...args, grid: "<grid>" } : args), err: out.err, text: text.slice(0, 2500) });
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

  // Setting: the base's own material set (Building) at night (in the game, with the post-FX off, dusk made the sea the
  // colour of sand and night a clear teal against the dark cone); the base's terrain wiped (its frames stay, so its 11 652 cells stay spent); the heightmap hill and the diner's neon and grass tufts
  // removed (they show through the water); the sea starts below the beaches.
  await call("set_theme", { theme: "BUILDING" });
  await call("set_time_of_day", { timeOfDay: "NIGHT" });
  await call("set_water", { level: WATER_START });
  await call("set_surround_mode", { mode: "none" });
  for (const search of ["Neon", "Grass"]) say("remove", search, brief(await call("remove_scenery", { search }, { soft: true }), 120));
  say("clear", brief(await call("clear_base_terrain", { all: true, wipe: true }), 200));
  const d0 = await call("describe_level", { limit: 1 });
  say("cells before", JSON.stringify(d0.json.limits.voxelCells));

  // Island and volcano: the heightfield calls.
  for (const [name, f] of Object.entries(FIELDS)) {
    const plan = await call("heightfield_plan", f);
    const pj = plan.json;
    say("plan", name, "fits", pj.fits, "blocks", JSON.stringify(pj.blocks?.placed), "/", JSON.stringify(pj.blocks?.needed), "cells", JSON.stringify(pj.budget?.cells),
      "voxels", JSON.stringify(pj.voxels), "notes", JSON.stringify(pj.notes ?? []), pj.why ?? "");
    fs.writeFileSync(path.join(OUT, `plan-${name}.json`), plan.text);
    if (!pj.fits && !flag("--plan")) throw new Error(`the ${name} field does not fit: ${pj.why ?? ""}`);
  }
  if (flag("--plan")) { await call("close", { discard: true }, { soft: true }); throw new Error("--plan: stopped"); }
  for (const [name, f] of Object.entries(FIELDS)) {
    const hf = await call("heightfield_region", f);
    say("island", name, JSON.stringify(hf.json.limits.voxelCells), "blocks", JSON.stringify(hf.json.limits.newFrames));
  }
  const groundOf = async (x, z) => {
    const g = await call("query_ground", { x, z, units: "map" });
    const top = g.json?.standOn?.map;
    if (typeof top !== "number") throw new Error(`no ground at ${x}, ${z}: ${g.text.slice(0, 300)}`);
    return top;
  };

  // ---- the castles, each a different plan, in blocks of CASTLE_SCALE voxels packed per part ----
  const spawnAt = {};                        // castle name -> list of [x, z] worm positions
  // local (u along the road, v across it) -> map x, z
  const loc = (c, u, v) => (c.axis === "z" ? [c.at[0] + v, c.at[1] + u] : [c.at[0] + u, c.at[1] + v]);
  const tidy = (p) => p.map(round1);
  for (const c of CASTLES) {
    const top = await groundOf(c.at[0], c.at[1]);
    const y = Math.floor(top + 1e-6);
    c.floor = y;
    const [cx, cz] = c.at;
    // One block per castle, hollowed out: every wall, tower and keep is then built into it (a block per call would pass
    // the 64-block limit). It is the castle's whole volume, so its cells are (size / scale)^3 of the game's budget.
    const s = c.scale ?? CASTLE_SCALE, [pw, pd, ph] = c.pad;
    const pc = c.padAt ?? c.at;
    const ab = await call("add_block", { size: [Math.ceil(pw / s), Math.ceil(ph / s), Math.ceil(pd / s)], scale: s, material: c.mat, at: [round1(pc[0]), y, round1(pc[1])], units: "map" });
    const frame = ab.json?.block?.id ?? ab.json?.frame?.id ?? ab.json?.id ?? ab.json?.frames?.find?.((f) => f.new)?.id;
    if (typeof frame !== "number") throw new Error("add_block answered no frame id: " + ab.text.slice(0, 300));
    await call("fill_box_raw", { frame, state: "empty" });
    const B = { place: "existing", blockScale: s, packing: "parts" };
    const W = (pts, height = 6, thickness = 2) => call("build_wall", { points: pts.map(([x, z]) => [round1(x), y, round1(z)]), material: c.mat, height, thickness, crenellations: true, ...B });
    const T = (x, z, r, hgt, door = "none") => call("build_tower", { at: [round1(x), y, round1(z)], material: c.mat, radius: r, height: hgt, hollow: true, wallThickness: 1.5, door, crenellations: true, ...B });
    const K = (x, z, size, hgt, door = "s") => call("build_keep", { at: [round1(x), y, round1(z)], size, height: hgt, door, doorWidth: 2, doorHeight: 3, windows: true, crenellations: true, material: c.mat, ...B });
    const A = (x, z, rot) => call("build_arch", { at: [round1(x), y, round1(z)], span: 5, rise: 2, pier: 3, depth: 3, crown: 2, rotation: rot, material: c.mat, ...B });
    let used = "";
    if (c.plan === "square") {
      // Seawall: a square castle with a keep, its gate towards the road.
      const a = tangentAt(ROAD.R1, 0);
      const gate = Math.abs(Math.cos(a)) > Math.abs(Math.sin(a)) ? (Math.cos(a) > 0 ? "e" : "w") : (Math.sin(a) > 0 ? "s" : "n");
      const r = await call("build_castle", { at: [cx, y, cz], size: c.size, wallHeight: 6, wallThickness: 2, towerShape: "round", towerRadius: 3.2, towerHeight: 10, keep: true, keepSize: [6, 6], keepHeight: 11,
        gate, gateWidth: 4, gateHeight: 4, crenellations: true, material: c.mat, place: "existing", scale: s });
      used = JSON.stringify(r.json?.usage?.voxelCells);
      spawnAt[c.name] = [[cx - 7.5, cz], [cx + 7.5, cz]];
    } else if (c.plan === "round") {
      // Harbour: an octagonal curtain wall (16 sides, a gap for the gate towards the road), towers, a keep.
      const ga = tangentAt(ROAD.R2, 0), ring = [];
      for (let k = 0; k <= 16; k++) {
        const a = ga + 0.45 + (k / 16) * (2 * Math.PI - 0.9);
        ring.push([cx + c.rad * Math.cos(a), cz + c.rad * Math.sin(a)]);
      }
      await W(ring);
      for (const k of [0, 4, 8, 12, 16]) await T(ring[k][0], ring[k][1], 3.2, 10);
      await K(cx, cz, [6, 6], 10, "s");
      spawnAt[c.name] = [[cx + 7.5 * Math.cos(ga + Math.PI / 2), cz + 7.5 * Math.sin(ga + Math.PI / 2)]];
    } else if (c.plan === "cliff") {
      // Headland: a curtain wall along the cliff edge (the sea side), side walls back to a gatehouse arch, towers, a keep.
      const arc = [];
      for (let k = 0; k <= 14; k++) { const a = Math.PI * (0.12 + 0.76 * k / 14); arc.push([cx + 19 * Math.cos(a), cz + 14 * Math.sin(a) + 2]); }
      await W([[cx - 17, cz - 12], ...arc, [cx + 17, cz - 12]], 7);
      for (const k of [0, 7, 14]) await T(arc[k][0], arc[k][1], 3.5, 11 + (k === 7 ? 1 : 0));
      await T(cx - 17, cz - 12, 3, 10); await T(cx + 17, cz - 12, 3, 10);
      await A(cx, cz - 12, 0);
      await K(cx, cz + 3, [8, 6], 12, "n");
      spawnAt[c.name] = [[cx - 7, cz - 4]];
    } else if (c.plan === "ell") {
      // Ashgate: an L-shaped walled yard astride the road (open at both ends along it), towers at the corners.
      const pl = [[[-14, -3], [-14, -10], [14, -10], [14, -3]], [[14, 3], [2, 3], [2, 18], [-14, 18], [-14, 3]]];
      for (const poly of pl) await W(poly.map(([u, v]) => loc(c, u, v)));
      for (const [u, v] of [[-14, -10], [14, -10], [2, 18], [-14, 18], [14, 3], [-14, 3]]) { const [x, z] = loc(c, u, v); await T(x, z, 2.8, 9); }
      spawnAt[c.name] = [loc(c, -6, 9), loc(c, 6, -6)];
    } else if (c.plan === "gatehouse") {
      // Cinder: a gatehouse yard on the road: side walls, an arch at each end, corner towers, a keep off the road.
      for (const sgn of [-1, 1]) await W([loc(c, -14, sgn * 11), loc(c, 14, sgn * 11)]);
      for (const sgn of [-1, 1]) {
        const [x, z] = loc(c, sgn * 14, 0);
        await A(x, z, c.axis === "x" ? 90 : 0);
        for (const sv of [-1, 1]) { const [tx, tz] = loc(c, sgn * 14, sv * 11); await T(tx, tz, 3.2, 10); }
      }
      const [kx, kz] = loc(c, -6, -7);
      await K(kx, kz, [7, 6], 11, "s");
      spawnAt[c.name] = [loc(c, 6, 6)];
    } else if (c.plan === "ring") {
      // Ember: a ring wall open to the road at both ends, a round donjon beside the road.
      const rad = 12, ring = [], ring2 = [];
      for (let k = 0; k <= 12; k++) {
        const a = 0.55 + (k / 12) * (Math.PI - 1.1);
        ring.push(loc(c, rad * Math.cos(a), rad * Math.sin(a)));
        ring2.push(loc(c, rad * Math.cos(-a), rad * Math.sin(-a)));
      }
      await W(ring); await W(ring2);
      const [dx, dz] = loc(c, 0, 7.5);
      await T(dx, dz, 3.5, 15, "s");
      for (const k of [0, 12]) { await T(ring[k][0], ring[k][1], 2.6, 9); await T(ring2[k][0], ring2[k][1], 2.6, 9); }
      spawnAt[c.name] = [loc(c, -5, -1.5)];
    }
    say("castle", c.name, c.plan, "shelf", round1(top), "floor", y, "at", JSON.stringify(tidy(c.at)), used);
  }
  const dc = await call("describe_level", { limit: 1 });
  say("cells after castles", JSON.stringify(dc.json.limits.voxelCells), "blocks", JSON.stringify(dc.json.limits.newFrames));

  // ---- lava: red channels with a yellow core down the four gullies (wide at the rim, tapering), fans at the foot ----
  // A dab paints the frame that holds its centre (a stroke would paint only its first frame), so each is one call.
  const paint = (x, z, size, material, dy = -0.5) =>
    call("sculpt", { mode: "paint", shape: "sphere", size: [Math.max(1, Math.round(size)), Math.max(1, Math.round(size)), Math.max(1, Math.round(size))], center: [round1(x), round1(island(x, z) + dy), round1(z)], material, units: "map", snap: false }, { soft: true });
  for (const t of LAVA) {
    let n = 0;
    for (let r = RC - 2; r <= 70; r += 2.4) {
      const w = Math.max(2, 6.5 - 4.5 * Math.min(1, Math.max(0, (r - RC) / 44)));
      const x = r * Math.cos(t), z = r * Math.sin(t);
      await paint(x, z, w, MAT.lava);
      if (r < 54) await paint(x, z, Math.max(1.5, w * 0.45), MAT.hot);
      n++;
    }
    say("lava", t.toFixed(2), n, "dabs");
  }
  for (const [t, r] of FANS) {
    const x = r * Math.cos(t), z = r * Math.sin(t);
    await paint(x, z, 8, MAT.ember); await paint(x, z, 5, MAT.lava);
    await paint(x + 4, z + 3, 4, MAT.ember); await paint(x - 4, z - 3, 4, MAT.ember);
  }
  // The lava lake round the factory: a yellow ring, 2.5 wide, on the bowl's floor.
  for (let k = 0; k < 14; k++) {
    const a = (k / 14) * 2 * Math.PI;
    await paint(7 * Math.cos(a), 7 * Math.sin(a), 3.2, MAT.hot, -0.3);
    await paint(9.5 * Math.cos(a + 0.2), 9.5 * Math.sin(a + 0.2), 3, MAT.lava, -0.3);
  }

  // The roads: a pale path along each bed, so the switchbacks read from the overview camera.
  for (const rd of Object.values(ROAD)) {
    for (let i = 0; i < rd.pts.length; i += 2) {
      const [x, z, y] = rd.pts[i];
      await call("sculpt", { mode: "paint", shape: "box", size: [4, 2, 4], center: [round1(x), round1(y - 0.5), round1(z)], material: MAT.path, units: "map", snap: false }, { soft: true });
    }
  }

  // Utility crates on the cone: girders and ropes (and a jetpack) along the roads.
  const crates = [];
  for (const [rn, fr, contents] of [["R1", 0.3, "kUtilityGirder"], ["R1", 0.8, "kUtilityNinjaRope"], ["R2", 0.35, "kUtilityNinjaRope"], ["R2", 0.85, "kUtilityGirder"], ["R3", 0.4, "kUtilityJetpack"], ["R3", 0.85, "kUtilityGirder"]]) {
    const p = ROAD[rn].pts[Math.floor(ROAD[rn].pts.length * fr)];
    const r = await call("place_object", { type: "crate", kind: "utility", contents, units: "map", xz: [round1(p[0] + 3), round1(p[1] + 3)] }, { soft: true });
    crates.push(brief(r, 200));
  }
  say("crates", crates.join(" | "));

  // The mine factory in the crater (its settings come from the level script), the spawns in the castles' courtyards.
  say("factory", brief(await call("place_object", { type: "minefactory", units: "map", xz: [0, 0] }), 300));
  // Worms 0-3 are the first team (the human one in a Quick Game), 4-7 the second: each team has a high castle and a low one.
  const SPAWN_ORDER = [["Ashgate", 0], ["Cinder", 0], ["Harbour", 0], ["Ember", 0], ["Ashgate", 1], ["Seawall", 0], ["Seawall", 1], ["Headland", 0]];
  const spawns = SPAWN_ORDER.map(([name, i], worm) => ({ worm, xz: tidy(spawnAt[name][i]), drop: true }));
  const sp = await call("place_spawn_knots", { mode: "knots", units: "map", spawns });
  say("spawns", brief(sp, 900));

  // Camelot decoration beside the castles (no collision: Nice To Siege's flags and banners).
  if (!flag("--no-scenery")) {
    const copies = [];
    for (const c of CASTLES) {
      const [x, z] = c.at;
      copies.push({ resource: "CAMELOT19", xz: [round1(x + 17), round1(z + 2)], rot: [0, 0, 0], scale: 2.4 });
      copies.push({ resource: "CAMELOT16", xz: [round1(x - 17), round1(z - 2)], rot: [0, 0, 0], scale: 3 });
    }
    const sc = await call("copy_base_scenery", { base: "Multi.NiceToSiege", copies, allowOtherTheme: true, units: "map" }, { soft: true });
    say("scenery", brief(sc, 300));
  }

  // The rising sea, the mine factory's settings, no random mines.
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
  const end = ROAD.R1.end;
  for (const [label, args] of [["rim45", { maxSlope: 45, target: { x: end[0], z: end[1], radius: 4 } }],
                               ["rim38", { maxSlope: 38, target: { x: end[0], z: end[1], radius: 4 } }],
                               ["crater45", { maxSlope: 45, target: { x: 0, z: 0, radius: 6 } }]]) {
    const rc = await call("check_reachability", args, { soft: true });
    fs.writeFileSync(path.join(OUT, `reach-${label}.json`), rc.text);
    say("reach", label, brief(rc, 500));
  }

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
