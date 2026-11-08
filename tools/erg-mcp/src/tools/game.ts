// Tool group "game": looking at a Test while it plays, through the running game's own Oasis methods. game_state reads
// the match (turn, worms, water), game_wait waits for a turn or a time, game_camera points the drawn camera at a place
// (wum.render.cameraOverride: the picture only, the game's camera and the match are untouched). They run Lua in the
// game's client console (lua.eval, target client), so they need the game running with Oasis writable, offline (the
// console refuses online matches unless [LuaConsole] MatchConsoleOnline=1).
import { ToolError, type Tool } from "../registry";
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
    description: "Point the game's drawn camera from `pos` at `target` for `seconds` (at most 10; call again to hold it longer), or clear it. A camera override changes only the picture on this machine: the game's own camera, aiming and the match are untouched. It is owned by a client mod (`mod`, default the first enabled mod with client code; it runs in that mod's console). Needs a match running (after test).",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        pos: vec3("Eye [x, y, z]."), target: vec3("Point looked at [x, y, z]."), units,
        fov: { type: "number", minimum: 5, maximum: 150, description: "Vertical field of view in degrees (default the game's)." },
        seconds: { type: "number", minimum: 0.1, maximum: 10, description: "How long it holds (default 5; it ends by itself after that)." },
        clear: { type: "boolean", description: "Give the camera back to the game." },
        mod: { type: "string", description: "The enabled client mod that owns the override (default the first one)." },
      },
    },
    async handler(s, a: { pos?: number[]; target?: number[]; units?: "map" | "world"; fov?: number; seconds?: number; clear?: boolean; mod?: string }) {
      const mod = await cameraMod(s, a.mod);
      if (a.clear) return { cleared: (await evalClient(s, "return tostring(wum.render.cameraOverride(nil))", mod)) === "true", mod };
      if (!a.pos || !a.target) throw new ToolError("give pos and target, or clear: true");
      const k = a.units === "world" ? 1 : WORLD_PER_MAP;
      const v = (p: number[]) => {
        if (!p.every(Number.isFinite)) throw new ToolError("pos and target are finite numbers");
        return `{${p.map((x) => (x * k).toFixed(3)).join(", ")}}`;
      };
      const frames = Math.max(1, Math.min(600, Math.round((a.seconds ?? 5) * 60)));
      const code = `local ok, why = wum.render.cameraOverride{pos = ${v(a.pos)}, target = ${v(a.target)}${a.fov ? `, fov = ${a.fov}` : ""}, frames = ${frames}}
return tostring(ok) .. ";" .. tostring(why)`;
      const [ok, why] = (await evalClient(s, code, mod)).split(";");
      if (ok !== "true") throw new ToolError(`the camera override was refused: ${why}${why === "unavailable" ? " (no match is running)" : ""}`);
      return { set: true, mod, frames, worldPos: a.pos.map((x) => x * k), worldTarget: a.target.map((x) => x * k), note: "it ends by itself after the frames; game_camera {clear: true} ends it now" };
    },
  },
];
