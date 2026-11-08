// Worm-walk reachability over the voxels, for check_reachability: a graph of the places a worm can stand (the top of
// every stretch of solid ground with room above it, sampled on a grid of columns) joined where a worm can walk, climb or
// drop from one to the next, then a search from each spawn. Pure: grids in, a report out. Map units throughout.
//
// The rules are the walking ones, kept simple and stated in the answer: a step up is taken when it is at most maxStep,
// or when it is one voxel riser (at most one voxel of the ground it climbs onto) and the stair it belongs to is no
// steeper than maxSlope: the riser over the tread above it, the flat run to the next riser in the direction walked
// (slopeOver "tread", the default: the game draws a voxel staircase as a slope, so terraces are walked at their average
// slope), or over one voxel of the upper ground (slopeOver "riser", the rule before, under which every voxel staircase
// is 45 degrees or more); a drop of at most maxDrop; optionally a jump over a gap of up to maxGap to ground no higher
// than maxStep above. Ground at or under the water drowns a worm; with a flood (FloodRules) the water rises by turn and
// a node is used only while it is dry when the worm gets there.
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
  gapRise?: number;       // the highest landing a gap is crossed to, above the take-off (default maxStep)
  slopeOver?: "tread" | "riser" | "segment" | "skin";   // what a riser's slope is judged over (default tread)
  flood?: FloodRules;     // rising water: a node is walked only while it is dry when the worm reaches it
  allow?: Uint8Array;     // per node: 0 = never walked onto (a route constraint: only the roads, or never the roads)
  slopeCap?: Float32Array;   // per node: the steepest climb onto it, degrees (a material's own maxSlope; >= 90 none)
}

/** A flood for the search: the water (map y) in each game turn, and when the worm walks: `perTurn` map units along the
 * walk in each of its own turns, the first in game turn `firstTurn`, then every `every` turns. A node `d` along the walk
 * is reached in the worm's own turn ceil(d / perTurn) and must be dry in that game turn. */
export interface FloodRules { waterAt(turn: number): number; perTurn: number; firstTurn: number; every: number; }

/** The game turn in which a worm `d` along its walk gets there (its first own turn for d = 0). */
export const turnAt = (f: FloodRules, d: number) => f.firstTurn + Math.max(0, Math.ceil(d / f.perTurn - 1e-9) - 1) * f.every;

/** The longest tread measured, in voxels of the upper ground. */
const TREAD_VOXELS = 8;

export interface Region { x0: number; z0: number; x1: number; z1: number; }

/** Standable surfaces per column: y (the top), the voxel height of the frame it is the top of. */
export interface Surfaces {
  r: Region; step: number; w: number; h: number;
  start: Int32Array;                     // per column, the first node; start[c + 1] the end
  cell: Int32Array;                      // per node, its column
  y: Float32Array;                       // per node
  voxel: Float32Array;                   // per node: the voxel height of the ground (its frame's y scale)
  material: Int16Array;                  // per node: the material of its top voxel
  draped: Uint8Array;                    // per node: 1 when its top is a draped block's (corner offsets: a smooth skin)
  nodes: number;
}

/** Merged solid stretches of every column of the region, and the tops a worm fits on. */
export function surfaces(gs: Grid[], r: Region, step: number, headroom: number): Surfaces {
  const w = Math.max(1, Math.ceil((r.x1 - r.x0) / step)), h = Math.max(1, Math.ceil((r.z1 - r.z0) / step));
  const cols: { top: number; bottom: number; voxel: number; material: number; draped: number }[][] = Array.from({ length: w * h }, () => []);
  for (const g of gs) {
    const b = g.bounds;
    if (b.max[0] <= r.x0 || b.min[0] >= r.x1 || b.max[2] <= r.z0 || b.min[2] >= r.z1) continue;
    const vy = Math.hypot(g.toMap[1], g.toMap[5], g.toMap[9]), draped = g.frame.corners ? 1 : 0;
    const i0 = Math.max(0, Math.floor((b.min[0] - r.x0) / step)), i1 = Math.min(w - 1, Math.floor((b.max[0] - r.x0) / step));
    const j0 = Math.max(0, Math.floor((b.min[2] - r.z0) / step)), j1 = Math.min(h - 1, Math.floor((b.max[2] - r.z0) / step));
    for (let j = j0; j <= j1; j++)
      for (let i = i0; i <= i1; i++)
        for (const s of columnSpans(g, r.x0 + (i + 0.5) * step, r.z0 + (j + 0.5) * step)) cols[j * w + i].push({ top: s.top, bottom: s.bottom, voxel: vy, material: s.material, draped });
  }
  const start = new Int32Array(w * h + 1), ys: number[] = [], vox: number[] = [], cell: number[] = [], mats: number[] = [], drp: number[] = [];
  for (let c = 0; c < w * h; c++) {
    start[c] = ys.length;
    const sp = cols[c].sort((a, b) => b.top - a.top);
    // union of overlapping or touching stretches, highest first
    const merged: { top: number; bottom: number; voxel: number; material: number; draped: number }[] = [];
    for (const s of sp) {
      const m = merged[merged.length - 1];
      if (m && s.top >= m.bottom - 1e-6) m.bottom = Math.min(m.bottom, s.bottom);
      else merged.push({ ...s });
    }
    for (let k = 0; k < merged.length; k++) {
      if (k > 0 && merged[k - 1].bottom - merged[k].top < headroom) continue;
      ys.push(merged[k].top);
      vox.push(merged[k].voxel);
      mats.push(merged[k].material);
      drp.push(merged[k].draped);
      cell.push(c);
    }
  }
  start[w * h] = ys.length;
  return { r, step, w, h, start, cell: Int32Array.from(cell), y: Float32Array.from(ys), voxel: Float32Array.from(vox), material: Int16Array.from(mats), draped: Uint8Array.from(drp), nodes: ys.length };
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

/** The flat run on top of a riser: from b's column on in the direction (dx, dz) while the ground stays within maxStep of
 * the column before (a node there; the first column off it ends the tread), in map units, at most TREAD_VOXELS voxels of
 * b's ground. */
export function treadRun(S: Surfaces, b: number, dx: number, dz: number, maxStep: number): number {
  const d = S.step * Math.hypot(dx, dz), cap = TREAD_VOXELS * S.voxel[b];
  let ci = S.cell[b] % S.w, cj = Math.floor(S.cell[b] / S.w), run = d, y = S.y[b];
  while (run < cap) {
    ci += dx;
    cj += dz;
    if (ci < 0 || cj < 0 || ci >= S.w || cj >= S.h) break;
    const nc = cj * S.w + ci;
    let next = -1;
    for (let n = S.start[nc]; n < S.start[nc + 1] && next < 0; n++) if (Math.abs(S.y[n] - y) <= maxStep + 1e-6) next = n;
    if (next < 0) break;
    y = S.y[next];
    run += d;
  }
  return Math.min(run, cap);
}

/** True when a climb a -> b is judged by its own segment (slopeOver "segment", or "skin" where either end is a draped
 * skin's): the rise over the move's horizontal length, so a skin's 62-78 degree cliff face is a barrier, not a stair. */
const bySegment = (S: Surfaces, a: number, b: number, o: ReachOptions) => o.slopeOver === "segment" || (o.slopeOver === "skin" && (S.draped[a] === 1 || S.draped[b] === 1));

/** The slope (degrees) a climb a -> b in direction (dx, dz) is judged at: its rise over the tread above it, over one
 * voxel of the upper ground with slopeOver "riser", or over the move itself (bySegment); 0 for a step of at most maxStep
 * or a drop. */
export function climbSlope(S: Surfaces, a: number, b: number, dx: number, dz: number, o: ReachOptions): number {
  const dy = S.y[b] - S.y[a];
  if (dy <= o.maxStep + 1e-6) return 0;
  const run = bySegment(S, a, b, o) ? S.step * Math.hypot(dx, dz) : o.slopeOver === "riser" ? S.voxel[b] : treadRun(S, b, dx, dz, o.maxStep);
  return (Math.atan2(dy, run) * 180) / Math.PI;
}

/** A move from a to the next column b, (dx, dz) columns away: a drop of at most maxDrop, a step of at most maxStep, or a
 * riser of at most one voxel of the upper ground whose stair is no steeper than maxSlope (climbSlope). */
export function climbs(S: Surfaces, a: number, b: number, dx: number, dz: number, o: ReachOptions): boolean {
  const dy = S.y[b] - S.y[a];
  if (dy < -o.maxDrop - 1e-6) return false;
  if (o.allow && !o.allow[b]) return false;
  // a material's own steepest climb: any rise onto it, small steps too, steeper than that is refused
  const cap = o.slopeCap ? o.slopeCap[b] : 90;
  if (cap < 90 && dy > 1e-6 && (Math.atan2(dy, S.step * Math.hypot(dx, dz)) * 180) / Math.PI > cap + 1e-6) return false;
  if (dy <= o.maxStep + 1e-6) return true;
  const tan = Math.tan((Math.min(o.maxSlope, cap) * Math.PI) / 180);
  if (bySegment(S, a, b, o)) return dy <= tan * S.step * Math.hypot(dx, dz) + 1e-6;
  if (o.slopeOver === "riser") return dy <= tan * S.voxel[b] + 1e-6;
  return dy <= S.voxel[b] + 1e-6 && dy <= tan * treadRun(S, b, dx, dz, o.maxStep) + 1e-6;
}

/** Breadth-first over the walk graph from `src` (Dijkstra on horizontal distance; the graph is small). With `stopAt`, it
 * ends at the first such node reached (the nearest; the rest of the graph is left unexplored). */
export function search(S: Surfaces, src: number, o: ReachOptions, stopAt?: Uint8Array): Search {
  const reached = new Uint8Array(S.nodes), dist = new Float32Array(S.nodes).fill(Infinity), from = new Int32Array(S.nodes).fill(-1);
  const blocked = new Float32Array(S.nodes).fill(Infinity);
  const rise = o.gapRise ?? o.maxStep, fl = o.flood;
  const dryAt = (n: number, d: number) => (o.water === null || S.y[n] > o.water) && (!fl || S.y[n] > fl.waterAt(turnAt(fl, d))) && (!o.allow || !!o.allow[n]);
  let here = 0;                                     // the walk to the node being expanded
  const dry = (n: number) => dryAt(n, here);
  const ok = (a: number, b: number, dx: number, dz: number) => climbs(S, a, b, dx, dz, o);
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
  const passable = (a: number, col: number, dx: number, dz: number) => {
    for (let b = S.start[col]; b < S.start[col + 1]; b++) if (dry(b) && ok(a, b, dx, dz)) return true;
    return false;
  };
  // with a flood a node counts only while it is dry when the worm gets there (the water only rises, so the shortest
  // walk to a node is also the earliest)
  const reach = (a: number, b: number, d: number) => {
    if (dist[a] + d < dist[b] && (!fl || dryAt(b, dist[a] + d))) { dist[b] = dist[a] + d; from[b] = a; push(b); }
  };
  while (heap.length) {
    const a = pop();
    if (reached[a]) continue;
    reached[a] = 1;
    here = dist[a];
    if (stopAt?.[a]) break;
    const c = S.cell[a], ci = c % S.w, cj = Math.floor(c / S.w);
    for (const [dx, dz] of DIRS) {
      const ni = ci + dx, nj = cj + dz;
      if (ni < 0 || nj < 0 || ni >= S.w || nj >= S.h) continue;
      const nc = nj * S.w + ni, d = S.step * Math.hypot(dx, dz);
      let best = -1, blockedClimb = Infinity;
      for (let b = S.start[nc]; b < S.start[nc + 1]; b++) {
        if (!dry(b)) continue;
        if (ok(a, b, dx, dz)) { if (best < 0 || S.y[b] > S.y[best]) best = b; }
        else if (S.y[b] > S.y[a]) blockedClimb = Math.min(blockedClimb, S.y[b] - S.y[a]);
      }
      // a diagonal step needs one of the two columns beside it walkable too: a worm does not slip between two walls
      // that touch only at a corner
      if (best >= 0 && dx && dz && !passable(a, cj * S.w + ci + dx, dx, 0) && !passable(a, (cj + dz) * S.w + ci, 0, dz)) best = -1;
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
            if (dry(b) && dy <= rise + 1e-6 && dy >= -o.maxDrop && (land < 0 || S.y[b] > S.y[land])) land = b;
          }
          if (land >= 0) { reach(a, land, d * k); break; }
          // a wall in the way ends the jump
          if (S.start[jc] < S.start[jc + 1] && S.y[S.start[jc]] > S.y[a] + rise) break;
        }
      }
    }
  }
  return { reached, dist, from, blocked };
}

/** The nodes from the search's start to n, in order. */
export function pathTo(q: Search, n: number): number[] {
  const path: number[] = [];
  for (let k = n; k >= 0; k = q.from[k]) path.push(k);
  return path.reverse();
}

/** What a path asks of a worm: its length along the ground, the steepest walked segment (degrees), the highest step up
 * and the deepest drop, and the jumps (moves past the next column). */
export interface PathStats { length: number; maxSlope: number; maxStepUp: number; maxDrop: number; jumps: number; maxStairSlope?: number; }

const across = (S: Surfaces, a: number, b: number) => {
  const p = nodeAt(S, a), q = nodeAt(S, b);
  return Math.hypot(q[0] - p[0], q[2] - p[2]);
};
const isJump = (S: Surfaces, h: number) => h > S.step * Math.SQRT2 + 1e-6;
/** The column step from a's column toward b's (each of x and z -1, 0 or 1). */
const dirOf = (S: Surfaces, a: number, b: number): [number, number] => {
  const ca = S.cell[a], cb = S.cell[b];
  return [Math.sign((cb % S.w) - (ca % S.w)), Math.sign(Math.floor(cb / S.w) - Math.floor(ca / S.w))];
};

/** With `o`, also maxStairSlope: the steepest climb as the rules judge it (a riser over its tread, climbSlope). */
export function pathStats(S: Surfaces, path: number[], o?: ReachOptions): PathStats {
  const st: PathStats = { length: 0, maxSlope: 0, maxStepUp: 0, maxDrop: 0, jumps: 0, ...(o ? { maxStairSlope: 0 } : {}) };
  for (let k = 1; k < path.length; k++) {
    const h = across(S, path[k - 1], path[k]), dy = S.y[path[k]] - S.y[path[k - 1]];
    st.length += Math.hypot(h, dy);
    st.maxStepUp = Math.max(st.maxStepUp, dy);
    st.maxDrop = Math.max(st.maxDrop, -dy);
    if (isJump(S, h)) st.jumps++;
    else {
      st.maxSlope = Math.max(st.maxSlope, (Math.atan2(Math.abs(dy), h) * 180) / Math.PI);
      if (o) { const [dx, dz] = dirOf(S, path[k - 1], path[k]); st.maxStairSlope = Math.max(st.maxStairSlope!, climbSlope(S, path[k - 1], path[k], dx, dz, o)); }
    }
  }
  return st;
}

/** True when the move a -> b is one the walking rules of o take (a step, climb or drop to the next column, or a jump). */
export function walks(S: Surfaces, a: number, b: number, o: ReachOptions): boolean {
  const h = across(S, a, b), dy = S.y[b] - S.y[a];
  if (dy < -o.maxDrop - 1e-6) return false;
  if (o.allow && !o.allow[b]) return false;
  if (!isJump(S, h)) { const [dx, dz] = dirOf(S, a, b); return climbs(S, a, b, dx, dz, o); }
  return h <= o.maxGap + S.step * Math.SQRT2 + 1e-6 && dy <= (o.gapRise ?? o.maxStep) + 1e-6;
}

/** The reach of each utility, map units: a heuristic, stated in check_reachability's answer. */
export const UTILITY_REACH = { jump: 3, girder: 8, rope: 40, jetpackUp: 60, jetpackAcross: 30 };
export type Utility = "jump" | "girder" | "rope" | "jetpack";

/** The walking rules widened to what each utility adds, cheapest first:
 *   jump    gaps up to UTILITY_REACH.jump to ground no higher than maxStep;
 *   girder  a girder of that length: gaps up to it, and steps or a gap's far side up to half of it higher;
 *   rope    a climb of up to UTILITY_REACH.rope up a wall next to the worm (the wall is the anchor), girder gaps too;
 *   jetpack up to jetpackUp higher and jetpackAcross across, any drop. */
export function relaxed(o: ReachOptions, u = UTILITY_REACH): [Utility, ReachOptions][] {
  const half = u.girder / 2;
  return [
    ["jump", { ...o, maxGap: Math.max(o.maxGap, u.jump) }],
    ["girder", { ...o, maxGap: Math.max(o.maxGap, u.girder), maxStep: Math.max(o.maxStep, half), gapRise: Math.max(o.gapRise ?? o.maxStep, half) }],
    ["rope", { ...o, maxGap: Math.max(o.maxGap, u.girder), maxStep: Math.max(o.maxStep, u.rope), gapRise: Math.max(o.gapRise ?? o.maxStep, half) }],
    ["jetpack", { ...o, maxGap: Math.max(o.maxGap, u.jetpackAcross), maxStep: Math.max(o.maxStep, u.jetpackUp), gapRise: Math.max(o.gapRise ?? o.maxStep, u.jetpackUp), maxDrop: Infinity }],
  ];
}

/** For a start the walking rules do not get to a goal: the cheapest utility whose widened rules do (null: none), the route
 * then taken and where the utility is used: its first move the next cheaper rules refuse (walking's for a jump). */
export function classify(S: Surfaces, src: number, goal: Uint8Array, o: ReachOptions, u = UTILITY_REACH):
  { utility: Utility | null; path: number[]; at: [number, number] | null } {
  let prev = o;
  for (const [utility, ro] of relaxed(o, u)) {
    const cheaper = prev;
    prev = ro;
    const q = search(S, src, ro, goal);
    let hit = -1;
    for (let n = 0; n < S.nodes; n++) if (q.reached[n] && goal[n] && (hit < 0 || q.dist[n] < q.dist[hit])) hit = n;
    if (hit < 0) continue;
    const path = pathTo(q, hit);
    let at: [number, number] | null = null;
    for (let k = 1; k < path.length && !at; k++) if (!walks(S, path[k - 1], path[k], cheaper)) at = [path[k - 1], path[k]];
    for (let k = 1; k < path.length && !at; k++) if (!walks(S, path[k - 1], path[k], o)) at = [path[k - 1], path[k]];
    return { utility, path, at };
  }
  return { utility: null, path: [], at: null };
}
