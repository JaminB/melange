// Tool group "harness": test moves in a running Test that the game's own inputs cannot make from a script, each through
// a Test-only hook of this branch's Melange (level.test.lua: engine Lua in the match VM at a sim tick; level.test.placeWorm:
// the worm anchor's own write; level.test.landRay: the game's land sweep). A team's inventory is given, a worm is parked
// on a step, a roof or a mesa without walking it there, an air strike falls on a named point of the level, the ground is
// read through the game's own collision along a walk, and turns are ended until something is due. Dustcrown's rounds 1
// and 2 could not test a jetpack flight, the palace's steps under Sandtide's line or a strike on the keep's roof: the
// drive had no utility fire (Input.FireUtil is the jetpack's thrust and is in game_drive now) and a worm had to walk to
// wherever it was needed. Positions are map units unless `units` says world.
import { spansAt } from "../lib/inspect-field";
import { grids } from "../lib/voxel";
import { ToolError, type Tool } from "../registry";
import type { Session } from "../session";

const W = 20;   // world units per map unit
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
const r2 = (v: number) => Math.round(v * 100) / 100;

async function need(s: Session, method: string, what: string) {
  if (!(await s.ensure()).methods.includes(method)) throw new ToolError(`this game has no ${method} (${what}): Test with this branch's Melange, game running`);
}
async function evalLua(s: Session, target: "client" | "match", code: string): Promise<string> {
  const r = await (await s.client()).call<{ ok: boolean; text: string }>("lua.eval", { target, code }, 15000);
  if (!r.ok) throw new ToolError(`the game's ${target} console refused: ${r.text}`);
  return r.text ?? "";
}
interface LuaRuns { seq: number; pending: number; runs: { tick: number; ok: boolean; code: string; text: string }[] }
/** Engine Lua at the next sim tick (level.test.lua), waited for: its text, or a ToolError with the engine's message. */
export async function tickLua(s: Session, code: string): Promise<string> {
  await need(s, "level.test.lua", "Melange 0.15+");
  const c = await s.client();
  const before = (await c.call<LuaRuns>("level.test.lua", {}, 15000)).seq;
  await c.call("level.test.lua", { code }, 15000);
  for (let i = 0; i < 40; i++) {
    await sleep(100);
    const q = await c.call<LuaRuns>("level.test.lua", {}, 15000);
    if (q.seq > before && !q.pending) {
      const run = q.runs[q.runs.length - 1];
      if (!run.ok) throw new ToolError(`the match VM refused: ${run.text}`);
      return run.text;
    }
  }
  throw new ToolError("the code did not run within 4 s (is the Test playing? the sim ticks only in a match)");
}

const WORMS_LUA = `local o = {}
for _, w in ipairs(wum.game.worms() or {}) do o[#o + 1] = string.format("%s,%s,%.3f,%.3f,%.3f,%s,%s", tostring(w.slot), tostring(w.team), w.pos.x, w.pos.y, w.pos.z, tostring(w.alive), tostring(w.health)) end
local t = wum.game.inMatch() and wum.game.turn() or nil
local m = wum.game.match and wum.game.match() or nil
return table.concat(o, ";") .. "|" .. (t and (tostring(t.index) .. "," .. tostring(t.worm) .. "," .. tostring(t.team) .. "," .. tostring(t.phase)) or "") .. "|" .. (m and (tostring(m.turnMsLeft) .. "," .. tostring(m.roundMsLeft) .. "," .. tostring(m.suddenDeath) .. "," .. tostring(m.waterLevel)) or "")`;
interface Now { worms: { slot: number; team: number; pos: number[]; alive: boolean; health: number }[]; turn?: { index: number; worm: number; team: number; phase: string }; turnMsLeft?: number; roundMsLeft?: number; suddenDeath?: boolean; water?: number }
async function now(s: Session): Promise<Now> {
  const [ws, t, m] = (await evalLua(s, "client", WORMS_LUA)).split("|");
  const worms = ws ? ws.split(";").map((x) => x.split(",")).map((v) => ({ slot: +v[0], team: +v[1], pos: [+v[2], +v[3], +v[4]], alive: v[5] === "true", health: +v[6] })) : [];
  const tv = t ? t.split(",") : null, mv = m ? m.split(",") : null;
  return { worms, ...(tv ? { turn: { index: +tv[0], worm: +tv[1], team: +tv[2], phase: tv[3] } } : {}),
    ...(mv ? { turnMsLeft: +mv[0], roundMsLeft: +mv[1], suddenDeath: mv[2] === "true", water: +mv[3] } : {}) };
}

interface Ray { hit: boolean; at?: number[]; error?: string }
/** The highest a ray starts: the sweep answers nothing for a ray from far over the land's box (8000 world units missed
 * everything on Dustcrown, 4000 hit), and a ray that starts inside solid hits at its start. Map units. */
const SKY = 150, DEEP = -20;
/** The game's land sweep straight down at each [x, z] (map units), from y0 to y1: the hit's height (map), or null. */
async function groundRays(s: Session, pts: number[][], y0: number, y1: number): Promise<(number | null)[]> {
  const c = await s.client(), out: (number | null)[] = [];
  for (let i = 0; i < pts.length; i += 64) {
    const b = pts.slice(i, i + 64);
    const r = await c.call<{ rays: Ray[] }>("level.test.landRay", { segments: b.map(([x, z]) => [x * W, y0 * W, z * W, x * W, y1 * W, z * W]) }, 30000);
    for (const ray of r.rays) {
      if (ray.error) throw new ToolError(`level.test.landRay: ${ray.error}`);
      out.push(ray.hit && ray.at ? ray.at[1] / W : null);
    }
  }
  return out;
}

/** The inventory fields a kit gives (the engine's inventory container field names). */
const KITS: Record<string, Record<string, number>> = {
  movement: { Jetpack: 9, NinjaRope: 9, Girder: 9, Parachute: 9, Teleport: 9 },
  strikes: { Airstrike: 9, Bazooka: -1, Grenade: -1 },
};

export const tools: Tool[] = [
  {
    name: "game_give_weapons",
    description: "Give the teams of the running Test weapons and utilities: `weapons` {<inventory field>: count} (the engine's field names: Jetpack, NinjaRope, Girder, Teleport, Parachute, Airstrike, Bazooka, Grenade, ClusterGrenade, Dynamite, Landmine, Sheep, HomingMissile, Shotgun, BaseballBat, ...; -1 is unlimited) and/or `kit` (movement: jetpack, rope, girder, parachute and teleport, 9 each; strikes: 9 air strikes and unlimited bazooka and grenade), for `teams` (0-3, default all four). The counts are written into Inventory.Alliance0N and Inventory.Team0N in the match VM at a sim tick (a Versus match plays from the alliance inventories; the Quick Game's scheme gives two jetpacks and ropes, one girder and no teleport). A field the game does not have is refused with the engine's message. Answers the counts read back. A Test only (level.test.lua, Melange 0.15+).",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      weapons: { type: "object", description: "{<field>: count}, counts -1 (unlimited) to 99." },
      kit: { type: "string", enum: Object.keys(KITS) },
      teams: { type: "array", maxItems: 4, items: { type: "integer", minimum: 0, maximum: 3 } },
    } },
    async handler(s: Session, a: { weapons?: Record<string, number>; kit?: string; teams?: number[] }) {
      const give = { ...(a.kit ? KITS[a.kit] : {}), ...(a.weapons ?? {}) };
      const names = Object.keys(give);
      if (!names.length) throw new ToolError("give weapons or kit");
      for (const n of names) {
        if (!/^[A-Za-z][A-Za-z0-9]{0,31}$/.test(n)) throw new ToolError(`weapons: '${n}' is not an inventory field name`);
        if (!Number.isInteger(give[n]) || give[n] < -1 || give[n] > 99) throw new ToolError(`weapons.${n}: a count from -1 (unlimited) to 99`);
      }
      const teams = a.teams?.length ? a.teams : [0, 1, 2, 3];
      const set = names.map((n) => `inv.${n} = ${give[n]}`).join("; ");
      await tickLua(s, `for _, t in {${teams.join(", ")}} do for _, n in {"Inventory.Alliance0" .. t, "Inventory.Team0" .. t} do local lock, inv = EditContainer(n); ${set}; CloseContainer(lock) end end return "given"`);
      const read = await evalLua(s, "match", `local q = QueryContainer("Inventory.Alliance0${teams[0]}"); return ${names.map((n) => `"${n}=" .. tostring(q.${n})`).join(' .. "," .. ')}`);
      return { given: give, teams, readBack: Object.fromEntries(read.split(",").map((x) => x.split("=")).map(([k, v]) => [k, +v])), inventory: `Inventory.Alliance0${teams[0]}`,
        note: "the active worm takes one with game_select_weapon (kUtilityJetpack, kUtilityNinjaRope, kUtilityGirder, kWeaponAirstrike); a jetpack flies with game_drive {inputs: [\"FireUtil\"]} (thrust) and W, A, S, D" };
    },
  },
  {
    name: "game_park_worm",
    description: "Put a worm of the running Test where a check needs it, without walking it there: `worm` (its slot, 0-15) stands at `at` ([x, z]: on the highest ground there; [x, y, z]: on the ground at or under y + 1, to pick a floor under a roof), read with the game's own land sweep (level.test.landRay), and its position is written at a sim tick with its velocity zeroed (level.test.placeWorm, the worm anchor's own write). A resting worm does not fall by itself, so the tool always lands it on the ground it finds (refused over water or where the sweep hits nothing, unless `exact: true` writes at just as given). `anchor: true` also anchors it (it stays through blasts and cannot walk; `release: true` with only `worm` lets it go). Answers where it was, the ground found and where the game reads it afterwards. For Sandtide's line: park a worm on each surface, then game_end_turns {untilElapsedSec: 185} and read game_probe's deaths. A Test only (Melange 0.15+).",
    inputSchema: { type: "object", additionalProperties: false, required: ["worm"], properties: {
      worm: { type: "integer", minimum: 0, maximum: 15 },
      at: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 3 },
      anchor: { type: "boolean" }, release: { type: "boolean" }, exact: { type: "boolean" },
      units: { type: "string", enum: ["map", "world"] },
    } },
    async handler(s: Session, a: { worm: number; at?: number[]; anchor?: boolean; release?: boolean; exact?: boolean; units?: "map" | "world" }) {
      await need(s, "level.test.placeWorm", "Melange 0.15+");
      const c = await s.client(), k = a.units === "world" ? 1 / W : 1;
      const before = (await now(s)).worms.find((w) => w.slot === a.worm);
      if (!before) throw new ToolError(`no worm in slot ${a.worm}`);
      if (!before.alive) throw new ToolError(`WORM${a.worm} is dead`);
      const last = async () => (await c.call<{ last: string }>("level.test.placeWorm", {}, 15000)).last;
      if (a.release) {
        if (a.at) throw new ToolError("release lets the anchor go: give it with worm only");
        await c.call("level.test.placeWorm", { worm: a.worm, release: true }, 15000);
        await sleep(400);
        return { worm: a.worm, released: true, game: await last() };
      }
      if (!a.at) throw new ToolError("at: [x, z] or [x, y, z]");
      const x = a.at[0] * k, z = a.at[a.at.length - 1] * k, yAsk = a.at.length === 3 ? a.at[1] * k : null;
      let y = yAsk ?? 0, ground: number | null = null;
      if (!a.exact) {
        [ground] = await groundRays(s, [[x, z]], yAsk === null ? SKY : yAsk + 1, DEEP);
        if (ground === null) throw new ToolError(`the game's land sweep finds no ground at ${r2(x)}, ${r2(z)}${yAsk === null ? "" : ` at or under y ${r2(yAsk + 1)}`}: a worm parked there would hang in the air (exact: true writes it anyway)`);
        const st = await now(s);
        if (st.water !== undefined && ground * W <= st.water) throw new ToolError(`the ground at ${r2(x)}, ${r2(z)} is at ${r2(ground)}, under the water (${r2(st.water / W)} map): the worm would drown`);
        y = ground + 0.02;
      } else if (yAsk === null) throw new ToolError("exact needs [x, y, z]");
      await c.call("level.test.placeWorm", { worm: a.worm, pos: [x * W, y * W, z * W], anchor: !!a.anchor }, 15000);
      await sleep(500);
      const game = await last(), after = (await now(s)).worms.find((w) => w.slot === a.worm);
      const out = (p: number[]) => p.map((v) => r2(v / W / k));
      const moved = after ? Math.hypot(after.pos[0] / W - x, after.pos[2] / W - z) < 0.5 && Math.abs(after.pos[1] / W - y) < 1.5 : false;
      if (!moved) throw new ToolError(`the worm is not there: ${game}; the game reads it at ${after ? out(after.pos).join(", ") : "?"}`);
      return { worm: a.worm, from: out(before.pos), ...(ground !== null ? { ground: r2(ground / k) } : {}), at: out(after!.pos), anchored: !!a.anchor, game, units: a.units ?? "map",
        note: "placed at a sim tick by writing the worm's data container (the worm anchor's write): take a shot to see it (game_shots)" };
    },
  },
  {
    name: "game_air_strike",
    description: "An air strike on a named point of the level in the running Test, whoever's turn it is: the engine's own GameLogic.CreateAirstrike (the missions' scripted strike: Airstrike.TargetEntity is a detail of the level) at a sim tick, so its bombs fall and explode as the weapon's do, on `detail` (a detail's name: a spawn knot WORMn, a blast point of place_blast_points, any named detail) or on the named detail nearest `at` ([x, z] or [x, y, z]; refused when none lies within `maxSnap`, default 3 map units: put a blast point there with place_blast_points before the Test, on the keep's roof and on each plinth). `direction` is the line the bombs fall along, degrees from +z towards +x (default 0: the row runs along z). `weapon` is the weapon properties name (default kWeaponAirstrike). It waits `seconds` (default 12) and answers the explosions the Test probe saw since, each with the game's ground read under it afterwards (the land sweep 3 x 3 round it: `noGround` counts rays that hit nothing, a hole through the level). Round 2's strikes could only fall where the firing worm stood. A Test only (Melange 0.15+).",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      detail: { type: "string" },
      at: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 3 },
      maxSnap: { type: "number", minimum: 0 },
      direction: { type: "number" },
      weapon: { type: "string" },
      seconds: { type: "number", minimum: 2, maximum: 60 },
      units: { type: "string", enum: ["map", "world"] },
    } },
    async handler(s: Session, a: { detail?: string; at?: number[]; maxSnap?: number; direction?: number; weapon?: string; seconds?: number; units?: "map" | "world" }) {
      const { store } = s.requireProject();
      const k = a.units === "world" ? 1 / W : 1;
      if ((a.detail === undefined) === (a.at === undefined)) throw new ToolError("give detail or at");
      const named = store.scene.details.filter((d) => d.name && /^[A-Za-z0-9_.\- ]{1,63}$/.test(d.name));
      let target = a.detail !== undefined ? named.find((d) => d.name === a.detail) : undefined, snap: number | undefined;
      if (a.detail !== undefined && !target) throw new ToolError(`no detail named '${a.detail}' in the open project (list_objects, place_blast_points)`);
      if (a.at) {
        const x = a.at[0] * k, z = a.at[a.at.length - 1] * k, y = a.at.length === 3 ? a.at[1] * k : null;
        let best = Infinity;
        for (const d of named) {
          const p = store.frames.detailWorld(d);   // map units
          const dist = Math.hypot(p[0] - x, p[2] - z, y === null ? 0 : p[1] - y);
          if (dist < best) { best = dist; target = d; }
        }
        const max = (a.maxSnap ?? 3 / k) * k;
        if (!target || best > max) throw new ToolError(`no named detail within ${r2(max / k)} of the target (the nearest${target ? `, ${target.name}, is ${r2(best / k)} away` : ": none"}): a scripted strike falls on a detail, so put a blast point there first (place_blast_points) and Test again`);
        snap = best;
      }
      if (/["\\\n]/.test(target!.name) || (a.weapon !== undefined && !/^[A-Za-z0-9_]{1,40}$/.test(a.weapon))) throw new ToolError("detail or weapon holds a character the call cannot pass");
      const pr = async () => (await (await s.client()).call<{ probe: { explosionsSeen: number; explosions: { turn: number; source: string; at: number[]; radius?: number }[] } }>("level.test.probe", {}, 15000)).probe;
      const seen0 = (await pr()).explosionsSeen;
      const rad = (((a.direction ?? 0) * Math.PI) / 180).toFixed(4);
      await tickLua(s, `SetData("Airstrike.Direction.Radians", ${rad}); SetData("Airstrike.TargetEntity", "${target!.name}"); SetData("Airstrike.WeaponProperties", "${a.weapon ?? "kWeaponAirstrike"}"); SendMessage("GameLogic.CreateAirstrike"); return "sent"`);
      await sleep((a.seconds ?? 12) * 1000);
      const p = await pr(), fresh = p.explosions.slice(-Math.min(p.explosions.length, Math.max(0, p.explosionsSeen - seen0)));
      const at = store.frames.detailWorld(target!);
      const blasts = [];
      for (const e of fresh) {
        const ex = e.at[0], ez = e.at[2];   // the probe answers map units
        const pts = [-1, 0, 1].flatMap((dx) => [-1, 0, 1].map((dz) => [ex + dx, ez + dz]));
        const hits = await groundRays(s, pts, SKY, DEEP), ok = hits.filter((h): h is number => h !== null);
        blasts.push({ at: [r2(ex / k), r2(e.at[1] / k), r2(ez / k)], ground: ok.length ? { min: r2(Math.min(...ok) / k), max: r2(Math.max(...ok) / k) } : null, noGround: hits.length - ok.length });
      }
      return { target: { detail: target!.name, at: at.map((v) => r2(v / k)), ...(snap !== undefined ? { snappedBy: r2(snap / k) } : {}) }, direction: a.direction ?? 0,
        explosions: blasts.length, blasts, holes: blasts.filter((b) => b.noGround > 0).length, units: a.units ?? "map",
        ...(blasts.length ? {} : { warning: "the probe saw no explosion: the strike may still be falling (raise seconds), or the match was between turns" }),
        note: "noGround: rays of the 3 x 3 round a blast that hit no ground afterwards (a hole through the level; compare with game_ground_scan before the strike)" };
    },
  },
  {
    name: "game_ground_scan",
    description: "The ground of the running Test as the GAME reads it, against the tools' own reading of the open project: the game's land sweep (level.test.landRay, the collision a worm and a mine meet) straight down every `step` (default 0.25 map units, finer than any skin's voxels) along a walk (`along`: waypoints [x, z] or [x, y, z], a corridor `width` wide, default 2) or over a region (x0, z0, x1, z1), and at each ray the tools' top of the ground there (the voxels, skins at their sheared height). Answers `misses` (rays that hit nothing where the tools read ground: a seam or hole in the game's collision that the voxels do not show, the kind a worm fell through at Dustcrown's (-0.67, 77.97)), `low` (the game's ground more than `tolerance`, default 1 map unit, under the tools': the ray went through the top surface and landed on what is under it) and `high`, each grouped with its place and, for a walk, `alongAt`. Scan at the start of a match, before anything explodes: a blast's hole is in the game and not in the project. At most 40 000 rays.",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      along: { type: "array", minItems: 2, maxItems: 4000, items: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 3 } },
      width: { type: "number", minimum: 0.1, maximum: 40 },
      x0: { type: "number" }, z0: { type: "number" }, x1: { type: "number" }, z1: { type: "number" },
      step: { type: "number", minimum: 0.05, maximum: 8 },
      tolerance: { type: "number", minimum: 0 },
      limit: { type: "integer", minimum: 1, maximum: 200 },
      units: { type: "string", enum: ["map", "world"] },
    } },
    async handler(s: Session, a: { along?: number[][]; width?: number; x0?: number; z0?: number; x1?: number; z1?: number; step?: number; tolerance?: number; limit?: number; units?: "map" | "world" }) {
      await need(s, "level.test.landRay", "Melange 0.13+");
      const { store } = s.requireProject();
      const k = a.units === "world" ? 1 / W : 1, step = (a.step ?? 0.25 / k) * k, tol = (a.tolerance ?? 1 / k) * k, gs = grids(store);
      const path = a.along?.map((q) => [q[0] * k, q[q.length - 1] * k] as [number, number]);
      const region = [a.x0, a.z0, a.x1, a.z1].every((v) => v !== undefined);
      if (!!path === region) throw new ToolError("give along, or the region x0, z0, x1, z1");
      const half = ((a.width ?? 2 / k) * k) / 2;
      const near = (x: number, z: number) => {
        let best = { d: Infinity, along: 0 }, run = 0;
        for (let i = 1; i < path!.length; i++) {
          const [ax, az] = path![i - 1], [bx, bz] = path![i], ex = bx - ax, ez = bz - az, len = Math.hypot(ex, ez);
          const t = len ? Math.max(0, Math.min(1, ((x - ax) * ex + (z - az) * ez) / (len * len))) : 0;
          const d = Math.hypot(x - (ax + ex * t), z - (az + ez * t));
          if (d < best.d) best = { d, along: run + len * t };
          run += len;
        }
        return best;
      };
      const box = path ? [Math.min(...path.map((p) => p[0])) - half, Math.min(...path.map((p) => p[1])) - half, Math.max(...path.map((p) => p[0])) + half, Math.max(...path.map((p) => p[1])) + half]
        : [Math.min(a.x0!, a.x1!) * k, Math.min(a.z0!, a.z1!) * k, Math.max(a.x0!, a.x1!) * k, Math.max(a.z0!, a.z1!) * k];
      const pts: number[][] = [];
      for (let z = box[1] + step / 2; z < box[3]; z += step) for (let x = box[0] + step / 2; x < box[2]; x += step) if (!path || near(x, z).d <= half) pts.push([x, z]);
      if (!pts.length) throw new ToolError("no ray in the area");
      if (pts.length > 40000) throw new ToolError(`${pts.length} rays is too many (at most 40000): a larger step or a smaller area`);
      const st = await now(s), water = st.water !== undefined ? st.water / W : -Infinity;
      const game = await groundRays(s, pts, SKY, DEEP);
      type Bad = { kind: "miss" | "low" | "high"; x: number; z: number; game: number | null; tools: number };
      const bad: Bad[] = [];
      let both = 0, sea = 0;
      pts.forEach(([x, z], i) => {
        const top = spansAt(gs, x, z)[0]?.top;
        if (top === undefined || top <= water) { sea++; return; }
        both++;
        const g = game[i];
        if (g === null) bad.push({ kind: "miss", x, z, game: null, tools: top });
        else if (g < top - tol) bad.push({ kind: "low", x, z, game: g, tools: top });
        else if (g > top + tol) bad.push({ kind: "high", x, z, game: g, tools: top });
      });
      // side-by-side rays of one kind are one place
      const groups: { kind: string; rays: number; at: number[]; game: number | null; tools: number; alongAt?: number }[] = [];
      const cell = (b: Bad) => `${b.kind}:${Math.round(b.x / (step * 3))}:${Math.round(b.z / (step * 3))}`;
      const byCell = new Map<string, Bad[]>();
      for (const b of bad) { const key = cell(b); byCell.set(key, [...(byCell.get(key) ?? []), b]); }
      for (const list of byCell.values()) {
        const worst = list.reduce((p, c) => ((c.game ?? -Infinity) < (p.game ?? -Infinity) ? c : p));
        groups.push({ kind: worst.kind, rays: list.length, at: [r2(worst.x / k), r2(worst.z / k)], game: worst.game === null ? null : r2(worst.game / k), tools: r2(worst.tools / k),
          ...(path ? { alongAt: r2(near(worst.x, worst.z).along / k) } : {}) });
      }
      groups.sort((p, q) => (p.kind === q.kind ? q.rays - p.rays : p.kind === "miss" ? -1 : q.kind === "miss" ? 1 : p.kind < q.kind ? 1 : -1));
      const n = (kind: string) => bad.filter((b) => b.kind === kind).length;
      return { ok: bad.length === 0, summary: `${pts.length} rays every ${r2(step / k)}: ${both} over ground the tools read, ${n("miss")} misses, ${n("low")} low, ${n("high")} high (${sea} over the sea or no ground, not compared)`,
        misses: n("miss"), low: n("low"), high: n("high"), places: groups.slice(0, a.limit ?? 40), ...(groups.length > (a.limit ?? 40) ? { more: groups.length - (a.limit ?? 40) } : {}),
        tolerance: r2(tol / k), turn: st.turn?.index, units: a.units ?? "map",
        note: "miss: the game's sweep hit nothing where the project has ground (a worm or a mine falls through there); low: it landed more than tolerance under the tools' top (through a skin onto its core); high: something stands there that the project does not have. A blast's hole reads as low or miss: scan before anything explodes" };
    },
  },
  {
    name: "game_end_turns",
    description: "End turns of the running Test until something is due, so a check does not depend on walking order or on waiting out every turn: each turn is left to run `runSec` seconds (default 0) and then ended (level.test.endTurn, the timer's own message), until `count` turns were ended, turn `untilTurn` is playing, `untilWorm`'s turn (a slot) is playing, the round clock has run `untilElapsedSec` since the Test's start (Sandtide's curse: 180, so 185) or shows at most `untilRoundLeftSec`, or sudden death began (`untilSuddenDeath`). The round clock only runs while a turn plays, so reaching an elapsed time means letting turns run: with untilElapsedSec or untilRoundLeftSec and no runSec, each turn is left to run to 2 s before its end. Stops at `maxTurns` (default 40) or `timeoutSec` (default 600) and says why. Do not hold the round clock (test {suddenDeath: false}) when waiting on it. Answers the turns ended, the clocks and every worm (alive, health, position) at the end: a worm parked with game_park_worm is read where it stands.",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      count: { type: "integer", minimum: 1, maximum: 200 },
      untilTurn: { type: "integer", minimum: 1 }, untilWorm: { type: "integer", minimum: 0, maximum: 15 },
      untilElapsedSec: { type: "number", minimum: 1 }, untilRoundLeftSec: { type: "number", minimum: 0 }, untilSuddenDeath: { type: "boolean" },
      runSec: { type: "number", minimum: 0, maximum: 120 }, maxTurns: { type: "integer", minimum: 1, maximum: 400 }, timeoutSec: { type: "number", minimum: 5, maximum: 3600 },
      units: { type: "string", enum: ["map", "world"] },
    } },
    async handler(s: Session, a: { count?: number; untilTurn?: number; untilWorm?: number; untilElapsedSec?: number; untilRoundLeftSec?: number; untilSuddenDeath?: boolean; runSec?: number; maxTurns?: number; timeoutSec?: number; units?: "map" | "world" }) {
      await need(s, "level.test.endTurn", "Melange 0.13+");
      if ([a.count, a.untilTurn, a.untilWorm, a.untilElapsedSec, a.untilRoundLeftSec, a.untilSuddenDeath].every((v) => v === undefined)) throw new ToolError("give count or an until condition");
      const c = await s.client(), t0 = Date.now(), limit = (a.timeoutSec ?? 600) * 1000, k = a.units === "world" ? 1 : 1 / W;
      const clock = a.untilElapsedSec !== undefined || a.untilRoundLeftSec !== undefined;
      let st = await now(s), ended = 0, why = "";
      const round0 = st.roundMsLeft;
      const due = (x: Now): string => {
        if (a.count !== undefined && ended >= a.count) return `${ended} turns ended`;
        if (x.turn?.phase !== "play") return "";
        if (a.untilTurn !== undefined && x.turn.index >= a.untilTurn) return `turn ${x.turn.index} is playing`;
        if (a.untilWorm !== undefined && x.turn.worm === a.untilWorm) return `WORM${a.untilWorm}'s turn is playing`;
        if (a.untilSuddenDeath && x.suddenDeath) return "sudden death began";
        if (a.untilRoundLeftSec !== undefined && x.roundMsLeft !== undefined && x.roundMsLeft <= a.untilRoundLeftSec * 1000) return `the round clock shows ${Math.round(x.roundMsLeft / 1000)} s`;
        if (a.untilElapsedSec !== undefined && round0 !== undefined && x.roundMsLeft !== undefined && round0 - x.roundMsLeft >= a.untilElapsedSec * 1000) return `the round clock ran ${Math.round((round0 - x.roundMsLeft) / 1000)} s`;
        return "";
      };
      for (;;) {
        st = await now(s);
        if (!st.turn) { why = "the match is over (or not playing)"; break; }
        if ((why = due(st))) break;
        if (ended >= (a.maxTurns ?? 40)) { why = `maxTurns (${a.maxTurns ?? 40}) reached`; break; }
        if (Date.now() - t0 > limit) { why = `timeoutSec (${a.timeoutSec ?? 600}) reached`; break; }
        if (st.turn.phase !== "play") { await sleep(400); continue; }
        // let the turn run: runSec, or to 2 s before its end when waiting on the round clock
        const run = a.runSec !== undefined ? a.runSec * 1000 : clock ? Math.max(0, (st.turnMsLeft ?? 0) - 2000) : 0;
        const index = st.turn.index, tEnd = Date.now() + run;
        let dueNow = "";
        while (Date.now() < tEnd && !(dueNow = due(await now(s)))) await sleep(Math.min(1000, Math.max(50, tEnd - Date.now())));
        if (dueNow) { why = dueNow; break; }
        await c.call("level.test.endTurn", {}, 15000);
        ended++;
        // the next turn's start (or the match's end)
        for (let i = 0; i < 150; i++) { await sleep(300); const x = await now(s); if (!x.turn || x.turn.index !== index) break; }
      }
      st = await now(s);
      return { stopped: why, turnsEnded: ended, seconds: Math.round((Date.now() - t0) / 1000), turn: st.turn ?? null,
        clock: { turnLeftSec: st.turnMsLeft !== undefined ? Math.round(st.turnMsLeft / 1000) : null, roundLeftSec: st.roundMsLeft !== undefined ? Math.round(st.roundMsLeft / 1000) : null,
          ...(round0 !== undefined && st.roundMsLeft !== undefined ? { roundElapsedSec: Math.round((round0 - st.roundMsLeft) / 1000) } : {}), suddenDeath: !!st.suddenDeath },
        worms: st.worms.map((w) => ({ worm: w.slot, team: w.team, alive: w.alive, health: w.health, at: w.pos.map((v) => r2(v * k)) })), units: a.units ?? "map" };
    },
  },
];
