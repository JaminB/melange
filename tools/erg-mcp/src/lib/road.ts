// sculpt_road's pure parts: a road's centreline marched over the ground at no more than a grade, and the height a draped
// skin's corners take to carry it. Map units throughout.
//
// The march (no switchbacks): a step of `step` map units at a time from `from` towards `to`. Where the ground straight
// towards `to` climbs no steeper than the grade the road heads there; where it is steeper the road leans off the uphill
// direction to the side that keeps it nearest the heading to `to`, by the angle at which the ground's own slope along the
// heading is the grade (a road up a cone winds round it), turning at most `turn` radians a step (more near `to`, as
// tight as the circle round it). The road's own height
// follows the ground but changes by at most the grade a step, so a stretch the ground climbs too fast for is a cut, and a
// dip a fill. With `switchbacks` the road is a zigzag of that many legs between from and to, its turns `spread` / 2 to
// alternate sides, each leg graded the same way. It stops at `to` (within three steps), or after maxLength.

export type Ground = (x: number, z: number) => number | null;
export type RoadPoint = [number, number, number];   // x, z, y

export interface MarchOptions { maxGrade: number; step?: number; maxLength?: number; switchbacks?: number; spread?: number; turn?: number; }

export interface March { pts: RoadPoint[]; stop: "arrived" | "maxLength" | "noGround"; length: number; climb: number; steepest: number; cut: number; fill: number; }

const clamp = (v: number, lo: number, hi: number) => Math.max(lo, Math.min(hi, v));

/** The road's height along `xz` points: from the ground at the first, each next changing by at most `tan` x the step. */
function grade(ground: Ground, xz: [number, number][], tan: number, y0: number): { pts: RoadPoint[]; stop?: "noGround"; cut: number; fill: number } {
  const pts: RoadPoint[] = [[xz[0][0], xz[0][1], y0]];
  let y = y0, cut = 0, fill = 0;
  for (let k = 1; k < xz.length; k++) {
    const [x, z] = xz[k], g = ground(x, z);
    if (g === null) return { pts, stop: "noGround", cut, fill };
    const d = Math.hypot(x - xz[k - 1][0], z - xz[k - 1][1]);
    y += clamp(g - y, -tan * d, tan * d);
    cut = Math.max(cut, g - y);
    fill = Math.max(fill, y - g);
    pts.push([x, z, y]);
  }
  return { pts, cut, fill };
}

export function marchRoad(ground: Ground, from: [number, number], to: [number, number], o: MarchOptions): March {
  const step = o.step ?? 1, tan = Math.tan((o.maxGrade * Math.PI) / 180), maxLength = o.maxLength ?? 2000, turn = o.turn ?? 0.12;
  const y0 = ground(from[0], from[1]);
  if (y0 === null) return { pts: [], stop: "noGround", length: 0, climb: 0, steepest: 0, cut: 0, fill: 0 };
  let xz: [number, number][] = [[from[0], from[1]]], stop: March["stop"] = "arrived";
  if (o.switchbacks && o.switchbacks > 1) {
    // a zigzag of legs: the turning points alternate `spread` / 2 to each side of the line from -> to
    const n = o.switchbacks, dx = to[0] - from[0], dz = to[1] - from[1], L = Math.hypot(dx, dz) || 1, nx = -dz / L, nz = dx / L;
    const spread = o.spread ?? Math.max(8, L / 2), corners: [number, number][] = [[from[0], from[1]]];
    for (let k = 1; k < n; k++) {
      const f = k / n, side = k % 2 ? 1 : -1;
      corners.push([from[0] + dx * f + nx * side * spread / 2, from[1] + dz * f + nz * side * spread / 2]);
    }
    corners.push([to[0], to[1]]);
    xz = [];
    for (let k = 0; k + 1 < corners.length; k++) {
      const [a, b] = [corners[k], corners[k + 1]], len = Math.hypot(b[0] - a[0], b[1] - a[1]), m = Math.max(1, Math.ceil(len / step));
      for (let i = k ? 1 : 0; i <= m; i++) xz.push([a[0] + ((b[0] - a[0]) * i) / m, a[1] + ((b[1] - a[1]) * i) / m]);
    }
  } else {
    // the lean march
    const e = Math.max(0.5, step / 2);
    const slopeAlong = (x: number, z: number, h: number) => {
      const a = ground(x + Math.cos(h) * e, z + Math.sin(h) * e), b = ground(x - Math.cos(h) * e, z - Math.sin(h) * e);
      return a === null || b === null ? 0 : (a - b) / (2 * e);
    };
    let [x, z] = from, psi = Math.atan2(to[1] - z, to[0] - x), side = 0, length = 0;
    for (;;) {
      const goal = Math.atan2(to[1] - z, to[0] - x);
      let want = goal;
      if (slopeAlong(x, z, goal) > tan) {
        // the headings whose climb is about the grade (searched round the circle), the side kept once chosen
        let best: number | null = null, bestScore = Infinity;
        for (let k = 0; k < 72; k++) {
          const h = (k / 72) * 2 * Math.PI, s = slopeAlong(x, z, h);
          if (s > tan * 0.97 || s < 0) continue;
          const off = Math.atan2(Math.sin(h - goal), Math.cos(h - goal));
          if (side && Math.sign(off) !== side) continue;
          const score = Math.abs(off) + (tan * 0.97 - s) * 2;
          if (score < bestScore) { bestScore = score; best = h; }
        }
        if (best !== null) { want = best; if (!side) side = Math.sign(Math.atan2(Math.sin(best - goal), Math.cos(best - goal))) || 1; }
      }
      // near `to` the road may turn as tight as the circle it is on (a contour round a summit is small)
      const d = Math.atan2(Math.sin(want - psi), Math.cos(want - psi)), lim = Math.max(turn, (1.2 * step) / Math.max(1, Math.hypot(to[0] - x, to[1] - z)));
      psi += clamp(d, -lim, lim);
      x += Math.cos(psi) * step;
      z += Math.sin(psi) * step;
      length += step;
      xz.push([x, z]);
      // within three steps: a summit's own circle can be too tight to come closer at the grade
      if (Math.hypot(to[0] - x, to[1] - z) <= 3 * step) { xz.push([to[0], to[1]]); break; }
      if (length >= maxLength) { stop = "maxLength"; break; }
    }
  }
  const g = grade(ground, xz, tan, y0);
  if (g.stop) stop = g.stop;
  let length = 0, steepest = 0;
  for (let k = 1; k < g.pts.length; k++) {
    const d = Math.hypot(g.pts[k][0] - g.pts[k - 1][0], g.pts[k][1] - g.pts[k - 1][1]);
    length += d;
    if (d > 1e-9) steepest = Math.max(steepest, (Math.atan2(Math.abs(g.pts[k][2] - g.pts[k - 1][2]), d) * 180) / Math.PI);
  }
  return { pts: g.pts, stop, length, climb: g.pts.length ? g.pts[g.pts.length - 1][2] - y0 : 0, steepest, cut: g.cut, fill: g.fill };
}

/** The road near a point: the distance to its centreline (x, z) and the road's height there. A grid of buckets keeps a
 * lookup to the segments within `reach`. */
export class RoadIndex {
  private buckets = new Map<string, number[]>();
  constructor(readonly pts: RoadPoint[], private reach: number, private cell = 4) {
    for (let k = 0; k + 1 < pts.length; k++) {
      const [a, b] = [pts[k], pts[k + 1]];
      const x0 = Math.floor((Math.min(a[0], b[0]) - reach) / cell), x1 = Math.floor((Math.max(a[0], b[0]) + reach) / cell);
      const z0 = Math.floor((Math.min(a[1], b[1]) - reach) / cell), z1 = Math.floor((Math.max(a[1], b[1]) + reach) / cell);
      for (let i = x0; i <= x1; i++) for (let j = z0; j <= z1; j++) {
        const key = `${i},${j}`, l = this.buckets.get(key);
        if (l) l.push(k); else this.buckets.set(key, [k]);
      }
    }
  }
  /** The nearest point of the centreline within reach: its distance, the road's height there and how far along it is. */
  near(x: number, z: number): { d: number; y: number; along: number } | null {
    const l = this.buckets.get(`${Math.floor(x / this.cell)},${Math.floor(z / this.cell)}`);
    if (!l) return null;
    let best: { d: number; y: number; along: number } | null = null;
    for (const k of l) {
      const a = this.pts[k], b = this.pts[k + 1], dx = b[0] - a[0], dz = b[1] - a[1], L2 = dx * dx + dz * dz;
      const t = L2 > 0 ? clamp(((x - a[0]) * dx + (z - a[1]) * dz) / L2, 0, 1) : 0;
      const d = Math.hypot(x - (a[0] + dx * t), z - (a[1] + dz * t));
      if (d <= this.reach && (!best || d < best.d)) best = { d, y: a[2] + (b[2] - a[2]) * t, along: k + t };
    }
    return best;
  }
}

/** How much of the road's height a point `d` from the centreline takes: all of it on the road (d <= half the width),
 * none past the shoulder, a smooth step between. */
export function roadWeight(d: number, halfWidth: number, shoulder: number): number {
  if (d <= halfWidth) return 1;
  if (d >= halfWidth + shoulder || shoulder <= 0) return 0;
  const t = (d - halfWidth) / shoulder;
  return 1 - t * t * (3 - 2 * t);
}
