// The Caldera v2 round-3 gaps, on hand-made grids (no server): drops told apart (a V-drop onto the leg under, a trap
// trough), the trail walked with the worm's body and step (a ridge between legs), the mine-ground suspects, blocks that
// poke out of a draped skin, per-corner blend tints for render_view, camera poses kept over the water, and the asserts'
// session scope helpers. Bundled for Node by test.ps1.
import assert from "node:assert/strict";
import { test } from "node:test";
import { withSecond, type Frame } from "../src/core";
import { drapePokes } from "../src/lib/drape-pokes";
import { topBlendTints } from "../src/lib/render-3d";
import { checkTrail, classifyDrops, mineSuspectAt, trailReach, walkTrail, type TrailCheckOptions } from "../src/lib/trail-check";
import type { Grid } from "../src/lib/voxel";
import { overWater, OVER_WATER } from "../src/tools/game";

const SOLID = 3;
const word = (m: number) => (SOLID | (m << 2)) >>> 0;

/** An upright grid of `size` voxels at `scale`, its low corner at `min`, filled solid up to height(x, z) (map units over
 * its bottom), optionally draped with per-corner lifts (map units). */
function block(id: number, min: [number, number, number], size: [number, number, number], scale: number, height: (x: number, z: number) => number, m = 1, corners?: (x: number, z: number) => number): Grid {
  const [X, Y, Z] = size, words = new Uint32Array(X * Y * Z);
  for (let z = 0; z < Z; z++)
    for (let x = 0; x < X; x++) {
      const h = height(min[0] + (x + 0.5) * scale, min[2] + (z + 0.5) * scale);
      for (let y = 0; y < Y; y++) if ((y + 1) * scale <= h + 1e-9) words[(z * X + x) * Y + y] = word(m);
    }
  const frame = { id, name: `block_${id}`, size: [X, Y, Z], pos: [0, 0, 0], rot: [0, 0, 0], scale,
    ...(corners ? { corners: Array.from({ length: (X + 1) * (Z + 1) }, (_, i) => corners(min[0] + (i % (X + 1)) * scale, min[2] + Math.floor(i / (X + 1)) * scale) / scale) } : {}) } as unknown as Frame;
  return { id, frame, ref: id, size: [X, Y, Z], words, base: new Uint32Array(words.length),
    toMap: [scale, 0, 0, min[0], 0, scale, 0, min[1], 0, 0, scale, min[2]], fromMap: [1 / scale, 0, 0, -min[0] / scale, 0, 1 / scale, 0, -min[1] / scale, 0, 0, 1 / scale, -min[2] / scale],
    bounds: { min: [min[0], min[1], min[2]], max: [min[0] + X * scale, min[1] + Y * scale, min[2] + Z * scale] } };
}

const opts: TrailCheckOptions = { halfWidth: 2.6, margin: 1, spacing: 0.25, maxDrop: 3, holeRadius: 1.5, above: 0, headroom: 1.5 };

// a plateau at 8 with a trench (floor 2) across z 4-7, open at neither end: a trail along z = 10 sees it on its left
const plateau = (x: number, z: number) => (z >= 4 && z < 7 ? 2 : 8);

test("check_trail: a drop beside the trail into a trench with no way out is a trap; a drop onto the trail's own next leg is told harmless", () => {
  const gs = [block(-1, [-10, 0, -5], [60, 10, 25], 1, plateau)];
  const rep = checkTrail(gs, [[2, 8, 10], [38, 8, 10]], opts);
  const drops = rep.runs.filter((r) => r.kind === "drop");
  assert.ok(drops.length >= 1, "the trench is a drop run");
  classifyDrops(gs, rep, opts, trailReach(null));
  assert.ok(drops.every((r) => r.landing?.kind === "trap"), JSON.stringify(drops.map((r) => r.landing)));
  // an edge with no ground at all under it (the block ends: the sea) is a trap too, never a harmless drop
  const edge = checkTrail(gs, [[2, 8, 17], [38, 8, 17]], opts);
  classifyDrops(gs, edge, opts, trailReach(null));
  const off = edge.runs.filter((r) => r.kind === "drop" && r.worst === undefined);
  assert.ok(off.length >= 1 && off.every((r) => r.landing?.kind === "trap" && /no ground at all/.test(r.landing.why)), JSON.stringify(off));
  assert.ok(drops.some((r) => r.landing?.climbRefused !== undefined && r.landing.climbRefused > 0.5), "the climb out is named");
  // the same ground, the trail's second leg running in the trench: the drop lands on the trail
  const rep2 = checkTrail(gs, [[2, 8, 10], [38, 8, 10], [38, 2, 5.5], [2, 2, 5.5]], opts);
  classifyDrops(gs, rep2, opts, trailReach(null));
  const d2 = rep2.runs.filter((r) => r.kind === "drop" && r.trailY === 8);
  assert.ok(d2.length && d2.every((r) => r.landing?.kind === "trail"), JSON.stringify(d2.map((r) => r.landing)));
  assert.ok(d2.every((r) => r.landing?.legAlong !== undefined && r.landing.legAlong > 36), "the leg under is named by its along");
});

test("check_trail: a trench with a ramp out is a drop with an exit, not a trap", () => {
  // the trench floor rises to the plateau along x 8-16 (a 6-unit climb over 8: about 37 degrees of risers at 0.75),
  // within the 14 map units the walk back is searched in from the drop's landing at the trail's start
  const ramped = (x: number, z: number) => (z >= 4 && z < 7 ? Math.min(8, x >= 8 ? 2 + (x - 8) * 0.75 : 2) : 8);
  const gs = [block(-1, [-10, 0, -5], [120, 20, 50], 0.5, ramped)];
  const rep = checkTrail(gs, [[2, 8, 10], [38, 8, 10]], opts);
  classifyDrops(gs, rep, opts, trailReach(null));
  const drops = rep.runs.filter((r) => r.kind === "drop");
  assert.ok(drops.length >= 1);
  assert.ok(drops.some((r) => r.landing?.kind === "exit" && r.landing.exitLength! > 0), JSON.stringify(drops.map((r) => r.landing)));
  assert.ok(!drops.some((r) => r.landing?.kind === "trap"), "a walk back up exists");
});

test("check_trail: the walk catches a ridge across the trail every column of which has ground, and passes the flat trail", () => {
  const flat = [block(-1, [0, 0, 0], [40, 12, 20], 1, () => 8)];
  const okWalk = walkTrail(flat, [[2, 8, 10], [38, 8, 10]], [-1.5, 0, 1.5], opts, trailReach(null));
  assert.ok(okWalk.every((w) => w.ok && w.segments > 20 && w.noFooting === 0), JSON.stringify(okWalk));
  // a kerb 2 high and one voxel wide across the whole trail at x 20 (a one-voxel kerb of 1 is a 45-degree stair, walked)
  const ridge = [block(-1, [0, 0, 0], [40, 12, 20], 1, (x) => (x >= 20 && x < 21 ? 10 : 8))];
  const rep = checkTrail(ridge, [[2, 8, 10], [38, 8, 10]], opts);
  assert.equal(rep.runs.filter((r) => r.kind === "drop").length, 0, "column by column the ridge is no drop");
  const walk = walkTrail(ridge, [[2, 8, 10], [38, 8, 10]], [-1.5, 0, 1.5], opts, trailReach(null));
  assert.ok(walk.every((w) => !w.ok && w.blocked.length >= 1), JSON.stringify(walk));
  const b = walk[1].blocked[0];
  assert.ok(Math.abs(b.along - 18) < 1.5 && b.rise > 1.5 && /rise|ridge/.test(b.why), JSON.stringify(b));
  // a worm climbs a 1.5 kerb on a 1.5-voxel block: the riser is one voxel
  const coarse = [block(-1, [0, 0, 0], [26, 8, 14], 1.5, (x) => (x >= 19.5 && x < 21 ? 9 : 7.5))];
  const w2 = walkTrail(coarse, [[2, 7.5, 10], [37, 7.5, 10]], [0], { ...opts, headroom: 1.5 }, trailReach(null));
  assert.ok(w2[0].ok, JSON.stringify(w2));
});

test("check_trail: mine-ground suspects are a block of a non-whole scale and a skin cell sheared more than a voxel", () => {
  const gs = [block(-1, [0, 0, 0], [10, 8, 10], 1.5, () => 6), block(-2, [20, 0, 0], [8, 4, 8], 2.5, () => 10, 1, (x) => (x >= 30 ? 6 : 0)), block(-3, [40, 0, 0], [8, 8, 8], 2, () => 6)];
  assert.equal(mineSuspectAt(gs, -1, 5, 5)?.kind, "scaledBlock");
  assert.equal(mineSuspectAt(gs, -3, 44, 4), null, "a whole scale is no suspect");
  // the cell x 27.5-30 has lifts 0, 6 across it: sheared 6 over 2.5
  assert.equal(mineSuspectAt(gs, -2, 29, 4)?.kind, "shearedCell");
  assert.equal(mineSuspectAt(gs, -2, 22, 4), null, "a level cell is none");
  const rep = checkTrail(gs, [[1, 6, 5], [9, 6, 5]], opts);
  assert.ok(rep.mineSuspects.length === 1 && rep.mineSuspects[0].kind === "scaledBlock" && rep.mineSuspects[0].to > 7, JSON.stringify(rep.mineSuspects));
});

test("drapePokes: a plug whose top stands out of the skin is reported, a buried plug and a castle on the skin are not", () => {
  // a skin block 2.5 a voxel, 3 thick (grid top 7.5), sloping down along x by its lifts: its surface runs from 7.5 at x 0 to 2.5 at x 20
  const skin = block(-1, [0, 0, 0], [8, 3, 8], 2.5, () => 7.5, 1, (x) => -x * 0.25);
  const poking = block(-2, [8, 0, 8], [4, 5, 4], 1, () => 5);          // top 5; the skin there is 5.5 .. 4.5: pokes on its downhill half
  const buried = block(-3, [2, 0, 8], [2, 3, 2], 1, () => 3);          // top 3; the skin at x 2-4 is about 7
  const castle = block(-4, [12, 0, 2], [4, 12, 4], 1, () => 12);       // 12 high: a structure, not a plug
  const pokes = drapePokes([skin, poking, buried, castle]);
  assert.equal(pokes.length, 1, JSON.stringify(pokes));
  assert.equal(pokes[0].block, -2);
  assert.ok(pokes[0].maxOver > 0.3 && pokes[0].maxOver < 1 && pokes[0].columns >= 2 && pokes[0].columns < pokes[0].of, JSON.stringify(pokes[0]));
});

test("render_view: a top with a second material on some corners splits into two tints by the corners each triangle carries", () => {
  const tint = (m: number): [number, number, number] => (m === 1 ? [200, 100, 0] : [0, 100, 200]);
  assert.equal(topBlendTints(word(1), tint), null);
  // the second material on the x1 corners only (bits 2 and 0: (x1 z0), (x1 z1))
  const w = withSecond(word(1), 2, (1 << 2) | (1 << 0));
  const t = topBlendTints(w, tint)!;
  // triangle (x0 z0)(x0 z1)(x1 z1): one of three corners; triangle (x0 z0)(x1 z1)(x1 z0): two of three
  assert.deepEqual(t[0], [133, 100, 67]);
  assert.deepEqual(t[1], [67, 100, 133]);
  // every corner: the second material outright
  assert.deepEqual(topBlendTints(withSecond(word(1), 2, 0xff), tint), [[0, 100, 200], [0, 100, 200]]);
});

test("overWater: a pose under the risen sea is lifted over it with its target, a pose over it is left alone", () => {
  const dry = overWater({ pos: [0, 900, 0], target: [10, 700, 10] }, 500);
  assert.equal(dry.lifted, undefined);
  const wet = overWater({ pos: [0, 300, 0], target: [10, 100, 10], fov: 60 }, 500);
  assert.equal(wet.pose.pos[1], 500 + OVER_WATER * 20);
  assert.equal(wet.pose.target[1], 500 + OVER_WATER * 20);
  assert.equal(wet.pose.fov, 60);
  assert.match(wet.lifted!, /eye raised 11.5 map units, the target raised 21.5/);
  assert.equal(overWater({ pos: [0, 300, 0], target: [0, 0, 0] }, null).lifted, undefined);
});
