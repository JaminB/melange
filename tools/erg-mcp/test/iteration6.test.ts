// The Caldera iteration-6 gaps, on the synthetic 12-frame level (its root renamed Scene so blocks can be added), away
// from its own frames (x 480-600): the factory's keepKnot and surface snap, the test answer's factory and sim mods,
// end_match, the script's water run by the server (and the static reading's warning), check_flood's unit guard, unknown
// arguments named, a structure's cells and costAt, blended paint edges, the overview's orbit and game_mines' countdown,
// trajectories and shots on a fire. Bundled for Node by test.ps1.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, secondOf, maskOf, type Loaded, type Scene } from "../src/core";
import { checkFactory } from "../src/lib/factory-check";
import { applyDrape, planDrape } from "../src/lib/heightfield-drape";
import { apply, plan } from "../src/lib/heightfield-plan";
import { scheduleCalls, scriptWater } from "../src/lib/script-water";
import { getVoxel, grids, isSolid, material } from "../src/lib/voxel";
import { unknownHint, validate } from "../src/registry";
import { tools as gameTools } from "../src/tools/game";
import { tools as inspectTools } from "../src/tools/inspect";
import { tools as placeTools } from "../src/tools/place";
import { tools as structureTools } from "../src/tools/structures";
import { tools as terrainTools } from "../src/tools/terrain";
import type { Session } from "../src/session";

function loaded(): Loaded {
  const s: Scene = JSON.parse(readFileSync(new URL("../../../../tests/fixtures/erg/synthetic-12.json", import.meta.url), "utf8"));
  s.frames.find((f) => f.id === 1)!.name = "Scene";
  const blobs = new Map<number, ArrayBuffer>();
  for (const [ref, buf] of syntheticBlobs(s) as Map<number, Uint8Array>) blobs.set(ref, buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength) as ArrayBuffer);
  return { scene: s, blobs };
}
function session(extra: Record<string, unknown> = {}, script = "") {
  const store = new EditorStore("t", loaded(), loaded());
  const project = { store, id: "t", info: {}, erg: { script: async () => script } };
  const s = { requireProject: () => project, get store() { return store; }, project,
    levels: async () => { throw new Error("no server"); }, client: async () => { throw new Error("no server"); }, ...extra } as unknown as Session;
  return { store, s, project };
}
/** A session whose server answers `answer(method, params)`; `onSub` gets each subscription's callback. */
function gameSession(answer: (m: string, p: any) => unknown, methods: string[], script = "", onSub?: (cb: (m: any) => void) => void) {
  const client = { call: async (m: string, p: any) => answer(m, p), subscribe: (_c: string, _f: unknown, cb: (m: any) => void) => { onSub?.(cb); return () => undefined; } };
  return session({ ensure: async () => ({ methods }), client: async () => client, save: async () => ({ saved: true, warnings: [], problems: [] }) }, script);
}
const all = [...inspectTools, ...structureTools, ...placeTools, ...gameTools, ...terrainTools];
const call = async (s: Session, name: string, args: Record<string, unknown>): Promise<any> => {
  const t = all.find((x) => x.name === name)!;
  const bad = validate(t.inputSchema, args);
  if (bad) throw new Error(bad);
  const r = await t.handler(s, args);
  if (r && typeof r === "object" && "content" in (r as object)) {
    const parts = (r as { content: { type: string; text?: string }[] }).content;
    return { ...JSON.parse(parts.find((p) => p.type === "text")!.text!), images: parts.filter((p) => p.type === "image").length };
  }
  return typeof r === "string" ? JSON.parse(r) : r;
};
const CONE = { min: [530, -10] as [number, number], max: [590, 10] as [number, number], floor: 0, expr: "max(2, 12 - abs(x)*0.3)", voxelSize: 2, thickness: 2, material: 1, core: false };

test("the mine factory: keepKnot makes a skin a warning, snap puts the knot on the skin, and both reach the patch", async () => {
  const { store, s } = session();
  applyDrape(store, planDrape(store, CONE));
  const placed = await call(s, "place_object", { type: "minefactory", units: "map", pos: [560, 40, 0], snap: "surface" });
  const g = checkFactory(store, (v) => `${v} map`)!;
  assert.ok(g.lands?.draped, "the knot is over the skin");
  assert.ok(Math.abs(placed.snapped[1] - g.lands!.y) < 1e-6, `snapped onto the skin's top: ${JSON.stringify(placed.snapped)} vs ${g.lands!.y}`);
  assert.ok(placed.factoryWillBeLost && placed.issues.some((l: string) => /^error: .*draped skin.*keepKnot/.test(l)), JSON.stringify(placed.issues));
  const set = await call(s, "set_mine_factory", { keepKnot: true, activation: 16, inactiveTurns: 0 });
  assert.deepEqual(set.factory, { keepKnot: true, activation: 16, inactiveTurns: 0 });
  const c = checkFactory(store, (v) => `${v} map`)!;
  assert.deepEqual(c.problems, [], JSON.stringify(c.problems));
  assert.ok(c.warnings.some((w) => /keepKnot it stays at its knot/.test(w)));
  // the game's default (false) is dropped again
  assert.deepEqual((await call(s, "set_mine_factory", { keepKnot: false })).factory, { activation: 16, inactiveTurns: 0 });
  await assert.rejects(() => call(s, "place_object", { type: "crate", units: "map", xz: [560, 0], snap: "surface" }), /snap "surface" takes pos/);
  // with keepKnot and nothing under the knot, the water is checked against the knot
  await call(s, "set_mine_factory", { keepKnot: true });
  await call(s, "move", { id: placed.placed[0].id, pos: [700, 5, 0], units: "map" });
  const air = checkFactory(store, (v) => `${v} map`, 8)!;
  assert.ok(air.warnings.some((w) => /in the air/.test(w)) && air.problems.some((p) => /over the factory's knot/.test(p)), JSON.stringify(air));
});

test("test: the answer's look reports the sim mods loaded and the factory's set-up; a refusal in a match names restart and end_match", async () => {
  let applied: any = { key: "Multi.ergtest_t", tod: { asked: "DAY", applied: true, why: "" }, simMods: ["erg:t"],
    factory: { stem: "ergtest_t", activation: { asked: 16, write: "ok", found: 30 }, inactiveTurns: { asked: 0, write: "ok", found: 0 }, knot: [1, 2, 3], settled: [1, 2, 3], landed: false, keptKnot: true } };
  let refuse = false;
  const g = gameSession((m) => {
    if (m === "level.test.applied") return applied;
    if (m === "erg.level.test" || m === "level.test") { if (refuse) throw new Error("the game must be at the frontend to test a level"); return { key: "Multi.ergtest_t", state: "playing" }; }
    return null;
  }, ["level.test", "level.test.applied"]);
  apply(g.store, plan(g.store, { min: [480, -10], max: [520, 10], floor: 0, expr: "4", material: 1 }));
  await call(g.s, "place_object", { type: "minefactory", units: "map", xz: [500, 0], activation: 16, inactiveTurns: 0 });
  const project = g.s.requireProject() as unknown as { erg: Record<string, unknown> };
  project.erg.test = async () => { if (refuse) throw new Error("the game must be at the frontend to test a level"); return { key: "Multi.ergtest_t", state: "playing" }; };
  project.erg.onTest = () => () => undefined;
  const r = await call(g.s, "test", { timeoutSec: 1 });
  assert.deepEqual(r.look.simModsLoaded, ["erg:t"]);
  assert.equal(r.look.factory.activation.heldBefore, 30);
  assert.equal(r.look.factory.dropWorld.keptKnot, true);
  assert.ok(r.warnings.some((w: string) => /held 30 at the factory's set-up/.test(w)), JSON.stringify(r.warnings));
  applied = { ...applied, factory: { ...applied.factory, activation: { asked: 16, write: "ok", found: 16 }, keptKnot: false } };
  const r2 = await call(g.s, "test", { timeoutSec: 1 });
  assert.ok(r2.warnings.some((w: string) => /drop hit no land/.test(w)), JSON.stringify(r2.warnings));
  refuse = true;
  await assert.rejects(() => call(g.s, "test", { timeoutSec: 1 }), /restart: true.*end_match/);
});

test("end_match: at the menu it says so; in a Test match it leaves through level.test.quit", async () => {
  let scene = "menu", quits = 0;
  const g = gameSession((m) => {
    if (m === "lua.eval") return { ok: true, text: scene };
    if (m === "level.test.quit") { quits++; scene = "menu"; return { queued: true }; }
    return null;
  }, ["lua.eval", "level.test.quit"]);
  assert.equal((await call(g.s, "end_match", {})).left, false);
  scene = "match";
  const r = await call(g.s, "end_match", {});
  assert.equal(r.left, true);
  assert.equal(quits, 1);
});

test("the script's water: the server's run is used when it has level.script.water, else the first call with a warning", async () => {
  const script = "wum.sim.waterSchedule{ startTurn = 2, step = 6, cap = 48 }\nwum.events.on(\"sim.turnStarted\", function(n, t) if t == 4 then wum.sim.waterSchedule{ startTurn = 5, step = 20, cap = 200 } end end)\n";
  assert.equal(scheduleCalls(script), 2);
  assert.equal(scheduleCalls("-- wum.sim.waterSchedule{step = 1}\n"), 0);
  let asked: any = null;
  const g = gameSession((m, p) => {
    if (m !== "level.script.water") return null;
    asked = p;
    return { levels: [36, 42, 48, 48, 68, 88].map((w, i) => ({ turn: i + 1, world: w, end: w })), schedules: [{ turn: 0 }, { turn: 4 }], errors: [], errorCount: 0, calls: {} };
  }, ["level.script.water"], script);
  const sim = (await scriptWater(g.s, 1.8, 6))!;
  assert.equal(sim.simulated, true);
  assert.equal(asked.start, 36);
  assert.equal(sim.waterAt(5), 68 / 20);
  assert.equal(sim.waterAt(9), 88 / 20, "past the run: the last level");
  const st = (await scriptWater(session({}, script).s, 1.8))!;
  assert.equal(st.simulated, false);
  assert.ok(st.warnings.some((w) => /2 wum\.sim\.waterSchedule calls/.test(w)));
});

test("check_flood: levels far over every ground in map units are refused as world units", async () => {
  const { store, s } = session();
  apply(store, plan(store, { min: [480, -12], max: [600, 12], floor: 0, expr: "2 + max(0, x + 20)*0.2", material: 1 }));
  await call(s, "place_spawn_knots", { units: "map", spawns: [{ worm: 0, xz: [482, 0] }] });
  const R = { x0: 480, z0: -12, x1: 600, z1: 12 };
  await assert.rejects(() => call(s, "check_flood", { ...R, levels: [{ turn: 1, level: 36 }, { turn: 9, level: 640 }], start: 36 }), /units: "world"/);
  const ok = await call(s, "check_flood", { x0: 480 * 20, z0: -12 * 20, x1: 600 * 20, z1: 12 * 20, levels: [{ turn: 1, level: 36 }, { turn: 9, level: 640 }], start: 36, units: "world" });
  assert.ok(ok.spawns.length >= 1 && ok.flood.levelsUnits.startsWith("world"), JSON.stringify(ok.flood).slice(0, 300));
});

test("unknown arguments name the one meant", () => {
  assert.match(unknownHint("scale", ["radius", "blockScale", "height"]), /did you mean blockScale/);
  assert.match(unknownHint("hieght", ["radius", "blockScale", "height"]), /did you mean height/);
  assert.match(unknownHint("zzz", ["a", "b"]), /the arguments are a, b/);
  const t = structureTools.find((x) => x.name === "build_tower")!;
  assert.match(validate(t.inputSchema, { at: [0, 0, 0], radius: 3, height: 5, scale: 1.5 })!, /scale: is not a known argument \(did you mean blockScale\?\)/);
});

test("structures: the answer counts the new blocks' cells, and costAt plans the same structure at other block scales", async () => {
  const { s } = session();
  const r = await call(s, "build_tower", { at: [700, 0, 0], radius: 4, height: 12, hollow: false, place: "new", dryRun: true, costAt: [1, 1.5, 2] });
  assert.ok(r.cells.newBlocks > 0 && r.cells.newBlocks === r.newBlocks.reduce((n: number, b: { cells: number }) => n + b.cells, 0));
  assert.equal(r.costAt.length, 3);
  const [c1, c15] = r.costAt;
  assert.ok(c1.fits && c15.fits && c15.cells < c1.cells * 0.5, JSON.stringify(r.costAt));
  assert.equal(c1.cells, r.cells.newBlocks, "costAt at 1 is the call's own plan");
});

test("paint_path blend: the band's edge voxels take the material as a second material on the corners inside it", async () => {
  const { store, s } = session();
  apply(store, plan(store, { min: [480, -10], max: [520, 10], floor: 0, expr: "4", material: 1 }));
  const plain = await call(s, "paint_path", { units: "map", material: 9, points: [[481, 0.3], [519, 0.3]], width: 3, dryRun: true });
  const r = await call(s, "paint_path", { units: "map", material: 9, points: [[481, 0.3], [519, 0.3]], width: 3, blend: true, falloff: 0.5 });
  assert.ok(r.blendedEdge > 0 && r.changed === plain.changed + r.blendedEdge, JSON.stringify({ plain: plain.changed, r }));
  let second = 0, full = 0;
  for (const g of grids(store)) {
    if (!g.frame.new) continue;
    const [X, Y, Z] = g.size;
    for (let x = 0; x < X; x++) for (let y = 0; y < Y; y++) for (let z = 0; z < Z; z++) {
      const w = getVoxel(g, x, y, z);
      if (!isSolid(w)) continue;
      if (material(w) === 9) full++;
      else if (secondOf(w) === 9 && maskOf(w)) second++;
    }
  }
  assert.ok(full > 0 && second === r.blendedEdge, `${full} painted, ${second} blended`);
  await assert.rejects(() => call(s, "paint_path", { units: "map", material: 9, points: [[481, 0], [519, 0]], width: 3, falloff: 1 }), /falloff is for blend/);
});

test("game_camera overview: azimuth, elevation and distance orbit the level's middle", async () => {
  let cam: any = null;
  const g = gameSession((m, p) => { if (m === "render.camera") { cam = p; return { set: true, frame: 1 }; } return null; }, ["render.camera"]);
  apply(g.store, plan(g.store, { min: [480, -10], max: [520, 10], floor: 0, expr: "4", material: 1 }));
  await call(g.s, "game_camera", { preset: "overview" });
  const south = cam;
  assert.ok(south.pos[2] > south.target[2] && Math.abs(south.pos[0] - south.target[0]) < 1e-6, "the default looks from the south (+z)");
  await call(g.s, "game_camera", { preset: "overview", azimuth: 90, elevation: 30, distance: 100 });
  const dx = cam.pos[0] - cam.target[0], dy = cam.pos[1] - cam.target[1], dz = cam.pos[2] - cam.target[2];
  assert.ok(dx > 0 && Math.abs(dz) < 1e-6, "azimuth 90: from the east (+x)");
  assert.ok(Math.abs(Math.hypot(dx, dy, dz) - 2000) < 1e-3 && Math.abs(Math.atan2(dy, dx) * 180 / Math.PI - 30) < 1e-6, JSON.stringify(cam));
  await assert.rejects(() => call(g.s, "game_camera", { pos: [0, 0, 0], target: [1, 0, 0], azimuth: 3 }), /for preset overview/);
});

test("game_mines: the factory's countdown, mines' trajectories, and shots when MineFactory.Fire arrives", async () => {
  const hex = () => {
    const b = new Uint8Array(0x90), v = new DataView(b.buffer);
    v.setFloat32(0x2c, 10000, true); v.setInt32(0x84, 3, true); v.setInt32(0x88, -1, true); v.setInt32(0x8c, 2, true);
    return Array.from(b, (n) => n.toString(16).padStart(2, "0")).join("");
  };
  let sub: ((m: any) => void) | null = null, poll = 0, shots = 0, cams = 0;
  const g = gameSession((m) => {
    if (m === "entities.list") {
      poll++;
      if (poll === 3) sub?.({ name: "MineFactory.Fire" });
      if (poll === 9) sub?.({ name: "MineFactory.FireEnd" });
      const mines = poll >= 4 ? [{ handle: 21, kind: "Projectile", type: "Landmine", pos: { x: 10000, y: 1000 - poll * 50, z: 0 } }] : [];
      return [{ handle: 7, kind: "Other", type: "MineFactoryLogicEntity", pos: null }, ...mines];
    }
    if (m === "entity.inspect") return { hex: hex() };
    if (m === "bus.names") return [{ name: "MineFactory.Fire" }, { name: "MineFactory.FireEnd" }];
    if (m === "render.camera") { cams++; return { set: true, cleared: true, frame: 5 }; }
    if (m === "render.shot") { shots++; return { ready: true, path: "x.png", width: 1, height: 1, frame: 7, clean: true }; }
    return null;
  }, ["entities.list", "entity.inspect", "bus.names", "render.camera", "render.shot"], "", (cb) => { sub = cb; });
  apply(g.store, plan(g.store, { min: [480, -10], max: [520, 10], floor: 0, expr: "4", material: 1 }));
  await call(g.s, "place_object", { type: "minefactory", units: "map", xz: [500, 0] });
  // render.shot's PNG is read from disk: point it at a file that exists
  const fixture = new URL("../../../../tests/fixtures/erg/synthetic-12.json", import.meta.url);
  const shotPath = decodeURIComponent(fixture.pathname.replace(/^\/([A-Za-z]:)/, "$1"));
  const r = await (async () => {
    const t = all.find((x) => x.name === "game_mines")!;
    const orig = (g.s as any).client;
    (g.s as any).client = async () => { const c = await orig(); return { ...c, call: async (m: string, p: any) => { const v = await c.call(m, p); return m === "render.shot" ? { ...v, path: shotPath } : v; } }; };
    const out = await t.handler(g.s, { alive: true, untilFire: true, watchSeconds: 6, trajectories: true, shotsOnFire: { count: 2, everyMs: 100, images: false } });
    return out as any;
  })();
  assert.equal(r.factoryEntity.idleTurnsLeft, -1);
  assert.equal(r.factoryEntity.minesToFire, 3);
  assert.equal(r.fired, true);
  assert.equal(r.appeared.length, 1);
  assert.ok(r.appeared[0].path.length >= 3 && r.appeared[0].path[0][2] > r.appeared[0].path.at(-1)[2], JSON.stringify(r.appeared[0].path));
  assert.equal(r.shotsOnFire.taken, 2);
  assert.equal(shots, 2);
  assert.ok(cams >= 3, "the camera is pointed for each shot and given back");
});
