// Detail layer round 3 (src/lib/scatter.ts): rotations in the engine's detail order, the lean toward a slope's normal,
// seeded kitbash crags and their seating below the lowest footprint sample, and the in-level draw verdicts.
import assert from "node:assert/strict";
import { test } from "node:test";
import { ARCHETYPES, apply, composeCrag, drawCheck, eulerOf, footprint, kitbash, mul, rotMat, tiltTo, type Box, type V3 } from "../src/lib/scatter";

const near = (a: number, b: number, e = 1e-6) => assert.ok(Math.abs(a - b) < e, `${a} != ${b}`);

test("eulerOf inverts rotMat (Rz * Ry * Rx)", () => {
  for (const d of [[10, 20, 30], [-40, 75, 5], [0, -60, 170], [33, 0, -12]] as V3[]) {
    const back = eulerOf(rotMat(d)), a = rotMat(d), b = rotMat(back);
    for (let i = 0; i < 3; i++) for (let j = 0; j < 3; j++) near(a[i][j], b[i][j]);
  }
  // yaw by y turns +x to (cos y, 0, -sin y)
  const v = apply(rotMat([0, 90, 0]), [1, 0, 0]);
  near(v[0], 0); near(v[2], -1);
});

test("tiltTo leans the up axis toward the normal, capped", () => {
  const n: V3 = [Math.sin(0.5), Math.cos(0.5), 0];          // a 28.6 degree slope falling toward +x
  const full = tiltTo(n, 1, 90).m, up = apply(full, [0, 1, 0]);
  near(up[0], n[0]); near(up[1], n[1]); near(up[2], 0);
  const capped = tiltTo(n, 1, 15);
  near(capped.tilt, 15, 1e-4);
  assert.ok(apply(capped.m, [0, 1, 0])[0] > 0, "leans toward the downhill side");
  near(tiltTo([0, 1, 0], 1, 15).tilt, 0);
  assert.ok(tiltTo(n, 0.5, 90, 5, () => 0.9).tilt <= 0.5 * 28.65 + 5 + 1e-6);
});

test("kitbash: every archetype is 1-4 pieces, seeded, no piece over the stretch", () => {
  for (const k of ARCHETYPES) {
    const a = kitbash(k, 20, 9, 42), b = kitbash(k, 20, 9, 42), c = kitbash(k, 20, 9, 43);
    assert.ok(a.length >= 1 && a.length <= 4, `${k}: ${a.length}`);
    assert.deepEqual(a, b);
    if (k !== "single") assert.notDeepEqual(a, c);
    for (const p of a) assert.ok(p.size[1] <= Math.max(p.size[0], p.size[2] / 0.85) * 2 + 1e-6, `${k} stretch`);
  }
  // a spire is taller than wide, a ledge steps down along +x
  const sp = kitbash("spire", 24, 8, 7), top = Math.max(...sp.map((p) => p.at[1] + p.size[1] / 2));
  assert.ok(top > 20 && top <= 24.01, `spire top ${top}`);
  const ld = kitbash("ledge", 6, 16, 7);
  for (let i = 1; i < ld.length; i++) assert.ok(ld[i].at[0] > ld[i - 1].at[0] && ld[i].at[1] <= ld[i - 1].at[1] + 1e-9);
});

test("composeCrag sinks the given fraction below the lowest sample and keeps scale per axis", () => {
  const mesh: Box = { min: [-1.5, 0, -1.6], max: [1.6, 2.05, 1.6] };
  const pieces = kitbash("spire", 20, 8, 3);
  const flat = composeCrag(pieces, mesh, 10, -5, 30, rotMat([0, 0, 0]), 4, 0.35);
  near(flat.bottom, 4 - 0.35 * flat.height, 1e-9);
  near(flat.top - flat.bottom, flat.height, 1e-9);
  assert.equal(flat.poses.length, pieces.length);
  // each pose's box (its mesh through rot and scale) lies inside [bottom, top]
  for (const p of flat.poses) {
    const r = rotMat(p.rot);
    for (let k = 0; k < 8; k++) {
      const c: V3 = [0, 1, 2].map((i) => ((k >> i) & 1 ? mesh.max[i] : mesh.min[i]) * p.scale[i]) as V3;
      const y = apply(r, c)[1] + p.pos[1];
      assert.ok(y >= flat.bottom - 1e-6 && y <= flat.top + 1e-6, `corner ${y}`);
    }
  }
  const t = tiltTo([0.3, 0.95, 0.1], 1, 15).m;
  const tilted = composeCrag(pieces, mesh, 0, 0, 0, t, 0, 0.4);
  near(tilted.bottom, -0.4 * tilted.height, 1e-9);
  assert.ok(Number.isFinite(mul(t, t)[0][0]));
  // conform: each based piece on its own ground; a ledge on a slope steps down with it
  const ledge = kitbash("ledge", 4, 12, 9);
  const lows = ledge.map((p) => 10 - p.at[0] * 0.5);                 // ground falling along +x
  const cf = composeCrag(ledge, mesh, 0, 0, 0, rotMat([0, 0, 0]), lows, 0.3);
  const conf = composeCrag(ledge, mesh, 0, 0, 0, rotMat([0, 0, 0]), Math.min(...lows), 0.3);
  assert.ok(cf.top > conf.top, "the uphill step is not buried");
  cf.poses.forEach((p, i) => {            // every ledge step is based: 30% of its own height under its own ground
    const r = rotMat(p.rot);
    const ys = [...Array(8).keys()].map((k) => apply(r, [0, 1, 2].map((j) => ((k >> j) & 1 ? mesh.max[j] : mesh.min[j]) * p.scale[j]) as V3)[1] + p.pos[1]);
    near(Math.min(...ys), lows[i] - 0.3 * (Math.max(...ys) - Math.min(...ys)), 1e-6);
  });
  const fp = footprint(pieces, 10, -5, 30);
  assert.equal(fp.pts.length, 1 + 8 * pieces.length);
  assert.ok(fp.radius > 2 && fp.radius < 12);
});

test("drawCheck: known verdicts, same theme, unchecked", () => {
  assert.match(drawCheck("PREHISTORIC", "ARABIAN", "ARABIAN6")!, /never/);
  assert.equal(drawCheck("PREHISTORIC", "PREHISTORIC", "PREHISTORIC12"), "same theme");
  assert.match(drawCheck("PREHISTORIC", "WAR", "WAR11")!, /unchecked/);
  assert.equal(drawCheck(null, "WAR", "WAR11"), undefined);
});
