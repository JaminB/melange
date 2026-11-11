// check_support: whether the ground under a point is solid down to the base, or a skin over a void. check_holes passes a
// thin draped skin over an empty bowl (its rays hit the skin), but a mine factory or a mine landing on it stands on a
// shell: one shot opens it onto the void, and the factory's drop in the game missed such floors. Each column is read
// straight down (a draped skin at its sheared height, rotated frames sampled, as every inspect tool reads the voxels);
// stacked or touching stretches are merged, and the top stretch must reach `downTo` (the tool's default: the water level,
// else the bottom of the lowest voxel frame) for the column to count as solid. Pure: grids in, a report out. Map units.
import { spansAt } from "./inspect-field";
import type { Grid } from "./voxel";

/** One column: the ground's top, how thick the top stretch is, the air under it before the next stretch (or down to
 * `downTo` when there is none) and whether the top stretch reaches `downTo`. */
export interface SupportColumn { x: number; z: number; top: number; thickness: number; voidBelow: number; solid: boolean; }
export interface SupportReport {
  columns: number; solid: number; hollow: number; empty: number; downTo: number;
  thinnest: SupportColumn | null;   // the hollow column with the thinnest top stretch
  deepest: SupportColumn | null;    // the hollow column with the most air under it
  centre: SupportColumn | null;     // the column at the point itself
  hollowColumns: SupportColumn[];   // every hollow column (at most `limit`), thinnest first
}

/** The top stretch of a column (stacked or touching stretches merged) and what is under it, or null with no ground. */
function column(gs: Grid[], x: number, z: number): { top: number; bottom: number; next: number | null } | null {
  const sp = spansAt(gs, x, z);
  if (!sp.length) return null;
  let lo = sp[0].bottom, next: number | null = null;
  for (const s of sp.slice(1)) {
    if (s.top >= lo - 1e-6) lo = Math.min(lo, s.bottom);
    else { next = s.top; break; }
  }
  return { top: sp[0].top, bottom: lo, next };
}

/** The support of the ground within `radius` of (x, z), a column every `step`; `downTo` the base a column must reach
 * (default the bottom of the lowest voxel frame). */
export function checkSupport(gs: Grid[], x: number, z: number, radius: number, step: number, downTo?: number, limit = 20): SupportReport {
  const pts: [number, number][] = [[x, z]];
  for (let dz = -radius; dz <= radius + 1e-9; dz += step)
    for (let dx = -radius; dx <= radius + 1e-9; dx += step)
      if ((dx || dz) && dx * dx + dz * dz <= radius * radius + 1e-9) pts.push([x + dx, z + dz]);
  const cols = pts.map(([px, pz]) => ({ px, pz, c: column(gs, px, pz) }));
  // the base: the bottom of the lowest frame (a skin with nothing at all under it is not its own base)
  let base = downTo;
  if (base === undefined) {
    base = Infinity;
    for (const g of gs) base = Math.min(base, g.bounds.min[1]);
    if (!Number.isFinite(base)) base = 0;
  }
  const out: SupportReport = { columns: cols.length, solid: 0, hollow: 0, empty: 0, downTo: base, thinnest: null, deepest: null, centre: null, hollowColumns: [] };
  const hollow: SupportColumn[] = [];
  cols.forEach(({ px, pz, c }, i) => {
    if (!c) { out.empty++; return; }
    const solid = c.bottom <= base! + 1e-6;
    const col: SupportColumn = { x: px, z: pz, top: c.top, thickness: c.top - c.bottom, voidBelow: solid ? 0 : c.bottom - Math.max(base!, c.next ?? base!), solid };
    if (i === 0) out.centre = col;
    if (solid) { out.solid++; return; }
    out.hollow++;
    hollow.push(col);
  });
  hollow.sort((a, b) => a.thickness - b.thickness || b.voidBelow - a.voidBelow);
  out.thinnest = hollow[0] ?? null;
  out.deepest = hollow.reduce<SupportColumn | null>((m, c) => (!m || c.voidBelow > m.voidBelow ? c : m), null);
  out.hollowColumns = hollow.slice(0, limit);
  return out;
}
