// Tool group "game": looking at a Test while it plays, through the running game's own Oasis methods. game_state reads
// the match (turn, worms, water), game_wait waits for a turn or a time, game_camera points the drawn camera at a place
// (wum.render.cameraOverride: the picture only, the game's camera and the match are untouched). They run Lua in the
// game's client console (lua.eval, target client), so they need the game running with Oasis writable, offline (the
// console refuses online matches unless [LuaConsole] MatchConsoleOnline=1). A game with render.camera and render.shot
// (Melange's own Oasis methods) points the camera with no client mod, and game_camera {shot} and game_shots read the
// picture back (clean: without the mods' wum.draw text).
import { appendFileSync, existsSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { extentXZ, markers } from "../lib/inspect-field";
import { grids } from "../lib/voxel";
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
 * (degrees down; default 50), distance from the level's middle (map units; default 0.95 x the level's span). */
interface Orbit { azimuth?: number; elevation?: number; distance?: number; }
interface Cam extends Orbit { pos?: number[]; target?: number[]; fov?: number; preset?: "default" | "overview"; name?: string; }

const orbitProps = {
  azimuth: { type: "number", minimum: -360, maximum: 360, description: "preset overview: the side it looks from, degrees clockwise from north (-z); default 180 (from the south)." },
  elevation: { type: "number", minimum: 5, maximum: 89, description: "preset overview: degrees it looks down (default 50)." },
  distance: { type: "number", minimum: 1, description: "preset overview: distance from the level's middle in `units` (default 0.95 x the level's span)." },
} as const;

/** The level's own overview: above its frames' middle, looking down at 50 degrees from the south (or `o`'s), far
 * enough back for the whole extent in a 60 degree view. World units. Needs the project open (its frames). */
function overview(s: Session, o: Orbit = {}, k = WORLD_PER_MAP): { pos: number[]; target: number[]; fov: number } {
  const gs = grids(s.store), e = extentXZ(gs);
  if (!e) throw new ToolError("preset overview needs the level's frames: open_project the level being tested");
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
async function setCamera(s: Session, c: { pos: number[]; target: number[]; fov?: number }, frames: number, mod?: string): Promise<{ frame?: number; owner: string }> {
  if (!mod && (await has(s, "render.camera"))) {
    const r = await (await s.client()).call<{ set: boolean; frame: number }>("render.camera", { pos: c.pos, target: c.target, ...(c.fov ? { fov: c.fov } : {}), frames }, 15000)
      .catch((e: Error) => { throw new ToolError(`the camera override was refused: ${e.message}`); });
    return { frame: r.frame, owner: "oasis" };
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
async function shot(s: Session, name: string, o: { width?: number; clean: boolean; minFrame?: number }): Promise<{ path: string; width: number; height: number; frame: number; clean: boolean; png: Uint8Array }> {
  if (!(await has(s, "render.shot"))) throw new ToolError("this game has no render.shot (Melange before erg-mcp's game_shots): take screenshots another way");
  const client = await s.client(), t0 = Date.now();
  for (;;) {
    const r = await client.call<{ ready: boolean; readyAt?: number; path?: string; width?: number; height?: number; frame: number; clean?: boolean }>("render.shot",
      { name, clean: o.clean, ...(o.width ? { width: o.width } : {}), ...(o.minFrame ? { minFrame: o.minFrame } : {}) }, 15000);
    if (r.ready && r.path) return { path: r.path, width: r.width!, height: r.height!, frame: r.frame, clean: !!r.clean, png: readFileSync(r.path) };
    if (Date.now() - t0 > 8000) throw new ToolError(`no frame to read after 8 s (at frame ${r.frame}, waiting for ${r.readyAt}): is the game minimised or paused?`);
    await sleep(30);
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

// ------------------------------------------------------------------ announcements

/** A wum.sim.announce message as the game logged it (jlog sim/announce, on Oasis's log channel): its text, the mod or
 * level script (erg:<project>) that sent it, how long it shows (Melange 0.13 logs it; 4 s, the default, before), when. */
export interface Announce { text: string; mod?: string; seconds: number; wall: number; seq: number; }

const parseAnnounce = (m: { seq?: number; j?: unknown }): Announce | null => {
  let j = m.j as { msg?: string; wall?: string; data?: { text?: string; mod?: string; seconds?: number }; seq?: number } | string | undefined;
  if (typeof j === "string") try { j = JSON.parse(j); } catch { return null; }
  if (!j || typeof j !== "object" || j.msg !== "announce" || typeof j.data?.text !== "string") return null;
  const wall = j.wall ? Date.parse(j.wall) : NaN;
  return { text: j.data.text, ...(j.data.mod ? { mod: j.data.mod } : {}), seconds: j.data.seconds ?? 4, wall: Number.isFinite(wall) ? wall : Date.now(), seq: j.seq ?? m.seq ?? 0 };
};

/** Follows the game's announcements: `fn` gets each one the log channel has (its backlog of the last 500 sim records,
 * then live ones). Null when the game has no log channel. */
async function followAnnouncements(s: Session, fn: (a: Announce) => void): Promise<(() => void) | null> {
  const client = await s.client();
  if (!client.has("log")) return null;
  return client.subscribe<{ seq?: number; j?: unknown }>("log", { cats: ["sim"], text: "announce" }, (m) => { const a = parseAnnounce(m); if (a) fn(a); });
}

/** The announcements on the screen now and the last few, for game_state (about a third of a second to read). */
async function announcementState(s: Session) {
  const got: Announce[] = [];
  const stop = await followAnnouncements(s, (a) => got.push(a)).catch(() => null);
  if (!stop) return undefined;
  await sleep(300);
  stop();
  const now = Date.now(), seen = new Set<string>(), key = (a: Announce) => `${a.seq}:${a.wall}:${a.text}`;
  const list = got.filter((a) => !seen.has(key(a)) && seen.add(key(a))).sort((a, b) => a.wall - b.wall);
  const on = list.filter((a) => a.wall + a.seconds * 1000 > now);
  return {
    onScreen: on.map((a) => ({ text: a.text, ...(a.mod ? { mod: a.mod } : {}), secondsLeft: Math.round((a.wall + a.seconds * 1000 - now) / 100) / 10 })),
    recent: list.slice(-5).map((a) => ({ text: a.text, ...(a.mod ? { mod: a.mod } : {}), secondsAgo: Math.round((now - a.wall) / 100) / 10, seconds: a.seconds })),
    note: "wum.sim.announce messages from the game's log (this session of the game): onScreen are still drawn (the game shows up to 3 at once); game_shots onAnnounce shoots while one is up",
  };
}

/** Waits for an announcement whose text matches `match` (a case-insensitive regular expression; any without) that is
 * on the screen now or comes within `timeoutMs`. */
async function waitAnnouncement(s: Session, match: string | undefined, timeoutMs: number): Promise<Announce> {
  let re: RegExp | null = null;
  if (match) try { re = new RegExp(match, "i"); } catch (e) { throw new ToolError(`onAnnounce.match: ${(e as Error).message}`); }
  const t0 = Date.now();
  let found: Announce | null = null;
  const stop = await followAnnouncements(s, (a) => {
    if (!found && (!re || re.test(a.text)) && a.wall + a.seconds * 1000 > Date.now() + 300) found = a;
  });
  if (!stop) throw new ToolError("onAnnounce needs the game's log channel (Oasis): this game has none");
  try {
    while (!found && Date.now() - t0 < timeoutMs) await sleep(50);
  } finally {
    stop();
  }
  if (!found) throw new ToolError(`no announcement${match ? ` matching /${match}/i` : ""} came on the screen within ${Math.round(timeoutMs / 1000)} s (game_state's announcements lists the recent ones)`);
  return found;
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
        hold: { type: "boolean", description: "Keep the last pose for 5 s instead of giving the camera back." },
        holdLast: { type: "boolean", description: "Keep the last pose until released (game_camera {clear: true}), as game_camera's hold." },
        images: { type: "boolean", description: "Answer the pictures as image content (default true); false: paths only." },
      },
    },
    async handler(s, a: { cams: Cam[]; units?: "map" | "world"; prefix?: string; unique?: boolean; onAnnounce?: { match?: string; timeoutSec?: number }; clean?: boolean; width?: number; settleFrames?: number; hold?: boolean; holdLast?: boolean; images?: boolean }) {
      if (!(await has(s, "render.camera")) || !(await has(s, "render.shot"))) throw new ToolError("game_shots needs the game's render.camera and render.shot (a Melange with them, running)");
      const k = a.units === "world" ? 1 : WORLD_PER_MAP, t0 = Date.now(), settle = a.settleFrames ?? 2;
      const shots: object[] = [], parts: Content[] = [], warnings: string[] = [], prefix = a.prefix ?? `erg_${stamp()}`;
      // onAnnounce: wait for the message, then shoot at once and not clean (the announcement is drawn with the HUD
      // primitives a clean shot hides)
      const ann = a.onAnnounce ? await waitAnnouncement(s, a.onAnnounce.match, (a.onAnnounce.timeoutSec ?? 60) * 1000) : null;
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
            frame = (await setCamera(s, lastPose, frames)).frame;
            const check = await confirmPose(s, lastPose, frames);
            if (check && !check.ok) warnings.push(`${name}: the game drew from ${check.drawn!.map((v) => Math.round(v))} world, not the pose ${lastPose.pos.map((v) => Math.round(v))}: another client (a held game_camera in another erg-mcp) has the camera`);
            if (check?.retried) frame = undefined;
          }
          const p = await shot(s, name, { width: a.width ?? 960, clean: cleanShots, minFrame: frame !== undefined ? frame + settle : undefined });
          shots.push({ name, ...(c.preset ? { preset: c.preset } : {}), ...(c.preset === "overview" && lastPose ? { azimuth: c.azimuth ?? 180, worldPos: lastPose.pos.map((v) => Math.round(v * 10) / 10) } : {}),
            path: p.path, width: p.width, height: p.height, frame: p.frame, clean: p.clean });
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
      parts.push({ type: "text", text: JSON.stringify({ shots, ms: Date.now() - t0, camera, ...(ann ? { announcement: { text: ann.text, ...(ann.mod ? { mod: ann.mod } : {}), seconds: ann.seconds, shownMsBeforeFirstShot: Math.max(0, t0 - ann.wall) } } : {}),
        ...(warnings.length ? { warnings } : {}) }, null, 1) });
      return { content: parts };
    },
  },
  {
    name: "game_walk",
    description: "Walk the active worm of the running Test along waypoints, in the game, and report how far it got: the in-game check of a check_reachability route (its waypoints or path). The keys are the game's own (W, A, S, D relative to the camera; the pair nearest each bearing, steered every 0.12 s from the worm's position), sent through Melange's Automation module (test builds with [Automation] Enabled=1; gameDir or MELANGE_GAME_DIR names the game's folder). It waits for a turn of a human team in its play phase (`worm`: only that WORMn's), walks until the last waypoint is within `arrive`, the turn ends, the worm dies or `timeoutSec`, and taps jump (RETURN) where it stops making progress. Answers reached (the waypoints reached, and whether the last), where it ended and how far from the target, the climb, the time, where it got stuck, and samples of its path, in `units`.",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["waypoints"],
      properties: {
        waypoints: { type: "array", minItems: 1, maxItems: 200, items: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 3 }, description: "The route: [x, z] or [x, y, z] (y unused), in `units`: check_reachability's waypoints." },
        units: { type: "string", enum: ["map", "world"], description: "Units of the waypoints and the answer (default map)." },
        worm: { type: "integer", minimum: 0, maximum: 7, description: "Walk only this WORMn: wait for its turn (default: the first human turn that comes)." },
        arrive: { type: "number", minimum: 0.2, description: "A waypoint is reached within this horizontal distance (units; default 1.5 map units)." },
        timeoutSec: { type: "number", minimum: 1, maximum: 300, description: "Give up after this long, waiting included (default 120)." },
        jump: { type: "boolean", description: "Tap jump where the worm makes no progress for 0.6 s (default true)." },
        gameDir: { type: "string", description: "The running game's folder (WormsMayhem.exe), for its Automation command file (default MELANGE_GAME_DIR)." },
      },
    },
    async handler(s, a: { waypoints: number[][]; units?: "map" | "world"; worm?: number; arrive?: number; timeoutSec?: number; jump?: boolean; gameDir?: string }) {
      const file = automationFile(a.gameDir);
      const k = a.units === "world" ? 1 : WORLD_PER_MAP;   // to world units
      const out = (v: number) => Math.round((v / k) * 100) / 100;
      const pts = a.waypoints.map((p) => (p.length === 3 ? [p[0] * k, p[2] * k] : [p[0] * k, p[1] * k]));
      if (!pts.every((p) => p.every(Number.isFinite))) throw new ToolError("waypoints are finite numbers");
      const arrive = (a.arrive ?? (a.units === "world" ? 30 : 1.5)) * k, limit = (a.timeoutSec ?? 120) * 1000, t0 = Date.now();
      const send = (lines: string[]) => appendFileSync(file, lines.join("\n") + "\n");
      // Automation reads the file every frame and removes what it ran: an unread line means the module is off
      const marker = `log erg-walk-${t0}`;
      send([marker]);
      await sleep(1500);
      if (readFileSync(file, "utf8").includes(marker)) throw new ToolError(`the game did not read ${file}: game_walk needs a Melange with the Automation module ([Automation] Enabled=1, a test build) running from that folder`);
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
          if (w.phase === "intro") send(["key RETURN 60"]);
        }
        await sleep(400);
        w = await read();
      }
      const start = w as W, path: number[][] = [[start.x, start.y, start.z]], stuck: number[][] = [];
      let i = 0, held: string[] = [], lastMove = Date.now(), lastPos = [start.x, start.z], end: W = start, why = "timeout", jumps = 0;
      const hold = (want: string[]) => {
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
          while (i < pts.length && Math.hypot(pts[i][0] - r.x, pts[i][1] - r.z) <= arrive) i++;
          if (i >= pts.length) { why = "arrived"; break; }
          hold(walkKeys(pts[i][0] - r.x, pts[i][1] - r.z, r.fx, r.fz));
          if (Math.hypot(r.x - lastPos[0], r.z - lastPos[1]) > 4) { lastPos = [r.x, r.z]; lastMove = Date.now(); }
          else if (Date.now() - lastMove > 600) {
            stuck.push([r.x, r.y, r.z]);
            if (a.jump !== false) { send(["key RETURN 80"]); jumps++; }
            lastMove = Date.now();
          }
          await sleep(WALK_TICK_MS);
        }
      } finally {
        hold([]);
      }
      const last = pts[pts.length - 1];
      const near = (p: number[]) => p.map(out);
      return {
        worm: `WORM${start.slot}`, team: start.team, reached: { waypoints: i, of: pts.length, target: i >= pts.length }, stop: why,
        from: near([start.x, start.y, start.z]), to: near([end.x, end.y, end.z]), toTarget: out(Math.hypot(last[0] - end.x, last[1] - end.z)), climb: out(end.y - start.y),
        seconds: Math.round((Date.now() - t0) / 100) / 10, jumps, ...(stuck.length ? { stuckAt: stuck.slice(0, 12).map(near) } : {}), path: path.slice(0, 120).map(near),
        units: a.units ?? "map", note: "the worm walked with the game's own keys (camera-relative W A S D, steered every 0.12 s; jump where it stopped): reached.target means it got to the last waypoint on foot in game",
      };
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
        units: { type: "string", enum: ["map", "world"], description: "Units of the positions (default map)." },
        alive: { type: "boolean", description: "Also answer `factoryEntity`: whether the game's MineFactoryLogicEntity exists in the match now, where it is, and how far from the project's knot (a factory that fell elsewhere or exploded at load shows here), and its countdown: idleTurnsLeft (the turn starts before it checks again; at 0 or under the next GameLogic.StartMineFactory fires when fewer mines than its target are in the level) and minesToFire." },
        trajectories: { type: "boolean", description: "With watchSeconds: answer each mine that appeared with its path, sampled every quarter second (ms, position in `units`; at most 80 samples a mine). The position is the one entities.list reads from the mine's payload, which the game moves when the mine comes to rest: in a Test on 2026-11-08 a fired mine read its launch point (72 map units up) for about 2 s and then where it landed, so the path gives the launch and the landing, not the arc; shotsOnFire shows the flight." },
        shotsOnFire: { type: "object", additionalProperties: false, description: "With watchSeconds: when the factory's MineFactory.Fire arrives, point the camera (render.camera) at the factory and take `count` shots (render.shot) `everyMs` apart, so the mines are seen in flight; the camera is given back after. Default pose: 60 map units south of the factory knot and 30 over it, looking at 10 over it; or pos and target (in `units`).",
          properties: { count: { type: "integer", minimum: 1, maximum: 12, description: "Shots (default 4)." }, everyMs: { type: "integer", minimum: 100, maximum: 5000, description: "Between shots (default 500)." },
            prefix: { type: "string", description: "PNG names <prefix>_<n> in Documents\Melange\shots (default erg_minefire_<yyyymmdd_hhmmss>)." }, pos: vec3("Eye [x, y, z]."), target: vec3("Point looked at [x, y, z]."),
            width: { type: "integer", minimum: 64, maximum: 8192, description: "Picture width (default 960)." }, images: { type: "boolean", description: "Answer the pictures as image content (default true)." } } },
      },
    },
    async handler(s, a: { watchSeconds?: number; untilFire?: boolean; units?: "map" | "world"; alive?: boolean; trajectories?: boolean;
      shotsOnFire?: { count?: number; everyMs?: number; prefix?: string; pos?: number[]; target?: number[]; width?: number; images?: boolean } }) {
      if (!(await has(s, "entities.list"))) throw new ToolError("this game has no entities.list (Oasis's game state readers): game_mines needs the game running a Test");
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
        const answer = {
          units: a.units ?? "map", factory: factory ? { at: out({ x: factory[0], y: factory[1], z: factory[2] }), note: "the open project's minefactory knot" } : null,
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
];
