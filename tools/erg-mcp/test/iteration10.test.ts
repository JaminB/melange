// The Caldera iteration-10 gaps, on the synthetic 12-frame level (its root renamed Scene so blocks can be added), away
// from its own frames (x 480-700) where a test builds ground: check_support and the factory's shell warning, the
// heightfield rules' exclusions and per-corner edges, paint_blob and the paint tools' exclusions, build_castle's
// keepAxes and gateLane, check_flood's escape and solve, the project hash, the assert log, the mines' zones and
// set_mine_factory's re-check. Bundled for Node by test.ps1.
import assert from "node:assert/strict";
import { mkdirSync, mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, maskOf, secondOf, type Loaded, type Scene } from "../src/core";
import { checkFactory } from "../src/lib/factory-check";
import { apply, plan } from "../src/lib/heightfield-plan";
import { count, diff, signature, type AssertRun } from "../src/lib/assert-log";
import { canonical, projectHash, readSource } from "../src/lib/project-hash";
import { grids, isSolid, material } from "../src/lib/voxel";
import { validate } from "../src/registry";
import { mineZone, tools as gameTools } from "../src/tools/game";
import { tools as inspectTools } from "../src/tools/inspect";
import { tools as placeTools } from "../src/tools/place";
import { tools as structureTools } from "../src/tools/structures";
import { tools as terrainTools } from "../src/tools/terrain";
import type { Session } from "../src/session";
import { readFileSync } from "node:fs";

function loaded(): Loaded {
  const s: Scene = JSON.parse(readFileSync(new URL("../../../../tests/fixtures/erg/synthetic-12.json", import.meta.url), "utf8"));
  s.frames.find((f) => f.id === 1)!.name = "Scene";
  const blobs = new Map<number, ArrayBuffer>();
  for (const [ref, buf] of syntheticBlobs(s) as Map<number, Uint8Array>) blobs.set(ref, buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength) as ArrayBuffer);
  return { scene: s, blobs };
}
function session() {
  const store = new EditorStore("t", loaded(), loaded());
  const project = { store, id: "t", info: { id: "t" }, erg: { script: async () => "" } };
  const s = { requireProject: () => project, get store() { return store; }, project,
    levels: async () => { throw new Error("no server"); }, client: async () => { throw new Error("no server"); } } as unknown as Session;
  return { store, s };
}
const all = [...inspectTools, ...structureTools, ...placeTools, ...gameTools, ...terrainTools];
const call = async (s: Session, name: string, args: Record<string, unknown>): Promise<any> => {
  const t = all.find((x) => x.name === name)!;
  const bad = validate(t.inputSchema, args);
  if (bad) throw new Error(bad);
  const r = await t.handler(s, args);
  return typeof r === "string" ? JSON.parse(r) : r;
};
/** The new blocks' solid words with a second material `m`, and those of first material `m`. */
function wordsOf(store: EditorStore, m: number) {
  let second = 0, first = 0;
  for (const g of grids(store)) {
    if (!g.frame.new) continue;
    for (const w of g.words) {
      if (!isSolid(w)) continue;
      if (material(w) === m) first++;
      if (maskOf(w) && secondOf(w) === m) second++;
    }
  }
  return { first, second };
}

test("check_support: a shell over a void is hollow, the solid field is not, and the factory warns over a shell", async () => {
  const { store, s } = session();
  apply(store, plan(store, { min: [480, -12], max: [520, 12], floor: 0, expr: "4", material: 1 }));
  const solid = await call(s, "check_support", { at: [500, 0], radius: 2, units: "map", downTo: 0 });
  assert.equal(solid.ok, true, JSON.stringify(solid));
  assert.equal(solid.hollow, 0);
  // a void under the top voxel: y 1-3 carved out of a 5 x 5 patch round (500, 0)
  const g = grids(store).filter((x) => x.frame.new && x.bounds.min[0] <= 497 && x.bounds.max[0] >= 503 && x.bounds.min[2] <= -3 && x.bounds.max[2] >= 3);
  assert.ok(g.length, "a block under the patch");
  await call(s, "fill_box_raw", { units: "map", frame: g[0].id, min: [497, 1, -3], max: [503, 3, 3], state: "empty" });
  const shell = await call(s, "check_support", { at: [500, 0], radius: 2, units: "map", downTo: 0 });
  assert.equal(shell.ok, false);
  assert.ok(shell.hollow > 0 && shell.centre.solid === false, JSON.stringify(shell));
  assert.equal(shell.thinnest.thickness, 1);
  assert.equal(shell.deepest.voidBelow, 2);
  assert.match(shell.summary, /shell over a void/);
  // the factory on it, the water at y 0.5 (the base the ground must reach): a warning names the shell
  await call(s, "set_water", { level: 10 });
  await call(s, "place_object", { type: "minefactory", units: "map", pos: [500, 4, 0], snap: "surface" });
  const fc = checkFactory(store, (v) => `${v} map`)!;
  assert.ok(fc.warnings.some((w) => /shell over a void/.test(w)), JSON.stringify(fc.warnings));
});

test("heightfield rules: exclude keeps a rule off an area, edge blends the rule's boundary per corner", async () => {
  const { store } = session();
  // grass (2) everywhere but a circle round (500, 0): the default (1) there
  apply(store, plan(store, { min: [480, -12], max: [520, 12], floor: 0, expr: "3", material: 1, centre: [0, 0],
    materials: [{ material: 2, surface: 1, exclude: [{ x: 500, z: 0, radius: 5 }] }] }));
  const top = (x: number, z: number) => {
    for (const g of grids(store)) {
      if (!g.frame.new || x < g.bounds.min[0] || x >= g.bounds.max[0] || z < g.bounds.min[2] || z >= g.bounds.max[2]) continue;
      const i = Math.floor(x - g.bounds.min[0]), k = Math.floor(z - g.bounds.min[2]), [X, Y] = g.size;
      for (let y = Y - 1; y >= 0; y--) { const w = g.words[(k * X + i) * Y + y]; if (isSolid(w)) return material(w); }
    }
    return -1;
  };
  assert.equal(top(500.5, 0.5), 1, "excluded: the default material");
  assert.equal(top(510.5, 0.5), 2, "outside the circle: the rule's");
  // a road (5) along x = 500 on a fresh copy: without edge no voxel has it as second material; with edge its border does
  const a = session().store, b = session().store;
  const road = { material: 5, surface: 1, paths: [{ points: [[500, -12], [500, 12]], width: 4.4 }] };
  apply(a, plan(a, { min: [480, -12], max: [520, 12], floor: 0, expr: "3", material: 1, materials: [road] }));
  apply(b, plan(b, { min: [480, -12], max: [520, 12], floor: 0, expr: "3", material: 1, materials: [{ ...road, edge: {} }] }));
  const wa = wordsOf(a, 5), wb = wordsOf(b, 5);
  assert.equal(wa.second, 0);
  assert.ok(wb.second > 0, JSON.stringify(wb));
  assert.equal(wa.first, wb.first, "the edge leaves the road's own voxels as they were");
  assert.throws(() => plan(a, { min: [480, -12], max: [520, 12], floor: 0, expr: "3", material: 1, materials: [{ material: 5, edge: {} }] }), /needs an area/);
});

test("paint_blob: a wandering pool with a soft edge, paint_region's exclude", async () => {
  const { store, s } = session();
  apply(store, plan(store, { min: [480, -12], max: [520, 12], floor: 0, expr: "3", material: 1 }));
  const r = await call(s, "paint_blob", { material: 9, at: [500, 0], radius: 6, units: "map", noise: { amount: 0.4, lobes: 6, seed: 3 } });
  assert.ok(r.changed > 20 && r.blendedEdge > 0 && r.soft === true, JSON.stringify(r));
  const hard = await call(s, "paint_blob", { material: 10, at: [500, 0], radius: 6, units: "map", soft: false, dryRun: true });
  assert.equal(hard.blendedEdge, undefined);
  await assert.rejects(() => call(s, "paint_blob", { material: 9, units: "map" }), /at and radius/);
  // the region paint keeps off the excluded circle
  const ex = await call(s, "paint_region", { material: 11, units: "map", shapes: [{ min: [490, -5], max: [510, 5] }], exclude: [{ x: 500, z: 0, radius: 4 }], dryRun: true });
  const all2 = await call(s, "paint_region", { material: 11, units: "map", shapes: [{ min: [490, -5], max: [510, 5] }], dryRun: true });
  assert.ok(ex.changed < all2.changed, `${ex.changed} < ${all2.changed}`);
});

test("build_castle: keepAxes gate reads keepSize across and deep, gateLane moves the keep or the gate", async () => {
  const { store, s } = session();
  apply(store, plan(store, { min: [470, -30], max: [530, 30], floor: 0, expr: "2", material: 1 }));
  const e = await call(s, "build_castle", { at: [500, 2, 0], size: [30, 30], gate: "e", keep: true, keepSize: [6, 10], keepAxes: "gate", units: "map", dryRun: true });
  assert.deepEqual(e.structureInfo.keep, [10, 6], JSON.stringify(e.structureInfo));
  const lane = await call(s, "build_castle", { at: [500, 2, 0], size: [36, 24], gate: "s", keep: true, keepSize: [8, 8], gateLane: true, units: "map", dryRun: true });
  const info = lane.structureInfo;
  assert.equal(info.lane.mode, "keep moved aside", JSON.stringify(info));
  assert.equal(Math.abs(info.keepAt[0]), 7);
  assert.ok(["e", "w"].includes(info.keepDoor));
  const gate = await call(s, "build_castle", { at: [500, 2, 0], size: [36, 24], gate: "s", keep: true, keepSize: [8, 8], gateLane: { width: 4, mode: "gate" }, units: "map", dryRun: true });
  const gi = gate.structureInfo;
  assert.equal(gi.lane.mode, "gate moved beside the keep");
  assert.ok(Math.abs(gi.gates[0].offset) > 4, JSON.stringify(gi.gates));
  await assert.rejects(() => call(s, "build_castle", { at: [500, 2, 0], size: [20, 20], gate: "s", keepSize: [10, 10], gateLane: { width: 6, mode: "keep" }, units: "map", dryRun: true }), /gateLane/);
});

test("check_flood: each spawn's escape and latest legal round, and solve's per-round sea", async () => {
  const { store, s } = session();
  // a low shelf (2) and a hill (8) beside it
  apply(store, plan(store, { min: [480, -12], max: [560, 12], floor: 0, expr: "2 + clamp(x / 4, 0, 6)", material: 1 }));
  await call(s, "place_spawn_knots", { units: "map", spawns: [{ worm: 0, xz: [490, 0] }, { worm: 4, xz: [495, 0] }] });
  await call(s, "set_water", { level: 0 });
  const r = await call(s, "check_flood", { units: "map", x0: 480, z0: -12, x1: 560, z1: 12, teams: 2, wormsPerTeam: 4, rounds: [{ round: 1, level: 0 }, { round: 4, level: 3 }] });
  const sp = r.spawns.find((x: { name: string }) => x.name === "WORM0");
  assert.ok(sp.flood.escape, JSON.stringify(sp.flood));
  // its first own turn: its own shelf is over the next round's sea; once the sea of the next round reaches 3, the hill
  const first = sp.flood.escape.rows[0];
  assert.equal(first.gameTurn, 1);
  assert.equal(first.walk, 0);
  const late = sp.flood.escape.rows.find((x: { nextRoundWater: number }) => x.nextRoundWater >= 3);
  assert.ok(late && late.walk > 20 && late.floorY > 4.5, JSON.stringify(sp.flood.escape));
  assert.ok(sp.flood.escape.latestLegalRound >= 1);
  const sv = await call(s, "check_flood", { units: "map", x0: 480, z0: -12, x1: 560, z1: 12, teams: 2, wormsPerTeam: 4, solve: { from: 1, step: 1, cap: 6 } });
  const lv = sv.flood.solvedSchedule.levels as { round: number; level: number; heldBy?: string }[];
  for (let i = 1; i < lv.length; i++) assert.ok(lv[i].level >= lv[i - 1].level, "never falls");
  assert.ok(lv.some((x) => x.heldBy), "a spawn holds the sea down");
  assert.ok(lv.every((x) => !x.heldBy || x.level <= 2 - 1.5 + 1e-9), JSON.stringify(lv));
  assert.equal(lv[lv.length - 1].level, 6);
});

test("the project hash: key order, stem and title do not count; a pack and a project folder read the same", () => {
  assert.equal(canonical({ b: 1, a: [2, { d: 1, c: 2 }] }), canonical({ a: [2, { c: 2, d: 1 }], b: 1 }));
  const patch = { format: "erg-patch/2", stem: "ergtest_x", title: "X", base: { key: "Multi.DinerMight" }, ops: [1, 2], script: { present: true, sha256: "aa" } };
  const h = projectHash(patch, "print(1)\r\n");
  assert.equal(h, projectHash({ ...patch, stem: "caldera_x", title: "Y", script: { present: true, sha256: "bb" } }, "print(1)\n\n"));
  assert.notEqual(h, projectHash(patch, "print(2)"));
  const dir = mkdtempSync(join(tmpdir(), "ergmcp-hash-"));
  mkdirSync(join(dir, "proj"));
  writeFileSync(join(dir, "proj", "project.ergpatch.json"), JSON.stringify(patch));
  writeFileSync(join(dir, "proj", "script.lua"), "print(1)\n");
  mkdirSync(join(dir, "pack", "src"), { recursive: true });
  mkdirSync(join(dir, "pack", "sim"));
  writeFileSync(join(dir, "pack", "src", "x_x.ergpatch.json"), JSON.stringify({ ...patch, stem: "x_x" }));
  writeFileSync(join(dir, "pack", "sim", "x_x.lua"), "print(1)");
  const a = readSource(join(dir, "proj")), b = readSource(join(dir, "pack"));
  assert.equal(projectHash(a.patch, a.script), h);
  assert.equal(projectHash(b.patch, b.script), h);
});

test("the assert log: signatures without the engine's prefix, and the Erg run diffed against the baseline", () => {
  assert.equal(signature("* XBase      | ASSERT((((HRESULT)(hRes)) >= 0)) fail: 1187 @ .\\ParticleEmitterEffectEntity.cpp"), "ASSERT((((HRESULT)(hRes)) >= 0)) fail: 1187 @ .\\ParticleEmitterEffectEntity.cpp");
  assert.equal(signature("***Error   : .\\PCPostProcess.cpp(761): GL ERROR = invalid operation (1282)"), ".\\PCPostProcess.cpp(761): GL ERROR = invalid operation (1282)");
  assert.equal(signature("[levels] something"), null);
  const run = (kind: AssertRun["kind"]): AssertRun => ({ label: kind, kind, startedAt: 0, endedAt: 60000, counts: new Map(), total: 0 });
  const erg = run("erg"), base = run("baseline");
  count(erg, "* XBase | ASSERT(a) fail: 1 @ x.cpp");
  count(erg, "* XBase | ASSERT(b) fail: 2 @ y.cpp");
  count(base, "* XBase | ASSERT(b) fail: 2 @ y.cpp");
  const d = diff(erg, base);
  assert.deepEqual(d.onlyErg.map((x) => x.sig), ["ASSERT(a) fail: 1 @ x.cpp"]);
  assert.equal(d.both.length, 1);
  assert.match(d.verdict, /only in the Erg run/);
});

test("mineZone: water, under the factory, in the crater (a rim further out) or outside", () => {
  // a bowl: ground 0 within 10 of the origin, a rim of 6 from 10 to 14, 1 beyond
  const ground = (x: number, z: number) => { const r = Math.hypot(x, z); return r < 10 ? 0 : r < 14 ? 6 : 1; };
  const f = [0, 2, 0];
  assert.equal(mineZone([1, 0, 1], f, ground, -5), "under the factory");
  assert.equal(mineZone([6, 0, 0], f, ground, -5), "crater");
  assert.equal(mineZone([20, 1, 0], f, ground, -5), "outside");
  assert.equal(mineZone([20, 1, 0], f, ground, 3), "water");
});

test("set_mine_factory with keepKnot answers the placement checked again", async () => {
  const { store, s } = session();
  apply(store, plan(store, { min: [480, -10], max: [520, 10], floor: 0, expr: "4", material: 1 }));
  const placed = await call(s, "place_object", { type: "minefactory", units: "map", xz: [500, 0] });
  assert.ok(placed.issues.some((l: string) => /set_mine_factory \{keepKnot: true\}/.test(l)), JSON.stringify(placed.issues));
  const set = await call(s, "set_mine_factory", { keepKnot: true });
  assert.ok(Array.isArray(set.issues), JSON.stringify(set));
  assert.ok(!set.issues.some((l: string) => /missed undraped blocks/.test(l)), JSON.stringify(set.issues));
});
