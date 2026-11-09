// Pure helpers behind game_play_turns' run trace and game_perf's runs file (the Caldera v2 round-4 gaps).
//
// The trace: every worm's position about once a second through a game_play_turns run, read beside the walk's own reads,
// so a worm that loses height or moves while another team plays is explained. In the round-4 run WORM5 dropped from
// 55.4 to 44.2 between its turns 21 and 29 and no record said why. offTurnMoves finds such moves (the worm is not the
// active one, and between two samples it dropped `drop` or moved `moved` map units), joins the consecutive ones into
// one event each with the trace's samples and the speed, and attributeMove names what the Test probe saw beside it in
// those ticks: explosions (with their source and the weapon of the turn), mines that came to rest near it, its death.
//
// The perf runs file: game_perf kept its runs in the process, so a baseline run by one erg-mcp process could not be the
// `against` of the next one's run (round 4: "no earlier game_perf run labelled baseline15 (runs: none)"); the runs are
// written to Documents\Melange\erg-mcp\perf-runs.json and read back, and pickPerfBase chooses the run to compare with.
// Map units throughout.

export interface TraceWorm { slot: number; team: number; pos: [number, number, number]; alive: boolean; }
export interface TraceSample { wall: number; tick: number; turn: number | null; active: number | null; worms: TraceWorm[]; }

/** The trace reader's Lua: the sim tick, the game turn, the active worm's slot, then every worm (world units). */
export const TRACE_LUA = `local t = wum.game.inMatch() and wum.game.turn() or nil
local m = wum.game.match and wum.game.match() or nil
local o = {}
for _, w in ipairs(wum.game.worms() or {}) do o[#o + 1] = string.format("%s:%s:%.2f:%.2f:%.2f:%s", tostring(w.slot), tostring(w.team), w.pos.x, w.pos.y, w.pos.z, tostring(w.alive)) end
return tostring(wum.game.tick()) .. ";" .. tostring(m and m.turnsStarted or (t and t.index) or nil) .. ";" .. tostring(t and t.worm or nil) .. ";" .. table.concat(o, ";")`;

/** One TRACE_LUA answer ("tick;turn;active;slot:team:x:y:z:alive;...", world units) as a sample in map units; null when
 * it does not parse (no match). */
export function parseTrace(line: string, wall: number, worldPerMap = 20): TraceSample | null {
  const parts = line.split(";");
  if (parts.length < 3) return null;
  const tick = Number(parts[0]), num = (v: string) => (v === "nil" || v === "" ? null : Number(v));
  if (!Number.isFinite(tick)) return null;
  const worms: TraceWorm[] = [];
  for (const p of parts.slice(3)) {
    const v = p.split(":");
    if (v.length < 6) continue;
    const pos = [v[2], v[3], v[4]].map((x) => Math.round((Number(x) / worldPerMap) * 100) / 100) as [number, number, number];
    if (!pos.every(Number.isFinite)) continue;
    worms.push({ slot: Number(v[0]), team: Number(v[1]), pos, alive: v[5] === "true" });
  }
  return { wall, tick, turn: num(parts[1]), active: num(parts[2]), worms };
}

export interface OffTurnMove {
  worm: string; slot: number; team: number;
  turn: number | null;                      // the game turn it happened in (the first sample's)
  activeWorm: number | null;                // whose turn it was
  ticks: [number, number]; wall: [number, number]; seconds: number;
  from: [number, number, number]; to: [number, number, number];
  drop: number;                             // map units lost (negative: gained)
  moved: number;                            // horizontal map units
  fastestDrop: number;                      // map units a second, the steepest sample step down
  samples: { tick: number; pos: [number, number, number] }[];
  kind: "fall" | "blast" | "slide" | "move";
  explosions?: NearbyExplosion[]; mines?: NearbyMine[]; death?: { tick: number; turn: number; cause: string };
}
export interface NearbyExplosion { tick: number; turn: number; at: number[]; distance: number; source: string; turnWeapon?: string; turnWorm?: number; }
export interface NearbyMine { fireTick: number; fireTurn: number; rest: number[]; distance: number; }

const r2 = (v: number) => Math.round(v * 100) / 100;

/** The moves of worms whose turn it was not: between two consecutive samples a worm not active in either dropped
 * `drop` (default 1.5 map units) or moved `moved` (default 2) horizontally; consecutive such steps of one worm are one
 * event. A dead worm's samples are passed over (its death is the probe's), and a sample where the worm is missing ends
 * the event. */
export function offTurnMoves(samples: TraceSample[], o: { drop?: number; moved?: number } = {}): OffTurnMove[] {
  const dropAt = o.drop ?? 1.5, movedAt = o.moved ?? 2, out: OffTurnMove[] = [];
  const open = new Map<number, OffTurnMove>();
  for (let i = 1; i < samples.length; i++) {
    const a = samples[i - 1], b = samples[i], seen = new Set<number>();
    const dt = Math.max(0.05, (b.wall - a.wall) / 1000);
    for (const w of b.worms) {
      const p = a.worms.find((x) => x.slot === w.slot);
      if (!p || !p.alive || !w.alive) continue;
      if (a.active === w.slot || b.active === w.slot) continue;
      const dy = p.pos[1] - w.pos[1], dxz = Math.hypot(w.pos[0] - p.pos[0], w.pos[2] - p.pos[2]);
      if (Math.abs(dy) < dropAt && dxz < movedAt) continue;
      seen.add(w.slot);
      const ev = open.get(w.slot);
      if (ev) {
        ev.to = w.pos; ev.ticks[1] = b.tick; ev.wall[1] = b.wall;
        ev.samples.push({ tick: b.tick, pos: w.pos });
        ev.fastestDrop = Math.max(ev.fastestDrop, dy / dt);
      } else {
        const nv: OffTurnMove = { worm: `WORM${w.slot}`, slot: w.slot, team: w.team, turn: a.turn ?? b.turn, activeWorm: a.active ?? b.active, ticks: [a.tick, b.tick], wall: [a.wall, b.wall], seconds: 0,
          from: p.pos, to: w.pos, drop: 0, moved: 0, fastestDrop: dy / dt, samples: [{ tick: a.tick, pos: p.pos }, { tick: b.tick, pos: w.pos }], kind: "move" };
        open.set(w.slot, nv);
        out.push(nv);
      }
    }
    for (const slot of [...open.keys()]) if (!seen.has(slot)) open.delete(slot);
  }
  for (const ev of out) {
    ev.drop = r2(ev.from[1] - ev.to[1]);
    ev.moved = r2(Math.hypot(ev.to[0] - ev.from[0], ev.to[2] - ev.from[2]));
    ev.seconds = r2((ev.wall[1] - ev.wall[0]) / 1000);
    ev.fastestDrop = r2(ev.fastestDrop);
    ev.kind = ev.drop >= dropAt ? "fall" : ev.moved >= movedAt ? "slide" : "move";
  }
  return out;
}

export interface ProbeLike {
  explosions?: { tick: number; turn: number; at: number[]; source: string; turnWeapon?: string; turnWorm?: number }[];
  fires?: { tick: number; turn: number; mines: { rest?: number[] }[] }[];
  deaths?: { slot: number; tick: number; turn: number; cause: string }[];
}

/** What the probe saw beside a move: explosions within `radius` (default 10 map units) of its start or end whose tick
 * is within `tickSlack` (default 150 sim ticks) of the move's ticks, mines at rest within `mineRadius` (default 4) of it
 * from fires in that window, and the worm's own death in it. A move with an explosion is a blast. */
export function attributeMove(ev: OffTurnMove, probe: ProbeLike, o: { radius?: number; mineRadius?: number; tickSlack?: number } = {}): OffTurnMove {
  const R = o.radius ?? 10, MR = o.mineRadius ?? 4, slack = o.tickSlack ?? 150, [t0, t1] = ev.ticks;
  const near = (at: number[]) => Math.min(Math.hypot(at[0] - ev.from[0], at[1] - ev.from[1], at[2] - ev.from[2]), Math.hypot(at[0] - ev.to[0], at[1] - ev.to[1], at[2] - ev.to[2]));
  const inWindow = (tick: number) => tick >= t0 - slack && tick <= t1 + slack;
  const ex = (probe.explosions ?? []).filter((e) => inWindow(e.tick) && near(e.at) <= R)
    .map((e) => ({ tick: e.tick, turn: e.turn, at: e.at.map(r2), distance: r2(near(e.at)), source: e.source, ...(e.turnWeapon ? { turnWeapon: e.turnWeapon } : {}), ...(e.turnWorm !== undefined ? { turnWorm: e.turnWorm } : {}) }))
    .sort((a, b) => a.distance - b.distance);
  const mines: NearbyMine[] = [];
  for (const f of probe.fires ?? []) {
    if (!inWindow(f.tick) && !(f.tick < t0 && f.tick >= t0 - 20 * slack)) continue;
    for (const m of f.mines) if (m.rest && near(m.rest) <= MR) mines.push({ fireTick: f.tick, fireTurn: f.turn, rest: m.rest.map(r2), distance: r2(near(m.rest)) });
  }
  const death = (probe.deaths ?? []).find((d) => d.slot === ev.slot && inWindow(d.tick));
  const out: OffTurnMove = { ...ev, ...(ex.length ? { explosions: ex.slice(0, 8) } : {}), ...(mines.length ? { mines: mines.sort((a, b) => a.distance - b.distance).slice(0, 8) } : {}),
    ...(death ? { death: { tick: death.tick, turn: death.turn, cause: death.cause } } : {}) };
  if (ex.length) out.kind = "blast";
  return out;
}

/** One line of why, for the log. */
export function describeMove(m: OffTurnMove): string {
  const what = m.kind === "blast" ? `knocked by ${m.explosions![0].source}${m.explosions![0].turnWeapon ? ` (${m.explosions![0].turnWeapon}, WORM${m.explosions![0].turnWorm}'s turn)` : ""} ${m.explosions![0].distance} map units off`
    : m.kind === "fall" ? `fell ${m.drop} map units${m.mines?.length ? ` (a mine at rest ${m.mines[0].distance} off)` : ""}` : `moved ${m.moved} map units${m.drop ? ` and ${m.drop > 0 ? "lost" : "gained"} ${Math.abs(m.drop)}` : ""}`;
  return `${m.worm} (team ${m.team}) ${what} in game turn ${m.turn ?? "?"} while WORM${m.activeWorm ?? "?"} played, over ${m.seconds} s from [${m.from.join(", ")}] to [${m.to.join(", ")}]${m.death ? `; it died (${m.death.cause})` : ""}`;
}

// ------------------------------------------------------------------ perf runs

export type PerfStat = { p50: number | null; p95: number | null; n: number } | null;
export interface PerfRun {
  label: string; scene: string; seconds: number; samples: number; startedAt: number;
  fps: PerfStat; busyMs: PerfStat; frameMs: PerfStat; gpuFrameMs: PerfStat; gpuWorldMs?: PerfStat; gpuPostWorldMs?: PerfStat; gpuFinalMs?: PerfStat;
  game?: string;                            // the game's build it ran in, so runs of one game pair up
  from?: "saved";                           // read back from the runs file (another erg-mcp process's run)
}

/** The runs of memory and of the runs file together (one per label and start, memory's first), oldest first, the last
 * `keep`. */
export function mergePerfRuns(mem: PerfRun[], saved: PerfRun[], keep = 24): PerfRun[] {
  const key = (r: PerfRun) => `${r.label}@${r.startedAt}`, seen = new Set(mem.map(key));
  return [...saved.filter((r) => !seen.has(key(r))).map((r) => ({ ...r, from: "saved" as const })), ...mem].sort((a, b) => a.startedAt - b.startedAt).slice(-keep);
}

/** The run to compare with: `against` names a label, found among this session's runs first (the latest), else the runs
 * file's of the same game (the latest), else the file's of any game (said in `note`); without `against`, the session's
 * first run (when the new run is not it), else the file's latest run of the same game whose label starts with
 * "baseline". null with nothing to compare. */
export function pickPerfBase(session: PerfRun[], saved: PerfRun[], against: string | undefined, game: string | undefined): { base: PerfRun | null; note?: string } {
  const latest = (rs: PerfRun[]) => (rs.length ? rs.reduce((a, b) => (b.startedAt >= a.startedAt ? b : a)) : null);
  const sameGame = (r: PerfRun) => !game || !r.game || r.game === game;
  if (against) {
    const mine = latest(session.filter((r) => r.label === against));
    if (mine) return { base: mine };
    const file = saved.filter((r) => r.label === against);
    const same = latest(file.filter(sameGame));
    if (same) return { base: { ...same, from: "saved" }, note: `${against} is an earlier erg-mcp process's run (the runs file), ${new Date(same.startedAt).toISOString()}, of the same game` };
    const any = latest(file);
    if (any) return { base: { ...any, from: "saved" }, note: `${against} is an earlier erg-mcp process's run of another game build (${any.game ?? "unknown"}, ${new Date(any.startedAt).toISOString()}): GPU times of different builds` };
    return { base: null };
  }
  if (session.length) return { base: session[0] };
  const base = latest(saved.filter((r) => sameGame(r) && /^baseline/i.test(r.label)));
  return base ? { base: { ...base, from: "saved" }, note: `compared with the runs file's latest baseline of this game, ${base.label} (${new Date(base.startedAt).toISOString()}): pass against to choose another` } : { base: null };
}
