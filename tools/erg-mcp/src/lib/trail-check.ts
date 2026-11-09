// check_trail: the ground across a walking trail. The centreline is sampled every `spacing` (map units); at each sample
// the trail's ground (the top at or under the trail's own height, so a switchback's leg is not confused with the leg
// over it) is compared with the ground across the bed and a margin either side: a column more than `maxDrop` under the
// trail's ground (or with no ground at all) is an edge a worm can step or be knocked off (a column whose only ground is
// a bank rising from just over the trail, a cut wall, is counted as a bank, not a drop), and a column within
// `holeRadius` of the centreline whose ray straight down meets no solid over the water (`above`) is a hole the camera
// and a worm see through. check_reachability's walk graph keeps to the ground it can stand on, so it passes a trail with
// a crumbling edge or a pit in its bed; this reads every column. It also lists the corners where four draped skin blocks
// meet near the centreline (`skinCorners`): in the Caldera v2 Test (2026-11-09) mines and worms fell through the skin
// within about 0.4 map units of each corner of the crater's skin block, which the voxels here show as solid (a test
// island's corners held every mine), so such a corner is a place to probe with game_mine_drop {trails}. Pure: grids in,
// a report out; map units.
//
// Drops are told apart (classifyDrops): a V-drop whose landing is back on the trail (the next leg under a hairpin), a
// drop with a walkable way back onto the trail (the walk graph of check_reachability, searched from the landing), and a
// trap: a trough a worm knocked or stepped into cannot climb out of. The Caldera v2 round-3 run listed a 6.1 drop on the
// NW trail as any other, and that trough is exactly where team 3 got stuck. walkTrail walks the trail itself: a worm on
// the centreline and on lines offset either side, with its body width and the game's step and slope rules, so a ridge
// between two legs that the column-by-column read passes (every column has ground) is caught as a rise the worm cannot
// cross. mineSuspects lists ground the game let mines through in Tests: the top of a block of a non-whole scale (the
// 1.5-scale castle block at (44.7, -70.44), the launcher pads) and a skin cell sheared more than a voxel across itself.
import { spansAt } from "./inspect-field";
import { nodeAt, search, spanAcross, standing, surfaces, walks, type ReachOptions } from "./reach";
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

export interface Landing { kind: "trail" | "exit" | "trap" | "unknown"; at: [number, number, number]; legAlong?: number; exitLength?: number; climbRefused?: number; why: string; }
export interface TrailRun { kind: "drop" | "hole" | "noGround"; side: "left" | "right" | "centre"; from: number; to: number; at: [number, number]; worst?: number; trailY?: number | null; landing?: Landing; }
export interface MineSuspect { kind: "scaledBlock" | "shearedCell"; from: number; to: number; at: [number, number]; frame: number; detail: string; }
export interface TrailReport { length: number; samples: number; columns: number; runs: TrailRun[]; worstDrop: number | null; banks: number; skinCorners: { at: [number, number]; along: number; off: number; blocks: number }[];
  centreline: [number, number, number, number][];   // [x, z, ground y, along] of each sample with ground
  mineSuspects: MineSuspect[]; }

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
  let columns = 0, worst: number | null = null, banks = 0;
  const centreline: TrailReport["centreline"] = [], suspects: MineSuspect[] = [];
  let suspect: MineSuspect | null = null;
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
    else centreline.push([x, z, centre.top, along]);
    // ground the game let mines through in Tests, under the centreline (one run per stretch)
    const sus = centre ? mineSuspectAt(gs, centre.frame, x, z) : null;
    if (sus && suspect && suspect.kind === sus.kind && suspect.frame === centre!.frame && along - suspect.to <= o.spacing * 1.5 + 1e-9) suspect.to = along;
    else if (sus) { suspect = { ...sus, from: along, to: along, at: [x, z] }; suspects.push(suspect); }
    else suspect = null;
    for (let k = -n; k <= n; k++) {
      const off = k * o.spacing, px = x - dz * off, pz = z + dx * off, side: TrailRun["side"] = k === 0 ? "centre" : k > 0 ? "left" : "right";
      columns++;
      if (Math.abs(off) <= o.holeRadius + 1e-9 && !spansAt(gs, px, pz).some((sp) => sp.top > o.above + 1e-6)) note("hole", side, along, [px, pz], undefined, trailY, seen);
      if (trailY === null) continue;
      const top = spansAt(gs, px, pz, trailY + o.headroom)[0];
      // no ground under the trail's but a bank rising from just over it (a cut wall: a skin's shell whose underside is
      // within twice the headroom of the trail's ground) is not an edge to fall off; Caldera v2 round 2 listed the NW
      // trail's uphill banks as "no ground" drops. Ground higher than that over nothing is an overhang over a drop.
      if (!top && spansAt(gs, px, pz).some((sp) => sp.bottom >= trailY + o.headroom - 1e-6 && sp.bottom <= trailY + 2 * o.headroom + 1e-6)) { banks++; continue; }
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
  return { length: line[line.length - 1][3], samples: line.length, columns, runs, worstDrop: worst, banks, skinCorners, centreline, mineSuspects: suspects };
}

/** Whether the ground frame under (x, z) is one the game let mines through in Tests: an upright block of a non-whole
 * scale (its top at (44.7, -70.44), a 1.5-scale castle block, let a mine fall 6 units to the skin under it; the
 * 1.5-scale launcher pads held at four of eight places), or a draped skin cell whose corner lifts differ by more than a
 * voxel across the cell (a quad folded steeper than 45 degrees: (54.44, -58.17) was sheared 7 units across one 2.5 cell,
 * (-44.1, -34.3) 13). Neither is proven to be the cause: game_mine_drop at the place says whether it holds. */
export function mineSuspectAt(gs: Grid[], frame: number, x: number, z: number): Omit<MineSuspect, "from" | "to" | "at"> | null {
  const g = gs.find((q) => q.id === frame);
  if (!g) return null;
  const m = g.toMap, sc = m[0];
  if (!(sc > 0) || m[1] !== 0 || m[2] !== 0) return null;
  const r2 = (v: number) => Math.round(v * 100) / 100;
  const c = g.frame.corners;
  if (c) {
    const X = g.size[0], cx = Math.min(X - 1, Math.max(0, Math.floor((x - m[3]) / sc))), cz = Math.min(g.size[2] - 1, Math.max(0, Math.floor((z - m[11]) / m[10])));
    const lifts = [c[cz * (X + 1) + cx], c[cz * (X + 1) + cx + 1], c[(cz + 1) * (X + 1) + cx], c[(cz + 1) * (X + 1) + cx + 1]].map((v) => (v ?? 0) * m[5]);
    const spread = Math.max(...lifts) - Math.min(...lifts);
    if (spread > sc + 1e-6) return { kind: "shearedCell", frame, detail: `the skin cell is sheared ${r2(spread)} map units across its ${r2(sc)} (corner lifts ${lifts.map(r2).join(", ")})` };
    return null;
  }
  if (Math.abs(sc - Math.round(sc)) > 1e-6) return { kind: "scaledBlock", frame, detail: `the ground is the top of ${g.frame.name} (frame ${frame}), a block of scale ${r2(sc)}` };
  return null;
}

// ------------------------------------------------------------------ drops told apart

/** The walk graph's rules for the drop and walk checks: a worm's step, slope, drop and body. */
export function trailReach(water: number | null, o: { maxStep?: number; maxSlope?: number; bodyWidth?: number } = {}): ReachOptions {
  return { step: 0.5, maxStep: o.maxStep ?? 0.5, maxSlope: o.maxSlope ?? 45, maxDrop: 8, maxGap: 0, headroom: 1.5, water, bodyWidth: o.bodyWidth ?? 0.6, slopeOver: "tread" };
}

/** The radius round a drop's landing the walk back is searched in, map units. */
export const EXIT_RADIUS = 14;

/** Tells each drop run's landing apart: `trail` (back on the trail's bed at the trail's own ground there: a V-drop onto
 * the leg under), `exit` (a walk of `exitLength` back onto the bed by the walking rules), `trap` (no way back within
 * EXIT_RADIUS; `climbRefused`: the smallest climb out that was too high), `unknown` (no standable landing: water, a
 * sliver). Writes `landing` into the runs. */
export function classifyDrops(gs: Grid[], rep: TrailReport, o: TrailCheckOptions, reach: ReachOptions): void {
  const cl = rep.centreline, r2 = (v: number) => Math.round(v * 100) / 100;
  for (const r of rep.runs) {
    if (r.kind !== "drop" || r.trailY === null || r.trailY === undefined) continue;
    // no ground at all under the edge (the sea, the void under an overhang): a fall with no landing, never harmless
    if (r.worst === undefined) { r.landing = { kind: "trap", at: [r.at[0], o.above, r.at[1]], why: "no ground at all under the edge within the bed and its margin: a fall into the water or the void" }; continue; }
    const L: [number, number, number] = [r.at[0], r.trailY - r.worst, r.at[1]];
    // back on the trail: a centreline sample within the bed whose ground is within a step of the landing
    let best: { d: number; along: number } | null = null;
    for (const c of cl) {
      if (Math.abs(c[2] - L[1]) > 1) continue;
      const d = Math.hypot(c[0] - L[0], c[1] - L[2]);
      if (d <= o.halfWidth + 0.25 && (!best || d < best.d)) best = { d, along: c[3] };
    }
    if (best) { r.landing = { kind: "trail", at: L, legAlong: best.along, why: `lands back on the trail, ${r2(best.along)} along: a V-drop onto the leg under` }; continue; }
    // a walk back onto the bed from the landing, by the walking rules, within EXIT_RADIUS
    const R = EXIT_RADIUS, S = surfaces(gs, { x0: L[0] - R, z0: L[2] - R, x1: L[0] + R, z1: L[2] + R }, reach.step, reach.headroom);
    const src = standing(S, L, reach, gs);
    if (src < 0) { r.landing = { kind: "unknown", at: L, why: "no standable ground at the landing (the water, or a sliver the walk graph does not sample)" }; continue; }
    const goal = new Uint8Array(S.nodes);
    let goals = 0;
    for (let n = 0; n < S.nodes; n++) {
      const p = nodeAt(S, n);
      for (const c of cl) if (Math.abs(c[2] - p[1]) <= 1 && Math.hypot(c[0] - p[0], c[1] - p[2]) <= o.halfWidth) { goal[n] = 1; goals++; break; }
    }
    if (!goals) { r.landing = { kind: "unknown", at: L, why: `no trail bed within ${R} map units of the landing` }; continue; }
    const q = search(S, src, reach, goal);
    let hit = -1;
    for (let n = 0; n < S.nodes && hit < 0; n++) if (goal[n] && q.reached[n]) hit = n;
    if (hit >= 0) { r.landing = { kind: "exit", at: L, exitLength: r2(q.dist[hit]), why: `a walk of ${r2(q.dist[hit])} map units leads back onto the trail` }; continue; }
    let refused = Infinity;
    for (let n = 0; n < S.nodes; n++) if (q.reached[n] && q.blocked[n] < refused) refused = q.blocked[n];
    r.landing = { kind: "trap", at: L, ...(Number.isFinite(refused) ? { climbRefused: r2(refused) } : {}),
      why: `no walkable way back onto the trail within ${R} map units${Number.isFinite(refused) ? ` (the smallest climb out of the trough that was too high: ${r2(refused)})` : ""}: a trough a worm cannot leave` };
  }
}

// ------------------------------------------------------------------ the walk itself

export interface WalkBlock { along: number; at: [number, number]; rise: number; run: number; why: string; }
export interface WalkLine { offset: number; ok: boolean; segments: number; blocked: WalkBlock[]; blockedCount: number; noFooting: number; turns: number; }

/** Walks the trail with the walking rules: on the centreline and on each of `offsets` (map units, left negative) a worm
 * is moved sample by sample over the ground at the trail's own height (a leg under a hairpin is not taken for the leg
 * over it), each move judged by check_reachability's rules (walks: a step of maxStep, a riser whose stair is no steeper
 * than maxSlope, a drop of maxDrop, a gap no narrower than bodyWidth). A move refused is a block: a ridge between legs, a
 * kerb too high, a slot too narrow. Where an offset line turns sharper than its offset allows (the inside of a hairpin runs
 * back on itself, the outside jumps round the tip), its point moves more than a few samples' length at once: that is no
 * move a worm makes, so the walk starts a new segment there (`turns` counts them) instead of judging the jump. */
export function walkTrail(gs: Grid[], pts: number[][], offsets: number[], o: TrailCheckOptions, reach: ReachOptions): WalkLine[] {
  const line = densify(pts, o.spacing), r2 = (v: number) => Math.round(v * 100) / 100;
  let x0 = Infinity, z0 = Infinity, x1 = -Infinity, z1 = -Infinity;
  for (const p of line) { x0 = Math.min(x0, p[0]); x1 = Math.max(x1, p[0]); z0 = Math.min(z0, p[1]); z1 = Math.max(z1, p[1]); }
  const R = Math.max(1, ...offsets.map(Math.abs)) + 3;
  const S = surfaces(gs, { x0: x0 - R, z0: z0 - R, x1: x1 + R, z1: z1 + R }, reach.step, reach.headroom);
  return offsets.map((off) => {
    const blocked: WalkBlock[] = [];
    let prev = -1, noFooting = 0, segments = 0, turns = 0, lx = NaN, lz = NaN;
    for (let i = 0; i < line.length; i++) {
      const [x, z, y, along] = line[i];
      const p = line[Math.max(0, i - 1)], q = line[Math.min(line.length - 1, i + 1)];
      let dx = q[0] - p[0], dz = q[1] - p[1];
      const l = Math.hypot(dx, dz) || 1;
      dx /= l; dz /= l;
      const px = x - dz * off, pz = z + dx * off, below = Number.isFinite(y) ? y + 2.5 : Infinity;
      // (Caldera v2 round 4) an offset line's jump round a sharp turn is not a move: a new segment from here
      const jump = off !== 0 && Number.isFinite(lx) && Math.hypot(px - lx, pz - lz) > Math.max(1, 4 * o.spacing);
      lx = px; lz = pz;
      if (jump) { turns++; prev = -1; }
      const g = spansAt(gs, px, pz, below)[0];
      const n = g ? standing(S, [px, g.top + 0.1, pz], reach, gs) : -1;
      if (n < 0) { noFooting++; prev = -1; continue; }
      if (prev >= 0 && n !== prev) {
        segments++;
        if (!walks(S, prev, n, reach)) {
          const rise = S.y[n] - S.y[prev], a = nodeAt(S, prev), b = nodeAt(S, n), run = Math.hypot(b[0] - a[0], b[2] - a[2]);
          const h = reach.bodyHeight ?? reach.headroom, span = Math.min(spanAcross(S, n, 1, 0, reach.maxStep, h), spanAcross(S, n, 0, 1, reach.maxStep, h));
          const why = rise < -reach.maxDrop ? `a drop of ${r2(-rise)}` : reach.bodyWidth && span < reach.bodyWidth - 1e-6 ? `a gap ${r2(span)} wide, narrower than the worm's ${reach.bodyWidth}`
            : rise > reach.maxStep ? `a rise of ${r2(rise)} over ${r2(run)} that the step and stair rules refuse (a ridge or kerb between legs)` : "refused by the walking rules";
          blocked.push({ along: r2(along), at: [r2(px), r2(pz)], rise: r2(rise), run: r2(run), why });
        }
      }
      prev = n;
    }
    return { offset: off, ok: blocked.length === 0, segments, blocked: blocked.slice(0, 20), blockedCount: blocked.length, noFooting, turns };
  });
}
