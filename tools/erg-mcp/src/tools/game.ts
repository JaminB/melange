// Tool group "game": looking at a Test while it plays, through the running game's own Oasis methods. game_state reads
// the match (turn, worms, water), game_wait waits for a turn or a time, game_camera points the drawn camera at a place
// (wum.render.cameraOverride: the picture only, the game's camera and the match are untouched). They run Lua in the
// game's client console (lua.eval, target client), so they need the game running with Oasis writable, offline (the
// console refuses online matches unless [LuaConsole] MatchConsoleOnline=1). A game with render.camera and render.shot
// (Melange's own Oasis methods) points the camera with no client mod, and game_camera {shot} and game_shots read the
// picture back (clean: without the mods' wum.draw text).
import { areaFirstKnot, type SpawnArea } from "../core";
import { appendFileSync, existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { extentXZ, markers, spansAt } from "../lib/inspect-field";
import { grids } from "../lib/voxel";
import { pieces } from "../lib/islets";
import { cumulative, keyOfLabel, planWalk, pursue } from "../lib/walk-plan";
import { WORM_WIDTH } from "../lib/reach";
import { clearEye } from "../lib/eye";
import { Raster, decodePng, hex, type Rgb } from "../lib/inspect-png";
import { centreColour, lookDelta, materialPatches } from "../lib/material-look";
import { projectMaterials } from "../lib/materials";
import { TRACE_LUA, attributeMove, cleanTrace, describeMove, mergePerfRuns, offTurnMoves, parseTrace, pickPerfBase, type PerfRun, type PerfStat, type TraceSample } from "../lib/play-trace";
import { documentsDir } from "../oasis";
import { ToolError, image, type Content, type Tool } from "../registry";
import type { Session } from "../session";

const WORLD_PER_MAP = 20;

// A lua.eval whose answer is lost is sent once more: in the Caldera v2 round-4 run one of about 1500 calls timed out
// (turn 74) and the turn was left without a record. luaRetries counts them for the answers.
let luaRetries = 0;
export const luaRetryCount = () => luaRetries;
async function evalClient(s: Session, code: string, mod?: string): Promise<string> {
  const conn = await s.ensure();
  if (!conn.methods.includes("lua.eval")) throw new ToolError("no lua.eval here: the game tools need the game running (Melange.exe --serve has no match)");
  const client = await s.client(), params = mod ? { target: "mod", mod, code } : { target: "client", code };
  let r: { ok: boolean; text: string };
  try {
    r = await client.call<{ ok: boolean; text: string }>("lua.eval", params, 15000);
  } catch (e) {
    if (!/no answer within/i.test((e as Error).message)) throw e;
    luaRetries++;
    r = await client.call<{ ok: boolean; text: string }>("lua.eval", params, 15000);
  }
  if (!r.ok) throw new ToolError(`the game's console refused: ${r.text}`);
  return r.text ?? "";
}

// One line of key=value pairs from the client API: no JSON library is needed on the Lua side.
const STATE_LUA = `
local out = {}
local function put(k, v) out[#out + 1] = k .. "=" .. tostring(v) end
put("scene", wum.game.scene())
local t = wum.game.inMatch() and wum.game.turn() or nil
if t then put("turn", t.index); put("team", t.team); put("worm", t.worm); put("phase", t.phase); put("ai", t.ai) end
local m = wum.game.match and wum.game.match() or nil
if m then put("water", m.waterLevel); put("turnMsLeft", m.turnMsLeft); put("roundMsLeft", m.roundMsLeft); put("suddenDeath", m.suddenDeath); put("turnsStarted", m.turnsStarted) end
local alive, total = {}, 0
for _, w in ipairs(wum.game.worms() or {}) do
  total = total + 1
  if w.alive then alive[w.team] = (alive[w.team] or 0) + 1 end
end
local teams = {}
for team, n in pairs(alive) do teams[#teams + 1] = team .. ":" .. n end
table.sort(teams)
put("worms", total); put("aliveByTeam", table.concat(teams, ","))
put("tick", wum.game.tick())
return table.concat(out, ";")`;

interface GameState { scene: string; turn?: number; team?: number; worm?: number; phase?: string; ai?: boolean; water?: number; turnMsLeft?: number;
  roundMsLeft?: number; suddenDeath?: boolean; worms: number; aliveByTeam: Record<string, number>; tick: number; clientTurnIndex?: number; }

// The turn: game turns count from 1, the match's n-th GameLogic.Turn.Started, as wum.sim.turnStarted's turn, a
// waterSchedule's startTurn and check_flood's game turns do. wum.game.match().turnsStarted counts them (rebased when the
// match's VM appears, so a first turn that starts in that frame is counted); wum.game.turn().index lost that turn before
// Melange 0.13 (one behind sim.turnStarted all match: the Caldera iteration-7 critique's "turn 5 = game_state turn 4"), so
// game_state takes turnsStarted when the game has it and answers the client's own index beside it.
const TURN_NOTE = "turn counts from 1 (the match's n-th turn start): the same number as wum.sim.turnStarted's turn, a waterSchedule's startTurn and check_flood's game turns; worm is the active worm's slot (WORMn)";

// The turns game_state and game_wait have seen in this match (game_wait looks every second), so the order the game
// moved the worms in can be given to check_flood as turnOrder. A new match (the turn or the tick going back) starts over.
const turnLog: { turn: number; team?: number; worm?: number }[] = [];
let lastTick = -1;
function noteTurn(st: GameState) {
  if (st.scene !== "match" || st.turn === undefined) return;
  const last = turnLog[turnLog.length - 1];
  if (last && (st.turn < last.turn || st.tick < lastTick)) turnLog.length = 0;
  lastTick = st.tick;
  const top = turnLog[turnLog.length - 1];
  if (!top || top.turn !== st.turn) turnLog.push({ turn: st.turn, team: st.team, worm: st.worm });
  else if (top.worm === undefined && st.worm !== undefined) { top.team = st.team; top.worm = st.worm; }
}
function turnLogOut() {
  if (!turnLog.length) return undefined;
  const fromOne = turnLog[0].turn === 1 && turnLog.every((x, i) => x.turn === i + 1 && x.worm !== undefined);
  return { seen: turnLog.map((x) => ({ turn: x.turn, ...(x.team !== undefined ? { team: x.team } : {}), ...(x.worm !== undefined ? { worm: `WORM${x.worm}` } : {}) })),
    ...(fromOne ? { turnOrder: turnLog.map((x) => x.worm!) } : {}),
    note: fromOne ? "turnOrder: the worms in the order seen from game turn 1, for check_flood's turnOrder" : "the turns game_state and game_wait saw in this match (a gap: a turn they did not see; turnOrder needs every turn from 1: call game_wait from the start of the match)" };
}

async function gameState(s: Session): Promise<GameState> {
  const text = await evalClient(s, STATE_LUA);
  const kv = new Map(text.split(";").map((p) => { const i = p.indexOf("="); return [p.slice(0, i), p.slice(i + 1)] as [string, string]; }));
  const num = (k: string) => (kv.has(k) && kv.get(k) !== "nil" ? Number(kv.get(k)) : undefined);
  const alive: Record<string, number> = {};
  for (const p of (kv.get("aliveByTeam") ?? "").split(",").filter(Boolean)) { const [t, n] = p.split(":"); alive[t] = Number(n); }
  return {
    scene: kv.get("scene") ?? "?", turn: num("turnsStarted") ?? num("turn"), clientTurnIndex: num("turn"), team: num("team"), worm: num("worm"), phase: kv.get("phase"), ai: kv.has("ai") ? kv.get("ai") === "true" : undefined,
    water: num("water"), turnMsLeft: num("turnMsLeft"), roundMsLeft: num("roundMsLeft"), suddenDeath: kv.has("suddenDeath") ? kv.get("suddenDeath") === "true" : undefined,
    worms: num("worms") ?? 0, aliveByTeam: alive, tick: num("tick") ?? 0,
  };
}

/** A camera override belongs to a mod: the one named, else the first enabled mod with client code. */
async function cameraMod(s: Session, want?: string): Promise<string> {
  if (want) return want;
  const mods = await (await s.client()).call<{ id: string; on: boolean; state: string; hasClient?: boolean }[]>("mods.list", {}, 15000).catch(() => []);
  const m = mods.find((x) => x.on && x.state === "enabled" && x.hasClient);
  if (!m) throw new ToolError("a camera override needs an enabled mod with client code to own it (none is enabled): enable one, or pass mod");
  return m.id;
}

const has = async (s: Session, method: string) => (await s.ensure()).methods.includes(method);

/** Sudden death off for the rest of the Test: the round clock held (level.test.roundTime {hold: true}: paused now and
 * at every turn start), and the round time left before and a second after, to show it stopped. */
export async function holdRoundClock(s: Session): Promise<{ held: boolean; roundMsLeft?: [number | null, number | null]; note: string }> {
  if (!(await has(s, "level.test.roundTime"))) throw new ToolError("suddenDeath: false holds the round clock with level.test.roundTime (this branch's Melange): this game has none");
  const left = async () => { try { const v = Number(await evalClient(s, "local m = wum.game.match and wum.game.match() return m and m.roundMsLeft or nil")); return Number.isFinite(v) ? v : null; } catch { return null; } };
  const before = await left();
  await (await s.client()).call("level.test.roundTime", { hold: true }, 15000).catch((e: Error) => { throw new ToolError(`level.test.roundTime refused: ${e.message}`); });
  await sleep(1500);
  const a = await left();
  await sleep(1500);
  const b = await left();
  return { held: true, roundMsLeft: [before, b], note: a !== null && b !== null && a === b ? "the round clock stands still (read 1.5 s apart): no sudden death in this Test" : "the round clock was asked to hold; it read " + JSON.stringify([before, a, b]) + " (ms left)" };
}
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

/** The overview preset's camera: azimuth (degrees clockwise from north, -z; default 180, from the south), elevation
 * (degrees down; default 50), distance from the level's middle (map units; default 0.95 x the level's span), and what it
 * frames (`fit`): the frames' boxes (the default, as before), the solid voxels, or the island (the solid voxels over the
 * water), each `margin` times as far as the closest pose that shows all of it in a 60 degree, 16:9 view. */
interface Orbit { azimuth?: number; elevation?: number; distance?: number; fit?: "frames" | "solid" | "island"; margin?: number; }
/** set_overview's framing per project id: the overview's defaults for the rest of this erg-mcp session. */
const overviewDefaults = new Map<string, Orbit>();
interface Cam extends Orbit { pos?: number[]; target?: number[]; fov?: number; preset?: "default" | "overview" | "castle"; name?: string; at?: number[]; radius?: number; team?: number; clearView?: boolean; }

const orbitProps = {
  azimuth: { type: "number", minimum: -360, maximum: 360, description: "preset overview: the side it looks from, degrees clockwise from north (-z); default 180 (from the south)." },
  elevation: { type: "number", minimum: 5, maximum: 89, description: "preset overview: degrees it looks down (default 50)." },
  distance: { type: "number", minimum: 1, description: "preset overview: distance from the level's middle in `units` (default 0.95 x the level's span; with fit solid or island, the closest that shows it all)." },
  fit: { type: "string", enum: ["frames", "solid", "island"], description: "preset overview: what it frames: frames (default: every voxel frame's box, a wiped base's empty frames too), solid (the solid voxels) or island (the main landmass over the water, as check_islets finds it: a stray pole the base left far off is not framed), centred on it and as close as shows all of it (times margin). set_overview makes one the default." },
  margin: { type: "number", minimum: 1, maximum: 4, description: "preset overview with fit solid or island: this many times the closest distance that shows it all (default 1.1)." },
} as const;

/** A pose (world units) in the call's units. */
const poseOut = (c: { pos: number[]; target: number[]; fov: number }, k: number) => ({
  pos: c.pos.map((v) => Math.round((v / k) * 100) / 100), target: c.target.map((v) => Math.round((v / k) * 100) / 100), fov: c.fov });

/** The box (map units) of the level's solid voxels, those over `above` only when it is given, or null for none. */
function solidBox(s: Session, above: number | null): { min: number[]; max: number[] } | null {
  const min = [Infinity, Infinity, Infinity], max = [-Infinity, -Infinity, -Infinity];
  for (const g of grids(s.store)) {
    const [X, Y] = g.size, m = g.toMap;
    for (let i = 0; i < g.words.length; i++) {
      if ((g.words[i] & 3) !== 3) continue;
      const y = i % Y, r = (i - y) / Y, x = r % X, z = (r - x) / X;
      const cx = x + 0.5, cy = y + 0.5, cz = z + 0.5;
      const p = [m[0] * cx + m[1] * cy + m[2] * cz + m[3], m[4] * cx + m[5] * cy + m[6] * cz + m[7], m[8] * cx + m[9] * cy + m[10] * cz + m[11]];
      if (above !== null && p[1] <= above) continue;
      for (let k = 0; k < 3; k++) { min[k] = Math.min(min[k], p[k] - 0.5); max[k] = Math.max(max[k], p[k] + 0.5); }
    }
  }
  return min[0] === Infinity ? null : { min, max };
}

/** The island's box (map units): the largest connected piece of terrain (check_islets' main landmass) over the water,
 * so a stray pole or sign the base left standing far off does not pull the overview back. Null without solid voxels
 * over the water. */
function islandBox(s: Session, water: number): { min: number[]; max: number[] } | null {
  const all = solidBox(s, water);
  if (!all) return null;
  const gs = grids(s.store), r = { x0: all.min[0], z0: all.min[2], x1: all.max[0], z1: all.max[2] };
  const step = Math.max(1, Math.max(r.x1 - r.x0, r.z1 - r.z0) / 500);
  const main = pieces(gs, r, step).find((p) => p.top > water);
  return main ? { min: [main.min[0], Math.max(main.min[1], water), main.min[2]], max: [main.max[0], main.max[1], main.max[2]] } : all;
}

/** The distance along (az, el) from the box's centre at which every corner of it is inside a `fov` degree view of
 * `aspect`, times margin. Map units. */
export function fitDistance(box: { min: number[]; max: number[] }, az: number, el: number, fov = 60, aspect = 16 / 9, margin = 1.1): number {
  const c = [0, 1, 2].map((k) => (box.min[k] + box.max[k]) / 2);
  const dir = [Math.cos(el) * Math.sin(az), Math.sin(el), -Math.cos(el) * Math.cos(az)];   // from the centre to the camera
  const f = dir.map((v) => -v), right = [-f[2], 0, f[0]], rl = Math.hypot(right[0], right[2]) || 1;
  const rx = right.map((v) => v / rl), up = [rx[1] * f[2] - rx[2] * f[1], rx[2] * f[0] - rx[0] * f[2], rx[0] * f[1] - rx[1] * f[0]];
  const tv = Math.tan((fov * Math.PI) / 360), th = tv * aspect;
  const corners: number[][] = [];
  for (let i = 0; i < 8; i++) corners.push([i & 1 ? box.max[0] : box.min[0], i & 2 ? box.max[1] : box.min[1], i & 4 ? box.max[2] : box.min[2]]);
  const fits = (d: number) => corners.every((p) => {
    const v = [p[0] - (c[0] + dir[0] * d), p[1] - (c[1] + dir[1] * d), p[2] - (c[2] + dir[2] * d)];
    const z = v[0] * f[0] + v[1] * f[1] + v[2] * f[2];
    return z > 0.1 && Math.abs(v[0] * rx[0] + v[1] * rx[1] + v[2] * rx[2]) <= th * z && Math.abs(v[0] * up[0] + v[1] * up[1] + v[2] * up[2]) <= tv * z;
  });
  let lo = 0.1, hi = 1;
  while (!fits(hi) && hi < 1e6) hi *= 2;
  for (let k = 0; k < 40; k++) { const mid = (lo + hi) / 2; if (fits(mid)) hi = mid; else lo = mid; }
  return hi * margin;
}

/** The level's own overview: above its frames' middle, looking down at 50 degrees from the south (or `o`'s), far
 * enough back for the whole extent in a 60 degree view. World units. Needs the project open (its frames). */
function overview(s: Session, o0: Orbit = {}, k = WORLD_PER_MAP): { pos: number[]; target: number[]; fov: number } {
  const given = Object.fromEntries(Object.entries({ azimuth: o0.azimuth, elevation: o0.elevation, distance: o0.distance, fit: o0.fit, margin: o0.margin }).filter(([, v]) => v !== undefined));
  // set_overview's defaults keep their distance in map units; a call's own is in its units
  const def = s.project ? overviewDefaults.get(s.project.id) : undefined;
  const o: Orbit = { ...def, ...given, ...(given.distance === undefined && def?.distance !== undefined ? { distance: (def.distance * WORLD_PER_MAP) / k } : {}) };
  const gs = grids(s.store), e = extentXZ(gs);
  if (!e) throw new ToolError("preset overview needs the level's frames: open_project the level being tested");
  if (o.fit === "solid" || o.fit === "island") {
    const water = s.store.scene.water.level === null ? null : s.store.scene.water.level / WORLD_PER_MAP;
    const box = o.fit === "island" ? islandBox(s, water ?? 0) : solidBox(s, null);
    if (!box) throw new ToolError(`preset overview fit ${o.fit}: no solid voxels${o.fit === "island" ? " over the water" : ""}`);
    const el = ((o.elevation ?? 50) * Math.PI) / 180, az = ((o.azimuth ?? 180) * Math.PI) / 180;
    const d = o.distance !== undefined ? (o.distance * k) / WORLD_PER_MAP : fitDistance(box, az, el, 60, 16 / 9, o.margin ?? 1.1);
    const c = [0, 1, 2].map((i) => (box.min[i] + box.max[i]) / 2);
    return { pos: [(c[0] + d * Math.cos(el) * Math.sin(az)) * WORLD_PER_MAP, (c[1] + d * Math.sin(el)) * WORLD_PER_MAP, (c[2] - d * Math.cos(el) * Math.cos(az)) * WORLD_PER_MAP],
      target: c.map((v) => v * WORLD_PER_MAP), fov: 60 };
  }
  let top = -Infinity, bottom = Infinity;
  for (const g of gs) { top = Math.max(top, g.bounds.max[1]); bottom = Math.min(bottom, g.bounds.min[1]); }
  const cx = (e.x0 + e.x1) / 2, cz = (e.z0 + e.z1) / 2, cy = Math.max(0, (Math.min(top, 200) + Math.max(bottom, -50)) / 3);
  const span = Math.max(e.x1 - e.x0, e.z1 - e.z0), d = o.distance !== undefined ? (o.distance * k) / WORLD_PER_MAP : span * 0.95;
  const el = ((o.elevation ?? 50) * Math.PI) / 180, az = ((o.azimuth ?? 180) * Math.PI) / 180;
  const h = d * Math.cos(el);
  return { pos: [(cx + h * Math.sin(az)) * WORLD_PER_MAP, (cy + d * Math.sin(el)) * WORLD_PER_MAP, (cz - h * Math.cos(az)) * WORLD_PER_MAP], target: [cx * WORLD_PER_MAP, cy * WORLD_PER_MAP, cz * WORLD_PER_MAP], fov: 60 };
}

// game_perf's runs of this erg-mcp session, to compare a level with its baseline.
// game_perf's runs: this process's, and the runs file (Documents\Melange\erg-mcp\perf-runs.json, the last 24 with the
// game build each ran in), so a baseline run by one erg-mcp process is the `against` of the next one's run (Caldera v2
// round 4: the baseline and the Erg run were two processes and the compare answered "runs: none").
const perfRuns: PerfRun[] = [];
const perfFile = () => { const d = documentsDir(); return d ? join(d, "Melange", "erg-mcp", "perf-runs.json") : null; };
function savedPerfRuns(): PerfRun[] {
  const f = perfFile();
  if (!f || !existsSync(f)) return [];
  try {
    const j = JSON.parse(readFileSync(f, "utf8")) as { runs?: PerfRun[] };
    return Array.isArray(j.runs) ? j.runs.filter((r) => r && typeof r.label === "string" && typeof r.startedAt === "number") : [];
  } catch {
    return [];
  }
}
function savePerfRuns() {
  const f = perfFile();
  if (!f) return;
  try {
    mkdirSync(join(f, ".."), { recursive: true });
    const runs = mergePerfRuns(perfRuns, savedPerfRuns()).map(({ from: _from, ...r }) => r);
    writeFileSync(f, JSON.stringify({ note: "erg-mcp's game_perf runs (fps, CPU and GPU frame times), for `against` across erg-mcp processes", runs }));
  } catch {
    // the compare then covers this process's runs only
  }
}

// game_play_turns' trace file: every worm's position about once a second through the run (lib/play-trace.ts), beside the
// pictures, in Documents\Melange\erg-mcp\traces\<prefix>.json.
function traceFile(prefix: string): string | null {
  const d = documentsDir();
  return d ? join(d, "Melange", "erg-mcp", "traces", `${prefix.replace(/[^A-Za-z0-9_.-]/g, "_")}.json`) : null;
}

// ------------------------------------------------------------------ poses from the worms

/** Every worm in the match (world units): slot, team, alive, position. */
const WORMS_LUA = `local o = {}
for _, w in ipairs(wum.game.worms() or {}) do o[#o + 1] = string.format("%s,%s,%s,%.2f,%.2f,%.2f", tostring(w.slot), tostring(w.team), tostring(w.alive), w.pos.x, w.pos.y, w.pos.z) end
return table.concat(o, ";")`;
type WormAt = { slot: number; team: number; alive: boolean; pos: number[] };
async function wormsNow(s: Session): Promise<WormAt[]> {
  const t = await evalClient(s, WORMS_LUA);
  return t.split(";").filter(Boolean).map((r) => { const v = r.split(","); return { slot: +v[0], team: +v[1], alive: v[2] === "true", pos: [+v[3], +v[4], +v[5]] }; })
    .filter((w) => w.pos.every(Number.isFinite));
}

/** A pose (world units) along (azimuth, elevation) from the box's centre (map units) that shows all of it. */
function poseOnBox(box: { min: number[]; max: number[] }, azDeg: number, elDeg: number, margin: number): { pos: number[]; target: number[]; fov: number } {
  const el = (elDeg * Math.PI) / 180, az = (azDeg * Math.PI) / 180, d = fitDistance(box, az, el, 60, 16 / 9, margin);
  const c = [0, 1, 2].map((i) => (box.min[i] + box.max[i]) / 2);
  return { pos: [(c[0] + d * Math.cos(el) * Math.sin(az)) * WORLD_PER_MAP, (c[1] + d * Math.sin(el)) * WORLD_PER_MAP, (c[2] - d * Math.cos(el) * Math.cos(az)) * WORLD_PER_MAP],
    target: c.map((v) => v * WORLD_PER_MAP), fov: 60 };
}

/** The overview preset's pose: the open project's (overview), or with none open (a baseline: test {map, noLevel}) the
 * box of the worms in the match padded by 25 map units, which is where the game is played. */
async function overviewOf(s: Session, o: Orbit, k: number): Promise<{ pos: number[]; target: number[]; fov: number; from?: string }> {
  if (s.project) return overview(s, o, k);
  const ws = (await wormsNow(s).catch(() => [])).filter((w) => w.alive);
  if (!ws.length) throw new ToolError("preset overview with no project open frames the match's worms, and none was found: open_project the tested level, or give pos and target");
  const min = [0, 1, 2].map((i) => Math.min(...ws.map((w) => w.pos[i] / WORLD_PER_MAP)) - (i === 1 ? 5 : 25)), max = [0, 1, 2].map((i) => Math.max(...ws.map((w) => w.pos[i] / WORLD_PER_MAP)) + (i === 1 ? 10 : 25));
  return { ...poseOnBox({ min, max }, o.azimuth ?? 180, o.elevation ?? 50, o.margin ?? 1.1), from: `no project is open: the box of the ${ws.length} worms in the match, padded 25 map units` };
}

/** The castle preset's pose: a sphere of `radius` round `at` (map units) and the worms of `team` (or every worm within
 * 1.5 radii), seen from `azimuth` (default from outside the island: away from x = z = 0) and `elevation` (default 55). */
function castlePose(at: number[], radius: number, worms: WormAt[], team: number | undefined, az?: number, el?: number) {
  const mine = worms.filter((w) => w.alive && (team !== undefined ? w.team === team : Math.hypot(w.pos[0] / WORLD_PER_MAP - at[0], w.pos[2] / WORLD_PER_MAP - at[2]) <= radius * 1.5));
  const pts = [...mine.map((w) => w.pos.map((v) => v / WORLD_PER_MAP)), [at[0] - radius, at[1] - 1, at[2] - radius], [at[0] + radius, at[1] + radius * 0.5, at[2] + radius]];
  const box = { min: [0, 1, 2].map((i) => Math.min(...pts.map((p) => p[i]))), max: [0, 1, 2].map((i) => Math.max(...pts.map((p) => p[i]))) };
  const azDeg = az ?? (Math.hypot(at[0], at[2]) > 1 ? (Math.atan2(at[0], -at[2]) * 180) / Math.PI : 180);
  return { pose: poseOnBox(box, azDeg, el ?? 55, 1.12), worms: mine.map((w) => `WORM${w.slot}`) };
}

const TEAM_RGB: Rgb[] = [[235, 64, 52], [66, 133, 244], [52, 199, 89], [250, 204, 21]];

/** Where world point q falls in a w x h picture from a pose (world units, vertical fov), or null behind the camera. */
export function project(pose: { pos: number[]; target: number[]; fov: number }, q: number[], w: number, h: number): [number, number] | null {
  const f0 = [0, 1, 2].map((i) => pose.target[i] - pose.pos[i]), fl = Math.hypot(...f0), f = f0.map((v) => v / fl);
  const r0 = [-f[2], 0, f[0]], rl = Math.hypot(r0[0], r0[2]) || 1, r = r0.map((v) => v / rl);
  const u = [r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]];
  const v = [0, 1, 2].map((i) => q[i] - pose.pos[i]), z = v[0] * f[0] + v[1] * f[1] + v[2] * f[2];
  if (z <= 0.1) return null;
  const tv = Math.tan((pose.fov * Math.PI) / 360), th = tv * (w / h);
  const x = (v[0] * r[0] + v[1] * r[1] + v[2] * r[2]) / (th * z), y = (v[0] * u[0] + v[1] * u[1] + v[2] * u[2]) / (tv * z);
  return [((x + 1) / 2) * w, ((1 - y) / 2) * h];
}

/** The shot with each living worm ringed in its team's colour and its slot number over it: <shot>_labels.png. */
function labelShot(path: string, png: Uint8Array, pose: { pos: number[]; target: number[]; fov: number }, worms: WormAt[]) {
  const pic = Raster.of(decodePng(png)), list: object[] = [];
  for (const wm of worms.filter((x) => x.alive)) {
    const c = project(pose, [wm.pos[0], wm.pos[1] + 0.6 * WORLD_PER_MAP, wm.pos[2]], pic.w, pic.h);
    const inView = !!c && c[0] >= 0 && c[1] >= 0 && c[0] < pic.w && c[1] < pic.h;
    list.push({ worm: `WORM${wm.slot}`, team: wm.team, ...(c ? { px: c.map(Math.round) } : {}), inView });
    if (!inView) continue;
    const col = TEAM_RGB[wm.team % 4], [cx, cy] = c!.map(Math.round);
    pic.ring(cx, cy, 13, col, 2);
    const txt = String(wm.slot), tw = txt.length * 16 - 4;
    pic.rect(cx - tw / 2 - 3, cy - 46, tw + 6, 26, [0, 0, 0]);
    pic.text(cx - tw / 2, cy - 43, txt, col, 4);
  }
  const out = path.replace(/\.png$/i, "") + "_labels.png";
  writeFileSync(out, pic.png());
  return { labelled: out, worms: list };
}

/** Points the drawn camera (world units) for `frames`, through render.camera when the game has it, else a client mod's
 * console. Answers the frame it was set at (render.camera) and who owns it. */
async function setCamera(s: Session, c: { pos: number[]; target: number[]; fov?: number }, frames: number, mod?: string, whenPlay = false):
  Promise<{ frame?: number; owner: string; pending?: boolean; phase?: string }> {
  if (!mod && (await has(s, "render.camera"))) {
    const r = await (await s.client()).call<{ set: boolean; frame: number; pending?: boolean; phase?: string }>("render.camera",
      { pos: c.pos, target: c.target, ...(c.fov ? { fov: c.fov } : {}), frames, ...(whenPlay ? { when: "play" } : {}) }, 15000)
      .catch((e: Error) => { throw new ToolError(`the camera override was refused: ${e.message}`); });
    // pending: the turn's intro is on; the game sets the pose when the turn plays (render.camera {when: "play"})
    return r.pending ? { owner: "oasis", pending: true, phase: r.phase } : { frame: r.frame, owner: "oasis", ...(r.phase ? { phase: r.phase } : {}) };
  }
  const owner = await cameraMod(s, mod);
  const v = (p: number[]) => `{${p.map((x) => x.toFixed(3)).join(", ")}}`;
  const code = `local ok, why = wum.render.cameraOverride{pos = ${v(c.pos)}, target = ${v(c.target)}${c.fov ? `, fov = ${c.fov}` : ""}, frames = ${frames}}
return tostring(ok) .. ";" .. tostring(why)`;
  const [ok, why] = (await evalClient(s, code, owner)).split(";");
  if (ok !== "true") throw new ToolError(`the camera override was refused: ${why}${why === "unavailable" ? " (no match is running)" : ""}`);
  return { owner };
}

// ------------------------------------------------------------------ a held camera

/** A camera held until released: render.camera ends an override `frames` after its last refresh (at most 600, about
 * 10 s), so a held pose is set again every HOLD_EVERY_MS. It stops on release (game_camera clear, preset default, a new
 * pose without hold), on game_shots (which puts it back after its own poses), or when the game refuses it twice in a
 * row (the match ended). */
const HOLD_EVERY_MS = 4000;
interface Held { cam: { pos: number[]; target: number[]; fov?: number }; mod?: string; since: number; refreshes: number; failures: number; timer: ReturnType<typeof setInterval>; lastError?: string; }
let held: Held | null = null;

export function stopHold(): Held | null {
  const h = held;
  if (h) clearInterval(h.timer);
  held = null;
  return h;
}

function startHold(s: Session, cam: Held["cam"], mod?: string) {
  stopHold();
  const h: Held = { cam, mod, since: Date.now(), refreshes: 0, failures: 0, timer: setInterval(() => {
    setCamera(s, cam, 600, mod).then(() => { h.refreshes++; h.failures = 0; }, (e: Error) => {
      h.failures++;
      h.lastError = e.message;
      if (h.failures >= 2 && held === h) stopHold();
    });
  }, HOLD_EVERY_MS) };
  (h.timer as { unref?: () => void }).unref?.();
  held = h;
}

/** The held camera, for the answers (world units). */
export const holdState = () => (held ? { holding: true, worldPos: held.cam.pos, worldTarget: held.cam.target, sinceMs: Date.now() - held.since, refreshes: held.refreshes,
  ...(held.lastError ? { lastError: held.lastError } : {}) } : { holding: false });

async function clearCamera(s: Session, mod?: string): Promise<{ cleared: boolean; owner: string }> {
  if (!mod && (await has(s, "render.camera"))) {
    const r = await (await s.client()).call<{ cleared: boolean }>("render.camera", { clear: true }, 15000);
    return { cleared: r.cleared, owner: "oasis" };
  }
  const owner = await cameraMod(s, mod);
  return { cleared: (await evalClient(s, "return tostring(wum.render.cameraOverride(nil))", owner)) === "true", owner };
}

/** render.shot: waits for a frame at or after `minFrame` (and, clean, one drawn without the mods' primitives), then the
 * PNG the game wrote. */
async function shot(s: Session, name: string, o: { width?: number; clean: boolean; minFrame?: number; play?: { settle: number; waitMs: number } }):
  Promise<{ path: string; width: number; height: number; frame: number; clean: boolean; png: Uint8Array; phase?: string; readyIntro?: boolean; waitedMs: number }> {
  if (!(await has(s, "render.shot"))) throw new ToolError("this game has no render.shot (Melange before erg-mcp's game_shots): take screenshots another way");
  const client = await s.client(), t0 = Date.now();
  for (;;) {
    const r = await client.call<{ ready: boolean; readyAt?: number; path?: string; width?: number; height?: number; frame: number; clean?: boolean; phase?: string; readyIntro?: boolean; waiting?: string }>("render.shot",
      { name, clean: o.clean, ...(o.width ? { width: o.width } : {}), ...(o.minFrame ? { minFrame: o.minFrame } : {}), ...(o.play ? { requirePlay: true, afterCamera: o.play.settle } : {}) }, 15000);
    if (r.ready && r.path) return { path: r.path, width: r.width!, height: r.height!, frame: r.frame, clean: !!r.clean, png: readFileSync(r.path), phase: r.phase, readyIntro: r.readyIntro, waitedMs: Date.now() - t0 };
    if (Date.now() - t0 > (o.play?.waitMs ?? 8000))
      throw new ToolError(r.waiting ? `no shot after ${Math.round((Date.now() - t0) / 1000)} s: waiting for ${r.waiting} (the turn's phase is ${r.phase})` : `no frame to read after 8 s (at frame ${r.frame}, waiting for ${r.readyAt}): is the game minimised or paused?`);
    await sleep(r.waiting ? 100 : 30);
  }
}

// Shot names. Every picture goes to the one Documents\Melange\shots folder that every erg-mcp process and agent shares,
// so a bare name (n, s, overview) from one run overwrote another's: a name is always <prefix>_<name>, the prefix by
// default erg_<yyyymmdd_hhmmss> of the call, and a name this process wrote before gets _2, _3, ... (unique: false keeps
// it as given).
const written = new Set<string>();
const stamp = () => { const d = new Date(), p = (v: number) => String(v).padStart(2, "0"); return `${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}_${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}`; };
const tidyName = (n: string) => n.replace(/[^A-Za-z0-9_.-]/g, "_").replace(/\.\.+/g, ".").replace(/^\.+/, "").slice(0, 64) || "shot";
export function uniqueName(prefix: string | undefined, base: string, unique = true): string {
  const pre = tidyName(prefix ?? `erg_${stamp()}`);
  let name = tidyName(base.startsWith(pre) ? base : `${pre}_${base}`);
  if (unique) for (let k = 2; written.has(name.toLowerCase()); k++) name = tidyName(`${pre}_${base}_${k}`);
  written.add(name.toLowerCase());
  return name;
}

/** The pose the game last drew from (wum.render.camera, world units), or null when the console cannot say. */
async function drawnPos(s: Session): Promise<number[] | null> {
  try {
    const t = await evalClient(s, `local c = wum.render.camera() if not c then return "" end return c.pos.x .. "," .. c.pos.y .. "," .. c.pos.z`);
    const v = t.split(",").map(Number);
    return v.length === 3 && v.every(Number.isFinite) ? v : null;
  } catch {
    return null;
  }
}

/** Waits until the game draws from `cam.pos` (within 2 world units, or 1 % of the distance to the target): another
 * client's camera override (another erg-mcp's held pose, set again every 4 s) can take the camera back between a pose
 * and its shot, which is how an overview from the north came out as the held one from the south. Sets the pose again
 * once when it is not drawn. Null when the drawn camera cannot be read (an older game, no console). */
async function confirmPose(s: Session, cam: { pos: number[]; target: number[]; fov?: number }, frames: number): Promise<{ ok: boolean; drawn: number[] | null; retried: boolean } | null> {
  let retried = false;
  const tol = Math.max(2, 0.01 * Math.hypot(cam.pos[0] - cam.target[0], cam.pos[1] - cam.target[1], cam.pos[2] - cam.target[2]));
  for (let attempt = 0; attempt < 2; attempt++) {
    const t0 = Date.now();
    for (;;) {
      const d = await drawnPos(s);
      if (!d) return null;
      if (Math.hypot(d[0] - cam.pos[0], d[1] - cam.pos[1], d[2] - cam.pos[2]) <= tol) return { ok: true, drawn: d, retried };
      if (Date.now() - t0 > 600) { if (attempt === 1) return { ok: false, drawn: d, retried }; break; }
      await sleep(40);
    }
    retried = true;
    await setCamera(s, cam, frames);
  }
  return null;
}

// ------------------------------------------------------------------ the Test probe

/** level.test.probe (Melange 0.13+): the Test probe's spawns and deaths (levels/probe.cpp, map units) and the mine
 * factory's fires (levels/factory.cpp). */
interface ProbeWorm { slot: number; team: number; posInTeam: number; name: string; pos: number[]; posWorld: number[]; health: number; state: number; }
interface ProbeExplosion { tick: number; turn: number; at: number[]; wormDamage: number; wormDamageRadiusMap: number; source: string;
  entity?: { handle: number; kind: string; type: string; label: string; at: number[] }; turnWorm?: number; turnWeapon?: string; }
interface ProbeDeath extends ProbeWorm { tick: number; turn: number; lastAlivePos: number[]; cause: string; evidence: string[]; waterMap?: number; waterWorld?: number;
  source?: ProbeExplosion; hits: { explosion: ProbeExplosion; distanceMap: number }[]; falls: { tick: number; fromY: number; toY: number; dropMap: number }[];
  healthSteps: { tick: number; from: number; to: number }[]; }
interface FireRec { stem: string; tick: number; turn: number; asked: number; launch: boolean; thrown?: boolean; factory: number[];
  mines: { handle: number; spawn: number[]; rest?: number[]; settled?: boolean; gone?: boolean; lastSeen?: number[]; restFromFactoryMap: number;
    // a thrown mine (this branch): where it was aimed, how it came down (melange: landed at the land sweep's hit; engine: the
    // engine's own rest; water; lost) and how far from its target it lies
    target?: number[]; landing?: "melange" | "engine" | "water" | "lost" | ""; restFromTargetMap?: number;
    // a game that watches the mines after the fire (this branch's levels/factory.cpp): what became of each
    fate?: "resting" | "not seen to land" | "drowned" | "destroyed" | "gone before it landed"; goneTick?: number; waterMap?: number; moves?: number; movedAfterRest?: number;
    // the hold on a landed thrown mine (this branch's levels/factory.cpp, mines round 2): put back after the engine moved it at
    // rest, rested again after the engine gave it a velocity with no blast about, dropped off walls, lifted out of the land,
    // freed by a blast; `dropped`: the engine's stale flight events (its launch arc's land, water and plane contacts, queued
    // before Melange landed it) kept from it while held (mines round 4); the land frame it rests on (65535: the heightmap)
    // `sunk`: the sea reached it while held and it was handed to the engine's drowning (mines round 4)
    rePins?: number; velPins?: number; deflects?: number; lifts?: number; dropped?: number; freed?: boolean; sunk?: boolean; frame?: number }[]; }
interface ProbeAnswer {
  probe: { key: string; playing: boolean; turn: number; tick: number; spawns: ProbeWorm[]; deaths: ProbeDeath[]; explosionsSeen: number; explosions: ProbeExplosion[] };
  // `held`: thrown mines Melange holds at their landing now; `drowned`: held mines the sea reached and the engine was given to drown (mines round 4)
  factory: { fires: FireRec[]; following: number; held?: number; drowned?: number; trigger: { tick: number; count: number; started: boolean; why: string; detail: string } | null };
}

async function probeOf(s: Session): Promise<ProbeAnswer | null> {
  if (!(await has(s, "level.test.probe"))) return null;
  return (await s.client()).call<ProbeAnswer>("level.test.probe", {}, 15000).catch(() => null);
}

const inUnits = (p: number[] | undefined, u: "map" | "world") => (p ? p.map((v) => Math.round(v * (u === "world" ? WORLD_PER_MAP : 1) * 100) / 100) : null);

/** The open project's ground (the top of the highest solid voxel, map units) and water, for fireOut; none without one. */
function groundLookup(s: Session): [((x: number, z: number) => number | null)?, (number | null)?] {
  if (!s.project) return [];
  const gs = grids(s.store), w = s.store.scene.water.level;
  return [(x, z) => spansAt(gs, x, z)[0]?.top ?? null, w === null ? null : w / WORLD_PER_MAP];
}

/** A fire as the answers give it: each mine's spawn and where it ended up (`rest`: the last position it was still at for
 * 2.5 s, kept up to date while the game watches it, 3 minutes after the fire), and its `fate` on a game that records it:
 * resting, not seen to land, drowned (gone from under the water) or destroyed (set off, or removed), with `fates` counting
 * them. */
/** Within this of the factory's axis (map units, horizontally) a mine is under the factory (the game logs the same). */
const UNDER_FACTORY = 2.5;
/** How far out from a mine the crater's rim is looked for (map units), and how much higher it must be. */
const RIM_REACH = 40, RIM_RISE = 2;

/** Where a mine lies (map units): in the water (at or under it), under the factory (within UNDER_FACTORY of its axis),
 * in the crater (the ground rises RIM_RISE over the mine within RIM_REACH further out from the factory along the line
 * through it: a rim between it and the outside), or outside it. */
export function mineZone(at: number[], factory: number[], groundAt: (x: number, z: number) => number | null, water: number | null): "water" | "under the factory" | "crater" | "outside" {
  if (water !== null && at[1] <= water) return "water";
  const dx = at[0] - factory[0], dz = at[2] - factory[2], d = Math.hypot(dx, dz);
  if (d <= UNDER_FACTORY) return "under the factory";
  const ux = dx / d, uz = dz / d;
  for (let t = 0.5; t <= RIM_REACH; t += 0.5) {
    const g = groundAt(at[0] + ux * t, at[2] + uz * t);
    if (g !== null && g >= at[1] + RIM_RISE) return "crater";
  }
  return "outside";
}

export function fireOut(f: FireRec, u: "map" | "world", groundAt?: (x: number, z: number) => number | null, water?: number | null) {
  const k = u === "world" ? WORLD_PER_MAP : 1;
  const fates: Record<string, number> = {};
  for (const m of f.mines) if (m.fate) fates[m.fate] = (fates[m.fate] ?? 0) + 1;
  // where each mine lies, with the open project's ground: the water, under the factory, the crater or outside it
  const zones: Record<string, number> = {};
  const zoneOf = (m: FireRec["mines"][number]) => {
    const at = m.rest ?? m.lastSeen;
    if (!groundAt || !at) return {};
    const z = m.fate === "drowned" ? "water" : mineZone(at, f.factory, groundAt, m.waterMap ?? water ?? null);
    zones[z] = (zones[z] ?? 0) + 1;
    return { where: z, belowFactory: Math.round((f.factory[1] - at[1]) * k * 100) / 100 };
  };
  // a mine gone before it landed was last seen at its launch point: with the open project, the ground under that point
  // says whether it fell into the sea (no ground over the water there)
  const sea = (m: FireRec["mines"][number]) => {
    if (m.fate !== "gone before it landed" || !groundAt || !m.lastSeen) return {};
    const g = groundAt(m.lastSeen[0], m.lastSeen[2]), w = m.waterMap ?? water ?? null;
    return g === null || (w !== null && g <= w) ? { likely: "drowned: no ground over the water under where it fell (the open project's voxels)" }
      : { likely: `set off before it landed, or sank elsewhere: ground at y ${Math.round(g * 100) / 100} under where it fell` };
  };
  const each = f.mines.map((m) => ({ handle: m.handle, spawn: inUnits(m.spawn, u),
    ...(m.gone ? { gone: true, lastSeen: inUnits(m.lastSeen, u), ...(m.rest ? { rest: inUnits(m.rest, u) } : {}) } : { rest: inUnits(m.rest, u), settled: !!m.settled }),
    ...(m.fate ? { fate: m.fate } : {}), ...sea(m), ...zoneOf(m), ...(m.goneTick !== undefined ? { goneTick: m.goneTick } : {}), ...(m.waterMap !== undefined ? { water: Math.round(m.waterMap * k * 100) / 100 } : {}),
    ...(m.movedAfterRest ? { movedAfterRest: m.movedAfterRest } : {}),
    ...(m.target ? { target: inUnits(m.target, u), landing: m.landing || "in flight", ...(m.restFromTargetMap !== undefined ? { restFromTarget: Math.round(m.restFromTargetMap * k * 100) / 100 } : {}),
      ...(m.rePins !== undefined ? { held: { rePins: m.rePins, velPins: m.velPins ?? 0, deflects: m.deflects ?? 0, lifts: m.lifts ?? 0, dropped: m.dropped ?? 0, freed: !!m.freed, ...(m.sunk ? { sunk: true } : {}), ...(m.frame !== undefined && m.frame >= 0 ? { frame: m.frame } : {}) } } : {}) } : {}),
    restFromFactory: Math.round(m.restFromFactoryMap * k * 100) / 100 }));
  const landings: Record<string, number> = {};
  for (const m of f.mines) if (m.target) landings[m.landing || "in flight"] = (landings[m.landing || "in flight"] ?? 0) + 1;
  return { tick: f.tick, turn: f.turn, mines: f.mines.length, launch: f.launch, ...(f.thrown ? { thrown: true, landings } : {}), factory: inUnits(f.factory, u), ...(Object.keys(fates).length ? { fates } : {}),
    ...(Object.keys(zones).length ? { where: zones, whereRule: `with the open project's ground: water (at or under it), under the factory (within ${UNDER_FACTORY} map units of its axis), crater (the ground rises ${RIM_RISE} map units over the mine within ${RIM_REACH} further out from the factory), else outside` } : {}),
    each };
}

// ------------------------------------------------------------------ announcements

/** A wum.sim.announce message as the game logged it (jlog sim/announce, on Oasis's log channel): its text, the mod or
 * level script (erg:<project>) that sent it, how long it shows (Melange 0.13 logs it; 4 s, the default, before), when. */
export interface Announce { text: string; mod?: string; seconds: number; wall: number; seq: number; id?: number; }
/** The first frame a message was drawn in at full opacity (jlog sim/announce.shown, Melange 0.13+, with its id). */
export interface Shown { id: number; text: string; frame: number; msAfterCall: number; wall: number; }

type LogRec = { msg?: string; wall?: string; data?: { text?: string; mod?: string; seconds?: number; id?: number; frame?: number; msAfterCall?: number }; seq?: number };
const logRec = (m: { j?: unknown }): LogRec | null => {
  let j = m.j as LogRec | string | undefined;
  if (typeof j === "string") try { j = JSON.parse(j); } catch { return null; }
  return j && typeof j === "object" ? j : null;
};

const parseAnnounce = (m: { seq?: number; j?: unknown }): Announce | null => {
  const j = logRec(m);
  if (!j || j.msg !== "announce" || typeof j.data?.text !== "string") return null;
  const wall = j.wall ? Date.parse(j.wall) : NaN;
  return { text: j.data.text, ...(j.data.mod ? { mod: j.data.mod } : {}), seconds: j.data.seconds ?? 4, wall: Number.isFinite(wall) ? wall : Date.now(), seq: j.seq ?? m.seq ?? 0,
    ...(typeof j.data.id === "number" ? { id: j.data.id } : {}) };
};

const parseShown = (m: { seq?: number; j?: unknown }): Shown | null => {
  const j = logRec(m);
  if (!j || j.msg !== "announce.shown" || typeof j.data?.id !== "number" || typeof j.data.frame !== "number") return null;
  const wall = j.wall ? Date.parse(j.wall) : NaN;
  return { id: j.data.id, text: j.data.text ?? "", frame: j.data.frame, msAfterCall: j.data.msAfterCall ?? 0, wall: Number.isFinite(wall) ? wall : Date.now() };
};

/** Follows the game's announcements: `fn` gets each one the log channel has (its backlog of the last 500 sim records,
 * then live ones), and `onShown` each first frame one was drawn in. Null when the game has no log channel. */
async function followAnnouncements(s: Session, fn: (a: Announce) => void, onShown?: (x: Shown) => void): Promise<(() => void) | null> {
  const client = await s.client();
  if (!client.has("log")) return null;
  return client.subscribe<{ seq?: number; j?: unknown }>("log", { cats: ["sim"], text: "announce" }, (m) => {
    const a = parseAnnounce(m);
    if (a) fn(a);
    else if (onShown) { const x = parseShown(m); if (x) onShown(x); }
  });
}

/** The announcements on the screen now and the last few, for game_state (about a third of a second to read). */
async function announcementState(s: Session) {
  const got: Announce[] = [], shown = new Map<number, Shown>();
  const stop = await followAnnouncements(s, (a) => got.push(a), (x) => shown.set(x.id, x)).catch(() => null);
  if (!stop) return undefined;
  await sleep(300);
  stop();
  const now = Date.now(), seen = new Set<string>(), key = (a: Announce) => `${a.seq}:${a.wall}:${a.text}`;
  const list = got.filter((a) => !seen.has(key(a)) && seen.add(key(a))).sort((a, b) => a.wall - b.wall);
  const on = list.filter((a) => a.wall + a.seconds * 1000 > now);
  // a game that logs announce.shown says the frame each was first drawn in at full opacity
  const drawn = (a: Announce) => (a.id === undefined ? {} : shown.has(a.id) ? { shownFrame: shown.get(a.id)!.frame } : { shownFrame: null });
  return {
    onScreen: on.map((a) => ({ text: a.text, ...(a.mod ? { mod: a.mod } : {}), secondsLeft: Math.round((a.wall + a.seconds * 1000 - now) / 100) / 10, ...drawn(a) })),
    recent: list.slice(-5).map((a) => ({ text: a.text, ...(a.mod ? { mod: a.mod } : {}), secondsAgo: Math.round((now - a.wall) / 100) / 10, seconds: a.seconds, ...drawn(a) })),
    note: "wum.sim.announce messages from the game's log (this session of the game): onScreen are still drawn (the game shows up to 3 at once); game_shots onAnnounce shoots while one is up",
  };
}

/** Waits for an announcement whose text matches `match` (a case-insensitive regular expression; any without) that is
 * on the screen now or comes within `timeoutMs`; on a game that logs the frame each message is first drawn in at full
 * opacity (announce.shown, Melange 0.13+), also for that record, so a shot taken after it has the message in it. */
async function waitAnnouncement(s: Session, match: string | undefined, timeoutMs: number): Promise<Announce & { shown?: Shown }> {
  let re: RegExp | null = null;
  if (match) try { re = new RegExp(match, "i"); } catch (e) { throw new ToolError(`onAnnounce.match: ${(e as Error).message}`); }
  const t0 = Date.now();
  let found: Announce | null = null;
  const shown = new Map<number, Shown>();
  const stop = await followAnnouncements(s, (a) => {
    if (!found && (!re || re.test(a.text)) && a.wall + a.seconds * 1000 > Date.now() + 300) found = a;
  }, (x) => shown.set(x.id, x));
  if (!stop) throw new ToolError("onAnnounce needs the game's log channel (Oasis): this game has none");
  try {
    while (!found && Date.now() - t0 < timeoutMs) await sleep(50);
    // the frame it is drawn in: logged a fade-in (0.2 s) after it is announced, so wait a little for it
    const f = found as Announce | null;
    if (f && f.id !== undefined) for (const t1 = Date.now(); !shown.has(f.id) && Date.now() - t1 < 1500; ) await sleep(30);
  } finally {
    stop();
  }
  if (!found) throw new ToolError(`no announcement${match ? ` matching /${match}/i` : ""} came on the screen within ${Math.round(timeoutMs / 1000)} s (game_state's announcements lists the recent ones)`);
  const f = found as Announce;
  const sh = f.id !== undefined ? shown.get(f.id) : undefined;
  return { ...f, ...(sh ? { shown: sh } : {}) };
}

// ------------------------------------------------------------------ a walk in the game

// The game's controls move a worm relative to the camera (tried in a Test on 2026-11-08: W walked away from the camera,
// S toward it, A and D to its left and right, the worm turning to face the way it walks; two keys walk the diagonal),
// so the walk holds the key or pair of keys nearest the bearing to the next waypoint, read again every WALK_TICK_MS
// from the active worm's position and the drawn camera's forward. Keys go through Melange's Automation module (test
// builds: [Automation] Enabled=1), which reads <gameDir>\Melange\automation.txt every frame.
const WALK_TICK_MS = 120;
const WALK_LUA = `local t = wum.game.turn() local w = nil
for _, x in ipairs(wum.game.worms() or {}) do if x.slot == t.worm then w = x end end
local c = wum.render.gameCamera and wum.render.gameCamera() or wum.render.camera()
if not w then return "none;" .. tostring(t.phase) .. ";" .. tostring(t.team) .. ";" .. tostring(t.ai) end
return string.format("%.2f,%.2f,%.2f,%s,%s,%s,%s,%s,%.3f,%.3f", w.pos.x, w.pos.y, w.pos.z, tostring(w.slot), tostring(t.team), tostring(t.phase),
  tostring(t.ai), tostring(w.alive), c and c.fwd.x or 0, c and c.fwd.z or 1)`;

/** The keys that walk toward world direction (dx, dz) with the camera looking along (fx, fz): of the 8, the nearest. */
export function walkKeys(dx: number, dz: number, fx: number, fz: number): string[] {
  const fl = Math.hypot(fx, fz) || 1, f = [fx / fl, fz / fl], r = [-f[1], f[0]];
  const a = dx * f[0] + dz * f[1], b = dx * r[0] + dz * r[1];
  const sector = ((Math.round(Math.atan2(b, a) / (Math.PI / 4)) % 8) + 8) % 8;
  return [["W"], ["W", "D"], ["D"], ["S", "D"], ["S"], ["S", "A"], ["A"], ["W", "A"]][sector];
}

export function automationFile(gameDir?: string): string {
  const dir = gameDir ?? process.env.MELANGE_GAME_DIR;
  if (!dir) throw new ToolError("game_walk drives the worm through Melange's Automation module: give gameDir (the game's folder) or set MELANGE_GAME_DIR");
  const data = join(dir, "Melange");
  if (!existsSync(data)) throw new ToolError(`no Melange folder in ${dir}: gameDir is the folder of WormsMayhem.exe`);
  return join(data, "automation.txt");
}

// ------------------------------------------------------------------ mines

/** A live entity as Oasis's entities.list answers it (world units). */
interface Entity { handle: number; kind: string; type: string; label?: string; pos: { x: number; y: number; z: number } | null; }

/** The mines of the running match: the entities whose class or label names a mine (not the factory or its machine). */
async function liveMines(s: Session): Promise<Entity[]> {
  const list = await (await s.client()).call<Entity[]>("entities.list", {}, 15000).catch((e: Error) => {
    throw new ToolError(`entities.list refused: ${e.message} (it needs a match and [GameState] Enabled=1 in the game's Melange.ini)`);
  });
  return list.filter((e) => /mine/i.test(`${e.type} ${e.label ?? ""}`) && !/factory|machine/i.test(`${e.type} ${e.label ?? ""}`));
}

/** The mine factory's own entity (MineFactoryLogicEntity) in the running match, if it exists. */
async function factoryEntities(s: Session): Promise<Entity[]> {
  const list = await (await s.client()).call<Entity[]>("entities.list", {}, 15000).catch((e: Error) => {
    throw new ToolError(`entities.list refused: ${e.message} (it needs a match and [GameState] Enabled=1 in the game's Melange.ini)`);
  });
  return list.filter((e) => /minefactory/i.test(`${e.type} ${e.label ?? ""}`.replace(/[^a-z]/gi, "")));
}

// Why a factory is gone, from the game's code (MineFactoryLogicEntity.cpp): placed, it drops from its knot along a land
// sweep of at most 1000 world units (0x5cf710) and takes the hit point; when the sweep hits no land it keeps whatever
// point the last sweep of the match left (asserting bCollidesWithLand), often under the sea. On its first update it
// explodes when the water is more than 10 world units over that point (0x5d0b52), and an explosion within its radius
// sets it off 100 ticks later; either posts MineFactory.Deleted and it is gone for the match.
const FACTORY_GONE = "the factory entity is not in the match: it exploded at load or since (MineFactory.Deleted). The game drops it from its knot onto the land under it (a sweep of at most 1000 world units); a sweep that hits nothing leaves it at a stale point of the match (on Caldera 2340 world units off, under the sea), and the water over that point sets it off on its first update. In the Tests the drop never landed on a draped skin, and missed undraped blocks inside Caldera's crater too: put the knot on an undraped block (a castle or keep), keep it more than 10 world units over the highest water, and Test again (place_object and save check the first two)";

/** The factory entity's own state, read from its object (entity.inspect, [Oasis] RawInspect): where it settled (+0x2c,
 * world units: the point its drop from the knot took), whether it has exploded (+0x48) or is about to (+0x50, hit by an
 * explosion), its fire state (+0x8c: 0 idle, 1-4 firing), the idle countdown (+0x88: set from NumTurnsInactive at its
 * set-up and after each fire, decremented by each GameLogic.StartMineFactory, which fires at 0 or under) and the mines
 * of the fire in progress (+0x84). Null when the game has no raw inspect. */
async function factoryState(s: Session, handle: number): Promise<{ pos: { x: number; y: number; z: number }; exploded: boolean; hit: boolean; fireState: number; countdown: number; toFire: number } | null> {
  if (!(await has(s, "entity.inspect"))) return null;
  const r = await (await s.client()).call<{ hex?: string | null }>("entity.inspect", { handle, len: 0x90 }, 15000).catch(() => null);
  const hex = r?.hex;
  if (!hex || hex.length < 0x90 * 2) return null;
  const at = (off: number, n: number) => hex.slice(off * 2, (off + n) * 2);
  if (/\?/.test(at(0x2c, 12) + at(0x48, 12) + at(0x84, 12))) return null;
  const bytes = new Uint8Array(0x90);
  for (let i = 0; i < 0x90; i++) bytes[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16) || 0;
  const v = new DataView(bytes.buffer);
  return { pos: { x: v.getFloat32(0x2c, true), y: v.getFloat32(0x30, true), z: v.getFloat32(0x34, true) }, exploded: bytes[0x48] !== 0, hit: bytes[0x50] !== 0, fireState: v.getInt32(0x8c, true),
    countdown: v.getInt32(0x88, true), toFire: v.getInt32(0x84, true) };
}

/** The bus messages of the mine factory and mines this game registers (names with "Mine" in them). */
async function mineMessages(s: Session): Promise<string[]> {
  if (!(await has(s, "bus.names"))) return [];
  const names = await (await s.client()).call<{ name: string }[]>("bus.names", {}, 15000).catch(() => []);
  return names.map((n) => n.name).filter((n) => /mine/i.test(n)).slice(0, 64);
}

// ------------------------------------------------------------------ playing the turns

/** Where a point is along a trail ([x, z] or [x, y, z] points, one unit): the nearest point on its segments (in 3D when
 * both have y, so a switchback's legs stacked over each other are told apart), how far along it that is, and how far off. */
export function alongTrail(trail: number[][], p: number[]): { seg: number; along: number; of: number; off: number } {
  const xyz = (q: number[]) => (q.length === 3 ? q : [q[0], NaN, q[1]]);
  const P = xyz(p);
  let cum = 0, best = { seg: 0, along: 0, off: Infinity };
  for (let i = 0; i < trail.length - 1; i++) {
    const a = xyz(trail[i]), b = xyz(trail[i + 1]);
    const useY = [a[1], b[1], P[1]].every(Number.isFinite);
    const d = [b[0] - a[0], useY ? b[1] - a[1] : 0, b[2] - a[2]], l2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    const v = [P[0] - a[0], useY ? P[1] - a[1] : 0, P[2] - a[2]];
    const t = l2 ? Math.max(0, Math.min(1, (v[0] * d[0] + v[1] * d[1] + v[2] * d[2]) / l2)) : 0;
    const off = Math.hypot(v[0] - t * d[0], v[1] - t * d[1], v[2] - t * d[2]);
    const len = Math.hypot(b[0] - a[0], b[2] - a[2]);
    if (off < best.off - 1e-9) best = { seg: i, along: cum + t * len, off };
    cum += len;
  }
  if (trail.length === 1) return { seg: 0, along: 0, of: 0, off: Math.hypot(P[0] - trail[0][0], P[2] - xyz(trail[0])[2]) };
  return { ...best, of: cum };
}

/** The rest of a trail from the point on it nearest p: that point, then the trail's later points. */
export function restOfTrail(trail: number[][], p: number[]): number[][] {
  if (trail.length < 2) return trail.map((q) => [...q]);
  const at = alongTrail(trail, p), a = trail[at.seg], b = trail[at.seg + 1];
  const len = Math.hypot(b[0] - a[0], b[b.length - 1] - a[a.length - 1]);
  let before = 0;
  for (let i = 0; i < at.seg; i++) before += Math.hypot(trail[i + 1][0] - trail[i][0], trail[i + 1][trail[i + 1].length - 1] - trail[i][trail[i].length - 1]);
  const t = len ? (at.along - before) / len : 0;
  const start = a.map((v, k) => v + (b[k] - v) * t);
  return [start, ...trail.slice(at.seg + 1).map((q) => [...q])];
}

/** game_play_turns' approach: a worm at `p` (world units) more than 1.5 map units off the start of the rest of its trail
 * (`rest`, world units, [x, z] or [x, y, z]) gets a route planned over the open project's voxels to it, or to a point a
 * few map units further on when that one is not reachable (inside a wall); null when the worm is on the trail or no
 * project is open. `lead` is the route's points ([x, y, z] world units), `length` its length (world). */
export function approachRoute(s: Session, p: number[], rest: number[][]): { lead?: number[][]; length?: number; why?: string } | null {
  if (!s.project || !rest.length) return null;
  const W = WORLD_PER_MAP, y3 = (q: number[]) => (q.length === 3 ? q[1] : NaN), xz = (q: number[]) => [q[0], q[q.length - 1]];
  const r0 = xz(rest[0]);
  if (Math.hypot(r0[0] - p[0], r0[1] - p[2]) <= 1.5 * W && !(Math.abs(y3(rest[0]) - p[1]) > 2.5 * W)) return null;
  let why = "";
  for (const ahead of [0, 2, 4, 7]) {
    const q = ahead ? aheadOn(rest, ahead * W) : r0;
    // the trail's height there (a switchback's leg), from the nearest of its points with one
    let ty = NaN, best = Infinity;
    for (const v of rest) { const d = Math.hypot(v[0] - q[0], v[v.length - 1] - q[1]); if (Number.isFinite(y3(v)) && d < best) { best = d; ty = y3(v); } }
    const r = planWalk(s.project.store, [p[0] / W, p[1] / W, p[2] / W], [q[0] / W, q[1] / W], { radius: 1, bodyWidth: WORM_WIDTH, ...(Number.isFinite(ty) ? { toY: ty / W } : {}) });
    if ("plan" in r) return { lead: r.plan.path.map((v) => [v[0] * W, v[1] * W, v[2] * W]), length: r.plan.length * W };
    why = r.why;
  }
  return { why };
}

/** The point `dist` (world units, horizontal) along a trail from its first point, or its last. */
function aheadOn(trail: number[][], dist: number): number[] {
  const xz = (q: number[]) => [q[0], q[q.length - 1]];
  let left = dist;
  for (let i = 0; i < trail.length - 1; i++) {
    const a = xz(trail[i]), b = xz(trail[i + 1]), l = Math.hypot(b[0] - a[0], b[1] - a[1]);
    if (l >= left && l > 0) return [a[0] + ((b[0] - a[0]) * left) / l, a[1] + ((b[1] - a[1]) * left) / l];
    left -= l;
  }
  return xz(trail[trail.length - 1]);
}

/** A turn's heading (horizontal unit vector): from the worm's spot `p` toward the point `dist` (world units) along the
 * rest of its trail. */
export function turnHeading(p: number[], rest: number[][], dist: number): number[] {
  const ahead = aheadOn(rest, dist), dx = ahead[0] - p[0], dz = ahead[1] - p[2], l = Math.hypot(dx, dz);
  return l < 1e-6 ? [0, 1] : [dx / l, dz / l];
}

/** A shot pose (world units) on the worm at `q`, from its left side of `heading` (10 map units off, 2 back, 11 up, over
 * castle walls), so every shot of a turn looks the same way and the ground behind the worm shows how far it went. */
export function sidePose(q: number[], heading: number[]): { pos: number[]; target: number[] } {
  const [dx, dz] = heading, side = 10 * WORLD_PER_MAP, up = 11 * WORLD_PER_MAP, back = 2 * WORLD_PER_MAP;
  return { pos: [q[0] - dz * side - dx * back, q[1] + up, q[2] + dx * side - dz * back], target: [q[0], q[1] + 0.6 * WORLD_PER_MAP, q[2]] };
}

/** game_play_turns' stuck point (Caldera v2 round 4): the spot (world units) a walk was last stuck at or left play from,
 * where that is along and off the trail (world units in, `out` for the answer), the open project's ground there (the
 * top, its frame and material; a skin at its sheared height), and a close shot of it from 4 map units off the side of
 * `heading`, 3.5 up, with the eye checked for a clear view and kept over the water. The shot's failure is in `shot`. */
async function stuckPointOf(s: Session, at: number[], trail: number[][], why: string, heading: number[], name: string, prefix: string, width: number, clearView: boolean, out: (v: number) => number) {
  const W = WORLD_PER_MAP, on = alongTrail(trail, trail[0].length === 3 ? at : [at[0], at[2]]);
  let ground: Record<string, unknown> | undefined;
  if (s.project) {
    const gs = grids(s.store), sp = spansAt(gs, at[0] / W, at[2] / W, at[1] / W + 1.5)[0];
    const g = sp ? gs.find((q) => q.id === sp.frame) : undefined;
    ground = sp ? { top: out(sp.top * W), frame: sp.frame, ...(g ? { name: g.frame.name, scale: g.toMap[0], draped: !!g.frame.corners } : {}), wormOverGround: out(at[1] - sp.top * W) }
      : { top: null, note: "no ground under the point within 1.5 map units over it (the worm was in the air, or the voxels have none there)" };
  }
  const [dx, dz] = heading, side = 4 * W, up = 3.5 * W;
  let pose = { pos: [at[0] - dz * side - dx * W, at[1] + up, at[2] + dx * side - dz * W], target: [at[0], at[1] + 0.4 * W, at[2]] };
  const views: string[] = [];
  if (clearView && s.project) {
    const m = (v: number[]) => v.map((x) => x / W), ce = clearEye(grids(s.store), m(pose.pos), m(pose.target), { skip: 1 });
    if (ce.moved) { pose = { ...pose, pos: ce.eye.map((x) => x * W) }; views.push(ce.moved); }
  }
  const ow = overWater(pose, (await gameState(s).catch(() => ({} as GameState))).water);
  if (ow.lifted) { pose = ow.pose; views.push(ow.lifted); }
  let picture: string;
  try {
    const set = await setCamera(s, { ...pose, fov: 50 }, 40);
    picture = (await shot(s, uniqueName(prefix, name), { clean: true, width, minFrame: set.frame !== undefined ? set.frame + 2 : undefined })).path;
    await clearCamera(s).catch(() => undefined);
  } catch (e) {
    picture = `failed: ${(e as Error).message}`;
  }
  return { at: at.map(out), why, along: out(on.along), of: out(on.of), offTrail: out(on.off), ...(ground ? { ground } : {}), shot: picture, ...(views.length ? { eyeMoved: views } : {}),
    note: "the exact spot the walk was last held at (or left play from), its place along the trail and off its centreline, the ground under it in the project's voxels, and a close shot of it: check_trail there, or sculpt" };
}

/** The clearance a camera keeps over the sea, map units. */
export const OVER_WATER = 1.5;

/** A pose (world units) lifted so its eye, and a target under the sea, stand OVER_WATER over the match's water `water`
 * (world units, the level now): late in a flood the side pose on a worm at the shore, the castle preset's low eye and
 * the fire camera's pose sank under the risen sea and the shots showed its underside (Caldera v2 round 3). `lifted` says
 * what moved; nothing without a water level. */
export function overWater<P extends { pos: number[]; target: number[] }>(pose: P, water: number | undefined | null): { pose: P; lifted?: string } {
  if (water === undefined || water === null || !Number.isFinite(water)) return { pose };
  const floor = water + OVER_WATER * WORLD_PER_MAP, m = (v: number) => Math.round((v / WORLD_PER_MAP) * 100) / 100;
  const dy = Math.max(0, floor - pose.pos[1]), ty = Math.max(0, floor - pose.target[1]);
  if (dy <= 0 && ty <= 0) return { pose };
  const parts = [dy > 0 ? `the eye raised ${m(dy)} map units` : "", ty > 0 ? `the target raised ${m(ty)}` : ""].filter(Boolean);
  return { pose: { ...pose, pos: [pose.pos[0], pose.pos[1] + dy, pose.pos[2]], target: ty > 0 ? [pose.target[0], pose.target[1] + ty, pose.target[2]] : pose.target },
    lifted: `${parts.join(", ")} to clear the water (${m(water)} map, + ${OVER_WATER})` };
}

// ------------------------------------------------------------------ the weapon

/** Selects a weapon for the active worm, as the weapon panel's pick does: level.test.selectWeapon (this branch's Melange:
 * the panel's own Weapon.DelayedPanelChanged sent in the sim, whatever the window's focus or DirectInput's polls) where
 * the game has it, else the Automation module's panel clicks (a right click opens the panel, a click takes its highlighted
 * first cell, the bazooka: test builds, the game window focused, the bazooka alone). The worm's weapon is read back
 * (wum.game.worms().weapon) until it is set; a ToolError when none was taken. In the Caldera v2 round-3 baseline the
 * clicks took nothing (the injected mouse reported polls=0) and nothing ever fired. */
export async function selectWeapon(s: Session, weapon: string | number, gameDir?: string): Promise<{ weapon: number; name?: string; via: string; before: string; ms: number }> {
  const client = await s.client(), t0 = Date.now();
  const st = await gameState(s);
  if (st.scene !== "match" || st.worm === undefined) throw new ToolError(`no match turn to select a weapon in (scene ${st.scene}): test first`);
  if (st.ai) throw new ToolError(`the turn is a computer team's (WORM${st.worm}): a weapon is selected on a human turn`);
  const slot = st.worm;
  const read = () => evalClient(s, `for _, w in ipairs(wum.game.worms() or {}) do if w.slot == ${slot} then return tostring(w.weapon) end end return "nil"`).catch(() => "?");
  const before = await read();
  let via: string, name: string | undefined, want: number | undefined;
  if (await has(s, "level.test.selectWeapon")) {
    const r = await client.call<{ weapon: number; name: string }>("level.test.selectWeapon", { weapon }, 15000)
      .catch((e: Error) => { throw new ToolError(`level.test.selectWeapon refused ${JSON.stringify(weapon)}: ${e.message}`); });
    name = r.name; want = r.weapon;
    via = "level.test.selectWeapon (Weapon.DelayedPanelChanged, the panel's own message, sent in the sim)";
  } else {
    if (weapon !== "bazooka" && weapon !== "kWeaponBazooka" && weapon !== 1)
      throw new ToolError("this game has no level.test.selectWeapon (a Melange of the erg-mcp branch): only the bazooka can be taken here, from the weapon panel's first cell (weapon \"bazooka\"; Automation module, the game window focused)");
    appendFileSync(automationFile(gameDir), ["click right", "wait 500", "click", "wait 300", ""].join("\n"));
    via = "the weapon panel (Automation module: a right click, then a click on its first cell, the bazooka)";
  }
  // the sim applies the pick at its next tick (the clicks take about 1.5 s): read until the weapon is the one asked
  let after = before;
  for (let i = 0; i < 12 && (after === "nil" || after === "?" || (want !== undefined ? after !== String(want) : after === before)); i++) { await sleep(250); after = await read(); }
  if (after === "nil" || after === "?") throw new ToolError(`no weapon selected ${Math.round((Date.now() - t0) / 100) / 10} s after ${via}: the worm's weapon reads ${after} (was ${before}); is the turn in play (game_state phase play)?`);
  if (want !== undefined && after !== String(want)) throw new ToolError(`the worm's weapon reads ${after} after ${via}, not ${want} (${name}): the pick was not applied (a weapon the scheme does not give, or a delay on it?)`);
  return { weapon: Number(after), ...(name ? { name } : {}), via, before, ms: Date.now() - t0 };
}

const withMap = (st: GameState) => ({ ...st, ...(st.water !== undefined ? { waterMap: Math.round((st.water / WORLD_PER_MAP) * 100) / 100 } : {}) });

const units = { type: "string", enum: ["map", "world"], description: "Units of pos and target: map (default, as the other tools) or world (map x 20, the game's)." } as const;
const vec3 = (d: string) => ({ type: "array", items: { type: "number" }, minItems: 3, maxItems: 3, description: d }) as const;

export const tools: Tool[] = [
  {
    name: "game_state",
    description: "The running match as the game sees it: scene (menu, loading, match), the turn (counted from 1 as wum.sim.turnStarted and check_flood count, team, the active worm's slot, phase, ai), turnLog (the turns seen so far, and turnOrder for check_flood once every turn from 1 was seen), water level (world and map units), turn and round time left, sudden death, worms alive per team, and the sim tick. Needs the game running (after test).",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      announceShot: { type: "boolean", description: "When a wum.sim.announce message is on the screen and the game logged the frame it was first drawn in at full opacity (shownFrame), also shoot the game's own view from a later frame (render.shot, not clean: the message is drawn with the HUD): picture evidence of the announcement, answered as `announcementShot` (path, frame, the text). Nothing is shot when none is up." },
      prefix: { type: "string", description: "announceShot: the PNG's name prefix (default erg_<yyyymmdd_hhmmss>): <prefix>_announce.png." },
      width: { type: "integer", minimum: 160, maximum: 1920, description: "announceShot: the picture's width (default 960)." } } },
    async handler(s, a: { announceShot?: boolean; prefix?: string; width?: number }) {
      const st = await gameState(s);
      noteTurn(st);
      const log = turnLogOut(), ann = st.scene === "match" ? await announcementState(s) : undefined;
      // announceShot: the game's view while the newest message that was drawn is still up
      let announcementShot: object | undefined;
      if (a.announceShot && ann) {
        const up = ann.onScreen.filter((x) => typeof (x as { shownFrame?: number | null }).shownFrame === "number").pop() as { text: string; shownFrame: number; secondsLeft: number } | undefined;
        if (!up) announcementShot = { taken: false, why: ann.onScreen.length ? "the message on the screen has no logged shownFrame yet (fading in, or a game that does not log announce.shown)" : "no announcement is on the screen" };
        else {
          const p = await shot(s, uniqueName(a.prefix, "announce"), { clean: false, width: a.width ?? 960, minFrame: up.shownFrame + 1 });
          announcementShot = { taken: true, path: p.path, frame: p.frame, text: up.text, shownFrame: up.shownFrame, secondsLeft: up.secondsLeft };
        }
      }
      return { ...withMap(st), turnNote: TURN_NOTE, ...(log ? { turnLog: log } : {}), ...(ann ? { announcements: ann } : {}), ...(announcementShot ? { announcementShot } : {}), camera: holdState() };
    },
  },
  {
    name: "game_wait",
    description: "Wait in the running game until the match reaches turn `turn` (turns started), or `seconds` pass, or the match ends; then answer game_state. Use it to watch a level script act over turns (a rising sea). A test match with an idle human team ends by itself after some minutes: keep waits short.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        turn: { type: "integer", minimum: 1, maximum: 1000, description: "Wait until at least this many turns have started: until game turn `turn` (counted from 1 as wum.sim.turnStarted does) is playing." },
        seconds: { type: "number", minimum: 0.5, maximum: 600, description: "Wait at most this long (default 120)." },
      },
    },
    async handler(s, a: { turn?: number; seconds?: number }) {
      const limit = (a.seconds ?? 120) * 1000, t0 = Date.now();
      let st = await gameState(s);
      noteTurn(st);
      while (Date.now() - t0 < limit) {
        if (st.scene !== "match" && st.scene !== "loading") break;
        if (a.turn !== undefined && (st.turn ?? 0) >= a.turn) break;
        await new Promise((r) => setTimeout(r, Math.min(1000, Math.max(0, limit - (Date.now() - t0)))));
        st = await gameState(s);
        noteTurn(st);
      }
      const reached = a.turn === undefined ? undefined : (st.turn ?? 0) >= a.turn;
      const log = turnLogOut();
      return { waitedMs: Date.now() - t0, ...(reached !== undefined ? { reached } : {}),
        ...(st.scene !== "match" ? { note: `the game is at '${st.scene}', not in a match` } : {}), state: withMap(st), turnNote: TURN_NOTE, ...(log ? { turnLog: log } : {}) };
    },
  },
  {
    name: "game_camera",
    description: "Point the game's drawn camera from `pos` at `target` for `seconds` (at most 10; call again to hold it longer), or clear it. `preset`: default (the game's own camera: the override cleared) or overview (the open project's whole level from the south, 50 degrees down). A camera override changes only the picture on this machine: the game's own camera, aiming and the match are untouched. With Melange's render.camera it needs no mod (owner oasis); on an older game it is owned by a client mod (`mod`, default the first enabled mod with client code), and `mod` forces that. `shot: true` also answers the frame as a PNG (game_shots' clean, width and name apply). Needs a match running (after test).",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        pos: vec3("Eye [x, y, z]."), target: vec3("Point looked at [x, y, z]."), units,
        preset: { type: "string", enum: ["default", "overview"], description: "default: give the camera back to the game (its own turn camera); overview: the whole level (the open project's frames), from the south 50 degrees down unless azimuth, elevation or distance say otherwise." },
        ...orbitProps,
        fov: { type: "number", minimum: 5, maximum: 150, description: "Vertical field of view in degrees (default the game's)." },
        seconds: { type: "number", minimum: 0.1, maximum: 10, description: "How long it holds (default 5; it ends by itself after that). See hold." },
        hold: { type: "boolean", description: "Hold the pose until released: erg-mcp sets it again every 4 s (a render.camera override lasts at most 10 s) until game_camera {clear: true} (or preset default, or a pose without hold), and game_shots put it back after their own poses. game_state answers whether a camera is held. It stops by itself when the game refuses it twice (the match ended)." },
        clear: { type: "boolean", description: "Give the camera back to the game (as preset default)." },
        mod: { type: "string", description: "Run the override in this enabled client mod's console instead of render.camera." },
        shot: { type: "boolean", description: "Also read the frame back (render.shot) and answer it as image content." },
        clean: { type: "boolean", description: "shot: hide the mods' wum.draw primitives (their HUD text) for the frames of the shot (default true)." },
        width: { type: "integer", minimum: 64, maximum: 8192, description: "shot: scale the picture down to this width (default the window's)." },
        name: { type: "string", description: "shot: the PNG's name in the shots folder after the prefix: <prefix>_<name> (default shot), made unique." },
        prefix: { type: "string", description: "shot: the name's prefix (default erg_<yyyymmdd_hhmmss> of the call)." },
      },
    },
    async handler(s, a: Orbit & { pos?: number[]; target?: number[]; units?: "map" | "world"; preset?: "default" | "overview"; fov?: number; seconds?: number; clear?: boolean; mod?: string;
      shot?: boolean; clean?: boolean; width?: number; name?: string; prefix?: string; hold?: boolean }) {
      const k = a.units === "world" ? 1 : WORLD_PER_MAP;
      const frames = a.hold ? 600 : Math.max(1, Math.min(600, Math.round((a.seconds ?? 5) * 60)));
      let out: Record<string, unknown>, frame: number | undefined, shotPose: { pos: number[]; target: number[]; fov?: number } | undefined, shotFrames = frames;
      if (a.clear || a.preset === "default") {
        if (a.pos || a.target) throw new ToolError("clear and preset default take no pos or target");
        if (a.hold) throw new ToolError("hold holds a pose: give pos and target or preset overview");
        const was = stopHold();
        const c = await clearCamera(s, a.mod);
        out = { ...c, preset: "default", ...(was ? { released: { heldMs: Date.now() - was.since, refreshes: was.refreshes } } : {}), note: "the game's own camera draws again" };
      } else {
        if (a.preset !== "overview" && (a.azimuth !== undefined || a.elevation !== undefined || a.distance !== undefined)) throw new ToolError("azimuth, elevation and distance are for preset overview");
        const cam = a.preset === "overview" ? await overviewOf(s, a, k) : (() => {
          if (!a.pos || !a.target) throw new ToolError("give pos and target, a preset, or clear: true");
          if (![...a.pos, ...a.target].every(Number.isFinite)) throw new ToolError("pos and target are finite numbers");
          return { pos: a.pos.map((x) => x * k), target: a.target.map((x) => x * k), fov: a.fov };
        })();
        if (a.preset === "overview" && (a.pos || a.target)) throw new ToolError("preset overview takes no pos or target");
        stopHold();
        const ow = overWater({ ...cam, fov: a.fov ?? cam.fov }, (await gameState(s).catch(() => ({} as GameState))).water), pose = ow.pose;
        const r = await setCamera(s, pose, frames, a.mod);
        if (!a.mod) { shotPose = pose; shotFrames = frames; }
        frame = r.frame;
        if (a.hold) startHold(s, pose, a.mod);
        out = { set: true, owner: r.owner, ...(a.mod ? { mod: a.mod } : {}), ...(ow.lifted ? { lifted: ow.lifted } : {}), frames, ...(a.preset ? { preset: a.preset } : {}), worldPos: cam.pos, worldTarget: cam.target,
          ...(a.hold ? { held: true, note: "held until game_camera {clear: true} (set again every 4 s)" } : { note: "it ends by itself after the frames; game_camera {clear: true} ends it now; hold: true keeps it" }) };
      }
      if (!a.shot) return out;
      let poseWarning: string | undefined;
      if (shotPose) {
        const check = await confirmPose(s, shotPose, shotFrames);
        if (check && !check.ok) poseWarning = `the game drew from ${check.drawn!.map((v) => Math.round(v))} world, not the pose: another client (a held game_camera in another erg-mcp) has the camera`;
        if (check?.retried) frame = undefined;
      }
      const p = await shot(s, uniqueName(a.prefix, a.name ?? "shot"), { width: a.width, clean: a.clean ?? true, minFrame: frame !== undefined ? frame + 2 : undefined });
      const parts: Content[] = [image(p.png), { type: "text", text: JSON.stringify({ ...out, shot: { path: p.path, width: p.width, height: p.height, frame: p.frame, clean: p.clean }, ...(poseWarning ? { warning: poseWarning } : {}) }, null, 1) }];
      return { content: parts };
    },
  },
  {
    name: "set_overview",
    description: "The open project's overview framing for the rest of this erg-mcp session: game_camera's and game_shots' preset overview then use it whenever a call does not say otherwise. `fit: \"island\"` (the solid voxels over the water) frames a small island tightly instead of every voxel frame's box (a wiped base's empty frames spread far past the island), centred on it and as close as shows all of it in the game's 60 degree view (16:9), times `margin` (default 1.1); `azimuth`, `elevation` and `distance` as the preset's. `clear: true` drops it. Answers the pose it gives (map and world units) and the box it frames. Read-only for the level: the game's own turn camera is not changed.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: { ...orbitProps, units, clear: { type: "boolean", description: "Drop the project's overview framing (the preset's defaults again)." } },
    },
    handler(s, a: Orbit & { units?: "map" | "world"; clear?: boolean }) {
      const p = s.requireProject(), k = a.units === "world" ? 1 : WORLD_PER_MAP;
      if (a.clear) { overviewDefaults.delete(p.id); return { project: p.id, cleared: true, pose: poseOut(overview(s, {}, k), k) }; }
      const o: Orbit = { fit: a.fit ?? "island", ...(a.azimuth !== undefined ? { azimuth: a.azimuth } : {}), ...(a.elevation !== undefined ? { elevation: a.elevation } : {}),
        ...(a.distance !== undefined ? { distance: (a.distance * k) / WORLD_PER_MAP } : {}), ...(a.margin !== undefined ? { margin: a.margin } : {}) };
      const pose = overview(s, o, WORLD_PER_MAP);
      overviewDefaults.set(p.id, o);
      const water = s.store.scene.water.level === null ? null : s.store.scene.water.level / WORLD_PER_MAP;
      const box = o.fit === "frames" ? null : o.fit === "island" ? islandBox(s, water ?? 0) : solidBox(s, null);
      const r = (v: number) => Math.round(v * 100) / 100;
      return { project: p.id, overview: { ...o, ...(o.distance !== undefined ? { distance: r((o.distance * WORLD_PER_MAP) / k) } : {}) }, pose: poseOut(pose, k),
        ...(box ? { frames: { min: box.min.map((v) => r((v * WORLD_PER_MAP) / k)), max: box.max.map((v) => r((v * WORLD_PER_MAP) / k)) } } : {}),
        note: "game_camera {preset: \"overview\"} and game_shots' {preset: \"overview\"} poses use it from now on in this session (their own azimuth, elevation, distance, fit or margin still win)" };
    },
  },
  {
    name: "game_shots",
    description: "Several camera poses captured in one call, so evidence fits in a short match: for each of `cams` ({pos, target, fov} in `units`, or {preset: \"default\" | \"overview\"}, each with an optional `name`), the camera is pointed (render.camera, no mod needed), the first frame drawn from it is read back (render.shot) and written to Documents\\Melange\\shots\\<name>.png, about a tenth of a second a pose. clean (default true) hides the mods' wum.draw text in the shots. The camera goes back to the game at the end (or holds the last pose with `hold`). Answers each PNG as image content (unless images: false) and their paths. Needs the game with render.shot and a match drawing.",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["cams"],
      properties: {
        cams: { type: "array", minItems: 1, maxItems: 24, items: { type: "object", additionalProperties: false, properties: {
          pos: vec3("Eye [x, y, z]."), target: vec3("Point looked at [x, y, z]."), fov: { type: "number", minimum: 5, maximum: 150 },
          preset: { type: "string", enum: ["default", "overview", "castle"], description: "default: the game's own camera; overview: the whole level (with no project open, as in a baseline test {map, noLevel}: the box of the match's worms); castle: a wide pose on a castle and its worms (`at`, `radius`, `team`), labelled (labels)." },
          ...orbitProps,
          at: vec3("preset castle: the castle's centre [x, y, z] (its courtyard floor), in `units`."),
          radius: { type: "number", minimum: 1, description: "preset castle: the castle's half size (to its far corner), in `units` (default 14 map units)." },
          team: { type: "integer", minimum: 0, maximum: 3, description: "preset castle: also frame this team's living worms wherever they are (default: the worms within 1.5 radii)." },
          name: { type: "string", description: "The PNG's name after the prefix: <prefix>_<name> (default <prefix>_<n>)." },
          clearView: { type: "boolean", description: "With pos and target: check the eye against the open project's voxels first, and when it is inside the ground, within 0.75 map units of it, or has voxels between it and the target, move it to the nearest eye in open air with a clear view (raised, nearer or farther, turned round the target); the shot's eyeMoved says how (default false: as given)." } } } },
        units,
        prefix: { type: "string", description: "Every PNG is <prefix>_<name> (a cam's name, else its number), so runs never overwrite each other's in the shared shots folder; default erg_<yyyymmdd_hhmmss> of the call." },
        unique: { type: "boolean", description: "A name this erg-mcp wrote before gets _2, _3, ... (default true; false overwrites it)." },
        onAnnounce: { type: "object", additionalProperties: false, description: "Wait for a wum.sim.announce message to be on the screen (one up now, or the next to come; from the game's log channel), then take the poses at once, not clean (the message is drawn with the HUD primitives clean hides): evidence that a level script announced. The answer's announcement gives its text.",
          properties: { match: { type: "string", description: "A case-insensitive regular expression the text must match (default any)." }, timeoutSec: { type: "number", minimum: 1, maximum: 600, description: "Wait at most this long (default 60)." } } },
        clean: { type: "boolean", description: "Hide the mods' wum.draw primitives in the shots (default true)." },
        width: { type: "integer", minimum: 64, maximum: 8192, description: "Scale every picture down to this width (default 960)." },
        settleFrames: { type: "integer", minimum: 1, maximum: 120, description: "Frames drawn from a pose before it is read (default 2; more lets particles and the water move on)." },
        waitPlay: { type: "boolean", description: "Take each pose only while the turn plays: the turn's \"Ready?\" intro draws its main view from another camera than the override (in the Caldera iteration-10 Test every castle shot taken in an intro showed the worm and the HUD), so the game holds the pose until the turn is in play (render.camera {when: \"play\"}), sets it then and the shot is read once it has been drawn settleFrames frames (default 6 with waitPlay). Each shot answers its phase. Waits at most waitSec per pose (default 60). Melange 0.13+: an older game takes the poses at once and the answer says so." },
        waitSec: { type: "number", minimum: 1, maximum: 300, description: "waitPlay: the longest wait for the turn to play, per pose (default 60)." },
        hold: { type: "boolean", description: "Keep the last pose for 5 s instead of giving the camera back." },
        holdLast: { type: "boolean", description: "Keep the last pose until released (game_camera {clear: true}), as game_camera's hold." },
        images: { type: "boolean", description: "Answer the pictures as image content (default true); false: paths only." },
        labels: { type: "boolean", description: "Also write <name>_labels.png beside each shot: every living worm in view ringed in its team's colour (0 red, 1 blue, 2 green, 3 yellow) with its slot number (WORMn) over it, placed from the pose, and answer the worms' places in the picture (default true for preset castle, else false). The game's own name tags are too small to read in a wide shot." },
      },
    },
    async handler(s, a: { cams: Cam[]; units?: "map" | "world"; prefix?: string; unique?: boolean; onAnnounce?: { match?: string; timeoutSec?: number }; clean?: boolean; width?: number; settleFrames?: number;
      waitPlay?: boolean; waitSec?: number; hold?: boolean; holdLast?: boolean; images?: boolean; labels?: boolean }) {
      if (!(await has(s, "render.camera")) || !(await has(s, "render.shot"))) throw new ToolError("game_shots needs the game's render.camera and render.shot (a Melange with them, running)");
      if (a.waitPlay && a.onAnnounce) throw new ToolError("waitPlay and onAnnounce both decide when to shoot: give one");
      const k = a.units === "world" ? 1 : WORLD_PER_MAP, t0 = Date.now(), settle = a.settleFrames ?? (a.waitPlay ? 6 : 2);
      const waterNow = (await gameState(s).catch(() => ({} as GameState))).water;   // every pose is kept over the sea as it stands now
      let playSupported: boolean | undefined;
      const shots: object[] = [], parts: Content[] = [], warnings: string[] = [], prefix = a.prefix ?? `erg_${stamp()}`;
      const castleWorms = new Map<number, string[] | undefined>(), moved = new Map<number, string>();
      // onAnnounce: wait for the message, then shoot at once and not clean (the announcement is drawn with the HUD
      // primitives a clean shot hides)
      const ann = a.onAnnounce ? await waitAnnouncement(s, a.onAnnounce.match, (a.onAnnounce.timeoutSec ?? 60) * 1000) : null;
      const shownFrame = ann?.shown?.frame;
      const cleanShots = ann ? false : a.clean ?? true;
      // a held pose pauses for the shots and comes back after them (unless they hold their own last pose)
      const paused = stopHold();
      let lastPose: Held["cam"] | null = null;
      try {
        for (const [i, c] of a.cams.entries()) {
          const name = uniqueName(prefix, c.name ?? String(i + 1), a.unique !== false);
          let frame: number | undefined;
          if (c.preset === "default") {
            if (c.pos || c.target) throw new ToolError(`cams[${i}]: preset default takes no pos or target`);
            await clearCamera(s);
          } else {
            let framed: string[] | undefined;
            const cam = c.preset === "overview" ? await overviewOf(s, c, k) : c.preset === "castle" ? await (async () => {
              if (!c.at) throw new ToolError(`cams[${i}]: preset castle needs at (the castle's centre)`);
              const cp = castlePose(c.at.map((x) => (x * k) / WORLD_PER_MAP), ((c.radius ?? 14 * WORLD_PER_MAP / k) * k) / WORLD_PER_MAP, await wormsNow(s), c.team, c.azimuth, c.elevation);
              framed = cp.worms;
              return cp.pose;
            })() : (() => {
              if (!c.pos || !c.target) throw new ToolError(`cams[${i}]: give pos and target, or a preset`);
              const pose = { pos: c.pos.map((x) => x * k), target: c.target.map((x) => x * k), fov: c.fov };
              if (!c.clearView) return pose;
              if (!s.project) throw new ToolError(`cams[${i}]: clearView reads the open project's voxels: open_project the tested level`);
              const m = (v: number[]) => v.map((x) => x / WORLD_PER_MAP), ce = clearEye(grids(s.store), m(pose.pos), m(pose.target), { skip: 1 });
              if (ce.moved) moved.set(i, ce.moved);
              else if (!ce.clear) warnings.push(`${name}: no eye near pos is in open air with a clear view of the target: shot as given`);
              return { ...pose, pos: ce.eye.map((x) => x * WORLD_PER_MAP) };
            })();
            if ((cam as { from?: string }).from) warnings.push(`${name}: ${(cam as { from?: string }).from}`);
            const ow = overWater(cam, waterNow);
            if (ow.lifted) { Object.assign(cam, ow.pose); warnings.push(`${name}: ${ow.lifted}`); }
            castleWorms.set(i, framed);
            const last = (a.hold || a.holdLast) && i === a.cams.length - 1;
            lastPose = { ...cam, fov: c.fov ?? cam.fov };
            const frames = last ? (a.holdLast ? 600 : 300) : Math.max(30, settle + 20);
            const set = await setCamera(s, lastPose, frames, undefined, !!a.waitPlay);
            frame = set.frame;
            if (a.waitPlay) playSupported = set.pending || set.phase !== undefined;
            // a pending pose is not drawn yet: the game confirms it (afterCamera) when the turn plays
            const check = set.pending ? null : await confirmPose(s, lastPose, frames);
            if (check && !check.ok) warnings.push(`${name}: the game drew from ${check.drawn!.map((v) => Math.round(v))} world, not the pose ${lastPose.pos.map((v) => Math.round(v))}: another client (a held game_camera in another erg-mcp) has the camera`);
            if (check?.retried) frame = undefined;
          }
          // with an announcement drawn: never a frame from before it was on the screen
          const minFrame = frame !== undefined ? frame + settle : undefined, afterShown = shownFrame !== undefined ? shownFrame + 1 : undefined;
          const play = a.waitPlay && playSupported && c.preset !== "default" ? { settle, waitMs: (a.waitSec ?? 60) * 1000 } : undefined;
          const p = await shot(s, name, { width: a.width ?? 960, clean: cleanShots, minFrame: minFrame !== undefined && afterShown !== undefined ? Math.max(minFrame, afterShown) : minFrame ?? afterShown, play });
          if (play && p.phase !== undefined && p.phase !== "play") warnings.push(`${name}: read in the turn's ${p.phase} phase`);
          // labels: the worms ringed and numbered over a copy of the shot, placed from the pose drawn
          const pose = lastPose as Held["cam"] | null;
          const labels = c.preset !== "default" && pose && (a.labels ?? c.preset === "castle")
            ? await wormsNow(s).then((ws) => labelShot(p.path, p.png, { pos: pose.pos, target: pose.target, fov: pose.fov ?? 60 }, ws)).catch((e: Error) => ({ labelError: e.message })) : null;
          shots.push({ name, ...(c.preset ? { preset: c.preset } : {}), ...(c.preset === "overview" && lastPose ? { azimuth: c.azimuth ?? 180, worldPos: lastPose.pos.map((v) => Math.round(v * 10) / 10) } : {}),
            ...(castleWorms.get(i) ? { framedWorms: castleWorms.get(i) } : {}),
            ...(moved.get(i) ? { eyeMoved: `the eye was inside the ground or had no clear view of the target: ${moved.get(i)}` } : {}),
            path: p.path, width: p.width, height: p.height, frame: p.frame, clean: p.clean, ...(p.phase !== undefined ? { phase: p.phase } : {}), ...(play ? { waitedMs: p.waitedMs } : {}), ...(labels ?? {}) });
          if (a.images !== false) parts.push(image(p.png));
        }
      } finally {
        if (a.holdLast && lastPose) startHold(s, lastPose);
        else if (paused && !a.hold) {
          await setCamera(s, paused.cam, 600, paused.mod).catch(() => undefined);
          startHold(s, paused.cam, paused.mod);
        } else if (!a.hold) await clearCamera(s).catch(() => undefined);
      }
      const camera = a.holdLast && lastPose ? "the last pose is held until released" : paused && !a.hold ? "the held pose is back" : a.hold ? "the last pose holds for 5 s" : "given back to the game";
      if (ann) {
        const left = (ann.wall + ann.seconds * 1000 - Date.now()) / 1000;
        if (left < 0) warnings.push(`the announcement left the screen ${Math.round(-left * 10) / 10} s before the last shot: fewer poses, or a longer announce`);
      }
      if (a.waitPlay && playSupported === false) warnings.push("this game cannot hold a pose until the turn plays (render.camera {when}, Melange 0.13+): the poses were taken at once and may show the turn's intro");
      const first = shots[0] as { frame?: number } | undefined;
      parts.push({ type: "text", text: JSON.stringify({ shots, ms: Date.now() - t0, camera, ...(ann ? { announcement: { text: ann.text, ...(ann.mod ? { mod: ann.mod } : {}), seconds: ann.seconds, shownMsBeforeFirstShot: Math.max(0, t0 - ann.wall),
        ...(ann.shown ? { shownFrame: ann.shown.frame, shownMsAfterCall: ann.shown.msAfterCall, firstShotFrame: first?.frame, proof: first?.frame !== undefined && first.frame > ann.shown.frame
          ? `the game logged the message drawn at full opacity in frame ${ann.shown.frame}; every shot was read from a later frame while it was on the screen (${ann.seconds} s)` : "the shot's frame is not after the logged one" }
          : { shownFrame: null, shownNote: "this game does not log the frame a message is drawn in (announce.shown, Melange 0.13+): the shot was taken while the log says it is up" }) } } : {}),
        ...(warnings.length ? { warnings } : {}) }, null, 1) });
      return { content: parts };
    },
  },
  {
    name: "game_walk",
    description: "Walk the active worm of the running Test in the game and report how far it got: the in-game check of a check_reachability route. Three ways to give the route: `waypoints` (straight lines from one to the next, steered at each in turn: the walk before), `path` (a dense route such as check_reachability {route: true}'s `route`: followed by pure pursuit, aiming `lookahead` along the route from the point on it nearest the worm, so it turns round pillars and keep corners with the route) or `to` ([x, z]: the route is planned from the worm's own spot over the open project's voxels with check_reachability's walk graph, the worm's width (`bodyWidth`, default 0.6 map units) enforced and jumps over gaps up to 2 map units, and planned again from where it is when it stays stuck). The keys are the game's own (W, A, S, D relative to the camera; the pair nearest each bearing, steered every 0.12 s), sent through Melange's Automation module (test builds with [Automation] Enabled=1; gameDir or MELANGE_GAME_DIR names the game's folder). It waits for a turn of a human team in its play phase (`worm`: only that WORMn's), walks until the end is within `arrive`, the turn ends, the worm dies or `timeoutSec`. Where it stops making progress for 0.6 s it recovers in turn (`recover`, default true; with waypoints only jump): jump, a side step, a backflip (the game's own jump and backflip keys, read from its bindings). Answers reached (and whether the end), the route followed (`along`/`of` for a path), where it ended and how far from the end, the climb, the time, the recoveries and where it got stuck, and samples of its path, in `units`.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        waypoints: { type: "array", minItems: 1, maxItems: 200, items: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 3 }, description: "The route as waypoints: [x, z] or [x, y, z] (y unused), in `units`, walked straight from one to the next (check_reachability's waypoints)." },
        path: { type: "array", minItems: 2, maxItems: 4000, items: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 3 }, description: "A dense route ([x, z] or [x, y, z], in `units`): check_reachability {route: true}'s `route`, followed by pure pursuit." },
        to: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 3, description: "Plan the route to this point ([x, z] or [x, y, z], in `units`) from the worm's spot over the open project's voxels, and follow it." },
        lookahead: { type: "number", minimum: 0.25, description: "With path or to: aim this far along the route ahead of the worm (default 2 map units)." },
        bodyWidth: { type: "number", minimum: 0, description: "With to: the worm's width the planned route must fit through (default 0.6 map units, a worm's body; 0 not enforced)." },
        recover: { type: "boolean", description: "With path or to: when stuck, try a jump, then a side step, then a backflip, in turn (default true); false: only jump (as waypoints do)." },
        replan: { type: "boolean", description: "With path and a project open: after each round of recoveries that leaves the worm stuck, back off its heading and plan a route over the open project's voxels (check_reachability's walk graph, the worm's width enforced) from where it is to the path a little further on, and follow that before the rest of the path, at most 4 times a walk (default true). The pursuit aims straight along the path, so a path that cuts a corner past a pillar or a keep held the worm against it for whole turns (Caldera v2 round 1: Ashgate's WORM2, 4 turns) while jumps and side steps kept the same heading." },
        units: { type: "string", enum: ["map", "world"], description: "Units of the route and the answer (default map)." },
        worm: { type: "integer", minimum: 0, maximum: 15, description: "Walk only this WORMn: wait for its turn (default: the first human turn that comes)." },
        arrive: { type: "number", minimum: 0.2, description: "The end (or a waypoint) is reached within this horizontal distance (units; default 1.5 map units)." },
        timeoutSec: { type: "number", minimum: 1, maximum: 300, description: "Give up after this long, waiting included (default 120)." },
        jump: { type: "boolean", description: "Tap jump where the worm makes no progress for 0.6 s (default true; false: no recovery at all)." },
        gameDir: { type: "string", description: "The running game's folder (WormsMayhem.exe), for its Automation command file (default MELANGE_GAME_DIR); only for driver keys." },
        driver: { type: "string", enum: ["auto", "input", "keys"], description: "How the worm is moved: input posts the game's own movement inputs (Input.MoveForward ... Pressed and Released, as the keys bound to them post them) from the input service's tick (input.drive, Melange 0.13+), so it works whatever window has the focus; keys injects W A S D through the Automation module (test builds), which the game reads only while its window has the focus (in the Caldera iteration-10 Test the window lost it a third of the time and W moved no worm). auto (default): input when the game has input.drive, else keys." },
      },
    },
    async handler(s, a: { waypoints?: number[][]; path?: number[][]; to?: number[]; lookahead?: number; bodyWidth?: number; recover?: boolean; replan?: boolean; units?: "map" | "world"; worm?: number; arrive?: number;
      timeoutSec?: number; jump?: boolean; gameDir?: string; driver?: "auto" | "input" | "keys" }) {
      const given = [a.waypoints, a.path, a.to].filter((x) => x !== undefined).length;
      if (given !== 1) throw new ToolError("give one of waypoints, path or to");
      const hasDrive = await has(s, "input.drive");
      if (a.driver === "input" && !hasDrive) throw new ToolError("driver input needs the game's input.drive (Melange 0.13+ with the Controls module): use driver keys");
      const byInput = a.driver === "input" || (a.driver !== "keys" && hasDrive);
      const file = byInput ? null : automationFile(a.gameDir);
      const k = a.units === "world" ? 1 : WORLD_PER_MAP;   // to world units
      const out = (v: number) => Math.round((v / k) * 100) / 100;
      const xz = (p: number[]) => (p.length === 3 ? [p[0] * k, p[2] * k] : [p[0] * k, p[1] * k]);
      if (a.to && !s.project) throw new ToolError("to plans over the open project's voxels: open_project first (or give path)");
      let pts = (a.waypoints ?? a.path ?? []).map(xz);
      // a path's heights (world units; NaN without): a replanned stretch ends on the path's own leg of a switchback
      let ys = (a.path ?? []).map((p) => (p.length === 3 ? p[1] * k : NaN));
      if (!pts.every((p) => p.every(Number.isFinite)) || (a.to && !a.to.every(Number.isFinite))) throw new ToolError("the route's points are finite numbers");
      const follow = a.waypoints ? "waypoints" : "path";
      const arrive = (a.arrive ?? (a.units === "world" ? 30 : 1.5)) * k, limit = (a.timeoutSec ?? 120) * 1000, t0 = Date.now();
      const look = (a.lookahead ?? (a.units === "world" ? 40 : 2)) * k;
      const recover = a.jump !== false && a.recover !== false && follow === "path";
      const send = (lines: string[]) => { if (file) appendFileSync(file, lines.join("\n") + "\n"); };
      if (file) {
        // Automation reads the file every frame and removes what it ran: an unread line means the module is off
        const marker = `log erg-walk-${t0}`;
        send([marker]);
        await sleep(1500);
        if (readFileSync(file, "utf8").includes(marker)) throw new ToolError(`the game did not read ${file}: game_walk needs a Melange with the Automation module ([Automation] Enabled=1, a test build) running from that folder`);
      }
      // the game's own jump and backflip keys (its bindings), else Enter and Backspace (driver keys); driver input posts
      // the inputs themselves (Input.Jump, Input.JumpBack)
      const bound = byInput ? "nil;nil" : await evalClient(s, `local b = wum.input and wum.input.binding return tostring(b and b("Input.JumpPressed")) .. ";" .. tostring(b and b("Input.JumpBackPressed"))`).catch(() => "nil;nil");
      const [jl, bl] = bound.split(";");
      const keys = byInput ? { jump: "Input.Jump", backflip: "Input.JumpBack", source: "the game's own inputs (input.drive)" }
        : { jump: keyOfLabel(jl === "nil" ? null : jl) ?? "RETURN", backflip: keyOfLabel(bl === "nil" ? null : bl) ?? "BACK",
          source: jl !== "nil" || bl !== "nil" ? `the game's bindings (${jl}, ${bl})` : "defaults (Enter, Backspace): the game's bindings were not read" };
      // driver input: the held inputs are posted again every tick with a short deadline (a lost erg-mcp stops the worm)
      const client = byInput ? await s.client() : null;
      const MOVE: Record<string, string> = { W: "MoveForward", A: "MoveLeft", S: "MoveBackward", D: "MoveRight" };
      let tap: { name: string; until: number } | null = null, drivePosts = 0;
      const drive = async (want: string[]) => {
        const names = want.map((x) => MOVE[x]);
        if (tap && Date.now() < tap.until) names.push(tap.name);
        const r = await client!.call<{ posts: number }>("input.drive", names.length ? { hold: names, ms: 500, replace: true } : { release: true }, 15000);
        drivePosts = r.posts;
      };
      const press = async (what: "jump" | "backflip") => {
        if (!byInput) { send([`key ${what === "jump" ? keys.jump : keys.backflip} 80`]); return; }
        tap = { name: what === "jump" ? "Jump" : "JumpBack", until: Date.now() + 100 };
        await drive(held);
      };
      type W = { x: number; y: number; z: number; slot: number; team: number; phase: string; ai: boolean; alive: boolean; fx: number; fz: number };
      const read = async (): Promise<W | string> => {
        const t = await evalClient(s, WALK_LUA);
        if (t.startsWith("none")) return t;
        const v = t.split(",");
        return { x: +v[0], y: +v[1], z: +v[2], slot: +v[3], team: +v[4], phase: v[5], ai: v[6] === "true", alive: v[7] === "true", fx: +v[8], fz: +v[9] };
      };
      // the turn to walk in: a human team's, in play (the Ready? intro is skipped with RETURN)
      let w: W | string = await read();
      for (;;) {
        if (Date.now() - t0 > limit) throw new ToolError(`no ${a.worm !== undefined ? `turn of WORM${a.worm}` : "human turn"} in play within ${Math.round(limit / 1000)} s (last: ${typeof w === "string" ? w : `WORM${w.slot} team ${w.team} ${w.phase}${w.ai ? " ai" : ""}`})`);
        if (typeof w !== "string" && !w.ai && (a.worm === undefined || w.slot === a.worm)) {
          if (w.phase === "play") break;
          if (w.phase === "intro") send(["key RETURN 60"]);   // driver keys skips the Ready? intro; input waits it out
        }
        await sleep(400);
        w = await read();
      }
      const start = w as W;
      // with to: the route planned from where the worm stands (map units), again from where it is when it stays stuck
      const plans: { from: number[]; length: number; narrowest: number | null; points: number }[] = [];
      const plan = (from: W) => {
        const r = planWalk(s.project!.store, [from.x / WORLD_PER_MAP, from.y / WORLD_PER_MAP, from.z / WORLD_PER_MAP], [a.to![0] * k / WORLD_PER_MAP, (a.to!.length === 3 ? a.to![2] : a.to![1]) * k / WORLD_PER_MAP],
          { radius: arrive / WORLD_PER_MAP, bodyWidth: a.bodyWidth === undefined ? WORM_WIDTH : (a.bodyWidth * k) / WORLD_PER_MAP });
        if ("why" in r) return r.why;
        plans.push({ from: [from.x, from.y, from.z].map(out), length: out(r.plan.length * WORLD_PER_MAP), narrowest: r.plan.narrowest === null ? null : out(r.plan.narrowest * WORLD_PER_MAP), points: r.plan.path.length });
        return r.plan.path.map((p) => [p[0] * WORLD_PER_MAP, p[2] * WORLD_PER_MAP]);
      };
      if (a.to) {
        const p = plan(start);
        if (typeof p === "string") throw new ToolError(`to: ${p}`);
        pts = p;
      }
      // path: when stuck after a round of recoveries, a route from the worm to the path a little further on (world units
      // in and out), spliced in front of the rest of the path
      const replanPath = follow === "path" && !a.to && a.replan !== false && recover && !!s.project;
      const detour = (r: W, fromAlong: number): string | null => {
        const want = fromAlong + Math.max(look * 3, 6 * WORLD_PER_MAP);
        let j = 0;
        while (j < pts.length - 1 && cum[j + 1] < want) j++;
        const t = j < pts.length - 1 && cum[j + 1] > cum[j] ? Math.min(1, Math.max(0, (want - cum[j]) / (cum[j + 1] - cum[j]))) : 0;
        const tx = j < pts.length - 1 ? pts[j][0] + (pts[j + 1][0] - pts[j][0]) * t : pts[j][0], tz = j < pts.length - 1 ? pts[j][1] + (pts[j + 1][1] - pts[j][1]) * t : pts[j][1];
        const ty = j < pts.length - 1 && Number.isFinite(ys[j]) && Number.isFinite(ys[j + 1]) ? ys[j] + (ys[j + 1] - ys[j]) * t : ys[j];
        // round what held it: the spots it got stuck on are not walked, and a route twice the worm's width is tried first
        // (a route along a keep's wall at the worm's own width held Strand's WORM0 in its corner, Caldera v2 round 2)
        const body = a.bodyWidth === undefined ? WORM_WIDTH : (a.bodyWidth * k) / WORLD_PER_MAP;
        const avoid = heldAhead.slice(-6).map((q) => ({ x: q[0] / WORLD_PER_MAP, z: q[1] / WORLD_PER_MAP, r: 0.8 }));
        let r2: ReturnType<typeof planWalk> = { why: "not planned" };
        for (const [bw, av] of [[body * 2, avoid], [body, avoid], [body, []]] as [number, typeof avoid][]) {
          r2 = planWalk(s.project!.store, [r.x / WORLD_PER_MAP, r.y / WORLD_PER_MAP, r.z / WORLD_PER_MAP], [tx / WORLD_PER_MAP, tz / WORLD_PER_MAP],
            { radius: 1.5, bodyWidth: bw, ...(av.length ? { avoid: av } : {}), ...(Number.isFinite(ty) ? { toY: ty / WORLD_PER_MAP } : {}) });
          if ("plan" in r2) break;
        }
        if ("why" in r2) return r2.why;
        const lead = r2.plan.path.map((p) => [p[0] * WORLD_PER_MAP, p[2] * WORLD_PER_MAP]);
        plans.push({ from: [r.x, r.y, r.z].map(out), length: out(r2.plan.length * WORLD_PER_MAP), narrowest: r2.plan.narrowest === null ? null : out(r2.plan.narrowest * WORLD_PER_MAP), points: lead.length });
        pts = [...lead, [tx, tz], ...pts.slice(j + 1)];
        ys = [...r2.plan.path.map((p) => p[1] * WORLD_PER_MAP), ty, ...ys.slice(j + 1)];
        return null;
      };
      let cum = cumulative(pts);
      const path: number[][] = [[start.x, start.y, start.z]], stuck: number[][] = [], tries: string[] = [], heldAhead: number[][] = [];
      let i = 0, seg = 0, along = 0, held: string[] = [], lastMove = Date.now(), lastPos = [start.x, start.z], end: W = start, why = "timeout", jumps = 0, ladder = 0, replans = 0;
      const hold = async (want: string[]) => {
        if (byInput) {
          held = want;
          await drive(want);
          return;
        }
        const lines = [...held.filter((x) => !want.includes(x)).map((x) => `up ${x}`), ...want.filter((x) => !held.includes(x)).map((x) => `down ${x}`)];
        if (lines.length) send(lines);
        held = want;
      };
      try {
        while (Date.now() - t0 < limit) {
          const r = await read();
          if (typeof r === "string") { why = "the turn ended"; break; }
          if (r.slot !== start.slot || r.phase !== "play") { why = r.slot !== start.slot ? "the turn ended" : `the turn left play (${r.phase})`; break; }
          if (!r.alive) { why = "the worm died"; end = r; break; }
          end = r;
          if (Math.hypot(r.x - path[path.length - 1][0], r.z - path[path.length - 1][2]) >= 10) path.push([r.x, r.y, r.z]);
          let aim: number[];
          if (follow === "waypoints") {
            while (i < pts.length && Math.hypot(pts[i][0] - r.x, pts[i][1] - r.z) <= arrive) i++;
            if (i >= pts.length) { why = "arrived"; break; }
            aim = pts[i];
          } else {
            const last = pts[pts.length - 1];
            if (Math.hypot(last[0] - r.x, last[1] - r.z) <= arrive) { why = "arrived"; along = cum[cum.length - 1]; break; }
            const pp = pursue(pts, cum, seg, [r.x, r.z], look, look * 4 + 6 * WORLD_PER_MAP);
            seg = pp.seg;
            along = Math.max(along, pp.along);
            aim = pp.aim;
          }
          await hold(walkKeys(aim[0] - r.x, aim[1] - r.z, r.fx, r.fz));
          if (Math.hypot(r.x - lastPos[0], r.z - lastPos[1]) > 4) { lastPos = [r.x, r.z]; lastMove = Date.now(); ladder = 0; }
          else if (Date.now() - lastMove > 600) {
            stuck.push([r.x, r.y, r.z]);
            // what held it: the ground a map unit ahead on the heading it was stuck on (a replan keeps off it)
            { const dx = aim[0] - r.x, dz = aim[1] - r.z, l = Math.hypot(dx, dz) || 1; heldAhead.push([r.x + (dx / l) * WORLD_PER_MAP, r.z + (dz / l) * WORLD_PER_MAP]); }
            if (a.jump !== false) {
              const move = recover ? ladder % 3 : 0;
              if (move === 0) { await press("jump"); jumps++; tries.push("jump"); }
              else if (move === 1) {
                // a side step: a short walk square to the aim, the side alternating, to come off a pillar's or a corner's edge
                const side = Math.floor(ladder / 3) % 2 ? 1 : -1, dx = aim[0] - r.x, dz = aim[1] - r.z;
                await hold(walkKeys(-dz * side, dx * side, r.fx, r.fz));
                await sleep(400);
                tries.push("side step");
              } else {
                await press("backflip");
                tries.push("backflip");
              }
              ladder++;
              // planned routes: from where it is now, after a whole round of recoveries
              if (a.to && ladder % 3 === 0 && replans < 4) {
                const p = plan(r);
                if (typeof p !== "string") { pts = p; cum = cumulative(pts); seg = 0; replans++; tries.push("replanned"); }
              }
              // a path: off the heading it is stuck on (a short walk straight back), then a route round what holds it
              if (replanPath && ladder % 3 === 0 && replans < 4) {
                await hold(walkKeys(r.x - aim[0], r.z - aim[1], r.fx, r.fz));
                await sleep(450);
                await hold([]);
                const at = await read();
                const why2 = detour(typeof at === "string" ? r : at, along);
                if (why2 === null) { cum = cumulative(pts); seg = 0; along = 0; replans++; tries.push("backed off, replanned"); }
                else tries.push(`replan failed: ${why2}`);
              }
            }
            lastMove = Date.now();
          }
          await sleep(WALK_TICK_MS);
        }
      } finally {
        if (byInput) await client!.call("input.drive", { release: true }, 15000).catch(() => undefined);
        else await hold([]);
      }
      const last = pts[pts.length - 1];
      const near = (p: number[]) => p.map(out);
      const reached = follow === "waypoints" ? { waypoints: i, of: pts.length, target: i >= pts.length }
        : { along: out(along), of: out(cum[cum.length - 1]), target: why === "arrived" };
      return {
        worm: `WORM${start.slot}`, team: start.team, follow: a.to ? "to" : follow, reached, stop: why,
        from: near([start.x, start.y, start.z]), to: near([end.x, end.y, end.z]), toTarget: out(Math.hypot(last[0] - end.x, last[1] - end.z)), climb: out(end.y - start.y),
        seconds: Math.round((Date.now() - t0) / 100) / 10, jumps, ...(tries.length ? { recoveries: tries.slice(0, 40) } : {}), ...(stuck.length ? { stuckAt: stuck.slice(0, 12).map(near) } : {}),
        ...(plans.length ? { plans } : {}), ...(follow === "path" ? { keys } : {}), path: path.slice(0, 120).map(near),
        driver: byInput ? { kind: "input", posts: drivePosts, note: "the game's own movement inputs, posted from its input tick (input.drive): the window's focus does not matter" }
          : { kind: "keys", note: "W A S D through the Automation module: the game reads them only while its window has the focus" },
        units: a.units ?? "map", note: follow === "waypoints"
          ? "the worm walked with the game's own controls (camera-relative W A S D, steered every 0.12 s toward the next waypoint; jump where it stopped): reached.target means it got to the last waypoint on foot in game"
          : "the worm followed the route with the game's own controls (camera-relative W A S D, steered every 0.12 s at the point `lookahead` along the route ahead of it; jump, side step and backflip where it stopped): reached.target means it got to the route's end on foot in game",
      };
    },
  },
  {
    name: "game_drive",
    description: "Drive the active worm of the running Test for a while with the game's own inputs, whatever window has the focus: waits for a human team's turn in play (`worm`: that WORMn's), then holds `inputs` for `seconds` (W, A, S, D: the camera-relative walk, as the keys; Jump, Backflip, Fire (held: the power), AimUp, AimDown), posted as the keys bound to them post them from the game's input tick (input.drive, Melange 0.13+, the Controls module), and answers where the worm was before and after, how far it moved and climbed. The simple check that a worm walks in the game (game_walk follows a whole route the same way).",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["inputs"],
      properties: {
        inputs: { type: "array", minItems: 1, maxItems: 6, items: { type: "string", enum: ["W", "A", "S", "D", "Jump", "Backflip", "Fire", "AimUp", "AimDown", "FireUtil", "TurnLeft", "TurnRight", "JetForward", "JetBack", "JetLeft", "JetRight", "Fire2"] }, description: "What to hold (two walk keys walk the diagonal). FireUtil is the selected UTILITY's fire (Melange 0.15+): held, it is a jetpack's thrust (game_select_weapon kUtilityJetpack first; W, A, S, D steer it in the air and its fuel lasts 7.5 s of thrust), where Fire is the weapon's. TurnLeft and TurnRight turn the worm; JetForward, JetBack, JetLeft and JetRight are the jetpack's own steering inputs." },
        seconds: { type: "number", minimum: 0.05, maximum: 20, description: "How long to hold them (default 2)." },
        worm: { type: "integer", minimum: 0, maximum: 15, description: "Only this WORMn's turn (default the first human turn in play)." },
        waitSec: { type: "number", minimum: 1, maximum: 300, description: "Wait at most this long for the turn (default 90)." },
        units: { type: "string", enum: ["map", "world"], description: "Units of the answer (default map)." },
      },
    },
    async handler(s, a: { inputs: string[]; seconds?: number; worm?: number; waitSec?: number; units?: "map" | "world" }) {
      if (!(await has(s, "input.drive"))) throw new ToolError("game_drive needs the game's input.drive (Melange 0.13+ with the Controls module)");
      const NAME: Record<string, string> = { W: "MoveForward", A: "MoveLeft", S: "MoveBackward", D: "MoveRight", Jump: "Jump", Backflip: "JumpBack", Fire: "Fire", AimUp: "AimUp", AimDown: "AimDown",
        FireUtil: "FireUtil", TurnLeft: "TurnLeft", TurnRight: "TurnRight", JetForward: "Jetpack.Forward", JetBack: "Jetpack.Back", JetLeft: "Jetpack.Left", JetRight: "Jetpack.Right", Fire2: "Fire.Second" };
      const k = a.units === "world" ? 1 : 1 / WORLD_PER_MAP, out = (v: number) => Math.round(v * k * 100) / 100;
      const client = await s.client(), t0 = Date.now(), wait = (a.waitSec ?? 90) * 1000;
      type W = { x: number; y: number; z: number; slot: number; team: number; phase: string; ai: boolean; alive: boolean };
      const read = async (): Promise<W | string> => {
        const t = await evalClient(s, WALK_LUA);
        if (t.startsWith("none")) return t;
        const v = t.split(",");
        return { x: +v[0], y: +v[1], z: +v[2], slot: +v[3], team: +v[4], phase: v[5], ai: v[6] === "true", alive: v[7] === "true" };
      };
      let w = await read();
      while (typeof w === "string" || w.ai || w.phase !== "play" || (a.worm !== undefined && w.slot !== a.worm)) {
        if (Date.now() - t0 > wait) throw new ToolError(`no ${a.worm !== undefined ? `turn of WORM${a.worm}` : "human turn"} in play within ${Math.round(wait / 1000)} s (last: ${typeof w === "string" ? w : `WORM${w.slot} team ${w.team} ${w.phase}${w.ai ? " ai" : ""}`})`);
        await sleep(300);
        w = await read();
      }
      const start = w, names = a.inputs.map((x) => NAME[x]), hold = (a.seconds ?? 2) * 1000, t1 = Date.now();
      let end: W = start, why = "held", posts = 0;
      try {
        while (Date.now() - t1 < hold) {
          const r = await client.call<{ posts: number }>("input.drive", { hold: names, ms: Math.min(500, Math.max(50, hold - (Date.now() - t1))), replace: true }, 15000);
          posts = r.posts;
          await sleep(Math.min(200, hold));
          const now = await read();
          if (typeof now === "string" || now.slot !== start.slot || now.phase !== "play") { why = "the turn left play"; break; }
          end = now;
          if (!now.alive) { why = "the worm died"; break; }
        }
      } finally {
        await client.call("input.drive", { release: true }, 15000).catch(() => undefined);
      }
      await sleep(400);
      const after = await read();
      if (typeof after !== "string" && after.slot === start.slot) end = after;
      return { worm: `WORM${start.slot}`, team: start.team, inputs: a.inputs, heldMs: Date.now() - t1, stop: why, posts,
        from: [start.x, start.y, start.z].map(out), to: [end.x, end.y, end.z].map(out), moved: out(Math.hypot(end.x - start.x, end.z - start.z)), climb: out(end.y - start.y),
        units: a.units ?? "map", note: "the inputs were posted from the game's input tick as their keys post them (input.drive): no keyboard and no window focus involved" };
    },
  },
  {
    name: "game_play_turns",
    description: "Play the running Test's turns with every human worm, one call for a whole run: for each turn it takes the active worm (a human team's; a computer team's turn is waited out), walks it along its team's trail (`trails`: a route per team, such as its castle's switchbacks to the summit) from the point of the trail nearest the worm with game_walk's driver (the game's own inputs through input.drive, pure pursuit along the trail; jump, side step and backflip where it is stuck) for at most `walkSeconds`, shoots that worm at the start, the middle and the end of its walk (the game's own turn camera, which follows the active worm; render.shot, clean), then ends the turn (`end`: skip, the default where the game has level.test.endTurn: Timer.TurnTimedOut, the turn timer's own message; fire: Input.Fire held `fireMs`, `weapon` selecting one first; wait: the turn time runs out) and goes on, for `turns` turns or `rounds` rounds (a round: every living worm once). Answers each turn (worm, team, from, to, along/of its trail, climb, stop, recoveries, shots, how it ended) and per worm its progress over the run. Make the match with test {teams: [{human: true, worms: 2}, ... x4]} for four human teams; with spawns.mode teams each team starts on its own knots. Needs input.drive and render.shot (Melange 0.13+).",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["trails"],
      properties: {
        trails: { type: "array", minItems: 1, maxItems: 4, items: { type: "array", minItems: 2, maxItems: 4000, items: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 3 } },
          description: "One trail per team (trails[t] for team t; a team without one takes the last given): points [x, z] or [x, y, z] in `units` (give y: a switchback's legs over each other are told apart by it), followed by pure pursuit from the point nearest the worm, so each worm carries on from where it stands." },
        turns: { type: "integer", minimum: 1, maximum: 200, description: "Turns to play (default: `rounds` rounds)." },
        rounds: { type: "integer", minimum: 1, maximum: 20, description: "Rounds to play when turns is not given: each living worm moves once a round (default 2)." },
        walkSeconds: { type: "number", minimum: 2, maximum: 120, description: "Walk each worm at most this long (default 20; a Quick Game turn is about 45 s)." },
        end: { type: "string", enum: ["skip", "fire", "wait"], description: "How a turn ends after the walk: skip (default where the game has level.test.endTurn), fire (Input.Fire held fireMs), wait (the turn time runs out)." },
        weapon: { type: "string", description: "With end fire: take this weapon first (game_select_weapon: level.test.selectWeapon on this branch's Melange, any weapon by engine name such as kWeaponBazooka or bazooka, or an id; an older game takes the bazooka alone, from the weapon panel's first cell through the Automation module: test builds, the game window focused, gameDir or MELANGE_GAME_DIR). A turn starts with no weapon, so fire without it fires nothing. Writing the worm's weapon into its container instead hung the turn (2026-11-08)." },
        gameDir: { type: "string", description: "With weapon: the running game's folder, for its Automation command file (default MELANGE_GAME_DIR)." },
        fireMs: { type: "integer", minimum: 50, maximum: 3000, description: "With end fire: how long fire is held, the shot's power (default 300)." },
        aimDownMs: { type: "integer", minimum: 0, maximum: 3000, description: "With end fire: hold AimDown this long first (default 0)." },
        shots: { type: "boolean", description: "Shoot each worm at the start, middle and end of its walk (default true)." },
        shotPose: { type: "string", enum: ["near", "game"], description: "near (default): each shot on the worm where it is then, all of a turn's from the same side of its heading along the trail (10 map units off, 11 up, over castle walls), looking down at it, so the ground behind it shows how far it went; game: the game's own turn camera (low and far back, following the worm: a cone or a wall can hide it)." },
        walkAhead: { type: "number", minimum: 1, maximum: 40, description: "With shotPose near: the heading is from the worm's spot to this far along its trail (map units, default 6)." },
        clearView: { type: "boolean", description: "With shotPose near and a project open: each shot's eye is checked against the project's voxels and, when it is inside the ground, within 0.75 map units of it, or has voxels between it and the worm, moved to the nearest eye in open air with a clear view (raised, nearer or farther, turned round the worm); a turn's eyesMoved lists the moves (default true)." },
        suddenDeath: { type: "boolean", description: "false: hold the round clock for the run (level.test.roundTime {hold}: paused now and at every turn start), so the scheme's sudden death never comes; a run of many rounds otherwise meets the Quick Game's at 30 minutes, which raises the water past a level's cap and sets every worm's health to 1 (Caldera v2 round 2). The hold lasts until the Test ends; test {suddenDeath: false} sets it from the start. Default: as the match is." },
        approach: { type: "boolean", description: "A worm more than 1.5 map units off its trail first walks a route planned over the open project's voxels (check_reachability's walk graph, the worm's width enforced) to the trail's point nearest it, then the trail (default true with a project open; the turn's `approach` says what was planned). Without it the pursuit heads straight for the trail, through a keep's pillar if one is in the way." },
        replan: { type: "boolean", description: "game_walk's replan: a worm stuck after a round of recoveries backs off and walks a route planned round what holds it to its trail further on (default true with a project open)." },
        announceShots: { type: "boolean", description: "While the turns play, shoot the game's own view (not clean: the message is drawn with the HUD) each time a wum.sim.announce message is first drawn at full opacity (the game's announce.shown record, Melange of this branch): <prefix>_ann<n>.png, listed in the answer's `announcements` with the text and the frames (default true)." },
        prefix: { type: "string", description: "Shot names <prefix>_t<turn>_w<slot>_<start|mid|end> (default erg_<time>_play)." },
        width: { type: "integer", minimum: 160, maximum: 1920, description: "Shot width (default 960)." },
        units: { type: "string", enum: ["map", "world"], description: "Units of the trails and the answer (default map)." },
        timeoutSec: { type: "number", minimum: 10, maximum: 3600, description: "Stop the run after this long (default 1800)." },
        trace: { type: "boolean", description: "Read every worm's position about once a second through the whole run (beside the walk's own reads) and write the trace to Documents\\Melange\\erg-mcp\\traces\\<prefix>.json (`traceFile`); the answer's `offTurnMoves` lists each worm that dropped `fallDrop` or moved 2 map units while another worm played, with the trace's samples, its speed and what the Test probe saw beside it then (explosions with their source and the turn's weapon, mines at rest near it, its death), so a fall between a worm's own turns is explained (Caldera v2 round 4: WORM5 from 55.4 to 44.2 between turns 21 and 29). Default true." },
        fallDrop: { type: "number", minimum: 0.25, description: "With trace: a worm not playing that loses this much height (units) between two samples is an off-turn move (default 1.5 map units)." },
        stuckShots: { type: "boolean", description: "A turn whose walk ended with the turn leaving play (settle: the worm fell or slid) or needed more than 2 jumps gets `stuckPoint`: the exact spot it was last stuck at (or ended at), its place along and off the trail, the ground there (the project's voxels), and a close shot of it from 4 map units off, <prefix>_t<turn>_w<slot>_stuck.png, so a trail defect is located without guessing (default true)." },
      },
    },
    async handler(s, a: { trails: number[][][]; turns?: number; rounds?: number; walkSeconds?: number; end?: "skip" | "fire" | "wait"; weapon?: string; fireMs?: number; aimDownMs?: number;
      shots?: boolean; shotPose?: "near" | "game"; walkAhead?: number; prefix?: string; width?: number; units?: "map" | "world"; timeoutSec?: number; gameDir?: string;
      approach?: boolean; replan?: boolean; announceShots?: boolean; clearView?: boolean; suddenDeath?: boolean; trace?: boolean; fallDrop?: number; stuckShots?: boolean }) {
      if (!(await has(s, "input.drive"))) throw new ToolError("game_play_turns drives the worms with input.drive (Melange 0.13+ with the Controls module)");
      const roundHeld = a.suddenDeath === false ? await holdRoundClock(s) : undefined;
      const canSkip = await has(s, "level.test.endTurn");
      const end = a.end ?? (canSkip ? "skip" : "wait");
      if (end === "skip" && !canSkip) throw new ToolError("end skip needs level.test.endTurn (this branch's Melange): use end wait or fire");
      if (a.weapon && end !== "fire") throw new ToolError("weapon selects what end fire fires: give end: \"fire\"");
      // the weapon: level.test.selectWeapon where the game has it, else the Automation panel clicks (a gameDir is needed then)
      if (a.weapon && !(await has(s, "level.test.selectWeapon"))) automationFile(a.gameDir);
      const k = a.units === "world" ? 1 : WORLD_PER_MAP, out = (v: number) => Math.round((v / k) * 100) / 100;
      const trails = a.trails.map((t) => t.map((p) => p.map((v) => v * k)));   // world units
      if (!trails.every((t) => t.every((p) => p.every(Number.isFinite)))) throw new ToolError("trails: finite numbers");
      const trailOf = (team: number) => trails[Math.min(team, trails.length - 1)];
      const client = await s.client(), t0 = Date.now(), limit = (a.timeoutSec ?? 1800) * 1000, prefix = a.prefix ?? `erg_${stamp()}_play`;
      const walk = tools.find((t) => t.name === "game_walk")!;
      type Turn = { turn: number; worm: string; slot: number; team: number; from: number[]; to?: number[]; along?: number; of?: number; climb?: number; stop?: string;
        recoveries?: string[]; shots: string[]; ended?: string; error?: string; positionAfterError?: string; weapon?: string; weaponVia?: string; stuck?: string; stuckPoint?: object; approach?: object; replans?: number; eyesMoved?: string[] };
      const played: Turn[] = [];
      // the trace: every worm about once a second through the run, read beside the walk's own reads (lib/play-trace.ts);
      // a lost read is passed over, the next sample comes a second later
      const samples: TraceSample[] = [];
      let tracing = a.trace !== false;
      const sampler = tracing ? (async () => {
        while (tracing) {
          try {
            const smp = parseTrace(await evalClient(s, TRACE_LUA), Date.now(), WORLD_PER_MAP);
            if (smp && smp.worms.length) samples.push(smp);
          } catch { /* the next sample */ }
          await sleep(1000);
        }
      })() : null;
      const lastTraced = (slot: number): number[] | null => {
        for (let i = samples.length - 1; i >= 0; i--) { const w = samples[i].worms.find((x) => x.slot === slot); if (w) return w.pos.map((v) => v * WORLD_PER_MAP); }
        return null;
      };
      // shotPose near: each shot on the worm where it is then, all of a turn's from the same side of its heading (the
      // trail's direction from its spot, walkAhead along it) and high over castle walls, so the ground behind it shows how
      // far it went (a pose fixed for the turn lost a worm that walked 36 map units in 12 s); the game's own turn camera
      // sits low and far back (a cone or a wall between hid the worm in the first run). game: that camera.
      const near = a.shotPose !== "game";
      const wormAt = async (slot: number) => (await evalClient(s, `for _, w in ipairs(wum.game.worms() or {}) do if w.slot == ${slot} then return w.pos.x .. "," .. w.pos.y .. "," .. w.pos.z end end return ""`)).split(",").map(Number);
      const shotAt = async (name: string, list: string[], aim: { slot: number; heading: number[] } | null, views?: string[]) => {
        if (a.shots === false) return;
        try {
          let minFrame: number | undefined;
          const q = near && aim ? await wormAt(aim.slot) : [];
          if (q.length === 3 && q.every(Number.isFinite)) {
            let pose = sidePose(q, aim!.heading);
            // the side pose checked against the open project's voxels: in the Caldera v2 round-2 run it stood inside the
            // cone's rock or behind a castle wall (c1_t3_w0_start, c1_t1_w4_mid, c1_t3_w0_end); moved to the nearest eye in
            // open air with a clear line of sight to the worm (raised, nearer or farther, turned round it)
            if (a.clearView !== false && s.project) {
              const m = (v: number[]) => v.map((x) => x / WORLD_PER_MAP), ce = clearEye(grids(s.store), m(pose.pos), m(pose.target), { skip: 1 });
              if (ce.moved) { pose = { ...pose, pos: ce.eye.map((x) => x * WORLD_PER_MAP) }; views?.push(`${name}: ${ce.moved}`); }
              else if (!ce.clear) views?.push(`${name}: no eye near the side pose has a clear view of the worm (shot as posed)`);
            }
            // never under the risen sea: a late turn's worm at the old shore had its side pose under the water
            const ow = overWater(pose, (await gameState(s).catch(() => ({} as GameState))).water);
            if (ow.lifted) { pose = ow.pose; views?.push(`${name}: ${ow.lifted}`); }
            const set = await setCamera(s, { ...pose, fov: 60 }, 40);
            minFrame = set.frame !== undefined ? set.frame + 2 : undefined;
          }
          list.push((await shot(s, uniqueName(prefix, name), { clean: true, width: a.width ?? 960, minFrame })).path);
          if (minFrame !== undefined) await clearCamera(s).catch(() => undefined);
        } catch (e) {
          list.push(`${name}: failed: ${(e as Error).message}`);
        }
      };
      let st = await gameState(s);
      noteTurn(st);
      if (st.scene !== "match") throw new ToolError(`no match is running (scene ${st.scene}): test first`);
      // the level script's announcements: a shot of the game's own view (with its HUD) as each is first drawn at full
      // opacity; the log channel's backlog (records from before this call) is passed over
      const announced: { text: string; shownFrame: number; shot?: string; frame?: number; error?: string }[] = [], pendingShots: Promise<void>[] = [];
      const stopAnn = a.announceShots !== false && (await has(s, "render.shot")) ? await followAnnouncements(s, () => undefined, (x) => {
        if (x.wall < t0 - 500) return;
        const rec = { text: x.text, shownFrame: x.frame } as (typeof announced)[number];
        announced.push(rec);
        pendingShots.push(shot(s, uniqueName(prefix, `ann${announced.length}`), { clean: false, width: a.width ?? 960, minFrame: x.frame + 1 })
          .then((r) => { rec.shot = r.path; rec.frame = r.frame; }, (e: Error) => { rec.error = e.message; }));
      }).catch(() => null) : null;
      const alive = Object.values(st.aliveByTeam).reduce((x, y) => x + y, 0);
      const want = a.turns ?? (a.rounds ?? 2) * Math.max(1, alive);
      let lastTurn = -1, why = "done", cancelMid = () => {};
      try {
        while (played.length < want) {
          if (Date.now() - t0 > limit) { why = "timeout"; break; }
          st = await gameState(s);
          noteTurn(st);
          if (st.scene !== "match") { why = `the match ended (scene ${st.scene})`; break; }
          if (st.turn === undefined || st.turn === lastTurn || st.phase !== "play" || st.worm === undefined) { await sleep(300); continue; }
          if (st.ai) { lastTurn = st.turn; await sleep(500); continue; }   // a computer team's turn is not steered
          lastTurn = st.turn;
          const turn = st.turn, slot = st.worm, team = st.team ?? 0, trail = trailOf(team);
          const p = await wormAt(slot);
          const rec: Turn = { turn, worm: `WORM${slot}`, slot, team, from: p.length === 3 ? p.map(out) : [], shots: [] };
          played.push(rec);
          const rest = p.length === 3 && p.every(Number.isFinite) ? restOfTrail(trail, trail[0].length === 3 ? p : [p[0], p[2]]) : trail;
          // a worm off its trail (a spawn in a courtyard, knocked aside) walks a planned route to it first: pursuit straight
          // at the trail's nearest point ran Ashgate's WORM2 into the keep's pillar for 4 turns (Caldera v2 round 1)
          const route = p.length === 3 && p.every(Number.isFinite) && a.approach !== false ? approachRoute(s, p, rest) : null;
          if (route?.why) rec.approach = { planned: false, why: route.why };
          else if (route?.lead) rec.approach = { planned: true, from: p.map(out), length: out(route.length!), points: route.lead.length };
          const pose = p.length === 3 && p.every(Number.isFinite) ? { slot, heading: turnHeading(p, rest, (a.walkAhead ?? 6) * WORLD_PER_MAP) } : null;
          const views: string[] = [];
          await shotAt(`t${turn}_w${slot}_start`, rec.shots, pose, views);
          const walkSec = a.walkSeconds ?? 20;
          // the middle shot from a timer while the walk runs (the walk steers by the game's own camera, which a shot leaves);
          // a walk that ends first cancels it, so it never fires in a later turn
          let midStarted = false;
          const midShot = new Promise<void>((ok) => {
            const timer = setTimeout(() => { midStarted = true; shotAt(`t${turn}_w${slot}_mid`, rec.shots, pose, views).then(ok, ok); }, (walkSec * 1000) / 2);
            cancelMid = () => { if (!midStarted) { clearTimeout(timer); ok(); } };
          });
          try {
            const r = await walk.handler(s, { path: route?.lead ? [...route.lead, ...rest] : rest, units: "world", worm: slot, timeoutSec: walkSec, arrive: 1.5 * WORLD_PER_MAP,
              ...(a.replan === false ? { replan: false } : {}) }) as Record<string, unknown>;
            const to = r.to as number[], from = r.from as number[];
            rec.to = to.map(out);
            rec.climb = out(to[1] - from[1]);
            rec.stop = String(r.stop);
            // a worm that did not move at all is most often inside solid ground: in the hx4 harness run (2026-11-08) the
            // wiped base's heightmap ground stood 1.5 map units over the courtyard knots and no worm could walk or jump
            if (Math.hypot(to[0] - from[0], to[2] - from[2]) < 0.3 * WORLD_PER_MAP && rec.stop !== "arrived")
              rec.stuck = "the worm did not move: is it inside solid ground (a base's heightmap ground over its spot: set_surround_mode none) or boxed in?";
            if (Array.isArray(r.recoveries)) rec.recoveries = (r.recoveries as string[]).slice(0, 12);
            if (Array.isArray(r.plans) && r.plans.length) rec.replans = r.plans.length;
            const at = alongTrail(trail, trail[0].length === 3 ? to : [to[0], to[2]]);
            rec.along = out(at.along);
            rec.of = out(at.of);
            // a walk the turn took out of play (settle: the worm fell or slid off) or that needed more than two jumps: the
            // exact spot it was last stuck at, where that is on the trail, the ground there, and a close shot of it
            const jumps = Array.isArray(r.recoveries) ? (r.recoveries as string[]).filter((x) => x === "jump").length : Number(r.jumps) || 0;
            const settled = /left play \(settle\)/.test(rec.stop);
            if (a.stuckShots !== false && (settled || jumps > 2)) {
              const stuckAt = Array.isArray(r.stuckAt) && (r.stuckAt as number[][]).length ? (r.stuckAt as number[][])[(r.stuckAt as number[][]).length - 1] : to;
              rec.stuckPoint = await stuckPointOf(s, stuckAt, trail, settled ? "the turn left play (settle): the worm fell or slid from here" : `${jumps} jumps: the worm was held here`, pose?.heading ?? [0, 1],
                `t${turn}_w${slot}_stuck`, prefix, a.width ?? 960, a.clearView !== false, out);
            }
          } catch (e) {
            rec.error = (e as Error).message;
            // (Caldera v2 round 4, turn 74) a lost lua.eval answer left the turn with no record: the worm's position is read
            // again (evalClient tries a timed-out call once more), else taken from the trace's last sample of it
            const q = await wormAt(slot).catch(() => [] as number[]);
            const pos = q.length === 3 && q.every(Number.isFinite) ? q : lastTraced(slot);
            if (pos && rec.from.length === 3) {
              rec.to = pos.map(out);
              rec.climb = out(pos[1] - rec.from[1] * k);
              const at = alongTrail(trail, trail[0].length === 3 ? pos : [pos[0], pos[2]]);
              rec.along = out(at.along);
              rec.of = out(at.of);
              rec.stop = "error";
              rec.positionAfterError = q.length === 3 ? "the worm's position read again after the error" : "the trace's last sample of the worm (the game did not answer)";
            }
          }
          // a walk that ended early (arrived, the turn ended) takes no middle shot; one under way is waited for
          cancelMid();
          await midShot;
          if (!midStarted && a.shots !== false) rec.shots.push(`mid: none (the walk ended before ${walkSec / 2} s)`);
          await shotAt(`t${turn}_w${slot}_end`, rec.shots, pose, views);
          if (views.length) rec.eyesMoved = views;
          const now = await gameState(s);
          if (now.turn === turn && now.phase === "play") {
            if (end === "skip") {
              rec.ended = await client.call("level.test.endTurn", {}, 15000).then(() => "skipped (level.test.endTurn)", (e: Error) => `endTurn refused: ${e.message}`);
            } else if (end === "fire") {
              let wp: string;
              if (a.weapon) {
                const sel = await selectWeapon(s, a.weapon, a.gameDir).then((r) => ({ ok: true as const, ...r }), (e: Error) => ({ ok: false as const, error: e.message }));
                wp = sel.ok ? String(sel.weapon) : "nil";
                rec.weaponVia = sel.ok ? `${sel.name ?? sel.weapon} by ${sel.via} in ${sel.ms} ms` : sel.error;
              } else wp = await evalClient(s, `for _, w in ipairs(wum.game.worms() or {}) do if w.slot == ${slot} then return tostring(w.weapon) end end return "nil"`).catch(() => "?");
              rec.weapon = wp;
              if (a.aimDownMs) {
                await client.call("input.drive", { hold: ["Input.AimDown"], ms: a.aimDownMs, replace: true }, 15000).catch(() => undefined);
                await sleep(a.aimDownMs + 100);
              }
              await client.call("input.drive", { hold: ["Input.Fire"], ms: a.fireMs ?? 300, replace: true }, 15000).catch(() => undefined);
              rec.ended = wp === "nil" ? "fire held, but the worm had no weapon selected (nothing fired)" : `fired (weapon ${wp}${a.weapon ? `, ${a.weapon} asked` : ""})`;
            } else rec.ended = "the turn time ran out";
          } else rec.ended = "the turn had ended";
          // the next turn: after the turn change, a shot's flight and the Ready? intro
          for (const tw = Date.now(); Date.now() - tw < 90000; await sleep(400)) {
            const n = await gameState(s);
            noteTurn(n);
            if (n.scene !== "match" || (n.turn !== undefined && n.turn !== turn)) break;
          }
        }
      } finally {
        stopAnn?.();
        tracing = false;
      }
      await Promise.all(pendingShots);
      if (sampler) await sampler;
      // the trace: written beside the pictures; the moves of worms whose turn it was not, each with what the probe saw
      let traced: Record<string, unknown> = {};
      if (a.trace !== false) {
        const file = traceFile(prefix);
        try {
          if (file) {
            mkdirSync(join(file, ".."), { recursive: true });
            writeFileSync(file, JSON.stringify({ prefix, units: "map", note: "every worm about once a second through game_play_turns: wall (ms), the sim tick, the game turn, the active worm's slot, each worm's slot, team, [x, y, z] and alive",
              samples: samples.map((x) => ({ wall: x.wall, tick: x.tick, turn: x.turn, active: x.active, worms: x.worms.map((w) => [w.slot, w.team, ...w.pos, w.alive ? 1 : 0]) })) }));
          }
        } catch { /* the answer carries the moves */ }
        // a one-sample jump and back is a wrong position read, not a move (mines round 3's second match): put back and listed
        const cleaned = cleanTrace(samples);
        const moves = offTurnMoves(cleaned.samples, { drop: a.fallDrop === undefined ? 1.5 : a.units === "world" ? a.fallDrop / WORLD_PER_MAP : a.fallDrop, glitch: 0 });
        const probe = moves.length ? await probeOf(s).catch(() => null) : null;
        const pl = probe ? { explosions: probe.probe.explosions, fires: probe.factory.fires.map((f) => ({ tick: f.tick, turn: f.turn, mines: f.mines })), deaths: probe.probe.deaths.map((d) => ({ slot: d.slot, tick: d.tick, turn: d.turn, cause: d.cause })) } : {};
        const attributed = moves.map((m) => attributeMove(m, pl));
        const ku = a.units === "world" ? WORLD_PER_MAP : 1, cv = (p: number[]) => p.map((v) => Math.round(v * ku * 100) / 100);
        traced = { traceSamples: samples.length, ...(file ? { traceFile: file } : {}),
          offTurnMoves: attributed.map((m) => ({ worm: m.worm, team: m.team, kind: m.kind, turn: m.turn, activeWorm: m.activeWorm === null ? null : `WORM${m.activeWorm}`, why: describeMove(m),
            from: cv(m.from), to: cv(m.to), drop: Math.round(m.drop * ku * 100) / 100, moved: Math.round(m.moved * ku * 100) / 100, seconds: m.seconds, fastestDrop: Math.round(m.fastestDrop * ku * 100) / 100, ticks: m.ticks,
            samples: m.samples.slice(0, 40).map((x) => ({ tick: x.tick, pos: cv(x.pos) })),
            ...(m.explosions ? { explosions: m.explosions.map((e) => ({ ...e, at: cv(e.at), distance: Math.round(e.distance * ku * 100) / 100 })) } : {}),
            ...(m.mines ? { mines: m.mines.map((x) => ({ ...x, rest: cv(x.rest), distance: Math.round(x.distance * ku * 100) / 100 })) } : {}), ...(m.death ? { death: m.death } : {}) })),
          ...(moves.length && !probe ? { offTurnNote: "the Test probe (level.test.probe) did not answer: the moves are listed without the explosions beside them" } : {}),
          ...(cleaned.glitches.length ? { traceGlitches: cleaned.glitches.slice(0, 40).map((g) => ({ worm: `WORM${g.slot}`, tick: g.tick, read: cv(g.read), was: cv(g.was) })),
            traceGlitchNote: `${cleaned.glitches.length} sample(s) read a worm 20 map units or more from where it stood and the next sample had it back: wrong position reads (mines round 3's second match: WORM0 at other worms' spots for one sample), put back before the off-turn moves were taken` } : {}),
          ...(moves.length ? {} : { offTurnNote: `no worm dropped ${a.fallDrop ?? 1.5} or moved 2 map units while another played` }) };
      }
      // per worm: its turns and how far along its team's trail it stood after each
      const byWorm = new Map<number, Turn[]>();
      for (const t of played) byWorm.set(t.slot, [...(byWorm.get(t.slot) ?? []), t]);
      const worms = [...byWorm.entries()].sort((x, y) => x[0] - y[0]).map(([slot, ts]) => ({ worm: `WORM${slot}`, team: ts[0].team, turns: ts.length,
        along: ts.map((t) => t.along ?? null), of: ts[ts.length - 1].of ?? null, climb: Math.round(ts.reduce((x, t) => x + (t.climb ?? 0), 0) * 100) / 100,
        from: ts[0].from, to: ts[ts.length - 1].to ?? null }));
      const log = turnLogOut();
      return { played: played.length, asked: want, stop: why, seconds: Math.round((Date.now() - t0) / 100) / 10, end, worms, turns: played, ...(log ? { turnLog: log } : {}),
        ...(roundHeld ? { roundClock: roundHeld } : {}), ...traced, ...(luaRetryCount() ? { luaEvalRetries: luaRetryCount() } : {}),
        ...(stopAnn ? { announcements: announced, ...(announced.length ? {} : { announcementsNote: "none was drawn during the run" }) } : {}),
        units: a.units ?? "map", note: "each human turn's worm walked its team's trail with the game's own inputs (input.drive) from the trail's point nearest it; along/of: how far along the trail it stood after the turn. A turn's shots look at its worm from one side of its heading (shotPose near; game: the turn camera), clean of the mods' HUD text" };
    },
  },
  {
    name: "game_select_weapon",
    description: "Select a weapon for the active worm in the running Test, the engine's own way: level.test.selectWeapon (a Melange of this branch) sends the weapon panel's own message, Weapon.DelayedPanelChanged <id>, in the sim's tick, so the pick needs neither the panel, nor its clicks, nor DirectInput (the Automation module's injected mouse reported polls=0 in the Caldera v2 round-3 baseline and no weapon was ever taken). `weapon`: the engine's name (kWeaponBazooka, or bazooka, kUtilityNinjaRope ...) or its id (the bazooka is 1); only the weapon panel's weapons (kWeaponBazooka-kWeaponSuperAirstrike 1-29, kUtilityGirder-kUtilityBinoculars 34-43): the others (the Cluster Bomb and Bananette sub-munitions 30-31, Double Damage, Crate Shower, Crate Spy, Armour) the game never selects, are refused (sent, they crashed the game). Answers the worm's weapon read back (wum.game.worms().weapon), the name, how it was taken and how long it took; an error when the worm's weapon did not change to it. On an older game only the bazooka can be taken, from the panel's first cell through the Automation module (test builds, the game window focused, gameDir or MELANGE_GAME_DIR). A human turn in play is needed (game_state: phase play, ai false).",
    inputSchema: { type: "object", additionalProperties: false, required: ["weapon"], properties: {
      weapon: { anyOf: [{ type: "string" }, { type: "integer", minimum: 1, maximum: 68 }], description: "The engine's weapon name (kWeaponBazooka, bazooka, kWeaponGrenade ...) or id (1-68)." },
      gameDir: { type: "string", description: "Older game (no level.test.selectWeapon): the running game's folder, for its Automation command file (default MELANGE_GAME_DIR)." } } },
    async handler(s, a: { weapon: string | number; gameDir?: string }) {
      const r = await selectWeapon(s, a.weapon, a.gameDir);
      return { ...r, note: "the active worm's weapon now (wum.game.worms().weapon); game_fire fires it, or input.drive Input.Fire" };
    },
  },
  {
    name: "game_fire",
    description: "Fire the active worm's weapon in the running Test: with `weapon`, select it first (game_select_weapon: level.test.selectWeapon, the engine's own weapon-select path), aim with Input.AimUp or Input.AimDown held `aimUpMs` / `aimDownMs`, then hold Input.Fire for `fireMs` (the shot's power; a bazooka at 300 ms lands near the worm), all through input.drive (the game's own inputs, whatever window has the focus). A turn starts with no weapon selected, so without `weapon` the worm's selected weapon must already be one (the answer's `weapon`; an error when it is nil: nothing would fire). Answers the weapon fired, how it was selected and whether the turn left its play phase after the shot (`fired`: the game's retreat after a fire, read from game_state). The vanilla baseline (test {baseline: true}) does this on every human turn itself.",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      weapon: { anyOf: [{ type: "string" }, { type: "integer", minimum: 1, maximum: 68 }], description: "Select this weapon first (the engine's name or id, as game_select_weapon)." },
      fireMs: { type: "integer", minimum: 50, maximum: 3000, description: "How long Input.Fire is held, the shot's power (default 500)." },
      aimDownMs: { type: "integer", minimum: 0, maximum: 3000, description: "Hold Input.AimDown this long first (default 0)." },
      aimUpMs: { type: "integer", minimum: 0, maximum: 3000, description: "Hold Input.AimUp this long first (default 0)." },
      gameDir: { type: "string", description: "Older game (no level.test.selectWeapon): the running game's folder, for the Automation command file (default MELANGE_GAME_DIR)." } } },
    async handler(s, a: { weapon?: string | number; fireMs?: number; aimDownMs?: number; aimUpMs?: number; gameDir?: string }) {
      if (!(await has(s, "input.drive"))) throw new ToolError("game_fire needs the game's input.drive (Melange 0.13+ with the Controls module)");
      const client = await s.client();
      const sel = a.weapon !== undefined ? await selectWeapon(s, a.weapon, a.gameDir) : null;
      const st = await gameState(s);
      if (st.scene !== "match" || st.worm === undefined) throw new ToolError(`no match turn to fire in (scene ${st.scene}): test first`);
      if (st.phase !== "play") throw new ToolError(`the turn is not in play (phase ${st.phase ?? "?"}): wait for the next turn's play phase (game_wait)`);
      const wp = sel ? String(sel.weapon) : await evalClient(s, `for _, w in ipairs(wum.game.worms() or {}) do if w.slot == ${st.worm} then return tostring(w.weapon) end end return "nil"`).catch(() => "?");
      if (wp === "nil" || wp === "?") throw new ToolError(`WORM${st.worm} has no weapon selected (${wp}): give weapon (game_select_weapon), the game's turns start with none`);
      const drive = async (input: string, ms: number) => { await client.call("input.drive", { hold: [input], ms, replace: true }, 15000); await sleep(ms + 150); };
      if (a.aimDownMs) await drive("Input.AimDown", a.aimDownMs);
      if (a.aimUpMs) await drive("Input.AimUp", a.aimUpMs);
      const fireMs = a.fireMs ?? 500;
      await drive("Input.Fire", fireMs);
      await sleep(600);
      const after = await gameState(s);
      const fired = after.scene !== "match" || after.turn !== st.turn || after.phase !== "play";
      return { worm: `WORM${st.worm}`, turn: st.turn, weapon: Number(wp), ...(sel ? { selected: { name: sel.name, via: sel.via, ms: sel.ms } } : {}), fireMs, fired, phaseAfter: after.phase ?? null,
        note: fired ? "the turn left its play phase after the shot (the game's retreat): the weapon fired" : "the turn is still in play after the fire was held: a weapon with no immediate shot (a utility, a charge), a greyed-out one (a scheme delay or none left), or the fire was ignored" };
    },
  },
  {
    name: "game_fire_shot",
    description: "Pin a camera for the mine factory's fires in the running Test: every time the factory fires (each PlaceMines, at a turn start or game_mines {fire}), the game points the camera at the pose `delayMs` after it (default 500: the mines in flight), holds it `holdFrames` frames, writes Documents\\\\Melange\\\\shots\\\\<prefix>_<n>.png and gives the camera back, for up to `max` fires (render.fireShot, Melange 0.13+). No watch is needed: it stays armed for the match. The default pose looks down into the crater: from 20 map units south and 25 over the open project's factory knot, at the knot. `clear` disarms it; with neither a pose nor clear it answers the state and the shots taken so far (with `images`, as pictures).",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        pos: vec3("Eye [x, y, z] in `units`."), target: vec3("Point looked at [x, y, z] in `units`."), fov: { type: "number", minimum: 5, maximum: 150 },
        units: { type: "string", enum: ["map", "world"], description: "Units of pos and target (default map)." },
        arm: { type: "boolean", description: "Arm it with the default pose (the crater over the factory knot) when no pos is given." },
        delayMs: { type: "integer", minimum: 0, maximum: 10000, description: "After the fire (default 500)." },
        max: { type: "integer", minimum: 1, maximum: 512, description: "Shots to take before it disarms itself (default 4; a game before this branch's Melange takes at most 32: arm it again within the session for more, which answers the shots taken so far and starts a new count)." },
        perFire: { type: "integer", minimum: 1, maximum: 12, description: "Shots per fire, `everyMs` apart from delayMs on (default 1): the flight's rise, top and fall in one volley." },
        everyMs: { type: "integer", minimum: 50, maximum: 5000, description: "With perFire: between a fire's shots (default 250)." },
        track: { anyOf: [{ type: "boolean" }, { type: "number", minimum: 1, maximum: 200 }], description: "Follow the fire's first mine in flight (this branch's Melange): each shot's eye is this many map units from where the mine is then (true: 12), on the pose's own bearing (from pos to target, or the default's from the south) and a third of that over it, looking at the mine, so it fills the picture instead of a dot over the crater. Where the mine is comes from its start, its upward speed and the gravity (the game's own numbers: a launcher's mine rises launchHeight and falls back to its spot), so the camera chases it without reading it. With perFire 3-6 and everyMs 300-500 the shots cover a launcher mine's 3 s flight." },
        holdFrames: { type: "integer", minimum: 1, maximum: 60, description: "Frames drawn from the pose before it is read (default 2)." },
        prefix: { type: "string", description: "The PNGs are <prefix>_<n> (default erg_fireshot_<yyyymmdd_hhmmss>)." },
        clear: { type: "boolean", description: "Disarm it." },
        images: { type: "boolean", description: "With the state: answer the shots taken as pictures (default false)." },
      },
    },
    async handler(s, a: { pos?: number[]; target?: number[]; fov?: number; units?: "map" | "world"; arm?: boolean; delayMs?: number; max?: number; holdFrames?: number; prefix?: string; clear?: boolean; images?: boolean;
      perFire?: number; everyMs?: number; track?: boolean | number }) {
      if (!(await has(s, "render.fireShot"))) throw new ToolError("this game has no render.fireShot (Melange 0.13+): use game_mines {fire, shotsOnFire} instead");
      const client = await s.client();
      type St = { armed: boolean; name: string; delayMs: number; taken: number; max: number; paths: string[]; fires: number; lastError?: string; perFire?: number; everyMs?: number; track?: number };
      if (a.clear) return { ...(await client.call<St>("render.fireShot", { clear: true }, 15000)), note: "disarmed" };
      if (!a.pos && !a.target && !a.arm) {
        const st = await client.call<St>("render.fireShot", {}, 15000);
        if (!a.images || !st.paths.length) return st;
        return { content: [...st.paths.map((p) => image(readFileSync(p))), { type: "text", text: JSON.stringify(st, null, 1) }] };
      }
      if (!a.pos !== !a.target) throw new ToolError("give pos and target, or neither (arm: true takes the default pose over the factory knot)");
      const kk = a.units === "world" ? 1 : WORLD_PER_MAP;
      let pose: { pos: number[]; target: number[] };
      if (a.pos && a.target) pose = { pos: a.pos.map((v) => v * kk), target: a.target.map((v) => v * kk) };
      else {
        const f = s.project ? markers(s.store).find((m) => m.kind === "minefactory") : undefined;
        if (!f) throw new ToolError("the default pose looks at the open project's minefactory knot: open the project (or give pos and target)");
        const [x, y, z] = f.pos;
        pose = { pos: [x * WORLD_PER_MAP, (y + 25) * WORLD_PER_MAP, (z + 20) * WORLD_PER_MAP], target: [x * WORLD_PER_MAP, y * WORLD_PER_MAP, z * WORLD_PER_MAP] };
      }
      // over the sea as it stands now (the fires come later, when a flood may be higher: give pos over the schedule's cap)
      const ow = overWater(pose, (await gameState(s).catch(() => ({} as GameState))).water);
      pose = ow.pose;
      const name = uniqueName(a.prefix ?? `erg_fireshot_${stamp()}`, "fire", false).replace(/_fire$/, "");
      // re-arming answers the shots of the arming it replaces (render.fireShot starts a new count)
      const before = await client.call<St>("render.fireShot", {}, 15000).catch(() => null);
      const track = a.track === true ? 12 : typeof a.track === "number" ? a.track : 0;
      const extra = { ...(a.perFire && a.perFire > 1 ? { perFire: a.perFire, everyMs: a.everyMs ?? 250 } : {}), ...(track ? { track: track * WORLD_PER_MAP } : {}) };
      const st = await client.call<St>("render.fireShot", { ...pose, ...(a.fov ? { fov: a.fov } : {}), name: name.slice(0, 56), delayMs: a.delayMs ?? 500, max: a.max ?? 4, holdFrames: a.holdFrames ?? 2, ...extra }, 15000)
        .catch((e: Error) => {
          if (/max must be/.test(e.message) && (a.max ?? 4) > 32) throw new ToolError(`this game's render.fireShot takes at most 32 shots an arming (a Melange before this branch): arm it with max 32 and again (game_fire_shot) when they are taken, or update the game (${e.message})`);
          if (/perFire|everyMs|track/.test(e.message) || (Object.keys(extra).length && /unknown|must be/.test(e.message))) throw new ToolError(`this game's render.fireShot has no perFire or track (a Melange before this branch): ${e.message}`);
          throw e;
        });
      if (Object.keys(extra).length && st.perFire === undefined) throw new ToolError("this game's render.fireShot ignored perFire and track (a Melange before this branch): the shots are one a fire from the pinned pose");
      return { ...st, pose: { pos: pose.pos.map((v) => Math.round(v / kk * 100) / 100), target: pose.target.map((v) => Math.round(v / kk * 100) / 100), units: a.units ?? "map" },
        ...(before && before.paths.length ? { previousArming: { name: before.name, taken: before.taken, paths: before.paths } } : {}),
        ...(ow.lifted ? { lifted: ow.lifted } : {}),
        note: `armed: ${a.perFire && a.perFire > 1 ? `${a.perFire} shots a fire, ${a.everyMs ?? 250} ms apart from ` : "each factory fire is shot "}${a.delayMs ?? 500} ms after it, ${a.max ?? 4} shots in all${track ? `, the camera following the fire's first mine from ${track} map units` : ""}; game_fire_shot {} answers the shots taken (the game also logs each, [oasis] render.fireShot)` };
    },
  },
  {
    name: "game_mines",
    description: "The mines in the running Test match and the level's mine factory, observed: lists every mine entity (the game's Landmine payloads) with its position (entities.list, in `units`) and its distance from the factory knot of the open project, and `watchSeconds` watches that long for new mines (polled every quarter second) and the bus messages about mines (every registered name with Mine in it: GameLogic.StartMineFactory at each turn start, MineFactory.Start, MineFactory.Fire, MineFactory.FireEnd, GameLogic.PlaceMine, ...), answering each new mine with when it appeared and where. `untilFire` ends the watch 3 s after the factory's MineFactory.FireEnd, so one call sees a fire and where its mines landed: set_mine_factory {inactiveTurns: 0} and a target above the level's mines, Test, then game_mines {untilFire: true, watchSeconds: 120} (a fire happens at a turn start, so watch over a few turns; game_wait in between if needed). Needs the game with Oasis entities.list ([GameState] Enabled=1) while a Test plays.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        watchSeconds: { type: "number", minimum: 0, maximum: 300, description: "Watch this long for new mines and mine messages (default 0, or 120 with untilFire)." },
        untilFire: { type: "boolean", description: "End the watch 3 s after the factory's next MineFactory.FireEnd (its mines are out), instead of at watchSeconds." },
        fire: { anyOf: [{ type: "boolean" }, { type: "object", additionalProperties: false, properties: { count: { type: "integer", minimum: 1, maximum: 10, description: "Mines in this fire, whatever the factory's target (default: the target's own rule: min(activation - mines in the level, 10))." } } }],
          description: "Fire the factory now (level.test.fireFactory, Melange 0.13+): at the next sim tick its countdown is set to 0 and its own turn-start handler runs, so it fires as at a turn start (a volley to watch and shoot: with shotsOnFire). Implies untilFire (watchSeconds default 60). The answer's `onDemand` gives whether it started (trigger: the mines in the level and the target it found, or why not: the level holds its target, it is gone or firing) and, once its mines rest, each mine's spawn and rest position (the game logs it as levels/factory.fired). The fire sets the factory's countdown to inactiveTurns again, as a turn-start fire does, so the next fires come that many turn starts after it." },
        units: { type: "string", enum: ["map", "world"], description: "Units of the positions (default map)." },
        alive: { type: "boolean", description: "Also answer `factoryEntity`: whether the game's MineFactoryLogicEntity exists in the match now, where it is, and how far from the project's knot (a factory that fell elsewhere or exploded at load shows here), and its countdown: idleTurnsLeft (the turn starts before it checks again; at 0 or under the next GameLogic.StartMineFactory fires when fewer mines than its target are in the level) and minesToFire." },
        trajectories: { type: "boolean", description: "With watchSeconds: answer each mine that appeared with its path, sampled every quarter second (ms, position in `units`; at most 80 samples a mine). The position is the one entities.list reads from the mine's payload, which the game moves when the mine comes to rest: in a Test on 2026-11-08 a fired mine read its launch point (72 map units up) for about 2 s and then where it landed, so the path gives the launch and the landing, not the arc; shotsOnFire shows the flight." },
        shotsOnFire: { type: "object", additionalProperties: false, description: "With watchSeconds: when the factory's MineFactory.Fire arrives, point the camera (render.camera) at the factory and take `count` shots (render.shot) `everyMs` apart, so the mines are seen in flight; the camera is given back after. Default pose: 60 map units south of the factory knot and 30 over it, looking at 10 over it; or pos and target (in `units`).",
          properties: { count: { type: "integer", minimum: 1, maximum: 12, description: "Shots (default 4)." }, everyMs: { type: "integer", minimum: 100, maximum: 5000, description: "Between shots (default 500)." },
            prefix: { type: "string", description: "PNG names <prefix>_<n> in Documents\Melange\shots (default erg_minefire_<yyyymmdd_hhmmss>)." }, pos: vec3("Eye [x, y, z]."), target: vec3("Point looked at [x, y, z]."),
            width: { type: "integer", minimum: 64, maximum: 8192, description: "Picture width (default 960)." }, images: { type: "boolean", description: "Answer the pictures as image content (default true)." },
            track: { anyOf: [{ type: "boolean" }, { type: "number", minimum: 1, maximum: 200 }], description: "Chase the fire's first mine instead of the fixed pose (this branch's Melange: the game's own fire camera, render.fireShot {track}, armed for this fire): each shot's eye this many map units from where the mine is then (true: 12) on the pose's bearing, looking at it, the count shots everyMs apart from 200 ms after the mines appear (everyMs default 400 with track: a launcher's mine flies about 3 s). Refused while game_fire_shot is armed (it is the same camera): use game_fire_shot {track} there." } } },
      },
    },
    async handler(s, a0: { watchSeconds?: number; untilFire?: boolean; units?: "map" | "world"; alive?: boolean; trajectories?: boolean; fire?: boolean | { count?: number };
      shotsOnFire?: { count?: number; everyMs?: number; prefix?: string; pos?: number[]; target?: number[]; width?: number; images?: boolean; track?: boolean | number } }) {
      if (!(await has(s, "entities.list"))) throw new ToolError("this game has no entities.list (Oasis's game state readers): game_mines needs the game running a Test");
      // fire: a volley now, watched until its mines are out
      const fireAsk = a0.fire === undefined || a0.fire === false ? null : a0.fire === true ? {} : a0.fire;
      if (fireAsk && !(await has(s, "level.test.fireFactory"))) throw new ToolError("this game has no level.test.fireFactory (Melange 0.13+): the factory fires at turn starts only; set_mine_factory {everyNTurns: 1} and game_mines {untilFire: true}");
      const a = fireAsk ? { ...a0, untilFire: true, watchSeconds: a0.watchSeconds ?? 60 } : a0;
      // the fire's record is matched, not counted: the game keeps its last 8 fires, so once 8 were logged the count
      // stayed 8 and every later burst read "no fire record" (Caldera v2 round 2). The trigger this call makes is the one
      // that differs from the trigger before it, and its fire the first record not there before, from its tick on.
      const pr0 = fireAsk ? await probeOf(s) : null;
      const fireId = (x: FireRec) => `${x.stem}:${x.tick}:${x.turn}:${x.mines.map((q) => q.handle).join(",")}`;
      const trigBefore = JSON.stringify(pr0?.factory.trigger ?? null), firesBefore = new Set((pr0?.factory.fires ?? []).map(fireId));
      const k = a.units === "world" ? 1 : 1 / WORLD_PER_MAP;
      const out = (p: { x: number; y: number; z: number } | null) => (p ? [p.x, p.y, p.z].map((v) => Math.round(v * k * 100) / 100) : null);
      // the factory knot of the open project, in world units
      let factory: number[] | null = null;
      try {
        const f = markers(s.store).find((m) => m.kind === "minefactory");
        if (f) factory = f.pos.map((v) => v * WORLD_PER_MAP);
      } catch {
        // no project open: no factory position
      }
      const dist = (p: { x: number; y: number; z: number } | null) => (p && factory ? Math.round(Math.hypot(p.x - factory[0], p.z - factory[2]) * k * 100) / 100 : undefined);
      const t0 = Date.now(), before = await liveMines(s), seen = new Map(before.map((m) => [m.handle, m]));
      const watch = (a.watchSeconds ?? (a.untilFire ? 120 : 0)) * 1000;
      let firedAt: number | null = null, fireStart: number | null = null;
      if ((a.trajectories || a.shotsOnFire) && watch <= 0) throw new ToolError("trajectories and shotsOnFire watch the match: give watchSeconds (or untilFire)");
      if (a.shotsOnFire && !((await has(s, "render.camera")) && (await has(s, "render.shot")))) throw new ToolError("shotsOnFire needs the game's render.camera and render.shot");
      const events: { name: string; ms: number; d?: unknown }[] = [];
      const counts: Record<string, number> = {};
      let unsub: (() => void) | undefined, names: string[] = [];
      if (watch > 0) {
        names = await mineMessages(s);
        if (names.length) unsub = (await s.client()).subscribe<{ name: string; d?: unknown }>("bus", { names, path: "post", decode: true }, (m) => {
          counts[m.name] = (counts[m.name] ?? 0) + 1;
          if (m.name === "MineFactory.FireEnd" && firedAt === null) firedAt = Date.now();
          if (m.name === "MineFactory.Fire" && fireStart === null) fireStart = Date.now();
          if (events.length < 200) events.push({ name: m.name, ms: Date.now() - t0, ...(m.d !== undefined ? { d: m.d } : {}) });
        });
      }
      if (a.untilFire && !names.includes("MineFactory.FireEnd")) throw new ToolError("untilFire needs the bus's MineFactory.FireEnd, which this game does not list (bus.names)");
      // shots on the fire: the pose (world units) and what was taken
      const sf = a.shotsOnFire, shots: object[] = [], pictures: Content[] = [];
      let posed = false;
      const firePose = () => {
        const kk = a.units === "world" ? 1 : WORLD_PER_MAP;
        if (sf?.pos && sf.target) return { pos: sf.pos.map((v) => v * kk), target: sf.target.map((v) => v * kk), fov: 60 };
        if (sf?.pos || sf?.target) throw new ToolError("shotsOnFire: give both pos and target, or neither");
        if (!factory) return overview(s);
        return { pos: [factory[0], factory[1] + 30 * WORLD_PER_MAP, factory[2] + 60 * WORLD_PER_MAP], target: [factory[0], factory[1] + 10 * WORLD_PER_MAP, factory[2]], fov: 60 };
      };
      const pose = sf ? firePose() : null;
      // track: the game's fire camera chases the fire's first mine (render.fireShot {track}), armed before the fire
      const trackMap = sf?.track === true ? 12 : typeof sf?.track === "number" ? sf.track : 0;
      type FsSt = { armed: boolean; taken: number; paths: string[]; perFire?: number; lastError?: string };
      let tracking: { name: string; count: number } | null = null;
      if (sf && trackMap && pose) {
        if (!(await has(s, "render.fireShot"))) throw new ToolError("shotsOnFire.track needs the game's render.fireShot (this branch's Melange)");
        const cl = await s.client(), st0 = await cl.call<FsSt>("render.fireShot", {}, 15000);
        if (st0.armed) throw new ToolError("shotsOnFire.track uses the game's fire camera, which game_fire_shot has armed: give game_fire_shot {track} there, or game_fire_shot {clear: true} first");
        const name = uniqueName(sf.prefix ?? `erg_minefire_${stamp()}`, "track").slice(0, 56), count = sf.count ?? 4;
        const st = await cl.call<FsSt>("render.fireShot", { pos: pose.pos, target: pose.target, fov: pose.fov ?? 60, name, delayMs: 200, max: count, holdFrames: 2, perFire: count,
          everyMs: sf.everyMs ?? 400, track: trackMap * WORLD_PER_MAP }, 15000);
        if (st.perFire === undefined) throw new ToolError("this game's render.fireShot has no track (a Melange before this branch)");
        tracking = { name, count };
      }
      let queued: unknown = null;
      if (fireAsk) queued = await (await s.client()).call("level.test.fireFactory", fireAsk.count ? { count: fireAsk.count } : {}, 15000);
      let trackTaken = 0, lastTrackRead = 0;
      const tracks = new Map<number, number[][]>();
      try {
        const appeared: { handle: number; type: string; ms: number; at: number[] | null; fromFactory?: number; path?: number[][] }[] = [];
        while (Date.now() - t0 < watch && !(a.untilFire && firedAt !== null && Date.now() - firedAt > 3000 && (!sf || (tracking ? trackTaken >= tracking.count : shots.length >= (sf.count ?? 4))))) {
          await sleep(250);
          if (tracking && Date.now() - lastTrackRead > 1000) {
            lastTrackRead = Date.now();
            trackTaken = (await (await s.client()).call<FsSt>("render.fireShot", {}, 15000).catch(() => ({ taken: trackTaken } as FsSt))).taken;
          }
          for (const m of await liveMines(s).catch(() => [] as Entity[])) {
            if (!seen.has(m.handle)) { seen.set(m.handle, m); appeared.push({ handle: m.handle, type: m.type, ms: Date.now() - t0, at: out(m.pos), fromFactory: dist(m.pos) }); tracks.set(m.handle, []); }
            const tr = a.trajectories ? tracks.get(m.handle) : undefined;
            if (tr && tr.length < 80 && m.pos) tr.push([Date.now() - t0, ...out(m.pos)!]);
          }
          if (sf && pose && !tracking && fireStart !== null && shots.length < (sf.count ?? 4) && Date.now() >= fireStart + shots.length * (sf.everyMs ?? 500)) {
            stopHold();
            const fr = (await setCamera(s, pose, 600)).frame;
            posed = true;
            const name = uniqueName(sf.prefix ?? `erg_minefire_${stamp()}`, String(shots.length + 1));
            const p = await shot(s, name, { width: sf.width ?? 960, clean: true, minFrame: fr !== undefined ? fr + 2 : undefined });
            shots.push({ name, path: p.path, ms: Date.now() - t0, sinceFireMs: Date.now() - fireStart, minesSeen: tracks.size });
            if (sf.images !== false) pictures.push(image(p.png));
          }
        }
        if (a.trajectories) for (const ap of appeared) ap.path = tracks.get(ap.handle) ?? [];
        if (tracking) {
          const cl = await s.client(), st = await cl.call<FsSt>("render.fireShot", {}, 15000);
          await cl.call("render.fireShot", { clear: true }, 15000).catch(() => undefined);
          for (const [i, p] of st.paths.entries()) {
            shots.push({ name: `${tracking.name}_${i + 1}`, path: p, tracked: `chasing the fire's first mine from ${trackMap} map units` });
            if (sf!.images !== false) { try { pictures.push(image(readFileSync(p))); } catch { /* the path is still answered */ } }
          }
          if (st.lastError) shots.push({ error: st.lastError });
        }
        if (posed) await clearCamera(s).catch(() => undefined);
        posed = false;
        const now = watch > 0 ? await liveMines(s) : before;
        let factoryEntity: object | undefined;
        if (a.alive) {
          const fe = await factoryEntities(s);
          const off = (p: Entity["pos"]) => (p && factory ? Math.round(Math.hypot(p.x - factory[0], p.y - factory[1], p.z - factory[2]) * k * 100) / 100 : undefined);
          const logic = fe.find((e) => /logic/i.test(e.type)) ?? fe[0];
          const st = logic ? await factoryState(s, logic.handle) : null;
          const far = st && factory && Math.hypot(st.pos.x - factory[0], st.pos.z - factory[2]) > 60;
          factoryEntity = logic
            ? { alive: true, entities: fe.map((e) => ({ handle: e.handle, type: e.type })),
                ...(st ? { at: out(st.pos), fromKnot: off(st.pos), exploded: st.exploded, hitByExplosion: st.hit, fireState: st.fireState, idleTurnsLeft: st.countdown, minesToFire: st.toFire }
                  : { note: "the game has no entity.inspect (or [Oasis] RawInspect=0): where it settled is not known" }),
                ...(far ? { warning: "the factory settled far from its knot: its drop from the knot hit no land the sweep sees (a draped skin) and it took a stale point" } : {}) }
            : { alive: false, why: factory ? FACTORY_GONE : "no factory entity in the match (and no project with a minefactory knot is open)" };
        }
        // fire: what the trigger found, then (its mines at rest, at most 45 s) the fire's mines from the game's own record
        let fired: object | undefined;
        if (fireAsk) {
          const mine = (p: ProbeAnswer | null): { trig: ProbeAnswer["factory"]["trigger"]; rec: FireRec | null } => {
            const tr = p?.factory.trigger;
            if (!p || !tr || JSON.stringify(tr) === trigBefore) return { trig: null, rec: null };
            return { trig: tr, rec: p.factory.fires.filter((x) => x.tick >= tr.tick && !firesBefore.has(fireId(x))).sort((x, y) => x.tick - y.tick)[0] ?? null };
          };
          let pr = await probeOf(s), m = mine(pr);
          // the trigger runs at the next sim tick, the record once the fire's mines rest (at most 30 s)
          for (const t1 = Date.now(); pr && !m.rec && (m.trig ? m.trig.started : true) && Date.now() - t1 < 45000; m = mine(pr)) { await sleep(500); pr = await probeOf(s); }
          const trig = m.trig ?? pr?.factory.trigger ?? null, last = m.rec;
          fired = { queued, trigger: trig, ...(last ? { fire: fireOut(last, a.units ?? "map", ...groundLookup(s)) } : { fire: null, note: m.trig && !m.trig.started ? `the factory did not fire: ${m.trig.why}` : !m.trig ? "the game did not run the fire's trigger within 45 s (is the match paused?)" : "no fire record within 45 s of the trigger (its mines may still be moving): game_probe's factory.fires has it later (the fire whose tick is at or after the trigger's)" }) };
        }
        const answer = {
          units: a.units ?? "map", factory: factory ? { at: out({ x: factory[0], y: factory[1], z: factory[2] }), note: "the open project's minefactory knot" } : null,
          ...(fired ? { onDemand: fired } : {}),
          ...(a.untilFire ? { fired: firedAt !== null, ...(firedAt !== null ? { fireEndMs: firedAt - t0 } : {}) } : {}),
          ...(factoryEntity ? { factoryEntity } : {}),
          minesBefore: before.length, mines: now.map((m) => ({ handle: m.handle, type: m.type, at: out(m.pos), fromFactory: dist(m.pos) })),
          ...(watch > 0 ? { watchedMs: Date.now() - t0, appeared, messages: { watched: names, counts, events } } : {}),
          ...(sf ? { shotsOnFire: fireStart === null && !shots.length ? { taken: 0, note: "no MineFactory.Fire in the watch" } : { taken: shots.length, shots } } : {}),
        };
        return pictures.length ? { content: [...pictures, { type: "text", text: JSON.stringify(answer, null, 1) }] } : answer;
      } finally {
        unsub?.();
        if (posed) await clearCamera(s).catch(() => undefined);
        if (tracking) await (await s.client()).call("render.fireShot", { clear: true }, 15000).catch(() => undefined);
      }
    },
  },
  {
    name: "game_mine_drop",
    description: "Whether the ground holds a mine, in the running Test (level.test.dropMines, Melange of this branch): one mine is made at each point (`points`, [x, z] or [x, y, z] in `units`, default map; [x, z] starts `height` (default 8 map units) over the open project's ground top in that column, [x, y, z] at y), falling as the factory's own (0, -0.3, 0) unless `vel` ([x, y, z] world units per ms); after `seconds` (default 10) each point says `held` (a new mine at rest within 1.5 map units of it, at most 1.5 under the ground top: it landed there), `low` (a new mine near it but lower: it went through the top to something under it) or `gone` (none: fell through to the water and drowned, or went off). Answers the counts and each point. Use it to choose a mine factory's launchers and landing zones: in Tests on 2026-11-08 a mine fell straight through Caldera's crater ground (the draped skin and solid undraped pads alike) everywhere west of x 7 map and held everywhere east of x 12, on the same frames, while worms stood on both; launchers must stand where the drops hold. `trails` probes walking routes instead (a pre-test, in a Test match of its own before the worms are walked: the mines stay where they land): along each trail (points [x, z] or [x, y, z]; give y on a switchback) a mine every `spacing` (default 3 map units) on the centreline and one on each shoulder (`shoulder`, default 2.6: half a 5.2 bed), each over the ground at the trail's own height, in batches of 64; points within `avoidWorms` (default 8) of a living worm are skipped (a mine there would go off under it); a probe whose mine went off (an explosion the Test probe saw within 2.5 map units: mines 3 apart set each other off in chains) is `setOff`, not a failure. Answers per trail the counts and every point that did not hold (where along the trail, centre or left/right): a void under a trail that check_reachability's graph and check_trail cannot see (Caldera v2 round 1: two worms fell 31 map units through the SW trail beside a corner of the crater's skin block, where every mine dropped fell through too).",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        points: { type: "array", minItems: 1, maxItems: 64, items: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 3 },
          description: "Points to drop a mine on ([x, z] or [x, y, z] in `units`), all at once: keep it under about 30 (the game keeps about 30 mines and removes the oldest past that, so the first of a larger drop read as gone)." },
        trails: { type: "array", minItems: 1, maxItems: 8, items: { type: "array", minItems: 2, maxItems: 4000, items: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 3 } },
          description: "Instead of points: trails ([x, z] or [x, y, z] points in `units`) to probe along, centre and shoulders." },
        spacing: { type: "number", minimum: 1, maximum: 50, description: "trails: map units between probes along a trail (default 3)." },
        shoulder: { type: "number", minimum: 0, maximum: 20, description: "trails: map units from the centreline to each shoulder probe (default 2.6; 0: the centreline only)." },
        avoidWorms: { type: "number", minimum: 0, maximum: 50, description: "trails: skip probes this close to a living worm, map units (default 8)." },
        batch: { type: "integer", minimum: 1, maximum: 30, description: "trails: mines dropped at once (default 24). The game keeps about 30 mines and removes the oldest past that, so a larger drop loses its first mines (they read as gone)." },
        height: { type: "number", minimum: 0.5, maximum: 100, description: "Map units over the ground top for [x, z] points (default 8)." },
        vel: { type: "array", items: { type: "number" }, minItems: 3, maxItems: 3, description: "The mines' start velocity, world units per ms (default the factory's (0, -0.3, 0))." },
        seconds: { type: "number", minimum: 3, maximum: 60, description: "How long to let them land (default 10)." },
        units: { type: "string", enum: ["map", "world"] },
      },
    },
    async handler(s, a: { points?: number[][]; trails?: number[][][]; spacing?: number; shoulder?: number; avoidWorms?: number; batch?: number; height?: number; vel?: number[]; seconds?: number; units?: "map" | "world" }) {
      if (!!a.points === !!a.trails) throw new ToolError("give points or trails");
      if (!(await has(s, "level.test.dropMines"))) throw new ToolError("this game has no level.test.dropMines (a Melange of the erg-mcp branch): Test with that build");
      if (!(await has(s, "entities.list"))) throw new ToolError("this game has no entities.list ([GameState] Enabled=1 in its Melange.ini)");
      const k = (a.units ?? "map") === "world" ? 1 / WORLD_PER_MAP : 1, h = a.height ?? 8;
      const r1 = (v: number) => Math.round(v * 100) / 100;
      // the ground top in a column (map units), the open project's; under `below` when given (a trail's own leg)
      const topAt = (x: number, z: number, below?: number): number | null => {
        try {
          const sp = spansAt(grids(s.store), x, z, below ?? Infinity);
          return sp.length ? Math.max(...sp.map((q) => q.top)) : null;
        } catch {
          return null;   // no project open: [x, y, z] points only
        }
      };
      type Probe = { at: number[]; top: number | null; trail?: number; along?: number; side?: string };
      // a mine that went off (an explosion the Test probe saw within 2.5 map units of its point) is `setOff`, not `gone`:
      // in the first trail probe (Caldera v2 round 2) mines 3 apart went off in chains along whole legs, which read as
      // ground that does not hold
      const drop = async (pts: Probe[]) => {
        const seen0 = (await probeOf(s))?.probe.explosionsSeen ?? null;
        const before = new Set((await liveMines(s)).map((m) => m.handle));
        const queued = await (await s.client()).call("level.test.dropMines", { points: pts.map((p) => p.at.map((v) => v * WORLD_PER_MAP)), ...(a.vel ? { vel: a.vel } : {}) }, 15000);
        await new Promise((r) => setTimeout(r, (a.seconds ?? 10) * 1000));
        const fresh = (await liveMines(s)).filter((m) => !before.has(m.handle) && m.pos).map((m) => ({ x: m.pos!.x / WORLD_PER_MAP, y: m.pos!.y / WORLD_PER_MAP, z: m.pos!.z / WORLD_PER_MAP }));
        const pr = seen0 === null ? null : await probeOf(s);
        const blasts = pr ? pr.probe.explosions.slice(-Math.min(pr.probe.explosions.length, Math.max(0, pr.probe.explosionsSeen - seen0!))) : [];
        return { queued, explosions: pr ? pr.probe.explosionsSeen - seen0! : null, each: pts.map((p) => {
          const near = fresh.filter((m) => Math.hypot(m.x - p.at[0], m.z - p.at[2]) <= 1.5).sort((u, v) => v.y - u.y)[0];
          const ground = p.top ?? p.at[1];
          const verdict = near ? (near.y >= ground - 1.5 ? "held" : "low") : blasts.some((b) => Math.hypot(b.at[0] - p.at[0], b.at[2] - p.at[2]) <= 2.5) ? "setOff" : "gone";
          return { at: p.at.map(r1), groundTop: p.top === null ? null : r1(p.top), verdict, ...(near ? { mine: [near.x, near.y, near.z].map(r1) } : {}),
            ...(p.trail !== undefined ? { trail: p.trail, along: r1(p.along!), side: p.side } : {}) };
        }) };
      };
      if (a.points) {
        // each point in map units, with the ground top under it (the open project's) when there is one
        const pts = a.points.map((p, i) => {
          if (!p.every(Number.isFinite)) throw new ToolError(`points[${i}]: numbers`);
          const x = p[0] * k, z = (p.length === 3 ? p[2] : p[1]) * k, top = topAt(x, z);
          if (p.length === 2 && top === null) throw new ToolError(`points[${i}]: no ground in the open project's column at (${p[0]}, ${p[1]}): give [x, y, z]`);
          return { at: [x, p.length === 3 ? p[1] * k : top! + h, z], top };
        });
        const r = await drop(pts);
        const counts: Record<string, number> = { held: 0, low: 0, gone: 0 };
        for (const e of r.each) counts[e.verdict] = (counts[e.verdict] ?? 0) + 1;
        return { units: "map", queued: r.queued, counts, points: r.each, ...(r.explosions !== null ? { explosions: r.explosions } : {}),
          note: "held: the mine lies on the ground at the point; low: it went through the top to something under it; setOff: it went off (an explosion near the point); gone: no mine near the point and no explosion (through to the water, drowned)" };
      }
      // trails: probes along each, centre and shoulders, over the ground at the trail's own height
      if (!s.project) throw new ToolError("trails are probed over the open project's ground: open_project the tested level");
      const worms = (await wormsNow(s).catch(() => [])).filter((w) => w.alive).map((w) => w.pos.map((v) => v / WORLD_PER_MAP));
      const spacing = a.spacing ?? 3, sh = a.shoulder ?? 2.6, avoid = a.avoidWorms ?? 8;
      const probes: Probe[] = [], skipped: object[] = [], noGround: { trail: number; along: number; side: string; at: number[] }[] = [];
      const sides: [string, number][] = sh > 0 ? [["centre", 0], ["left", sh], ["right", -sh]] : [["centre", 0]];
      a.trails!.forEach((t, ti) => {
        const q = t.map((p) => p.map((v) => v * k));
        let along = 0, carry = 0;
        for (let i = 0; i + 1 < q.length; i++) {
          const A = q[i], B = q[i + 1], ax = A[0], az = A[A.length - 1], bx = B[0], bz = B[B.length - 1], L = Math.hypot(bx - ax, bz - az);
          if (L < 1e-6) continue;
          const ux = (bx - ax) / L, uz = (bz - az) / L, ya = A.length === 3 ? A[1] : NaN, yb = B.length === 3 ? B[1] : NaN;
          let d = carry;
          for (; d <= L + 1e-9; d += spacing) {
            const x = ax + ux * d, z = az + uz * d, y = ya + ((yb - ya) * d) / L;
            for (const [side, o] of sides) {
              const px = x - uz * o, pz = z + ux * o;
              if (worms.some((w) => Math.hypot(w[0] - px, w[2] - pz) < avoid)) { skipped.push({ trail: ti, along: r1(along + d), side, why: "a worm is near" }); continue; }
              const top = topAt(px, pz, Number.isFinite(y) ? y + 2.5 : undefined);
              if (top === null) { noGround.push({ trail: ti, along: r1(along + d), side, at: [r1(px), r1(pz)] }); continue; }
              probes.push({ at: [px, top + h, pz], top, trail: ti, along: along + d, side });
            }
          }
          carry = d - L;
          along += L;
        }
      });
      if (probes.length > 1024) throw new ToolError(`${probes.length} probes: at most 1024 (raise spacing)`);
      // batches of `batch` (default 24) dealt round the whole list (probe i in batch i % n), so the mines of a batch are far
      // apart. The game keeps about 30 mines: past that the oldest go as new ones are made (Caldera v2 round 2: of every
      // 64 dropped at once the first 30-odd vanished without a blast, which read as ground that does not hold), so a
      // batch stays under it and is read before the next one is dropped
      const results: Awaited<ReturnType<typeof drop>>["each"] = [];
      const nb = Math.ceil(probes.length / (a.batch ?? 24));
      let blasts = 0;
      for (let b = 0; b < nb; b++) { const r = await drop(probes.filter((_, i) => i % nb === b)); results.push(...r.each); blasts += r.explosions ?? 0; }
      const per = a.trails!.map((_, ti) => {
        const mine = results.filter((r) => r.trail === ti).sort((x, y) => (x.along ?? 0) - (y.along ?? 0)), counts = { held: 0, low: 0, gone: 0, setOff: 0 };
        for (const r of mine) counts[r.verdict as keyof typeof counts]++;
        const bad = mine.filter((r) => r.verdict === "gone" || r.verdict === "low").map((r) => ({ along: r.along, side: r.side, at: r.at, verdict: r.verdict, ...(r.mine ? { mine: r.mine } : {}) }));
        return { trail: ti, probes: mine.length, counts, ok: bad.length === 0 && !noGround.some((g) => g.trail === ti), ...(bad.length ? { notHeld: bad.slice(0, 60) } : {}),
          ...(counts.setOff ? { setOffAt: mine.filter((r) => r.verdict === "setOff").map((r) => r.along).slice(0, 60) } : {}) };
      });
      return { units: "map", trails: per, ok: per.every((t) => t.ok), probes: probes.length, explosions: blasts, ...(skipped.length ? { skipped: skipped.slice(0, 40), skippedCount: skipped.length } : {}),
        ...(noGround.length ? { noGround: noGround.slice(0, 40) } : {}),
        note: "every probe is a mine dropped from `height` over the ground at the trail's own height; held: it lies on the trail; low: it went through to something under it; gone: no mine and no explosion near it (through to the water); setOff: it went off (another mine's blast, a worm near), so the ground there was not tested: probe it again with a wider spacing. The mines stay: probe in a Test of its own, end_match, then Test again to walk the worms" };
    },
  },
  {
    name: "game_perf",
    description: "Frame cost of the running game over `seconds` (default 10): fps and the CPU's busy time per frame (wum.render.timing) and the GPU's own time per frame (gpuMs: the whole frame and its stages, the Mirage GPU timers that gltrace.gpu reads), sampled every `everyMs`, as medians and 95th percentiles. fps alone cannot show a graphics regression where the game is CPU-bound (testenv B: about 36 fps on the menu and in every match), so compare gpuFrameMs: each run is kept for this erg-mcp session under `label` (default the open project's id, or baseline with none open), and `against` (a label, default the first run) answers the difference. Read-only; needs the game running (the GPU timers are [Mirage] GpuTimers=1, the default).",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      seconds: { type: "number", minimum: 1, maximum: 120, description: "How long to sample (default 10)." },
      everyMs: { type: "integer", minimum: 50, maximum: 2000, description: "Sample spacing (default 200)." },
      label: { type: "string", description: "The run's name for comparing (default the open project's id, or baseline)." },
      against: { type: "string", description: "Compare with this earlier run (default the first run of the session, when this is not it)." } } },
    async handler(s, a: { seconds?: number; everyMs?: number; label?: string; against?: string }) {
      const code = `local t = wum.render.timing() local g = t.gpuMs or {}
return string.format("%.3f,%.3f,%.3f,%s,%s,%s,%s", t.fps, t.busyMsP50, t.frameMsP50, tostring(g.frame), tostring(g.world), tostring(g.postWorld), tostring(g.final))`;
      const st = await gameState(s).catch(() => null);
      const rows: number[][] = [], t0 = Date.now(), dur = (a.seconds ?? 10) * 1000, every = a.everyMs ?? 200;
      while (Date.now() - t0 < dur) {
        rows.push((await evalClient(s, code)).split(",").map((x) => (x === "nil" ? NaN : Number(x))));
        await sleep(every);
      }
      const stat = (i: number): PerfStat => {
        const v = rows.map((r) => r[i]).filter(Number.isFinite).sort((x, y) => x - y);
        const pct = (q: number) => Math.round(v[Math.min(v.length - 1, Math.floor(q * (v.length - 1) + 0.5))] * 100) / 100;
        return v.length ? { p50: pct(0.5), p95: pct(0.95), n: v.length } : null;
      };
      const game = (await s.ensure()).build;
      const run: PerfRun = { label: a.label ?? s.project?.id ?? "baseline", scene: st?.scene ?? "?", seconds: Math.round((Date.now() - t0) / 100) / 10, samples: rows.length, startedAt: t0,
        fps: stat(0), busyMs: stat(1), frameMs: stat(2), gpuFrameMs: stat(3), gpuWorldMs: stat(4), gpuPostWorldMs: stat(5), gpuFinalMs: stat(6), ...(game ? { game } : {}) };
      const cpuBound = !!run.busyMs && !!run.frameMs && run.busyMs.p50! >= run.frameMs.p50! * 0.85;
      // the run to compare with: this session's, else an earlier process's from the runs file (pickPerfBase)
      const saved = savedPerfRuns(), pick = pickPerfBase(perfRuns, saved, a.against, game);
      if (a.against && !pick.base) throw new ToolError(`no game_perf run labelled ${a.against} in this session or the runs file (runs: ${[...new Set([...perfRuns, ...saved].map((r) => r.label))].join(", ") || "none"})`);
      const base = pick.base;
      perfRuns.push(run);
      savePerfRuns();
      const d = (x: PerfStat, y: PerfStat) => (x?.p50 != null && y?.p50 != null ? Math.round((x.p50 - y.p50) * 100) / 100 : null);
      const compare = base && base !== run ? { against: base.label, ...(base.from === "saved" ? { savedRun: `an earlier erg-mcp process's run, ${new Date(base.startedAt).toISOString()}${base.game ? `, game ${base.game}` : ""}` } : {}),
        ...(pick.note ? { note: pick.note } : {}), gpuFrameMsDelta: d(run.gpuFrameMs, base.gpuFrameMs), fpsDelta: d(run.fps, base.fps), busyMsDelta: d(run.busyMs, base.busyMs),
        verdict: run.gpuFrameMs?.p50 == null || base.gpuFrameMs?.p50 == null ? "no GPU times to compare (the timers are off or unsupported)"
          : run.gpuFrameMs.p50 <= base.gpuFrameMs.p50 * 1.15 + 0.5 ? `GPU time per frame not worse than ${base.label} (within 15 % + 0.5 ms)` : `GPU time per frame worse than ${base.label}` } : undefined;
      const { startedAt: _t, from: _f, ...out } = run;
      return { ...out, ...(cpuBound ? { cpuBound: "the CPU's busy time is most of each frame: fps follows the CPU, not the drawing; compare gpuFrameMs" } : {}), ...(compare ? { compare } : {}),
        runs: perfRuns.map((r) => r.label), ...(saved.length ? { savedRuns: mergePerfRuns([], saved).map((r) => `${r.label} (${new Date(r.startedAt).toISOString()})`), runsFile: perfFile() } : {}),
        note: "gpuFrameMs: the GPU's time for the whole frame (swap to swap); busyMs: the CPU's time in the frame before SwapBuffers; medians (p50) and 95th percentiles of the samples; every run is kept in the runs file for `against` from another erg-mcp process" };
    },
  },
  {
    name: "game_material_shots",
    description: "The in-game look of the open project's materials, with the plugins the Test runs with (Sunstone on): for each material (or `materials`) the level's best exposed patch of it is found in the project's voxels (the ground sampled every `step`, the flattest stretch with the most of that material round it), the camera is posed `distance` map units off it, `elevation` degrees down, and a clean shot is read back (<prefix>_mat<index>_<name>.png), whose middle's mean colour is compared with the material file's swatch (preview_materials' tile, the texture's mean): per channel, in lightness and in words (lighter, darker, a colour cast). preview_materials' tile is not what the players see: the game lights and tiles the texture, and Sunstone's landscape shader adds detail per texture class (its texture tags: a stone texture gets grime, and moss where its pits face the sky), so GreySlateFloor's warm grey tile came out pale off-white with green blotches (Caldera v2 round 4). Needs the Test match running (test) and render.shot. Read-only.",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      materials: { type: "array", maxItems: 64, items: { type: "integer", minimum: 0, maximum: 63 }, description: "Material indices to shoot (default every material exposed on the level)." },
      prefix: { type: "string", description: "Shot names <prefix>_mat<index>_<name> (default erg_<time>_mat)." },
      width: { type: "integer", minimum: 160, maximum: 1920, description: "Shot width (default 640)." },
      distance: { type: "number", minimum: 1, maximum: 60, description: "The eye's distance from the patch, map units (default 6)." },
      elevation: { type: "number", minimum: 5, maximum: 89, description: "Degrees down at the patch (default 50)." },
      azimuth: { type: "number", minimum: 0, maximum: 360, description: "Where the eye stands, degrees clockwise from north (-z) of the patch (default 200: a little west of south, the sun behind)." },
      step: { type: "number", minimum: 0.5, maximum: 8, description: "The ground sample spacing for finding the patches, map units (default 2)." },
      images: { type: "boolean", description: "Also answer the pictures (default false: the paths and colours)." } } },
    async handler(s, a: { materials?: number[]; prefix?: string; width?: number; distance?: number; elevation?: number; azimuth?: number; step?: number; images?: boolean }) {
      s.requireProject();
      if (!(await has(s, "render.shot"))) throw new ToolError("game_material_shots needs the game's render.shot (Melange of this branch)");
      const st = await gameState(s);
      if (st.scene !== "match") throw new ToolError(`no match is running (scene ${st.scene}): test the project first; the look is the match's light and the plugins the Test runs with`);
      const gs = grids(s.store), wl = s.store.scene.water.level, water = wl === null ? null : wl / WORLD_PER_MAP;
      const patches = materialPatches(gs, { only: a.materials, step: a.step ?? 2, water });
      if (!patches.length) throw new ToolError(a.materials ? `none of materials ${a.materials.join(", ")} is exposed on the level's ground` : "no ground to shoot: the level has no exposed voxels over the water");
      const mats = await projectMaterials(s), prefix = a.prefix ?? `erg_${stamp()}_mat`, W = WORLD_PER_MAP;
      const dist = (a.distance ?? 6) * W, el = ((a.elevation ?? 50) * Math.PI) / 180, az = ((a.azimuth ?? 200) * Math.PI) / 180;
      const dir = [Math.sin(az) * Math.cos(el), Math.sin(el), -Math.cos(az) * Math.cos(el)];
      const shots: Record<string, unknown>[] = [], pics: Content[] = [];
      for (const p of patches) {
        const name = mats?.names[p.material], tidy = (name ?? `m${p.material}`).replace(/[^A-Za-z0-9]+/g, "_").replace(/^_+|_+$/g, "").slice(0, 24);
        const target = [p.at[0] * W, (p.at[1] + 0.3) * W, p.at[2] * W];
        let pose = { pos: [target[0] + dir[0] * dist, target[1] + dir[1] * dist, target[2] + dir[2] * dist], target };
        const moved: string[] = [];
        const ce = clearEye(gs, pose.pos.map((v) => v / W), pose.target.map((v) => v / W), { skip: 1 });
        if (ce.moved) { pose = { ...pose, pos: ce.eye.map((v) => v * W) }; moved.push(ce.moved); }
        const ow = overWater(pose, st.water);
        if (ow.lifted) { pose = ow.pose; moved.push(ow.lifted); }
        const rec: Record<string, unknown> = { material: p.material, slot: `P${String(p.material + 1).padStart(2, "0")}`, ...(name ? { name } : {}), at: p.at, samples: p.samples, ...(moved.length ? { eyeMoved: moved } : {}) };
        try {
          const set = await setCamera(s, { ...pose, fov: 50 }, 40);
          const sh = await shot(s, uniqueName(prefix, `mat${p.material}_${tidy}`), { clean: true, width: a.width ?? 640, minFrame: set.frame !== undefined ? set.frame + 2 : undefined });
          await clearCamera(s).catch(() => undefined);
          const img = decodePng(sh.png), inGame = centreColour(img.rgb, img.w, img.h);
          const swatch = mats?.colors?.[p.material] ?? null;
          Object.assign(rec, { shot: sh.path, frame: sh.frame, inGame: hex(inGame), ...(swatch ? { swatch: hex(swatch), ...lookDelta(swatch, inGame) } : { swatch: null, note: "the server gave no texture colour for this material" }) });
          if (a.images) pics.push(image(sh.png));
        } catch (e) {
          rec.error = (e as Error).message;
        }
        shots.push(rec);
      }
      const out = { prefix, materials: shots, pose: { distance: a.distance ?? 6, elevation: a.elevation ?? 50, azimuth: a.azimuth ?? 200, fov: 50 }, units: "map",
        note: "inGame: the mean colour of the middle 30 % of the shot (the patch, lit as the match lights it, with the mods' detail); swatch: the material file's texture mean (preview_materials' tile); delta per channel, lightness 0-255, and the cast the shot leans to: what the players see of the material, not what the swatch sheet shows" };
      return a.images ? { content: [...pics, { type: "text", text: JSON.stringify(out) }] } : out;
    },
  },
  {
    name: "game_probe",
    description: "What happened to the worms in the running (or last) Test match, from the game's Test probe (level.test.probe, Melange 0.13+; also on the session log as levels/test.spawns and levels/test.death in events.jsonl): every worm's position at turn 1 (WORMn its slot, as game_state; in a level with knot spawns, the open project's WORMn knot and how far from it the worm stood: a knot in solid or off an edge shows here), and each worm that died: when (game turn, tick), where, the water then, and the cause, inferred from what the probe saw: drown (the engine's drowning state, or its feet under the water), mine, oildrum or weapon (its latest hit: an explosion whose worm-damage radius reached it, with the source entity and the weapon of the worm whose turn it was), fall (its latest fall, the drop in map units), or unknown; `evidence` says what it was inferred from, `hits`, `falls` and `healthSteps` the history. Also the mine factory's fires (each mine's spawn and rest) and the explosions seen. `units` for the positions (default map).",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        units: { type: "string", enum: ["map", "world"], description: "Units of the positions (default map)." },
        explosions: { type: "boolean", description: "Also list every explosion seen (at most the last 64)." },
      },
    },
    async handler(s, a: { units?: "map" | "world"; explosions?: boolean }) {
      const r = await probeOf(s);
      if (!r) throw new ToolError("this game has no level.test.probe (Melange 0.13+): the Test probe is not there; game_state and game_mines read the match instead");
      const u = a.units ?? "map", p = r.probe;
      if (!p.key) return { probe: null, note: "no Test has played since the game started: test first" };
      // WORMn is the worm's slot, as game_state's worm (the Quick Game's slots: team 0 in 0-3, team 1 in 4-7)
      const wormName = (w: { slot: number }) => `WORM${w.slot}`;
      // the open project's knots, to set the worms' turn-1 positions against (a level in knots mode starts worm n there)
      let knots = new Map<string, number[]>();
      let byTeam = false;
      let areas: SpawnArea[] | null = null;
      try {
        const mode = s.store.scene.spawns.mode;
        if (mode === "knots" || mode === "teams" || mode === "areas") knots = new Map(markers(s.store).filter((m) => m.kind === "spawn").map((m) => [m.name, m.pos]));
        byTeam = mode === "teams";
        if (mode === "areas") areas = s.store.scene.spawns.areas ?? [];
      } catch { /* no project open */ }
      // spawns.mode areas: the match dealt each team an area from its seed (Melange's log: "spawn areas: seed"); the
      // team's area is read back as the one whose knot is nearest its first worm, and each worm checked against its
      // place there
      const areaOfTeam = new Map<number, number>();
      if (areas) for (const w of [...p.spawns].sort((x, y) => x.slot - y.slot)) {
        if (areaOfTeam.has(w.team)) continue;
        let best = -1, bestD = Infinity;
        areas.forEach((ar, i) => { for (let k = 0; k < ar.knots; k++) { const kn = knots.get(`WORM${areaFirstKnot(areas!, i) + k}`); if (!kn) continue;
          const d = Math.hypot(w.pos[0] - kn[0], w.pos[1] - kn[1], w.pos[2] - kn[2]); if (d < bestD) { bestD = d; best = i; } } });
        if (best >= 0) areaOfTeam.set(w.team, best);
      }
      // spawns.mode teams: a worm's knot is WORM<4 x team + its place in the team>, the place counted in slot order (as
      // the generated chunk deals them)
      const knotOf = (w: { slot: number; team: number }) => {
        if (areas) {
          const ai = areaOfTeam.get(w.team), place = p.spawns.filter((x) => x.team === w.team && x.slot < w.slot).length;
          return ai !== undefined && place < areas[ai].knots ? `WORM${areaFirstKnot(areas, ai) + place}` : "";
        }
        if (!byTeam) return wormName(w);
        const place = p.spawns.filter((x) => x.team === w.team && x.slot < w.slot).length;
        return place < 4 ? `WORM${w.team * 4 + place}` : "";
      };
      const k = u === "world" ? WORLD_PER_MAP : 1, r2 = (v: number) => Math.round(v * 100) / 100;
      const explosionOut = (x: ProbeExplosion) => ({ turn: x.turn, tick: x.tick, at: inUnits(x.at, u), source: x.source, wormDamage: x.wormDamage, radius: r2(x.wormDamageRadiusMap * k),
        ...(x.entity ? { entity: { kind: x.entity.kind, type: x.entity.type, label: x.entity.label, handle: x.entity.handle } } : {}),
        ...(x.turnWorm !== undefined ? { turnWormSlot: x.turnWorm } : {}), ...(x.turnWeapon ? { turnWeapon: x.turnWeapon } : {}) });
      const spawns = p.spawns.map((w) => {
        const n = wormName(w), kname = knotOf(w), kn = knots.get(kname);
        return { worm: n, name: w.name, team: w.team, slot: w.slot, at: inUnits(w.pos, u), health: w.health,
          ...(areas && areaOfTeam.has(w.team) ? { area: areas[areaOfTeam.get(w.team)!].name } : {}),
          ...(kn ? { knot: inUnits(kn, u), ...(byTeam || areas ? { knotName: kname } : {}), fromKnot: r2(Math.hypot(w.pos[0] - kn[0], w.pos[1] - kn[1], w.pos[2] - kn[2]) * k) } : {}) };
      });
      const deaths = p.deaths.map((d) => ({ worm: wormName(d), name: d.name, team: d.team, slot: d.slot, turn: d.turn, tick: d.tick, cause: d.cause, evidence: d.evidence,
        at: inUnits(d.pos, u), lastAlive: inUnits(d.lastAlivePos, u), health: d.health,
        ...(d.waterMap !== undefined ? { water: { map: r2(d.waterMap), world: r2(d.waterWorld ?? d.waterMap * WORLD_PER_MAP), feetOverWater: r2((d.lastAlivePos[1] - d.waterMap) * k) } } : {}),
        ...(d.source ? { source: explosionOut(d.source) } : {}),
        ...(d.hits.length ? { hits: d.hits.map((h) => ({ ...explosionOut(h.explosion), distance: r2(h.distanceMap * k) })) } : {}),
        ...(d.falls.length ? { falls: d.falls.map((f) => ({ tick: f.tick, drop: r2(f.dropMap * k), from: r2(f.fromY * k), to: r2(f.toY * k) })) } : {}),
        ...(d.healthSteps.length ? { healthSteps: d.healthSteps } : {}) }));
      return {
        key: p.key, playing: p.playing, turn: p.turn, units: u,
        spawns: spawns.length ? spawns : null, ...(spawns.length ? {} : { spawnsNote: "the first turn has not started yet" }),
        deaths, explosionsSeen: p.explosionsSeen, ...(a.explosions ? { explosions: p.explosions.map(explosionOut) } : {}),
        factory: { fires: r.factory.fires.map((f) => fireOut(f, u, ...groundLookup(s))), ...(r.factory.following ? { following: r.factory.following } : {}),
          ...(r.factory.held !== undefined ? { held: r.factory.held } : {}), ...(r.factory.drowned !== undefined ? { drowned: r.factory.drowned } : {}), trigger: r.factory.trigger },
        note: "turn counts game turns from 1 (as game_state and check_flood); causes are inferred from what the probe saw (evidence), not read from the engine, which keeps no damage source; WORMn is the worm's slot (game_state's worm); knot and fromKnot only for a level in knots, teams or areas spawn mode (teams: knotName, the team's knot the worm stood on: WORM<4 x team + its place in the team>; areas: area, the area whose knot is nearest the team's first worm, and knotName, that area's knot for the worm's place in its team)",
      };
    },
  },
];
