// The Caldera iteration-4 gaps, on the synthetic 12-frame level (its root renamed Scene so blocks can be added), away
// from its own frames (x 480-600): a drape's skirt, a slot cut through a sheared skin leaving no back-facing or degenerate
// triangle, material masks lists, feathered edges and corner blending, sheared and filtered sculpting, the script's
// constants for check_flood, structures set on the ground, route constraints for check_reachability, place_object
// lists, the held game camera, game_mines and the Test's applied look. Bundled for Node by test.ps1.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, isSolid, secondOf, type Loaded, type Scene } from "../src/core";
import { frameMesh } from "../src/lib/drape-mesh";
import { scheduleFromScript, scriptConstants } from "../src/lib/flood";
import { applyDrape, planDrape } from "../src/lib/heightfield-drape";
import { apply, plan, sampleField } from "../src/lib/heightfield-plan";
import { spansAt } from "../src/lib/inspect-field";
import { grids } from "../src/lib/voxel";
import { validate } from "../src/registry";
import { holdState, stopHold, tools as gameTools } from "../src/tools/game";
import { schema as fieldSchema } from "../src/tools/heightfield";
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
function session(extra: Record<string, unknown> = {}) {
  const store = new EditorStore("t", loaded(), loaded());
  const project = { store, id: "t", info: {}, erg: { script: async () => "" } };
  const s = { requireProject: () => project, get store() { return store; }, project,
    levels: async () => { throw new Error("no server"); }, client: async () => { throw new Error("no server"); }, ...extra } as unknown as Session;
  return { store, s, project };
}
const all = [...inspectTools, ...terrainTools, ...structureTools, ...placeTools, ...gameTools];
const call = async (s: Session, name: string, args: Record<string, unknown>): Promise<any> => {
  const t = all.find((x) => x.name === name)!;
  const bad = validate(t.inputSchema, args);
  if (bad) throw new Error(bad);
  const r = await t.handler(s, args);
  return typeof r === "string" ? JSON.parse(r) : r;
};
const top = (store: EditorStore, x: number, z: number) => spansAt(grids(store), x, z)[0];

// A cone 30 high on x 480-560, z -30..30, its flanks 1.2 map units up per unit across (50 degrees): a skin of voxel 2.
const CONE = { min: [480, -30] as [number, number], max: [560, 30] as [number, number], floor: 0, expr: "max(0, 30 - abs(x)*1.2)", voxelSize: 2, thickness: 2, material: 1 };

test("heightfield_drape skirtTo: a hollow skin gets ground down to the sea floor, skirtTop only under the shore, and the cells are counted", () => {
  const { store } = session();
  const field = { ...CONE, expr: "max(0, 20 - abs(x)*0.5)", core: false };
  const hollow = planDrape(store, field);
  assert.equal(hollow.core, null);
  const all = planDrape(store, { ...field, skirtTo: -4 });
  const shore = planDrape(store, { ...field, skirtTo: -4, skirtTop: 6 });
  for (const p of [all, shore]) assert.equal(p.report.fits, true, String(p.report.why));
  const sk = (p: typeof all) => p.report.skirt as { columns: number };
  const cells = (p: typeof all) => (p.report.core as { cells: number; neededCells: number });
  assert.ok(sk(shore).columns > 0 && sk(shore).columns < sk(all).columns, `${sk(shore).columns} of ${sk(all).columns} columns`);
  assert.ok(cells(shore).cells > 0 && cells(shore).cells < cells(all).cells);
  assert.equal(cells(shore).neededCells, cells(shore).cells);
  assert.throws(() => planDrape(store, { ...field, skirtTop: 6 }), /skirtTo/);
  applyDrape(store, shore);
  // the shore (height about 2 at x 484) is solid down to -4; the middle (20 high) is the skin alone
  const low = spansAt(grids(store), 484, 0), mid = spansAt(grids(store), 520, 0);
  assert.ok(low.length && Math.min(...low.map((q) => q.bottom)) <= -3.99, JSON.stringify(low));
  assert.ok(mid.length && Math.min(...mid.map((q) => q.bottom)) > 10, JSON.stringify(mid));
  assert.deepEqual(store.problems(), []);
});

test("a slot cut through a sheared skin leaves no back-facing or degenerate triangle, and cuts where the skin is drawn", async () => {
  const { store, s } = session();
  applyDrape(store, planDrape(store, { ...CONE, core: false }));
  const skins = () => grids(store).filter((g) => g.frame.corners);
  assert.ok(skins().length >= 1);
  const sides = () => skins().reduce((n, g) => n + frameMesh(g).triangles.filter((t) => t.face === 0 || t.face === 1).length, 0);
  const sides0 = sides();
  // the skin's top at x 532 (12 from the axis: 30 - 14.4 = 15.6) before the cut
  const before = top(store, 532, 0);
  assert.ok(before && Math.abs(before.top - 15.6) < 2.5, JSON.stringify(before));
  // a slot across the flank at the skin's own height (carve: every frame, sheared)
  const r = await call(s, "sculpt_path", { mode: "carve", shape: "box", size: [2, 4, 2], points: [[532, before!.top - 1, -20], [532, before!.top - 1, 20]], units: "map", frames: skins().map((g) => g.id) });
  assert.ok(r.changed > 0, JSON.stringify(r));
  const after = top(store, 532, 0);
  assert.ok(!after || after.top < before!.top - 3, `the slot is cut where the skin is drawn: ${JSON.stringify(after)}`);
  for (const g of skins()) {
    const m = frameMesh(g);
    assert.equal(m.degenerate, 0, "no degenerate triangle");
    const back = m.triangles.filter((t) => t.n[0] * t.out[0] + t.n[1] * t.out[1] + t.n[2] * t.out[2] <= 1e-9);
    assert.equal(back.length, 0, `back-facing: ${JSON.stringify(back.slice(0, 2))}`);
  }
  // the slot's walls (faces across x) are new faces of the skin
  assert.ok(sides() > sides0, `${sides()} side triangles, ${sides0} before`);
});

test("sculpt filters: frames, onlyBlocks and skipFrames limit what a carve touches", async () => {
  const { store, s } = session();
  applyDrape(store, planDrape(store, { ...CONE, core: true }));
  const skin = grids(store).filter((g) => g.frame.corners).map((g) => g.id), core = grids(store).filter((g) => g.frame.new && !g.frame.corners).map((g) => g.id);
  assert.ok(core.length);
  const solid = (ids: number[]) => grids(store).filter((g) => ids.includes(g.id)).reduce((n, g) => n + g.words.filter(isSolid).length, 0);
  const c0 = solid(core), k0 = solid(skin);
  await call(s, "sculpt", { mode: "carve", shape: "sphere", size: [6, 6, 6], center: [530, 16, 0], units: "map", frames: skin });
  assert.equal(solid(core), c0, "the core is untouched");
  assert.ok(solid(skin) < k0);
  const c1 = solid(core);
  await call(s, "sculpt", { mode: "carve", shape: "sphere", size: [6, 6, 6], center: [510, 16, 0], units: "map", skipFrames: core });
  assert.equal(solid(core), c1);
  await call(s, "sculpt", { mode: "carve", shape: "sphere", size: [6, 6, 6], center: [510, 16, 0], units: "map", onlyBlocks: true });
  assert.ok(solid(core) < c1, "onlyBlocks reaches the added core");
  await assert.rejects(call(s, "sculpt", { mode: "fill", shape: "box", size: [2, 2, 2], center: [530, 40, 0], units: "map", material: 1, frames: [99999] }), /filter/);
});

test("materials: 64 rules, a rule with a list of masks, feathered edges and corner blending", () => {
  const rules = Array.from({ length: 64 }, (_, i) => ({ material: i % 64 }));
  assert.equal(validate(fieldSchema, { min: [0, 0], max: [1, 1], floor: 0, materials: rules }), null);
  assert.match(String(validate(fieldSchema, { min: [0, 0], max: [1, 1], floor: 0, materials: [...rules, { material: 1 }] })), /at most 64/);
  const { store } = session();
  const base = { min: [480, -10] as [number, number], max: [560, 10] as [number, number], floor: 0, expr: "10", material: 1 };
  const f = sampleField(store, { ...base, materials: [{ material: 5, masks: ["2 - abs(x - 20)", "2 - abs(x + 20)"] }] });
  const colAt = (fx: typeof f, x: number) => Math.floor((x - fx.t[0]) / fx.s) - fx.i0 + 10 * fx.nx;   // z row 10
  assert.equal(f.materialAt(colAt(f, 540), 5), 5);
  assert.equal(f.materialAt(colAt(f, 500), 5), 5);
  assert.equal(f.materialAt(colAt(f, 520), 5), 1);
  // feather: a band 8 wide round x = 520 where both materials appear; outside it one each
  const fe = sampleField(store, { ...base, materials: [{ material: 7, mask: "x", feather: { width: 8 } }] });
  const mats = (from: number, to: number) => { const m = new Set<number>(); for (let x = from; x < to; x++) for (let y = 0; y < 10; y++) m.add(fe.materialAt(colAt(fe, x + 0.5), y)); return m; };
  assert.deepEqual([...mats(517, 523)].sort(), [1, 7]);
  assert.deepEqual([...mats(480, 514)], [1]);
  assert.deepEqual([...mats(526, 560)], [7]);
  // blend: the top voxel beside the boundary takes the other material as its second, on the corners it shares
  const hard = sampleField(store, { ...base, materials: [{ material: 7, mask: "x" }] }), soft = sampleField(store, { ...base, materials: [{ material: 7, mask: "x" }], blend: true });
  const c = colAt(soft, 519.5), y = 9;
  assert.equal(secondOf(hard.wordAt(c, y)), -1);
  assert.equal(secondOf(soft.wordAt(c, y)), 7);
  assert.equal(secondOf(soft.wordAt(colAt(soft, 500), y)), -1, "away from the edge: no second material");
  assert.equal(secondOf(soft.wordAt(c, 2)), -1, "under the top voxel: no second material");
  assert.throws(() => sampleField(store, { ...base, blend: { depth: 9 } }), /blend/);
  // a blended field applies and stays a valid patch
  const p = plan(store, { ...base, materials: [{ material: 7, mask: "x" }], blend: true });
  apply(store, p);
  assert.deepEqual(store.problems(), []);
});

test("check_flood reads the script's top-level constants", () => {
  const script = "local FIRST = 5\nlocal STEP, CAP = 62, 740\nlocal TOLD = { }\nlocal function f(t) local FIRST = 1 return t end\n" +
    "wum.sim.waterSchedule{ startTurn = FIRST, step = STEP, cap = CAP - 40, units = \"world\" }\n";
  assert.deepEqual([...scriptConstants(script)], [["FIRST", 5], ["STEP", 62], ["CAP", 740]]);
  assert.deepEqual(scheduleFromScript(script), { startTurn: 5, step: 62, cap: 700, units: "world" });
  // a local assigned again is not a constant
  assert.throws(() => scheduleFromScript("local S = 2\nS = 3\nwum.sim.waterSchedule{ step = S, cap = 9 }"), /not a literal/);
  // literals as before
  assert.deepEqual(scheduleFromScript("wum.sim.waterSchedule{ step = 3, cap = 40 }"), { step: 3, cap: 40 });
});

test("build_castle ground: the floor is set from the ground under the footprint, less sink", async () => {
  const { store, s } = session();
  applyDrape(store, planDrape(store, { ...CONE, expr: "max(0, 20 - abs(x)*0.3)", core: true }));
  const r = await call(s, "build_castle", { at: [530, 0, 0], size: [12, 12], wallHeight: 3, towerRadius: 2, keep: false, place: "new", ground: "max", sink: 0.5, units: "map", dryRun: true });
  const g = r.groundUnder;
  assert.ok(g && g.max > g.min && g.mean > g.min && g.mean < g.max, JSON.stringify(g));
  assert.ok(Math.abs(r.structureInfo.courtyardFloorY - (g.max - 0.5)) < 1e-6, JSON.stringify(r.structureInfo));
  const m = await call(s, "build_tower", { at: [530, 0, 0], radius: 2, height: 4, ground: "min", units: "map", dryRun: true });
  assert.ok(Math.abs(m.groundUnder.y - m.groundUnder.min) < 1e-6);
  await assert.rejects(call(s, "build_keep", { at: [900, 0, 900], ground: "max", units: "map", dryRun: true }), /no voxel ground/);
});

test("check_reachability via and materialSlopes: the road is the climb, the flanks are not", async () => {
  const { store, s } = session();
  // a ramp up x (17 degrees), a road of material 3 along z = 0, rock (1) beside it
  apply(store, plan(store, { min: [480, -10], max: [600, 10], floor: 0, expr: "1 + 0.3*(x + 60)", material: 1, materials: [{ material: 3, mask: "2 - abs(z)" }] }));
  const R = { x0: 480, z0: -10, x1: 600, z1: 10, units: "map", target: { x: 597, z: 6, radius: 2 }, maxSlope: 40 };
  const from = [[484.5, 9, 6]];
  const open = await call(s, "check_reachability", { ...R, from });
  assert.equal(open.spawns[0].reachable, true);
  const steep = await call(s, "check_reachability", { ...R, from, materialSlopes: [{ materials: [1], maxSlope: 10 }] });
  assert.equal(steep.spawns[0].reachable, true, "the rock is too steep, the road is not: it walks the road");
  assert.ok(steep.rules.materialSlopes);
  const offRoad = await call(s, "check_reachability", { ...R, from, materialSlopes: [{ materials: [1], maxSlope: 10 }], via: { materials: [3], mode: "avoid" } });
  assert.equal(offRoad.spawns[0].reachable, false, "off the road the flank cannot be climbed");
  const onRoad = await call(s, "check_reachability", { ...R, from, via: { mask: "material - 2.5", mode: "only", near: 5 } });
  assert.equal(onRoad.spawns[0].reachable, true, "along the road alone it gets there");
  assert.equal(onRoad.rules.via.mode, "only");
  await assert.rejects(call(s, "check_reachability", { ...R, from, via: { mode: "only" } }), /materials, a mask/);
});

test("place_object objects: a failed item is named in the answer and the rest are placed", async () => {
  const { store, s } = session();
  apply(store, plan(store, { min: [480, -10], max: [600, 10], floor: 0, expr: "4", material: 1 }));
  const r = await call(s, "place_object", { units: "map", objects: [
    { type: "mine", xz: [500, 0], label: "near the gate" }, { type: "oildrum", xz: [9000, 9000], label: "off the map" }, { type: "mine", xz: [520, 0] }] });
  assert.equal(r.placedCount, 2);
  assert.equal(r.failedCount, 1);
  assert.equal(r.failed[0].label, "off the map");
  assert.equal(r.failed[0].index, 1);
  assert.match(r.failed[0].error, /off the map: .*nothing below/);
  await assert.rejects(call(s, "place_object", { objects: [{ type: "mine", xz: [500, 0] }], kind: "health" }), /inside each item/);
});

/** A session whose game answers `answer(method, params)`; calls are recorded. */
function gameSession(answer: (m: string, p: any) => unknown, methods: string[], extra: Record<string, unknown> = {}) {
  const calls: [string, any][] = [];
  const client = {
    call: async (m: string, p: any) => { calls.push([m, p]); return answer(m, p); },
    subscribe: (_ch: string, _f: unknown, fn: (m: unknown) => void) => { (client as any).emit = fn; return () => { (client as any).emit = null; }; },
  };
  const t = session({ ensure: async () => ({ methods }), client: async () => client, ...extra });
  return { ...t, calls, client };
}

test("game_camera hold: the pose is held until released, and game_shots put it back", async () => {
  const g = gameSession((m) => (m === "render.camera" ? { set: true, cleared: true, frame: 10 } : m === "render.shot" ? { ready: true, path: "x.png", width: 2, height: 2, frame: 12 } : null), ["render.camera", "render.shot"]);
  const r = await call(g.s, "game_camera", { pos: [0, 10, 0], target: [1, 0, 1], hold: true });
  assert.equal(r.held, true);
  assert.equal(g.calls[0][1].frames, 600);
  assert.equal(holdState().holding, true);
  const c = await call(g.s, "game_camera", { clear: true });
  assert.ok(c.released);
  assert.equal(holdState().holding, false);
  await call(g.s, "game_camera", { pos: [0, 10, 0], target: [1, 0, 1], hold: true });
  await call(g.s, "game_camera", { pos: [0, 10, 0], target: [2, 0, 1] });
  assert.equal(holdState().holding, false, "a pose without hold releases the held one");
  stopHold();
});

test("game_mines: new mines are seen with where they came from, and untilFire ends after the factory's fire", async () => {
  let n = 0;
  const mine = (h: number, x: number) => ({ handle: h, kind: "Other", type: "ParabolicPayloadLogicEntity", label: "Landmine", pos: { x, y: 200, z: 0 } });
  const g = gameSession((m) => {
    if (m === "entities.list") return n++ < 2 ? [mine(1, 100), { handle: 9, kind: "Other", type: "MineFactoryLogicEntity", label: "", pos: { x: 0, y: 0, z: 0 } }] : [mine(1, 100), mine(2, 40)];
    if (m === "bus.names") return [{ name: "MineFactory.Fire" }, { name: "MineFactory.FireEnd" }, { name: "Worm.Hit" }];
    return null;
  }, ["entities.list", "bus.names"]);
  const r = await call(g.s, "game_mines", { watchSeconds: 0.6, units: "world" });
  assert.equal(r.minesBefore, 1);
  assert.equal(r.appeared.length, 1);
  assert.equal(r.appeared[0].handle, 2);
  assert.deepEqual(r.messages.watched, ["MineFactory.Fire", "MineFactory.FireEnd"]);
  assert.ok(!r.mines.some((x: { type: string }) => /Factory/.test(x.type)), "the factory is not a mine");
  // untilFire: the watch ends 3 s after MineFactory.FireEnd, well before watchSeconds
  const t0 = Date.now();
  setTimeout(() => (g.client as any).emit?.({ name: "MineFactory.FireEnd" }), 200);
  const f = await call(g.s, "game_mines", { untilFire: true, watchSeconds: 60 });
  assert.equal(f.fired, true);
  assert.ok(Date.now() - t0 < 10000);
  assert.equal(f.messages.counts["MineFactory.FireEnd"], 1);
});

test("test: the project's time of day is asked for, and the answer says what the match applied", async () => {
  const asked: unknown[] = [];
  const g = gameSession((m) => {
    if (m === "level.test.applied") return { key: "k", tod: { asked: "NIGHT", applied: false, why: "the frontend's time of day could not be cleared: its value loads" } };
    if (m === "lua.eval") return { ok: true, text: "BUILDING" };
    return null;
  }, ["level.test", "level.test.applied", "lua.eval"], { save: async () => ({ problems: [] }) });
  g.store.scene.databank.timeOfDay = "NIGHT";
  (g.project as any).erg = { test: async (o: unknown) => { asked.push(o); return { key: "k", state: "playing" }; }, onTest: () => () => {} };
  const r = await call(g.s, "test", { timeoutSec: 1 });
  assert.deepEqual(asked, [{ tod: "NIGHT" }]);
  assert.equal(r.look.timeOfDay.applied, "DAY");
  assert.equal(r.look.timeOfDay.fellBack, true);
  assert.equal(r.look.theme.applied, "BUILDING");
  assert.ok(r.warnings.some((w: string) => /NIGHT/.test(w)), JSON.stringify(r.warnings));
});
