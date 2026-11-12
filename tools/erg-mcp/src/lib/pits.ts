// Pits a worm walks into and cannot leave, for check_pits, check_reachability's `traps` and save's advisory. The walk
// graph (lib/reach.ts) passes a route across a sheer-sided hollow as long as each move is a legal drop or step, and says
// nothing about getting out again (Dustcrown round 1: a worm sat in a 5 x 5 pit 2.5 deep for two turns). Here the same
// graph is read the other way: its moves as a directed graph with every drop allowed (a worm can always jump off a
// ledge, so a plateau is no trap), and a trap is a set of nodes from which every walk stays inside a small area: all its
// exits are climbs over the limit. Pure: surfaces in, traps out. Map units.
import type { Vec3 } from "../core";
import { climbs, nodeAt, spanAcross, type ReachOptions, type Surfaces } from "./reach";

const DIRS: [number, number][] = [[1, 0], [-1, 0], [0, 1], [0, -1], [1, 1], [1, -1], [-1, 1], [-1, -1]];

/** The nodes one move from a under the walking rules (no flood): the next column's highest node it climbs or drops to,
 * the diagonal only when one of the two columns beside it is walkable too, and a jump over a gap of up to maxGap. The
 * same moves search() makes (the tests hold them together). */
export function movesFrom(S: Surfaces, a: number, o: ReachOptions): number[] {
  const out: number[] = [], rise = o.gapRise ?? o.maxStep, h = o.bodyHeight ?? o.headroom;
  const dry = (n: number) => (o.water === null || S.y[n] > o.water) && (!o.allow || !!o.allow[n]);
  const span = (n: number) => Math.min(spanAcross(S, n, 1, 0, o.maxStep, h), spanAcross(S, n, 0, 1, o.maxStep, h), spanAcross(S, n, 1, 1, o.maxStep, h), spanAcross(S, n, 1, -1, o.maxStep, h));
  const wide = (b: number) => !o.bodyWidth || span(b) >= o.bodyWidth - 1e-6;
  const ok = (b: number, dx: number, dz: number) => climbs(S, a, b, dx, dz, o) && wide(b);
  const passable = (col: number, dx: number, dz: number) => {
    for (let b = S.start[col]; b < S.start[col + 1]; b++) if (dry(b) && ok(b, dx, dz)) return true;
    return false;
  };
  const c = S.cell[a], ci = c % S.w, cj = Math.floor(c / S.w);
  for (const [dx, dz] of DIRS) {
    const ni = ci + dx, nj = cj + dz;
    if (ni < 0 || nj < 0 || ni >= S.w || nj >= S.h) continue;
    const nc = nj * S.w + ni, d = S.step * Math.hypot(dx, dz);
    let best = -1;
    for (let b = S.start[nc]; b < S.start[nc + 1]; b++) if (dry(b) && ok(b, dx, dz) && (best < 0 || S.y[b] > S.y[best])) best = b;
    if (best >= 0 && dx && dz && !passable(cj * S.w + ci + dx, dx, 0) && !passable((cj + dz) * S.w + ci, 0, dz)) best = -1;
    if (best >= 0) { out.push(best); continue; }
    if (!(o.maxGap > 0)) continue;
    for (let k = 2; k * d <= o.maxGap + d + 1e-6; k++) {
      const mi = ci + dx * (k - 1), mj = cj + dz * (k - 1);
      if (mi < 0 || mj < 0 || mi >= S.w || mj >= S.h) break;
      const mc = mj * S.w + mi;
      let walled = false;
      for (let t = S.spanStart[mc]; t < S.spanStart[mc + 1] && !walled; t++) walled = S.spanTop[t] > S.y[a] + rise + 1e-6 && S.spanBottom[t] < S.y[a] + o.headroom - 1e-6;
      if (o.bodyWidth) for (let b = S.start[mc]; b < S.start[mc + 1] && !walled; b++) walled = Math.abs(S.y[b] - S.y[a]) <= o.headroom && span(b) < o.bodyWidth - 1e-6;
      if (walled) break;
      const ji = ci + dx * k, jj = cj + dz * k;
      if (ji < 0 || jj < 0 || ji >= S.w || jj >= S.h) break;
      const jc = jj * S.w + ji;
      let land = -1;
      for (let b = S.start[jc]; b < S.start[jc + 1]; b++) {
        const dy = S.y[b] - S.y[a];
        if (dry(b) && dy <= rise + 1e-6 && dy >= -o.maxDrop && (land < 0 || S.y[b] > S.y[land])) land = b;
      }
      if (land >= 0) { if (wide(land)) out.push(land); break; }
      if (S.start[jc] < S.start[jc + 1] && S.y[S.start[jc]] > S.y[a] + rise) break;
    }
  }
  return out;
}

export interface Trap {
  nodes: number[];                 // every node of the trap (all a worm in it can walk to)
  area: number;                    // map units squared
  floor: number;                   // its lowest ground
  top: number;                     // its highest ground
  at: Vec3;                        // the lowest node
  min: [number, number]; max: [number, number];   // its box in x, z
  /** The least climb out: from a node of the trap up to the lowest ground beside it that is not part of it (null: no
   * ground beside it at all, an islet in the sea or the void). */
  climbOut: number | null;
  exitAt: Vec3 | null;             // where that climb is (the ground climbed onto)
  kind: "pit" | "islet";
  /** A pit beside the sea: one of its nodes has ground at or under the water in the next column that it could step or
   * drop to were it dry. Its way out is into the water: a steep shore (a perch between the waterline and a face too
   * steep to climb), not a closed hollow, so filling basins never removes it (limit the shore's slope instead). */
  shore?: boolean;
  /** A pit under a roof all over: every one of its nodes has solid over it in its own column (a hall inside a keep, a
   * sealed cellar). With no start's walk reaching it, nothing falls into it either: a room, not a hole in the ground. */
  roofed?: boolean;
}

export interface TrapOptions {
  maxArea: number;                 // a closed area larger than this is a region of the map, not a trap
  minClimb?: number;               // a pit whose least climb out is under this is left out (default 0)
}

/** The traps of the walk graph under `o` with every drop allowed: per node its trap's index (-1 none), and the traps,
 * largest first. An islet (no ground beside it to climb to) is told from a pit. */
export function findTraps(S: Surfaces, o: ReachOptions, t: TrapOptions): { trapOf: Int32Array; traps: Trap[] } {
  const ro: ReachOptions = { ...o, maxDrop: Infinity, flood: undefined };
  const N = S.nodes, cap = Math.max(1, Math.floor(t.maxArea / (S.step * S.step)));
  const dry = (n: number) => (ro.water === null || S.y[n] > ro.water) && (!ro.allow || !!ro.allow[n]);
  // adjacency, flat
  const head = new Int32Array(N + 1), adj: number[] = [];
  for (let a = 0; a < N; a++) {
    head[a] = adj.length;
    if (dry(a)) for (const b of movesFrom(S, a, ro)) adj.push(b);
  }
  head[N] = adj.length;
  // Tarjan's strongly connected components, iterative; components come out sinks first
  const index = new Int32Array(N).fill(-1), low = new Int32Array(N), comp = new Int32Array(N).fill(-1), onStack = new Uint8Array(N);
  const stack: number[] = [], compSize: number[] = [];
  let next = 0;
  const it = new Int32Array(N);
  for (let r = 0; r < N; r++) {
    if (index[r] >= 0 || !dry(r)) continue;
    const call = [r];
    index[r] = low[r] = next++; stack.push(r); onStack[r] = 1; it[r] = head[r];
    while (call.length) {
      const v = call[call.length - 1];
      if (it[v] < head[v + 1]) {
        const w = adj[it[v]++];
        if (index[w] < 0) { index[w] = low[w] = next++; stack.push(w); onStack[w] = 1; it[w] = head[w]; call.push(w); }
        else if (onStack[w]) low[v] = Math.min(low[v], index[w]);
      } else {
        call.pop();
        if (call.length) { const p = call[call.length - 1]; low[p] = Math.min(low[p], low[v]); }
        if (low[v] === index[v]) {
          const id = compSize.length;
          let n = 0;
          for (;;) { const w = stack.pop()!; onStack[w] = 0; comp[w] = id; n++; if (w === v) break; }
          compSize.push(n);
        }
      }
    }
  }
  // a component's closure (all a worm in it can walk to), sinks first: small (a trap) or big
  const C = compSize.length, members: number[][] = Array.from({ length: C }, () => []);
  for (let n = 0; n < N; n++) if (comp[n] >= 0) members[comp[n]].push(n);
  const big = new Uint8Array(C), closure: (number[] | null)[] = new Array(C).fill(null);
  for (let c = 0; c < C; c++) {
    if (compSize[c] > cap) { big[c] = 1; continue; }
    const seen = new Set<number>([c]), todo = [c];
    let total = 0, isBig = false;
    while (todo.length && !isBig) {
      const q = todo.pop()!;
      total += compSize[q];
      if (total > cap) { isBig = true; break; }
      for (const n of members[q]) for (let e = head[n]; e < head[n + 1] && !isBig; e++) {
        const q2 = comp[adj[e]];
        if (seen.has(q2)) continue;
        if (big[q2]) { isBig = true; break; }
        seen.add(q2); todo.push(q2);
      }
    }
    if (isBig) big[c] = 1;
    else closure[c] = [...seen];
  }
  // each small closure as a candidate: its nodes, and the least climb out (ground beside it that is not its own)
  const make = (c: number): Trap => {
    const own = new Set(closure[c]!), nodes: number[] = [];
    for (const q of own) for (const n of members[q]) nodes.push(n);
    let lowN = nodes[0], top = -Infinity, climbOut: number | null = null, exitAt: Vec3 | null = null;
    const min: [number, number] = [Infinity, Infinity], max: [number, number] = [-Infinity, -Infinity];
    let shore = false;
    for (const n of nodes) {
      if (S.y[n] < S.y[lowN]) lowN = n;
      top = Math.max(top, S.y[n]);
      const p = nodeAt(S, n);
      min[0] = Math.min(min[0], p[0] - S.step / 2); min[1] = Math.min(min[1], p[2] - S.step / 2);
      max[0] = Math.max(max[0], p[0] + S.step / 2); max[1] = Math.max(max[1], p[2] + S.step / 2);
      const ci = S.cell[n] % S.w, cj = Math.floor(S.cell[n] / S.w);
      for (const [dx, dz] of DIRS) {
        const ni = ci + dx, nj = cj + dz;
        if (ni < 0 || nj < 0 || ni >= S.w || nj >= S.h) continue;
        const nc = nj * S.w + ni;
        for (let b = S.start[nc]; b < S.start[nc + 1]; b++) {
          // wet ground beside it, no higher than a step up: the sea is this node's way out
          if (ro.water !== null && S.y[b] <= ro.water && S.y[b] <= S.y[n] + ro.maxStep + 1e-6) shore = true;
          if (!dry(b) || own.has(comp[b])) continue;
          const rise = S.y[b] - S.y[n];
          if (rise > 0 && (climbOut === null || rise < climbOut)) { climbOut = rise; exitAt = nodeAt(S, b); }
        }
      }
    }
    // roofed: solid over every node, in its own column (the spans are highest first; one whose bottom is over the node)
    const roofed = nodes.every((n) => { const c = S.cell[n]; for (let t = S.spanStart[c]; t < S.spanStart[c + 1]; t++) if (S.spanBottom[t] > S.y[n] + 1e-6) return true; return false; });
    return { nodes, area: nodes.length * S.step * S.step, floor: S.y[lowN], top, at: nodeAt(S, lowN), min, max, climbOut, exitAt, kind: climbOut === null ? "islet" : "pit", ...(shore ? { shore } : {}), ...(roofed ? { roofed } : {}) };
  };
  const cand: (Trap | null)[] = closure.map((cl, c) => (cl ? make(c) : null));
  // closures nest (a pit in the floor of a pit, a pit on an islet): closure(q) lies inside closure(c) when q is in it.
  // A pit is listed once, as the largest closure that still has ground beside it to climb to; an islet as the largest
  // closure with none, and a pit on it apart.
  const inPit = new Uint8Array(C), inAny = new Uint8Array(C);
  for (let c = 0; c < C; c++) {
    if (!cand[c]) continue;
    for (const q of closure[c]!) if (q !== c) { inAny[q] = 1; if (cand[c]!.kind === "pit") inPit[q] = 1; }
  }
  const minClimb = t.minClimb ?? 0;
  const pits = [...Array(C).keys()].filter((c) => cand[c]?.kind === "pit" && !inPit[c] && cand[c]!.climbOut! >= minClimb - 1e-6);
  const islets = [...Array(C).keys()].filter((c) => cand[c]?.kind === "islet" && !inAny[c]);
  const order = [...pits.sort((a, b) => cand[b]!.nodes.length - cand[a]!.nodes.length), ...islets.sort((a, b) => cand[b]!.nodes.length - cand[a]!.nodes.length)];
  // a node's trap: its pit when it is in one, else its islet
  const trapOf = new Int32Array(N).fill(-1), traps: Trap[] = [];
  for (const c of order) {
    const id = traps.length;
    traps.push(cand[c]!);
    for (const n of cand[c]!.nodes) if (trapOf[n] < 0) trapOf[n] = id;
  }
  return { trapOf, traps };
}
