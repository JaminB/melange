// src/lib/inspect-field.ts and inspect-png.ts on the synthetic 12-frame level (rotated, scaled frames) and on an upright
// frame, bundled for Node by test.ps1. Spans are checked against a brute-force fine sampling of the same grids.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { inflateSync } from "node:zlib";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, filled, type Loaded, type Scene } from "../src/core";
import { columnSpans, extentXZ, markers, sampleField, spansAt } from "../src/lib/inspect-field";
import { Raster, encodePng, materialTint } from "../src/lib/inspect-png";
import { cellAt, grid, grids, isSolid, material, voxelIndex } from "../src/lib/voxel";

const scene = (): Scene => JSON.parse(readFileSync(new URL("../../../../tests/fixtures/erg/synthetic-12.json", import.meta.url), "utf8"));
function loaded(edit?: (s: Scene) => void): Loaded {
  const s = scene();
  edit?.(s);
  const blobs = new Map<number, ArrayBuffer>();
  for (const [ref, buf] of syntheticBlobs(s) as Map<number, Uint8Array>) blobs.set(ref, buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength) as ArrayBuffer);
  return { scene: s, blobs };
}
const store = (edit?: (s: Scene) => void) => new EditorStore("t", loaded(edit), loaded(edit));
/** Frame 5 (2x7x4) unrotated and unscaled, at an integer position. */
const upright = (s: Scene) => { const f = s.frames.find((x) => x.id === 5)!; f.rot = [0, 0, 0]; f.scale = [1, 1, 1]; f.pos = [10, 3, 20]; };

test("an upright frame's columns are read cell by cell with exact tops", () => {
  const st = store(upright);
  const g = grid(st, 5);
  // a known column: solid cells y = 1..2 and 5, the rest empty
  const [cx, cz] = [1, 2];
  for (let y = 0; y < g.size[1]; y++) g.words[voxelIndex(g.size, cx, y, cz)] = [1, 2, 5].includes(y) ? filled(9) : 0;
  const p = (gx: number, gz: number) => { const m = g.toMap; return [m[3] + gx + 0.5, m[11] + gz + 0.5]; };
  const [px, pz] = p(cx, cz);
  const sp = columnSpans(g, px, pz);
  assert.equal(sp.length, 2);
  const y0 = g.toMap[7];
  assert.deepEqual([sp[0].top, sp[0].bottom, sp[0].material, sp[0].frame], [y0 + 6, y0 + 5, 9, 5]);
  assert.deepEqual([sp[1].top, sp[1].bottom], [y0 + 3, y0 + 1]);
  assert.equal(columnSpans(g, px, pz, true).length, 1);
  assert.deepEqual(columnSpans(g, g.bounds.max[0] + 1, pz), []);
  // spansAt: clipping under a height, newest first
  const gs = grids(st);
  const under = spansAt(gs.filter((x) => x.id === 5), px, pz, y0 + 4);
  assert.deepEqual(under.map((s) => s.top), [y0 + 3]);
  const clipped = spansAt(gs.filter((x) => x.id === 5), px, pz, y0 + 5.5);
  assert.deepEqual(clipped.map((s) => [s.top, s.bottom]), [[y0 + 5.5, y0 + 5], [y0 + 3, y0 + 1]]);
});

test("rotated and scaled frames: sampled tops agree with a fine brute-force sampling", () => {
  const st = store();
  const gs = grids(st);
  let compared = 0;
  for (const g of gs) {
    const b = g.bounds;
    for (let k = 0; k < 40; k++) {
      const px = b.min[0] + ((k * 0.37) % 1) * (b.max[0] - b.min[0]), pz = b.min[2] + ((k * 0.61 + 0.1) % 1) * (b.max[2] - b.min[2]);
      let brute = -Infinity;
      for (let y = b.max[1]; y > b.min[1]; y -= 0.02) {
        const c = cellAt(g, [px, y, pz]);
        if (c && isSolid(g.words[voxelIndex(g.size, c[0], c[1], c[2])])) { brute = y; break; }
      }
      const got = columnSpans(g, px, pz, true)[0];
      const solidAt = (y: number) => { const c = cellAt(g, [px, y, pz]); return !!c && isSolid(g.words[voxelIndex(g.size, c[0], c[1], c[2])]); };
      // a tilted frame's edge can leave ground thinner than the half-voxel sampling: compare only where it is 0.6 thick
      if (brute === -Infinity || ![0.1, 0.2, 0.3, 0.4, 0.5, 0.6].every((d) => solidAt(brute - d))) continue;
      assert.ok(got, `frame ${g.id} column ${k}: no span but ground at ${brute}`);
      compared++;
      // sampled every half voxel and a scaled grid's voxels are 1.5 tall: within a voxel of the true surface
      assert.ok(Math.abs(got.top - brute) <= 1.6, `frame ${g.id}: top ${got.top} vs ${brute}`);
      assert.ok(material(0) === 0);
    }
  }
  assert.ok(compared > 100, `compared ${compared}`);
});

test("sampleField keeps the highest top over frames, with its frame and material", () => {
  const st = store(upright);
  const gs = grids(st);
  const ext = extentXZ(gs)!;
  assert.ok(ext.x1 > ext.x0 && ext.z1 > ext.z0);
  const f = sampleField(gs, ext, 2);
  assert.equal(f.w, Math.ceil((ext.x1 - ext.x0) / 2));
  assert.ok(f.solid > 0 && f.solid <= f.w * f.h);
  assert.ok(f.max >= f.min);
  let seen = 0;
  for (let k = 0; k < f.top.length; k++) {
    if (f.top[k] === -Infinity) { assert.equal(f.mat[k], -1); continue; }
    seen++;
    const x = f.x0 + ((k % f.w) + 0.5) * 2, z = f.z0 + (Math.floor(k / f.w) + 0.5) * 2;
    const best = Math.max(...gs.map((g) => columnSpans(g, x, z, true)[0]?.top ?? -Infinity));
    assert.equal(f.top[k], best);
    assert.ok(f.mat[k] >= 0 && f.mat[k] < 64);
  }
  assert.equal(seen, f.solid);
  // a region with no frames
  const none = sampleField(gs, { x0: ext.x1 + 50, z0: ext.z1 + 50, x1: ext.x1 + 60, z1: ext.z1 + 60 }, 1);
  assert.equal(none.solid, 0);
  assert.ok(Number.isNaN(none.min));
});

test("markers: spawns at their map positions", () => {
  const st = store();
  const m = markers(st);
  const spawns = m.filter((x) => x.kind === "spawn");
  assert.equal(spawns.length, st.scene.details.filter((d) => d.role === "spawn").length);
  assert.ok(spawns.every((x) => x.pos.every(Number.isFinite)));
});

test("PNG encoder: signature, chunk CRCs and pixels survive inflate", () => {
  const img = new Raster(5, 3, [10, 20, 30]);
  img.set(2, 1, [200, 100, 50]);
  img.text(0, 0, "12", [255, 255, 255]);
  const png = img.png();
  assert.deepEqual([...png.slice(0, 8)], [137, 80, 78, 71, 13, 10, 26, 10]);
  const dv = new DataView(png.buffer, png.byteOffset, png.byteLength);
  let at = 8, idat = new Uint8Array(0), ihdr: number[] = [];
  const types: string[] = [];
  while (at < png.length) {
    const len = dv.getUint32(at), type = String.fromCharCode(...png.slice(at + 4, at + 8));
    types.push(type);
    const body = png.slice(at + 8, at + 8 + len);
    if (type === "IHDR") ihdr = [dv.getUint32(at + 8), dv.getUint32(at + 12), body[8], body[9]];
    if (type === "IDAT") idat = body;
    // CRC-32 of type+data, checked with an independent bitwise implementation
    let c = 0xffffffff;
    for (const b of png.slice(at + 4, at + 8 + len)) { c ^= b; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; }
    assert.equal(dv.getUint32(at + 8 + len), (c ^ 0xffffffff) >>> 0, `${type} crc`);
    at += 12 + len;
  }
  assert.deepEqual(types, ["IHDR", "IDAT", "IEND"]);
  assert.deepEqual(ihdr, [5, 3, 8, 2]);
  const raw = inflateSync(idat);
  assert.equal(raw.length, 3 * (1 + 5 * 3));
  assert.deepEqual([...raw.slice(16 + 1 + 6, 16 + 1 + 9)], [200, 100, 50]);   // row 1, pixel 2
  assert.throws(() => encodePng(2, 2, new Uint8Array(5)));
  assert.notDeepEqual(materialTint(1), materialTint(2));
});
