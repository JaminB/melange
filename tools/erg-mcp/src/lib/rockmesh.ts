// Procedural rock meshes for a map's mod mesh bank (the generalised core of bench/caldera/meshes/gen.mjs, whose hull rocks
// it reproduces byte for byte at Caldera's seeds and palettes: test/rockbank.test.ts). Pure geometry, textures and glTF
// text; src/lib/rockbank.ts turns them into a bank. Units are the engine's (xom units, 20 = 1 map unit), Y up.
//   boulder/slab/wedge/shard  faceted convex hulls: a chamfered block, a flat slab with a tilted top, a block whose top
//                             falls away to one side, a squat pointed shard. Flat shaded (every face its own vertices).
//   strata                    stacked irregular slabs, each stepped back from the one below: lies along a steep face.
//   scree                     a heap of small hulls in one mesh: fills a slope's foot or a boulder field with one copy.
// The albedo is BAKED into the texture (the released Sunstone has no tint shader, docs/erg-mcp.md detail round 11): top
// faces dusty, sides darkening toward the foot.
import { deflateSync } from "node:zlib";

export type RockKind = "boulder" | "slab" | "wedge" | "shard" | "strata" | "scree";
export const ROCK_KINDS: readonly RockKind[] = ["boulder", "slab", "wedge", "shard", "strata", "scree"];
type V3 = [number, number, number];
type V2 = [number, number];

/** A rock palette: the stone `base` and the `dust` laid over the top faces (sRGB bytes), the share of dust (1 = Caldera's
 * ash cover, 0.3 = bare rock), and the texture's noise seeds [grain, noise]. */
export interface Palette { base: V3; dust: V3; dustShare?: number; textureSeed?: [number, number]; }
export const PALETTES: Record<string, Palette> = {
  basalt: { base: [0x6c, 0x53, 0x3c], dust: [0x98, 0x82, 0x64], dustShare: 1, textureSeed: [47, 7707] },
  sandstone: { base: [0xa4, 0x7c, 0x52], dust: [0xd2, 0xb2, 0x84], dustShare: 0.8, textureSeed: [47, 7707] },
  redrock: { base: [0x92, 0x4f, 0x37], dust: [0xc6, 0x88, 0x60], dustShare: 0.7, textureSeed: [47, 7707] },
  limestone: { base: [0xae, 0xa4, 0x92], dust: [0xd8, 0xd0, 0xc0], dustShare: 0.7, textureSeed: [47, 7707] },
};

function rng(seed: number) { let s = seed >>> 0; return () => ((s = (s * 1664525 + 1013904223) >>> 0) / 4294967296); }
function noise3(seed: number) {
  const R = rng(seed), P = new Float32Array(4096);
  for (let i = 0; i < P.length; i++) P[i] = R() * 2 - 1;
  const h = (x: number, y: number, z: number) => P[((x * 73856093) ^ (y * 19349663) ^ (z * 83492791)) & 4095];
  const sm = (t: number) => t * t * (3 - 2 * t);
  return (x: number, y: number, z: number) => {
    const xi = Math.floor(x), yi = Math.floor(y), zi = Math.floor(z), xf = sm(x - xi), yf = sm(y - yi), zf = sm(z - zi);
    const l = (a: number, b: number, t: number) => a + (b - a) * t;
    return l(l(l(h(xi, yi, zi), h(xi + 1, yi, zi), xf), l(h(xi, yi + 1, zi), h(xi + 1, yi + 1, zi), xf), yf),
      l(l(h(xi, yi, zi + 1), h(xi + 1, yi, zi + 1), xf), l(h(xi, yi + 1, zi + 1), h(xi + 1, yi + 1, zi + 1), xf), yf), zf);
  };
}

export class Mesh {
  p: V3[] = []; uv: V2[] = []; idx: number[] = [];
  v(p: V3, uv: V2) { this.p.push(p); this.uv.push(uv); return this.p.length - 1; }
  tri(a: number, b: number, c: number) { this.idx.push(a, b, c); }
  /** Smooth normals (area weighted) over shared vertices. */
  normals(): V3[] {
    const n: V3[] = this.p.map(() => [0, 0, 0]);
    for (let i = 0; i < this.idx.length; i += 3) {
      const [a, b, c] = [this.idx[i], this.idx[i + 1], this.idx[i + 2]], A = this.p[a], B = this.p[b], C = this.p[c];
      const u = [B[0] - A[0], B[1] - A[1], B[2] - A[2]], w = [C[0] - A[0], C[1] - A[1], C[2] - A[2]];
      const f = [u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]];
      for (const k of [a, b, c]) for (let j = 0; j < 3; j++) n[k][j] += f[j];
    }
    return n.map((v) => { const l = Math.hypot(...v) || 1; return [v[0] / l, v[1] / l, v[2] / l] as V3; });
  }
}

type Face = [number, number, number];
/** The convex hull of points (brute force over triples: n is ~20-30), triangles wound counter-clockwise from outside. */
function hull(P: V3[]): Face[] {
  const F: Face[] = [], n = P.length, sub = (a: V3, b: V3): V3 => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
  const cross = (u: V3, w: V3): V3 => [u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]];
  const dot = (u: V3, w: V3) => u[0] * w[0] + u[1] * w[1] + u[2] * w[2];
  for (let i = 0; i < n; i++) for (let j = i + 1; j < n; j++) for (let k = j + 1; k < n; k++) {
    const nn = cross(sub(P[j], P[i]), sub(P[k], P[i])), l = Math.hypot(...nn);
    if (l < 1e-6) continue;
    let pos = 0, neg = 0;
    for (let m = 0; m < n && !(pos && neg); m++) { if (m === i || m === j || m === k) continue; const d = dot(nn, sub(P[m], P[i])) / l; if (d > 1e-4) pos++; else if (d < -1e-4) neg++; }
    if ((pos && neg) || (!pos && !neg)) continue;
    F.push(pos ? [i, k, j] : [i, j, k]);
  }
  return F;
}

/** Where a hull rock goes inside a bigger mesh (scree): scale, yaw (radians) and offset, all in xom units. */
interface Xf { scale: number; yaw: number; at: V3; }

/** A faceted rock: the hull of chamfered-box (or shard) points, jittered; the underside flat at about y = -4. Appends to
 * `m` (a fresh mesh by default). Caldera's gen.mjs hullRock, the same random draws in the same order. */
export function hullRock(kind: "blocky" | "slab" | "wedge" | "shard", seed: number, m = new Mesh(), xf?: Xf): Mesh {
  const R = rng(seed), J = (a: number) => (R() - 0.5) * 2 * a, P: V3[] = [];
  const [hx, hy, hz] = { blocky: [30, 34, 26], slab: [34, 15, 25], wedge: [30, 36, 25], shard: [27, 22, 23] }[kind];
  if (kind === "shard") {
    for (let i = 0; i < 7; i++) { const a = (i / 7) * Math.PI * 2 + J(0.25); P.push([Math.cos(a) * hx * (0.8 + R() * 0.25), -4 - R() * 0.6, Math.sin(a) * hz * (0.8 + R() * 0.25)]); }
    for (let i = 0; i < 5; i++) { const a = (i / 5) * Math.PI * 2 + J(0.4); P.push([Math.cos(a) * hx * 0.62 + 4, hy * (0.4 + R() * 0.2), Math.sin(a) * hz * 0.6]); }
    P.push([8 + J(2), hy, J(3)], [-2 + J(2), hy * 0.86, 6 + J(2)], [14 + J(2), hy * 0.7, -5 + J(2)]);
  } else {
    for (const sx of [-1, 1]) for (const sz of [-1, 1]) for (const top of [0, 1]) {
      let y = top ? hy * (0.82 + R() * 0.18) : -4 - R() * 0.6;
      if (top && kind === "wedge" && sx > 0) y = hy * (0.3 + R() * 0.12);
      if (top && kind === "slab") y = hy * (0.75 + R() * 0.25) + sz * 3;
      const x = sx * hx * (0.85 + R() * 0.15), z = sz * hz * (0.85 + R() * 0.15), c = (top ? 7 : 3) + R() * (top ? 6 : 3);
      const cy = top ? -c * (kind === "slab" ? 0.5 : 0.9) : 0;
      P.push([x - sx * c, y, z + J(1.5)], [x + J(1.5), y, z - sz * c], [x, y + cy, z]);
      if (top) P.push([x - sx * c * 0.3 + J(1), y + cy * 0.4, z - sz * c * 0.3 + J(1)]);
    }
    for (let i = 0; i < 2; i++) { const a = R() * Math.PI * 2; P.push([Math.cos(a) * hx * 1.02, hy * (0.25 + R() * 0.35), Math.sin(a) * hz * 1.02]); }
  }
  const F = hull(P);
  const ys = P.map((p) => p[1]), y0 = Math.min(...ys), y1 = Math.max(...ys);
  const put = (p: V3): V3 => {
    if (!xf) return p;
    const c = Math.cos(xf.yaw), s = Math.sin(xf.yaw), x = p[0] * xf.scale, z = p[2] * xf.scale;
    return [x * c + z * s + xf.at[0], p[1] * xf.scale + xf.at[1], -x * s + z * c + xf.at[2]];
  };
  for (const [a, b, c] of F) {
    const A = P[a], B = P[b], C = P[c];
    const u = [B[0] - A[0], B[1] - A[1], B[2] - A[2]], w = [C[0] - A[0], C[1] - A[1], C[2] - A[2]];
    const nn = [u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]], l = Math.hypot(...nn) || 1, ny = nn[1] / l;
    let uvs: V2[];
    if (ny < -0.9) uvs = [[0.5, 0.97], [0.5, 0.97], [0.5, 0.97]];
    else if (ny > 0.55) {
      const off = R() * 0.5;
      uvs = [A, B, C].map((p) => [off + (p[0] + 40) / 160, 0.03 + Math.max(0, Math.min(0.44, (p[2] + 40) / 182))] as V2);
    } else {
      const hxz = Math.hypot(nn[0], nn[2]) || 1, tx = -nn[2] / hxz, tz = nn[0] / hxz, off = R() * 0.5;
      uvs = [A, B, C].map((p) => [off + (p[0] * tx + p[2] * tz + 60) / 130, 0.97 - 0.44 * (p[1] - y0) / (y1 - y0 || 1)] as V2);
    }
    m.tri(m.v(put(A), uvs[0]), m.v(put(B), uvs[1]), m.v(put(C), uvs[2]));
  }
  return m;
}

/** An extruded prism from a closed outline (xz, counter-clockwise from above) between y0 and y1, the top edge bevelled inward. */
function prism(m: Mesh, outline: V2[], y0: number, y1: number, bevel: number, uvScale = 1 / 60) {
  const N = outline.length, cx = outline.reduce((s, p) => s + p[0], 0) / N, cz = outline.reduce((s, p) => s + p[1], 0) / N;
  const inset = outline.map(([x, z]) => { const dx = x - cx, dz = z - cz, l = Math.hypot(dx, dz) || 1; return [x - dx / l * bevel, z - dz / l * bevel] as V2; });
  let per = 0; const acc = [0];
  for (let i = 0; i < N; i++) { const a = outline[i], b = outline[(i + 1) % N]; per += Math.hypot(b[0] - a[0], b[1] - a[1]); acc.push(per); }
  const ring = (pts: V2[], y: number, vv: number) => pts.map((p, i) => m.v([p[0], y, p[1]], [acc[i] * uvScale, vv]));
  const r0 = ring(outline, y0, y0 * uvScale), r1 = ring(outline, y1 - bevel, (y1 - bevel) * uvScale), r2 = ring(inset, y1, y1 * uvScale);
  const s0 = m.v([outline[0][0], y0, outline[0][1]], [per * uvScale, y0 * uvScale]), s1 = m.v([outline[0][0], y1 - bevel, outline[0][1]], [per * uvScale, (y1 - bevel) * uvScale]);
  const s2 = m.v([inset[0][0], y1, inset[0][1]], [per * uvScale, y1 * uvScale]);
  for (let i = 0; i < N; i++) {
    const j = (i + 1) % N, last = j === 0;
    const a0 = r0[i], b0 = last ? s0 : r0[j], a1 = r1[i], b1 = last ? s1 : r1[j], a2 = r2[i], b2 = last ? s2 : r2[j];
    m.tri(a0, b0, b1); m.tri(a0, b1, a1);
    m.tri(a1, b1, b2); m.tri(a1, b2, a2);
  }
  const c = m.v([cx, y1 + bevel * 0.15, cz], [cx * uvScale, cz * uvScale]);
  const top = inset.map((p) => m.v([p[0], y1, p[1]], [p[0] * uvScale, p[1] * uvScale]));
  for (let i = 0; i < N; i++) m.tri(c, top[(i + 1) % N], top[i]);
}

/** Caldera's three strata: [y0, y1, half length, half depth]. */
const CALDERA_LAYERS: [number, number, number, number][] = [[0, 9, 40, 16], [9, 17, 36, 13], [17, 24, 31, 10]];
export interface StrataOpts { layers?: number; length?: number; depth?: number; thickness?: number; }
/** Stacked strata slabs, each an irregular rounded slab stepped back (toward -z, into the face) from the one below. Without
 * options it is Caldera's three-layer ledge; `layers` (1-8), `length` and `depth` (half sizes, xom units) and `thickness`
 * (one layer's) build others, each layer a little shorter, shallower and thinner than the last. */
export function strata(seed: number, o: StrataOpts = {}): Mesh {
  const R = rng(seed), n = noise3(seed + 1), m = new Mesh();
  let layers = CALDERA_LAYERS;
  if (o.layers !== undefined || o.length !== undefined || o.depth !== undefined || o.thickness !== undefined) {
    const count = o.layers ?? 3, L = o.length ?? 40, D = o.depth ?? 16, T = o.thickness ?? 9;
    layers = []; let y = 0;
    for (let i = 0; i < count; i++) {
      const t = Math.max(3, T - i), f = 1 - 0.1 * i;
      layers.push([y, y + t, Math.max(6, L * f), Math.max(4, D - 3 * i * (D / 16))]); y += t;
    }
  }
  layers.forEach(([y0, y1, hl, hd], li) => {
    const back = li * 6, pts: V2[] = [];
    for (let i = 0; i < 28; i++) {
      const a = (i / 28) * Math.PI * 2, c = Math.cos(a), s = Math.sin(a);
      const se = (v: number) => Math.sign(v) * Math.pow(Math.abs(v), 0.45);
      const j = 1 + 0.12 * n(c * 2 + li * 5, s * 2, li) + 0.05 * (R() - 0.5);
      pts.push([se(c) * hl * j + (R() - 0.5) * 2, se(s) * hd * j - back]);
    }
    prism(m, pts.reverse(), y0 - (li ? 1.5 : 4), y1, 2.2);
  });
  return m;
}

export interface ScreeOpts { rocks?: number; radius?: number; }
/** A heap of `rocks` (3-24, default 9) small faceted hulls in a disc of `radius` (xom units, default 70): bigger and
 * higher at the middle, smaller and sunk toward the rim. One mesh, so a scree fan or a boulder field costs one copy. */
export function scree(seed: number, o: ScreeOpts = {}): Mesh {
  const R = rng(seed), m = new Mesh(), count = o.rocks ?? 9, rad = o.radius ?? 70;
  const kinds = ["blocky", "slab", "wedge", "shard"] as const;
  for (let i = 0; i < count; i++) {
    const a = R() * Math.PI * 2, d = Math.sqrt((i + 0.5) / count) * rad * (0.8 + R() * 0.2), mid = 1 - d / rad;
    hullRock(kinds[Math.floor(R() * 4)], Math.floor(R() * 1e6) + 1, m,
      { scale: 0.2 + 0.22 * mid + R() * 0.1, yaw: R() * Math.PI * 2, at: [Math.cos(a) * d, mid * 7 - 2, Math.sin(a) * d] });
  }
  return m;
}

// ---------------------------------------------------------------- textures (128 x 128 RGB)
/** The hull rocks' baked albedo: rows 0-63 the top faces (dust over the stone), rows 64-127 the sides, dustier at row 64
 * (a face's top) down to the bare stone at row 127 (its foot); vesicle pits and a fine grain. */
export function rockTexture(p: Palette): Uint8Array {
  const [rs, ns] = p.textureSeed ?? [47, 7707], ds = p.dustShare ?? 1, base = p.base, DUST = p.dust;
  const W = 128, R = rng(rs), n = noise3(ns), px = new Uint8Array(W * W * 3);
  const pits = Array.from({ length: 60 }, () => [R() * W, R() * W, 0.6 + R() * 1.6]);
  for (let y = 0; y < W; y++) for (let x = 0; x < W; x++) {
    const a = (x / W) * Math.PI * 2, tx = Math.cos(a) * 2, ty = Math.sin(a) * 2;
    const nv = n(tx * 1.5 + 9, ty * 1.5, y / 22), fine = n(tx * 5, ty * 5 + 3, y / 7);
    let v = 1 + 0.16 * nv + 0.08 * fine + (R() - 0.5) * 0.08;
    for (const [px0, py0, r] of pits) { let dx = Math.abs(x - px0), dy = Math.abs(y - py0); dx = Math.min(dx, W - dx); dy = Math.min(dy, W - dy); const d = Math.hypot(dx, dy); if (d < r) v -= 0.35 * (1 - d / r); }
    const dust = ds * (y < 64 ? Math.max(0, Math.min(0.65, 0.42 + 0.35 * n(tx * 2 + 4, ty * 2, y / 12))) : Math.max(0, 0.24 * (1 - (y - 64) / 50) + 0.08 * fine));
    const o = (y * W + x) * 3;
    for (let k = 0; k < 3; k++) px[o + k] = Math.max(0, Math.min(255, Math.round((base[k] * (1 - dust) + DUST[k] * dust) * v)));
  }
  return png(W, W, px);
}
/** The strata mesh's texture: its uv is (along the outline, y) / 60, so the rows are horizontal beds, alternately stone and
 * dust-pale, with wavy edges, pits and grain; tiles in v. */
export function strataTexture(p: Palette): Uint8Array {
  const [rs, ns] = p.textureSeed ?? [47, 7707], ds = p.dustShare ?? 1, W = 128, R = rng(rs + 1), n = noise3(ns + 1), px = new Uint8Array(W * W * 3);
  const pits = Array.from({ length: 40 }, () => [R() * W, R() * W, 0.6 + R() * 1.4]);
  for (let y = 0; y < W; y++) for (let x = 0; x < W; x++) {
    const a = (x / W) * Math.PI * 2, tx = Math.cos(a) * 2, ty = Math.sin(a) * 2, bed = (y / W) * Math.PI * 2 * 7 + 1.6 * n(tx, ty, y / 30);
    let v = 1 + 0.12 * n(tx * 1.5 + 9, ty * 1.5, y / 22) + 0.06 * n(tx * 5, ty * 5 + 3, y / 7) + (R() - 0.5) * 0.08;
    for (const [px0, py0, r] of pits) { let dx = Math.abs(x - px0), dy = Math.abs(y - py0); dx = Math.min(dx, W - dx); dy = Math.min(dy, W - dy); const d = Math.hypot(dx, dy); if (d < r) v -= 0.3 * (1 - d / r); }
    const dust = ds * Math.max(0, Math.min(0.6, 0.3 + 0.3 * Math.sin(bed)));
    const o = (y * W + x) * 3;
    for (let k = 0; k < 3; k++) px[o + k] = Math.max(0, Math.min(255, Math.round((p.base[k] * (1 - dust) + p.dust[k] * dust) * v)));
  }
  return png(W, W, px);
}

function concat(parts: Uint8Array[]): Uint8Array {
  const out = new Uint8Array(parts.reduce((s, p) => s + p.length, 0));
  let o = 0;
  for (const p of parts) { out.set(p, o); o += p.length; }
  return out;
}
const be32 = (v: number) => { const b = new Uint8Array(4); new DataView(b.buffer).setUint32(0, v >>> 0); return b; };
const ascii = (s: string) => Uint8Array.from(s, (c) => c.charCodeAt(0));
function png(w: number, h: number, rgb: Uint8Array): Uint8Array {
  const crcT = new Int32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c; });
  const crc = (buf: Uint8Array) => { let c = -1; for (const b of buf) c = crcT[(c ^ b) & 255] ^ (c >>> 8); return (c ^ -1) >>> 0; };
  const chunk = (t: string, d: Uint8Array) => { const td = concat([ascii(t), d]); return concat([be32(d.length), td, be32(crc(td))]); };
  const ihdr = new Uint8Array(13); ihdr.set(be32(w), 0); ihdr.set(be32(h), 4); ihdr[8] = 8; ihdr[9] = 2;
  const raw = new Uint8Array((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) { raw[y * (w * 3 + 1)] = 0; raw.set(rgb.subarray(y * w * 3, (y + 1) * w * 3), y * (w * 3 + 1) + 1); }
  return concat([Uint8Array.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr), chunk("IDAT", deflateSync(raw, { level: 9 })), chunk("IEND", new Uint8Array(0))]);
}

// ---------------------------------------------------------------- glTF
export interface Gltf { gltf: string; bin: Uint8Array; vertices: number; triangles: number; min: V3; max: V3; }
/** The glTF text (referencing `<name>.bin`) and binary of a mesh. The u16 vertex limit is the bank's. */
export function gltf(name: string, m: Mesh, generator = "erg-mcp rockmesh"): Gltf {
  const N = m.normals(), nv = m.p.length;
  if (nv > 65535) throw new Error(`${name}: ${nv} vertices (u16 indices)`);
  const arrays = [new Float32Array(m.p.flat()), new Float32Array(N.flat()), new Float32Array(m.uv.flat()), new Uint32Array(m.idx)];
  const bin = concat(arrays.map((a) => new Uint8Array(a.buffer)));
  const min = [0, 1, 2].map((i) => Math.min(...m.p.map((p) => p[i]))) as V3, max = [0, 1, 2].map((i) => Math.max(...m.p.map((p) => p[i]))) as V3;
  let off = 0; const views = arrays.map((a) => { const v = { buffer: 0, byteOffset: off, byteLength: a.byteLength }; off += a.byteLength; return v; });
  const idxCount = m.idx.length;
  const doc = { asset: { version: "2.0", generator }, buffers: [{ uri: `${name}.bin`, byteLength: bin.length }], bufferViews: views,
    accessors: [{ bufferView: 0, componentType: 5126, count: nv, type: "VEC3", min, max }, { bufferView: 1, componentType: 5126, count: nv, type: "VEC3" },
      { bufferView: 2, componentType: 5126, count: nv, type: "VEC2" }, { bufferView: 3, componentType: 5125, count: idxCount, type: "SCALAR" }],
    meshes: [{ name, primitives: [{ attributes: { POSITION: 0, NORMAL: 1, TEXCOORD_0: 2 }, indices: 3, mode: 4 }] }],
    nodes: [{ name, mesh: 0 }], scenes: [{ nodes: [0] }], scene: 0 };
  return { gltf: JSON.stringify(doc, null, 1), bin, vertices: nv, triangles: idxCount / 3, min, max };
}

// ---------------------------------------------------------------- the spec
export interface RockSpec {
  /** The mesh's name after the mod id: `<id>.<name>` (letters, digits, `_.-`). */
  name: string;
  kind: RockKind;
  /** Overrides the derived seed (the kind's seed + the map `seed` + 101 per earlier rock of the kind). */
  seed?: number;
  /** A palette name or a palette, for this rock only. */
  palette?: string | Palette;
  /** Multiplies the rock's size (0.1-4; default 1). Applied to the finished positions. */
  scale?: number;
  strata?: StrataOpts;
  scree?: ScreeOpts;
}
/** The default seeds of each kind: Caldera's four hulls and its ledge. */
const KIND_SEED: Record<RockKind, number> = { boulder: 1101, slab: 1202, wedge: 1303, shard: 1404, strata: 2202, scree: 5505 };
const HULL: Record<string, "blocky" | "slab" | "wedge" | "shard"> = { boulder: "blocky", slab: "slab", wedge: "wedge", shard: "shard" };
export const DEFAULT_ROCKS: RockSpec[] = [{ name: "Boulder", kind: "boulder" }, { name: "Slab", kind: "slab" }, { name: "Wedge", kind: "wedge" }, { name: "Shard", kind: "shard" }];

export function resolvePalette(p: string | Palette | undefined, fallback: Palette = PALETTES.basalt): Palette {
  if (p === undefined) return fallback;
  if (typeof p === "string") { const q = PALETTES[p]; if (!q) throw new Error(`unknown palette "${p}" (${Object.keys(PALETTES).join(", ")})`); return q; }
  return p;
}

export interface BuiltRock {
  name: string; kind: RockKind; seed: number; palette: Palette; textureKey: string; mesh: Gltf;
  /** The triangles in map units (9 numbers each) and the overall colour, for render_view's scenery drawing. */
  tris: Float32Array; albedo: V3;
  /** The box in map units (xom / 20): place_mod_mesh's `box`. */
  box: { min: V3; max: V3 };
}
export interface BuiltRocks { rocks: BuiltRock[]; textures: Map<string, Uint8Array>; }

/** Builds the meshes and textures of a rock set: deterministic in (rocks, palette, seed). Rocks sharing a palette and a
 * texture kind share one texture file (`textureKey`). */
export function buildRocks(rocks: RockSpec[], opts: { palette?: string | Palette; seed?: number; generator?: string } = {}): BuiltRocks {
  const base = resolvePalette(opts.palette), mapSeed = opts.seed ?? 0, seen = new Map<RockKind, number>(), names = new Set<string>();
  const textures = new Map<string, Uint8Array>(), out: BuiltRock[] = [];
  for (const r of rocks) {
    if (names.has(r.name)) throw new Error(`two rocks named ${r.name}`);
    names.add(r.name);
    const nth = seen.get(r.kind) ?? 0; seen.set(r.kind, nth + 1);
    const seed = r.seed ?? KIND_SEED[r.kind] + mapSeed + 101 * nth, pal = resolvePalette(r.palette, base);
    let mesh: Mesh;
    if (r.kind === "strata") mesh = strata(seed, r.strata);
    else if (r.kind === "scree") mesh = scree(seed, r.scree);
    else mesh = hullRock(HULL[r.kind], seed);
    if (r.scale !== undefined && r.scale !== 1) mesh.p = mesh.p.map((p) => [p[0] * r.scale!, p[1] * r.scale!, p[2] * r.scale!] as V3);
    const key = `${r.kind === "strata" ? "strata" : "rock"}-${pal.base.join("_")}-${pal.dust.join("_")}-${pal.dustShare ?? 1}-${(pal.textureSeed ?? [47, 7707]).join("_")}`;
    if (!textures.has(key)) textures.set(key, r.kind === "strata" ? strataTexture(pal) : rockTexture(pal));
    const g = gltf(r.name, mesh, opts.generator);
    const tris = new Float32Array(mesh.idx.length * 3), dv = 0.35 * (pal.dustShare ?? 1);
    mesh.idx.forEach((vi, k) => { for (let a = 0; a < 3; a++) tris[k * 3 + a] = mesh.p[vi][a] / 20; });
    out.push({ name: r.name, kind: r.kind, seed, palette: pal, textureKey: key, mesh: g, tris,
      albedo: [0, 1, 2].map((k) => Math.round(pal.base[k] * (1 - dv) + pal.dust[k] * dv)) as V3,
      box: { min: g.min.map((v) => +(v / 20).toFixed(4)) as V3, max: g.max.map((v) => +(v / 20).toFixed(4)) as V3 } });
  }
  return { rocks: out, textures };
}
