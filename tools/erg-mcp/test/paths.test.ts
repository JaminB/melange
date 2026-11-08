// Strokes that reach every frame, tapered and downhill paths (sculpt_path allFrames, taper, followSurface), ramps cut into
// the terrain (build_ramp mode cut) and walkable tunnels (dig_tunnel floor flat), on the synthetic 12-frame level (its
// root renamed Scene so blocks can be added) and a heightfield cone over several blocks. Bundled for Node by test.ps1.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, apply, cellBudget, isSolid, material, strokeChanges, type Brush, type Loaded, type Scene, type Vec3 } from "../src/core";
import { apply as applyField, plan } from "../src/lib/heightfield-plan";
import { spansAt } from "../src/lib/inspect-field";
import { laneTop } from "../src/lib/structures-shapes";
import { downhillPath, frameGrids, groundAt, taperSizes } from "../src/lib/terrain-ops";
import { grid, grids, limitProblems } from "../src/lib/voxel";
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
function session() {
  const store = new EditorStore("t", loaded(), loaded());
  const s = { requireProject: () => ({ store, id: "t", info: {} }), get store() { return store; } } as unknown as Session;
  return { store, s };
}
const all = [...terrainTools, ...structureTools];
const call = async (s: Session, name: string, args: Record<string, unknown>): Promise<any> => all.find((t) => t.name === name)!.handler(s, args);

/** A cone of height 20 at the origin, falling 0.8 per map unit, over several added blocks. */
function cone(store: EditorStore) {
  const hf = plan(store, { min: [-30, -30], max: [30, 30], floor: 0, expr: "max(0, 20 - r*0.8)", material: 1 });
  assert.ok(hf.fits, hf.why);
  applyField(store, hf);
  assert.ok(grids(store).filter((g) => g.frame.new).length >= 4);
  assert.equal(store.stack.depth, 1);
}
const top = (store: EditorStore, x: number, z: number) => groundAt(grids(store), x, z, Infinity);

test("allFrames: fill and paint reach every frame the brush overlaps, rotated ones too, as carve does", async () => {
  const { store, s } = session();
  // a block over the middle of frame 6 (rotated about y), so a dab there covers both
  const g6 = grid(store, 6);
  const mid = g6.bounds.min.map((v, a) => (v + g6.bounds.max[a]) / 2) as Vec3;
  const b = await call(s, "add_block", { size: [6, 6, 6], material: 2, center: mid, units: "map" });
  const args = { mode: "paint", shape: "sphere", size: [7, 7, 7], material: 63, center: mid, units: "map", frame: b.id };
  const only = await call(s, "sculpt", { ...args, dryRun: true });
  assert.deepEqual(only.frames.map((f: { id: number }) => f.id), [b.id], "the panel's paint stays in the anchor");
  const before = g6.words.slice();
  const r = await call(s, "sculpt", { ...args, allFrames: true });
  assert.equal(r.allFrames, true);
  assert.ok(r.frames.some((f: { id: number }) => f.id === 6), JSON.stringify(r.frames));
  assert.equal(store.stack.depth, 2);
  // the cells of frame 6 that were painted are exactly those the panel's carve would clear with the same dab
  const gs = frameGrids(store), anchor = gs.find((g) => g.frame.id === b.id)!;
  const c = [0, 1, 2].map((a) => Math.floor(apply(anchor.toLocal, mid)[a]) + 0.5) as Vec3;
  const live = new Map(store.voxels);
  live.set(g6.ref, before);
  const carve: Brush = { mode: "carve", shape: "sphere", size: [7, 7, 7], material: 0 };
  const want = new Set(strokeChanges(gs, live, { grid: anchor, center: c }, carve).get(g6.ref)!.keys());
  const got = new Set([...g6.words.keys()].filter((i) => g6.words[i] !== before[i]));
  assert.ok(want.size > 0);
  assert.deepEqual([...got].sort(), [...want].sort());
  for (const i of got) assert.ok(isSolid(g6.words[i]) && material(g6.words[i]) === 63);
  // fill reaches the other frames' empty cells, still one undo step
  const f = await call(s, "sculpt", { ...args, mode: "fill", material: 62, allFrames: true });
  assert.ok(f.frames.some((x: { id: number }) => x.id === 6), JSON.stringify(f.frames));
  assert.equal(store.stack.depth, 3);
  assert.deepEqual(limitProblems(store), []);
  store.undo(); store.undo();
  assert.deepEqual([...g6.words], [...before]);
});

test("allFrames paint along a path over a cone of several blocks; the panel's paint stops at the anchor block", async () => {
  const { store, s } = session();
  cone(store);
  const pts = [-14, -7, 0, 7, 14].map((x) => [x, (top(store, x, 1.5) ?? 0) - 0.5, 1.5]);
  const args = { mode: "paint", shape: "sphere", size: [3, 3, 3], material: 9, points: pts, units: "map", spacing: 1 };
  const one = await call(s, "sculpt_path", { ...args, dryRun: true });
  const many = await call(s, "sculpt_path", { ...args, allFrames: true });
  assert.equal(one.frames.length, 1);
  assert.ok(many.frames.length >= 2, JSON.stringify(many.frames));
  assert.ok(many.changed > one.changed);
  assert.equal(store.stack.depth, 2);
  for (const x of [-12, 12]) assert.equal(spansAt(grids(store), x, 1.5)[0].material, 9, `painted at x ${x}`);
});

test("taper narrows the brush linearly along the path", async () => {
  const { store, s } = session();
  const b = await call(s, "add_block", { size: [31, 8, 11], material: 3, at: [515.5, 0, 505.5], units: "map" });
  const g = grid(store, b.id);
  const r = await call(s, "sculpt_path", { mode: "carve", shape: "sphere", size: [7, 7, 7], points: [[502.5, 7.5, 505.5], [528.5, 7.5, 505.5]], units: "map", spacing: 1, taper: 0.2 });
  assert.equal(r.taper, 0.2);
  // carved cells per x slice of the block: wide at the start, narrow at the end
  const slice = (x: number) => { let n = 0; for (let z = 0; z < 11; z++) for (let y = 0; y < 8; y++) if (!isSolid(g.words[(z * 31 + x) * 8 + y])) n++; return n; };
  const xs = [3, 8, 13, 18, 23];
  const counts = xs.map(slice);
  for (let i = 1; i < counts.length; i++) assert.ok(counts[i] <= counts[i - 1], `slices ${counts}`);
  assert.ok(counts[0] >= 3 * counts[4], `slices ${counts}`);
  assert.equal(store.stack.depth, 2);
  // the sizes themselves: size at the start, size x taper at the end, linear, never under 1
  const sz = taperSizes([[0, 0, 0], [10, 0, 0]], 2, 6, [10, 4, 1], 0.3);
  assert.deepEqual(sz[0], [10, 4, 1]);
  assert.deepEqual(sz[5].map((v) => Math.round(v * 1000) / 1000), [3, 1.2, 1]);
  assert.ok(Math.abs(sz[2][0] - 10 * (1 - 0.7 * 0.4)) < 1e-9);
  // no taper: every dab the same
  const flat = await call(s, "sculpt_path", { mode: "carve", shape: "sphere", size: [3, 3, 3], points: [[502.5, 3.5, 505.5], [528.5, 3.5, 505.5]], units: "map", spacing: 1, dryRun: true });
  assert.equal(flat.taper, undefined);
});

test("followSurface downhill walks the cone's flank to its foot and paints along it, across blocks", async () => {
  const { store, s } = session();
  cone(store);
  const gs = grids(store);
  const w = downhillPath(gs, [-3, 40, -3], { step: 1, maxLength: 400, maxSteps: 1000, water: null });
  assert.ok(w.points.length > 15, `${w.points.length} points, stop ${w.stop}`);
  for (let i = 1; i < w.points.length; i++) assert.ok(w.points[i][1] <= w.points[i - 1][1] + 1e-9, `climbs at ${i}`);
  const r0 = Math.hypot(w.points[0][0], w.points[0][2]), r1 = Math.hypot(w.points.at(-1)![0], w.points.at(-1)![2]);
  assert.ok(r1 > r0 + 15, `from r ${r0} to ${r1}`);
  assert.ok(w.points[0][1] > 15 && w.points.at(-1)![1] < 3, `from y ${w.points[0][1]} to ${w.points.at(-1)![1]}`);
  assert.ok(["edge", "pit"].includes(w.stop), w.stop);
  // limits: maxSteps, maxLength, water
  assert.equal(downhillPath(gs, [-3, 40, -3], { step: 1, maxLength: 400, maxSteps: 5, water: null }).points.length, 6);
  const ml = downhillPath(gs, [-3, 40, -3], { step: 1, maxLength: 7.5, maxSteps: 1000, water: null });
  assert.equal(ml.stop, "maxLength");
  assert.equal(ml.length, 7);
  const wet = downhillPath(gs, [-3, 40, -3], { step: 1, maxLength: 400, maxSteps: 1000, water: 10 });
  assert.equal(wet.stop, "water");
  assert.ok(wet.points.at(-1)![1] <= 10 && wet.points.at(-2)![1] > 10);
  assert.throws(() => downhillPath(gs, [500, 0, 500], { step: 1, maxLength: 10, maxSteps: 10, water: null }), /no ground under the start/);

  // the tool: a lava river from the summit, painted in every block it crosses, one undo step, the path in the answer
  store.scene.water.level = 4 * 20;
  const r = await call(s, "sculpt_path", { mode: "paint", shape: "sphere", size: [3, 3, 3], material: 9, points: [[-3 * 20, 800, -3 * 20]], followSurface: "downhill", allFrames: true, spacing: 1, taper: 0.5 });
  assert.equal(r.followSurface.stop, "water");
  assert.equal(r.followSurface.units, "world");
  assert.ok(r.followSurface.path.length > 10 && r.followSurface.end[1] <= 4 * 20, JSON.stringify(r.followSurface.end));
  assert.ok(r.frames.length >= 2, JSON.stringify(r.frames));
  assert.equal(store.stack.depth, 2);
  const mid = r.followSurface.path[Math.floor(r.followSurface.path.length / 2)].map((v: number) => v / 20);
  assert.equal(spansAt(grids(store), mid[0], mid[2])[0].material, 9, "the river is painted on the ground");
  // carve: a channel below the path
  const c = await call(s, "sculpt_path", { mode: "carve", shape: "sphere", size: [3, 3, 3], points: [[-3, 40, -3]], units: "map", followSurface: "downhill", depth: -1, spacing: 1, maxLength: 12 });
  assert.equal(c.followSurface.stop, "maxLength");
  assert.ok(c.changed > 0);
  assert.ok(c.followSurface.path.length === 13);
  await assert.rejects(call(s, "sculpt_path", { mode: "carve", size: [3, 3, 3], points: [[0, 0, 0]], frame: grids(store)[0].id, units: "grid", followSurface: "downhill" }), /world or map/);
  await assert.rejects(call(s, "sculpt_path", { mode: "carve", size: [3, 3, 3], points: [[0, 0, 0]], depth: 1 }), /for followSurface/);
});

test("build_ramp mode cut: a switchback carved into a cone, the air over it cleared, no new blocks", async () => {
  const { store, s } = session();
  cone(store);
  const blocks = store.scene.frames.filter((f) => f.new).length, cells = cellBudget(store.scene).used;
  const y0 = top(store, -18, 0)!, y1 = top(store, -4, 0)!, gs0 = grids(store);
  await assert.rejects(call(s, "build_ramp", { from: [-18, y0, 0], to: [-4, y1, 0], mode: "cut", switchback: true, maxSlope: 25, place: "auto" }), /existing frames/);
  await assert.rejects(call(s, "build_ramp", { from: [-18, y0, 0], to: [-4, y1, 0], headroom: 3 }), /for mode cut/);
  const r = await call(s, "build_ramp", { from: [-18, y0, 0], to: [-4, y1, 0], width: 3, mode: "cut", switchback: true, maxSlope: 25, material: 7 });
  assert.equal(r.structureInfo.mode, "cut");
  assert.ok(r.structureInfo.lanes >= 2, JSON.stringify(r.structureInfo));
  assert.equal(r.newBlocks.length, 0);
  assert.equal(store.scene.frames.filter((f) => f.new).length, blocks);
  assert.equal(cellBudget(store.scene).used, cells);
  assert.ok(r.voxels.removed > 20, JSON.stringify(r.voxels));
  assert.ok(r.frames.length >= 2, JSON.stringify(r.frames));
  assert.equal(store.stack.depth, 2);
  // along the middle of each lane, where it is cut into the cone, the ground is the ramp's surface (to half a voxel) and
  // the 3 above it are air (the second lane of a switchback climbs back over the flank, onto air outside every block)
  const L = 14, dh = (y1 - y0) / r.structureInfo.lanes, gs = grids(store);
  let checked = 0;
  for (let i = 0; i < r.structureInfo.lanes; i++) {
    const forward = i % 2 === 0, z = i * 3;
    for (let u = 1; u < L - 1; u++) {
      const x = forward ? -18 + u + 0.5 : -4 - u - 0.5;
      const want = laneTop({ a: [0, 0], d: [1, 0], len: L, w: 3, yA: y0 + dh * i, yB: y0 + dh * (i + 1), floorY: null, thick: 1, mat: 7 }, u + 0.5);
      if (want > (groundAt(gs0, x, z, Infinity) ?? -Infinity)) continue;
      checked++;
      const sp = spansAt(gs, x, z);
      const ground = sp.find((q) => q.bottom < want + 0.5);
      assert.ok(ground && Math.abs(ground.top - want) <= 0.5 + 1e-6, `lane ${i} u ${u}: ground ${ground?.top}, ramp ${want}`);
      assert.ok(!sp.some((q) => q.top > want + 0.5 + 1e-6 && q.bottom < want + 2.5), `lane ${i} u ${u}: solid in the headroom`);
    }
  }
  assert.ok(checked >= 8, `${checked} points checked`);
  assert.deepEqual(limitProblems(store), []);
  store.undo();
  assert.equal(store.stack.depth, 1);
});

test("dig_tunnel floor flat: a walkable sloping floor, headroom over it, the steepest segment refused", async () => {
  const { store, s } = session();
  await call(s, "build_tower", { at: [500, 0, 500], shape: "square", radius: 14, height: 12, crenellations: false, material: 6 });
  await assert.rejects(call(s, "dig_tunnel", { points: [[488, 2, 500], [492, 10, 500]], floor: "flat" }), /slopes 63\.4 degrees \(maxSlope 40\)/);
  await assert.rejects(call(s, "dig_tunnel", { points: [[488, 2, 500], [512, 6, 500]], headroom: 3 }), /for floor "flat"/);
  const r = await call(s, "dig_tunnel", { points: [[488, 2, 500.5], [512, 6, 500.5]], floor: "flat", radius: 1.5, headroom: 3, material: 6 });
  assert.equal(r.structureInfo.floor, "flat");
  assert.ok(Math.abs(r.structureInfo.steepestDegrees - 9.4623) < 1e-3, JSON.stringify(r.structureInfo));
  assert.ok(r.voxels.removed > 100);
  const gs = grids(store);
  for (let x = 490; x < 511; x++) {
    const floorY = 2 + ((x + 0.5 - 488) * 4) / 24;
    const sp = spansAt(gs, x + 0.5, 500.5);
    const ground = sp.find((q) => q.bottom < floorY);
    assert.ok(ground && Math.abs(ground.top - floorY) <= 0.5 + 1e-6, `x ${x}: floor ${ground?.top}, want ${floorY}`);
    assert.ok(!sp.some((q) => q.top > floorY + 0.5 + 1e-6 && q.bottom < floorY + 2.5), `x ${x}: rock in the headroom`);
    // the floor is level across the bore: the same at the bore's sides
    for (const dz of [-1, 1]) assert.equal(spansAt(gs, x + 0.5, 500.5 + dz).find((q) => q.bottom < floorY)!.top, ground!.top);
  }
  // the default tube keeps its round floor
  const t = await call(s, "dig_tunnel", { points: [[490, 8, 495], [510, 8, 495]], radius: 1.5, dryRun: true });
  assert.equal(t.structureInfo.floor, undefined);
});
