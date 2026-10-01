// Voxel words and frame geometry for the terrain tools. The word rules are the server's (src/erg/voxels.h): bits 0-1
// solid (3) or empty (0), 2-7 material, 8-15 second material, 16-23 blend, 24-31 clear; index (z*X + x)*Y + y.
// Voxel (x, y, z) fills [x, x+1) x [y, y+1) x [z, z+1) of its frame's space (.xan units before the frame transform).
import { frameWorld, validRunValue, type Frame, type Mat3x4, type Scene, type Vec3 } from "../../../sdk/erg";

export const SOLID = 3;
export const MATERIALS = 64;
const MATERIAL_MASK = 0xfc, KEPT_MASK = 0x00ffff00;

export const isSolid = (v: number) => (v & 3) === SOLID;
export const material = (v: number) => (v >>> 2) & 63;
export const filled = (m: number) => (SOLID | ((m & 63) << 2)) >>> 0;
export const carved = (v: number) => (v & ~3) >>> 0;
export const painted = (v: number, m: number) => (isSolid(v) ? ((v & ~MATERIAL_MASK) | ((m & 63) << 2)) >>> 0 : v);

/** A word the server accepts in place of `base`: bits 8-23 are the base's or zero (any mix of carve, fill, paint). */
export function validEdit(base: number, now: number): boolean {
  if (!validRunValue(now)) return false;
  const kept = now & KEPT_MASK;
  return kept === 0 || kept === (base & KEPT_MASK);
}

export const cells = (f: Pick<Frame, "size">) => f.size[0] * f.size[1] * f.size[2];
export const index = (f: Pick<Frame, "size">, x: number, y: number, z: number) => (z * f.size[0] + x) * f.size[1] + y;

export function invert(m: Mat3x4): Mat3x4 | null {
  const [a, b, c, tx, d, e, f, ty, g, h, i, tz] = m;
  const A = e * i - f * h, B = f * g - d * i, C = d * h - e * g;
  const det = a * A + b * B + c * C;
  if (!Number.isFinite(det) || Math.abs(det) < 1e-12) return null;
  const r = 1 / det;
  const inv = [
    A * r, (c * h - b * i) * r, (b * f - c * e) * r,
    B * r, (a * i - c * g) * r, (c * d - a * f) * r,
    C * r, (b * g - a * h) * r, (a * e - b * d) * r,
  ];
  const t = [tx, ty, tz];
  const out = new Array(12).fill(0) as Mat3x4;
  for (let row = 0; row < 3; row++) {
    for (let k = 0; k < 3; k++) out[row * 4 + k] = inv[row * 3 + k];
    out[row * 4 + 3] = -(inv[row * 3] * t[0] + inv[row * 3 + 1] * t[1] + inv[row * 3 + 2] * t[2]);
  }
  return out;
}

export const applyDir = (m: Mat3x4, v: Vec3): Vec3 => [0, 1, 2].map((r) => m[r * 4] * v[0] + m[r * 4 + 1] * v[1] + m[r * 4 + 2] * v[2]) as Vec3;

export interface Box { min: Vec3; max: Vec3; }

/** A voxel frame with its matrices and world bounds, cached for the tools (frames never move while sculpting). */
export interface GridFrame {
  frame: Frame;
  ref: number;
  toWorld: Mat3x4;
  toLocal: Mat3x4;
  bounds: Box;
}

export function transformBox(m: Mat3x4, b: Box): Box {
  const min: Vec3 = [Infinity, Infinity, Infinity], max: Vec3 = [-Infinity, -Infinity, -Infinity];
  for (let k = 0; k < 8; k++) {
    const p: Vec3 = [k & 1 ? b.max[0] : b.min[0], k & 2 ? b.max[1] : b.min[1], k & 4 ? b.max[2] : b.min[2]];
    const q = [0, 1, 2].map((r) => m[r * 4] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3]);
    for (let a = 0; a < 3; a++) {
      min[a] = Math.min(min[a], q[a]);
      max[a] = Math.max(max[a], q[a]);
    }
  }
  return { min, max };
}

export const overlaps = (a: Box, b: Box) =>
  a.min[0] <= b.max[0] && b.min[0] <= a.max[0] && a.min[1] <= b.max[1] && b.min[1] <= a.max[1] && a.min[2] <= b.max[2] && b.min[2] <= a.max[2];

/** Every frame with a voxel blob of the right size, with invertible matrices. `refOf` gives a frame's key in `voxels`. */
export function gridFrames(scene: Scene, voxels: Map<number, Uint32Array>, refOf: (id: number) => number | null): GridFrame[] {
  const byId = new Map(scene.frames.map((f) => [f.id, f]));
  const out: GridFrame[] = [];
  for (const f of scene.frames) {
    const ref = refOf(f.id);
    if (ref === null || !cells(f)) continue;
    const words = voxels.get(ref);
    if (!words || words.length !== cells(f)) continue;
    const toWorld = frameWorld(byId, f.id);
    const toLocal = toWorld && invert(toWorld);
    if (!toWorld || !toLocal) continue;
    out.push({ frame: f, ref, toWorld, toLocal, bounds: transformBox(toWorld, { min: [0, 0, 0], max: [f.size[0], f.size[1], f.size[2]] }) });
  }
  return out;
}
