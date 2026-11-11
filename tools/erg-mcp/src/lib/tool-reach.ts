// Movement-weapon reachability, for check_tool_reach: where a worm gets with a jetpack, a ninja rope or a girder when its
// legs stop. Built on check_reachability's walk graph (lib/reach.ts): the walk from a start is level 0; each tool use (a
// "hop") takes a worm from a place it stands to another place it could stand, the walk from there is the next level, and
// so on, so a level is the least number of tool uses (about a turn each) it takes to get there. Pure: surfaces in, a
// report out. Map units throughout.
//
// The tools are geometric estimates: a hop is judged from the take-off, the landing and the open air between them, never
// by the game's physics.
//   jetpack  a flight up to `rise` high with no run, or `run` far on the level with no climb, trading one for the other
//            linearly (rise / rise_max + run / run_max <= 1), over a path that is clear of solid ground; any drop;
//   rope     a shot from the worm at any bearing and elevation as far as `length`, anchored on the first solid ground it
//            meets (a wall face, a tower's flank, an arch's soffit); the worm climbs or swings to the landing, which lies
//            within `length` of the anchor (a straight line of air from the anchor's air side to the landing) and not
//            higher than the anchor;
//   girder   a span of `length` between the take-off and the landing along a clear line, no steeper than `maxSlope`.
// What the estimates leave out is stated in the tool's answer: thin ground narrower than hopStep is not a landing, a wall
// thinner than the column step may be flown through, the swing is not simulated.
import type { Vec3 } from "../core";
import { nodeAt, search, type ReachOptions, type Surfaces } from "./reach";

export type MoveTool = "jetpack" | "rope" | "girder";
export const MOVE_TOOLS: MoveTool[] = ["jetpack", "rope", "girder"];

export interface ToolLimits {
  jetpack: { rise: number; run: number };
  rope: { length: number };
  girder: { length: number; maxSlope: number };
}

/** The limits in map units. The rope's is the game's own number (Ninja.MaxLength 450 world units, Data/Tweak/TWEAK.XOM); the
 * jetpack's and the girder's are estimates no game test has measured yet (see CALIBRATION). */
export const TOOL_LIMITS: ToolLimits = {
  jetpack: { rise: 30, run: 40 },
  rope: { length: 22.5 },
  girder: { length: 6, maxSlope: 45 },
};

/** Tool uses that fit in one turn, from the game's data: the rope may be shot Ninja.NumShots (5) times a turn, a jetpack
 * flight is one tool use (its fuel, Jetpack.InitFuel 7500, is the turn's), and a girder ends the turn unless the scheme
 * sets GirdersDontEndTurn. */
export const USES_PER_TURN: Record<MoveTool, number> = { jetpack: 1, rope: 5, girder: 1 };

/** Where the limits come from, for the answer. */
export const CALIBRATION = {
  status: "partial",
  fromGameData: { "rope.length": "22.5 map units = Ninja.MaxLength 450 world units (Data/Tweak/TWEAK.XOM); the rope is also shot up to Ninja.NumShots = 5 times a turn" },
  estimates: ["jetpack.rise", "jetpack.run", "girder.length", "girder.maxSlope"],
  note: "the jetpack's and the girder's numbers are estimates: the game's input.drive cannot fly a jetpack or place a girder, so no game test has measured them (the jetpack's Jetpack.InitFuel is 7500 and its Jetpack.MaxAltitude 2000 world units = 100 map, neither gives a reach on its own). Confirm a plan in the game and pass limits.* from what it shows.",
};

/** The air a hop needs round the worm's path, map units: the body's height. */
const BODY = 0.9;
/** A take-off stands this far over its ground (the worm's centre), map units. */
const LIFT = 0.45;
/** The distance between samples along a path, map units. */
const SAMPLE = 0.5;

export interface Hop {
  tool: MoveTool;
  from: number;               // node taken off from
  to: number;                 // node landed on
  ratio: number;              // how much of the tool's limit the hop uses (1: the very limit)
  anchor?: Vec3;              // rope: where the rope holds
  ropeOut?: number;           // rope: the shot's length
  ropeIn?: number;            // rope: the anchor to the landing
}

export interface ToolRun {
  uses: Uint8Array;           // per node: the tool uses it takes to stand there (255: not reached)
  hopOf: Int32Array;          // per node: the hop that landed on the walk that reached it (-1: the start's own walk)
  hops: Hop[];
  levels: { uses: number; reached: number; hops: number }[];
  truncated: boolean;
}

export interface RunOptions {
  tools: MoveTool[];
  limits: ToolLimits;
  maxUses: number;
  hopStep: number;            // spacing of take-offs and landings, map units
  maxWork?: number;           // samples and pairs looked at in all, a guard (default 300 million)
}

/** True when the point is inside solid ground. */
function solidAt(S: Surfaces, x: number, y: number, z: number): boolean {
  const i = Math.floor((x - S.r.x0) / S.step), j = Math.floor((z - S.r.z0) / S.step);
  if (i < 0 || j < 0 || i >= S.w || j >= S.h) return false;
  const c = j * S.w + i;
  for (let k = S.spanStart[c]; k < S.spanStart[c + 1]; k++) if (S.spanTop[k] >= y && S.spanBottom[k] <= y) return true;
  return false;
}

/** True when a body of `BODY` height with its feet at y overlaps solid ground in the column. */
function bodyBlocked(S: Surfaces, x: number, y: number, z: number): boolean {
  const i = Math.floor((x - S.r.x0) / S.step), j = Math.floor((z - S.r.z0) / S.step);
  if (i < 0 || j < 0 || i >= S.w || j >= S.h) return false;
  const c = j * S.w + i;
  for (let k = S.spanStart[c]; k < S.spanStart[c + 1]; k++) if (S.spanTop[k] > y + 1e-6 && S.spanBottom[k] < y + BODY - 1e-6) return true;
  return false;
}

/** Whether a straight segment is clear for the body, sampled every SAMPLE, ignoring `skipA` at its start and `skipB` at
 * its end (the ground the worm stands on and lands on). Adds the samples to `work`. */
function clear(S: Surfaces, a: Vec3, b: Vec3, skipA: number, skipB: number, work: { n: number }): boolean {
  const dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2], len = Math.hypot(dx, dy, dz);
  const n = Math.max(1, Math.ceil(len / SAMPLE));
  for (let k = 0; k <= n; k++) {
    const d = (k / n) * len;
    if (d < skipA || len - d < skipB) continue;
    work.n++;
    const t = k / n;
    if (bodyBlocked(S, a[0] + dx * t, a[1] + dy * t - BODY / 2, a[2] + dz * t)) return false;
  }
  return true;
}

/** A polyline of points is clear when each of its segments is. */
function clearPath(S: Surfaces, pts: Vec3[], work: { n: number }): boolean {
  for (let k = 1; k < pts.length; k++) if (!clear(S, pts[k - 1], pts[k], k === 1 ? 0.8 : 0, k === pts.length - 1 ? 0.8 : 0, work)) return false;
  return true;
}

const above = (S: Surfaces, n: number, lift = LIFT): Vec3 => { const p = nodeAt(S, n); return [p[0], p[1] + lift, p[2]]; };

/** The coarse sample of a set of nodes: those in columns on a grid of every `ts` columns. */
function coarse(S: Surfaces, ts: number): (n: number) => boolean {
  return (n) => (S.cell[n] % S.w) % ts === 0 && Math.floor(S.cell[n] / S.w) % ts === 0;
}

/** Run the tool search from the walking starts. */
export function toolReach(S: Surfaces, starts: number[], o: ReachOptions, opt: RunOptions): ToolRun {
  const N = S.nodes, uses = new Uint8Array(N).fill(255), hopOf = new Int32Array(N).fill(-1);
  const hops: Hop[] = [], levels: ToolRun["levels"] = [];
  const ts = Math.max(1, Math.round(opt.hopStep / S.step)), isCoarse = coarse(S, ts);
  const work = { n: 0 }, cap = opt.maxWork ?? 300_000_000;
  let truncated = false;
  const L = opt.limits;
  const dry = (n: number) => o.water === null || S.y[n] > o.water;

  const walk = (src: number, level: number, hopIdx: number): number[] => {
    const q = search(S, src, o), fresh: number[] = [];
    for (let n = 0; n < N; n++) if (q.reached[n] && uses[n] === 255) { uses[n] = level; hopOf[n] = hopIdx; fresh.push(n); }
    return fresh;
  };

  let frontier: number[] = [];
  for (const s of starts) if (s >= 0 && uses[s] === 255) frontier.push(...walk(s, 0, -1));
  levels.push({ uses: 0, reached: frontier.length, hops: 0 });

  // the coarse landing columns, as lists of nodes, for the range loops
  const cw = Math.ceil(S.w / ts), ch = Math.ceil(S.h / ts);
  const colNodes: number[][] = Array.from({ length: cw * ch }, () => []);
  for (let n = 0; n < N; n++) if (isCoarse(n)) { const c = S.cell[n]; colNodes[Math.floor(Math.floor(c / S.w) / ts) * cw + Math.floor((c % S.w) / ts)].push(n); }
  const cellsAround = (x: number, z: number, radius: number, fn: (n: number) => void) => {
    const i0 = Math.max(0, Math.floor((x - radius - S.r.x0) / (S.step * ts))), i1 = Math.min(cw - 1, Math.floor((x + radius - S.r.x0) / (S.step * ts)));
    const j0 = Math.max(0, Math.floor((z - radius - S.r.z0) / (S.step * ts))), j1 = Math.min(ch - 1, Math.floor((z + radius - S.r.z0) / (S.step * ts)));
    for (let j = j0; j <= j1; j++) for (let i = i0; i <= i1; i++) for (const n of colNodes[j * cw + i]) fn(n);
  };

  for (let level = 1; level <= opt.maxUses && frontier.length && !truncated; level++) {
    const takeoffs = frontier.filter(isCoarse);
    const best = new Map<number, Hop>();
    const offer = (h: Hop) => { const cur = best.get(h.to); if (!cur || h.ratio < cur.ratio) best.set(h.to, h); };
    const fresh = (n: number) => uses[n] === 255 && dry(n);

    if (opt.tools.includes("jetpack")) {
      const J = L.jetpack;
      for (const a of takeoffs) {
        const pa = nodeAt(S, a), ha = above(S, a);
        cellsAround(pa[0], pa[2], J.run, (b) => {
          if (!fresh(b)) return;
          const pb = nodeAt(S, b), rise = pb[1] - pa[1], run = Math.hypot(pb[0] - pa[0], pb[2] - pa[2]);
          if (run > J.run || rise > J.rise) return;
          const ratio = rise > 0 ? rise / J.rise + run / J.run : run / J.run;
          if (ratio > 1) return;
          const cur = best.get(b);
          if (cur && cur.ratio <= ratio) return;
          work.n += 2;
          const hb = above(S, b), top = Math.max(ha[1], hb[1]) + 0.35;
          // straight; or up, over and down at the higher end's height
          if (clearPath(S, [ha, hb], work) || clearPath(S, [ha, [ha[0], top, ha[2]], [hb[0], top, hb[2]], hb], work)) offer({ tool: "jetpack", from: a, to: b, ratio });
        });
        if (work.n > cap) { truncated = true; break; }
      }
    }

    if (opt.tools.includes("girder") && !truncated) {
      const G = L.girder, tan = Math.tan((G.maxSlope * Math.PI) / 180);
      for (const a of takeoffs) {
        const pa = nodeAt(S, a);
        cellsAround(pa[0], pa[2], G.length, (b) => {
          if (!fresh(b)) return;
          const pb = nodeAt(S, b), rise = pb[1] - pa[1], run = Math.hypot(pb[0] - pa[0], pb[2] - pa[2]), len = Math.hypot(run, rise);
          if (len > G.length || Math.abs(rise) > tan * run + 1e-6) return;
          const ratio = len / G.length, cur = best.get(b);
          if (cur && cur.ratio <= ratio) return;
          // the span lies along the ground's tops, over the air between: its line is clear of solid at body height
          const lift = BODY / 2 + 0.1;
          if (clear(S, [pa[0], pa[1] + lift, pa[2]], [pb[0], pb[1] + lift, pb[2]], 1.2, 1.2, work)) offer({ tool: "girder", from: a, to: b, ratio });
        });
        if (work.n > cap) { truncated = true; break; }
      }
    }

    if (opt.tools.includes("rope") && !truncated) {
      const R = L.rope.length;
      // anchors: for each take-off, rays at every bearing and a fan of elevations, as far as the rope goes, held by the
      // first solid ground; one anchor per hopStep cell and 2-high band (the highest, then the nearest take-off to it: a higher anchor lands higher)
      interface Anchor { p: Vec3; q: Vec3; from: number; out: number; }
      const anchors = new Map<string, Anchor>();
      const AZ = 24, ELEV = [10, 25, 40, 55, 70, 85];
      const cell = opt.hopStep;
      for (const a of takeoffs) {
        const head = above(S, a, 0.6);
        for (let ai = 0; ai < AZ; ai++) {
          const az = (ai / AZ) * Math.PI * 2;
          for (const el of ELEV) {
            const e = (el * Math.PI) / 180, dx = Math.cos(az) * Math.cos(e), dy = Math.sin(e), dz = Math.sin(az) * Math.cos(e);
            let prev: Vec3 = head;
            for (let d = 1; d <= R; d += SAMPLE) {
              work.n++;
              const x = head[0] + dx * d, y = head[1] + dy * d, z = head[2] + dz * d;
              if (x < S.r.x0 || x >= S.r.x1 || z < S.r.z0 || z >= S.r.z1) break;
              if (solidAt(S, x, y, z)) {
                const key = `${Math.floor(x / cell)},${Math.floor(y / 2)},${Math.floor(z / cell)}`;
                const cur = anchors.get(key);
                if (!cur || y > cur.p[1] + 1e-6 || (y > cur.p[1] - 1e-6 && d < cur.out)) anchors.set(key, { p: [x, y, z], q: prev, from: a, out: d });
                break;
              }
              prev = [x, y, z];
            }
          }
        }
        if (work.n > cap) { truncated = true; break; }
      }
      if (!truncated)
        for (const an of anchors.values()) {
          cellsAround(an.p[0], an.p[2], R, (b) => {
            if (!fresh(b)) return;
            const pb = nodeAt(S, b), hb = above(S, b, 0.6);
            if (pb[1] > an.p[1] + 0.5) return;
            const dIn = Math.hypot(hb[0] - an.p[0], hb[1] - an.p[1], hb[2] - an.p[2]);
            if (dIn > R) return;
            const ratio = Math.max(an.out, dIn) / R, cur = best.get(b);
            if (cur && cur.ratio <= ratio) return;
            work.n += 2;
            const top = Math.max(an.q[1], hb[1]) + 0.35;
            // hanging at the anchor, the worm is drawn up to the landing's height and over: straight, or up, over and down
            if (clear(S, an.q, hb, 0.3, 0.8, work) || clearPath(S, [an.q, [an.q[0], top, an.q[2]], [hb[0], top, hb[2]], hb], work)) offer({ tool: "rope", from: an.from, to: b, ratio, anchor: an.p, ropeOut: an.out, ropeIn: dIn });
          });
          if (work.n > cap) { truncated = true; break; }
        }
    }

    // land the hops, the tightest last: the walk from each landing the walk before did not reach is the next level
    const order = [...best.values()].sort((p, q) => p.ratio - q.ratio);
    const next: number[] = [];
    let landed = 0;
    for (const h of order) {
      if (uses[h.to] !== 255) continue;
      hops.push(h);
      const f = walk(h.to, level, hops.length - 1);
      landed++;
      next.push(...f);
    }
    levels.push({ uses: level, reached: next.length, hops: landed });
    frontier = next;
  }
  return { uses, hopOf, hops, levels, truncated };
}

/** The hops that took a worm to node n, in order (empty: the walk got it there, or it was not reached). */
export function planTo(run: ToolRun, n: number): Hop[] {
  const out: Hop[] = [];
  for (let h = run.hopOf[n]; h >= 0; h = run.hopOf[run.hops[h].from]) out.unshift(run.hops[h]);
  return out;
}
