// Closed basins of a height grid, for heightfield fields (fillBasins) and check_pits: the level each cell would fill to
// before it spills (priority flood: Barnes, Lehman and Mulla 2014), and the basins as connected groups of cells deeper
// than a threshold under that level. Pure: grids in, grids out, in whatever unit the heights are in.
//
// A cell is ground (0), an outlet (1: what falls there leaves: the grid's edge, the sea, a column with no ground) or a
// wall (2: a structure fills it, nothing passes). Cells join over all eight neighbours, as a worm walks.

export const GROUND = 0, OUTLET = 1, WALL = 2;

/** The spill level of every ground cell: the lowest height a path from it to an outlet must climb to (its own height
 * where it drains freely). Outlets answer their own height (or -Infinity for NaN), walls Infinity. A ground cell no path
 * joins to an outlet (walled in) answers Infinity. */
export function spillLevels(h: ArrayLike<number>, w: number, d: number, kind: ArrayLike<number>): Float32Array {
  const n = w * d, spill = new Float32Array(n).fill(Infinity), seen = new Uint8Array(n);
  // a binary heap of cells on spill
  const heap: number[] = [];
  const push = (c: number) => {
    heap.push(c);
    for (let i = heap.length - 1; i > 0;) { const p = (i - 1) >> 1; if (spill[heap[p]] <= spill[heap[i]]) break; [heap[p], heap[i]] = [heap[i], heap[p]]; i = p; }
  };
  const pop = () => {
    const top = heap[0], last = heap.pop()!;
    if (heap.length) {
      heap[0] = last;
      for (let i = 0; ;) {
        const l = 2 * i + 1, r = l + 1;
        let m = i;
        if (l < heap.length && spill[heap[l]] < spill[heap[m]]) m = l;
        if (r < heap.length && spill[heap[r]] < spill[heap[m]]) m = r;
        if (m === i) break;
        [heap[m], heap[i]] = [heap[i], heap[m]];
        i = m;
      }
    }
    return top;
  };
  for (let c = 0; c < n; c++) {
    if (kind[c] !== OUTLET) continue;
    spill[c] = Number.isFinite(h[c]) ? h[c] : -Infinity;
    seen[c] = 1;
    push(c);
  }
  while (heap.length) {
    const c = pop(), x = c % w, z = (c - x) / w;
    for (let dz = -1; dz <= 1; dz++)
      for (let dx = -1; dx <= 1; dx++) {
        const x2 = x + dx, z2 = z + dz;
        if ((!dx && !dz) || x2 < 0 || z2 < 0 || x2 >= w || z2 >= d) continue;
        const c2 = z2 * w + x2;
        if (seen[c2] || kind[c2] === WALL) continue;
        seen[c2] = 1;
        spill[c2] = Math.max(h[c2], spill[c]);
        push(c2);
      }
  }
  return spill;
}

export interface Basin {
  cells: number;
  deepest: number;                 // the most a cell lies under the spill level
  at: [number, number];            // the deepest cell (column, row)
  floor: number;                   // its height
  spill: number;                   // the level the basin fills to
  min: [number, number]; max: [number, number];   // its box in cells
  walledIn: boolean;               // no path to an outlet at all (spill Infinity)
}

/** The basins: connected groups (eight neighbours) of ground cells lying more than `minDepth` under their spill level,
 * largest first. */
export function basins(h: ArrayLike<number>, w: number, d: number, kind: ArrayLike<number>, spill: ArrayLike<number>, minDepth: number): Basin[] {
  const n = w * d, mark = new Uint8Array(n), out: Basin[] = [];
  const deep = (c: number) => kind[c] === GROUND && spill[c] - h[c] > minDepth + 1e-6;
  for (let s = 0; s < n; s++) {
    if (mark[s] || !deep(s)) continue;
    const b: Basin = { cells: 0, deepest: 0, at: [s % w, Math.floor(s / w)], floor: h[s], spill: spill[s], min: [Infinity, Infinity], max: [-Infinity, -Infinity], walledIn: spill[s] === Infinity };
    const stack = [s];
    mark[s] = 1;
    while (stack.length) {
      const c = stack.pop()!, x = c % w, z = (c - x) / w, depth = spill[c] === Infinity ? Infinity : spill[c] - h[c];
      b.cells++;
      if (depth > b.deepest || (b.cells === 1)) { b.deepest = depth; b.at = [x, z]; b.floor = h[c]; b.spill = spill[c]; }
      b.min = [Math.min(b.min[0], x), Math.min(b.min[1], z)];
      b.max = [Math.max(b.max[0], x), Math.max(b.max[1], z)];
      for (let dz = -1; dz <= 1; dz++)
        for (let dx = -1; dx <= 1; dx++) {
          const x2 = x + dx, z2 = z + dz;
          if (x2 < 0 || z2 < 0 || x2 >= w || z2 >= d) continue;
          const c2 = z2 * w + x2;
          if (!mark[c2] && deep(c2)) { mark[c2] = 1; stack.push(c2); }
        }
    }
    out.push(b);
  }
  return out.sort((a, b) => b.cells - a.cells);
}

/** Raises every ground cell lying more than `maxDepth` under its spill level to spill - maxDepth, in place: no closed
 * depression deeper than maxDepth is left (0: none at all, each basin a flat floor at its spill level). A walled-in
 * cell (no outlet) is left as it is. Answers the cells raised, the most one was raised by and the volume added. */
export function fillBasins(h: Float32Array, w: number, d: number, kind: ArrayLike<number>, maxDepth: number): { raised: number; most: number; volume: number; spill: Float32Array } {
  const spill = spillLevels(h, w, d, kind);
  let raised = 0, most = 0, volume = 0;
  for (let c = 0; c < w * d; c++) {
    if (kind[c] !== GROUND || spill[c] === Infinity) continue;
    const to = spill[c] - maxDepth;
    if (to > h[c] + 1e-6) { raised++; most = Math.max(most, to - h[c]); volume += to - h[c]; h[c] = to; }
  }
  return { raised, most, volume, spill };
}
