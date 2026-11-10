// Caldera's own rock meshes for the detail layer (round 10: the judge's "mod mesh-bank import": "the single rock mesh caps
// rock quality"). Writes three glTF models and their basalt textures, procedurally and seeded, so the banks rebuild
// byte-for-byte:
//   caldera.Boulder   a rounded, weathered basalt boulder (an icosphere, two subdivisions, displaced by low noise, flat
//                     underside, smooth normals): replaces the 25-vertex faceted HORROR5 shard that read as glossy crystal
//   caldera.Ledge     three stacked strata slabs, each stepped back from the one under it, irregular outlines, bevelled
//                     edges: lies along a steep face's contour (place_mod_mesh / place_aligned axes: x length, y thickness, z depth)
//   caldera.Columns   a cluster of seven hexagonal basalt columns of stepped heights (columnar jointing): crests and cliff tops
// Units are the engine's (xom units; 20 = 1 map unit), Y up, the node matrix the identity. Each texture is mid grey
// (luminance about 0.5) so the scenery tint (Melange mg_tint: a texel of luminance 0.5 becomes the tint) maps it to the
// band's colour, with vesicles (dark pits), a fine grain and, on the ledge and columns, horizontal bands.
//
//   tools\node\node.exe tools\erg-mcp\bench\caldera\meshes\gen.mjs <outDir>
// then build.mjs (same folder) turns them into mesh banks with xomtool.
import fs from "node:fs";
import path from "node:path";
import zlib from "node:zlib";

export const MESHES = {
  Boulder: { section: 486, texture: "basalt" },
  Ledge: { section: 487, texture: "strata" },
  Columns: { section: 488, texture: "columns" },
};

function rng(seed) { let s = seed >>> 0; return () => ((s = (s * 1664525 + 1013904223) >>> 0) / 4294967296); }
// value noise in 3D (smooth), seeded
function noise3(seed) {
  const R = rng(seed), P = new Float32Array(4096);
  for (let i = 0; i < P.length; i++) P[i] = R() * 2 - 1;
  const h = (x, y, z) => P[((x * 73856093) ^ (y * 19349663) ^ (z * 83492791)) & 4095];
  const sm = (t) => t * t * (3 - 2 * t);
  return (x, y, z) => {
    const xi = Math.floor(x), yi = Math.floor(y), zi = Math.floor(z), xf = sm(x - xi), yf = sm(y - yi), zf = sm(z - zi);
    const l = (a, b, t) => a + (b - a) * t;
    return l(l(l(h(xi, yi, zi), h(xi + 1, yi, zi), xf), l(h(xi, yi + 1, zi), h(xi + 1, yi + 1, zi), xf), yf),
      l(l(h(xi, yi, zi + 1), h(xi + 1, yi, zi + 1), xf), l(h(xi, yi + 1, zi + 1), h(xi + 1, yi + 1, zi + 1), xf), yf), zf);
  };
}

// ---------------------------------------------------------------- geometry
class Mesh {
  constructor() { this.p = []; this.uv = []; this.idx = []; }
  v(p, uv) { this.p.push(p); this.uv.push(uv); return this.p.length - 1; }
  tri(a, b, c) { this.idx.push(a, b, c); }
  // smooth normals (area weighted) over shared vertices
  normals() {
    const n = this.p.map(() => [0, 0, 0]);
    for (let i = 0; i < this.idx.length; i += 3) {
      const [a, b, c] = [this.idx[i], this.idx[i + 1], this.idx[i + 2]], A = this.p[a], B = this.p[b], C = this.p[c];
      const u = [B[0] - A[0], B[1] - A[1], B[2] - A[2]], w = [C[0] - A[0], C[1] - A[1], C[2] - A[2]];
      const f = [u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]];
      for (const k of [a, b, c]) for (let j = 0; j < 3; j++) n[k][j] += f[j];
    }
    return n.map((v) => { const l = Math.hypot(...v) || 1; return v.map((x) => x / l); });
  }
}

function icosphere(sub) {
  const t = (1 + Math.sqrt(5)) / 2;
  let V = [[-1, t, 0], [1, t, 0], [-1, -t, 0], [1, -t, 0], [0, -1, t], [0, 1, t], [0, -1, -t], [0, 1, -t], [t, 0, -1], [t, 0, 1], [-t, 0, -1], [-t, 0, 1]]
    .map((v) => { const l = Math.hypot(...v); return v.map((x) => x / l); });
  let F = [[0, 11, 5], [0, 5, 1], [0, 1, 7], [0, 7, 10], [0, 10, 11], [1, 5, 9], [5, 11, 4], [11, 10, 2], [10, 7, 6], [7, 1, 8],
    [3, 9, 4], [3, 4, 2], [3, 2, 6], [3, 6, 8], [3, 8, 9], [4, 9, 5], [2, 4, 11], [6, 2, 10], [8, 6, 7], [9, 8, 1]];
  for (let s = 0; s < sub; s++) {
    const cache = new Map(), mid = (a, b) => {
      const k = a < b ? `${a},${b}` : `${b},${a}`;
      if (cache.has(k)) return cache.get(k);
      const m = V[a].map((x, i) => (x + V[b][i]) / 2), l = Math.hypot(...m);
      V.push(m.map((x) => x / l)); cache.set(k, V.length - 1); return V.length - 1;
    };
    F = F.flatMap(([a, b, c]) => { const ab = mid(a, b), bc = mid(b, c), ca = mid(c, a); return [[a, ab, ca], [b, bc, ab], [c, ca, bc], [ab, bc, ca]]; });
  }
  return { V, F };
}

/** A weathered boulder: radius ~30 (1.5 map), height ~34 over a flat base sunk to y = -4. */
function boulder() {
  const n = noise3(1101), n2 = noise3(1102), { V, F } = icosphere(3), m = new Mesh();
  for (const v of V) {
    // broad lumps, then smaller knobs; squashed (wider than tall), the bottom flattened
    const d = 1 + 0.22 * n(v[0] * 1.3 + 3, v[1] * 1.3, v[2] * 1.3) + 0.08 * n2(v[0] * 3.5, v[1] * 3.5 + 7, v[2] * 3.5);
    let x = v[0] * 31 * d, y = v[1] * 22 * d + 12, z = v[2] * 27 * d;
    if (y < -4) y = -4 + (y + 4) * 0.08;   // flat, slightly rounded underside
    // u round the sides, v up: a vertical cylinder projection (the texture tiles twice round)
    m.v([x, y, z], [Math.atan2(z, x) / Math.PI + 1, (y + 6) / 40]);
  }
  for (const [a, b, c] of F) m.tri(a, b, c);
  return m;
}

/** An extruded prism from a closed outline (xz, CCW seen from above), between y0 and y1, the top edge bevelled inward. */
function prism(m, outline, y0, y1, bevel, uvScale = 1 / 60) {
  const N = outline.length, cx = outline.reduce((s, p) => s + p[0], 0) / N, cz = outline.reduce((s, p) => s + p[1], 0) / N;
  const inset = outline.map(([x, z]) => { const dx = x - cx, dz = z - cz, l = Math.hypot(dx, dz) || 1; return [x - dx / l * bevel, z - dz / l * bevel]; });
  let per = 0; const acc = [0];
  for (let i = 0; i < N; i++) { const a = outline[i], b = outline[(i + 1) % N]; per += Math.hypot(b[0] - a[0], b[1] - a[1]); acc.push(per); }
  const ring = (pts, y, vv) => pts.map((p, i) => m.v([p[0], y, p[1]], [acc[i] * uvScale, vv]));
  const r0 = ring(outline, y0, y0 * uvScale), r1 = ring(outline, y1 - bevel, (y1 - bevel) * uvScale), r2 = ring(inset, y1, y1 * uvScale);
  // the seam vertex again at u = per (so the side texture does not smear back across the last quad)
  const s0 = m.v([outline[0][0], y0, outline[0][1]], [per * uvScale, y0 * uvScale]), s1 = m.v([outline[0][0], y1 - bevel, outline[0][1]], [per * uvScale, (y1 - bevel) * uvScale]);
  const s2 = m.v([inset[0][0], y1, inset[0][1]], [per * uvScale, y1 * uvScale]);
  for (let i = 0; i < N; i++) {
    const j = (i + 1) % N, last = j === 0;
    const a0 = r0[i], b0 = last ? s0 : r0[j], a1 = r1[i], b1 = last ? s1 : r1[j], a2 = r2[i], b2 = last ? s2 : r2[j];
    m.tri(a0, b0, b1); m.tri(a0, b1, a1);
    m.tri(a1, b1, b2); m.tri(a1, b2, a2);
  }
  // the top cap: a fan from the centre (planar uv)
  const c = m.v([cx, y1 + bevel * 0.15, cz], [cx * uvScale, cz * uvScale]);
  const top = inset.map((p) => m.v([p[0], y1, p[1]], [p[0] * uvScale, p[1] * uvScale]));
  for (let i = 0; i < N; i++) m.tri(c, top[(i + 1) % N], top[i]);
}

/** Three strata, each an irregular rounded slab stepped back (toward -z, into the face) from the one below. */
function ledge() {
  const R = rng(2202), n = noise3(2203), m = new Mesh();
  const layers = [[0, 9, 40, 16], [9, 17, 36, 13], [17, 24, 31, 10]];   // [y0, y1, half length, half depth]
  layers.forEach(([y0, y1, hl, hd], li) => {
    const back = li * 6, pts = [];
    // a rounded rectangle (superellipse) with noisy edges, 28 points
    for (let i = 0; i < 28; i++) {
      const a = (i / 28) * Math.PI * 2, c = Math.cos(a), s = Math.sin(a);
      const se = (v) => Math.sign(v) * Math.pow(Math.abs(v), 0.45);
      const j = 1 + 0.12 * n(c * 2 + li * 5, s * 2, li) + 0.05 * (R() - 0.5);
      pts.push([se(c) * hl * j + (R() - 0.5) * 2, se(s) * hd * j - back]);
    }
    prism(m, pts.reverse(), y0 - (li ? 1.5 : 4), y1, 2.2);
  });
  return m;
}

/** Seven hexagonal columns, radius ~7, heights stepped 20-44, tops tilted a little. */
function columns() {
  const R = rng(3303), m = new Mesh();
  const at = [[0, 0, 44], [13, 2, 36], [-12, 4, 38], [6, 12, 30], [-6, -12, 33], [7, -11, 26], [-14, -9, 22]];
  for (const [x, z, h] of at) {
    const r = 6.2 + R() * 1.2, rot = R() * Math.PI, pts = [];
    for (let i = 0; i < 6; i++) { const a = rot + (i / 6) * Math.PI * 2; pts.push([x + Math.cos(a) * r * (0.92 + R() * 0.1), z + Math.sin(a) * r * (0.92 + R() * 0.1)]); }
    prism(m, pts.reverse(), -6, h - R() * 3, 1.0, 1 / 45);
  }
  return m;
}

// ---------------------------------------------------------------- textures (128 x 128 RGB)
function texture(kind) {
  const W = 128, R = rng(kind === "basalt" ? 41 : kind === "strata" ? 42 : 43), n = noise3(kind.length * 977), px = Buffer.alloc(W * W * 3);
  const pits = Array.from({ length: kind === "basalt" ? 70 : 40 }, () => [R() * W, R() * W, 0.6 + R() * 1.8]);
  for (let y = 0; y < W; y++) for (let x = 0; x < W; x++) {
    // tileable: sample the noise on a torus
    const a = (x / W) * Math.PI * 2, b = (y / W) * Math.PI * 2, tx = Math.cos(a) * 2, ty = Math.sin(a) * 2, tz = Math.cos(b) * 2, tw = Math.sin(b) * 2;
    let v = 0.5 + 0.10 * n(tx * 1.5 + 9, ty * 1.5, tz * 1.5 + tw) + 0.05 * n(tx * 5, ty * 5 + tw * 2, tz * 5);
    v += (R() - 0.5) * 0.06;                                                       // grain
    if (kind !== "basalt") v += 0.05 * Math.sin((y / W) * Math.PI * 2 * (kind === "strata" ? 6 : 3) + 2 * n(tx, ty, tz)); // bands
    for (const [px0, py0, r] of pits) { let dx = Math.abs(x - px0), dy = Math.abs(y - py0); dx = Math.min(dx, W - dx); dy = Math.min(dy, W - dy); const d = Math.hypot(dx, dy); if (d < r) v -= 0.22 * (1 - d / r); }
    const c = Math.max(0, Math.min(1, v)), o = (y * W + x) * 3;
    // a whisper of warm grey (the tint replaces the hue; this only matters with Sunstone off)
    px[o] = Math.round(c * 255 * 1.02); px[o + 1] = Math.round(c * 255); px[o + 2] = Math.round(c * 255 * 0.96);
  }
  return png(W, W, px);
}
function png(w, h, rgb) {
  const crcT = new Int32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c; });
  const crc = (buf) => { let c = -1; for (const b of buf) c = crcT[(c ^ b) & 255] ^ (c >>> 8); return (c ^ -1) >>> 0; };
  const chunk = (t, d) => { const len = Buffer.alloc(4); len.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(t), d]); const cr = Buffer.alloc(4); cr.writeUInt32BE(crc(td)); return Buffer.concat([len, td, cr]); };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2;
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) { raw[y * (w * 3 + 1)] = 0; rgb.copy(raw, y * (w * 3 + 1) + 1, y * w * 3, (y + 1) * w * 3); }
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr), chunk("IDAT", zlib.deflateSync(raw, { level: 9 })), chunk("IEND", Buffer.alloc(0))]);
}

// ---------------------------------------------------------------- glTF
function gltf(name, m, dir) {
  const N = m.normals(), nv = m.p.length;
  if (nv > 65535) throw new Error(`${name}: ${nv} vertices (u16 indices)`);
  const pos = new Float32Array(m.p.flat()), nor = new Float32Array(N.flat()), uv = new Float32Array(m.uv.flat()), idx = new Uint32Array(m.idx);
  const bin = Buffer.concat([pos, nor, uv, idx].map((a) => Buffer.from(a.buffer)));
  const min = [0, 1, 2].map((i) => Math.min(...m.p.map((p) => p[i]))), max = [0, 1, 2].map((i) => Math.max(...m.p.map((p) => p[i])));
  let off = 0; const views = [pos, nor, uv, idx].map((a) => { const v = { buffer: 0, byteOffset: off, byteLength: a.byteLength }; off += a.byteLength; return v; });
  const doc = { asset: { version: "2.0", generator: "caldera meshes/gen.mjs" }, buffers: [{ uri: `${name}.bin`, byteLength: bin.length }], bufferViews: views,
    accessors: [{ bufferView: 0, componentType: 5126, count: nv, type: "VEC3", min, max }, { bufferView: 1, componentType: 5126, count: nv, type: "VEC3" },
      { bufferView: 2, componentType: 5126, count: nv, type: "VEC2" }, { bufferView: 3, componentType: 5125, count: idx.length, type: "SCALAR" }],
    meshes: [{ name, primitives: [{ attributes: { POSITION: 0, NORMAL: 1, TEXCOORD_0: 2 }, indices: 3, mode: 4 }] }],
    nodes: [{ name, mesh: 0 }], scenes: [{ nodes: [0] }], scene: 0 };
  fs.writeFileSync(path.join(dir, `${name}.bin`), bin);
  fs.writeFileSync(path.join(dir, `${name}.gltf`), JSON.stringify(doc, null, 1));
  return { vertices: nv, triangles: idx.length / 3, min, max };
}

export function generate(dir) {
  fs.mkdirSync(dir, { recursive: true });
  const out = {};
  for (const [name, mk] of [["Boulder", boulder], ["Ledge", ledge], ["Columns", columns]]) {
    const info = gltf(name, mk(), dir);
    fs.writeFileSync(path.join(dir, `${MESHES[name].texture}.png`), texture(MESHES[name].texture));
    // the box in map units (xom / 20): place_mod_mesh's `box`
    out[`caldera.${name}`] = { ...info, section: MESHES[name].section, texture: `${MESHES[name].texture}.png`,
      box: { min: info.min.map((v) => +(v / 20).toFixed(4)), max: info.max.map((v) => +(v / 20).toFixed(4)) } };
  }
  fs.writeFileSync(path.join(dir, "meshes.json"), JSON.stringify(out, null, 1));
  return out;
}

if (process.argv[1] && path.resolve(process.argv[1]) === path.resolve(new URL(import.meta.url).pathname.replace(/^\/(\w:)/, "$1"))) {
  console.log(JSON.stringify(generate(path.resolve(process.argv[2] ?? "out/meshes")), null, 1));
}
