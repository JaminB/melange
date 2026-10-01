// Rays against the voxels themselves (no mesh needed): "drop to ground" and picking a place on the terrain. Grids are
// centred on their frames (Frames.gridOf).
import type { Scene, Vec3 } from "../../../sdk/erg";
import { isSolid } from "../terrain/mesher";
import type { Frames } from "./geometry";

/** The first solid voxel along p + t*d (t > 0) in one frame's grid, as t; local ray, voxels [x,x+1] etc. */
export function rayVoxels(size: readonly number[], voxels: Uint32Array, o: Vec3, d: Vec3): number | null {
  const [X, Y, Z] = size;
  let t0 = 0, t1 = Infinity;
  const hi = [X, Y, Z];
  for (let a = 0; a < 3; a++) {
    if (Math.abs(d[a]) < 1e-12) {
      if (o[a] < 0 || o[a] > hi[a]) return null;
      continue;
    }
    let ta = (0 - o[a]) / d[a], tb = (hi[a] - o[a]) / d[a];
    if (ta > tb) [ta, tb] = [tb, ta];
    t0 = Math.max(t0, ta);
    t1 = Math.min(t1, tb);
    if (t0 > t1) return null;
  }
  const eps = 1e-9;
  const cell = [0, 1, 2].map((a) => Math.min(hi[a] - 1, Math.max(0, Math.floor(o[a] + d[a] * (t0 + eps)))));
  const step = [0, 1, 2].map((a) => (d[a] > 0 ? 1 : d[a] < 0 ? -1 : 0));
  const next = [0, 1, 2].map((a) => (step[a] === 0 ? Infinity : ((cell[a] + (step[a] > 0 ? 1 : 0)) - o[a]) / d[a]));
  const delta = [0, 1, 2].map((a) => (step[a] === 0 ? Infinity : Math.abs(1 / d[a])));
  let t = t0;
  for (let guard = 0; guard < X + Y + Z + 3; guard++) {
    if (isSolid(voxels[(cell[2] * X + cell[0]) * Y + cell[1]])) return t;
    const a = next[0] < next[1] ? (next[0] < next[2] ? 0 : 2) : next[1] < next[2] ? 1 : 2;
    t = next[a];
    if (t > t1) return null;
    cell[a] += step[a];
    if (cell[a] < 0 || cell[a] >= hi[a]) return null;
    next[a] += delta[a];
  }
  return null;
}

/** The nearest terrain hit along a level ray (t in units of |d|), or null. */
export function rayTerrain(scene: Scene, frames: Frames, voxelsOf: (id: number) => Uint32Array | undefined, p: Vec3, d: Vec3): number | null {
  let best: number | null = null;
  for (const f of scene.frames) {
    if (f.voxels === null || f.parent === null || !f.size[0] || !f.size[1] || !f.size[2]) continue;
    const inv = frames.gridInverseOf(f.id);
    const v = voxelsOf(f.id);
    if (!inv || !v) continue;
    const o: Vec3 = [
      inv[0] * p[0] + inv[1] * p[1] + inv[2] * p[2] + inv[3],
      inv[4] * p[0] + inv[5] * p[1] + inv[6] * p[2] + inv[7],
      inv[8] * p[0] + inv[9] * p[1] + inv[10] * p[2] + inv[11],
    ];
    const ld: Vec3 = [
      inv[0] * d[0] + inv[1] * d[1] + inv[2] * d[2],
      inv[4] * d[0] + inv[5] * d[1] + inv[6] * d[2],
      inv[8] * d[0] + inv[9] * d[1] + inv[10] * d[2],
    ];
    const t = rayVoxels(f.size, v, o, ld);
    if (t !== null && (best === null || t < best)) best = t;
  }
  return best;
}

/** Where a point lands when dropped straight down onto the terrain, or null over empty space. */
export function dropPoint(scene: Scene, frames: Frames, voxelsOf: (id: number) => Uint32Array | undefined, p: Vec3): Vec3 | null {
  const top: Vec3 = [p[0], p[1] + 0.01, p[2]];
  const t = rayTerrain(scene, frames, voxelsOf, top, [0, -1, 0]);
  return t === null ? null : [p[0], top[1] - t, p[2]];
}
