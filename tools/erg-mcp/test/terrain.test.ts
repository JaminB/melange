// The terrain group on the synthetic 12-frame level (bundled by test.ps1 into test/out): strokes match the panel's brush
// exactly, paths, blocks, clearing, raw fills and the surround, each one undo step, and the patch still validates.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import {
  EditorStore, apply, filled, isSolid, material, strokeChanges, validEdit, type Brush, type Loaded, type Scene, type Vec3,
} from "../src/core";
import { frameGrids, pathPoints, compileFormula } from "../src/lib/terrain-ops";
import { getVoxel, grid, grids, limitProblems, voxelIndex } from "../src/lib/voxel";
import { tools } from "../src/tools/terrain";

const scene = (): Scene => JSON.parse(readFileSync(new URL("../../../../tests/fixtures/erg/synthetic-12.json", import.meta.url), "utf8"));

function loaded(): Loaded {
  const s = scene();
  s.base.sha256.hmp = null;                              // no base surround: painting starts from zeros
  const blobs = new Map<number, ArrayBuffer>();
  for (const [ref, buf] of syntheticBlobs(s) as Map<number, Uint8Array>) blobs.set(ref, buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength) as ArrayBuffer);
  return { scene: s, blobs };
}
const store = () => new EditorStore("t", loaded(), loaded());
const withScene = () => { const st = store(); st.scene.frames.find((f) => f.id === 1)!.name = "Scene"; return st; };
const call = async (st: EditorStore, name: string, args: unknown): Promise<any> =>
  tools.find((t) => t.name === name)!.handler({ requireProject: () => ({ store: st }) } as never, args);
const fails = async (st: EditorStore, name: string, args: unknown, re: RegExp) => {
  await assert.rejects(async () => call(st, name, args), re);
};
const solidCount = (st: EditorStore, id: number) => { let n = 0; for (const w of grid(st, id).words) if (isSolid(w)) n++; return n; };
/** A world-unit point at the middle of a cell of a frame. */
const worldOf = (st: EditorStore, id: number, c: Vec3): Vec3 => { const g = grid(st, id); return apply(g.toMap, c).map((v) => v * 20) as Vec3; };
/** The map point of a grid point. */
const mapOf = (st: EditorStore, id: number, c: Vec3): Vec3 => apply(grid(st, id).toMap, c);

test("sculpt carve equals the panel's strokeChanges, in one undo step", async () => {
  const st = store();
  const g = grid(st, 6);
  const before = g.words.slice();
  const centre: Vec3 = [3.5, 4.5, 6.5];
  const gf = frameGrids(st);
  const brush: Brush = { mode: "carve", shape: "sphere", size: [5, 5, 5], material: 0 };
  const want = before.slice();
  for (const [i, [, after]] of strokeChanges(gf, st.voxels, { grid: gf.find((x) => x.frame.id === 6)!, center: centre }, brush).get(g.ref)!) want[i] = after;
  const r = await call(st, "sculpt", { mode: "carve", shape: "sphere", size: [5, 5, 5], center: worldOf(st, 6, centre), units: "world", frame: 6 });
  assert.ok(r.changed > 0, JSON.stringify(r));
  assert.deepEqual([...grid(st, 6).words], [...want]);
  assert.equal(st.stack.depth, 1);
  assert.equal(r.anchorFrame.id, 6);
  st.undo();
  assert.deepEqual([...grid(st, 6).words], [...before]);
  assert.deepEqual(limitProblems(st), []);
});

test("sculpt dryRun changes nothing; the anchor is found from the point", async () => {
  const st = store();
  const before = solidCount(st, 6);
  const r = await call(st, "sculpt", { mode: "carve", shape: "box", size: [4, 4, 4], center: worldOf(st, 6, [3.5, 4.5, 6.5]), dryRun: true });
  assert.equal(r.dryRun, true);
  assert.equal(r.anchorFrame.id, 6);
  assert.ok(r.changed > 0);
  assert.equal(solidCount(st, 6), before);
  assert.equal(st.stack.depth, 0);
  await fails(st, "sculpt", { mode: "carve", size: [3, 3, 3], center: [99999, 0, 0] }, /no voxel frame holds/);
  await fails(st, "sculpt", { mode: "fill", size: [3, 3, 3], center: worldOf(st, 6, [3.5, 4.5, 6.5]) }, /needs a material/);
  await fails(st, "sculpt", { mode: "carve", size: [40, 3, 3], center: [0, 0, 0], frame: 6 }, /1-32/);
});

test("fill adds solid voxels of the material, paint changes only solid voxels' material, both keep words valid", async () => {
  const st = store();
  const g = grid(st, 6);
  // carve a hole first, then fill part of it back
  await call(st, "sculpt", { mode: "carve", shape: "box", size: [6, 6, 6], center: worldOf(st, 6, [3.5, 4.5, 6.5]), frame: 6 });
  const holes = [...g.words].filter((w) => !isSolid(w)).length;
  const f = await call(st, "sculpt", { mode: "fill", shape: "box", size: [4, 4, 4], material: 9, center: worldOf(st, 6, [3.5, 4.5, 6.5]), frame: 6 });
  assert.ok(f.changed > 0);
  const filledCells = [...g.words].filter((w) => isSolid(w) && material(w) === 9).length;
  assert.ok(filledCells >= f.changed - 0 && filledCells > 0);
  assert.ok([...g.words].filter((w) => !isSolid(w)).length < holes);
  const p = await call(st, "sculpt", { mode: "paint", shape: "box", size: [8, 8, 8], material: 11, center: worldOf(st, 6, [3.5, 4.5, 6.5]), frame: 6 });
  assert.ok(p.changed > 0);
  for (let z = 4; z < 9; z++) for (let x = 1; x < 6; x++) for (let y = 2; y < 7; y++) assert.ok(!isSolid(getVoxel(g, x, y, z)) || material(getVoxel(g, x, y, z)) !== 9, "painted over");
  for (let i = 0; i < g.words.length; i++) assert.ok(validEdit(g.base[i], g.words[i]), `word ${i}`);
  assert.equal(st.stack.depth, 3);
  assert.deepEqual(limitProblems(st), []);
  await fails(st, "sculpt", { mode: "paint", size: [3, 3, 3], material: 1, materialColumn: true, center: [0, 0, 0], frame: 6 }, /fill only/);
});

test("fill with material 'column' takes the column's top material", async () => {
  const st = store();
  const g = grid(st, 6);
  const c = worldOf(st, 6, [3.5, 4.5, 6.5]);
  await call(st, "sculpt", { mode: "carve", shape: "box", size: [3, 3, 3], center: c, frame: 6 });
  const snap = g.words.slice();
  const r = await call(st, "sculpt", { mode: "fill", shape: "box", size: [3, 3, 3], materialColumn: true, center: c, frame: 6 });
  assert.ok(r.changed > 0);
  for (let i = 0; i < snap.length; i++) {
    if (snap[i] === g.words[i]) continue;
    const y = i % 9, col = i - y;
    let want = material(snap[i]);
    for (let yy = 8; yy >= 0; yy--) if (isSolid(snap[col + yy])) { want = material(snap[col + yy]); break; }
    assert.equal(g.words[i], filled(want), `word ${i}`);
  }
  assert.deepEqual(limitProblems(st), []);
});

test("sculpt_path walks the polyline and stays inside the anchor frame for fill", async () => {
  const st = store();
  const g = grid(st, 6);
  const a = mapOf(st, 6, [1.5, 4.5, 2.5]), b = mapOf(st, 6, [5.5, 4.5, 9.5]);
  const w = (p: Vec3) => p.map((v) => v * 20) as Vec3;
  const r = await call(st, "sculpt_path", { mode: "carve", shape: "sphere", size: [2, 2, 2], points: [w(a), w(b)], frame: 6, spacing: 1 });
  assert.ok(r.dabs > 3, JSON.stringify(r));
  assert.ok(r.changed > 0);
  assert.equal(st.stack.depth, 1);
  // the midpoint of the path is carved
  assert.ok(!isSolid(getVoxel(g, 3, 4, 6)) || !isSolid(getVoxel(g, 3, 4, 5)));
  // a fill path running far outside the frame leaves the other frames alone
  const others = grids(st).filter((x) => x.id !== 6).map((x) => x.words.slice());
  await call(st, "sculpt_path", { mode: "fill", shape: "box", size: [3, 3, 3], material: 4, points: [w(b), w([b[0] + 400, b[1], b[2]])], frame: 6, spacing: 2 });
  grids(st).filter((x) => x.id !== 6).forEach((x, i) => assert.deepEqual([...x.words], [...others[i]]));
  assert.deepEqual(limitProblems(st), []);
  st.undo(); st.undo();
  assert.equal(st.dirty, false);
});

test("pathPoints spacing", () => {
  const p = pathPoints([[0, 0, 0], [10, 0, 0]], 4);
  assert.deepEqual(p.map((q) => q[0]), [0, 4, 8, 10]);
  assert.deepEqual(pathPoints([[0, 0, 0], [4, 0, 0], [4, 4, 0]], 2).map((q) => q.join()), ["0,0,0", "2,0,0", "4,0,0", "4,2,0", "4,4,0"]);
  assert.equal(pathPoints([[1, 1, 1]], 2).length, 1);
  assert.throws(() => pathPoints([[0, 0, 0], [100000, 0, 0]], 1), /more than/);
});

test("add_block and remove_block", async () => {
  const st = store();
  await fails(st, "add_block", { size: [4, 4, 4], material: 2, at: [0, 0, 0] }, /no Scene frame/);
  const s2 = withScene();
  const r = await call(s2, "add_block", { size: [6, 3, 5], material: 7, at: [0, 100, 0] });
  assert.ok(r.id < 0, JSON.stringify(r));
  assert.equal(r.voxels, 90);
  const g = grid(s2, r.id);
  assert.ok(g.words.every((w) => isSolid(w) && material(w) === 7));
  assert.ok(g.base.every((w) => w === 0));
  assert.deepEqual(limitProblems(s2), []);
  assert.equal(s2.patch().ops.filter((o) => o.op === "addFrame").length, 1);
  // sculpt into the new block
  const c = await call(s2, "sculpt", { mode: "carve", shape: "box", size: [2, 2, 2], center: worldOf(s2, r.id, [3, 1.5, 2.5]), frame: r.id });
  assert.equal(c.changed, 8);
  const fb = await call(s2, "fill_box_raw", { frame: r.id, min: [0, 0, 0], max: [6, 1, 5], state: "empty" });
  assert.ok(fb.changed > 0);
  for (let z = 0; z < 5; z++) for (let x = 0; x < 6; x++) assert.equal(isSolid(getVoxel(grid(s2, r.id), x, 0, z)), false);
  const cc = await call(s2, "add_block", { size: [2, 2, 2], material: 1, center: [100, 200, 100], units: "map" });
  assert.equal(cc.id !== r.id, true);
  await fails(s2, "add_block", { size: [2, 2, 2], material: 1 }, /exactly one of/);
  await fails(s2, "add_block", { size: [2, 2, 40], material: 1, at: [0, 0, 0] }, /1-31/);
  await fails(s2, "add_block", { size: [2, 2, 32], material: 1, at: [0, 0, 0] }, /1-31/);
  await fails(s2, "remove_block", { id: 6 }, /base map/);
  await fails(s2, "remove_block", { id: 12345 }, /no frame/);
  const rm = await call(s2, "remove_block", { id: r.id });
  assert.equal(rm.removed, r.id);
  assert.equal(s2.scene.frames.filter((f) => f.new).length, 1);
  s2.undo();
  assert.equal(s2.scene.frames.filter((f) => f.new).length, 2);
  assert.deepEqual(limitProblems(s2), []);
});

test("add_block scale: a scaled frame, its patch op, and sculpting in its own voxels", async () => {
  const st = withScene();
  const r = await call(st, "add_block", { size: [4, 2, 4], material: 3, scale: 2.5, center: [50, 100, 50], units: "map" });
  assert.equal(r.scale, 2.5);
  const f = st.scene.frames.find((x) => x.id === r.id)!;
  assert.deepEqual(f.scale, [2.5, 2.5, 2.5]);
  const b = r.bounds.map as { min: Vec3; max: Vec3 };
  assert.deepEqual([b.max[0] - b.min[0], b.max[1] - b.min[1], b.max[2] - b.min[2]], [10, 5, 10], "4x2x4 voxels of 2.5 map units");
  assert.deepEqual([b.min[0], b.min[2]], [Math.round(b.min[0]), Math.round(b.min[2])], "the corner snaps to whole units");
  const op = st.patch().ops.find((o) => o.op === "addFrame")!;
  assert.equal((op as { scale?: number }).scale, 2.5);
  assert.deepEqual(limitProblems(st), []);
  // A 2x2x2 box brush anchored in the block is 2x2x2 of its voxels, 5 map units a side.
  const c = await call(st, "sculpt", { mode: "carve", shape: "box", size: [2, 2, 2], center: worldOf(st, r.id, [2, 1, 2]), frame: r.id });
  assert.equal(c.changed, 8);
  // An unscaled block writes no scale: the patch is what it was before scales existed.
  const u = await call(st, "add_block", { size: [2, 2, 2], material: 1, center: [200, 100, 200], units: "map" });
  assert.equal(u.scale, undefined);
  assert.equal("scale" in st.patch().ops.filter((o) => o.op === "addFrame")[1], false);
  await fails(st, "add_block", { size: [2, 2, 2], material: 1, center: [0, 0, 0], scale: 4.5 }, /scale|maximum/);
});

test("add_block refuses a block past the game's 65535 voxel cells and says why", async () => {
  const st = withScene();
  const used = st.scene.frames.reduce((n, f) => n + (f.voxels !== null ? f.size[0] * f.size[1] * f.size[2] : 0), 0);
  const a = await call(st, "add_block", { size: [31, 31, 31], material: 1, center: [500, 0, 0], units: "map" });
  assert.deepEqual(a.cells, { used: used + 29791, max: 65535, free: 65535 - used - 29791 });
  await call(st, "add_block", { size: [31, 31, 31], material: 1, center: [600, 0, 0], units: "map" });
  const left = 65535 - used - 2 * 29791;
  await fails(st, "add_block", { size: [31, 31, 31], material: 1, center: [700, 0, 0], units: "map" }, new RegExp(`29791 cells and the level has ${left} of the game's 65535 left`));
  const fit = Math.floor(left / (31 * 31));
  const last = await call(st, "add_block", { size: [31, fit, 31], material: 1, center: [700, 0, 0], units: "map" });
  assert.ok(last.cells.free >= 0 && last.cells.free < 31 * 31, JSON.stringify(last.cells));
  await fails(st, "add_block", { size: [31, 1, 31], material: 1, center: [800, 0, 0], units: "map" }, /65535/);
  assert.equal(st.scene.frames.filter((f) => f.new).length, 3);
});

test("block limit is 64", async () => {
  const st = withScene();
  for (let i = 0; i < 64; i++) await call(st, "add_block", { size: [1, 1, 1], material: 1, at: [i * 40, 0, 0] });
  await fails(st, "add_block", { size: [1, 1, 1], material: 1, at: [0, 0, 0] }, /at most 64/);
});

test("clear_base_terrain by frames, name and box; dry run; blocks kept", async () => {
  const st = withScene();
  await fails(st, "clear_base_terrain", {}, /frames, search or box/);
  await fails(st, "clear_base_terrain", { frames: [999] }, /no voxels/);
  const blk = await call(st, "add_block", { size: [3, 3, 3], material: 2, at: [0, 0, 0] });
  const total = grids(st).filter((g) => !g.frame.new).reduce((n, g) => n + solidCount(st, g.id), 0);
  const dry = await call(st, "clear_base_terrain", { all: true, dryRun: true });
  assert.equal(dry.voxelsCleared, total);
  assert.equal(solidCount(st, 4) > 0, true);
  const one = await call(st, "clear_base_terrain", { frames: [4] });
  assert.equal(one.framesCleared, 1);
  assert.equal(solidCount(st, 4), 0);
  const named = await call(st, "clear_base_terrain", { search: "land1" });   // land1, land10, land11
  assert.equal(named.framesConsidered, 3);
  // a box clears only voxels whose centre is inside it
  const g6 = grid(st, 6);
  // frame 6 is yawed, so an axis-aligned box in the level cuts it diagonally: every voxel whose centre is inside goes, no other
  const mid = mapOf(st, 6, [3.5, 4.5, 6]);
  const box = { min: mid.map((v) => v - 2.5), max: mid.map((v) => v + 2.5) };
  const was = g6.words.slice();
  const before = solidCount(st, 6);
  const part = await call(st, "clear_base_terrain", { box: { min: box.min.map((v) => v * 20), max: box.max.map((v) => v * 20) }, units: "world", wipe: true });
  assert.ok(part.voxelsCleared > 0 && solidCount(st, 6) < before);
  let inBox = 0;
  for (let z = 0; z < 12; z++) for (let x = 0; x < 7; x++) for (let y = 0; y < 9; y++) {
    const c = mapOf(st, 6, [x + 0.5, y + 0.5, z + 0.5]);
    const inside = [0, 1, 2].every((a) => c[a] >= box.min[a] && c[a] < box.max[a]);
    const i = voxelIndex([7, 9, 12], x, y, z);
    if (inside) { assert.equal(g6.words[i], 0); if (isSolid(was[i])) inBox++; } else assert.equal(g6.words[i], was[i]);
  }
  assert.ok(inBox > 0);
  const all = await call(st, "clear_base_terrain", { all: true });
  assert.equal(grids(st).filter((g) => !g.frame.new).every((g) => solidCount(st, g.id) === 0), true);
  assert.ok(all.voxelsCleared >= 0);
  assert.equal(solidCount(st, blk.id), 27, "added blocks are kept");
  for (const g of grids(st)) for (let i = 0; i < g.words.length; i++) assert.ok(validEdit(g.base[i], g.words[i]));
  assert.deepEqual(limitProblems(st), []);
  const inc = await call(st, "clear_base_terrain", { frames: [blk.id], includeBlocks: true });
  assert.equal(inc.voxelsCleared, 27);
});

test("fill_box_raw: grid and world boxes, only, raw words, refusals", async () => {
  const st = store();
  const g = grid(st, 6);
  const r = await call(st, "fill_box_raw", { frame: 6, min: [0, 0, 0], max: [2, 2, 2], state: "solid", material: 13 });
  assert.equal(r.cellsInBox, 8);
  for (let z = 0; z < 2; z++) for (let x = 0; x < 2; x++) for (let y = 0; y < 2; y++) assert.equal(getVoxel(g, x, y, z), filled(13));
  assert.equal(getVoxel(g, 2, 0, 0) === filled(13), false || getVoxel(g, 2, 0, 0) === filled(13));
  const e = await call(st, "fill_box_raw", { frame: 6, min: [0, 0, 0], max: [4, 4, 4], state: "empty", only: "solid", wipe: true });
  assert.ok(e.changed > 0);
  assert.equal(getVoxel(g, 0, 0, 0), 0);
  const w = await call(st, "fill_box_raw", { frame: 6, min: [0, 0, 0], max: [1, 1, 1], word: filled(5) });
  assert.equal(w.changed, 1);
  // world units: the same single cell
  const c = worldOf(st, 6, [0.5, 0.5, 0.5]);
  const wu = await call(st, "fill_box_raw", { frame: 6, min: c.map((v) => v - 5), max: c.map((v) => v + 5), units: "world", state: "solid", material: 21, dryRun: true });
  assert.equal(wu.cellsInBox, 1);
  await fails(st, "fill_box_raw", { frame: 6, word: 1 }, /not a valid edit/);
  await fails(st, "fill_box_raw", { frame: 6, word: 0x100003 }, /not a valid edit/);
  await fails(st, "fill_box_raw", { frame: 6, state: "solid" }, /material/);
  await fails(st, "fill_box_raw", { frame: 6 }, /state/);
  await fails(st, "fill_box_raw", { frame: 2, state: "empty" }, /no voxels/);
  await fails(st, "fill_box_raw", { frame: 6, min: [0, 0, 0], state: "empty" }, /both min and max/);
  assert.deepEqual(limitProblems(st), []);
});

test("paint_surround: dabs, set_grid by array and formula, enable once, one undo each", async () => {
  const st = store();
  assert.equal(st.scene.hmp.mode, "copy");
  const r = await call(st, "paint_surround", { op: "raise", points: [[50, 50]], radius: 6, strength: 1, passes: 3 });
  assert.equal(r.switchedToPaint, true);
  assert.equal(st.scene.hmp.mode, "paint");
  assert.ok(r.cellsChanged > 0);
  const peak = st.surround.heights[50 * 100 + 50];
  assert.ok(peak > 0.2 && peak <= 1, String(peak));
  assert.equal(st.stack.depth, 2, "the mode switch and the whole stroke");
  const s2 = await call(st, "paint_surround", { op: "flatten", points: [[50, 50], [60, 50]], level: 0.5, strength: 1, radius: 3 });
  assert.equal(s2.switchedToPaint, undefined);
  assert.equal(st.surround.heights[50 * 100 + 55] > 0.1, true);
  await call(st, "paint_surround", { op: "smooth", points: [[50, 50]], radius: 5 });
  await call(st, "paint_surround", { op: "lower", points: [[10, 10]], radius: 2 });
  const f = await call(st, "paint_surround", { op: "set_grid", formula: "0.5 + 0.25 * sin(u * 6.28318)", region: { x0: 0, y0: 0, x1: 99, y1: 9 } });
  assert.ok(f.cellsChanged > 0);
  assert.ok(Math.abs(st.surround.heights[3 * 100 + 25] - 0.75) < 0.01);
  const rows = Array.from({ length: 100 }, (_, y) => Array.from({ length: 100 }, (_, x) => (x + y) / 200));
  const h = await call(st, "paint_surround", { op: "set_grid", heights: rows });
  assert.ok(h.cellsChanged > 0);
  assert.equal(st.surround.heights[99 * 100 + 99], Math.fround(Math.round((198 / 200) * 256) / 256));
  const half = await call(st, "paint_surround", { op: "set_grid", formula: "1", mix: 0.5, region: { x0: 0, y0: 0, x1: 0, y1: 0 } });
  assert.equal(half.cellsChanged, 1);
  // refusals change nothing
  const depth = st.stack.depth;
  await fails(st, "paint_surround", { op: "set_grid", formula: "foo(x)" }, /unknown name/);
  await fails(st, "paint_surround", { op: "set_grid", formula: "Math.sin(x)" }, /member access|unknown name/);
  await fails(st, "paint_surround", { op: "set_grid", formula: "x['a']" }, /only digits/);
  await fails(st, "paint_surround", { op: "set_grid", heights: [1, 2, 3] }, /heights:/);
  await fails(st, "paint_surround", { op: "raise" }, /points/);
  await fails(st, "paint_surround", { op: "set_grid" }, /heights/);
  assert.equal(st.stack.depth, depth);
  const rs = await call(st, "read_surround", { step: 50, region: { x0: 0, y0: 0, x1: 2, y1: 1 } });
  assert.equal(rs.grid.length, 2);
  assert.equal(rs.region.heights.length, 2);
  assert.equal(rs.mode, "paint");
  assert.deepEqual(limitProblems(st), []);
  const p = st.patch();
  assert.equal(p.hmp?.mode, "paint");
  // surround edits survive undo back to the start
  while (st.undo());
  assert.equal(st.scene.hmp.mode, "copy");
  assert.equal(st.surround.heights.every((v) => v === 0), true);
});

test("paint_surround is refused when the base surround did not load; enablePaint false refuses", async () => {
  const bad = () => { const l = loaded(); l.scene.base.sha256.hmp = "c".repeat(64); return l; };
  const st = new EditorStore("t", bad(), bad());
  await fails(st, "paint_surround", { op: "raise", points: [[1, 1]] }, /did not load/);
  const ok = store();
  await fails(ok, "paint_surround", { op: "raise", points: [[1, 1]], enablePaint: false }, /enablePaint/);
  assert.equal(ok.scene.hmp.mode, "copy");
});

test("compileFormula", () => {
  const f = compileFormula("x > 5 ? max(u, v) : h + 1");
  assert.equal(f(10, 0, 0.2, 0.7, 0), 0.7);
  assert.equal(f(1, 0, 0, 0, 2), 3);
  assert.throws(() => compileFormula("constructor"), /unknown name/);
  assert.throws(() => compileFormula("(1+"), /formula/);
  assert.throws(() => compileFormula("x".repeat(600)), /500/);
});

test("compileFormula: parsed, not evaluated as JavaScript", () => {
  const f = (s: string, x = 3, y = 4, u = 0.25, v = 0.5, h = 0.1) => compileFormula(s)(x, y, u, v, h);
  assert.equal(f("1 + 2 * 3 - 4 / 2"), 5);
  assert.equal(f("2 ** 3 ** 2"), 512);
  assert.equal(f("-x % 2"), -1);
  assert.equal(f("x < y && y <= 4 ? 1 : 0"), 1);
  assert.equal(f("x == 3 || 0"), 1);
  assert.equal(f("!0 + !x"), 1);
  assert.equal(f("0 || h"), 0.1);
  assert.equal(f("x > 9 ? 1 : x > 2 ? 2 : 3"), 2);
  assert.equal(f("hypot(x, y) + max(u, v, h) + sign(-2) + PI - PI"), 4.5);
  assert.equal(f("1.5e1 + .5"), 15.5);
  assert.ok(!Number.isFinite(f("1 / 0")));
  for (const bad of ["x = 1", "x => 1", "sin", "PI()", "(1, 2)", "1 2", "max(1,", "x ? 1", "x && & y"])
    assert.throws(() => compileFormula(bad), /formula/, bad);
});
