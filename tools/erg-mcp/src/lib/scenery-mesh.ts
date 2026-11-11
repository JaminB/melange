// Scenery meshes for the offline views and the art catalog: a detail's preview mesh (the server's /erg/assets/mesh_<resource>.glb,
// the Erg panel's own) as triangles and its colour. The server writes one flat material per mesh whose baseColorFactor is
// the average of the mesh's texture (src/erg/preview.cpp), so `albedo` is the piece's overall colour: what a dressing pass
// needs to pick dark rock for a basalt cone. Map units (the mesh's xan units divided by WORLD_PER_XAN, as
// backdrop.ts's meshBounds). Pure parsing plus a fetch cache per server.
import { WORLD_PER_XAN, apply as applyMat, type Detail, type Frames, type Vec3 } from "../core";
import { tintOf } from "./tint";
import type { Session } from "../session";

export type Rgb = [number, number, number];
export interface MeshData {
  tris: Float32Array;        // 9 numbers per triangle, the mesh's own frame, map units
  albedo: Rgb;               // sRGB 0-255: the texture's average (the material's baseColorFactor)
  min: Vec3; max: Vec3;
}

const toSrgb = (l: number) => Math.round(255 * Math.max(0, Math.min(1, l <= 0.0031308 ? 12.92 * l : 1.055 * Math.pow(l, 1 / 2.4) - 0.055)));
/** Relative luminance 0-1 of an sRGB colour (Rec. 709 weights on the linear values). */
export function luminance(c: Rgb): number {
  const lin = (v: number) => { const s = v / 255; return s <= 0.04045 ? s / 12.92 : Math.pow((s + 0.055) / 1.055, 2.4); };
  return Math.round((0.2126 * lin(c[0]) + 0.7152 * lin(c[1]) + 0.0722 * lin(c[2])) * 1000) / 1000;
}
export const hex = (c: Rgb) => `#${c.map((v) => v.toString(16).padStart(2, "0")).join("")}`;

type M4 = number[];
const mul4 = (a: M4, b: M4): M4 => { const o = new Array<number>(16).fill(0); for (let c = 0; c < 4; c++) for (let r = 0; r < 4; r++) for (let k = 0; k < 4; k++) o[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k]; return o; };
function trs(n: { matrix?: number[]; translation?: number[]; rotation?: number[]; scale?: number[] }): M4 {
  if (n.matrix?.length === 16) return n.matrix;
  const [x, y, z, w] = n.rotation ?? [0, 0, 0, 1], [sx, sy, sz] = n.scale ?? [1, 1, 1], [tx, ty, tz] = n.translation ?? [0, 0, 0];
  return [(1 - 2 * (y * y + z * z)) * sx, 2 * (x * y + z * w) * sx, 2 * (x * z - y * w) * sx, 0,
    2 * (x * y - z * w) * sy, (1 - 2 * (x * x + z * z)) * sy, 2 * (y * z + x * w) * sy, 0,
    2 * (x * z + y * w) * sz, 2 * (y * z - x * w) * sz, (1 - 2 * (x * x + y * y)) * sz, 0, tx, ty, tz, 1];
}

/** A GLB's triangles (TRIANGLES primitives through the default scene's node transforms) and the first material's
 * baseColorFactor as sRGB, in the GLB's own units; null for anything this reader does not handle. */
export function glbMesh(bytes: Uint8Array, unit = 1): MeshData | null {
  if (bytes.length < 28) return null;
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (v.getUint32(0, true) !== 0x46546c67) return null;
  const jlen = v.getUint32(12, true);
  if (v.getUint32(16, true) !== 0x4e4f534a || 20 + jlen > bytes.length) return null;
  let doc: any;
  try { doc = JSON.parse(new TextDecoder().decode(bytes.subarray(20, 20 + jlen))); } catch { return null; }
  let bin: Uint8Array | null = null;
  const b0 = 20 + jlen;
  if (b0 + 8 <= bytes.length && v.getUint32(b0 + 4, true) === 0x004e4942) bin = bytes.subarray(b0 + 8, b0 + 8 + v.getUint32(b0, true));
  if (!bin) return null;
  const bv = new DataView(bin.buffer, bin.byteOffset, bin.byteLength);
  const read = (ai: number): number[][] | null => {
    const a = doc.accessors?.[ai];
    if (!a || a.bufferView === undefined) return null;
    const view = doc.bufferViews?.[a.bufferView];
    if (!view) return null;
    const n = ({ SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4 } as Record<string, number>)[a.type];
    const cs = ({ 5126: 4, 5125: 4, 5123: 2, 5122: 2, 5121: 1, 5120: 1 } as Record<number, number>)[a.componentType];
    if (!n || !cs) return null;
    const stride = view.byteStride || n * cs, base = (view.byteOffset ?? 0) + (a.byteOffset ?? 0), out: number[][] = [];
    for (let i = 0; i < a.count; i++) {
      const row: number[] = [];
      for (let k = 0; k < n; k++) {
        const o = base + i * stride + k * cs;
        if (o + cs > bin!.byteLength) return null;
        row.push(a.componentType === 5126 ? bv.getFloat32(o, true) : a.componentType === 5125 ? bv.getUint32(o, true) : a.componentType === 5123 ? bv.getUint16(o, true)
          : a.componentType === 5122 ? bv.getInt16(o, true) : a.componentType === 5121 ? bv.getUint8(o) : bv.getInt8(o));
      }
      out.push(row);
    }
    return out;
  };
  const tris: number[] = [];
  const addMesh = (mi: number, m: M4) => {
    for (const p of doc.meshes?.[mi]?.primitives ?? []) {
      if ((p.mode ?? 4) !== 4 || p.attributes?.POSITION === undefined) continue;
      const pos = read(p.attributes.POSITION);
      if (!pos) continue;
      const idx = p.indices !== undefined ? read(p.indices)?.map((r) => r[0]) : pos.map((_, i) => i);
      if (!idx) continue;
      const w = pos.map(([x, y, z]) => [(m[0] * x + m[4] * y + m[8] * z + m[12]) / unit, (m[1] * x + m[5] * y + m[9] * z + m[13]) / unit, (m[2] * x + m[6] * y + m[10] * z + m[14]) / unit]);
      for (let i = 0; i + 2 < idx.length; i += 3) for (let k = 0; k < 3; k++) { const q = w[idx[i + k]]; if (!q) return; tris.push(q[0], q[1], q[2]); }
    }
  };
  const walk = (ni: number, parent: M4, depth: number) => {
    const n = doc.nodes?.[ni];
    if (!n || depth > 32) return;
    const m = mul4(parent, trs(n));
    if (n.mesh !== undefined) addMesh(n.mesh, m);
    for (const c of n.children ?? []) walk(c, m, depth + 1);
  };
  const I: M4 = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
  const roots = doc.scenes?.[doc.scene ?? 0]?.nodes;
  if (roots?.length) for (const r of roots) walk(r, I, 0);
  else (doc.meshes ?? []).forEach((_: unknown, mi: number) => addMesh(mi, I));
  if (!tris.length) return null;
  const f = doc.materials?.[0]?.pbrMetallicRoughness?.baseColorFactor as number[] | undefined;
  const albedo: Rgb = f ? [toSrgb(f[0]), toSrgb(f[1]), toSrgb(f[2])] : [153, 153, 153];
  const min: Vec3 = [Infinity, Infinity, Infinity], max: Vec3 = [-Infinity, -Infinity, -Infinity];
  for (let i = 0; i < tris.length; i += 3) for (let k = 0; k < 3; k++) { min[k] = Math.min(min[k], tris[i + k]); max[k] = Math.max(max[k], tris[i + k]); }
  return { tris: Float32Array.from(tris), albedo, min, max };
}

const cache = new Map<string, Promise<MeshData | null>>();

/** A resource's preview mesh (map units) and albedo from the connected server, or null where it serves none. */
export async function sceneryMesh(s: Session, resource: string): Promise<MeshData | null> {
  const conn = await s.ensure();
  const base = new URL(conn.url), token = base.searchParams.get("k"), key = `mesh_${resource.toLowerCase()}`;
  const ck = `${base.host}|${key}`;
  let p = cache.get(ck);
  if (!p) {
    const url = `http://${base.host}/erg/assets/${encodeURIComponent(key)}.glb${token ? `?k=${encodeURIComponent(token)}` : ""}`;
    p = fetch(url).then(async (r) => (r.ok ? glbMesh(new Uint8Array(await r.arrayBuffer()), WORLD_PER_XAN) : null), () => null);
    cache.set(ck, p);
  }
  return p;
}

export interface PlacedMesh { tris: Float64Array; rgb: Rgb; detail: number; }

/** Each scenery detail's mesh placed in the level (map units) with its albedo: for render_view {scenery: true}. */
export async function placedMeshes(s: Session, frames: Frames, details: Detail[], within?: (d: Detail) => boolean): Promise<{ meshes: PlacedMesh[]; without: string[] }> {
  const meshes: PlacedMesh[] = [], without = new Set<string>();
  for (const d of details) {
    if (within && !within(d)) continue;
    const m = await sceneryMesh(s, d.resource);
    if (!m) { without.add(d.resource); continue; }
    const mat = frames.detailMatrix(d), out = new Float64Array(m.tris.length);
    for (let i = 0; i < m.tris.length; i += 3) {
      const q = applyMat(mat, [m.tris[i], m.tris[i + 1], m.tris[i + 2]]);
      out[i] = q[0]; out[i + 1] = q[1]; out[i + 2] = q[2];
    }
    meshes.push({ tris: out, rgb: tintRgb(d.name) ?? m.albedo, detail: d.id });
  }
  return { meshes, without: [...without] };
}

/** A tinted copy ("_tint<rrggbb>") draws in its tint (what Sunstone shows in the game), else null. */
function tintRgb(name: string): [number, number, number] | null {
  const t = tintOf(name);
  return t ? [1, 3, 5].map((i) => parseInt(t.slice(i, i + 2), 16)) as [number, number, number] : null;
}
