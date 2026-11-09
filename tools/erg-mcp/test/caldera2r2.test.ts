// The Caldera v2 round-2 gaps: save's projectHash against the saved project's (the server's object defaults), a column
// on a skin tile's edge (no false hole), check_trail's banks, the region object, check_reachability's per-start route
// zone, heightfield_drape's tile origin and corner shift, the clear-view eye, and the assert runs kept across erg-mcp
// processes. On the synthetic 12-frame level (its root renamed Scene), away from its own frames. Bundled by test.ps1.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, type Loaded, type Scene } from "../src/core";
import { apply, plan } from "../src/lib/heightfield-plan";
import { applyDrape, planDrape } from "../src/lib/heightfield-drape";
import { spansAt } from "../src/lib/inspect-field";
import { grids } from "../src/lib/voxel";
import { projectHash, storedObject } from "../src/lib/project-hash";
import { blocked, clearEye, inSolid, openAir } from "../src/lib/eye";
import { mergeRuns, pairOf, toSaved, type AssertRun } from "../src/lib/assert-log";
import { validate } from "../src/registry";
import { tools as inspectTools } from "../src/tools/inspect";
import type { Session } from "../src/session";

function loaded(): Loaded {
  const s: Scene = JSON.parse(readFileSync(new URL("../../../../tests/fixtures/erg/synthetic-12.json", import.meta.url), "utf8"));
  s.frames.find((f) => f.id === 1)!.name = "Scene";
  const blobs = new Map<number, ArrayBuffer>();
  for (const [ref, buf] of syntheticBlobs(s) as Map<number, Uint8Array>) blobs.set(ref, buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength) as ArrayBuffer);
  return { scene: s, blobs };
}
function session() {
  const store = new EditorStore("t", loaded(), loaded());
  const project = { store, id: "t", info: {}, erg: { script: async () => "" } };
  const s = { requireProject: () => project, get store() { return store; }, project,
    levels: async () => { throw new Error("no server"); }, client: async () => { throw new Error("no server"); }, ensure: async () => { throw new Error("no server"); } } as unknown as Session;
  return { store, s };
}
const call = async (s: Session, name: string, args: Record<string, unknown>): Promise<any> => {
  const t = inspectTools.find((x) => x.name === name)!;
  const bad = validate(t.inputSchema, args);
  if (bad) throw new Error(bad);
  const r = await t.handler(s, args);
  return typeof r === "string" && r.startsWith("{") ? JSON.parse(r) : r;
};
const slab = (store: EditorStore, x0: number, z0: number, x1: number, z1: number, floor: number, h: number, material = 1) =>
  apply(store, plan(store, { min: [x0, z0], max: [x1, z1], floor, expr: String(h), material }));

test("projectHash: the patch save sends and the project the server wrote hash the same (object defaults filled)", () => {
  const base = { format: "erg-patch/2", stem: "a", title: "A", base: { key: "Multi.X" }, ops: [], script: { present: false, sha256: null } };
  const sent = { ...base, objects: [{ knot: "CRATE_1", type: "crate", crate: { kind: "weapon", contents: "kWeaponBazooka", hitpoints: 25, parachute: false } },
    { knot: "TRIGGER_0", type: "trigger", trigger: { index: 0 } }, { knot: "minefactory", type: "minefactory", factory: {} }] };
  // as src/erg/scene.cpp ObjectsJson writes them: every crate and trigger field, an empty factory left out
  const stored = { ...base, stem: "other", title: "Other", script: { present: true, sha256: "ab" }, objects: [
    { knot: "CRATE_1", type: "crate", crate: { kind: "weapon", contents: "kWeaponBazooka", count: 1, hitpoints: 25, parachute: false } },
    { knot: "TRIGGER_0", type: "trigger", trigger: { index: 0, radius: 60, teamCollect: 0, teamDestroy: 4, hitpoints: 1, wormCollect: false } },
    { knot: "minefactory", type: "minefactory" }] };
  assert.equal(projectHash(sent, "x = 1\r\n"), projectHash(stored, "x = 1"));
  // a real difference still shows
  const more = { ...stored, objects: [{ ...stored.objects[0], crate: { ...(stored.objects[0] as { crate: object }).crate, count: 2 } }, ...stored.objects.slice(1)] };
  assert.notEqual(projectHash(sent, "x = 1"), projectHash(more, "x = 1"));
  assert.deepEqual(storedObject({ knot: "minefactory", type: "minefactory", factory: { keepKnot: false, schemeMines: true, launchers: [[1, 2, 3]] } }),
    { knot: "minefactory", type: "minefactory", factory: { launchers: [[1, 2, 3]] } });
});

test("a column a hair under a skin tile's far edge reads the tile's ground (no false hole), as does the column on the edge", () => {
  const { store } = session();
  const p = planDrape(store, { min: [500, -20], max: [570, 20], floor: 0, expr: "6 + 0.1 * x", material: 1, voxelSize: 1, core: false });
  assert.equal(p.report.fits, true, String(p.report.why));
  applyDrape(store, p);
  const skin = grids(store).filter((g) => g.frame.corners);
  const edge = skin.map((g) => g.bounds.max[0]).find((x) => skin.some((h) => Math.abs(h.bounds.min[0] - x) < 1e-9))!;
  assert.ok(edge !== undefined, "two skin tiles meet along x");
  for (const x of [edge - 1e-12, edge - 1e-10, edge, edge + 1e-12]) assert.ok(spansAt(grids(store), x, 0).length, `ground at x ${x}`);
  const holes = findAround(store, edge);
  assert.equal(holes, 0);
});
function findAround(store: EditorStore, edge: number) {
  let n = 0;
  for (let z = -15; z <= 15; z += 0.25) if (!spansAt(grids(store), edge - 1e-12, z).length) n++;
  return n;
}

test("check_trail: a cut bank rising from just over the trail is a bank, not a no-ground drop; a real edge still is", async () => {
  const { store, s } = session();
  slab(store, 500, -6, 540, 3, 0, 8);      // the trail's bed, top 8
  slab(store, 500, 3, 540, 7, 10, 4);      // the uphill bank on its left: solid 10..14, nothing under it
  const r = await call(s, "check_trail", { trails: [[[502, 8, 0], [538, 8, 0]]], width: 4, margin: 2 });
  assert.equal(r.ok, true, JSON.stringify(r.trails[0].runs));
  assert.ok(r.trails[0].banks > 0);
  const { store: st2, s: s2 } = session();
  slab(st2, 500, -6, 540, 3, 0, 8);
  slab(st2, 500, 3, 540, 7, 16, 4);        // ground 8 over the trail with air under it: an overhang over a drop
  const r2 = await call(s2, "check_trail", { trails: [[[502, 8, 0], [538, 8, 0]]], width: 4, margin: 2 });
  assert.equal(r2.ok, false);
  assert.ok(r2.trails[0].runs.some((x: { kind: string; side: string }) => x.kind === "drop" && x.side === "left"));
});

test("the region object {x0, z0, x1, z1} reads as the flat edges; an edge given twice must agree", async () => {
  const { store, s } = session();
  slab(store, 500, -6, 540, 6, 0, 8);
  const flat = await call(s, "heightmap_ascii", { x0: 500, z0: -6, x1: 540, z1: 6, step: 1 });
  const obj = await call(s, "heightmap_ascii", { region: { x0: 500, z0: -6, x1: 540, z1: 6 }, step: 1 });
  assert.equal(obj, flat);
  const h = await call(s, "check_holes", { region: { x0: 500, z0: -6, x1: 540, z1: 6 } });
  assert.deepEqual(h.region, (await call(s, "check_holes", { x0: 500, z0: -6, x1: 540, z1: 6 })).region);
  await assert.rejects(call(s, "heightmap_ascii", { x0: 501, region: { x0: 500 } }), /given twice/);
});

test("check_reachability via: each start walks with its own near zone, so another start's zone never bridges its walk", async () => {
  const { store, s } = session();
  slab(store, 500, -4, 520, 4, 0, 8, 1);   // the route (material 1)
  slab(store, 520, -4, 530, 4, 0, 8, 2);   // off the route
  slab(store, 530, -4, 550, 4, 0, 8, 1);
  const args = { x0: 498, z0: -6, x1: 552, z1: 6, target: { x: 545, z: 0, radius: 2 }, via: { materials: [1], mode: "only", near: 6 }, units: "map" };
  const alone = await call(s, "check_reachability", { ...args, from: [[505, 8, 0]] });
  assert.equal(alone.spawns[0].reachable, false, "off the route between the slabs");
  const both = await call(s, "check_reachability", { ...args, from: [[505, 8, 0], [525, 8, 0]] });
  assert.equal(both.spawns[0].reachable, false, "the second start's zone (519-531) does not carry the first across");
  assert.equal(both.spawns[1].reachable, true, "its own zone carries the second start across");
});

test("heightfield_drape: tileOrigin puts a tile corner there; cornersOff moves the four-block corners off a path", () => {
  const { store } = session();
  const base = { min: [500, -40] as [number, number], max: [580, 40] as [number, number], floor: 0, expr: "6", material: 1, voxelSize: 1, core: false };
  const p0 = planDrape(store, base);
  const corners0 = (p0.report.skin as { meetingCorners: number[][] }).meetingCorners;
  assert.ok(corners0.length >= 1, "an 80 x 80 skin of 31-column tiles has four-block corners");
  // a trail straight through the first corner, 40 long along x
  const trail = [[corners0[0][0] - 20, corners0[0][1]], [corners0[0][0] + 20, corners0[0][1]]];
  const off = (c: number[]) => { const x = Math.max(trail[0][0], Math.min(trail[1][0], c[0])); return Math.hypot(c[0] - x, c[1] - trail[0][1]); };
  const p1 = planDrape(store, { ...base, cornersOff: { paths: [trail], distance: 4 } });
  const t1 = (p1.report.skin as { tiling: { leastDistance: number; unshifted: number; shift: number[] }; meetingCorners: number[][] });
  assert.ok(t1.tiling.unshifted < 1, `the unshifted corner is on the path (${t1.tiling.unshifted})`);
  assert.ok(t1.tiling.leastDistance >= 4, `every corner is 4 off the path after the shift (${t1.tiling.leastDistance})`);
  for (const c of t1.meetingCorners) assert.ok(off(c) >= 4 - 1e-9, `corner ${c} is ${off(c)} off the trail`);
  const p2 = planDrape(store, { ...base, tileOrigin: [510, -30] });
  const t2 = (p2.report.skin as { tiling: { origin: { map: number[] } }; meetingCorners: number[][] });
  assert.deepEqual(t2.tiling.origin.map, [510, -30]);
  assert.ok(t2.meetingCorners.some((c) => (c[0] - 510) % 31 === 0 && (c[1] + 30) % 31 === 0), JSON.stringify(t2.meetingCorners));
  assert.throws(() => planDrape(store, { ...base, tileOrigin: [510, -30], cornersOff: { paths: [trail] } }), /give one/);
  // the shifted skin applies and stands where the unshifted one does
  applyDrape(store, p1);
  assert.ok(Math.abs(spansAt(grids(store), 540, 0)[0].top - 6) < 1.01);
});

test("clearEye: an eye inside the ground moves to open air with a clear view; a clear one stays", () => {
  const { store } = session();
  slab(store, 500, -10, 540, 10, 0, 8);
  slab(store, 518, -10, 522, 10, 8, 10);   // a wall between x 518 and 522, 10 high over the slab
  const gs = grids(store), target = [510, 8.6, 0];
  assert.ok(inSolid(gs, [530, 5, 0]) && !openAir(gs, [530, 5, 0]));
  const behind = [530, 12, 0];             // over the slab, behind the wall
  assert.ok(blocked(gs, behind, target, 1));
  const r = clearEye(gs, behind, target, { skip: 1 });
  assert.equal(r.clear, true);
  assert.ok(r.moved && openAir(gs, r.eye) && !blocked(gs, r.eye, target, 1), JSON.stringify(r));
  const open = [505, 14, 6];
  assert.deepEqual(clearEye(gs, open, target, { skip: 1 }), { eye: open, clear: true, moved: null, tried: 1 });
});

test("assert runs: a baseline from an earlier erg-mcp process pairs with this one's Erg run of the same game", () => {
  const run = (label: string, kind: AssertRun["kind"], at: number, game?: string): AssertRun => ({ label, kind, startedAt: at, endedAt: at + 60000, counts: new Map([["ASSERT(x) fail: 1", 2]]), total: 2, ...(game ? { game } : {}) });
  const saved = [toSaved(run("Multi.DinerMight", "baseline", 1000, "b1@x")), toSaved(run("Multi.Other", "baseline", 500, "b0@x"))];
  const mem = [run("Multi.ergtest_c", "erg", 5000, "b1@x")];
  const all = mergeRuns(mem, [...saved, toSaved(mem[0])]);
  assert.equal(all.length, 3, "the run saved by this process is not doubled");
  const { erg, base } = pairOf(all);
  assert.equal(erg?.label, "Multi.ergtest_c");
  assert.equal(base?.label, "Multi.DinerMight");
  assert.equal(base?.from, "saved");
  assert.equal(base?.counts.get("ASSERT(x) fail: 1"), 2);
  // another game's baseline is not taken
  assert.equal(pairOf(mergeRuns([run("Multi.ergtest_c", "erg", 5000, "b2@x")], saved)).base, undefined);
});
