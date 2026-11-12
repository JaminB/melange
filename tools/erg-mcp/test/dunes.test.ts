// The dunes() and ridged() expression built-ins, and heightfield_drape's crestMaterial / troughMaterial (materials by the
// skin's local curvature), on the synthetic 12-frame level: ranges, asymmetry, determinism, old expressions unchanged, the
// curvature rules painting crests and hollows, steepMaterial first, default-off, and the validation.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, material, type Loaded, type Scene } from "../src/core";
import { compile, dunes, fbm, noise2, ridged } from "../src/lib/heightfield-expr";
import { applyDrape, planDrape } from "../src/lib/heightfield-drape";
import { grids } from "../src/lib/voxel";

function loaded(): Loaded {
  const s: Scene = JSON.parse(readFileSync(new URL("../../../../tests/fixtures/erg/synthetic-12.json", import.meta.url), "utf8"));
  s.frames.find((f) => f.id === 1)!.name = "Scene";
  const blobs = new Map<number, ArrayBuffer>();
  for (const [ref, buf] of syntheticBlobs(s) as Map<number, Uint8Array>) blobs.set(ref, buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength) as ArrayBuffer);
  return { scene: s, blobs };
}
const store = () => new EditorStore("t", loaded(), loaded());
const vars = (x: number, z: number) => ({ x, z, u: 0, v: 0, r: 0, theta: 0, W: 0, D: 0, R: 0, pi: Math.PI, e: Math.E });

test("dunes: 0..height, deterministic, a seed moves the crests, bad wavelength gives 0", () => {
  let lo = Infinity, hi = -Infinity;
  for (let i = 0; i < 4000; i++) {
    const v = dunes(i * 0.37 - 300, i * 0.91 - 800, 25, 40, 6, 0.6, 3);
    lo = Math.min(lo, v); hi = Math.max(hi, v);
  }
  assert.ok(lo >= 0 && hi <= 6.0001, `${lo}..${hi}`);
  assert.ok(lo < 0.2 && hi > 5, `spans the range: ${lo}..${hi}`);
  assert.equal(dunes(12.5, -7, 0, 40, 6, 0.5, 1), dunes(12.5, -7, 0, 40, 6, 0.5, 1));
  assert.notEqual(dunes(12.5, -7, 0, 40, 6, 0.5, 1), dunes(12.5, -7, 0, 40, 6, 0.5, 2));
  assert.equal(dunes(5, 5, 0, 0, 6), 0);
});

test("dunes: asym lengthens the windward slope against the slip face, asym 0 is symmetric", () => {
  // along +x (dirDeg 0) at z = 0
  const profile = (asym: number) => {
    const ys: number[] = [];
    for (let x = 0; x < 400; x += 0.25) ys.push(dunes(x, 0, 0, 40, 6, asym, 1));
    return ys;
  };
  const runs = (ys: number[]) => {
    let up = 0, down = 0;
    for (let i = 1; i < ys.length; i++) (ys[i] > ys[i - 1] ? up++ : down++);
    return { up, down };
  };
  const sym = runs(profile(0)), skew = runs(profile(1));
  assert.ok(Math.abs(sym.up - sym.down) / (sym.up + sym.down) < 0.15, `asym 0: ${JSON.stringify(sym)}`);
  assert.ok(skew.up > skew.down * 2.5, `asym 1: ${JSON.stringify(skew)}`);
  // the slip face is steeper than the windward side
  const steepest = (ys: number[], sign: number) => Math.max(...ys.slice(1).map((y, i) => (sign * (y - ys[i])) / 0.25));
  const p = profile(1);
  assert.ok(steepest(p, -1) > steepest(p, 1) * 2);
});

test("dunes: crests run across the direction (steady along the crest, varying along the wind)", () => {
  const across = Array.from({ length: 20 }, (_, i) => dunes(10, i * 0.5, 0, 40, 6, 0.5, 0));
  const along = Array.from({ length: 80 }, (_, i) => dunes(i * 0.5, 10, 0, 40, 6, 0.5, 0));
  const range = (a: number[]) => Math.max(...a) - Math.min(...a);
  assert.ok(range(along) > 4 * range(across), `along ${range(along)} across ${range(across)}`);
});

test("ridged: 0..1, deterministic, scale and octaves and seed matter", () => {
  let lo = Infinity, hi = -Infinity;
  for (let i = 0; i < 4000; i++) {
    const v = ridged(i * 0.173, i * 0.311, 8, 4, 2);
    lo = Math.min(lo, v); hi = Math.max(hi, v);
  }
  assert.ok(lo >= 0 && hi <= 1, `${lo}..${hi}`);
  assert.ok(hi - lo > 0.4);
  assert.equal(ridged(3.3, 4.7, 8, 4, 2), ridged(3.3, 4.7, 8, 4, 2));
  assert.notEqual(ridged(3.3, 4.7, 8, 4, 2), ridged(3.3, 4.7, 8, 4, 3));
  assert.notEqual(ridged(3.3, 4.7, 8, 2, 2), ridged(3.3, 4.7, 8, 4, 2));
  assert.notEqual(ridged(3.3, 4.7, 8, 4, 2), ridged(3.3, 4.7, 16, 4, 2));
});

test("the new built-ins compile with their argument counts, in expressions with lets; old expressions are unchanged", () => {
  const f = compile("let d = dunes(x, z, 30, 40, 6, 0.5, 2); d + 2*ridged(x/9, z/9, 1)");
  assert.equal(f(vars(7, 9)), dunes(7, 9, 30, 40, 6, 0.5, 2) + 2 * ridged(7 / 9, 9 / 9, 1));
  assert.equal(compile("dunes(x, z, 0, 40, 6)")(vars(3, 4)), dunes(3, 4, 0, 40, 6));
  assert.throws(() => compile("dunes(x, z, 0)"), /dunes takes 4-7 arguments, not 3/);
  assert.throws(() => compile("ridged(x, z)"), /ridged takes 3-5 arguments, not 2/);
  assert.equal(compile("3*fbm(x/10, z/10, 7, 3) + noise2(x, z, 1)")(vars(5.5, 2.5)), 3 * fbm(0.55, 0.25, 7, 3) + noise2(5.5, 2.5, 1));
});

const args = (extra: Record<string, unknown> = {}) => ({ min: [100, 100], max: [160, 160], floor: 0, expr: "2 + dunes(x, z, 0, 24, 5, 0.6, 3)", material: 2, voxelSize: 1, thickness: 2, ...extra });

/** The material of the top voxel of every skin column. */
function topMaterials(st: EditorStore): number[] {
  const out: number[] = [];
  for (const g of grids(st).filter((g) => g.frame.corners)) {
    const [X, Y, Z] = g.frame.size;
    for (let z = 0; z < Z; z++) for (let x = 0; x < X; x++) for (let y = Y - 1; y >= 0; y--) {
      const w = g.words[(z * X + x) * Y + y];
      if (w) { out.push(material(w)); break; }
    }
  }
  return out;
}

test("heightfield_drape: crestMaterial and troughMaterial paint dune crests and hollows, counts in the plan", () => {
  const st = store();
  const p = planDrape(st, args({ crestMaterial: { material: 5, minCurvature: 0.02 }, troughMaterial: { material: 7, minCurvature: 0.02 } }));
  assert.equal(p.report.fits, true, String(p.report.why));
  const cm = p.report.curveMaterials as { crest: { material: number; columns: number; minCurvature: number }; trough: { columns: number } };
  assert.ok(cm.crest.columns > 20 && cm.trough.columns > 20, JSON.stringify(cm));
  assert.equal(cm.crest.minCurvature, 0.02);
  applyDrape(st, p);
  assert.equal(st.stack.depth, 1);
  assert.deepEqual(st.problems(), []);
  const mats = topMaterials(st);
  assert.equal(mats.filter((m) => m === 5).length, cm.crest.columns);
  assert.equal(mats.filter((m) => m === 7).length, cm.trough.columns);
  assert.ok(mats.some((m) => m === 2), "flats keep the default material");
});

test("heightfield_drape: without the new rules nothing changes (no curveMaterials, default material only)", () => {
  const a = store();
  const pa = planDrape(a, args());
  assert.equal("curveMaterials" in pa.report, false);
  applyDrape(a, pa);
  assert.ok(topMaterials(a).every((m) => m === 2));
});

test("heightfield_drape: steepMaterial wins over the curvature rules", () => {
  const st = store();
  const p = planDrape(st, args({ steepMaterial: { material: 9, minSlope: 20 }, crestMaterial: { material: 5, minCurvature: 0.02 }, troughMaterial: { material: 5, minCurvature: 0.02 } }));
  const sm = p.report.steepMaterial as { columns: number };
  assert.ok(sm.columns > 0);
  applyDrape(st, p);
  assert.equal(topMaterials(st).filter((m) => m === 9).length, sm.columns);
});

test("heightfield_drape: radius is reported, feather dithers the edge", () => {
  const sharp = (planDrape(store(), args({ crestMaterial: { material: 5, minCurvature: 0.02 } })).report.curveMaterials as { crest: { columns: number } }).crest.columns;
  const feathered = (planDrape(store(), args({ crestMaterial: { material: 5, minCurvature: 0.02, feather: 0.03 } })).report.curveMaterials as { crest: { columns: number; feather: number } }).crest;
  assert.equal(feathered.feather, 0.03);
  assert.notEqual(feathered.columns, sharp);
  const wide = (planDrape(store(), args({ crestMaterial: { material: 5, minCurvature: 0.02, radius: 4 } })).report.curveMaterials as { crest: { radius: number } }).crest;
  assert.equal(wide.radius, 4);
});

test("heightfield_drape: the curvature rules are checked", () => {
  assert.throws(() => planDrape(store(), args({ crestMaterial: { material: 64 } })), /crestMaterial.material is a material index/);
  assert.throws(() => planDrape(store(), args({ troughMaterial: { material: 3, minCurvature: -1 } })), /troughMaterial.minCurvature is a non-negative number/);
  assert.throws(() => planDrape(store(), args({ crestMaterial: { material: 3, radius: 0 } })), /crestMaterial.radius is a positive number/);
});
