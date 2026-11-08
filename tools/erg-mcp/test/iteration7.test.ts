// The Caldera iteration-7 gaps, on the synthetic 12-frame level (its root renamed Scene so blocks can be added), away
// from its own frames (x 480-700): the engine's worm rotation and the flood verdict (turnOrder, minOwnTurns,
// minDryMargin), the factory advisory with keepKnot on the ground, the one standability rule, query_ground's atOrBelow,
// build_castle's fixes (units in the errors, keepHeight with keep false, gates as high as the wall, costOnly, packing
// fit), scale on every structure tool, sculpt_road, the drape's steepMaterial, unique shot names, the announcements,
// game_walk's keys and the factory's launch settings.
// Bundled for Node by test.ps1.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, type Loaded, type Scene } from "../src/core";
import { checkFactory } from "../src/lib/factory-check";
import { applyDrape, columnSlope, planDrape } from "../src/lib/heightfield-drape";
import { apply, plan } from "../src/lib/heightfield-plan";
import { marchRoad } from "../src/lib/road";
import { fitBoxes } from "../src/lib/structures-apply";
import { rotation } from "../src/lib/turns";
import { grids, isSolid, material } from "../src/lib/voxel";
import { validate } from "../src/registry";
import { tools as gameTools, uniqueName, walkKeys } from "../src/tools/game";
import { tools as inspectTools } from "../src/tools/inspect";
import { tools as placeTools } from "../src/tools/place";
import { tools as roadTools } from "../src/tools/road";
import { tools as structureTools } from "../src/tools/structures";
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
  return { store, s };
}
const all = [...inspectTools, ...structureTools, ...placeTools, ...gameTools, ...roadTools];
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

test("the worm rotation: teams alternate from team 0, each moves its worms in turn, a drowned worm is skipped", () => {
  const worms = [0, 1, 2, 3, 4, 5, 6, 7];
  const r = rotation({ worms, teams: 2, perTeam: 4, horizon: 16 });
  // the iteration-7 Test: 0/WORM0, 1/WORM4, 0/WORM1, 1/WORM5, ... (its monitor started in turn 2)
  assert.deepEqual(r.byTurn.slice(0, 8), [0, 4, 1, 5, 2, 6, 3, 7]);
  assert.deepEqual(r.own.get(4), [2, 10]);
  // WORM4 drowns at turn 5: team 1 moves 5, 6, 7 from then on, so WORM5 moves again sooner
  const d = rotation({ worms, teams: 2, perTeam: 4, horizon: 16, drownsAt: (w) => (w === 4 ? 5 : null) });
  assert.deepEqual(d.byTurn.slice(0, 12), [0, 4, 1, 5, 2, 6, 3, 7, 0, 5, 1, 6]);
  // an observed order is taken as given, repeated
  assert.deepEqual(rotation({ worms, teams: 2, perTeam: 4, horizon: 5, order: [4, 0] }).byTurn, [4, 0, 4, 0, 4]);
});

test("check_flood: the water at each spawn's first own turns, and the verdict with minOwnTurns, minDryMargin and turnOrder", async () => {
  const { store, s } = session();
  apply(store, plan(store, { min: [480, -12], max: [600, 12], floor: 0, expr: "2 + max(0, x + 20)*0.4", material: 1 }));
  // two teams of one: WORM0 low (ground 2), WORM1 high
  await call(s, "place_spawn_knots", { units: "map", spawns: [{ worm: 0, xz: [482, 0] }, { worm: 1, xz: [595, 0] }] });
  const base = { units: "map", start: 1, x0: 480, z0: -12, x1: 600, z1: 12, wormsPerTeam: 1, teams: 2 };
  // the sea reaches 2 at turn 4 (1 + 3 x 0.34): WORM0 moves in turns 1 and 3, so it is dry at both
  const sched = { startTurn: 2, step: 0.34, cap: 3, units: "map" };
  const ok = await call(s, "check_flood", { ...base, schedule: sched });
  const w0 = ok.spawns.find((x: { name: string }) => x.name === "WORM0");
  assert.deepEqual(w0.flood.moves.ownTurns.map((r: { gameTurn: number }) => r.gameTurn), [1, 3]);
  assert.equal(ok.floodVerdict.pass, true, JSON.stringify(ok.floodVerdict));
  assert.match(ok.summary, /flood PASS/);
  assert.match(ok.flood.turnIndex, /wum\.sim\.turnStarted's turn n/);
  assert.deepEqual(ok.flood.turnOrder.firstTurns.slice(0, 2), [{ turn: 1, worm: "WORM0" }, { turn: 2, worm: "WORM1" }]);
  // a margin of 1 map unit: at turn 3 the water is 1.68, 0.32 under the ground
  const margin = await call(s, "check_flood", { ...base, schedule: sched, minDryMargin: 1 });
  assert.equal(margin.floodVerdict.pass, false);
  assert.match(margin.floodVerdict.failures[0], /^WORM0: only 0\.32 over the water \(minDryMargin 1\) at its own turn 2 \(game turn 3/);
  assert.match(margin.summary, /flood FAIL: WORM0/);
  // three own turns: the third (turn 5) is under the water
  const three = await call(s, "check_flood", { ...base, schedule: sched, minOwnTurns: 3 });
  assert.match(three.floodVerdict.failures[0], /^WORM0: .* under the water at its own turn 3 \(game turn 5/);
  // an observed order where WORM0 moves second: its second own turn is 4, under the water
  const order = await call(s, "check_flood", { ...base, schedule: sched, turnOrder: [1, 0] });
  const o0 = order.spawns.find((x: { name: string }) => x.name === "WORM0");
  assert.deepEqual(o0.flood.moves.turns.slice(0, 2), [2, 4]);
  assert.equal(order.floodVerdict.pass, false);
  assert.match(order.flood.turnOrder.source, /turnOrder/);
});

test("place_spawn_knots: each worm's first and second own turns and the water then", async () => {
  const { store, s } = session();
  apply(store, plan(store, { min: [480, -12], max: [600, 12], floor: 0, expr: "4", material: 1 }));
  const r = await call(s, "place_spawn_knots", { units: "map", spawns: [{ worm: 0, xz: [490, 0] }, { worm: 4, xz: [500, 0] }] });
  const t1 = r.teams.teams.find((t: { team: number }) => t.team === 1);
  const w4 = t1.ownTurns.find((x: { worm: string }) => x.worm === "WORM4");
  assert.equal(w4.first.gameTurn, 2);
  assert.match(r.teams.note, /first and second own game turns/);
});

test("the mine factory: keepKnot with its knot on a voxel top gets no stale advisory", async () => {
  const { store, s } = session();
  apply(store, plan(store, { min: [480, -10], max: [520, 10], floor: 0, expr: "4", material: 1 }));
  const placed = await call(s, "place_object", { type: "minefactory", units: "map", pos: [500, 4, 0], snap: "surface" });
  const without = checkFactory(store, (v) => `${v} map`)!;
  assert.ok(without.warnings.some((w) => /missed undraped blocks/.test(w)), "without keepKnot the advisory stays");
  await call(s, "set_mine_factory", { keepKnot: true });
  const kept = checkFactory(store, (v) => `${v} map`)!;
  assert.deepEqual(kept.warnings, [], JSON.stringify(kept.warnings));
  // a knot 3 over the ground is not on it: the advisory is back
  await call(s, "move", { id: placed.placed[0].id, pos: [500, 7, 0], units: "map" });
  assert.ok(checkFactory(store, (v) => `${v} map`)!.warnings.some((w) => /missed undraped blocks/.test(w)));
});

test("one standability rule: a spawn on a thin wall-walk is a start for check_reachability, and a failing one is named", async () => {
  const { store, s } = session();
  apply(store, plan(store, { min: [480, -12], max: [560, 12], floor: 0, expr: "2", material: 1 }));
  // a wall one voxel thick at x 520.0-521.0, 6 high: its top is ground a worm can stand on
  await call(s, "build_wall", { points: [[520.5, 2, -6], [520.5, 2, 6]], height: 6, thickness: 1, crenellations: false, place: "new" });
  // the spawn sits at x 520.9, which the default grid (centres at .5 from the region's edge) can put in the air beside the wall
  await call(s, "place_spawn_knots", { units: "map", spawns: [{ worm: 0, pos: [520.9, 8, 0] }, { worm: 1, pos: [600, 30, 0] }] });
  const r = await call(s, "check_reachability", { units: "map", x0: 480.3, z0: -12, x1: 560, z1: 12, from: undefined });
  const w0 = r.spawns.find((x: { name: string }) => x.name === "WORM0");
  assert.ok(w0.standsOn !== undefined && Math.abs(w0.standsOn - 8) < 0.01, JSON.stringify(w0));
  const w1 = r.spawns.find((x: { name: string }) => x.name === "WORM1");
  assert.equal(w1.reachable, false);
  assert.match(r.summary, /not: WORM1 \(no ground under it/);
});

test("query_ground: mode atOrBelow answers the floor under a lintel, not its top", async () => {
  const { store, s } = session();
  apply(store, plan(store, { min: [480, -4], max: [500, 4], floor: 0, expr: "2", material: 1 }));
  // a lintel over x 489-491: solid from 6 to 8, air from 2 to 6
  await call(s, "build_wall", { points: [[490, 6, -3], [490, 6, 3]], height: 2, thickness: 2, crenellations: false, place: "new" });
  const top = await call(s, "query_ground", { x: 490, z: 0, units: "map" });
  assert.equal(top.standOn.map, 8);
  const floor = await call(s, "query_ground", { x: 490, z: 0, units: "map", y: 3, mode: "atOrBelow" });
  assert.equal(floor.standOn.map, 2);
  assert.match(floor.mode, /floor under y/);
  const inWall = await call(s, "query_ground", { x: 490, z: 0, units: "map", y: 7, mode: "atOrBelow" });
  assert.equal(inWall.ground, null);
  await assert.rejects(() => call(s, "query_ground", { x: 490, z: 0, mode: "atOrBelow" }), /give y/);
});

test("build_castle: units in the fit errors, keepHeight ignored with keep false, gates as high as the wall cut open, costOnly", async () => {
  const { s } = session();
  await assert.rejects(() => call(s, "build_castle", { at: [600, 0, 0], size: [8, 8], place: "new", dryRun: true }), /each side needs at least \d+(\.\d+)? map units/);
  await assert.rejects(() => call(s, "build_castle", { at: [12000, 0, 0], size: [160, 160], units: "world", place: "new", dryRun: true }), /world units/);
  const nk = await call(s, "build_castle", { at: [600, 0, 0], size: [24, 24], keep: false, keepHeight: 0, place: "new", dryRun: true });
  assert.ok(nk.newBlocks.length > 0);
  const tall = await call(s, "build_castle", { at: [600, 0, 0], size: [24, 24], wallHeight: 6, gateHeight: 6, keep: false, place: "new", dryRun: true });
  assert.ok(tall.notes.some((n: string) => /gate s: .* leaves no lintel/.test(n)), JSON.stringify(tall.notes));
  const cost = await call(s, "build_castle", { at: [600, 0, 0], size: [24, 24], keep: false, place: "new", costOnly: true, rotation: 45, costAt: [1.5] });
  assert.equal(cost.costOnly, true);
  const by = Object.fromEntries(cost.byPacking.map((p: { packing: string; cells: number }) => [p.packing, p.cells]));
  assert.ok(by.fit < by.tiles * 0.8, `fit ${by.fit} vs tiles ${by.tiles}: a rotated castle's boxes follow its walls`);
  assert.equal(cost.cheapest.packing === "fit" || cost.cheapest.packing === "parts", true);
  assert.equal(cost.costAt.length, 1);
  await assert.rejects(() => call(s, "build_castle", { at: [600, 0, 0], size: [24, 24], keep: false, place: "new", dryRun: true, maxCells: 100 }), /over maxCells 100/);
});

test("a gate as high as the wall leaves no merlon floating over it", async () => {
  const { store, s } = session();
  await call(s, "build_castle", { at: [600, 0, 0], size: [24, 24], wallHeight: 6, gateHeight: 6, keep: false, place: "new" });
  // over the gate's middle (south wall, x 600, z about 10.5) nothing is solid from the wall's top up
  const g = await call(s, "query_ground", { x: 600, z: 10.5, units: "map" });
  assert.ok(g.ground === null || g.standOn.map <= 0.01, JSON.stringify(g.spans));
  void store;
});

test("fitBoxes: a diagonal line of cells gets boxes that follow it", () => {
  const dim: [number, number, number] = [31, 1, 31], cells = new Uint8Array(31 * 31);
  for (let i = 0; i < 31; i++) cells[(i * 31 + i) * 1 + 0] = 1;
  const one = fitBoxes(cells, dim, 1);
  assert.equal(one.length, 1);
  const many = fitBoxes(cells, dim, 16);
  const vol = (b: { lo: number[]; hi: number[] }) => (b.hi[0] - b.lo[0] + 1) * (b.hi[1] - b.lo[1] + 1) * (b.hi[2] - b.lo[2] + 1);
  assert.ok(many.length <= 16 && many.reduce((n, b) => n + vol(b), 0) < 31 * 31 / 4, JSON.stringify(many.length));
});

test("scale is accepted by build_wall and build_tower as blockScale", async () => {
  const { s } = session();
  const a = await call(s, "build_tower", { at: [700, 0, 0], radius: 4, height: 12, hollow: false, place: "new", dryRun: true, scale: 2 });
  const b = await call(s, "build_tower", { at: [700, 0, 0], radius: 4, height: 12, hollow: false, place: "new", dryRun: true, blockScale: 2 });
  assert.deepEqual(a.cells, b.cells);
  assert.equal(a.newBlocks[0].scale, 2);
  await assert.rejects(() => call(s, "build_wall", { points: [[700, 0, 0], [710, 0, 0]], place: "new", dryRun: true, scale: 2, blockScale: 1.5 }), /same setting/);
});

test("marchRoad: a road up a cone keeps to its grade and winds round it", () => {
  const ground = (x: number, z: number) => Math.max(0, 40 - Math.hypot(x, z));   // a 45-degree cone 40 high
  const m = marchRoad(ground, [38, 0], [0, 0], { maxGrade: 15, maxLength: 1200 });
  assert.equal(m.stop, "arrived");
  assert.ok(m.steepest <= 15.01, String(m.steepest));
  assert.ok(m.length > 40 / Math.tan((15 * Math.PI) / 180) * 0.8, `it winds: ${m.length}`);
  const z = marchRoad(ground, [38, 0], [0, 0], { maxGrade: 15, switchbacks: 4, spread: 20 });
  assert.ok(z.steepest <= 15.01);
});

test("sculpt_road: grades a draped skin's corners to the road and answers the centreline", async () => {
  const { store, s } = session();
  applyDrape(store, planDrape(store, { min: [520, -30], max: [600, 30], floor: 0, expr: "max(1, 18 - 0.6*hypot(x, z))", voxelSize: 2, thickness: 2, material: 1, core: false }));
  const before = grids(store).filter((g) => g.frame.corners).map((g) => g.frame.corners!.join(",")).join("|");
  const r = await call(s, "sculpt_road", { from: [592, 0], to: [560, 0], maxGrade: 12, units: "map" });
  assert.ok(r.road.steepestDegrees <= 12.01);
  assert.ok(r.centreline.points.length > 2 && r.skins.graded > 0 && r.skins.cornersMoved > 0, JSON.stringify(r.skins));
  assert.ok(r.paintPath.points.length > 2 && r.paintPath.width === 4);
  const after = grids(store).filter((g) => g.frame.corners).map((g) => g.frame.corners!.join(",")).join("|");
  assert.notEqual(after, before);
  // the skin's top under the centreline is the road's height
  const mid = r.centreline.points[Math.floor(r.centreline.points.length / 2)];
  const q = await call(s, "query_ground", { x: mid[0], z: mid[2], units: "map" });
  assert.ok(Math.abs(q.standOn.map - mid[1]) < 0.6, `ground ${q.standOn.map} vs road ${mid[1]}`);
  store.undo();
  assert.equal(grids(store).filter((g) => g.frame.corners).map((g) => g.frame.corners!.join(",")).join("|"), before, "one undo step");
  // dryRun changes nothing
  await call(s, "sculpt_road", { from: [592, 0], to: [560, 0], maxGrade: 12, units: "map", dryRun: true });
  assert.equal(grids(store).filter((g) => g.frame.corners).map((g) => g.frame.corners!.join(",")).join("|"), before);
});

test("heightfield_drape steepMaterial: cliff columns take the rock material", () => {
  const { store } = session();
  const args = { min: [520, -20], max: [580, 20] as [number, number], floor: 0, expr: "select(x - 5, 30, 2)", voxelSize: 1, thickness: 2, material: 1, core: false };
  const p = planDrape(store, { ...args, min: [520, -20], steepMaterial: { material: 9, minSlope: 50 } });
  const st = p.report.steepMaterial as { columns: number };
  assert.ok(st.columns > 0, JSON.stringify(st));
  applyDrape(store, p);
  let rock = 0, other = 0;
  for (const g of grids(store).filter((x) => x.frame.corners)) for (const w of g.words) if (isSolid(w)) (material(w) === 9 ? rock++ : other++);
  assert.ok(rock > 0 && other > rock, `${rock} rock, ${other} other`);
  assert.ok(columnSlope([0, 0, 0, 0], 2, 0, 0) === 0 && Math.abs(columnSlope([0, 1, 0, 1], 2, 0, 0) - 45) < 1e-9);
});

test("shot names: always prefixed and unique in a process", () => {
  const a = uniqueName("calit8", "ov_n"), b = uniqueName("calit8", "ov_n"), c = uniqueName(undefined, "s");
  assert.equal(a, "calit8_ov_n");
  assert.equal(b, "calit8_ov_n_2");
  assert.match(c, /^erg_\d{8}_\d{6}_s$/);
  assert.equal(uniqueName("calit8", "calit8_x"), "calit8_x");
});

test("game_state: announcements from the log channel, and game_shots onAnnounce shoots while one is up", async () => {
  const now = Date.now();
  const rec = (text: string, ago: number, seconds: number) => ({ seq: 1, j: { msg: "announce", wall: new Date(now - ago).toISOString(), data: { text, mod: "erg:t", seconds } } });
  let frame = 100;
  const shots: any[] = [];
  const client = {
    has: (n: string) => n === "log",
    subscribe: (_c: string, _f: unknown, cb: (m: any) => void) => { setTimeout(() => { cb(rec("old news", 60000, 4)); cb(rec("The sea is rising!", 500, 6)); }, 5); return () => undefined; },
    async call(m: string, p: any) {
      frame += 3;
      if (m === "lua.eval") return { ok: true, text: "scene=match;turnsStarted=5;turn=4;team=0;worm=2;worms=8;aliveByTeam=0:4,1:4;tick=99" };
      if (m === "render.camera") return p.clear ? { cleared: true, frame } : { set: true, frame, frames: p.frames };
      if (m === "render.shot") { shots.push(p); return { ready: true, path: "x.png", width: 640, height: 360, frame, clean: !!p.clean }; }
      throw new Error(`unexpected ${m}`);
    },
  };
  const { s } = session({ ensure: async () => ({ methods: ["lua.eval", "render.camera", "render.shot"] }), client: async () => client });
  const st = await call(s, "game_state", {});
  assert.equal(st.turn, 5, "turn is turnsStarted, as wum.sim.turnStarted counts");
  assert.equal(st.clientTurnIndex, 4);
  assert.deepEqual(st.announcements.onScreen.map((a: { text: string }) => a.text), ["The sea is rising!"]);
  assert.equal(st.announcements.recent.length, 2);
  // shots wait for a matching announcement and are not clean
  await assert.rejects(() => call(s, "game_shots", { cams: [{ pos: [0, 5, 9], target: [0, 0, 0] }], onAnnounce: { match: "volcano", timeoutSec: 1 } }), /no announcement matching \/volcano\/i/);
});

test("game_walk's keys: camera-relative, the nearest of eight", () => {
  // the camera looking along -z (as in the Test): W walks -z, D +x, S +z, A -x
  assert.deepEqual(walkKeys(0, -10, 0, -1), ["W"]);
  assert.deepEqual(walkKeys(10, 0, 0, -1), ["D"]);
  assert.deepEqual(walkKeys(0, 10, 0, -1), ["S"]);
  assert.deepEqual(walkKeys(-10, 0, 0, -1), ["A"]);
  assert.deepEqual(walkKeys(7, -7, 0, -1), ["W", "D"]);
  // the camera turned to look along +x: W walks +x
  assert.deepEqual(walkKeys(10, 0.5, 1, 0), ["W"]);
});

test("set_mine_factory: launchHeight and launchRadius reach the patch; launch false clears them", async () => {
  const { store, s } = session();
  apply(store, plan(store, { min: [480, -10], max: [520, 10], floor: 0, expr: "4", material: 1 }));
  await call(s, "place_object", { type: "minefactory", units: "map", pos: [500, 4, 0], snap: "surface", launchHeight: 25, launchRadius: 8 });
  const spec = () => (store.scene.objects!.find((o) => o.type === "minefactory") as { factory?: Record<string, unknown> }).factory;
  assert.deepEqual(spec(), { launchHeight: 25, launchRadius: 8 });
  await call(s, "set_mine_factory", { launchHeight: 0 });
  assert.deepEqual(spec(), { launchRadius: 8 });
  await call(s, "set_mine_factory", { launch: false });
  assert.equal(spec(), undefined);
  await assert.rejects(() => call(s, "set_mine_factory", { launch: true }), /launch: false clears/);
});
