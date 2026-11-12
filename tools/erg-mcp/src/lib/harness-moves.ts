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

// ------------------------------------------------------------------ round 6: the rope's solved aim, the girder's steering

/** Where a worm's rope leaves it: over its feet (world or map units alike: `up` in the same unit; half a map unit). */
export const muzzleOf = (pos: number[], up: number): number[] => [pos[0], pos[1] + up, pos[2]];

/** The aim that points from `from` at `at`: the pitch (radians over the horizon, what the worm's WeaponAngle holds) and
 * the yaw (degrees, 0 toward +z, 90 toward +x), and the straight distance. */
export function aimAt(from: number[], at: number[]): { pitch: number; yaw: number | null; distance: number } {
  const dx = at[0] - from[0], dy = at[1] - from[1], dz = at[2] - from[2], h = Math.hypot(dx, dz);
  return { pitch: Math.atan2(dy, h), yaw: h < 1e-6 ? null : Math.atan2(dx, dz) * DEG, distance: Math.hypot(dx, dy, dz) };
}

/** The unit direction of a yaw (degrees) and a pitch (radians). */
export const aimDir = (yaw: number, pitch: number): number[] => [Math.sin(yaw / DEG) * Math.cos(pitch), Math.sin(pitch), Math.cos(yaw / DEG) * Math.cos(pitch)];

export interface FanHit { pitch: number; at: number[] | null; }   // a ray of the fan: its pitch (radians) and where it met land

/** The rope's hook for a ledge: of a fan of rays from the worm toward the target (one yaw, rising pitches, each ray as
 * long as the rope), the hit a worm let down from lands on the ledge: the land is met OVER the target's height by at
 * least `over`, no further than `reach` in plan from the target (a point over the ledge: the next riser's face, an
 * overhang), and at or beyond the ledge's lip as seen from the worm (`lipBack`: how far short of the target's own
 * distance a hook may be). The lowest such pitch is taken: the shortest swing. Null with the reason when no ray does. */
export function solveHook(muzzle: number[], target: number[], fan: FanHit[], o: { over: number; reach: number; lipBack: number }, body?: FanHit[]):
  { hit: FanHit; height: number; plan: number } | { hit: null; why: string } {
  const dT = Math.hypot(target[0] - muzzle[0], target[2] - muzzle[2]);
  let under = 0, short = 0, far = 0, miss = 0, dragged = 0;
  // `body`: the same fan from the worm's feet. The rope pulls the worm along its own line, and where the line from its
  // feet meets land short of the ledge the worm is dragged into the face under the lip and held there (a rope whose
  // own line just clears a lip did that in round 6: the worm rose to 0.7 under the ledge and no further)
  const bodyAt = new Map((body ?? []).map((b) => [Math.round(b.pitch * 1e4), b.at]));
  for (const f of [...fan].sort((a, b) => a.pitch - b.pitch)) {
    if (!f.at) { miss++; continue; }
    const d = Math.hypot(f.at[0] - muzzle[0], f.at[2] - muzzle[2]), plan = Math.hypot(f.at[0] - target[0], f.at[2] - target[2]);
    if (f.at[1] < target[1] + o.over) { under++; continue; }
    if (d < dT - o.lipBack) { short++; continue; }
    if (plan > o.reach) { far++; continue; }
    const b = bodyAt.get(Math.round(f.pitch * 1e4));
    if (b && Math.hypot(b[0] - muzzle[0], b[2] - muzzle[2]) < dT - o.lipBack) { dragged++; continue; }
    return { hit: f, height: f.at[1] - target[1], plan };
  }
  if (dragged) return { hit: null, why: `${dragged} ray${dragged === 1 ? "" : "s"} hook land over the ledge, but the line from the worm's feet along the same aim meets the face under the lip first: the rope would drag the worm into the face and hold it under the ledge. Stand further back from the riser (or lower), so the worm's own line clears the lip too` };
  const why = !fan.length ? "no ray" : short && !far ? `every ray that meets land over the ledge meets it short of the ledge (${short}: the riser's own face, under or at its lip): a hook there hangs the worm on the face, not over the ledge. Stand further back so the line clears the lip`
    : far ? `the land over the ledge is met too far past the target (${far} rays): aim at a nearer target or move closer`
      : under && miss ? `the rays that clear the ledge's lip meet nothing within the rope's length (${miss} pass over whatever stands behind it), and the others meet land under the ledge's height: from further back a lower aim clears the lip and meets the face behind the ledge; with nothing behind the ledge a rope has nothing to hold`
        : under === fan.length - miss && under ? "every ray meets land under the ledge's height: nothing stands over the ledge within the rope's length (a rope needs something above to hold)"
        : "no ray of the fan meets land over the ledge";
  return { hit: null, why };
}

/** The girder's two steering axes from two measured pulses: `f` and `r` are what a held Forward and a held Right moved
 * it in plan per millisecond ([x, z] each). Answers how long to hold each (negative: the opposite input) to move it by
 * `err` ([x, z]); null when the two pulses are parallel or too small to steer by. */
export function girderHold(err: number[], f: number[], r: number[]): { forwardMs: number; rightMs: number } | null {
  const det = f[0] * r[1] - f[1] * r[0];
  if (Math.abs(det) < 1e-12) return null;
  return { forwardMs: (err[0] * r[1] - err[1] * r[0]) / det, rightMs: (f[0] * err[1] - f[1] * err[0]) / det };
}

/** A hazard line by the land's box as the GAME reads it (Sandtide's rule): the base is the water when the land reaches
 * under it, else the land's lowest point; the line is base + fraction x (top - base). */
export function gameLine(landMinY: number, landMaxY: number, water: number | null, fraction: number): { line: number; base: number; baseKind: "water" | "landMin" } {
  const wet = water !== null && landMinY < water && water < landMaxY;
  const base = wet ? water! : landMinY;
  return { line: base + fraction * (landMaxY - base), base, baseKind: wet ? "water" : "landMin" };
}

export interface BandSurface { x: number; z: number; y: number; covered: boolean; }
/** Surfaces within `band` of a line, grouped on a grid of `cell`: each group's place, how many columns, its heights and
 * its nearest distance to the line (signed: over 0 above it), the nearest first. */
export function bandGroups(surf: BandSurface[], line: number, cell: number): { at: number[]; columns: number; minY: number; maxY: number; nearest: number; covered: number }[] {
  const m = new Map<string, { n: number; x: number; z: number; lo: number; hi: number; near: number; cov: number }>();
  for (const s of surf) {
    const key = `${Math.floor(s.x / cell)},${Math.floor(s.z / cell)}`;
    const g = m.get(key) ?? { n: 0, x: 0, z: 0, lo: Infinity, hi: -Infinity, near: Infinity, cov: 0 };
    g.n++; g.x += s.x; g.z += s.z; g.lo = Math.min(g.lo, s.y); g.hi = Math.max(g.hi, s.y); if (s.covered) g.cov++;
    if (Math.abs(s.y - line) < Math.abs(g.near)) g.near = s.y - line;
    m.set(key, g);
  }
  return [...m.values()].sort((a, b) => Math.abs(a.near) - Math.abs(b.near)).map((g) => ({ at: [g.x / g.n, g.z / g.n], columns: g.n, minY: g.lo, maxY: g.hi, nearest: g.near, covered: g.cov }));
}

// ------------------------------------------------------------------ round 6: the Versus menu route

/** The game's own prebuilt landscapes in the Versus landscape list (build #1077), which lists the five random themes,
 * then "Prebuilt" in alphabetical order with the map packs' levels among them, then the Worms 3D maps. */
export const STOCK_PREBUILT = ["Desert Oasis", "Riverboat Harbour", "This Game Does Dragon", "Wormwigs Big Rig Jig"];
export const RANDOM_THEMES = 5;

/** How many times DOWN is pressed from the top of the landscape list to reach `title`: the random themes, then its
 * place among the prebuilt titles (the game's and the installed packs') in alphabetical order. -1 when it is not there. */
export function landscapeIndex(title: string, packTitles: string[]): number {
  const all = [...new Set([...STOCK_PREBUILT, ...packTitles])].sort((a, b) => a.toLowerCase().localeCompare(b.toLowerCase()));
  const i = all.findIndex((t) => t.toLowerCase() === title.toLowerCase());
  return i < 0 ? -1 : RANDOM_THEMES + i;
}

export interface VersusTeam { human?: boolean; cpu?: "easy" | "normal" | "hard"; }

/** The key presses that take the game from its main menu into a Versus Deathmatch on a landscape (Automation's `key`
 * and `wait` commands). Every list is walked from its top (UP as often as it is long: the lists do not wrap), so the
 * route does not depend on where the highlight was left. `humanTeams`: how many player teams the save has (they are
 * listed first in the team list, and a chosen one leaves it; the CPU teams follow: Easy, Normal, Hard). */
export function versusRoute(teams: VersusTeam[], landscape: number, o: { humanTeams?: number; dismiss?: number } = {}): string[] {
  const out: string[] = [], key = (k: string, n = 1, wait = 280) => { for (let i = 0; i < n; i++) out.push(`key ${k}`, `wait ${wait}`); };
  let humansLeft = o.humanTeams ?? 4;
  for (let i = 0; i < (o.dismiss ?? 0); i++) { out.push("key RETURN", "wait 4000"); }   // a prompt in the way (the language, a notice)
  key("UP", 6); out.push("key RETURN", "wait 3000");                 // main menu: Local Game
  key("UP", 6); key("DOWN", 4); out.push("key RETURN", "wait 3500");  // Local Game: Versus (the fifth entry)
  key("UP", 6); out.push("key RETURN", "wait 4000");                 // the game type: Deathmatch (the first)
  teams.forEach((t, i) => {
    if (i) key("DOWN");                                              // the next team slot
    out.push("key RETURN", "wait 1500");                             // its list of teams
    key("UP", 12, 120);
    if (t.human === false) key("DOWN", humansLeft + ["easy", "normal", "hard"].indexOf(t.cpu ?? "normal"));
    else { if (humansLeft <= 0) throw new Error(`team ${i}: the save has no player team left (humanTeams)`); humansLeft--; }
    out.push("key RETURN", "wait 1500");
  });
  out.push("key LEFT", "wait 800", "key RETURN", "wait 3000");       // the landscape, and its list
  key("UP", 48, 90); key("DOWN", landscape); out.push("wait 400", "key RETURN", "wait 2500");
  out.push("key DOWN", "wait 400", "key DOWN", "wait 600", "key RIGHT", "wait 600", "key RETURN");   // the back arrow, the grenade beside it: start
  return out;
}

/** The key presses of a two-peer network match (LocalNet), each from the game's main menu (seen in the game, build
 * #1077): Network Game, the player's team from the Select Team list, then for the host Create Match (Deathmatch,
 * Standard, Max Players 2: the first of its list) into the lobby and the landscape picked there, for the joiner Custom
 * Match, the search and the first match found. The joiner's route is in two parts: joinBoot takes it to the Network Game
 * menu at once (a game left at its main menu starts its demo match after a minute), joinFind searches once the host's
 * lobby is up. `ready`: the grenade, which the joiner's highlight is on when it enters the lobby and the host reaches
 * from the landscape. */
export function localnetRoute(role: "host" | "joinBoot" | "joinFind" | "hostReady" | "joinReady", o: { team?: number; landscape?: number } = {}): string[] {
  const out: string[] = [], key = (k: string, n = 1, wait = 280) => { for (let i = 0; i < n; i++) out.push(`key ${k}`, `wait ${wait}`); };
  if (role === "joinReady") return ["key RETURN", "wait 1500"];
  if (role === "hostReady") { key("DOWN", 2, 400); out.push("key RIGHT", "wait 600", "key RETURN", "wait 1500"); return out; }
  if (role === "joinFind") { key("UP", 4); key("DOWN", 2); out.push("key RETURN", "wait 3500", "key RETURN", "wait 7000", "key RETURN", "wait 6000"); return out; }   // Custom Match, search, join the first
  key("UP", 6); key("DOWN"); out.push("key RETURN", "wait 5000");      // main menu: Network Game; the Select Team list takes a moment
  key("UP", 6, 120); key("DOWN", o.team ?? 0); out.push("key RETURN", "wait 5000");
  if (role === "host") {
    key("UP", 4);
    key("DOWN", 3); out.push("key RETURN", "wait 4000");               // Create Match
    key("UP", 4); key("DOWN", 2); out.push("key RETURN", "wait 1500", "key RETURN", "wait 1000");   // Max Players: 2
    key("DOWN", 2); out.push("key RETURN", "wait 6000");               // the tick: into the lobby, the highlight on the grenade
    out.push("key LEFT", "wait 400"); key("UP", 2, 400); out.push("key RETURN", "wait 3000");       // the landscape and its list
    key("UP", 48, 90); key("DOWN", o.landscape ?? 0); out.push("wait 400", "key RETURN", "wait 2500");
  }
  return out;
}

/** Worm positions as two peers read them ("slot:team:x,y,z" rows), compared: the worms that differ by more than `tol`. */
export function comparePositions(a: { slot: number; team: number; pos: number[] }[], b: { slot: number; team: number; pos: number[] }[], tol = 0.05) {
  const rows = a.map((w) => {
    const o = b.find((x) => x.slot === w.slot);
    const apart = o ? Math.hypot(w.pos[0] - o.pos[0], w.pos[1] - o.pos[1], w.pos[2] - o.pos[2]) : null;
    return { worm: w.slot, team: w.team, host: w.pos, join: o?.pos ?? null, apart, same: apart !== null && apart <= tol && o!.team === w.team };
  });
  return { agree: rows.length > 0 && rows.length === b.length && rows.every((r) => r.same), worms: rows.length, differ: rows.filter((r) => !r.same) };
}
