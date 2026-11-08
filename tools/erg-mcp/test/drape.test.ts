// heightfield_drape on the synthetic 12-frame level (its root renamed Scene): skin blocks carry corner offsets that put
// the surface on the field's heights, the core stays under the skin, the patch carries the offsets (addFrame corners)
// and the ground readers (query_ground's spans, drops) see the sheared height.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, applyPatch, cornerOffset, dropPoint, validatePatch, type Loaded, type Scene } from "../src/core";
import { applyDrape, planDrape } from "../src/lib/heightfield-drape";
import { spansAt } from "../src/lib/inspect-field";
import { grids } from "../src/lib/voxel";

const scene = (): Scene => JSON.parse(readFileSync(new URL("../../../../tests/fixtures/erg/synthetic-12.json", import.meta.url), "utf8"));
function loaded(): Loaded {
  const s = scene();
  s.frames.find((f) => f.id === 1)!.name = "Scene";
  const blobs = new Map<number, ArrayBuffer>();
  for (const [ref, buf] of syntheticBlobs(s) as Map<number, Uint8Array>) blobs.set(ref, buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength) as ArrayBuffer);
  return { scene: s, blobs };
}
const store = () => new EditorStore("t", loaded(), loaded());

test("cornerOffset: bilinear over a column's corners, 0 without corners", () => {
  const f = { size: [2, 1, 1] as [number, number, number], corners: [0, 1, 2, 4, 5, 6] };
  assert.equal(cornerOffset(f, 0, 0), 0);
  assert.equal(cornerOffset(f, 1, 1), 5);
  assert.equal(cornerOffset(f, 0.5, 0.5), (0 + 1 + 4 + 5) / 4);
  assert.equal(cornerOffset(f, 2, 0), 2);
  assert.equal(cornerOffset({ size: [2, 1, 1] }, 0.5, 0.5), 0);
});

test("a cone drapes: skin blocks follow the surface within a fraction of a voxel, the core stays under it, one undo step", () => {
  const st = store();
  const cone = (r: number) => Math.max(0, 30 - r * 0.8);
  const p = planDrape(st, { min: [100, 100], max: [160, 160], floor: 0, expr: "max(0, 30 - r*0.8)", material: 2, voxelSize: 2, thickness: 2 });
  assert.equal(p.report.fits, true, String(p.report.why));
  assert.ok(p.tiles.length >= 1 && p.core && p.core.kept.length >= 1);
  const r = applyDrape(st, p);
  assert.equal(st.stack.depth, 1);
  assert.deepEqual(st.problems(), []);
  const skin = grids(st).filter((g) => g.frame.corners);
  assert.equal(skin.length, p.tiles.length);
  assert.ok(r.blocks.some((b) => b.skin) && r.blocks.some((b) => !b.skin));
  // The ground at points across the cone is the skin's top, close to the field (corner means smooth the apex).
  let worst = 0;
  for (const [x, z] of [[130, 130], [140, 128], [118, 137], [150, 150], [126, 112]]) {
    const top = spansAt(grids(st), x, z)[0];
    assert.ok(top, `ground at ${x}, ${z}`);
    assert.ok(skin.some((g) => g.id === top.frame), `the skin is the top at ${x}, ${z}`);
    worst = Math.max(worst, Math.abs(top.top - cone(Math.hypot(x - 130, z - 130))));
  }
  assert.ok(worst < 2, `the skin is within 2 map units of the cone (worst ${worst})`);
  // A drop lands on the sheared skin, not on the unsheared grid.
  const d = dropPoint(st.scene, st.frames, (id) => st.voxelsOf({ id }), [140, 100, 128]);
  assert.ok(d && Math.abs(d[1] - spansAt(grids(st), 140, 128)[0].top) < 1e-6, `drop ${d}`);
  // The patch carries the corners, validates, and applying it gives the same frames back.
  const patch = st.patch();
  const adds = patch.ops.filter((o) => o.op === "addFrame");
  assert.equal(adds.filter((o) => o.op === "addFrame" && o.corners).length, p.tiles.length);
  assert.ok(validatePatch(patch).ok, JSON.stringify(validatePatch(patch)));
  const again = applyPatch(st.base, patch);
  assert.equal(again.frames.filter((f) => f.corners).length, p.tiles.length);
  st.undo();
  assert.equal(grids(st).filter((g) => g.frame.new).length, 0);
});

test("core: false is a hollow skin in a fraction of the cells; a later heightfield does not fill a draped block", () => {
  const st = store();
  const a = { min: [100, 100] as [number, number], max: [160, 160] as [number, number], floor: 0, expr: "max(0, 30 - r*0.8)", voxelSize: 2 };
  const solid = planDrape(st, a), hollow = planDrape(st, { ...a, core: false });
  const cells = (q: typeof solid) => (q.report.budget as { cells: { after: number; before: number } }).cells;
  assert.ok(cells(hollow).after - cells(hollow).before < (cells(solid).after - cells(solid).before) / 2);
  applyDrape(st, hollow);
  assert.equal(grids(st).filter((g) => g.frame.new).length, hollow.tiles.length);
  assert.throws(() => planDrape(st, { ...a, thickness: 9 }), /thickness/);
});
