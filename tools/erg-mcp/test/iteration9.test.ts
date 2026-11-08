// The Caldera iteration-9 gaps, on the synthetic 12-frame level (its root renamed Scene so blocks can be added), away
// from its own frames (x 480-700) where a test builds ground: base frames trimmed and removed out of the cell count
// (trim_base_frames, remove_base_frames, the patch's trimFrame), the worm's body in check_reachability (narrowest gap,
// bodyWidth), game_walk's route planning and pure pursuit, check_flood by round, check_holes and the factory's hole
// warning, the mine fates in the fire records, and the overview's island fit (set_overview).
// Bundled for Node by test.ps1.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, voxelCells, type Loaded, type Scene } from "../src/core";
import { checkFactory } from "../src/lib/factory-check";
import { apply, plan } from "../src/lib/heightfield-plan";
import { cumulative, keyOfLabel, planWalk, pursue } from "../src/lib/walk-plan";
import { grids, limitProblems } from "../src/lib/voxel";
import { validate } from "../src/registry";
import { fireOut, fitDistance, tools as gameTools } from "../src/tools/game";
import { tools as inspectTools } from "../src/tools/inspect";
import { tools as placeTools } from "../src/tools/place";
import { tools as projectTools } from "../src/tools/project";
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
  const project = { store, id: "t", info: { id: "t" }, erg: { script: async () => "" } };
  const s = { requireProject: () => project, get store() { return store; }, project,
    levels: async () => { throw new Error("no server"); }, client: async () => { throw new Error("no server"); }, ...extra } as unknown as Session;
  return { store, s };
}
const all = [...inspectTools, ...structureTools, ...placeTools, ...gameTools, ...terrainTools, ...projectTools];
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

test("remove_base_frames: the frames' voxels emptied and each trimmed to one cell, one undo step, a trimFrame op each", async () => {
  const { store, s } = session();
  const before = voxelCells(store.scene);
  const dry = await call(s, "remove_base_frames", { frames: [6, 10], dryRun: true });
  assert.equal(dry.cellsFreed, 7 * 9 * 12 - 1 + 10 * 10 * 12 - 1);
  assert.equal(voxelCells(store.scene), before, "a dry run changes nothing");
  const r = await call(s, "remove_base_frames", { frames: [6, 10] });
  assert.equal(r.trimmed, 2);
  assert.equal(voxelCells(store.scene), before - dry.cellsFreed);
  assert.ok(grids(store).filter((g) => g.id === 6 || g.id === 10).every((g) => g.words.every((w) => w === 0)), "every word emptied");
  const p = store.patch();
  assert.equal(p.format, "erg-patch/2");
  assert.deepEqual(p.ops.filter((o) => o.op === "trimFrame").map((o) => (o as { frame: number }).frame).sort(), [10, 6]);
  assert.deepEqual(limitProblems(store), []);
  const d = await call(s, "describe_level", { limit: 20 });
  assert.equal(d.frames.find((f: { id: number }) => f.id === 10).trim.cells, 1);
  await call(s, "undo", {});
  assert.equal(voxelCells(store.scene), before);
  assert.equal(store.scene.trims, undefined);
  assert.ok(grids(store).find((g) => g.id === 6)!.words.some((w) => (w & 3) === 3), "undo puts the voxels back");
  assert.equal(store.patch().format, "erg-patch/1");
});

test("trim_base_frames: empty frames to a cell, others to their solid box; a fill outside the box is a problem until trimmed again", async () => {
  const { store, s } = session();
  const before = voxelCells(store.scene);
  await call(s, "clear_base_terrain", { frames: [7], wipe: true });
  const empty = await call(s, "trim_base_frames", { all: true, keep: "empty" });
  assert.equal(empty.trimmed, 1);
  assert.equal(empty.frames[0].id, 7);
  assert.ok(empty.skipped.some((k: { why: string }) => /solid voxels/.test(k.why)));
  assert.equal(voxelCells(store.scene), before - (11 * 2 * 6 - 1));
  // land2 (7x9x12) cut to its top layers: carve its lower half, then trim it to the box of what is left
  await call(s, "fill_box_raw", { frame: 6, min: [0, 0, 0], max: [7, 4, 12], state: "empty" });
  const tight = await call(s, "trim_base_frames", { frames: [6] });
  assert.equal(tight.trimmed, 1);
  const t6 = store.scene.trims!.find((t) => t.frame === 6)!;
  assert.ok(t6.min[1] >= 4 && t6.size[1] <= 5, JSON.stringify(t6));
  assert.equal(tight.frames[0].moves, true);
  assert.deepEqual(limitProblems(store), []);
  await call(s, "fill_box_raw", { frame: 6, min: [0, 0, 0], max: [1, 1, 1], state: "solid", material: 2 });
  assert.ok(limitProblems(store).some((p) => /outside its kept box/.test(p)), limitProblems(store).join("; "));
  await call(s, "trim_base_frames", { frames: [6] });
  assert.deepEqual(limitProblems(store), []);
  const cleared = await call(s, "trim_base_frames", { clear: true });
  assert.deepEqual(cleared.cleared.sort(), [6, 7]);
  assert.equal(voxelCells(store.scene), before);
  await assert.rejects(() => call(s, "trim_base_frames", {}), /give frames, search or box/);
});

/** Flat ground at 2 over x 480-560, z -12..12, and a wall across it at x 520-521 with a slot in it at z -1..1. */
async function slotted(s: Session, store: EditorStore) {
  apply(store, plan(store, { min: [480, -12], max: [560, 12], floor: 0, expr: "2", material: 1 }));
  await call(s, "build_wall", { points: [[520.5, 2, -12], [520.5, 2, -1]], height: 6, thickness: 1, crenellations: false, place: "new" });
  await call(s, "build_wall", { points: [[520.5, 2, 1], [520.5, 2, 12]], height: 6, thickness: 1, crenellations: false, place: "new" });
}

test("check_reachability: every route reports its narrowest gap; bodyWidth refuses a slot narrower than the worm", async () => {
  const { store, s } = session();
  await slotted(s, store);
  const base = { units: "map", x0: 480, z0: -12, x1: 560, z1: 12, from: [[500, 2, 0]], target: { x: 545, z: 0, radius: 2 } };
  const open = await call(s, "check_reachability", { ...base, route: true });
  const sp = open.spawns[0];
  assert.equal(sp.reachable, true, JSON.stringify(sp));
  assert.ok(sp.path.narrowest && sp.path.narrowest.width <= 2.5, JSON.stringify(sp.path));
  assert.ok(Math.abs(sp.path.narrowest.at[0] - 520.5) <= 1.5, JSON.stringify(sp.path.narrowest));
  assert.ok(sp.route.length > 30 && sp.route.every((p: number[]) => p.length === 3), "route: the walk column by column");
  assert.equal(open.rules.body.width, null);
  const wide = await call(s, "check_reachability", { ...base, bodyWidth: 3 });
  assert.equal(wide.spawns[0].reachable, false);
  assert.equal(wide.rules.body.width, 3);
  const thin = await call(s, "check_reachability", { ...base, bodyWidth: 0.8 });
  assert.equal(thin.spawns[0].reachable, true);
  // a gap under the assumed worm width is named in the summary when the body is not enforced
  const tight = await call(s, "check_reachability", { ...base, step: 0.5 });
  if (tight.spawns[0].path.narrowest.width < 1) assert.match(tight.summary, /narrower than a worm/);
});

test("game_walk's route: planned through the slot for a thin worm, refused for a wide one; pure pursuit follows the corners", async () => {
  const { store, s } = session();
  await slotted(s, store);
  const ok = planWalk(store, [500, 2, 0], [545, 0], { bodyWidth: 0.8 });
  assert.ok("plan" in ok, JSON.stringify(ok));
  if ("plan" in ok) {
    const at = ok.plan.path.find((p) => Math.abs(p[0] - 520.5) < 0.6);
    assert.ok(at && Math.abs(at[2]) <= 1.5, `the route goes through the slot: ${JSON.stringify(ok.plan.path.map((p) => p.map((v) => Math.round(v * 10) / 10)))}`);
  }
  const no = planWalk(store, [500, 2, 0], [545, 0], { bodyWidth: 3 });
  assert.ok("why" in no, JSON.stringify("plan" in no ? no.plan.path.filter((_, i) => i % 3 === 0).map((p) => p.map((v) => Math.round(v * 10) / 10)) : no));
  // an L: along x, then along z; near the corner the aim is already round it
  const pts = [[0, 0], [10, 0], [10, 10]], cum = cumulative(pts);
  assert.deepEqual(cum, [0, 10, 20]);
  const p1 = pursue(pts, cum, 0, [9, 0.2], 2, 8);
  assert.equal(p1.seg, 0);
  assert.ok(Math.abs(p1.aim[0] - 10) < 1e-9 && Math.abs(p1.aim[1] - 1) < 1e-6, JSON.stringify(p1));
  const p2 = pursue(pts, cum, p1.seg, [10.3, 5], 2, 8);
  assert.equal(p2.seg, 1);
  assert.ok(Math.abs(p2.aim[1] - 7) < 1e-6);
  assert.equal(keyOfLabel("Enter"), "RETURN");
  assert.equal(keyOfLabel("Backspace"), "BACK");
  assert.equal(keyOfLabel("Space"), "SPACE");
  assert.equal(keyOfLabel("LMB"), null);
  await assert.rejects(() => call(s, "game_walk", { waypoints: [[0, 0]], to: [1, 1], gameDir: "C:/nowhere" }), /one of waypoints, path or to/);
});

test("check_flood: the water by round, round r from game turn (r - 1) x teams + 1", async () => {
  const { store, s } = session();
  apply(store, plan(store, { min: [480, -12], max: [560, 12], floor: 0, expr: "4", material: 1 }));
  await call(s, "place_spawn_knots", { units: "map", spawns: [{ worm: 0, xz: [490, 0] }, { worm: 4, xz: [500, 0] }] });
  await call(s, "set_water", { level: 0 });
  const r = await call(s, "check_flood", { units: "map", x0: 480, z0: -12, x1: 560, z1: 12, rounds: [{ round: 1, level: 0 }, { round: 3, level: 1 }, { round: 6, level: 3 }] });
  assert.deepEqual(r.flood.levels, [{ turn: 1, level: 0 }, { turn: 5, level: 1 }, { turn: 11, level: 3 }]);
  assert.match(r.flood.roundTurns, /teams 2/);
  const three = await call(s, "check_flood", { units: "map", x0: 480, z0: -12, x1: 560, z1: 12, teams: 3, rounds: [{ round: 3, level: 1 }] });
  assert.deepEqual(three.flood.levels, [{ turn: 7, level: 1 }]);
  await assert.rejects(() => call(s, "check_flood", { units: "map", rounds: [{ round: 1, level: 0 }], levels: [{ turn: 1, level: 0 }] }), /one of schedule/);
});

test("check_holes: rays that hit no ground over the water, grouped, and the factory's warning over them", async () => {
  const { store, s } = session();
  apply(store, plan(store, { min: [480, -12], max: [520, 12], floor: 0, expr: "3", material: 1 }));
  await call(s, "set_water", { level: 20 });
  const solid = await call(s, "check_holes", { units: "map", around: { x: 500, z: 0, radius: 6 } });
  assert.equal(solid.ok, true, JSON.stringify(solid));
  // a 2 x 2 shaft through the field at x 500-502, z 0-2
  const g = grids(store).filter((x) => x.frame.new && x.bounds.min[0] <= 500 && x.bounds.max[0] >= 502 && x.bounds.min[2] <= 0 && x.bounds.max[2] >= 2);
  assert.ok(g.length, "a block under the shaft");
  await call(s, "fill_box_raw", { units: "map", frame: g[0].id, min: [500, -10, 0], max: [502, 10, 2], state: "empty" });
  const r = await call(s, "check_holes", { units: "map", around: { x: 500, z: 0, radius: 6 } });
  assert.equal(r.ok, false);
  assert.equal(r.holes.groupCount, 1);
  assert.ok(Math.abs(r.holes.groups[0].centre[0] - 501) < 0.6 && Math.abs(r.holes.groups[0].centre[1] - 1) < 0.6, JSON.stringify(r.holes));
  assert.equal(r.holes.groups[0].area, 4);
  assert.equal(r.holes.groups[0].through, "void");
  await call(s, "place_object", { type: "minefactory", units: "map", pos: [497, 3, 0], snap: "surface" });
  const fc = checkFactory(store, (v) => `${v} map`)!;
  assert.ok(fc.warnings.some((w) => /see-through holes/.test(w)), JSON.stringify(fc.warnings));
});

test("fire records: each mine's fate and where it ended up", () => {
  const out = fireOut({ stem: "x", tick: 10, turn: 3, asked: 3, launch: true, factory: [1, 2, 3], mines: [
    { handle: 1, spawn: [1, 20, 3], rest: [2, 2, 3], settled: true, restFromFactoryMap: 1, fate: "resting", moves: 2, movedAfterRest: 1 },
    { handle: 2, spawn: [1, 20, 3], gone: true, lastSeen: [5, -1, 3], restFromFactoryMap: 4, fate: "drowned", goneTick: 99, waterMap: 0.5, moves: 1, movedAfterRest: 0 },
    { handle: 3, spawn: [1, 20, 3], gone: true, lastSeen: [1, 2, 4], restFromFactoryMap: 1, fate: "destroyed", goneTick: 120, moves: 1 } ] }, "map");
  assert.deepEqual(out.fates, { resting: 1, drowned: 1, destroyed: 1 });
  assert.equal(out.each[0].movedAfterRest, 1);
  assert.equal(out.each[1].fate, "drowned");
  assert.equal(out.each[1].water, 0.5);
  // a mine gone before it landed: the ground under its launch point says whether it fell into the sea
  const flew = fireOut({ stem: "x", tick: 1, turn: 1, asked: 2, launch: true, factory: [0, 0, 0], mines: [
    { handle: 1, spawn: [50, 20, 0], gone: true, lastSeen: [50, 20, 0], restFromFactoryMap: 50, fate: "gone before it landed", goneTick: 5 },
    { handle: 2, spawn: [1, 20, 0], gone: true, lastSeen: [1, 20, 0], restFromFactoryMap: 1, fate: "gone before it landed", goneTick: 5 } ] }, "map",
    (x) => (x > 10 ? null : 3), 0);
  assert.match(String((flew.each[0] as { likely?: string }).likely), /^drowned/);
  assert.match(String((flew.each[1] as { likely?: string }).likely), /set off before it landed/);
  // a record from an older game has no fates
  const old = fireOut({ stem: "x", tick: 1, turn: 1, asked: 1, launch: false, factory: [0, 0, 0], mines: [{ handle: 1, spawn: [0, 9, 0], rest: [0, 1, 0], settled: true, restFromFactoryMap: 0 }] }, "map");
  assert.equal((old as { fates?: unknown }).fates, undefined);
});

test("the overview's island fit: framed on the solid ground over the water, closer than the frames' box; set_overview keeps it", async () => {
  const { store, s } = session();
  // the island alone: the base's terrain cleared (its empty frames' boxes still count for fit frames)
  await call(s, "clear_base_terrain", { all: true, wipe: true });
  apply(store, plan(store, { min: [600, -10], max: [620, 10], floor: 0, expr: "6", material: 1 }));
  await call(s, "set_water", { level: 20 });
  // a box 20 wide seen from 50 degrees in a 60 degree 16:9 view
  const d = fitDistance({ min: [-10, 0, -10], max: [10, 6, 10] }, Math.PI, (50 * Math.PI) / 180);
  assert.ok(d > 15 && d < 40, String(d));
  const r = await call(s, "set_overview", { fit: "solid" });
  assert.equal(r.overview.fit, "solid");
  const island = await call(s, "set_overview", {});
  assert.equal(island.overview.fit, "island");
  assert.ok(Math.abs(island.pose.target[0] - 610) < 1 && Math.abs(island.pose.target[2]) < 1, JSON.stringify(island.pose));
  const span = Math.hypot(island.pose.pos[0] - island.pose.target[0], island.pose.pos[1] - island.pose.target[1], island.pose.pos[2] - island.pose.target[2]);
  assert.ok(span < 60, `the island fills the view: ${span}`);
  const cleared = await call(s, "set_overview", { clear: true });
  const far = Math.hypot(cleared.pose.pos[0] - cleared.pose.target[0], cleared.pose.pos[1] - cleared.pose.target[1], cleared.pose.pos[2] - cleared.pose.target[2]);
  assert.ok(far > span, "the frames' overview takes in the whole level");
});
