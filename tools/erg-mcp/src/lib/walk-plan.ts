// game_walk's route following: a route planned over the open project's voxels with check_reachability's walk graph
// (lib/reach.ts, the worm's body width enforced, short jumps allowed), and pure-pursuit steering along a dense route, so
// the worm follows the route's turns round pillars and keep corners instead of heading straight at the next waypoint.
// Map units in and out; pure (no game calls).
import type { EditorStore, Vec3 } from "../core";
import { waterMap } from "./inspect-field";
import { nodeAt, pathStats, pathTo, search, standing, surfaces, type ReachOptions } from "./reach";
import { grids } from "./voxel";

export interface Plan { path: Vec3[]; length: number; narrowest: number | null; rules: ReachOptions; }

/** The walk from `from` (where the worm stands) to within `radius` of `to` ([x, z]) over the project's voxels, as a dense
 * route (a point per column), or null with why. `margin` pads the searched box round the two points. */
export function planWalk(store: EditorStore, from: Vec3, to: [number, number], opts: { radius?: number; bodyWidth?: number; maxGap?: number; maxSlope?: number; step?: number; margin?: number } = {}):
  { plan: Plan } | { why: string } {
  const gs = grids(store), step = opts.step ?? 1, pad = opts.margin ?? 24;
  const r = { x0: Math.min(from[0], to[0]) - pad, z0: Math.min(from[2], to[1]) - pad, x1: Math.max(from[0], to[0]) + pad, z1: Math.max(from[2], to[1]) + pad };
  const o: ReachOptions = { step, maxStep: 0.5, maxSlope: opts.maxSlope ?? 45, maxDrop: 8, maxGap: opts.maxGap ?? 2, headroom: 1.5, water: waterMap(store), slopeOver: "tread",
    ...(opts.bodyWidth ? { bodyWidth: opts.bodyWidth } : {}) };
  const S = surfaces(gs, r, step, o.headroom);
  const n0 = standing(S, from, o, gs);
  if (n0 < 0) return { why: `no walk-graph ground under the worm at (${from.map((v) => Math.round(v * 10) / 10).join(", ")})` };
  const goal = new Uint8Array(S.nodes), rad = opts.radius ?? 1.5;
  let goals = 0;
  for (let n = 0; n < S.nodes; n++) { const p = nodeAt(S, n); if (Math.hypot(p[0] - to[0], p[2] - to[1]) <= rad) { goal[n] = 1; goals++; } }
  if (!goals) return { why: `no ground within ${rad} of the target` };
  const q = search(S, n0, o, goal);
  let hit = -1;
  for (let n = 0; n < S.nodes; n++) if (q.reached[n] && goal[n] && (hit < 0 || q.dist[n] < q.dist[hit])) hit = n;
  if (hit < 0) return { why: "the walk graph does not get there from the worm's spot (check_reachability says why)" };
  const nodes = pathTo(q, hit), st = pathStats(S, nodes, o);
  return { plan: { path: nodes.map((n) => nodeAt(S, n)), length: st.length, narrowest: st.narrowest ? st.narrowest.width : null, rules: o } };
}

/** Distances along a polyline ([x, z] points) from its start. */
export function cumulative(pts: number[][]): number[] {
  const out = [0];
  for (let i = 1; i < pts.length; i++) out.push(out[i - 1] + Math.hypot(pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1]));
  return out;
}

/** Pure pursuit on a polyline ([x, z]): the point nearest p on the segments from `seg` on that start within `window`
 * past the end of segment `seg` (so a route that doubles back is not cut short), and the point `lookahead` further along it. Returns the segment the
 * nearest point is on (the next call's `seg`), how far along the line it is, how far p is off the line, and the aim. */
export function pursue(pts: number[][], cum: number[], seg: number, p: [number, number], lookahead: number, window: number):
  { seg: number; along: number; off: number; aim: [number, number] } {
  let best = { seg, along: cum[seg], off: Infinity };
  const reach = (seg + 1 < cum.length ? cum[seg + 1] : cum[seg]) + window;
  for (let i = seg; i < pts.length - 1 && cum[i] <= reach; i++) {
    const a = pts[i], b = pts[i + 1], dx = b[0] - a[0], dz = b[1] - a[1], l2 = dx * dx + dz * dz;
    const t = l2 ? Math.max(0, Math.min(1, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dz) / l2)) : 0;
    const d = Math.hypot(a[0] + t * dx - p[0], a[1] + t * dz - p[1]);
    if (d < best.off - 1e-9) best = { seg: i, along: cum[i] + t * Math.sqrt(l2), off: d };
  }
  if (pts.length === 1) return { seg: 0, along: 0, off: Math.hypot(pts[0][0] - p[0], pts[0][1] - p[1]), aim: [pts[0][0], pts[0][1]] };
  const want = Math.min(cum[cum.length - 1], best.along + lookahead);
  let k = best.seg;
  while (k < pts.length - 2 && cum[k + 1] < want) k++;
  const a = pts[k], b = pts[k + 1], len = cum[k + 1] - cum[k], t = len ? (want - cum[k]) / len : 1;
  return { seg: best.seg, along: best.along, off: best.off, aim: [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t] };
}

/** The keys.h name of a game binding's label (wum.input.binding gives controls_logic.h KeyLabel's: "Enter", "Backspace",
 * "Ctrl", "Space", "Q"), for Automation's key lines; null for none or a mouse button. */
export function keyOfLabel(label: string | null | undefined): string | null {
  if (!label || /^[LMR]MB$/.test(label)) return null;
  const special: Record<string, string> = { Ctrl: "LCONTROL", RCtrl: "RCONTROL", Shift: "LSHIFT", RShift: "RSHIFT", Alt: "LMENU", RAlt: "RMENU", Enter: "RETURN",
    Esc: "ESCAPE", Backspace: "BACK", Caps: "CAPITAL" };
  return special[label] ?? (/^[A-Za-z0-9]+$/.test(label) ? label.toUpperCase() : null);
}
