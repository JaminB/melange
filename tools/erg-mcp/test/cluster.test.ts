// Detail layer round 5: cluster planning (src/lib/cluster.ts) and the dressing lint (src/lib/dress-lint.ts). Bundled
// for Node by test.ps1.
import assert from "node:assert/strict";
import { test } from "node:test";
import { downhill, planClusters, type Ground } from "../src/lib/cluster";
import { clumps, contact, mineSuspects, type Piece } from "../src/lib/dress-lint";

// a plane falling toward +x (downhill is +x), 20 degrees
const slope = Math.tan((20 * Math.PI) / 180);
const plane = (x: number, _z: number): Ground => { const n = Math.hypot(slope, 1); return { y: 10 - x * slope, slope: 20, normal: [slope / n, 1 / n, 0] }; };

test("plan_clusters: outcrop children fall downhill and shrink with distance; scree is a fan of flat slabs; a skirt rings the foot", () => {
  assert.deepEqual(downhill([0, 1, 0]), [0, 0]);
  const kids = planClusters([{ xz: [0, 0], size: 5 }], plane, { mode: "outcrop", count: [4, 4], seed: 3 });
  assert.equal(kids.length, 4);
  for (const k of kids) assert.ok(k.xz[0] > 0, "on the downhill side");
  assert.ok(kids[0].height > kids[3].height, "the nearest is the biggest");
  for (const k of kids) assert.ok(k.height <= 5 * 0.55 * 1.15 && k.height >= 0.8);
  const scree = planClusters([{ xz: [0, 0], size: 5 }], plane, { mode: "scree", seed: 4 });
  assert.ok(scree.length >= 4 && scree.length <= 8);
  for (const k of scree) { assert.ok(k.height >= 0.8 && k.height <= 1.6 + 1e-9); assert.ok(k.width / k.height > 2); assert.ok(k.xz[0] > 3, "past the anchor's foot, down the slope"); }
  const skirt = planClusters([{ xz: [0, 0], size: 6, radius: 3 }], plane, { mode: "skirt", count: [6, 6], seed: 5 });
  assert.equal(skirt.length, 6);
  for (const k of skirt) { const d = Math.hypot(...k.xz); assert.ok(d >= 3 * 0.85 - 1e-9 && d < 3 * 1.8, `on the contact line (${d})`); }
  assert.ok(skirt.filter((k) => k.xz[0] > 0).length >= 3, "more of it downhill or as much");
  assert.equal(planClusters([{ xz: [0, 0], size: 5 }], () => null, { mode: "outcrop" }).length, 0, "no ground, nothing");
});

const box = (x: number, y: number, z: number, s: number) => ({ min: [x - s / 2, y, z - s / 2] as [number, number, number], max: [x + s / 2, y + s, z + s / 2] as [number, number, number] });
test("lint: clumps join touching pieces; a small dark lump on walkable ground is a mine suspect; hovering and buried clumps", () => {
  const flat = (_x: number, _z: number) => ({ y: 0, slope: 5 });
  const pieces: Piece[] = [
    { id: 1, box: box(0, -0.2, 0, 1), lum: 0.1 },            // alone, small, dark: a mine suspect
    { id: 2, box: box(10, -0.5, 0, 3), lum: 0.1 },           // an anchor
    { id: 3, box: box(11.6, -0.2, 0, 1), lum: 0.1 },         // touching it: one clump of 3+ across
    { id: 4, box: box(20, 0.6, 0, 1), lum: 0.6 },            // pale, hovering 0.6
    { id: 5, box: box(30, -2, 0, 1), lum: 0.6 },             // buried
  ];
  const cs = clumps(pieces, 0.4);
  assert.equal(cs.length, 4);
  const sus = mineSuspects(cs, flat, {});
  assert.deepEqual(sus.map((c) => c.ids), [[1]]);
  assert.equal(mineSuspects(cs, () => ({ y: 0, slope: 60 }), {}).length, 0, "too steep to walk");
  const st = cs.map((c) => contact(c, flat).state);
  assert.deepEqual(st, ["seated", "seated", "hovering", "buried"]);
});
