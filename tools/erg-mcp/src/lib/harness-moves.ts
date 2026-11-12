// Pure parts of the test-harness moves (tools/moves.ts): a worm's facing, the eye for a worm close-up, a riser test's
// verdict, a jetpack flight's inputs, the spawn deals of several peers' logs, and how two shots of one pose differ.
// Angles are degrees, 0 toward +z and 90 toward +x, as a worm's yaw is (its facing is (sin yaw, 0, cos yaw)).
import type { Rgb } from "./inspect-png";

const DEG = 180 / Math.PI;

/** a - b brought into -180..180. */
export const wrapDeg = (a: number) => ((((a + 180) % 360) + 360) % 360) - 180;

/** The yaw (degrees) that faces from `from` toward `to` ([x, z] or [x, y, z]); null when they are the same spot in plan. */
export function yawToward(from: number[], to: number[]): number | null {
  const dx = to[0] - from[0], dz = to[to.length - 1] - from[from.length - 1];
  return Math.hypot(dx, dz) < 1e-6 ? null : Math.atan2(dx, dz) * DEG;
}

/** The unit facing of a yaw in degrees: [x, z]. */
export const facingOf = (yawDeg: number): [number, number] => [Math.sin(yawDeg / DEG), Math.cos(yawDeg / DEG)];

/** The next turn pulse toward `want`: which way and for how long, from the error and the turn rate measured so far
 * (degrees per ms; unknown: 0). A pulse turns about three quarters of the error, 40-400 ms, so it does not overshoot. */
export function turnPulse(yawNow: number, want: number, rate: number, tolerance: number): { done: boolean; error: number; sign: 1 | -1; ms: number } {
  const error = wrapDeg(want - yawNow);
  if (Math.abs(error) <= tolerance) return { done: true, error, sign: 1, ms: 0 };
  const ms = rate > 1e-4 ? Math.max(40, Math.min(400, (0.75 * Math.abs(error)) / rate)) : 80;
  return { done: false, error, sign: error > 0 ? 1 : -1, ms: Math.round(ms) };
}

/** Eyes to try for a close-up of a point (a worm's head), nearest to the asked pose first: the asked bearing, then
 * turned round by 45 degrees each way; the asked distance, then nearer, then farther; the asked elevation, then higher.
 * Each is [x, y, z] in the target's units. The caller keeps the first whose sight line the game's land sweep leaves clear. */
export function eyeCandidates(target: number[], azimuth: number, elevation: number, distance: number): { eye: number[]; azimuth: number; elevation: number; distance: number }[] {
  const out: { eye: number[]; azimuth: number; elevation: number; distance: number }[] = [];
  for (const dEl of [0, 15, 30, 45])
    for (const f of [1, 0.7, 1.4, 0.5, 2])
      for (const dAz of [0, 45, -45, 90, -90, 135, -135, 180]) {
        const el = Math.min(85, elevation + dEl), az = azimuth + dAz, d = distance * f;
        const h = d * Math.cos(el / DEG);
        // the bearing is where the eye stands from the target, clockwise from north (-z), as the overview's azimuth
        out.push({ eye: [target[0] + h * Math.sin(az / DEG), target[1] + d * Math.sin(el / DEG), target[2] - h * Math.cos(az / DEG)], azimuth: ((az % 360) + 360) % 360, elevation: el, distance: d });
      }
  return out;
}

export interface RiserTry { kind: "walk" | "jump" | "backflip"; startY: number; maxY: number; endY: number; endAt: number[]; moved: number; toward?: number; yaw?: number; }
/** A riser test's verdict: the height each try reached and whether any ended standing on the riser's top (within
 * `slack` of it, default 0.3). Heights in the samples' units. */
export function riserVerdict(footY: number, topY: number, tries: RiserTry[], slack = 0.3) {
  const height = topY - footY;
  const list = tries.map((t) => ({ kind: t.kind, maxRise: Math.round((t.maxY - footY) * 100) / 100, endRise: Math.round((t.endY - footY) * 100) / 100, moved: Math.round(t.moved * 100) / 100,
    ...(t.toward !== undefined ? { toward: Math.round(t.toward * 100) / 100 } : {}), ...(t.yaw !== undefined ? { yaw: Math.round(t.yaw) } : {}), climbed: t.endY >= topY - slack }));
  const best = list.reduce((m, t) => Math.max(m, t.maxRise), 0);
  return { riserHeight: Math.round(height * 100) / 100, tries: list, climbed: list.some((t) => t.climbed), highestReached: Math.round(best * 100) / 100,
    shortBy: Math.round(Math.max(0, height - best) * 100) / 100 };
}

/** A jetpack flight's inputs for this instant. A jetpack only pushes while it thrusts (steering without thrust does
 * not move the worm along; what speed it has carries it on), so the flight is a lob: straight up until the worm is over
 * the target's height (a worm steered at a wall from its foot is held against it and burns its tank there), then thrust
 * and steering together until the speed it has would carry it to the target as it falls (`g`, the fall's acceleration),
 * then no thrust: it coasts over and drops. Thrust stops at the ceiling (`clear` + `band` over the target) whatever the
 * rest says. Positions [x, y, z] and the velocity `vel` in one unit and seconds; `arrive` is the plan distance that
 * counts as over the target. */
export function jetStep(pos: number[], vel: number[], target: number[], o: { clear: number; arrive: number; g: number; band?: number; maxFall?: number; climbed?: boolean }):
  { thrust: boolean; steer: [number, number] | null; over: boolean; plan: number; phase: "climb" | "push" | "coast" | "drop"; coastReach: number } {
  const dx = target[0] - pos[0], dz = target[2] - pos[2], plan = Math.hypot(dx, dz), over = plan <= o.arrive;
  const above = pos[1] - target[1];
  // over the target: fall, but brake a fall faster than maxFall within `clear` of the ground
  if (over) return { thrust: o.maxFall !== undefined && vel[1] < -o.maxFall && above < o.clear, steer: null, over, plan, phase: "drop", coastReach: 0 };
  const ux = dx / plan, uz = dz / plan;
  // how far the speed it has carries it toward the target before it is down at the target's height
  const disc = vel[1] * vel[1] + 2 * o.g * above;
  const tFall = disc > 0 && (above > 0 || vel[1] > 0) ? (vel[1] + Math.sqrt(disc)) / o.g : 0;
  const coastReach = Math.max(0, vel[0] * ux + vel[2] * uz) * tFall;
  if (!(o.climbed ?? false) && above < 0.5 * o.clear) return { thrust: true, steer: null, over, plan, phase: "climb", coastReach };
  if (coastReach >= plan - 0.5 * o.arrive) return { thrust: false, steer: [ux, uz], over, plan, phase: "coast", coastReach };
  return { thrust: above < o.clear + (o.band ?? o.clear), steer: [ux, uz], over, plan, phase: "push", coastReach };
}

export interface SpawnDeal { seed: string; teams: number; areas: number[]; line: number; }
/** The spawn-area deals a Melange.log holds, in order: `[levels] spawn areas: seed 1a2b3c4d, 3 teams -> areas [2,0,4]`. */
export function parseSpawnDeals(log: string): SpawnDeal[] {
  const out: SpawnDeal[] = [];
  log.split(/\r?\n/).forEach((l, i) => {
    const m = /spawn areas: seed ([0-9a-f]{8}), (\d+) teams -> areas \[([^\]]*)\]/i.exec(l);
    if (m) out.push({ seed: m[1].toLowerCase(), teams: Number(m[2]), areas: m[3].split(/[,\s]+/).filter(Boolean).map(Number), line: i + 1 });
  });
  return out;
}

/** The peers' last `last` deals side by side (match k counted from each log's end, so logs of different lengths line
 * up on the newest match): per match whether every peer has the same seed and the same areas. */
export function compareDeals(peers: { name: string; deals: SpawnDeal[] }[], last = 1) {
  const n = Math.min(last, ...peers.map((p) => p.deals.length));
  const matches = [];
  for (let k = n; k >= 1; k--) {
    const row = peers.map((p) => ({ peer: p.name, ...p.deals[p.deals.length - k] }));
    const seeds = new Set(row.map((r) => r.seed)), areas = new Set(row.map((r) => `${r.teams}:${r.areas.join(",")}`));
    matches.push({ fromEnd: k, agree: seeds.size === 1 && areas.size === 1, sameSeed: seeds.size === 1, sameAreas: areas.size === 1, peers: row.map((r) => ({ peer: r.peer, seed: r.seed, teams: r.teams, areas: r.areas, line: r.line })) });
  }
  return { compared: n, agree: n > 0 && matches.every((m) => m.agree), matches,
    ...(n === 0 ? { why: `a peer's log holds no spawn deal (${peers.filter((p) => !p.deals.length).map((p) => p.name).join(", ")}): the level has no spawn areas, or no match started` } : {}) };
}

/** How two pictures of one pose differ: the mean absolute difference per channel, the share of pixels that differ by more
 * than `threshold` (the sum over the channels, default 48), the box those pixels lie in, and a picture of them (the first
 * picture dimmed, the changed pixels in magenta). Both are w x h RGB. */
export function imageDiff(a: Uint8Array, b: Uint8Array, w: number, h: number, threshold = 48): { mean: Rgb; changed: number; box: { x0: number; y0: number; x1: number; y1: number } | null; rgb: Uint8Array } {
  const sum = [0, 0, 0], out = new Uint8Array(w * h * 3);
  let changed = 0, x0 = w, y0 = h, x1 = -1, y1 = -1;
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) {
      const i = (y * w + x) * 3;
      const d = [Math.abs(a[i] - b[i]), Math.abs(a[i + 1] - b[i + 1]), Math.abs(a[i + 2] - b[i + 2])];
      sum[0] += d[0]; sum[1] += d[1]; sum[2] += d[2];
      if (d[0] + d[1] + d[2] > threshold) {
        changed++;
        if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
        out[i] = 255; out[i + 1] = 0; out[i + 2] = 255;
      } else { out[i] = a[i] >> 2; out[i + 1] = a[i + 1] >> 2; out[i + 2] = a[i + 2] >> 2; }
    }
  const n = w * h || 1;
  return { mean: sum.map((v) => Math.round((v / n) * 10) / 10) as Rgb, changed: Math.round((changed / n) * 10000) / 10000, box: x1 < 0 ? null : { x0, y0, x1, y1 }, rgb: out };
}

/** Whether the level has been quiet long enough for a clean shot: no explosion seen for `quietMs`. `seen` is the probe's
 * count now, `last` the count and the time (ms) it last changed. Answers the state to carry on with. */
export function quietState(seen: number, last: { seen: number; at: number } | null, now: number, quietMs: number): { last: { seen: number; at: number }; quiet: boolean; waitMs: number } {
  const st = !last || seen !== last.seen ? { seen, at: last ? now : now - quietMs } : last;
  const since = now - st.at;
  return { last: st, quiet: since >= quietMs, waitMs: Math.max(0, quietMs - since) };
}
