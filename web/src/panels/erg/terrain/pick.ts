// The solid voxel under the cursor: a ray in .xan units against every voxel frame (slab test, then a grid walk in
// frame space). t is along the caller's direction, so hits in different frames compare directly.
import { apply, type Vec3 } from "../../../sdk/erg";
import { applyDir, isSolid, index, type Box, type GridFrame } from "./voxel";

export interface Hit {
  grid: GridFrame;
  cell: Vec3;         // voxel x, y, z in the frame
  normal: Vec3;       // the face the ray entered through, in frame space (0,0,0 when the ray starts inside)
  t: number;
  point: Vec3;        // world (.xan units)
}

function slab(o: Vec3, d: Vec3, b: Box): [number, number, number] | null {
  let t0 = -Infinity, t1 = Infinity, axis = -1;
  for (let a = 0; a < 3; a++) {
    if (Math.abs(d[a]) < 1e-15) {
      if (o[a] < b.min[a] || o[a] > b.max[a]) return null;
      continue;
    }
    let n = (b.min[a] - o[a]) / d[a], f = (b.max[a] - o[a]) / d[a];
    if (n > f) [n, f] = [f, n];
    if (n > t0) { t0 = n; axis = a; }
    t1 = Math.min(t1, f);
    if (t0 > t1) return null;
  }
  return [t0, t1, axis];
}

function walk(g: GridFrame, words: Uint32Array, origin: Vec3, dir: Vec3, best: number): Omit<Hit, "grid" | "point"> | null {
  const f = g.frame, size = f.size;
  const o = apply(g.toLocal, origin), d = applyDir(g.toLocal, dir);
  const s = slab(o, d, { min: [0, 0, 0], max: [size[0], size[1], size[2]] });
  if (!s || s[1] < 0 || s[0] >= best) return null;
  let t = Math.max(s[0], 0);
  const normal: Vec3 = [0, 0, 0];
  if (s[0] > 0 && s[2] >= 0) normal[s[2]] = d[s[2]] > 0 ? -1 : 1;
  const cell = [0, 1, 2].map((a) => Math.min(size[a] - 1, Math.max(0, Math.floor(o[a] + d[a] * t)))) as Vec3;
  const step = d.map((v) => (v > 0 ? 1 : v < 0 ? -1 : 0));
  const tMax = [0, 1, 2].map((a) => (step[a] ? (cell[a] + (step[a] > 0 ? 1 : 0) - o[a]) / d[a] : Infinity));
  const tDelta = d.map((v) => (v ? Math.abs(1 / v) : Infinity));
  for (let n = size[0] + size[1] + size[2] + 3; n > 0 && t < best; n--) {
    if (isSolid(words[index(f, cell[0], cell[1], cell[2])])) return { cell: [...cell] as Vec3, normal: [...normal] as Vec3, t };
    const a = tMax[0] < tMax[1] ? (tMax[0] < tMax[2] ? 0 : 2) : tMax[1] < tMax[2] ? 1 : 2;
    cell[a] += step[a];
    if (cell[a] < 0 || cell[a] >= size[a]) return null;
    t = tMax[a];
    tMax[a] += tDelta[a];
    normal[0] = normal[1] = normal[2] = 0;
    normal[a] = -step[a];
  }
  return null;
}

export function pick(grids: GridFrame[], voxels: Map<number, Uint32Array>, origin: Vec3, dir: Vec3, maxT = Infinity): Hit | null {
  let best: Hit | null = null;
  let bestT = maxT;
  for (const g of grids) {
    const s = slab(origin, dir, g.bounds);
    if (!s || s[1] < 0 || s[0] >= bestT) continue;
    const words = voxels.get(g.ref);
    if (!words) continue;
    const h = walk(g, words, origin, dir, bestT);
    if (!h) continue;
    bestT = h.t;
    best = { ...h, grid: g, point: [0, 1, 2].map((a) => origin[a] + dir[a] * h.t) as Vec3 };
  }
  return best;
}
