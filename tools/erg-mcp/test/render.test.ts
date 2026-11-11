// src/lib/render-3d.ts on synthetic levels, bundled for Node by test.ps1: a block covers the pixels its corners project
// to, top and side faces shade apart, a rotated frame renders where its rotation puts it, the presets frame the box, and
// the timing on the 400-frame synthetic level and on a dense 100-block cone (the size of Caldera's voxels).
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, filled, invert, type Loaded, type Mat3x4, type Scene, type Vec3 } from "../src/core";
import { LOOKS, boxCorners, cameraAngles, fitDistance, orbitCamera, renderView, type Camera, type ViewInput } from "../src/lib/render-3d";
import { grid, grids, voxelIndex, type Grid } from "../src/lib/voxel";

const fixture = (name: string): Scene => JSON.parse(readFileSync(new URL(`../../../../tests/fixtures/erg/${name}`, import.meta.url), "utf8"));
function store(name: string, edit?: (s: Scene) => void): EditorStore {
  const load = (): Loaded => {
    const s = fixture(name);
    edit?.(s);
    const blobs = new Map<number, ArrayBuffer>();
    for (const [ref, buf] of syntheticBlobs(s) as Map<number, Uint8Array>) blobs.set(ref, buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength) as ArrayBuffer);
    return { scene: s, blobs };
  };
  return new EditorStore("t", load(), load());
}

/** Frame 5 of the 12-frame level as a solid block of `size` at `pos`, rotated `rotY` radians about y. */
function block(pos: Vec3, size: Vec3, rotY = 0): Grid {
  const st = store("synthetic-12.json", (s) => {
    const f = s.frames.find((x) => x.id === 5)!;
    f.rot = [0, rotY, 0]; f.scale = [1, 1, 1]; f.pos = pos; f.size = size;
  });
  const g = grid(st, 5);
  g.words.fill(filled(1));
  return g;
}

const GREY = () => [200, 200, 200] as [number, number, number];
const input = (gs: Grid[], o: Partial<ViewInput> = {}): ViewInput => ({ grids: gs, tint: GREY, look: LOOKS.DAY, water: null, edges: false, ...o });
const px = (v: { w: number; rgb: Uint8Array }, x: number, y: number) => {
  const i = (Math.round(y) * v.w + Math.round(x)) * 3;
  return [v.rgb[i], v.rgb[i + 1], v.rgb[i + 2]].join(",");
};

test("a block seen from the south covers the pixels its corners project to, and nothing else", () => {
  const g = block([10, 3, 20], [4, 6, 2]);           // map box x 8..12, y 0..6, z 19..21
  const b = g.bounds;
  assert.deepEqual([b.min, b.max], [[8, 0, 19], [12, 6, 21]]);
  const cam: Camera = { pos: [10, 3, 60], target: [10, 3, 20], fov: 30 };
  const v = renderView(input([g]), cam, 200, 200), empty = renderView(input([]), cam, 200, 200);
  // the south face (z = 21) is what the eye sees: its corners
  const p0 = v.project([8, 0, 21])!, p1 = v.project([12, 6, 21])!;
  const [xl, xr] = [Math.min(p0[0], p1[0]), Math.max(p0[0], p1[0])], [yt, yb] = [Math.min(p0[1], p1[1]), Math.max(p0[1], p1[1])];
  assert.ok(xr - xl > 10 && yb - yt > 15, `face spans ${xr - xl} x ${yb - yt} px`);
  // straight ahead it is centred: x 8..12 about the middle, y 0..6 about the middle
  assert.ok(Math.abs((xl + xr) / 2 - 100) < 1 && Math.abs((yt + yb) / 2 - 100) < 1);
  let inside = 0, outside = 0, bad = 0;
  for (let y = 0; y < 200; y++)
    for (let x = 0; x < 200; x++) {
      const same = px(v, x, y) === px(empty, x, y);
      const inn = x + 0.5 > xl + 1 && x + 0.5 < xr - 1 && y + 0.5 > yt + 1 && y + 0.5 < yb - 1;
      const out = x + 0.5 < xl - 1 || x + 0.5 > xr + 1 || y + 0.5 < yt - 1 || y + 0.5 > yb + 1;
      if (inn) { inside++; if (same) bad++; }
      if (out) { outside++; if (!same) bad++; }
    }
  assert.equal(bad, 0);
  assert.ok(inside > 150 && outside > 30000);
  assert.ok(v.faces >= 8 && v.faces <= 4 * 6 + 8, `faces ${v.faces}`);   // only the south face's cells are seen head-on
  assert.equal(v.framesDrawn, 1);
});

test("top faces and the side faces shade apart (sun and ambient per face)", () => {
  const g = block([0, 3, 0], [6, 6, 6]);
  const cam = orbitCamera([0, 3, 0], 135, 35, 40, 40);    // from the south-east, above
  const v = renderView(input([g]), cam, 300, 200);
  const at = (p: Vec3) => { const q = v.project(p)!; return px(v, q[0], q[1]); };
  const empty = renderView(input([]), cam, 300, 200);
  const top = at([0, 6, 0]), south = at([0, 3, 3]), east = at([3, 3, 0]);
  for (const p of [[0, 6, 0], [0, 3, 3], [3, 3, 0]] as Vec3[]) { const q = v.project(p)!; assert.notEqual(px(v, q[0], q[1]), px(empty, q[0], q[1])); }
  assert.notEqual(top, south);
  assert.notEqual(top, east);
  assert.notEqual(south, east);
  // the top is the brightest face in daylight
  const lum = (c: string) => c.split(",").map(Number).reduce((a, b) => a + b, 0);
  assert.ok(lum(top) > lum(south) && lum(top) > lum(east), `${top} ${south} ${east}`);
  // the night is darker than the day
  const n = renderView(input([g], { look: LOOKS.NIGHT }), cam, 300, 200);
  const q = n.project([0, 6, 0])!;
  assert.ok(lum(px(n, q[0], q[1])) < lum(top));
});

test("a rotated frame renders where its rotation puts it", () => {
  // long along the grid's z; rotated 90 degrees about y it lies along map x
  const flat = block([0, 0, 0], [2, 1, 12]), turned = block([0, 0, 0], [2, 1, 12], Math.PI / 2);
  const down: Camera = { pos: [0, 60, 0], target: [0, 0, 0], fov: 30 };   // straight down: x right, north up
  const a = renderView(input([flat]), down, 200, 200), b = renderView(input([turned]), down, 200, 200), empty = renderView(input([]), down, 200, 200);
  const hit = (v: typeof a, p: Vec3) => { const q = v.project(p)!; return px(v, q[0], q[1]) !== px(empty, q[0], q[1]); };
  assert.ok(hit(a, [0, 0.5, 4.5]) && !hit(a, [4.5, 0.5, 0]), "unrotated: along z");
  assert.ok(hit(b, [4.5, 0.5, 0]) && hit(b, [-4.5, 0.5, 0]) && !hit(b, [0, 0.5, 4.5]), "rotated: along x");
  // the faces drawn follow the grid's own matrix: its bounds agree
  assert.ok(turned.bounds.max[0] - turned.bounds.min[0] > 11.9 && turned.bounds.max[2] - turned.bounds.min[2] < 2.1);
});

test("water is drawn translucent over ground below it, markers as billboards, clip keeps the region", () => {
  const g = block([0, 1, 0], [8, 2, 8]);           // y 0..2
  const cam = orbitCamera([0, 1, 0], 180, 50, 30, 50);
  const dry = renderView(input([g]), cam, 200, 150), wet = renderView(input([g], { water: 1.5 }), cam, 200, 150);
  const q = dry.project([0, 2, 0])!;
  // the top (y 2) stays above the water; open water replaces the sky colour below the horizon
  assert.equal(px(wet, q[0], q[1]), px(dry, q[0], q[1]));
  const open = dry.project([0, 1.5, 12])!;
  assert.notEqual(px(wet, open[0], open[1]), px(dry, open[0], open[1]));
  const sunk = renderView(input([g], { water: 3 }), cam, 200, 150);
  assert.notEqual(px(sunk, q[0], q[1]), px(dry, q[0], q[1]));
  const m = renderView(input([g], { markers: [{ kind: "spawn", name: "WORM0", detail: 1, pos: [2, 2, 2] }] }), cam, 200, 150);
  assert.equal(m.markers.length, 1);
  assert.ok(m.markers[0].visible);
  assert.equal(px(m, m.markers[0].px[0], m.markers[0].px[1]), "60,220,60");
  const clipped = renderView(input([g], { clip: { x0: 100, z0: 100, x1: 101, z1: 101 } }), cam, 200, 150);
  assert.equal(clipped.faces, 0);
});

test("presets: the fitted distance keeps every corner of the box in the picture", () => {
  const box = { min: [-50, 0, -30] as Vec3, max: [50, 40, 30] as Vec3 };
  for (const az of [45, 135, 225, 315]) {
    const D = fitDistance(boxCorners(box.min, box.max), [0, 20, 0], az, 35, 60, 16 / 9);
    const cam = orbitCamera([0, 20, 0], az, 35, D, 60);
    const ang = cameraAngles(cam);
    assert.ok(Math.abs(ang.azimuth - az) < 1e-6 && Math.abs(ang.elevation - 35) < 1e-6 && Math.abs(ang.distance - D) < 1e-6);
    const v = renderView(input([]), cam, 160, 90);
    let edge = false;
    for (let k = 0; k < 8; k++) {
      const p = v.project([k & 1 ? 50 : -50, k & 2 ? 40 : 0, k & 4 ? 30 : -30])!;
      assert.ok(p[0] >= 0 && p[0] <= 160 && p[1] >= 0 && p[1] <= 90, `corner ${k} at ${p}`);
      if (p[0] < 8 || p[0] > 152 || p[1] < 5 || p[1] > 85) edge = true;
    }
    assert.ok(edge, "fitted tight: some corner is near the edge");
  }
});

/** A dense cone in 100 upright blocks at scale 2 (about 80 000 cells), and two rotated ones: Caldera's size. */
function cone(): Grid[] {
  const out: Grid[] = [];
  const S = 2, B = 6, Y = 22;
  for (let bz = 0; bz < 10; bz++)
    for (let bx = 0; bx < 10; bx++) {
      const toMap: Mat3x4 = [S, 0, 0, (bx - 5) * B * S, 0, S, 0, 0, 0, 0, S, (bz - 5) * B * S];
      const words = new Uint32Array(B * Y * B);
      for (let z = 0; z < B; z++)
        for (let x = 0; x < B; x++) {
          const mx = ((bx - 5) * B + x + 0.5) * S, mz = ((bz - 5) * B + z + 0.5) * S, r = Math.hypot(mx, mz);
          const hgt = Math.max(1, Math.min(Y, Math.round((44 * Math.exp(-((r / 30) ** 2)) - (r < 8 ? 10 * (1 - (r / 8) ** 2) : 0) + 2) / S)));
          for (let y = 0; y < hgt; y++) words[voxelIndex([B, Y, B], x, y, z)] = filled(y > hgt - 2 ? 3 : 1);
        }
      out.push({ id: out.length + 1, frame: null as never, ref: out.length + 1, size: [B, Y, B], words, base: words, toMap, fromMap: invert(toMap)!,
        bounds: { min: [toMap[3], 0, toMap[11]], max: [toMap[3] + B * S, Y * S, toMap[11] + B * S] } });
    }
  return out;
}

test("timing: 960x540 of the 400-frame level and of a 100-block cone stay well under 10 s", () => {
  const big = grids(store("synthetic-400.json"));
  const box = (gs: Grid[]) => {
    const min: Vec3 = [Infinity, Infinity, Infinity], max: Vec3 = [-Infinity, -Infinity, -Infinity];
    for (const g of gs) for (let k = 0; k < 3; k++) { min[k] = Math.min(min[k], g.bounds.min[k]); max[k] = Math.max(max[k], g.bounds.max[k]); }
    return { min, max, c: [0, 1, 2].map((k) => (min[k] + max[k]) / 2) as Vec3 };
  };
  for (const [name, gs] of [["synthetic-400", big], ["cone", cone()]] as const) {
    const b = box(gs);
    const cam = orbitCamera(b.c, 135, 35, fitDistance(boxCorners(b.min, b.max), b.c, 135, 35, 60, 16 / 9), 60);
    const cells = gs.reduce((n, g) => n + g.size[0] * g.size[1] * g.size[2], 0);
    for (const ss of [1, 2]) {
      const t0 = performance.now();
      const v = renderView(input(gs, { water: 2, fog: 400 }), cam, 960, 540, ss);
      const ms = performance.now() - t0;
      console.log(`render_view ${name}: ${gs.length} frames, ${cells} cells, ${v.faces} faces, ss ${ss}: ${Math.round(ms)} ms`);
      assert.ok(v.framesDrawn > gs.length / 2);
      assert.ok(ms < (ss === 1 ? 10000 : 30000), `${name} took ${ms} ms`);
    }
  }
});
