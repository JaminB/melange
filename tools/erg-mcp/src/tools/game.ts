// Tool group "game": looking at a Test while it plays, through the running game's own Oasis methods. game_state reads
// the match (turn, worms, water), game_wait waits for a turn or a time, game_camera points the drawn camera at a place
// (wum.render.cameraOverride: the picture only, the game's camera and the match are untouched). They run Lua in the
// game's client console (lua.eval, target client), so they need the game running with Oasis writable, offline (the
// console refuses online matches unless [LuaConsole] MatchConsoleOnline=1). A game with render.camera and render.shot
// (Melange's own Oasis methods) points the camera with no client mod, and game_camera {shot} and game_shots read the
// picture back (clean: without the mods' wum.draw text).
import { readFileSync } from "node:fs";
import { extentXZ } from "../lib/inspect-field";
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

const withMap = (st: GameState) => ({ ...st, ...(st.water !== undefined ? { waterMap: Math.round((st.water / WORLD_PER_MAP) * 100) / 100 } : {}) });

const units = { type: "string", enum: ["map", "world"], description: "Units of pos and target: map (default, as the other tools) or world (map x 20, the game's)." } as const;
const vec3 = (d: string) => ({ type: "array", items: { type: "number" }, minItems: 3, maxItems: 3, description: d }) as const;

export const tools: Tool[] = [
  {
    name: "game_state",
    description: "The running match as the game sees it: scene (menu, loading, match), the turn (index, team, worm, phase, ai), water level (world and map units), turn and round time left, sudden death, worms alive per team, and the sim tick. Needs the game running (after test).",
    inputSchema: { type: "object", additionalProperties: false, properties: {} },
    async handler(s) {
      return withMap(await gameState(s));
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
        seconds: { type: "number", minimum: 0.1, maximum: 10, description: "How long it holds (default 5; it ends by itself after that)." },
        clear: { type: "boolean", description: "Give the camera back to the game (as preset default)." },
        mod: { type: "string", description: "Run the override in this enabled client mod's console instead of render.camera." },
        shot: { type: "boolean", description: "Also read the frame back (render.shot) and answer it as image content." },
        clean: { type: "boolean", description: "shot: hide the mods' wum.draw primitives (their HUD text) for the frames of the shot (default true)." },
        width: { type: "integer", minimum: 64, maximum: 8192, description: "shot: scale the picture down to this width (default the window's)." },
        name: { type: "string", description: "shot: the PNG's name in Documents\\Melange\\shots (default erg_shot)." },
      },
    },
    async handler(s, a: { pos?: number[]; target?: number[]; units?: "map" | "world"; preset?: "default" | "overview"; fov?: number; seconds?: number; clear?: boolean; mod?: string;
      shot?: boolean; clean?: boolean; width?: number; name?: string }) {
      const k = a.units === "world" ? 1 : WORLD_PER_MAP;
      const frames = Math.max(1, Math.min(600, Math.round((a.seconds ?? 5) * 60)));
      let out: Record<string, unknown>, frame: number | undefined;
      if (a.clear || a.preset === "default") {
        if (a.pos || a.target) throw new ToolError("clear and preset default take no pos or target");
        const c = await clearCamera(s, a.mod);
        out = { ...c, preset: "default", note: "the game's own camera draws again" };
      } else {
        const cam = a.preset === "overview" ? overview(s) : (() => {
          if (!a.pos || !a.target) throw new ToolError("give pos and target, a preset, or clear: true");
          if (![...a.pos, ...a.target].every(Number.isFinite)) throw new ToolError("pos and target are finite numbers");
          return { pos: a.pos.map((x) => x * k), target: a.target.map((x) => x * k), fov: a.fov };
        })();
        if (a.preset === "overview" && (a.pos || a.target)) throw new ToolError("preset overview takes no pos or target");
        const r = await setCamera(s, { ...cam, fov: a.fov ?? cam.fov }, frames, a.mod);
        frame = r.frame;
        out = { set: true, owner: r.owner, ...(a.mod ? { mod: a.mod } : {}), frames, ...(a.preset ? { preset: a.preset } : {}), worldPos: cam.pos, worldTarget: cam.target,
          note: "it ends by itself after the frames; game_camera {clear: true} ends it now" };
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
        images: { type: "boolean", description: "Answer the pictures as image content (default true); false: paths only." },
      },
    },
    async handler(s, a: { cams: Cam[]; units?: "map" | "world"; prefix?: string; clean?: boolean; width?: number; settleFrames?: number; hold?: boolean; images?: boolean }) {
      if (!(await has(s, "render.camera")) || !(await has(s, "render.shot"))) throw new ToolError("game_shots needs the game's render.camera and render.shot (a Melange with them, running)");
      const k = a.units === "world" ? 1 : WORLD_PER_MAP, t0 = Date.now(), settle = a.settleFrames ?? 2;
      const shots: object[] = [], parts: Content[] = [];
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
            const last = a.hold && i === a.cams.length - 1;
            frame = (await setCamera(s, { ...cam, fov: c.fov ?? cam.fov }, last ? 300 : Math.max(30, settle + 20))).frame;
          }
          const p = await shot(s, name, { width: a.width ?? 960, clean: a.clean ?? true, minFrame: frame !== undefined ? frame + settle : undefined });
          shots.push({ name, ...(c.preset ? { preset: c.preset } : {}), path: p.path, width: p.width, height: p.height, frame: p.frame, clean: p.clean });
          if (a.images !== false) parts.push(image(p.png));
        }
      } finally {
        if (!a.hold) await clearCamera(s).catch(() => undefined);
      }
      parts.push({ type: "text", text: JSON.stringify({ shots, ms: Date.now() - t0, camera: a.hold ? "the last pose holds for 5 s" : "given back to the game" }, null, 1) });
      return { content: parts };
    },
  },
];
