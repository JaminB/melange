// Worm-walk reachability over the voxels, for check_reachability: a graph of the places a worm can stand (the top of
// every stretch of solid ground with room above it, sampled on a grid of columns) joined where a worm can walk, climb or
// drop from one to the next, then a search from each spawn. Pure: grids in, a report out. Map units throughout.
//
// The rules are the walking ones, kept simple and stated in the answer: a step up is taken when it is at most maxStep,
// or no steeper than maxSlope over one voxel of the ground it climbs onto (the game draws a voxel staircase as a slope:
// at voxel size 2.75 a one-voxel riser is a 45 degree ramp a worm walks up); a drop of at most maxDrop; optionally a
// jump over a gap of up to maxGap to ground no higher than maxStep above. Ground at or under the water drowns a worm.
import type { Vec3 } from "../core";
import { columnSpans } from "./inspect-field";
import type { Grid } from "./voxel";

export interface ReachOptions {
  step: number;           // column spacing
  maxStep: number;        // a step up any worm takes
  maxSlope: number;       // degrees: a riser this steep over one voxel of the upper ground is walked
  maxDrop: number;
  maxGap: number;         // 0: no jumps
  headroom: number;       // air a worm needs above the ground
  water: number | null;   // map y; ground at or under it is not used
}

export interface Region { x0: number; z0: number; x1: number; z1: number; }

/** Standable surfaces per column: y (the top), the voxel height of the frame it is the top of. */
export interface Surfaces {
  r: Region; step: number; w: number; h: number;
  start: Int32Array;                     // per column, the first node; start[c + 1] the end
  cell: Int32Array;                      // per node, its column
  y: Float32Array;                       // per node
  voxel: Float32Array;                   // per node: the voxel height of the ground (its frame's y scale)
  nodes: number;
}

/** Merged solid stretches of every column of the region, and the tops a worm fits on. */
export function surfaces(gs: Grid[], r: Region, step: number, headroom: number): Surfaces {
  const w = Math.max(1, Math.ceil((r.x1 - r.x0) / step)), h = Math.max(1, Math.ceil((r.z1 - r.z0) / step));
  const cols: { top: number; bottom: number; voxel: number }[][] = Array.from({ length: w * h }, () => []);
  for (const g of gs) {
    const b = g.bounds;
    if (b.max[0] <= r.x0 || b.min[0] >= r.x1 || b.max[2] <= r.z0 || b.min[2] >= r.z1) continue;
    const vy = Math.hypot(g.toMap[1], g.toMap[5], g.toMap[9]);
    const i0 = Math.max(0, Math.floor((b.min[0] - r.x0) / step)), i1 = Math.min(w - 1, Math.floor((b.max[0] - r.x0) / step));
    const j0 = Math.max(0, Math.floor((b.min[2] - r.z0) / step)), j1 = Math.min(h - 1, Math.floor((b.max[2] - r.z0) / step));
    for (let j = j0; j <= j1; j++)
      for (let i = i0; i <= i1; i++)
        for (const s of columnSpans(g, r.x0 + (i + 0.5) * step, r.z0 + (j + 0.5) * step)) cols[j * w + i].push({ top: s.top, bottom: s.bottom, voxel: vy });
  }
  const start = new Int32Array(w * h + 1), ys: number[] = [], vox: number[] = [], cell: number[] = [];
  for (let c = 0; c < w * h; c++) {
    start[c] = ys.length;
    const sp = cols[c].sort((a, b) => b.top - a.top);
    // union of overlapping or touching stretches, highest first
    const merged: { top: number; bottom: number; voxel: number }[] = [];
    for (const s of sp) {
      const m = merged[merged.length - 1];
      if (m && s.top >= m.bottom - 1e-6) m.bottom = Math.min(m.bottom, s.bottom);
      else merged.push({ ...s });
    }
    for (let k = 0; k < merged.length; k++) {
      if (k > 0 && merged[k - 1].bottom - merged[k].top < headroom) continue;
      ys.push(merged[k].top);
      vox.push(merged[k].voxel);
      cell.push(c);
    }
  }
  start[w * h] = ys.length;
  return { r, step, w, h, start, cell: Int32Array.from(cell), y: Float32Array.from(ys), voxel: Float32Array.from(vox), nodes: ys.length };
}

/** reached nodes, their walking distance from the start and the node before; blocked: per reached node, the smallest
 * climb up to a neighbouring column that was too high (Infinity where none was). */
export interface Search { reached: Uint8Array; dist: Float32Array; from: Int32Array; blocked: Float32Array; }

const DIRS: [number, number][] = [[1, 0], [-1, 0], [0, 1], [0, -1], [1, 1], [1, -1], [-1, 1], [-1, -1]];

export const nodeAt = (S: Surfaces, n: number): Vec3 =>
  [S.r.x0 + ((S.cell[n] % S.w) + 0.5) * S.step, S.y[n], S.r.z0 + (Math.floor(S.cell[n] / S.w) + 0.5) * S.step];

/** The node a worm standing at p stands on: in p's column, the highest surface not above p.y + 1.5 (and not under the water). */
export function standing(S: Surfaces, p: Vec3, o: ReachOptions): number {
  const i = Math.floor((p[0] - S.r.x0) / S.step), j = Math.floor((p[2] - S.r.z0) / S.step);
  if (i < 0 || j < 0 || i >= S.w || j >= S.h) return -1;
  const c = j * S.w + i;
  for (let n = S.start[c]; n < S.start[c + 1]; n++) if (S.y[n] <= p[1] + 1.5) return o.water !== null && S.y[n] <= o.water ? -1 : n;
  return -1;
}

/** Breadth-first over the walk graph from `src` (Dijkstra on horizontal distance; the graph is small). */
export function search(S: Surfaces, src: number, o: ReachOptions): Search {
  const reached = new Uint8Array(S.nodes), dist = new Float32Array(S.nodes).fill(Infinity), from = new Int32Array(S.nodes).fill(-1);
  const blocked = new Float32Array(S.nodes).fill(Infinity);
  const tan = Math.tan((o.maxSlope * Math.PI) / 180);
  const dry = (n: number) => o.water === null || S.y[n] > o.water;
  const ok = (a: number, b: number) => {
    const dy = S.y[b] - S.y[a];
    if (dy < -o.maxDrop) return false;
    return dy <= Math.max(o.maxStep, tan * S.voxel[b]) + 1e-6;
  };
  // a binary heap on dist
  const heap: number[] = [];
  const push = (n: number) => {
    heap.push(n);
    for (let i = heap.length - 1; i > 0;) { const p = (i - 1) >> 1; if (dist[heap[p]] <= dist[heap[i]]) break; [heap[p], heap[i]] = [heap[i], heap[p]]; i = p; }
  };
  const pop = () => {
    const top = heap[0], last = heap.pop()!;
    if (heap.length) {
      heap[0] = last;
      for (let i = 0; ;) {
        const l = 2 * i + 1, r = l + 1;
        let m = i;
        if (l < heap.length && dist[heap[l]] < dist[heap[m]]) m = l;
        if (r < heap.length && dist[heap[r]] < dist[heap[m]]) m = r;
        if (m === i) break;
        [heap[m], heap[i]] = [heap[i], heap[m]];
        i = m;
      }
    }
    return top;
  };
  dist[src] = 0;
  push(src);
  const passable = (a: number, col: number) => {
    for (let b = S.start[col]; b < S.start[col + 1]; b++) if (dry(b) && ok(a, b)) return true;
    return false;
  };
  const reach = (a: number, b: number, d: number) => {
    if (dist[a] + d < dist[b]) { dist[b] = dist[a] + d; from[b] = a; push(b); }
  };
  while (heap.length) {
    const a = pop();
    if (reached[a]) continue;
    reached[a] = 1;
    const c = S.cell[a], ci = c % S.w, cj = Math.floor(c / S.w);
    for (const [dx, dz] of DIRS) {
      const ni = ci + dx, nj = cj + dz;
      if (ni < 0 || nj < 0 || ni >= S.w || nj >= S.h) continue;
      const nc = nj * S.w + ni, d = S.step * Math.hypot(dx, dz);
      let best = -1, blockedClimb = Infinity;
      for (let b = S.start[nc]; b < S.start[nc + 1]; b++) {
        if (!dry(b)) continue;
        if (ok(a, b)) { if (best < 0 || S.y[b] > S.y[best]) best = b; }
        else if (S.y[b] > S.y[a]) blockedClimb = Math.min(blockedClimb, S.y[b] - S.y[a]);
      }
      // a diagonal step needs one of the two columns beside it walkable too: a worm does not slip between two walls
      // that touch only at a corner
      if (best >= 0 && dx && dz && !passable(a, cj * S.w + ci + dx) && !passable(a, (cj + dz) * S.w + ci)) best = -1;
      if (best >= 0) reach(a, best, d);
      else if (blockedClimb < blocked[a]) blocked[a] = blockedClimb;
      // a jump over a gap: along the same direction, past columns with no ground near the take-off height
      if (o.maxGap > 0 && best < 0) {
        for (let k = 2; k * d <= o.maxGap + d + 1e-6; k++) {
          const ji = ci + dx * k, jj = cj + dz * k;
          if (ji < 0 || jj < 0 || ji >= S.w || jj >= S.h) break;
          const jc = jj * S.w + ji;
          let land = -1;
          for (let b = S.start[jc]; b < S.start[jc + 1]; b++) {
            const dy = S.y[b] - S.y[a];
            if (dry(b) && dy <= o.maxStep + 1e-6 && dy >= -o.maxDrop && (land < 0 || S.y[b] > S.y[land])) land = b;
          }
          if (land >= 0) { reach(a, land, d * k); break; }
          // a wall in the way ends the jump
          if (S.start[jc] < S.start[jc + 1] && S.y[S.start[jc]] > S.y[a] + o.maxStep) break;
        }
      }
    }
  }
  return { reached, dist, from, blocked };
}
