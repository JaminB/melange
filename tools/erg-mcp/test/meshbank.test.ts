// Detail layer round 10: lib/slope-break.ts (plan_slope_break, lint_contact) and the mod-mesh name rule of place_mod_mesh.
import assert from "node:assert/strict";
import { test } from "node:test";
import { contactAngle, slopeBreaks, talusFan, type Ground } from "../src/lib/slope-break";
import { MOD_MESH } from "../src/tools/meshbank";
import { classifyDetail } from "../src/lib/sim-fingerprint";

// a step: flat ground (y 0) for x < 0, a 60-degree face rising from x = 0 to x = 6, a flat top beyond
function step(x: number): number { return x < 0 ? 0 : x < 6 ? x * Math.tan(Math.PI / 3) : 6 * Math.tan(Math.PI / 3); }
const ground = (x: number, _z: number): Ground => {
  const h = 0.25, dx = (step(x + h) - step(x - h)) / (2 * h), n = Math.hypot(dx, 1);
  return { y: step(x), slope: Math.atan(Math.abs(dx)) * 180 / Math.PI, normal: [-dx / n, 1 / n, 0] };
};

test("slopeBreaks finds the foot of the face, not the face or the top", () => {
  const r = slopeBreaks(ground, { box: [-10, -4, 12, 4], step: 1 });
  assert.ok(r.sites.length > 0);
  for (const s of r.sites) {
    assert.ok(s.x < 0 && s.x >= -5, `site at x ${s.x}`);
    assert.ok(s.faceSlope >= 45);
    assert.ok(s.down[0] < -0.9, "downhill points away from the face");
  }
  assert.equal(r.runs, 1, "one run along the contour");
});

test("talusFan grades big at the toe, small at the apex, with a skirt", () => {
  let seed = 7;
  const R = () => ((seed = (seed * 1664525 + 1013904223) >>> 0) / 4294967296);
  const f = talusFan({ x: 0, z: 0, down: [-1, 0] }, { n: 12, length: 4, big: 2, small: 0.5, R });
  const toe = f.filter((p) => p.kind === "toe"), apex = f.filter((p) => p.kind === "apex"), skirt = f.filter((p) => p.kind === "skirt");
  assert.ok(toe.length && apex.length && skirt.length);
  const mean = (a: { size: number }[]) => a.reduce((s, p) => s + p.size, 0) / a.length;
  assert.ok(mean(toe) > mean(apex) * 1.8, `${mean(toe)} vs ${mean(apex)}`);
  for (const p of f) assert.ok(p.x <= 0.5, "the fan spreads downhill");
  assert.ok(skirt.every((p) => p.embed >= 0.55), "the skirt is half buried");
});

test("contactAngle", () => {
  assert.equal(Math.round(contactAngle([0, 1, 0])), 0);
  assert.equal(Math.round(contactAngle([Math.sin(Math.PI / 3), Math.cos(Math.PI / 3), 0])), 60);
});

test("mod mesh names, and their copies are visual scenery", () => {
  assert.ok(MOD_MESH.test("caldera.Boulder"));
  assert.ok(!MOD_MESH.test("HORROR5"));
  assert.ok(!MOD_MESH.test("Caldera.Boulder"));
  const cl = classifyDetail({ name: "VISIBLE_Boulder_dtl_tint36373a", resource: "caldera.Boulder" });
  assert.equal(cl.cls, "visual");
  assert.equal(cl.role, "scenery");
});
