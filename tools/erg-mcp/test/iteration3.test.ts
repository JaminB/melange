// The Caldera iteration-3 gaps, on the synthetic 12-frame level (its root renamed Scene so blocks can be added), away
// from its own frames (x 480-600): stairs judged over their treads, turn counts, the flood check, detached terrain,
// empty and hollow blocks, surface paint over several frames, material rules by region, mask and noise, fitBudget,
// sculpt_cone, fractional brushes and the scenery helpers. Bundled for Node by test.ps1.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, isSolid, material, type Loaded, type Scene } from "../src/core";
import { coneExpr } from "../src/lib/cone";
import { scheduleFromScript, waterByTurn } from "../src/lib/flood";
import { compile } from "../src/lib/heightfield-expr";
import { apply, plan, planFit } from "../src/lib/heightfield-plan";
import { pieces } from "../src/lib/islets";
import { search, standing, surfaces, type ReachOptions } from "../src/lib/reach";
import { grid, grids, limitProblems } from "../src/lib/voxel";
import { tools as coneTools } from "../src/tools/cone";
import { tools as inspectTools } from "../src/tools/inspect";
import { kindOf, scaleOf } from "../src/tools/scenery";
import { tools as terrainTools } from "../src/tools/terrain";
import type { Session } from "../src/session";

function loaded(): Loaded {
  const s: Scene = JSON.parse(readFileSync(new URL("../../../../tests/fixtures/erg/synthetic-12.json", import.meta.url), "utf8"));
  s.frames.find((f) => f.id === 1)!.name = "Scene";
  const blobs = new Map<number, ArrayBuffer>();
  for (const [ref, buf] of syntheticBlobs(s) as Map<number, Uint8Array>) blobs.set(ref, buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength) as ArrayBuffer);
  return { scene: s, blobs };
}
function session(script = "") {
  const store = new EditorStore("t", loaded(), loaded());
  const project = { store, id: "t", info: {}, erg: { script: async () => script } };
  const s = { requireProject: () => project, get store() { return store; }, project,
    levels: async () => { throw new Error("no server"); }, client: async () => { throw new Error("no server"); } } as unknown as Session;
  return { store, s };
}
const all = [...inspectTools, ...terrainTools, ...coneTools];
const call = async (s: Session, name: string, args: Record<string, unknown>): Promise<any> => {
  const r = await all.find((t) => t.name === name)!.handler(s, args);
  return typeof r === "string" ? JSON.parse(r) : r;
};
const R = { x0: 480, z0: -10, x1: 600, z1: 10, units: "map" };
/** A strip along x from 480 to 600, 20 deep, of height expr (map units). */
function strip(expr: string, voxelSize = 1) {
  const t = session();
  apply(t.store, plan(t.store, { min: [480, -10], max: [600, 10], floor: 0, expr, material: 1, voxelSize }));
  return t;
}
const o: ReachOptions = { step: 1, maxStep: 0.5, maxSlope: 30, maxDrop: 8, maxGap: 0, headroom: 1.5, water: null };

test("check_reachability: a voxel staircase is judged over its treads, not each riser", async () => {
  // risers of one voxel every 3: about 18 degrees on average, every riser alone 45
  const { store, s } = strip("1 + floor(max(0, x + 50) / 3)");
  const S = surfaces(grids(store), R, 1, 1.5);
  const top = (opts: ReachOptions) => { const q = search(S, standing(S, [485, 3, 0], opts), opts); let b = -Infinity; for (let n = 0; n < S.nodes; n++) if (q.reached[n]) b = Math.max(b, S.y[n]); return b; };
  assert.ok(top(o) >= 30, "tread: walks to the top at maxSlope 30");
  assert.ok(top({ ...o, slopeOver: "riser" }) < 5, "riser: the first riser stops it (each riser is 45 degrees)");
  const r = await call(s, "check_reachability", { ...R, from: [[485, 3, 0]], maxSlope: 30 });
  assert.equal(r.spawns[0].reachable, true, JSON.stringify(r.spawns[0]));
  assert.ok(r.spawns[0].path.maxStairSlope <= 30 && r.spawns[0].path.maxSlope >= 44, JSON.stringify(r.spawns[0].path));
  assert.equal(r.rules.slopeOver, "tread");
  // the least turns: the walk over walkSpeed x turnTime
  const t = await call(s, "check_reachability", { ...R, from: [[485, 3, 0]], maxSlope: 30, walkSpeed: 1, turnTime: 10 });
  assert.equal(t.spawns[0].turns.ownTurns, Math.ceil(t.spawns[0].walk / 10));
  // a riser of two voxels is not a stair step at any slope
  const { s: s2 } = strip("1 + 2*floor(max(0, x + 50) / 6)");
  const r2 = await call(s2, "check_reachability", { ...R, from: [[485, 3, 0]], maxSlope: 60 });
  assert.equal(r2.spawns[0].reachable, false);
  // riser keeps the old rule
  const r3 = await call(s, "check_reachability", { ...R, from: [[485, 3, 0]], maxSlope: 30, slopeOver: "riser" });
  assert.equal(r3.spawns[0].reachable, false);
});

test("flood schedules: wum.sim.waterSchedule's semantics, levels by turn, and the script's own call", () => {
  const w = waterByTurn(0, { schedule: { startTurn: 3, step: 1, cap: 4, units: "map" } });
  assert.deepEqual([1, 2, 3, 4, 5, 6, 7, 9].map(w), [0, 0, 1, 2, 3, 4, 4, 4]);
  const e = waterByTurn(-1, { schedule: { startTurn: 2, every: 2, step: 20, cap: 100, from: 40 } });   // world units
  assert.deepEqual([1, 2, 3, 4, 6].map(e), [-1, 2, 2, 3, 4]);
  const l = waterByTurn(0, { levels: [{ turn: 5, level: 3 }, { turn: 2, level: 1 }] });
  assert.deepEqual([1, 2, 4, 5, 9].map(l), [0, 1, 1, 3, 3]);
  const sc = scheduleFromScript("-- a comment wum.sim.waterSchedule{step = 9}\nwum.sim.waterSchedule{ startTurn = 3, step = 1, cap = 8, units = \"map\" }");
  assert.deepEqual(sc, { startTurn: 3, step: 1, cap: 8, units: "map" });
  assert.equal(scheduleFromScript("print(1)"), null);
  assert.throws(() => scheduleFromScript("wum.sim.waterSchedule{ step = S, cap = 8 }"), /not a literal/);
});

test("check_flood: the turn a spawn goes under, and whether a dry walk gets to the summit in time", async () => {
  const { store, s } = strip("1 + 0.2*max(0, x + 40)");      // a ramp from y 1 at x 500 up to 21 at x 600
  store.scene.water.level = 0;
  // a slow sea: the low spawn walks up ahead of it
  const slow = await call(s, "check_flood", { ...R, from: [[505, 3, 0]], schedule: { step: 0.5, cap: 30, units: "map" }, walkSpeed: 1, turnTime: 20 });
  const a = slow.spawns[0];
  assert.equal(a.flood.escapes, true, JSON.stringify(a));
  assert.ok(a.flood.floodsAtTurn > 1 && a.flood.arrivesTurn >= 1, JSON.stringify(a.flood));
  assert.match(slow.summary, /1 of 1 reach it dry/);
  // a fast sea: under in two turns, the walk takes several
  const fast = await call(s, "check_flood", { ...R, from: [[505, 3, 0]], schedule: { step: 6, cap: 40, units: "map" }, walkSpeed: 1, turnTime: 10 });
  assert.equal(fast.spawns[0].flood.escapes, false, JSON.stringify(fast.spawns[0]));
  assert.ok(fast.spawns[0].flood.floodsAtTurn <= 2, JSON.stringify(fast.spawns[0].flood));
  // the project's script's schedule
  const t = session("wum.sim.waterSchedule{ startTurn = 1, step = 0.5, cap = 30, units = \"map\" }");
  apply(t.store, plan(t.store, { min: [480, -10], max: [600, 10], floor: 0, expr: "1 + 0.2*max(0, x + 40)", material: 1 }));
  t.store.scene.water.level = 0;
  const fromScript = await call(t.s, "check_reachability", { ...R, from: [[505, 3, 0]], flood: { schedule: "script" }, walkSpeed: 1, turnTime: 20 });
  assert.equal(fromScript.flood.from, "the project's script");
  assert.equal(fromScript.spawns[0].flood.escapes, true);
  await assert.rejects(() => call(s, "check_flood", { ...R, from: [[505, 3, 0]], schedule: { step: 1 } }), /step and cap/);
});

test("check_islets: the main landmass and the detached pieces, smallest first", async () => {
  const { store, s } = strip("max(0, 6 - abs(x)*0.2)");
  await call(s, "add_block", { size: [2, 2, 2], material: 3, at: [595, 0, 8], units: "map" });     // a speck off the coast
  await call(s, "add_block", { size: [3, 3, 3], material: 3, at: [540, 30, 0], units: "map" });    // floating debris
  const ps = pieces(grids(store), R, 1);
  assert.ok(ps.length >= 3 && ps[0].volume > ps[1].volume);
  const r = await call(s, "check_islets", { ...R });
  assert.ok(r.detached >= 2, JSON.stringify(r));
  assert.equal(r.list[0].volume, 8);
  assert.equal(r.list[0].debris, true);
  const c = await call(s, "check_reachability", { ...R, from: [[540, 7, 0]], islets: true });
  assert.ok(c.islets.detached >= 2);
});

test("add_block: fill empty and hollow, frameId at the top level", async () => {
  const { store, s } = session();
  const e = await call(s, "add_block", { size: [5, 4, 5], material: 2, at: [520, 0, 0], units: "map", fill: "empty" });
  assert.equal(typeof e.frameId, "number");
  assert.equal(e.frameId, e.id);
  assert.equal(e.voxels, 0);
  assert.ok(grid(store, e.frameId).words.every((w) => !isSolid(w)));
  const h = await call(s, "add_block", { size: [5, 5, 5], material: 2, at: [540, 0, 0], units: "map", fill: "hollow" });
  assert.equal(h.voxels, 125 - 27);
  const d = await call(s, "add_block", { size: [5, 5, 5], material: 2, at: [560, 0, 0], units: "map", fill: "hollow", shell: 2, dryRun: true });
  assert.equal(d.voxels, 125 - 1);
  // a sculpt fill anchored on the empty block fills it
  await call(s, "sculpt", { mode: "fill", size: [3, 3, 3], center: [520.5, 1.5, 0.5], material: 4, units: "map", frame: e.frameId });
  assert.ok(grid(store, e.frameId).words.some(isSolid));
  assert.deepEqual(limitProblems(store), []);
  // fractional brush sizes
  const f = await call(s, "sculpt", { mode: "carve", size: [2.5, 2.5, 2.5], center: [540.5, 4.5, 0.5], units: "map", dryRun: true });
  assert.ok(f.changed > 0);
});

test("paint_path and paint_region: the surface over several frames, nothing painted over air", async () => {
  const { store, s } = session();
  const a = await call(s, "add_block", { size: [10, 3, 6], material: 2, at: [505, 0, 0], units: "map" });
  const b = await call(s, "add_block", { size: [10, 5, 6], material: 2, at: [515, 0, 0], units: "map" });
  // a road from one block across a gap of air onto the other: the top voxel of each, y not used
  const p = await call(s, "paint_path", { material: 9, points: [[501, 0], [530, 0]], width: 2, units: "map" });
  const ids = p.frames.map((x: { id: number }) => x.id).sort();
  assert.deepEqual(ids, [a.frameId, b.frameId].sort());
  const top = (id: number) => { const g = grid(store, id); let painted = 0, deep = 0; g.words.forEach((w, i) => { if (isSolid(w) && material(w) === 9) { if (i % g.size[1] === g.size[1] - 1) painted++; else deep++; } }); return { painted, deep }; };
  assert.ok(top(a.frameId).painted > 0 && top(a.frameId).deep === 0);
  assert.ok(top(b.frameId).painted > 0 && top(b.frameId).deep === 0);
  // a mask by height: only the taller block's top is above 4
  const r = await call(s, "paint_region", { material: 11, shapes: [{ min: [495, -5], max: [525, 5] }], mask: "h - 4", units: "map" });
  assert.deepEqual(r.frames.map((x: { id: number }) => x.id), [b.frameId]);
  await assert.rejects(() => call(s, "paint_region", { material: 11, shapes: [{ x: 700, z: 700, radius: 3 }], units: "map" }), /no voxel frame|nothing/);
  await assert.rejects(() => call(s, "paint_region", { material: 11, mask: "h +", units: "map" }), /mask/);
  assert.deepEqual(limitProblems(store), []);
});

test("heightfield materials: region, mask and noise terms", () => {
  const { store } = session();
  const p = plan(store, { min: [480, -16], max: [544, 16], floor: 0, expr: "4", material: 1, materials: [
    { material: 5, region: [{ x: 500, z: 0, radius: 6 }] },
    { material: 6, mask: "x - 20" },                 // x from the rectangle's centre (512)
    { material: 7, noise: { threshold: 0.2, scale: 6, seed: 3 } },
  ] });
  const f = p.field;
  const at = (mx: number, mz: number) => f.materialAt(Math.floor(mz - (f.j0 + f.t[2])) * f.nx + Math.floor(mx - (f.i0 + f.t[0])), f.floor + 1);
  assert.equal(at(500.5, 0.5), 5);
  assert.equal(at(540.5, 0.5), 6);
  const seen = new Set<number>();
  for (let z = -15; z < 15; z += 1) for (let x = 481; x < 528; x += 1) seen.add(at(x + 0.5, z + 0.5));
  assert.ok(seen.has(7) && seen.has(1), [...seen].join());
  assert.throws(() => plan(store, { min: [480, -16], max: [544, 16], floor: 0, expr: "4", materials: [{ material: 6, mask: "q" }] }), /mask: unknown name/);
  assert.ok(compile("h + y", ["h", "y"])({ h: 2, y: 3 } as never) === 5);
});

test("fitBudget: a coarser voxel size instead of halving into every block", () => {
  const { store } = session();
  const a = { min: [480, -60] as [number, number], max: [600, 60] as [number, number], floor: 0, expr: "max(0, 30 - r*0.4)", material: 1, maxBlocks: 6 };
  const p = plan(store, a);
  assert.ok(!(p.fits && !p.dropped.length), "too many blocks at voxelSize 1");
  const q = planFit(store, { ...a, fitBudget: true });
  const fitted = q.report.fitted as { voxelSize: number; tried: unknown[] };
  assert.ok(q.fits && !q.dropped.length && fitted.voxelSize > 1 && q.kept.length <= 6, JSON.stringify(fitted));
  assert.equal((planFit(store, a).report as { fitted?: unknown }).fitted, undefined);
});

test("sculpt_cone: profiles, a wandering outline, a crater; smooth or stepped", async () => {
  const { expr } = coneExpr({ at: [0, 0], floor: 0, radius: 20, height: 10, profile: [[0, 1], [0.5, 0.6], [1, 0]] });
  const f = compile(expr);
  const h = (r: number) => f({ x: r, z: 0, u: 0, v: 0, r, theta: 0, W: 0, D: 0, R: 0, pi: Math.PI, e: Math.E });
  assert.ok(Math.abs(h(0) - 10) < 1e-6 && Math.abs(h(10) - 6) < 1e-6 && Math.abs(h(15) - 3) < 1e-6 && h(21) === 0);
  const c = compile(coneExpr({ at: [0, 0], floor: 0, radius: 20, height: 10, profile: "linear", crater: { radius: 5, depth: 4 } }).expr);
  const hc = (r: number) => c({ x: r, z: 0, u: 0, v: 0, r, theta: 0, W: 0, D: 0, R: 0, pi: Math.PI, e: Math.E });
  assert.ok(hc(0) < hc(5) && Math.abs(hc(0) - 6) < 1e-6);
  const { store, s } = session();
  const dry = await call(s, "sculpt_cone", { at: [540, 0], floor: 0, radius: 24, height: 14, angleNoise: { amount: 0.25 }, gullies: { count: 4, depth: 2 }, voxelSize: 2, dryRun: true });
  assert.equal(dry.applied, false);
  assert.match(dry.cone.expr, /fbm/);
  const r = await call(s, "sculpt_cone", { at: [540, 0], floor: 0, radius: 24, height: 14, smooth: false, voxelSize: 2, edges: "noise" });
  assert.ok(r.applied && r.added.length > 0);
  let top = -Infinity;
  for (const g of grids(store)) if (g.frame.new) for (let i = 0; i < g.words.length; i++) if (isSolid(g.words[i])) top = Math.max(top, g.toMap[7] + ((i % g.size[1]) + 1) * g.toMap[5]);
  assert.ok(Math.abs(top - 14) <= 2, `summit ${top}`);
  const sm = await call(s, "sculpt_cone", { at: [580, 0], floor: 0, radius: 12, height: 8, voxelSize: 2 });
  assert.ok(sm.applied && sm.added.some((b: { skin: boolean }) => b.skin));
  assert.deepEqual(limitProblems(store), []);
});

test("scenery helpers: a scale as a number, a list or a string; kinds from detail names", () => {
  assert.deepEqual(scaleOf(1.5, "s"), [1.5, 1.5, 1.5]);
  assert.deepEqual(scaleOf("2", "s"), [2, 2, 2]);
  assert.deepEqual(scaleOf("1, 2, 3", "s"), [1, 2, 3]);
  assert.deepEqual(scaleOf([1, 1, 2], "s"), [1, 1, 2]);
  assert.throws(() => scaleOf("big", "copies[0].scale"), /copies\[0\]\.scale: a scale is a number from 0.05 to 20/);
  assert.equal(kindOf("VISIBLE_flag"), "flag");
  assert.equal(kindOf("VISIBLE_Flag2"), "flag");
  assert.equal(kindOf("VISIBLE_MORTAL_Grass"), "grass");
  assert.equal(kindOf("D05_02"), "d05");
});
