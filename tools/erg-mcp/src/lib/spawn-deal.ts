// spawns.mode areas: the deal Melange makes at match start (src/erg/luagen.cpp DealAreas, the same draws in the same
// order), for place_spawn_knots' preview and game_probe's check. Pure.
import type { SpawnArea } from "../core";

export interface AreasSpec { areas: SpawnArea[]; pattern?: string[]; opposite?: [number, number][]; }

/** Per team rank (the match's teams in TeamIndex order, 0-based) the area index, or -1 for a team past the areas.
 * seed null: the chunk's fixed deal (each step's lowest unused area, the teams in order). */
export function dealAreas(a: AreasSpec, teams: number, seed: number | null): number[] {
  let x = ((seed ?? 0) ^ 0x9e3779b9) >>> 0;
  if (!x) x = 1;
  const pick = (k: number): number => {
    if (k <= 1 || seed === null) return 0;
    x = (x ^ (x << 13)) >>> 0;
    x = (x ^ (x >>> 17)) >>> 0;
    x = (x ^ (x << 5)) >>> 0;
    return x % k;
  };
  const n = a.areas.length, used = new Array<boolean>(n).fill(false), dealt: number[] = [];
  const partner = (i: number) => {
    for (const [p, q] of a.opposite ?? []) { if (p === i) return q; if (q === i) return p; }
    return -1;
  };
  for (const step of a.pattern ?? []) {
    const opposite = step.endsWith(":opposite"), group = opposite ? step.slice(0, -":opposite".length) : step;
    const cand: number[] = [];
    for (let i = 0; i < n; i++) if (!used[i] && a.areas[i].group === group) cand.push(i);
    if (!cand.length) continue;
    let chosen = -1;
    if (opposite) for (const d of dealt) { const q = partner(d); if (q >= 0 && cand.includes(q)) { chosen = q; break; } }
    if (chosen < 0) chosen = cand[pick(cand.length)];
    used[chosen] = true;
    dealt.push(chosen);
  }
  const rest: number[] = [];
  for (let i = 0; i < n; i++) if (!used[i]) rest.push(i);
  while (rest.length) dealt.push(rest.splice(pick(rest.length), 1)[0]);
  const t = Math.max(0, teams), order = Array.from({ length: t }, (_, i) => i);
  if (seed !== null) for (let i = t - 1; i > 0; i--) { const j = pick(i + 1); [order[i], order[j]] = [order[j], order[i]]; }
  const out = new Array<number>(t).fill(-1);
  for (let j = 0; j < t; j++) out[order[j]] = j < dealt.length ? dealt[j] : -1;
  return out;
}

/** What `teams` teams get over `seeds` seeds: each distinct set of areas (names, sorted) and its share; the fixed deal. */
export function dealPreview(a: AreasSpec, teams: number, seeds = 1000) {
  const sets = new Map<string, number>();
  let s = 0x2545f491;
  for (let k = 0; k < seeds; k++) {
    s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
    const key = dealAreas(a, teams, s).map((i) => (i < 0 ? "(random spawns)" : a.areas[i].name)).sort().join(" + ");
    sets.set(key, (sets.get(key) ?? 0) + 1);
  }
  const fixed = dealAreas(a, teams, null).map((i) => (i < 0 ? "(random spawns)" : a.areas[i].name));
  return { teams, fixed, sets: [...sets.entries()].sort((p, q) => q[1] - p[1]).map(([areas, n]) => ({ areas, share: Math.round((n / seeds) * 1000) / 1000 })) };
}
