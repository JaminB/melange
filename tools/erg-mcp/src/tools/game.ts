// Tool group "game": looking at a Test while it plays, through the running game's own Oasis methods. game_state reads
// the match (turn, worms, water), game_wait waits for a turn or a time, game_camera points the drawn camera at a place
// (wum.render.cameraOverride: the picture only, the game's camera and the match are untouched). They run Lua in the
// game's client console (lua.eval, target client), so they need the game running with Oasis writable, offline (the
// console refuses online matches unless [LuaConsole] MatchConsoleOnline=1). A game with render.camera and render.shot
// (Melange's own Oasis methods) points the camera with no client mod, and game_camera {shot} and game_shots read the
// picture back (clean: without the mods' wum.draw text).
import { appendFileSync, existsSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { extentXZ, markers, spansAt } from "../lib/inspect-field";
import { grids } from "../lib/voxel";
import { pieces } from "../lib/islets";
import { cumulative, keyOfLabel, planWalk, pursue } from "../lib/walk-plan";
import { WORM_WIDTH } from "../lib/reach";
import { ToolError, image, type Content, type Tool } from "../registry";
import type { Session } from "../session";

const WORLD_PER_MAP = 20;

async function evalClient(s: Session, code: string, mod?: string): Promise<string> {
  const conn = await s.ensure();
  if (!conn.methods.includes("lua.eval")) throw new ToolError("no lua.eval here: the game tools need the game running (Melange.exe --serve has no match)");
  const r = await (await s.client()).call<{ ok: boolean; text: string }>("lua.eval", mod ? { target: "mod", mod, code } : { target: "client", code }, 15000);
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
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

/** The overview preset's camera: azimuth (degrees clockwise from north, -z; default 180, from the south), elevation
 * (degrees down; default 50), distance from the level's middle (map units; default 0.95 x the level's span), and what it
 * frames (`fit`): the frames' boxes (the default, as before), the solid voxels, or the island (the solid voxels over the
 * water), each `margin` times as far as the closest pose that shows all of it in a 60 degree, 16:9 view. */
interface Orbit { azimuth?: number; elevation?: number; distance?: number; fit?: "frames" | "solid" | "island"; margin?: number; }
/** set_overview's framing per project id: the overview's defaults for the rest of this erg-mcp session. */
const overviewDefaults = new Map<string, Orbit>();
interface Cam extends Orbit { pos?: number[]; target?: number[]; fov?: number; preset?: "default" | "overview"; name?: string; }

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
interface FireRec { stem: string; tick: number; turn: number; asked: number; launch: boolean; factory: number[];
  mines: { handle: number; spawn: number[]; rest?: number[]; settled?: boolean; gone?: boolean; lastSeen?: number[]; restFromFactoryMap: number;
    // a game that watches the mines after the fire (this branch's levels/factory.cpp): what became of each
    fate?: "resting" | "not seen to land" | "drowned" | "destroyed" | "gone before it landed"; goneTick?: number; waterMap?: number; moves?: number; movedAfterRest?: number }[]; }
interface ProbeAnswer {
  probe: { key: string; playing: boolean; turn: number; tick: number; spawns: ProbeWorm[]; deaths: ProbeDeath[]; explosionsSeen: number; explosions: ProbeExplosion[] };
  factory: { fires: FireRec[]; following: number; trigger: { tick: number; count: number; started: boolean; why: string; detail: string } | null };
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
    restFromFactory: Math.round(m.restFromFactoryMap * k * 100) / 100 }));
  return { tick: f.tick, turn: f.turn, mines: f.mines.length, launch: f.launch, factory: inUnits(f.factory, u), ...(Object.keys(fates).length ? { fates } : {}),
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

function automationFile(gameDir?: string): string {
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

const withMap = (st: GameState) => ({ ...st, ...(st.water !== undefined ? { waterMap: Math.round((st.water / WORLD_PER_MAP) * 100) / 100 } : {}) });

const units = { type: "string", enum: ["map", "world"], description: "Units of pos and target: map (default, as the other tools) or world (map x 20, the game's)." } as const;
const vec3 = (d: string) => ({ type: "array", items: { type: "number" }, minItems: 3, maxItems: 3, description: d }) as const;

export const tools: Tool[] = [
  {
    name: "game_state",
    description: "The running match as the game sees it: scene (menu, loading, match), the turn (counted from 1 as wum.sim.turnStarted and check_flood count, team, the active worm's slot, phase, ai), turnLog (the turns seen so far, and turnOrder for check_flood once every turn from 1 was seen), water level (world and map units), turn and round time left, sudden death, worms alive per team, and the sim tick. Needs the game running (after test).",
    inputSchema: { type: "object", additionalProperties: false, properties: {} },
    async handler(s) {
      const st = await gameState(s);
      noteTurn(st);
      const log = turnLogOut(), ann = st.scene === "match" ? await announcementState(s) : undefined;
      return { ...withMap(st), turnNote: TURN_NOTE, ...(log ? { turnLog: log } : {}), ...(ann ? { announcements: ann } : {}), camera: holdState() };
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
        const cam = a.preset === "overview" ? overview(s, a, k) : (() => {
          if (!a.pos || !a.target) throw new ToolError("give pos and target, a preset, or clear: true");
          if (![...a.pos, ...a.target].every(Number.isFinite)) throw new ToolError("pos and target are finite numbers");
          return { pos: a.pos.map((x) => x * k), target: a.target.map((x) => x * k), fov: a.fov };
        })();
        if (a.preset === "overview" && (a.pos || a.target)) throw new ToolError("preset overview takes no pos or target");
        stopHold();
        const pose = { ...cam, fov: a.fov ?? cam.fov };
        const r = await setCamera(s, pose, frames, a.mod);
        if (!a.mod) { shotPose = pose; shotFrames = frames; }
        frame = r.frame;
        if (a.hold) startHold(s, pose, a.mod);
        out = { set: true, owner: r.owner, ...(a.mod ? { mod: a.mod } : {}), frames, ...(a.preset ? { preset: a.preset } : {}), worldPos: cam.pos, worldTarget: cam.target,
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
          preset: { type: "string", enum: ["default", "overview"] }, ...orbitProps, name: { type: "string", description: "The PNG's name after the prefix: <prefix>_<name> (default <prefix>_<n>)." } } } },
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
      },
    },
    async handler(s, a: { cams: Cam[]; units?: "map" | "world"; prefix?: string; unique?: boolean; onAnnounce?: { match?: string; timeoutSec?: number }; clean?: boolean; width?: number; settleFrames?: number;
      waitPlay?: boolean; waitSec?: number; hold?: boolean; holdLast?: boolean; images?: boolean }) {
      if (!(await has(s, "render.camera")) || !(await has(s, "render.shot"))) throw new ToolError("game_shots needs the game's render.camera and render.shot (a Melange with them, running)");
      if (a.waitPlay && a.onAnnounce) throw new ToolError("waitPlay and onAnnounce both decide when to shoot: give one");
      const k = a.units === "world" ? 1 : WORLD_PER_MAP, t0 = Date.now(), settle = a.settleFrames ?? (a.waitPlay ? 6 : 2);
      let playSupported: boolean | undefined;
      const shots: object[] = [], parts: Content[] = [], warnings: string[] = [], prefix = a.prefix ?? `erg_${stamp()}`;
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
            const cam = c.preset === "overview" ? overview(s, c, k) : (() => {
              if (!c.pos || !c.target) throw new ToolError(`cams[${i}]: give pos and target, or a preset`);
              return { pos: c.pos.map((x) => x * k), target: c.target.map((x) => x * k), fov: c.fov };
            })();
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
          shots.push({ name, ...(c.preset ? { preset: c.preset } : {}), ...(c.preset === "overview" && lastPose ? { azimuth: c.azimuth ?? 180, worldPos: lastPose.pos.map((v) => Math.round(v * 10) / 10) } : {}),
            path: p.path, width: p.width, height: p.height, frame: p.frame, clean: p.clean, ...(p.phase !== undefined ? { phase: p.phase } : {}), ...(play ? { waitedMs: p.waitedMs } : {}) });
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
        units: { type: "string", enum: ["map", "world"], description: "Units of the route and the answer (default map)." },
        worm: { type: "integer", minimum: 0, maximum: 7, description: "Walk only this WORMn: wait for its turn (default: the first human turn that comes)." },
        arrive: { type: "number", minimum: 0.2, description: "The end (or a waypoint) is reached within this horizontal distance (units; default 1.5 map units)." },
        timeoutSec: { type: "number", minimum: 1, maximum: 300, description: "Give up after this long, waiting included (default 120)." },
        jump: { type: "boolean", description: "Tap jump where the worm makes no progress for 0.6 s (default true; false: no recovery at all)." },
        gameDir: { type: "string", description: "The running game's folder (WormsMayhem.exe), for its Automation command file (default MELANGE_GAME_DIR); only for driver keys." },
        driver: { type: "string", enum: ["auto", "input", "keys"], description: "How the worm is moved: input posts the game's own movement inputs (Input.MoveForward ... Pressed and Released, as the keys bound to them post them) from the input service's tick (input.drive, Melange 0.13+), so it works whatever window has the focus; keys injects W A S D through the Automation module (test builds), which the game reads only while its window has the focus (in the Caldera iteration-10 Test the window lost it a third of the time and W moved no worm). auto (default): input when the game has input.drive, else keys." },
      },
    },
    async handler(s, a: { waypoints?: number[][]; path?: number[][]; to?: number[]; lookahead?: number; bodyWidth?: number; recover?: boolean; units?: "map" | "world"; worm?: number; arrive?: number;
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
      let cum = cumulative(pts);
      const path: number[][] = [[start.x, start.y, start.z]], stuck: number[][] = [], tries: string[] = [];
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
        inputs: { type: "array", minItems: 1, maxItems: 4, items: { type: "string", enum: ["W", "A", "S", "D", "Jump", "Backflip", "Fire", "AimUp", "AimDown"] }, description: "What to hold (two walk keys walk the diagonal)." },
        seconds: { type: "number", minimum: 0.05, maximum: 20, description: "How long to hold them (default 2)." },
        worm: { type: "integer", minimum: 0, maximum: 7, description: "Only this WORMn's turn (default the first human turn in play)." },
        waitSec: { type: "number", minimum: 1, maximum: 300, description: "Wait at most this long for the turn (default 90)." },
        units: { type: "string", enum: ["map", "world"], description: "Units of the answer (default map)." },
      },
    },
    async handler(s, a: { inputs: string[]; seconds?: number; worm?: number; waitSec?: number; units?: "map" | "world" }) {
      if (!(await has(s, "input.drive"))) throw new ToolError("game_drive needs the game's input.drive (Melange 0.13+ with the Controls module)");
      const NAME: Record<string, string> = { W: "MoveForward", A: "MoveLeft", S: "MoveBackward", D: "MoveRight", Jump: "Jump", Backflip: "JumpBack", Fire: "Fire", AimUp: "AimUp", AimDown: "AimDown" };
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
    name: "game_fire_shot",
    description: "Pin a camera for the mine factory's fires in the running Test: every time the factory fires (each PlaceMines, at a turn start or game_mines {fire}), the game points the camera at the pose `delayMs` after it (default 500: the mines in flight), holds it `holdFrames` frames, writes Documents\\\\Melange\\\\shots\\\\<prefix>_<n>.png and gives the camera back, for up to `max` fires (render.fireShot, Melange 0.13+). No watch is needed: it stays armed for the match. The default pose looks down into the crater: from 20 map units south and 25 over the open project's factory knot, at the knot. `clear` disarms it; with neither a pose nor clear it answers the state and the shots taken so far (with `images`, as pictures).",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        pos: vec3("Eye [x, y, z] in `units`."), target: vec3("Point looked at [x, y, z] in `units`."), fov: { type: "number", minimum: 5, maximum: 150 },
        units: { type: "string", enum: ["map", "world"], description: "Units of pos and target (default map)." },
        arm: { type: "boolean", description: "Arm it with the default pose (the crater over the factory knot) when no pos is given." },
        delayMs: { type: "integer", minimum: 0, maximum: 10000, description: "After the fire (default 500)." },
        max: { type: "integer", minimum: 1, maximum: 32, description: "Fires to shoot before it disarms itself (default 4)." },
        holdFrames: { type: "integer", minimum: 1, maximum: 60, description: "Frames drawn from the pose before it is read (default 2)." },
        prefix: { type: "string", description: "The PNGs are <prefix>_<n> (default erg_fireshot_<yyyymmdd_hhmmss>)." },
        clear: { type: "boolean", description: "Disarm it." },
        images: { type: "boolean", description: "With the state: answer the shots taken as pictures (default false)." },
      },
    },
    async handler(s, a: { pos?: number[]; target?: number[]; fov?: number; units?: "map" | "world"; arm?: boolean; delayMs?: number; max?: number; holdFrames?: number; prefix?: string; clear?: boolean; images?: boolean }) {
      if (!(await has(s, "render.fireShot"))) throw new ToolError("this game has no render.fireShot (Melange 0.13+): use game_mines {fire, shotsOnFire} instead");
      const client = await s.client();
      type St = { armed: boolean; name: string; delayMs: number; taken: number; max: number; paths: string[]; fires: number; lastError?: string };
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
      const name = uniqueName(a.prefix ?? `erg_fireshot_${stamp()}`, "fire", false).replace(/_fire$/, "");
      const st = await client.call<St>("render.fireShot", { ...pose, ...(a.fov ? { fov: a.fov } : {}), name: name.slice(0, 56), delayMs: a.delayMs ?? 500, max: a.max ?? 4, holdFrames: a.holdFrames ?? 2 }, 15000);
      return { ...st, pose: { pos: pose.pos.map((v) => Math.round(v / kk * 100) / 100), target: pose.target.map((v) => Math.round(v / kk * 100) / 100), units: a.units ?? "map" },
        note: `armed: each of the next ${a.max ?? 4} factory fires is shot ${a.delayMs ?? 500} ms after it; game_fire_shot {} answers the shots taken (the game also logs each, [oasis] render.fireShot)` };
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
            width: { type: "integer", minimum: 64, maximum: 8192, description: "Picture width (default 960)." }, images: { type: "boolean", description: "Answer the pictures as image content (default true)." } } },
      },
    },
    async handler(s, a0: { watchSeconds?: number; untilFire?: boolean; units?: "map" | "world"; alive?: boolean; trajectories?: boolean; fire?: boolean | { count?: number };
      shotsOnFire?: { count?: number; everyMs?: number; prefix?: string; pos?: number[]; target?: number[]; width?: number; images?: boolean } }) {
      if (!(await has(s, "entities.list"))) throw new ToolError("this game has no entities.list (Oasis's game state readers): game_mines needs the game running a Test");
      // fire: a volley now, watched until its mines are out
      const fireAsk = a0.fire === undefined || a0.fire === false ? null : a0.fire === true ? {} : a0.fire;
      if (fireAsk && !(await has(s, "level.test.fireFactory"))) throw new ToolError("this game has no level.test.fireFactory (Melange 0.13+): the factory fires at turn starts only; set_mine_factory {everyNTurns: 1} and game_mines {untilFire: true}");
      const a = fireAsk ? { ...a0, untilFire: true, watchSeconds: a0.watchSeconds ?? 60 } : a0;
      const firesBefore = fireAsk ? ((await probeOf(s))?.factory.fires.length ?? 0) : 0;
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
      let queued: unknown = null;
      if (fireAsk) queued = await (await s.client()).call("level.test.fireFactory", fireAsk.count ? { count: fireAsk.count } : {}, 15000);
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
      const tracks = new Map<number, number[][]>();
      try {
        const appeared: { handle: number; type: string; ms: number; at: number[] | null; fromFactory?: number; path?: number[][] }[] = [];
        while (Date.now() - t0 < watch && !(a.untilFire && firedAt !== null && Date.now() - firedAt > 3000 && (!sf || shots.length >= (sf.count ?? 4)))) {
          await sleep(250);
          for (const m of await liveMines(s).catch(() => [] as Entity[])) {
            if (!seen.has(m.handle)) { seen.set(m.handle, m); appeared.push({ handle: m.handle, type: m.type, ms: Date.now() - t0, at: out(m.pos), fromFactory: dist(m.pos) }); tracks.set(m.handle, []); }
            const tr = a.trajectories ? tracks.get(m.handle) : undefined;
            if (tr && tr.length < 80 && m.pos) tr.push([Date.now() - t0, ...out(m.pos)!]);
          }
          if (sf && pose && fireStart !== null && shots.length < (sf.count ?? 4) && Date.now() >= fireStart + shots.length * (sf.everyMs ?? 500)) {
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
        // fire: what the trigger found, then (its mines at rest, at most 35 s) the fire's mines from the game's own record
        let fired: object | undefined;
        if (fireAsk) {
          let pr = await probeOf(s);
          for (const t1 = Date.now(); pr && pr.factory.trigger?.started && pr.factory.fires.length <= firesBefore && Date.now() - t1 < 35000; pr = await probeOf(s)) await sleep(500);
          const trig = pr?.factory.trigger ?? null, last = pr && pr.factory.fires.length > firesBefore ? pr.factory.fires[pr.factory.fires.length - 1] : null;
          fired = { queued, trigger: trig, ...(last ? { fire: fireOut(last, a.units ?? "map", ...groundLookup(s)) } : { fire: null, note: trig && !trig.started ? `the factory did not fire: ${trig.why}` : "no fire record within 35 s (its mines may still be moving): game_probe's factory.fires has it later" }) };
        }
        const answer = {
          units: a.units ?? "map", factory: factory ? { at: out({ x: factory[0], y: factory[1], z: factory[2] }), note: "the open project's minefactory knot" } : null,
          ...(fired ? { onDemand: fired } : {}),
          ...(a.untilFire ? { fired: firedAt !== null, ...(firedAt !== null ? { fireEndMs: firedAt - t0 } : {}) } : {}),
          ...(factoryEntity ? { factoryEntity } : {}),
          minesBefore: before.length, mines: now.map((m) => ({ handle: m.handle, type: m.type, at: out(m.pos), fromFactory: dist(m.pos) })),
          ...(watch > 0 ? { watchedMs: Date.now() - t0, appeared, messages: { watched: names, counts, events } } : {}),
          ...(sf ? { shotsOnFire: fireStart === null ? { taken: 0, note: "no MineFactory.Fire in the watch" } : { taken: shots.length, shots } } : {}),
        };
        return pictures.length ? { content: [...pictures, { type: "text", text: JSON.stringify(answer, null, 1) }] } : answer;
      } finally {
        unsub?.();
        if (posed) await clearCamera(s).catch(() => undefined);
      }
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
      try { if (s.store.scene.spawns.mode === "knots") knots = new Map(markers(s.store).filter((m) => m.kind === "spawn").map((m) => [m.name, m.pos])); } catch { /* no project open */ }
      const k = u === "world" ? WORLD_PER_MAP : 1, r2 = (v: number) => Math.round(v * 100) / 100;
      const explosionOut = (x: ProbeExplosion) => ({ turn: x.turn, tick: x.tick, at: inUnits(x.at, u), source: x.source, wormDamage: x.wormDamage, radius: r2(x.wormDamageRadiusMap * k),
        ...(x.entity ? { entity: { kind: x.entity.kind, type: x.entity.type, label: x.entity.label, handle: x.entity.handle } } : {}),
        ...(x.turnWorm !== undefined ? { turnWormSlot: x.turnWorm } : {}), ...(x.turnWeapon ? { turnWeapon: x.turnWeapon } : {}) });
      const spawns = p.spawns.map((w) => {
        const n = wormName(w), kn = knots.get(n);
        return { worm: n, name: w.name, team: w.team, slot: w.slot, at: inUnits(w.pos, u), health: w.health,
          ...(kn ? { knot: inUnits(kn, u), fromKnot: r2(Math.hypot(w.pos[0] - kn[0], w.pos[1] - kn[1], w.pos[2] - kn[2]) * k) } : {}) };
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
        factory: { fires: r.factory.fires.map((f) => fireOut(f, u, ...groundLookup(s))), ...(r.factory.following ? { following: r.factory.following } : {}), trigger: r.factory.trigger },
        note: "turn counts game turns from 1 (as game_state and check_flood); causes are inferred from what the probe saw (evidence), not read from the engine, which keeps no damage source; WORMn is the worm's slot (game_state's worm); knot and fromKnot only for a level in knots spawn mode",
      };
    },
  },
];
