// Caldera: an Erg MCP benchmark map, built only through the erg-mcp server's tools (stdio JSON-RPC, no hand-written
// patches). A volcanic island on Multi.DinerMight: a crater at the summit holding the mine factory, six castles on
// shelves at different heights round the flanks and the shore, a lava tube into the crater, and a level script
// (script.lua) that raises the sea every turn until only the upper cone is dry.
//
//   tools\node\node.exe tools\erg-mcp\bench\caldera\build.mjs [--out <dir>] [--no-export] [--keep-mod]
//
// It connects like erg-mcp does: MELANGE_GAME_DIR (default C:\Users\Jamin\Desktop\WUMFix\testenv\C) with the game
// closed, starting Melange.exe --serve (MELANGE_EXE names another Melange.exe; default this checkout's
// build\x86-release\Melange.exe). The project id is `caldera`; its folder (<game>\Melange\erg\projects in a portable
// install, else Documents\Melange\erg\projects) is deleted first, so every run starts from the base. Renders go to
// --out (default out\ next to this file); the source export is copied to pack\ next to this file.
import { spawn } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
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
// A portable install (a Melange folder next to the game) keeps its projects there, otherwise Documents\Melange.
const PROJECTS = fs.existsSync(path.join(GAME, "Melange")) ? path.join(GAME, "Melange/erg/projects")
  : path.join(os.homedir(), "Documents/Melange/erg/projects");

// ---- the design (map units: 1 voxel of the base = 1 map unit = 20 world units; y up, floor 0) ----
//
// Budget (measured with heightfield_plan and dry-run castles): Diner Might's frames hold 11 652 of the game's 65 535
// cells, leaving 53 883. The island at 2.75 map units per voxel takes about 35 800 in 25 blocks; a 16 x 16 castle with
// a keep is one 18 x 10 x 18 block (3240 cells) and a 12 x 12 one 1568, so two large and four small castles take about
// 12 750, leaving some 5000 for later work. At 2.5 the same island needs about 52 000 cells.

const VOXEL = 2.75;                // island voxels, map units
const ISLAND = 120;                // half side of the heightfield rectangle
const CELLS_ISLAND = 36000;        // cells the island may take
const WATER_START = -20;           // world units: the beaches are dry at the start

// Castle sites round the volcano: centre, shelf height (whole voxels of 2.75; the castle stands on the voxel top
// rounded down, so its walls bed into the shelf), outer size, style. The heightfield flattens a shelf under each, and
// the gate faces the summit. Heights from the shore up: 5.5, 8.25, 11, 16.5, 19.25, 27.5.
const CASTLES = [
  { name: "Seawall",  at: [-88, 16],  h: 5.5,   size: [12, 12], mat: 12, towerShape: "square" },
  { name: "Harbour",  at: [64, -56],  h: 8.25,  size: [16, 16], mat: 37, keep: true },
  { name: "Headland", at: [12, 86],   h: 11,    size: [12, 12], mat: 55 },
  { name: "Ashgate",  at: [-40, -48], h: 16.5,  size: [12, 12], mat: 12 },
  { name: "Cinder",   at: [54, 22],   h: 19.25, size: [12, 12], mat: 37, towerShape: "square" },
  { name: "Ember",    at: [-26, 32],  h: 27.5,  size: [16, 16], mat: 55, keep: true },
];
// Two worms in each large castle (either side of the keep), one in each of the others.
const SPAWNS = [[0, 0], [1, 1], [2, 1], [3, 2], [4, 3], [5, 4], [6, 5], [7, 5]];

// Diner Might's material file (Building theme): grass01, rock01, rock02, sand01, Road (dark asphalt, for ash) and
// Red Metal (the reddest material it has, for the crater floor).
const MAT = { grass: 0, rock: 4, darkRock: 6, sand: 10, ash: 14, lava: 21 };

function heightExpr() {
  const coast = "(104 + 9*fbm(x/35, z/35, 11, 2))";
  const land = `smoothstep(${coast} + 2, ${coast} - 16, r)`;
  const lowland = `(4 + 3*fbm(x/22, z/22, 4, 3))*${land}`;
  // A cone of about 38 degrees with a softplus foot, and seven ridges with gullies between them round the flanks.
  const cone = "6.2*log(1 + exp((76 - r)/8))*(1 + 0.12*cos(7*theta + 1.5*noise2(x/40, z/40, 9))*smoothstep(15, 40, r))";
  let h = `(max(${lowland}, ${cone}) + 1.2*fbm(x/9, z/9, 21, 2)*${land})`;
  for (const c of CASTLES) {
    const s = Math.max(c.size[0], c.size[1]) / 2 + 3;
    const d = `sqrt((x - ${c.at[0]})^2 + (z - ${c.at[1]})^2)`;
    h = `lerp(${h}, ${c.h}, smoothstep(${s + 8}, ${s}, ${d}))`;
  }
  return h;
}

const FIELD = {
  min: [-ISLAND, -ISLAND], max: [ISLAND, ISLAND], floor: 0, baseDepth: VOXEL, voxelSize: VOXEL, maxCells: CELLS_ISLAND,
  expr: heightExpr(),
  crater: { radius: 14, depth: 18, rim: 3 },  // rim about 49, floor 38.5
  material: MAT.rock,
  materials: [
    { material: MAT.lava, minHeight: 36, maxHeight: 41.25, maxSlope: 20 },   // the crater floor
    { material: MAT.ash, minHeight: 44 },                   // the summit and the crater wall
    { material: MAT.darkRock, minHeight: 28 },              // the upper cone
    { material: MAT.sand, maxHeight: 3 },                   // beaches
    { material: MAT.grass, maxHeight: 12 },                 // lowlands
  ],
};

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
// erg-mcp has no tool to delete a project, and the server lists the projects when it starts, so the old one goes first.
const dir = path.join(PROJECTS, ID);
if (fs.existsSync(dir)) {
  fs.rmSync(dir, { recursive: true, force: true });
  say("deleted the old project", dir);
}
const { call, stop } = await startServer();
try {
  say("connect", brief(await call("connect", { gameDir: GAME }), 160));
  const np = await call("new_project", { base: "Multi.DinerMight", slug: ID, title: "Caldera" });
  if (np.json?.created?.id !== ID) throw new Error(`new_project gave id ${np.json?.created?.id}, not ${ID}`);
  await call("open_project", { id: ID });

  // Setting: the base's own material set (Building), at evening; the base's terrain wiped (its frames stay, so its
  // 11 652 cells stay spent); the sea starts below the beaches.
  await call("set_theme", { theme: "BUILDING" });
  await call("set_time_of_day", { timeOfDay: "EVENING" });
  await call("set_water", { level: WATER_START });
  say("clear", brief(await call("clear_base_terrain", { all: true, wipe: true }), 200));

  // Island and volcano.
  const plan = await call("heightfield_plan", FIELD);
  const pj = plan.json;
  say("plan: fits", pj.fits, "blocks", JSON.stringify(pj.blocks?.placed), "/", JSON.stringify(pj.blocks?.needed), "cells", JSON.stringify(pj.budget?.cells),
    "voxels", JSON.stringify(pj.voxels), "notes", JSON.stringify(pj.notes ?? []), pj.why ?? "");
  fs.writeFileSync(path.join(OUT, "plan.json"), plan.text);
  if (!pj.fits) throw new Error("the island does not fit: " + (pj.why ?? ""));
  const hf = await call("heightfield_region", FIELD);
  say("island", JSON.stringify(hf.json.limits.voxelCells), "blocks", hf.json.limits.newFrames?.used ?? JSON.stringify(hf.json.limits.newFrames));

  // Castles, in fine voxels (scale 1, new blocks) on their shelves, gates towards the summit. Each stands on the
  // shelf's voxel top rounded down to a whole unit, so its walls bed into the coarser island voxels.
  const castleFloor = [];
  for (const c of CASTLES) {
    const [x, z] = c.at;
    const g = await call("query_ground", { x, z, units: "map" });
    const top = g.json?.standOn?.map;
    if (typeof top !== "number") throw new Error(`no ground at ${c.name}: ${g.text.slice(0, 300)}`);
    const y = Math.floor(top + 1e-6);
    castleFloor.push(y);
    const gate = Math.abs(x) > Math.abs(z) ? (x > 0 ? "w" : "e") : (z > 0 ? "n" : "s");
    const big = !!c.keep;
    const r = await call("build_castle", {
      at: [x, y, z], size: c.size, wallHeight: big ? 5 : 4, wallThickness: 2, towerHeight: big ? 8 : 7, towerRadius: big ? 2.5 : 2,
      towerShape: c.towerShape ?? "round", keep: big, ...(big ? { keepSize: [6, 6], keepHeight: 9 } : {}),
      gate, gateWidth: 3, gateHeight: big ? 4 : 3, crenellations: true, material: c.mat, place: "new",
    });
    say("castle", c.name, "shelf", top, "floor", y, "gate", gate, "cells", r.json?.usage?.voxelCells ?? r.text.slice(0, 200));
  }

  // Two lava tubes through the crater rim, from the south flank (above Ember) and the north-east flank (above
  // Harbour): square bores 6 wide whose floor runs from the ground at the mouth up to the crater floor, so the
  // summit can be reached on foot without climbing the rim. The mouths are read off the terrain.
  const crater = (await call("query_ground", { x: 0, z: 0, units: "map" })).json.standOn.map;
  for (const deg of [90, -40]) {
    const a = (deg * Math.PI) / 180, at = (r) => [Math.round(r * Math.cos(a) * 10) / 10, Math.round(r * Math.sin(a) * 10) / 10];
    const [mx, mz] = at(26), [ix, iz] = at(5);
    const mouth = (await call("query_ground", { x: mx, z: mz, units: "map" })).json.standOn.map;
    const t = await call("dig_tunnel", { points: [[mx, mouth + 2.5, mz], [ix, crater + 3, iz]], radius: 3, shape: "square" });
    say("tunnel", deg, "mouth", mouth, "crater", crater, "removed", t.json?.voxels?.removed);
  }

  // The mine factory on the crater floor, the spawns in the castles' courtyards.
  say("factory", brief(await call("place_object", { type: "minefactory", units: "map", xz: [0, 0] }), 300));
  const spawns = SPAWNS.map(([worm, ci]) => {
    const c = CASTLES[ci];
    const pair = SPAWNS.filter((s) => s[1] === ci);
    // Either side of the keep (between it and the curtain wall), or the middle of an open courtyard.
    const dx = c.keep ? (pair[0][0] === worm ? -1 : 1) * (c.size[0] / 2 - 3.5) : 0;
    return { worm, xz: [c.at[0] + dx, c.at[1]] };
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
} finally {
  fs.writeFileSync(path.join(OUT, "calls.json"), JSON.stringify(log, null, 1));
  say("MCP tool calls:", calls);
  await stop();
}
