// Detail layer round 8: lib/prop-support.ts (lint_support), lib/tint-jitter.ts (tint_variants), lib/ship-gate.ts (export gate).
import assert from "node:assert/strict";
import { test } from "node:test";
import { supportOf, undersideAt, type SupportClump } from "../src/lib/prop-support";
import { hexToRgb, jitterTint, rgbToHsv, seeded, tintVariants } from "../src/lib/tint-jitter";
import { shipRange, VANILLA_SAFE_DETAILS, MESH_POOL_MELANGE } from "../src/lib/ship-gate";

// a 2 x 2 x 2 box of vertices on a 5 x 5 grid at its bottom (y0) and top (y0 + 2), centred at (cx, cz)
function box(id: number, cx: number, cz: number, y0: number, w = 2): SupportClump {
  const verts: number[] = [];
  for (let i = 0; i <= 4; i++) for (let k = 0; k <= 4; k++) for (const y of [y0, y0 + w]) verts.push(cx - w / 2 + (w * i) / 4, y, cz - w / 2 + (w * k) / 4);
  return { ids: [id], verts, min: [cx - w / 2, y0, cz - w / 2], max: [cx + w / 2, y0 + w, cz + w / 2] };
}
// a 60 degree face falling toward +x: y = 10 - tan(60) * x; its normal leans toward +x (downhill)
const T60 = Math.tan((60 * Math.PI) / 180);
const n60 = [Math.sin((60 * Math.PI) / 180), Math.cos((60 * Math.PI) / 180), 0];
const face = (x: number) => ({ y: 10 - T60 * x, slope: 60, normal: n60 });

test("underside", () => {
  const b = box(1, 0, 0, 3);
  assert.equal(undersideAt(b.verts, 0, 0, 0.3), 3);
  assert.equal(undersideAt(b.verts, 9, 9, 0.3), null);
});

test("flat ground is not judged; a box resting on a steep face is supported by the ground", () => {
  const flat = supportOf(box(1, 0, 0, 0), () => ({ y: 0, slope: 5 }), []);
  assert.equal(flat.by, "flat"); assert.ok(flat.supported);
  // a box sunk into the face: its underside (y0) under the face at most samples
  const sunk = supportOf(box(1, 0, 0, 10 - T60 * 1.2), (x) => face(x), []);
  assert.equal(sunk.by, "ground", JSON.stringify(sunk)); assert.ok(sunk.contact >= 0.6);
});

test("a box hanging off a steep face is unsupported, unless talus under its downhill edge holds it", () => {
  // its underside at the face's height at its uphill edge: most of its footprint is over air
  const hang = box(1, 0, 0, 10 - T60 * -1);
  const v = supportOf(hang, (x) => face(x), []);
  assert.equal(v.supported, false, JSON.stringify(v)); assert.ok(v.contact < 0.6);
  // a talus block downhill (+x) reaching up to the underside
  const talus = box(2, 0.9, 0, hang.min[1] - 1.9);
  const w = supportOf(hang, (x) => face(x), [hang, talus]);
  assert.equal(w.by, "talus"); assert.deepEqual(w.holder, [2]);
  // the same block UPHILL does not hold it
  const up = box(3, -0.9, 0, hang.min[1] - 1.9);
  assert.equal(supportOf(hang, (x) => face(x), [hang, up]).supported, false);
});

test("tint jitter stays near the colour, is seeded, and darkens toward the edge", () => {
  const a = tintVariants(["#4f9a3a"], 20, 7)[0], b = tintVariants(["#4f9a3a"], 20, 7)[0];
  assert.deepEqual(a, b);
  assert.ok(new Set(a).size > 10);
  const [h0, , v0] = rgbToHsv(hexToRgb("#4f9a3a"));
  for (const c of a) {
    const [h, , v] = rgbToHsv(hexToRgb(c));
    assert.ok(Math.abs(h - h0) <= 8.5, c); assert.ok(Math.abs(v - v0) <= v0 * 0.15, c);
  }
  const r = seeded(3), core = jitterTint("#4f9a3a", r, { hue: 0, sat: 0, value: 0 }), rim = jitterTint("#4f9a3a", r, { hue: 0, sat: 0, value: 0, falloff: 1 });
  assert.equal(core, "#4f9a3a");
  assert.ok(rgbToHsv(hexToRgb(rim))[2] < rgbToHsv(hexToRgb(core))[2]);
});

test("ship gate", () => {
  assert.equal(shipRange(VANILLA_SAFE_DETAILS).gated, false);
  assert.equal(shipRange(VANILLA_SAFE_DETAILS).melangeRange, ">=0.2.0");
  const g = shipRange(1000);
  assert.ok(g.gated); assert.equal(g.melangeRange, `>=${MESH_POOL_MELANGE}`);
  assert.equal(shipRange(10, ">=0.13.0").melangeRange, ">=0.13.0");
});
