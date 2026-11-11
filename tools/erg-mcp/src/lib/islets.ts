// Detached terrain for check_islets (and check_reachability's islets): the solid voxels of every frame, merged per column
// into stretches on a grid of columns, joined where stretches of side-by-side columns overlap or touch in y, then split
// into connected pieces. The largest piece is the main landmass; every other is reported (offshore chunks, coastal
// speckle, floating debris), smallest first flagged as debris. Pure: grids in, pieces out. Map units.
import { columnSpans } from "./inspect-field";
import type { Grid } from "./voxel";
import type { Region } from "./reach";

export interface Piece {
  volume: number;                 // map units cubed (step x step x height of its stretches)
  columns: number;
  min: [number, number, number]; max: [number, number, number];
  top: number;                    // map y of its highest ground
  frames: number[];               // the frames it is made of, most of it first
}

/** The connected pieces of solid terrain in the region, largest first. */
export function pieces(gs: Grid[], r: Region, step: number): Piece[] {
  const w = Math.max(1, Math.ceil((r.x1 - r.x0) / step)), h = Math.max(1, Math.ceil((r.z1 - r.z0) / step));
  const cols: { top: number; bottom: number; frame: number }[][] = Array.from({ length: w * h }, () => []);
  for (const g of gs) {
    const b = g.bounds;
    if (b.max[0] <= r.x0 || b.min[0] >= r.x1 || b.max[2] <= r.z0 || b.min[2] >= r.z1) continue;
    const i0 = Math.max(0, Math.floor((b.min[0] - r.x0) / step)), i1 = Math.min(w - 1, Math.floor((b.max[0] - r.x0) / step));
    const j0 = Math.max(0, Math.floor((b.min[2] - r.z0) / step)), j1 = Math.min(h - 1, Math.floor((b.max[2] - r.z0) / step));
    for (let j = j0; j <= j1; j++)
      for (let i = i0; i <= i1; i++)
        for (const s of columnSpans(g, r.x0 + (i + 0.5) * step, r.z0 + (j + 0.5) * step)) cols[j * w + i].push({ top: s.top, bottom: s.bottom, frame: g.id });
  }
  // merge each column's stretches (overlapping or touching), highest first
  const start = new Int32Array(w * h + 1), top: number[] = [], bottom: number[] = [], frameOf: Map<number, number>[] = [];
  for (let c = 0; c < w * h; c++) {
    start[c] = top.length;
    for (const s of cols[c].sort((a, b) => b.top - a.top)) {
      const k = top.length - 1;
      if (k >= start[c] && s.top >= bottom[k] - 1e-6) {
        bottom[k] = Math.min(bottom[k], s.bottom);
        frameOf[k].set(s.frame, (frameOf[k].get(s.frame) ?? 0) + s.top - s.bottom);
      } else {
        top.push(s.top);
        bottom.push(s.bottom);
        frameOf.push(new Map([[s.frame, s.top - s.bottom]]));
      }
    }
  }
  start[w * h] = top.length;
  const n = top.length, parent = new Int32Array(n);
  for (let i = 0; i < n; i++) parent[i] = i;
  const find = (i: number): number => { while (parent[i] !== i) { parent[i] = parent[parent[i]]; i = parent[i]; } return i; };
  const union = (a: number, b: number) => { const x = find(a), y = find(b); if (x !== y) parent[Math.max(x, y)] = Math.min(x, y); };
  for (let j = 0; j < h; j++)
    for (let i = 0; i < w; i++) {
      const c = j * w + i;
      for (const nc of [i + 1 < w ? c + 1 : -1, j + 1 < h ? c + w : -1]) {
        if (nc < 0) continue;
        for (let a = start[c]; a < start[c + 1]; a++)
          for (let b = start[nc]; b < start[nc + 1]; b++) if (bottom[a] <= top[b] + 1e-6 && bottom[b] <= top[a] + 1e-6) union(a, b);
      }
    }
  const by = new Map<number, Piece & { frameVol: Map<number, number>; cols: Set<number> }>();
  for (let c = 0; c < w * h; c++)
    for (let k = start[c]; k < start[c + 1]; k++) {
      const root = find(k), x = r.x0 + (c % w) * step, z = r.z0 + Math.floor(c / w) * step;
      let p = by.get(root);
      if (!p) by.set(root, (p = { volume: 0, columns: 0, min: [x, bottom[k], z], max: [x + step, top[k], z + step], top: top[k], frames: [], frameVol: new Map(), cols: new Set() }));
      p.volume += (top[k] - bottom[k]) * step * step;
      p.cols.add(c);
      p.min = [Math.min(p.min[0], x), Math.min(p.min[1], bottom[k]), Math.min(p.min[2], z)];
      p.max = [Math.max(p.max[0], x + step), Math.max(p.max[1], top[k]), Math.max(p.max[2], z + step)];
      p.top = Math.max(p.top, top[k]);
      for (const [f, v] of frameOf[k]) p.frameVol.set(f, (p.frameVol.get(f) ?? 0) + v);
    }
  return [...by.values()].map((p) => ({
    volume: p.volume, columns: p.cols.size, min: p.min, max: p.max, top: p.top,
    frames: [...p.frameVol].sort((a, b) => b[1] - a[1]).map(([f]) => f),
  })).sort((a, b) => b.volume - a.volume);
}
