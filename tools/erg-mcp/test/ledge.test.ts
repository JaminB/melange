// Detail layer round 6: surface-aligned slabs, strata bands, crests and the perched-from-view lint (src/lib/ledge.ts).
// Bundled for Node by test.ps1.
import assert from "node:assert/strict";
import { test } from "node:test";
import { alignedPose, chain, findCrests, perchedFromView, planBands, surfaceFrame, type Ground, type V3 } from "../src/lib/ledge";
import { apply, rotMat } from "../src/lib/scatter";

const DEG = 180 / Math.PI;
const unit = { min: [-1, 0, -1] as V3, max: [1, 2, 1] as V3 };          // a 2 x 2 x 2 mesh whose origin is at its base

// a 60-degree face falling toward +x
const tan60 = Math.tan(60 / DEG);
const face = (x: number, _z: number): Ground => { const n = Math.hypot(tan60, 1); return { y: 50 - x * tan60, slope: 60, normal: [tan60 / n, 1 / n, 0] }; };

test("surfaceFrame: x along the contour, z out of the face pitched toward the normal, right-handed", () => {
  const f = surfaceFrame(face(0, 0).normal, 0);
  const col = (j: number): V3 => [f[0][j], f[1][j], f[2][j]];
  assert.ok(Math.abs(col(0)[1]) < 1e-9 && Math.abs(col(0)[0]) < 1e-9, "x is horizontal along the contour (z axis)");
  assert.ok(col(2)[0] > 0.999, "pitch 0: z is the horizontal downhill");
  assert.ok(col(1)[1] > 0.999, "pitch 0: y is world up (a level shelf)");
  const f1 = surfaceFrame(face(0, 0).normal, 1), z1: V3 = [f1[0][2], f1[1][2], f1[2][2]];
  assert.ok(Math.abs(z1[0] - face(0, 0).normal[0]) < 1e-9 && Math.abs(z1[1] - face(0, 0).normal[1]) < 1e-9, "pitch 1: z is the normal");
  // determinant +1
  const det = f[0][0] * (f[1][1] * f[2][2] - f[1][2] * f[2][1]) - f[0][1] * (f[1][0] * f[2][2] - f[1][2] * f[2][0]) + f[0][2] * (f[1][0] * f[2][1] - f[1][1] * f[2][0]);
  assert.ok(Math.abs(det - 1) < 1e-9);
  const ft = surfaceFrame(face(0, 0).normal, 0, [0, -1]);
  assert.ok(ft[2][0] < -0.999, "a tangent picks the contour's direction");
});

test("alignedPose: the slab's size, its rotation round-trips through the engine's order, and embed sinks its depth into the face", () => {
  const f = surfaceFrame(face(0, 0).normal, 0);
  const p = alignedPose(unit, [0, 50, 0], f, [6, 1.2, 2], 0.5);
  assert.deepEqual(p.scale.map((v) => +v.toFixed(6)), [3, 0.6, 1]);
  const r = rotMat(p.rot);
  for (let i = 0; i < 3; i++) for (let j = 0; j < 3; j++) assert.ok(Math.abs(r[i][j] - f[i][j]) < 1e-9, "eulerOf(frame) composes back to the frame");
  assert.ok(Math.abs(p.centre[0]) < 1e-9 && Math.abs(p.centre[1] - 50) < 1e-9, "embed 0.5: the box centre on the surface point");
  assert.equal(p.front, 1);
  // the origin is the mesh's base: centre - R * (mid * s) with mid (0, 1, 0)
  const off = apply(f, [0, 0.6, 0]);
  assert.ok(Math.abs(p.pos[1] - (50 - off[1])) < 1e-9);
  const deep = alignedPose(unit, [0, 50, 0], f, [6, 1.2, 2], 0.8);
  assert.ok(deep.centre[0] < -0.59 && deep.centre[0] > -0.61, "embed 0.8: the centre 0.3 of the depth into the face");
  assert.ok(Math.abs(deep.front - 0.4) < 1e-9);
});

test("planBands: one face, bands at shared heights, slabs along the contour with their tangent on it", () => {
  const plan = planBands(face, { box: [-10, -12, 10, 12] }, { step: 1, minSlope: 50, spacing: 8, offset: 2, length: [3, 3], gap: [0.5, 0.5], breaks: 0, seed: 1 });
  assert.equal(plan.faces.length, 1);
  const hs = plan.faces[0].heights;
  assert.ok(hs.length >= 2, `two or more bands (${hs})`);
  for (const h of hs) assert.equal((h - 2) % 8, 0, "level strata: every `spacing` from `offset`");
  assert.ok(plan.slabs.length >= hs.length * 4, `slabs along every band (${plan.slabs.length})`);
  for (const s of plan.slabs) {
    assert.ok(Math.abs(s.at[1] - s.height) < 0.7, `on its band (${s.at[1]} vs ${s.height})`);
    assert.ok(Math.abs(s.tangent[0]) < 0.2, "the tangent runs along the contour (z)");
  }
  const counted = planBands(face, { box: [-10, -12, 10, 12] }, { step: 1, count: 3, minGap: 3, seed: 2 });
  assert.equal(counted.faces[0].heights.length, 3);
  const kept = planBands(face, { box: [-10, -12, 10, 12] }, { step: 1, count: 2, seed: 3, keep: (_x, z) => z < 0 });
  for (const s of kept.slabs) assert.ok(s.at[2] < 0.5, "keep-outs are honoured");
  assert.equal(planBands((x) => ({ y: 0, slope: 0, normal: [0, 1, 0] }), { centre: [0, 0], radius: 5 }).faces.length, 0, "flat ground has no face");
});

test("findCrests: a ridge along z is a crest with its outward side; a mesa edge is a cliff top", () => {
  const ridge = (x: number, _z: number): Ground => ({ y: 20 - Math.abs(x) * 1.2, slope: 50, normal: [Math.sign(x) * 0.77, 0.64, 0] });
  const lines = findCrests(ridge, { box: [-6, -10, 6, 10] }, { step: 1, kinds: ["crest"] });
  assert.ok(lines.length >= 1);
  for (const p of lines[0].points) assert.ok(Math.abs(p.x) < 0.01, "the crest is the ridge line");
  assert.ok(lines[0].length >= 15);
  const mesa = (x: number, _z: number): Ground => (x < 0 ? { y: 10, slope: 0, normal: [0, 1, 0] } : { y: 10 - x * 4, slope: 76, normal: [0.97, 0.24, 0] });
  const tops = findCrests(mesa, { box: [-6, -10, 6, 10] }, { step: 1, kinds: ["cliffTop"] });
  assert.ok(tops.length >= 1);
  for (const p of tops[0].points) { assert.ok(p.x <= 0 && p.x >= -2, `at the edge (${p.x})`); assert.ok(p.out[0] > 0.9, "facing the drop"); }
});

test("chain orders points into polylines and splits at gaps", () => {
  const pts = [0, 1, 2, 3, 10, 11].map((x) => ({ x, z: 0 }));
  const lines = chain(pts.slice().reverse(), 1.5);
  assert.equal(lines.length, 2);
  assert.deepEqual(lines.map((l) => l.length).sort(), [2, 4]);
});

test("perchedFromView: a block hovering over the ground shows its underside to a camera on its side; a seated one does not", () => {
  const ground = () => 0;
  const box = (y0: number): V3[] => { const v: V3[] = []; for (const x of [-1, 1]) for (const y of [y0, y0 + 2]) for (const z of [-1, 1]) v.push([x, y, z]); return v; };
  const cam = [{ name: "east", pos: [20, 5, 0] as V3 }];
  const up = perchedFromView(box(1.5), ground, cam);
  assert.ok(up.gap >= 1.49 && up.cams.includes("east"));
  const seated = perchedFromView(box(-0.5), ground, cam);
  assert.equal(seated.gap, 0);
  // a camera behind the block, above it, does not see the far side's underside
  const behind = perchedFromView(box(1.5), (x) => (x > 0 ? 1.5 : 0), [{ name: "east", pos: [20, 30, 0] }]);
  assert.equal(behind.gap, 0, "the overhang faces away from the camera");
});
