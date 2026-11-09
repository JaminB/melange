// check_trail: the ground across a walking trail. The centreline is sampled every `spacing` (map units); at each sample
// the trail's ground (the top at or under the trail's own height, so a switchback's leg is not confused with the leg
// over it) is compared with the ground across the bed and a margin either side: a column more than `maxDrop` under the
// trail's ground (or with no ground at all) is an edge a worm can step or be knocked off, and a column within
// `holeRadius` of the centreline whose ray straight down meets no solid over the water (`above`) is a hole the camera
// and a worm see through. check_reachability's walk graph keeps to the ground it can stand on, so it passes a trail with
// a crumbling edge or a pit in its bed; this reads every column. It also lists the corners where four draped skin blocks
// meet near the centreline (`skinCorners`): in the Caldera v2 Test (2026-11-09) mines and worms fell through the skin
// within about 0.4 map units of each corner of the crater's skin block, which the voxels here show as solid (a test
// island's corners held every mine), so such a corner is a place to probe with game_mine_drop {trails}. Pure: grids in,
// a report out; map units.
import { spansAt } from "./inspect-field";
import type { Grid } from "./voxel";

export interface TrailCheckOptions {
  halfWidth: number;    // half the bed's width
  margin: number;       // looked at past each edge of the bed
  spacing: number;      // along and across
  maxDrop: number;      // a lateral drop larger than this is reported
  holeRadius: number;   // see-through holes within this of the centreline are reported
  above: number;        // a ray must meet solid over this (the water level)
  headroom: number;     // the ground at a column is the top at or under the trail's ground + this
}

export interface TrailRun { kind: "drop" | "hole" | "noGround"; side: "left" | "right" | "centre"; from: number; to: number; at: [number, number]; worst?: number; trailY?: number | null; }
export interface TrailReport { length: number; samples: number; columns: number; runs: TrailRun[]; worstDrop: number | null; skinCorners: { at: [number, number]; along: number; off: number; blocks: number }[]; }

/** The trail's points densified every `spacing` along the horizontal: [x, z, y (NaN without), along]. */
export function densify(pts: number[][], spacing: number): number[][] {
  const out: number[][] = [];
  let along = 0;
  for (let i = 0; i + 1 < pts.length; i++) {
    const A = pts[i], B = pts[i + 1], ax = A[0], az = A[A.length - 1], bx = B[0], bz = B[B.length - 1];
    const ya = A.length === 3 ? A[1] : NaN, yb = B.length === 3 ? B[1] : NaN, L = Math.hypot(bx - ax, bz - az);
    const n = Math.max(1, Math.ceil(L / spacing));
    for (let k = 0; k < n; k++) { const t = k / n; out.push([ax + (bx - ax) * t, az + (bz - az) * t, ya + (yb - ya) * t, along + L * t]); }
    along += L;
  }
  const E = pts[pts.length - 1];
  out.push([E[0], E[E.length - 1], E.length === 3 ? E[1] : NaN, along]);
  return out;
}

export function checkTrail(gs: Grid[], pts: number[][], o: TrailCheckOptions): TrailReport {
  const line = densify(pts, o.spacing), runs: TrailRun[] = [];
  const reach = o.halfWidth + o.margin, n = Math.max(1, Math.round(reach / o.spacing));
  let columns = 0, worst: number | null = null;
  // a run of the same finding on the same side, extended while consecutive samples have it
  const open = new Map<string, TrailRun>();
  const note = (kind: TrailRun["kind"], side: TrailRun["side"], along: number, at: [number, number], drop: number | undefined, trailY: number | null, seen: Set<string>) => {
    const key = `${kind}:${side}`;
    seen.add(key);
    const r = open.get(key);
    if (r && along - r.to <= o.spacing * 1.5 + 1e-9) {
      r.to = along;
      if (drop !== undefined && (r.worst === undefined || drop > r.worst)) { r.worst = drop; r.at = at; }
      return;
    }
    const nr: TrailRun = { kind, side, from: along, to: along, at, ...(drop !== undefined ? { worst: drop } : {}), trailY };
    runs.push(nr);
    open.set(key, nr);
  };
  for (let i = 0; i < line.length; i++) {
    const [x, z, y, along] = line[i];
    const seen = new Set<string>();
    // the heading here: from the previous sample to the next
    const p = line[Math.max(0, i - 1)], q = line[Math.min(line.length - 1, i + 1)];
    let dx = q[0] - p[0], dz = q[1] - p[1];
    const l = Math.hypot(dx, dz) || 1;
    dx /= l; dz /= l;
    const below = Number.isFinite(y) ? y + 2.5 : Infinity;
    const centre = spansAt(gs, x, z, below)[0];
    const trailY = centre ? centre.top : null;
    if (!centre) note("noGround", "centre", along, [x, z], undefined, null, seen);
    for (let k = -n; k <= n; k++) {
      const off = k * o.spacing, px = x - dz * off, pz = z + dx * off, side: TrailRun["side"] = k === 0 ? "centre" : k > 0 ? "left" : "right";
      columns++;
      if (Math.abs(off) <= o.holeRadius + 1e-9 && !spansAt(gs, px, pz).some((sp) => sp.top > o.above + 1e-6)) note("hole", side, along, [px, pz], undefined, trailY, seen);
      if (trailY === null) continue;
      const top = spansAt(gs, px, pz, trailY + o.headroom)[0];
      const drop = top ? trailY - top.top : Infinity;
      if (drop > o.maxDrop) {
        note("drop", side, along, [px, pz], Number.isFinite(drop) ? drop : undefined, trailY, seen);
        if (Number.isFinite(drop) && (worst === null || drop > worst)) worst = drop;
      }
    }
    // runs not seen at this sample are closed
    for (const key of [...open.keys()]) if (!seen.has(key)) open.delete(key);
  }
  // corners where four draped skin blocks meet, near the centreline
  const skinCorners: TrailReport["skinCorners"] = [];
  const corners = new Map<string, { at: [number, number]; n: number }>();
  for (const g of gs) {
    if (!g.frame.corners) continue;
    for (const cx of [g.bounds.min[0], g.bounds.max[0]])
      for (const cz of [g.bounds.min[2], g.bounds.max[2]]) {
        const key = `${Math.round(cx * 100)}:${Math.round(cz * 100)}`, c = corners.get(key);
        if (c) c.n++;
        else corners.set(key, { at: [cx, cz], n: 1 });
      }
  }
  for (const c of corners.values()) {
    if (c.n < 4) continue;
    let best = { d: Infinity, along: 0 };
    for (const s of line) { const d = Math.hypot(s[0] - c.at[0], s[1] - c.at[1]); if (d < best.d) best = { d, along: s[3] }; }
    if (best.d <= o.holeRadius + 0.6) skinCorners.push({ at: c.at, along: best.along, off: best.d, blocks: c.n });
  }
  return { length: line[line.length - 1][3], samples: line.length, columns, runs, worstDrop: worst, skinCorners };
}
