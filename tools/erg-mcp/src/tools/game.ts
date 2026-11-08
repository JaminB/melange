// Tool group "game": looking at a Test while it plays, through the running game's own Oasis methods. game_state reads
// the match (turn, worms, water), game_wait waits for a turn or a time, game_camera points the drawn camera at a place
// (wum.render.cameraOverride: the picture only, the game's camera and the match are untouched). They run Lua in the
// game's client console (lua.eval, target client), so they need the game running with Oasis writable, offline (the
// console refuses online matches unless [LuaConsole] MatchConsoleOnline=1). A game with render.camera and render.shot
// (Melange's own Oasis methods) points the camera with no client mod, and game_camera {shot} and game_shots read the
// picture back (clean: without the mods' wum.draw text).
import { readFileSync } from "node:fs";
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
if m then put("water", m.waterLevel); put("turnMsLeft", m.turnMsLeft); put("roundMsLeft", m.roundMsLeft); put("suddenDeath", m.suddenDeath) end
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
  roundMsLeft?: number; suddenDeath?: boolean; worms: number; aliveByTeam: Record<string, number>; tick: number; }

async function gameState(s: Session): Promise<GameState> {
  const text = await evalClient(s, STATE_LUA);
  const kv = new Map(text.split(";").map((p) => { const i = p.indexOf("="); return [p.slice(0, i), p.slice(i + 1)] as [string, string]; }));
  const num = (k: string) => (kv.has(k) && kv.get(k) !== "nil" ? Number(kv.get(k)) : undefined);
  const alive: Record<string, number> = {};
  for (const p of (kv.get("aliveByTeam") ?? "").split(",").filter(Boolean)) { const [t, n] = p.split(":"); alive[t] = Number(n); }
  return {
    scene: kv.get("scene") ?? "?", turn: num("turn"), team: num("team"), worm: num("worm"), phase: kv.get("phase"), ai: kv.has("ai") ? kv.get("ai") === "true" : undefined,
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

interface Cam { pos?: number[]; target?: number[]; fov?: number; preset?: "default" | "overview"; name?: string; }

/** The level's own overview: above its frames' middle, looking down at 50 degrees from the south, far enough back for
 * the whole extent in a 60 degree view. World units. Needs the project open (its frames). */
function overview(s: Session): { pos: number[]; target: number[]; fov: number } {
  const gs = grids(s.store), e = extentXZ(gs);
  if (!e) throw new ToolError("preset overview needs the level's frames: open_project the level being tested");
  let top = -Infinity, bottom = Infinity;
  for (const g of gs) { top = Math.max(top, g.bounds.max[1]); bottom = Math.min(bottom, g.bounds.min[1]); }
  const cx = (e.x0 + e.x1) / 2, cz = (e.z0 + e.z1) / 2, cy = Math.max(0, (Math.min(top, 200) + Math.max(bottom, -50)) / 3);
  const span = Math.max(e.x1 - e.x0, e.z1 - e.z0), d = span * 0.95, el = (50 * Math.PI) / 180;
  return { pos: [cx * WORLD_PER_MAP, (cy + d * Math.sin(el)) * WORLD_PER_MAP, (cz + d * Math.cos(el)) * WORLD_PER_MAP], target: [cx * WORLD_PER_MAP, cy * WORLD_PER_MAP, cz * WORLD_PER_MAP], fov: 60 };
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

const shotName = (prefix: string, i: number) => `${prefix}${i === 0 ? "" : `_${i}`}`.replace(/[^A-Za-z0-9_.-]/g, "_").slice(0, 64);

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
 * explosion) and its fire state (+0x8c: 0 idle, 1-4 firing). Null when the game has no raw inspect. */
async function factoryState(s: Session, handle: number): Promise<{ pos: { x: number; y: number; z: number }; exploded: boolean; hit: boolean; fireState: number } | null> {
  if (!(await has(s, "entity.inspect"))) return null;
  const r = await (await s.client()).call<{ hex?: string | null }>("entity.inspect", { handle, len: 0x90 }, 15000).catch(() => null);
  const hex = r?.hex;
  if (!hex || hex.length < 0x90 * 2) return null;
  const at = (off: number, n: number) => hex.slice(off * 2, (off + n) * 2);
  if (/\?/.test(at(0x2c, 12) + at(0x48, 12) + at(0x8c, 4))) return null;
  const bytes = new Uint8Array(0x90);
  for (let i = 0; i < 0x90; i++) bytes[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16) || 0;
  const v = new DataView(bytes.buffer);
  return { pos: { x: v.getFloat32(0x2c, true), y: v.getFloat32(0x30, true), z: v.getFloat32(0x34, true) }, exploded: bytes[0x48] !== 0, hit: bytes[0x50] !== 0, fireState: v.getInt32(0x8c, true) };
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
    description: "The running match as the game sees it: scene (menu, loading, match), the turn (index, team, worm, phase, ai), water level (world and map units), turn and round time left, sudden death, worms alive per team, and the sim tick. Needs the game running (after test).",
    inputSchema: { type: "object", additionalProperties: false, properties: {} },
    async handler(s) {
      return { ...withMap(await gameState(s)), camera: holdState() };
    },
  },
  {
    name: "game_wait",
    description: "Wait in the running game until the match reaches turn `turn` (turns started), or `seconds` pass, or the match ends; then answer game_state. Use it to watch a level script act over turns (a rising sea). A test match with an idle human team ends by itself after some minutes: keep waits short.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        turn: { type: "integer", minimum: 1, maximum: 1000, description: "Wait until at least this many turns have started." },
        seconds: { type: "number", minimum: 0.5, maximum: 600, description: "Wait at most this long (default 120)." },
      },
    },
    async handler(s, a: { turn?: number; seconds?: number }) {
      const limit = (a.seconds ?? 120) * 1000, t0 = Date.now();
      let st = await gameState(s);
      while (Date.now() - t0 < limit) {
        if (st.scene !== "match" && st.scene !== "loading") break;
        if (a.turn !== undefined && (st.turn ?? 0) >= a.turn) break;
        await new Promise((r) => setTimeout(r, Math.min(1000, Math.max(0, limit - (Date.now() - t0)))));
        st = await gameState(s);
      }
      const reached = a.turn === undefined ? undefined : (st.turn ?? 0) >= a.turn;
      return { waitedMs: Date.now() - t0, ...(reached !== undefined ? { reached } : {}),
        ...(st.scene !== "match" ? { note: `the game is at '${st.scene}', not in a match` } : {}), state: withMap(st) };
    },
  },
  {
    name: "game_camera",
    description: "Point the game's drawn camera from `pos` at `target` for `seconds` (at most 10; call again to hold it longer), or clear it. `preset`: default (the game's own camera: the override cleared) or overview (the open project's whole level from the south, 50 degrees down). A camera override changes only the picture on this machine: the game's own camera, aiming and the match are untouched. With Melange's render.camera it needs no mod (owner oasis); on an older game it is owned by a client mod (`mod`, default the first enabled mod with client code), and `mod` forces that. `shot: true` also answers the frame as a PNG (game_shots' clean, width and name apply). Needs a match running (after test).",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        pos: vec3("Eye [x, y, z]."), target: vec3("Point looked at [x, y, z]."), units,
        preset: { type: "string", enum: ["default", "overview"], description: "default: give the camera back to the game (its own turn camera); overview: the whole level (the open project's frames) from the south." },
        fov: { type: "number", minimum: 5, maximum: 150, description: "Vertical field of view in degrees (default the game's)." },
        seconds: { type: "number", minimum: 0.1, maximum: 10, description: "How long it holds (default 5; it ends by itself after that). See hold." },
        hold: { type: "boolean", description: "Hold the pose until released: erg-mcp sets it again every 4 s (a render.camera override lasts at most 10 s) until game_camera {clear: true} (or preset default, or a pose without hold), and game_shots put it back after their own poses. game_state answers whether a camera is held. It stops by itself when the game refuses it twice (the match ended)." },
        clear: { type: "boolean", description: "Give the camera back to the game (as preset default)." },
        mod: { type: "string", description: "Run the override in this enabled client mod's console instead of render.camera." },
        shot: { type: "boolean", description: "Also read the frame back (render.shot) and answer it as image content." },
        clean: { type: "boolean", description: "shot: hide the mods' wum.draw primitives (their HUD text) for the frames of the shot (default true)." },
        width: { type: "integer", minimum: 64, maximum: 8192, description: "shot: scale the picture down to this width (default the window's)." },
        name: { type: "string", description: "shot: the PNG's name in Documents\\Melange\\shots (default erg_shot)." },
      },
    },
    async handler(s, a: { pos?: number[]; target?: number[]; units?: "map" | "world"; preset?: "default" | "overview"; fov?: number; seconds?: number; clear?: boolean; mod?: string;
      shot?: boolean; clean?: boolean; width?: number; name?: string; hold?: boolean }) {
      const k = a.units === "world" ? 1 : WORLD_PER_MAP;
      const frames = a.hold ? 600 : Math.max(1, Math.min(600, Math.round((a.seconds ?? 5) * 60)));
      let out: Record<string, unknown>, frame: number | undefined;
      if (a.clear || a.preset === "default") {
        if (a.pos || a.target) throw new ToolError("clear and preset default take no pos or target");
        if (a.hold) throw new ToolError("hold holds a pose: give pos and target or preset overview");
        const was = stopHold();
        const c = await clearCamera(s, a.mod);
        out = { ...c, preset: "default", ...(was ? { released: { heldMs: Date.now() - was.since, refreshes: was.refreshes } } : {}), note: "the game's own camera draws again" };
      } else {
        const cam = a.preset === "overview" ? overview(s) : (() => {
          if (!a.pos || !a.target) throw new ToolError("give pos and target, a preset, or clear: true");
          if (![...a.pos, ...a.target].every(Number.isFinite)) throw new ToolError("pos and target are finite numbers");
          return { pos: a.pos.map((x) => x * k), target: a.target.map((x) => x * k), fov: a.fov };
        })();
        if (a.preset === "overview" && (a.pos || a.target)) throw new ToolError("preset overview takes no pos or target");
        stopHold();
        const pose = { ...cam, fov: a.fov ?? cam.fov };
        const r = await setCamera(s, pose, frames, a.mod);
        frame = r.frame;
        if (a.hold) startHold(s, pose, a.mod);
        out = { set: true, owner: r.owner, ...(a.mod ? { mod: a.mod } : {}), frames, ...(a.preset ? { preset: a.preset } : {}), worldPos: cam.pos, worldTarget: cam.target,
          ...(a.hold ? { held: true, note: "held until game_camera {clear: true} (set again every 4 s)" } : { note: "it ends by itself after the frames; game_camera {clear: true} ends it now; hold: true keeps it" }) };
      }
      if (!a.shot) return out;
      const p = await shot(s, a.name ?? "erg_shot", { width: a.width, clean: a.clean ?? true, minFrame: frame !== undefined ? frame + 2 : undefined });
      const parts: Content[] = [image(p.png), { type: "text", text: JSON.stringify({ ...out, shot: { path: p.path, width: p.width, height: p.height, frame: p.frame, clean: p.clean } }, null, 1) }];
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
          preset: { type: "string", enum: ["default", "overview"] }, name: { type: "string", description: "The PNG's name (default <prefix>_<n>)." } } } },
        units,
        prefix: { type: "string", description: "Names for shots without one: <prefix>_<n> (default erg_shots)." },
        clean: { type: "boolean", description: "Hide the mods' wum.draw primitives in the shots (default true)." },
        width: { type: "integer", minimum: 64, maximum: 8192, description: "Scale every picture down to this width (default 960)." },
        settleFrames: { type: "integer", minimum: 1, maximum: 120, description: "Frames drawn from a pose before it is read (default 2; more lets particles and the water move on)." },
        hold: { type: "boolean", description: "Keep the last pose for 5 s instead of giving the camera back." },
        holdLast: { type: "boolean", description: "Keep the last pose until released (game_camera {clear: true}), as game_camera's hold." },
        images: { type: "boolean", description: "Answer the pictures as image content (default true); false: paths only." },
      },
    },
    async handler(s, a: { cams: Cam[]; units?: "map" | "world"; prefix?: string; clean?: boolean; width?: number; settleFrames?: number; hold?: boolean; holdLast?: boolean; images?: boolean }) {
      if (!(await has(s, "render.camera")) || !(await has(s, "render.shot"))) throw new ToolError("game_shots needs the game's render.camera and render.shot (a Melange with them, running)");
      const k = a.units === "world" ? 1 : WORLD_PER_MAP, t0 = Date.now(), settle = a.settleFrames ?? 2;
      const shots: object[] = [], parts: Content[] = [];
      // a held pose pauses for the shots and comes back after them (unless they hold their own last pose)
      const paused = stopHold();
      let lastPose: Held["cam"] | null = null;
      try {
        for (const [i, c] of a.cams.entries()) {
          const name = c.name ?? shotName(a.prefix ?? "erg_shots", i + 1);
          let frame: number | undefined;
          if (c.preset === "default") {
            if (c.pos || c.target) throw new ToolError(`cams[${i}]: preset default takes no pos or target`);
            await clearCamera(s);
          } else {
            const cam = c.preset === "overview" ? overview(s) : (() => {
              if (!c.pos || !c.target) throw new ToolError(`cams[${i}]: give pos and target, or a preset`);
              return { pos: c.pos.map((x) => x * k), target: c.target.map((x) => x * k), fov: c.fov };
            })();
            const last = (a.hold || a.holdLast) && i === a.cams.length - 1;
            lastPose = { ...cam, fov: c.fov ?? cam.fov };
            frame = (await setCamera(s, lastPose, last ? (a.holdLast ? 600 : 300) : Math.max(30, settle + 20))).frame;
          }
          const p = await shot(s, name, { width: a.width ?? 960, clean: a.clean ?? true, minFrame: frame !== undefined ? frame + settle : undefined });
          shots.push({ name, ...(c.preset ? { preset: c.preset } : {}), path: p.path, width: p.width, height: p.height, frame: p.frame, clean: p.clean });
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
      parts.push({ type: "text", text: JSON.stringify({ shots, ms: Date.now() - t0, camera }, null, 1) });
      return { content: parts };
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
        alive: { type: "boolean", description: "Also answer `factoryEntity`: whether the game's MineFactoryLogicEntity exists in the match now, where it is, and how far from the project's knot (a factory that fell elsewhere or exploded at load shows here)." },
      },
    },
    async handler(s, a: { watchSeconds?: number; untilFire?: boolean; units?: "map" | "world"; alive?: boolean }) {
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
      let firedAt: number | null = null;
      const events: { name: string; ms: number; d?: unknown }[] = [];
      const counts: Record<string, number> = {};
      let unsub: (() => void) | undefined, names: string[] = [];
      if (watch > 0) {
        names = await mineMessages(s);
        if (names.length) unsub = (await s.client()).subscribe<{ name: string; d?: unknown }>("bus", { names, path: "post", decode: true }, (m) => {
          counts[m.name] = (counts[m.name] ?? 0) + 1;
          if (m.name === "MineFactory.FireEnd" && firedAt === null) firedAt = Date.now();
          if (events.length < 200) events.push({ name: m.name, ms: Date.now() - t0, ...(m.d !== undefined ? { d: m.d } : {}) });
        });
      }
      if (a.untilFire && !names.includes("MineFactory.FireEnd")) throw new ToolError("untilFire needs the bus's MineFactory.FireEnd, which this game does not list (bus.names)");
      try {
        const appeared: { handle: number; type: string; ms: number; at: number[] | null; fromFactory?: number }[] = [];
        while (Date.now() - t0 < watch && !(a.untilFire && firedAt !== null && Date.now() - firedAt > 3000)) {
          await sleep(250);
          for (const m of await liveMines(s).catch(() => [] as Entity[]))
            if (!seen.has(m.handle)) { seen.set(m.handle, m); appeared.push({ handle: m.handle, type: m.type, ms: Date.now() - t0, at: out(m.pos), fromFactory: dist(m.pos) }); }
        }
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
                ...(st ? { at: out(st.pos), fromKnot: off(st.pos), exploded: st.exploded, hitByExplosion: st.hit, fireState: st.fireState } : { note: "the game has no entity.inspect (or [Oasis] RawInspect=0): where it settled is not known" }),
                ...(far ? { warning: "the factory settled far from its knot: its drop from the knot hit no land the sweep sees (a draped skin) and it took a stale point" } : {}) }
            : { alive: false, why: factory ? FACTORY_GONE : "no factory entity in the match (and no project with a minefactory knot is open)" };
        }
        return {
          units: a.units ?? "map", factory: factory ? { at: out({ x: factory[0], y: factory[1], z: factory[2] }), note: "the open project's minefactory knot" } : null,
          ...(a.untilFire ? { fired: firedAt !== null, ...(firedAt !== null ? { fireEndMs: firedAt - t0 } : {}) } : {}),
          ...(factoryEntity ? { factoryEntity } : {}),
          minesBefore: before.length, mines: now.map((m) => ({ handle: m.handle, type: m.type, at: out(m.pos), fromFactory: dist(m.pos) })),
          ...(watch > 0 ? { watchedMs: Date.now() - t0, appeared, messages: { watched: names, counts, events } } : {}),
        };
      } finally {
        unsub?.();
      }
    },
  },
];
