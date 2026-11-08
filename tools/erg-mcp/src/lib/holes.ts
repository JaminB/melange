// check_holes: see-through holes in the ground of a region. A ray is cast straight down every `step` over the region (a
// draped skin read at its sheared height, rotated frames sampled, as every inspect tool reads the voxels): a column whose
// ray hits no solid voxel over the sea (`above`, default the project's water level, else 0) is a hole, where the
// camera sees through the island to the water or the void under it (the crater floor round a plug cut out of a
// skin, a gap between a draped skin and an undraped block). Neighbouring holes are grouped. A seam is two
// side-by-side columns whose top stretches do not overlap in height, the higher with nothing under it: ground there,
// but a crack seen through from the side (a thin skin stepping more than its thickness). Pure: grids in, a report out.
// Map units throughout.
import { spansAt, type Region } from "./inspect-field";
import type { Grid } from "./voxel";

/** A group of side-by-side hole columns: their count and area, centre and box (map x, z), and what the hole looks onto. */
export interface HoleCluster { cells: number; area: number; centre: [number, number]; min: [number, number]; max: [number, number]; through: "low" | "void"; }
export interface HoleReport {
  columns: number; holes: number; fraction: number; clusters: HoleCluster[]; seams: { at: [number, number]; gap: number; tops: [number, number] }[];
  seamCount: number; above: number;
}

/** Holes and seams over the region, a column every `step`; `mask` (optional, map x, z) limits the columns looked at
 * (a circle round a point). */
export function findHoles(gs: Grid[], r: Region, step: number, above: number, mask?: (x: number, z: number) => boolean): HoleReport {
  const w = Math.max(1, Math.ceil((r.x1 - r.x0) / step)), h = Math.max(1, Math.ceil((r.z1 - r.z0) / step));
  // per column: 0 not looked at, 1 ground, 2 hole; and the top stretch's top and bottom
  const kind = new Uint8Array(w * h), top = new Float32Array(w * h).fill(NaN), bottom = new Float32Array(w * h).fill(NaN);
  const alone = new Uint8Array(w * h);   // 1: nothing solid over `above` under the top stretch (a crack there is seen through)
  let columns = 0, holes = 0;
  for (let j = 0; j < h; j++)
    for (let i = 0; i < w; i++) {
      const x = r.x0 + (i + 0.5) * step, z = r.z0 + (j + 0.5) * step;
      if (mask && !mask(x, z)) continue;
      columns++;
      const sp = spansAt(gs, x, z).filter((s) => s.top > above + 1e-6);
      if (!sp.length) { kind[j * w + i] = 2; holes++; continue; }
      kind[j * w + i] = 1;
      top[j * w + i] = sp[0].top;
      // the top stretch: stacked or touching stretches merged, as the renders show them
      let lo = sp[0].bottom;
      for (const s of sp.slice(1)) if (s.top >= lo - 1e-6) lo = Math.min(lo, s.bottom);
      bottom[j * w + i] = lo;
      alone[j * w + i] = sp.some((s) => s.top < lo - 1e-6) ? 0 : 1;
    }
  // holes grouped by side-by-side columns
  const seen = new Uint8Array(w * h), clusters: HoleCluster[] = [];
  for (let c = 0; c < w * h; c++) {
    if (kind[c] !== 2 || seen[c]) continue;
    const stack = [c];
    seen[c] = 1;
    let n = 0, sx = 0, sz = 0, i0 = Infinity, j0 = Infinity, i1 = -Infinity, j1 = -Infinity;
    while (stack.length) {
      const k = stack.pop()!, i = k % w, j = Math.floor(k / w);
      n++; sx += i; sz += j;
      i0 = Math.min(i0, i); i1 = Math.max(i1, i); j0 = Math.min(j0, j); j1 = Math.max(j1, j);
      for (const [di, dj] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
        const ni = i + di, nj = j + dj;
        if (ni < 0 || nj < 0 || ni >= w || nj >= h) continue;
        const nk = nj * w + ni;
        if (kind[nk] === 2 && !seen[nk]) { seen[nk] = 1; stack.push(nk); }
      }
    }
    const cx = r.x0 + (sx / n + 0.5) * step, cz = r.z0 + (sz / n + 0.5) * step;
    clusters.push({ cells: n, area: n * step * step, centre: [cx, cz], min: [r.x0 + i0 * step, r.z0 + j0 * step], max: [r.x0 + (i1 + 1) * step, r.z0 + (j1 + 1) * step],
      // low: ground under `above` there (the sea's floor seen through); void: no voxel at all under it
      through: spansAt(gs, cx, cz).length ? "low" : "void" });
  }
  clusters.sort((a, b) => b.cells - a.cells);
  // seams: neighbours (along x and z) whose top stretches do not overlap in height
  const seams: HoleReport["seams"] = [];
  let seamCount = 0;
  for (let j = 0; j < h; j++)
    for (let i = 0; i < w; i++) {
      const a = j * w + i;
      if (kind[a] !== 1) continue;
      for (const [di, dj] of [[1, 0], [0, 1]]) {
        const ni = i + di, nj = j + dj;
        if (ni >= w || nj >= h) continue;
        const b = nj * w + ni;
        if (kind[b] !== 1) continue;
        const gap = Math.max(bottom[a] - top[b], bottom[b] - top[a]);
        // the higher one must have nothing under it: a block or a scrap over other ground is an overhang, not a crack
        if (gap <= 1e-3 || !alone[bottom[a] - top[b] > 0 ? a : b]) continue;
        seamCount++;
        if (seams.length < 40) seams.push({ at: [r.x0 + (i + 0.5 + di / 2) * step, r.z0 + (j + 0.5 + dj / 2) * step], gap, tops: [top[a], top[b]] });
      }
    }
  return { columns, holes, fraction: columns ? holes / columns : 0, clusters, seams, seamCount, above };
}
