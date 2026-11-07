// src/lib/voxel.ts on the synthetic 12-frame level, bundled for Node by test.ps1 into test/out (paths are from there): grids and coordinates, VoxelEdit as one
// undo step, the runs toPatch writes, and the limit checks.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, LIMITS, filled, isSolid, planBlock, type Loaded, type Scene } from "../src/core";
import {
  VoxelEdit, cellAt, cellCentre, columnRuns, encodeRuns, getVoxel, grid, grids, gridsAt, gridToWorld, limitProblems, mapToGrid, newFrameProblem,
  runBytes, setVoxel, toMap, toWorld, usage, voxelIndex, worldToGrid,
} from "../src/lib/voxel";

const scene = (): Scene => JSON.parse(readFileSync(new URL("../../../../tests/fixtures/erg/synthetic-12.json", import.meta.url), "utf8"));

function loaded(edit?: (s: Scene) => void): Loaded {
  const s = scene();
  edit?.(s);
  const blobs = new Map<number, ArrayBuffer>();
  for (const [ref, buf] of syntheticBlobs(s) as Map<number, Uint8Array>) blobs.set(ref, buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength) as ArrayBuffer);
  return { scene: s, blobs };
}

const store = (edit?: (s: Scene) => void) => new EditorStore("t", loaded(edit), loaded(edit));

test("grids, indices and coordinates", () => {
  const st = store();
  const gs = grids(st);
  assert.equal(gs.length, 12);
  const g = grid(st, 4);
  assert.deepEqual(g.size, [7, 2, 11]);
  assert.equal(voxelIndex(g.size, 3, 1, 5), (5 * 7 + 3) * 2 + 1);
  assert.equal(getVoxel(g, 3, 1, 5), g.words[voxelIndex(g.size, 3, 1, 5)]);
  assert.throws(() => getVoxel(g, 7, 0, 0));
  assert.throws(() => grid(st, 9999));
  const c = cellCentre(g, 3, 1, 5);
  assert.deepEqual(cellAt(g, c), [3, 1, 5]);
  const back = mapToGrid(g, c);
  for (let a = 0; a < 3; a++) assert.ok(Math.abs(back[a] - [3.5, 1.5, 5.5][a]) < 1e-9);
  const w = gridToWorld(g, [3.5, 1.5, 5.5]);
  for (let a = 0; a < 3; a++) assert.ok(Math.abs(w[a] - c[a] * 20) < 1e-9);
  const g2 = worldToGrid(g, w);
  for (let a = 0; a < 3; a++) assert.ok(Math.abs(g2[a] - [3.5, 1.5, 5.5][a]) < 1e-9);
  assert.deepEqual(toMap(toWorld([1, 2, 3])), [1, 2, 3]);
  assert.ok(gridsAt(st, c).some((h) => h.grid.id === 4));
});

test("VoxelEdit: one undo step, the patch's runs, redo", () => {
  const st = store();
  const g = grid(st, 6);
  const e = new VoxelEdit(st);
  const before = getVoxel(g, 0, 0, 0);
  e.set(6, 0, 0, 0, 0);
  e.set(6, 1, 0, 0, filled(5));
  e.set(6, 1, 0, 0, getVoxel(g, 1, 0, 0));   // back to its word: not a change
  assert.equal(e.size, before === 0 ? 0 : 1);
  assert.equal(e.get(6, 0, 0, 0), 0);
  assert.throws(() => e.set(6, 0, 0, 0, 1), /not a voxel word/);
  e.set(6, 2, 8, 3, filled(7));
  const r = e.commit("Test edit");
  assert.ok(r.changed >= 1);
  assert.deepEqual(r.frames, [6]);
  assert.equal(getVoxel(g, 2, 8, 3), filled(7));
  assert.equal(st.stack.depth, 1);
  assert.equal(st.stack.peek()?.label, "Test edit");
  assert.ok(st.dirty);
  const p = st.patch();
  const op = p.ops.find((o) => o.op === "voxels");
  assert.ok(op && op.op === "voxels" && op.frame === 6);
  if (op?.op === "voxels") assert.deepEqual(op.runs, columnRuns(g.base, g.words));
  st.undo();
  assert.equal(getVoxel(g, 2, 8, 3), g.base[voxelIndex(g.size, 2, 8, 3)]);
  assert.equal(st.dirty, false);
  st.redo();
  assert.equal(getVoxel(g, 2, 8, 3), filled(7));
  assert.equal(setVoxel(st, 6, 2, 8, 3, filled(7)), false);
  assert.equal(setVoxel(st, 6, 2, 8, 3, 0), true);
  assert.equal(st.stack.depth, 2);
});

test("VoxelEdit refuses more than 2000 runs in a frame", () => {
  const st = store((s) => { s.frames.find((f) => f.id === 10)!.size = [20, 20, 20]; });
  const g = grid(st, 10);
  const e = new VoxelEdit(st);
  // Every other word changes: one run each.
  for (let i = 0; i < 2 * (LIMITS.runsPerFrame + 1); i += 2) {
    const y = i % 20, x = Math.floor(i / 20) % 20, z = Math.floor(i / 400);
    e.set(10, x, y, z, isSolid(getVoxel(g, x, y, z)) ? 0 : filled(1));
  }
  assert.match(e.check() ?? "", /would need 2001 runs/);
  assert.throws(() => e.commit(), /2001 runs/);
  assert.equal(st.stack.depth, 0, "a refused batch is not applied");
});

test("runs, usage and limits", () => {
  const st = store();
  const words = new Uint32Array([0, 3, 3, 0, 7]);
  assert.deepEqual(encodeRuns(words), [[1, 2, 3], [4, 1, 7]]);
  assert.equal(runBytes([[1, 2, 3]]), JSON.stringify([1, 2, 3]).length + 1);
  const u = usage(st);
  assert.equal(u.ops.used, 0);
  assert.equal(u.newFrames.used, 0);
  assert.deepEqual(limitProblems(st), []);
  assert.equal(newFrameProblem(st, [4, 4, 4]), null);
  assert.ok(newFrameProblem(st, [33, 1, 1]));
  assert.match(String(planBlock(st.scene, st.voxels, st.freshRef(), [0, 5, 0], [4, 4, 4], 2)), /no Scene frame/);
  st.scene.frames.find((f) => f.id === 1)!.name = "Scene";   // the synthetic level has none; blocks go under it
  const cmd = planBlock(st.scene, st.voxels, st.freshRef(), [0, 5, 0], [4, 4, 4], 2);
  assert.ok(typeof cmd !== "string", String(cmd));
  st.exec(cmd);
  assert.equal(usage(st).newFrames.used, 1);
  const b = grids(st).find((x) => x.frame.new)!;
  assert.equal(b.words.length, 64);
  assert.ok(b.base.every((v) => v === 0));
});
