// Tool group "moves": test moves and checks on a running Test that the rounds of Dustcrown had to do by hand or could
// not do (round 3's gaps). game_face turns the active worm (a jetpack flies the way the worm faces: the SAND S flight
// went the wrong way); game_use_tool flies a jetpack to a point and says where the worm landed; game_riser_test tries a
// walk, a jump and a backflip at a riser and says how high the worm got; game_start_level starts an installed pack level
// in a named game style; game_shot_compare keeps a pose's shot per plugin set and diffs them across launches;
// game_spawn_deals reads the spawn-area deal each peer of a network game logged and says whether they agree. The pure
// parts are lib/harness-moves.ts. Positions are map units unless `units` says world.
import { existsSync, mkdirSync, readFileSync, readdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { compareDeals, facingOf, imageDiff, jetStep, parseSpawnDeals, riserVerdict, turnPulse, wrapDeg, yawToward, type RiserTry } from "../lib/harness-moves";
import { decodePng, encodePng } from "../lib/inspect-png";
import { colourReads, pixelReads } from "../lib/exposed-core";
import { documentsDir } from "../oasis";
import { ToolError, image, type Content, type Tool } from "../registry";
import type { Session } from "../session";
import { clearCamera, evalClient, gameState, selectWeapon, setCamera, shot, walkKeys } from "./game";
import { tools as harnessTools, tickLua } from "./harness";

const W = 20;
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
const r2 = (v: number) => Math.round(v * 100) / 100;
const DEG = 180 / Math.PI;

const has = async (s: Session, method: string) => (await s.ensure()).methods.includes(method);

/** The active worm: its slot, place (world units), yaw (degrees), the turn's phase and the game camera's heading. */
const ACTIVE_LUA = `local t = wum.game.inMatch() and wum.game.turn() or nil
if not t then return "none" end
local w = nil
for _, x in ipairs(wum.game.worms() or {}) do if x.slot == t.worm then w = x end end
local c = wum.render.gameCamera and wum.render.gameCamera() or wum.render.camera()
if not w then return "none;" .. tostring(t.phase) end
return string.format("%d,%.2f,%.2f,%.2f,%.5f,%s,%s,%s,%.3f,%.3f,%s", w.slot, w.pos.x, w.pos.y, w.pos.z, w.yaw or 0, tostring(t.phase), tostring(t.ai), tostring(w.alive), c and c.fwd.x or 0, c and c.fwd.z or 1, tostring(w.health))`;
interface Active { slot: number; pos: number[]; yaw: number; phase: string; ai: boolean; alive: boolean; fx: number; fz: number; health: number; }
async function active(s: Session): Promise<Active | null> {
  const t = await evalClient(s, ACTIVE_LUA);
  if (t.startsWith("none")) return null;
  const v = t.split(",");
  return { slot: +v[0], pos: [+v[1], +v[2], +v[3]], yaw: +v[4] * DEG, phase: v[5], ai: v[6] === "true", alive: v[7] === "true", fx: +v[8], fz: +v[9], health: +v[10] };
}

/** Waits for a human turn in play (`worm`: that slot's), or throws. */
async function turnOf(s: Session, worm: number | undefined, waitSec: number): Promise<Active> {
  const t0 = Date.now();
  for (;;) {
    const a = await active(s);
    if (a && !a.ai && a.phase === "play" && (worm === undefined || a.slot === worm)) return a;
    if (Date.now() - t0 > waitSec * 1000) throw new ToolError(`no ${worm !== undefined ? `turn of WORM${worm}` : "human turn"} in play within ${waitSec} s (now: ${a ? `WORM${a.slot} ${a.phase}${a.ai ? " ai" : ""}` : "no turn"}): game_end_turns {untilWorm} brings it`);
    await sleep(300);
  }
}

async function drive(s: Session, inputs: string[], ms: number) {
  await (await s.client()).call("input.drive", inputs.length ? { hold: inputs, ms: Math.max(1, Math.min(5000, Math.round(ms))), replace: true } : { release: true }, 15000);
}
const KEY: Record<string, string> = { W: "MoveForward", A: "MoveLeft", S: "MoveBackward", D: "MoveRight" };

/** Turns the active worm to `want` (degrees) within `tol`: the turn inputs while they turn it, else short walk taps
 * toward the heading (a tap of a walk key turns a worm before it steps). */
async function face(s: Session, want: number, tol: number, timeoutMs: number): Promise<{ yaw: number; error: number; pulses: number; method: string; moved: number; ok: boolean }> {
  const t0 = Date.now();
  let a = await active(s);
  if (!a) throw new ToolError("no active worm to turn");
  const start = a.pos;
  let pulses = 0, rate = 0, dead = 0, method = "TurnLeft / TurnRight", turnSign = 0;
  while (Date.now() - t0 < timeoutMs) {
    const p = turnPulse(a.yaw, want, rate, tol);
    if (p.done) break;
    const before = a.yaw;
    if (method.startsWith("Turn")) {
      // which input raises the yaw is learnt from the first pulse that turns it
      const input = (turnSign || 1) * p.sign > 0 ? "TurnRight" : "TurnLeft";
      await drive(s, [input], p.ms);
      await sleep(p.ms + 120);
      await drive(s, [], 0);
      const b = await active(s);
      if (!b || b.slot !== a.slot || b.phase !== "play") break;
      const d = wrapDeg(b.yaw - before);
      if (Math.abs(d) < 0.5) { if (++dead >= 2) method = "walk taps"; }
      else {
        dead = 0;
        rate = Math.abs(d) / p.ms;
        if (!turnSign) turnSign = (d > 0) === (input === "TurnRight") ? 1 : -1;
      }
      a = b;
    } else {
      const f = facingOf(want);
      await drive(s, walkKeys(f[0], f[1], a.fx, a.fz).map((k) => KEY[k] ?? k), 70);
      await sleep(260);
      const b = await active(s);
      if (!b || b.slot !== a.slot || b.phase !== "play") break;
      a = b;
    }
    pulses++;
  }
  await drive(s, [], 0).catch(() => undefined);
  const err = wrapDeg(want - a.yaw);
  return { yaw: r2(wrapDeg(a.yaw)), error: r2(err), pulses, method, moved: r2(Math.hypot(a.pos[0] - start[0], a.pos[2] - start[2]) / W), ok: Math.abs(err) <= tol };
}

/** The game's land sweep along segments (world units): each hit's point, or null. */
async function rays(s: Session, segs: number[][]): Promise<(number[] | null)[]> {
  const out: (number[] | null)[] = [];
  const c = await s.client();
  for (let i = 0; i < segs.length; i += 64) {
    const r = await c.call<{ rays: { hit: boolean; at?: number[]; error?: string }[] }>("level.test.landRay", { segments: segs.slice(i, i + 64) }, 30000);
    for (const ray of r.rays) out.push(ray.hit && ray.at ? ray.at : null);
  }
  return out;
}

const park = (s: Session, a: Record<string, unknown>) => harnessTools.find((t) => t.name === "game_park_worm")!.handler(s, a as never) as Promise<{ at: number[]; ground?: number }>;

/** Samples the worm's place every `everyMs` for `ms`: the highest y, and where it is at the end (world units). */
async function watch(s: Session, slot: number, ms: number, everyMs = 60): Promise<{ maxY: number; end: number[]; samples: number; alive: boolean }> {
  const t0 = Date.now();
  let maxY = -Infinity, end: number[] = [0, 0, 0], n = 0, alive = true;
  while (Date.now() - t0 < ms) {
    const a = await active(s);
    if (!a || a.slot !== slot) break;
    maxY = Math.max(maxY, a.pos[1]); end = a.pos; alive = a.alive; n++;
    await sleep(everyMs);
  }
  return { maxY, end, samples: n, alive };
}

function shotDir(): string {
  const d = documentsDir();
  if (!d) throw new ToolError("no Documents\\Melange folder to keep the compare in");
  return join(d, "Melange", "erg-mcp", "shot-compare");
}

const unitsProp = { type: "string", enum: ["map", "world"], description: "Units of the positions given and answered: map (default) or world (map x 20)." } as const;

interface RopePlan { aimSec?: number; release?: boolean; refire?: number; steps?: { do: "shorten" | "lengthen" | "swingForward" | "swingBackward" | "swingLeft" | "swingRight" | "wait" | "jump"; sec?: number; until?: "still" }[] }
const ROPE_MSG: Record<string, string> = { shorten: "NinjaRope.Shorten", lengthen: "NinjaRope.Lengthen", swingForward: "NinjaRope.SwingForward", swingBackward: "NinjaRope.SwingBackward",
  swingLeft: "NinjaRope.SwingLeft", swingRight: "NinjaRope.SwingRight" };

/** game_use_tool {weapon: "rope"}: the ninja rope fired up at what the worm faces and worked by its own messages. The
 * game's input layer turns the walk and aim keys into NinjaRope.Shorten / Lengthen / Swing*.On and .Off while a rope
 * hangs; sent in the match VM (level.test.lua) they reel and swing it whatever the window's focus (Dustcrown round 4
 * hooked a rope and could not climb it: the drive had no rope input). Measured in round 5: Shorten took a worm hooked
 * from step 1 up 1.3 map units in half a second, Lengthen put it back. */
async function useRope(s: Session, a: { target: number[]; worm?: number; arrive?: number; waitSec?: number; units?: "map" | "world"; rope?: RopePlan }) {
  for (const m of ["input.drive", "level.test.lua"]) if (!(await has(s, m))) throw new ToolError(`game_use_tool needs the game's ${m} (Test with this branch's Melange)`);
  const k = a.units === "world" ? 1 : W, out = (p: number[]) => p.map((v) => r2(v / k));
  const target = a.target.map((v) => v * k), arrive = (a.arrive ?? 1.5 * W / k) * k, plan = a.rope ?? {};
  const steps = plan.steps ?? [{ do: "shorten" as const, sec: 4, until: "still" as const }, { do: "swingForward" as const, sec: 0.8 }];
  const w = await turnOf(s, a.worm, a.waitSec ?? 60);
  // the worm turns first: a turn after the pick left the rope unfired (round 5)
  const want = yawToward([w.pos[0], w.pos[2]], [target[0], target[2]]);
  const faced = want === null ? null : await face(s, want, 15, 8000);
  const sel = await selectWeapon(s, "kUtilityNinjaRope");
  await sleep(300);
  const here = async () => (await active(s))?.pos ?? null;
  const msg = (m: string) => tickLua(s, `SendMessage("${m}") return "sent"`);
  const shots: Record<string, unknown>[] = [], notes: string[] = [];
  let pos = w.pos, why = "done";
  for (let shotN = 0; shotN <= (plan.refire ?? 0); shotN++) {
    const from = pos;
    // the aim comes up from where it is, then the utility's fire throws the hook
    if ((plan.aimSec ?? 1.2) > 0 && shotN === 0) await drive(s, ["AimUp"], (plan.aimSec ?? 1.2) * 1000).then(() => sleep((plan.aimSec ?? 1.2) * 1000 + 100));
    await drive(s, ["FireUtil"], 250);
    await sleep(900);
    await drive(s, [], 0).catch(() => undefined);
    const hooked = await here();
    if (!hooked) { why = "the turn left play"; break; }
    const trace: { do: string; sec: number; at: number[] }[] = [];
    let maxY = hooked[1];
    for (const st of steps) {
      const ms = Math.min(6000, (st.sec ?? 1) * 1000), m = ROPE_MSG[st.do];
      if (m) await msg(`${m}.On`);
      else if (st.do === "jump") await drive(s, ["Jump", "MoveForward"], ms);
      const t0 = Date.now();
      let last = (await here()) ?? pos, stillFor = 0, lastT = t0;
      while (Date.now() - t0 < ms) {
        await sleep(150);
        const p = await here();
        if (!p) break;
        maxY = Math.max(maxY, p[1]);
        const moved = Math.hypot(p[0] - last[0], p[1] - last[1], p[2] - last[2]);
        stillFor = moved < 0.05 * W ? stillFor + (Date.now() - lastT) : 0;
        last = p; lastT = Date.now();
        if (st.until === "still" && stillFor >= 600) break;
      }
      if (m) await msg(`${m}.Off`);
      pos = last;
      trace.push({ do: st.do, sec: r2((Date.now() - t0) / 1000), at: out(last) });
    }
    if (plan.release !== false) {
      await msg("NinjaRope.Kill");
      // down to rest: the worm has not moved for 0.6 s (at most 6 s)
      let last = (await here()) ?? pos, still = 0;
      for (let i = 0; i < 40 && still < 4; i++) {
        await sleep(150);
        const p = await here();
        if (!p) break;
        still = Math.hypot(p[0] - last[0], p[1] - last[1], p[2] - last[2]) < 0.03 * W ? still + 1 : 0;
        last = p;
      }
      pos = last;
    }
    const rose = hooked[1] - from[1], pulled = Math.hypot(hooked[0] - from[0], hooked[2] - from[2]);
    shots.push({ from: out(from), afterFire: out(hooked), ...(rose < 0.05 * W && pulled < 0.05 * W ? { hooked: "the worm did not move at the shot: the hook may have found nothing (a higher or lower aim, or nearer the wall)" } : {}),
      steps: trace, maxY: r2(maxY / k), ...(plan.release !== false ? { landed: out(pos) } : { hangsAt: out(pos) }) });
    const now = await active(s);
    if (!now || now.slot !== w.slot || now.phase !== "play") { why = "the turn left play"; break; }
    if (!now.alive) { why = "the worm died"; break; }
  }
  const off = Math.hypot(pos[0] - target[0], pos[1] - target[1], pos[2] - target[2]);
  if (sel && (sel as { selected?: boolean }).selected === false) notes.push("the rope was not selected (the scheme may give none: game_give_weapons {kit: \"movement\"})");
  return { weapon: "rope", worm: w.slot, from: out(w.pos), target: out(target), ...(faced ? { faced: { yaw: faced.yaw, error: faced.error, ok: faced.ok } } : {}),
    aimSec: plan.aimSec ?? 1.2, shots, end: out(pos), climbed: r2((pos[1] - w.pos[1]) / k), offTarget: r2(off / k), reached: off <= arrive, stop: why, units: a.units ?? "map",
    ...(notes.length ? { notes } : {}),
    note: "the rope is fired with the utility's fire after the aim was raised aimSec, then worked by its own messages in the match VM (NinjaRope.Shorten / Lengthen / Swing*.On and .Off) and let go with NinjaRope.Kill; afterFire is where the worm hung once the hook held, landed where it rested. reached: within `arrive` of the target" };
}

/** game_crater_look: each crater shot straight down and read. */
interface CraterArgs { points?: number[][]; last?: number; height?: number; part?: number; minShare?: number; flag?: ("blue" | "white")[]; prefix?: string; width?: number; fov?: number; images?: boolean; units?: "map" | "world"; }

export const tools: Tool[] = [
  {
    name: "game_crater_look",
    description: "What the craters of the running Test show: for each explosion the Test probe has seen (the `last` of them, default 12; or `points`), the camera is put `height` map units straight over it looking down, a clean shot is taken, and the middle of the picture (`part` of its width and height) is read pixel by pixel: the mean colour and the share of pixels that read as earth, as blue (cyan to violet, pale lilac included) and as white (bright and nearly grey). A crater whose blue and white share is `minShare` or more is flagged: its floor reads as a hole to the sky (the top texture of the band it opened onto: check_exposed_core finds those before a build, the structure tools' coreMaterial fixes them). Run it after game_air_strike, test_explosion {game: true} or a volley; ok when none is flagged.",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      points: { type: "array", maxItems: 48, items: { type: "array", items: { type: "number" }, minItems: 3, maxItems: 3 }, description: "Crater centres [x, y, z] in `units` instead of the probe's explosions." },
      last: { type: "integer", minimum: 1, maximum: 48, description: "How many of the probe's latest explosions (default 12)." },
      height: { type: "number", minimum: 2, maximum: 80, description: "The eye's height over the point, map units (default 9)." },
      part: { type: "number", minimum: 0.05, maximum: 1, description: "The share of the picture's width and height read, round its centre (default 0.3)." },
      minShare: { type: "number", minimum: 0, maximum: 1, description: "Flag a crater whose flagged pixels are this share or more (default 0.2)." },
      flag: { type: "array", items: { type: "string", enum: ["blue", "white"] }, description: "What counts (default both)." },
      prefix: { type: "string" }, width: { type: "integer", minimum: 160, maximum: 1920 }, fov: { type: "number", minimum: 10, maximum: 120 },
      images: { type: "boolean" }, units: { type: "string", enum: ["map", "world"] },
    } },
    async handler(s: Session, a: CraterArgs) {
      const st = await gameState(s);
      if (st.scene !== "match") throw new ToolError(`no match is running (scene ${st.scene}): test the project and set off the explosions first`);
      const k = a.units === "world" ? 1 / 20 : 1, W = 20;
      let pts: { at: number[]; source?: string; turn?: number }[];
      if (a.points?.length) pts = a.points.map((p) => ({ at: [p[0] * k, p[1] * k, p[2] * k] }));
      else {
        const pr = (await (await s.client()).call<{ probe: { explosions: { turn: number; source: string; at: number[] }[] } }>("level.test.probe", {}, 15000)).probe;
        pts = pr.explosions.slice(-(a.last ?? 12)).map((e) => ({ at: e.at, source: e.source, turn: e.turn }));
        if (!pts.length) throw new ToolError("the Test probe has seen no explosion in this match: set one off first (game_air_strike, test_explosion {game: true}), or give points");
      }
      const flagOn = new Set(a.flag ?? ["blue", "white"]), minShare = a.minShare ?? 0.2, h = a.height ?? 9, prefix = a.prefix ?? `erg_${Date.now().toString(36)}_crater`;
      const rows: Record<string, unknown>[] = [], pics: Content[] = [];
      for (const [i, p] of pts.entries()) {
        const row: Record<string, unknown> = { at: p.at.map((v) => Math.round((v / k) * 100) / 100), ...(p.source ? { source: p.source, turn: p.turn } : {}) };
        try {
          // straight down, a hair off the vertical so the camera's up is defined
          const set = await setCamera(s, { pos: [p.at[0] * W, (p.at[1] + h) * W, p.at[2] * W + 2], target: [p.at[0] * W, (p.at[1] - 2) * W, p.at[2] * W], fov: a.fov ?? 50 }, 40);
          const sh = await shot(s, `${prefix}_${i}`, { clean: true, width: a.width ?? 480, minFrame: set.frame !== undefined ? set.frame + 2 : undefined });
          const img = decodePng(sh.png), rd = pixelReads(img.rgb, img.w, img.h, a.part ?? 0.3);
          const bad = (flagOn.has("blue") ? rd.share.blue : 0) + (flagOn.has("white") ? rd.share.white : 0);
          Object.assign(row, { shot: sh.path, mean: `#${rd.mean.map((v) => v.toString(16).padStart(2, "0")).join("")}`, meanReads: colourReads(rd.mean), share: rd.share, ...(bad >= minShare ? { flagged: true } : {}) });
          if (a.images) pics.push(image(sh.png));
        } catch (e) { row.error = (e as Error).message; }
        rows.push(row);
      }
      await clearCamera(s).catch(() => undefined);
      const flagged = rows.filter((r) => r.flagged), failed = rows.filter((r) => r.error);
      const out = { ok: flagged.length === 0 && failed.length < rows.length, craters: rows.length, flagged: flagged.length,
        summary: flagged.length ? `${flagged.length} of ${rows.length} craters show a floor that reads as sky or ice (blue or white pixels over ${Math.round(minShare * 100)} % of the middle of the shot)` : `${rows.length - failed.length} craters read as earth`,
        rows, ...(flagged.length ? { fix: "check_exposed_core names the material and the face; the structure's coreMaterial (or a band whose top reads as earth) fixes it" } : {}),
        note: "each crater shot from straight over it; the middle of the picture read pixel by pixel (blue: cyan to violet, pale lilac included; white: bright and nearly grey)" };
      return a.images ? { content: [...pics, { type: "text" as const, text: JSON.stringify(out) }] } : out;
    },
  },
  {
    name: "game_face",
    description: "Turn the active worm of the running Test to face a heading or a point, before a jetpack flight, a rope, a jump or a shot (a jetpack flies the way the worm faces: Dustcrown's SAND S flight failed because the worm faced away from the palace). `yaw`: degrees, 0 toward +z and 90 toward +x (a worm's own yaw: its facing is (sin, cos)); or `towards`: [x, z] or [x, y, z]. It waits for a human turn in play (`worm`: that WORMn's), then turns with the game's own inputs posted from its input tick (input.drive: Input.TurnLeft / TurnRight in pulses sized from the turn rate it measures; where those do not turn the worm, short taps of the walk keys toward the heading, which turn a worm before it steps), reading the worm's yaw after each until it is within `tolerance` degrees (default 12). Answers the yaw reached, the error, the pulses, the method and how far the worm moved (a walk tap can step it). No keyboard and no window focus involved.",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      yaw: { type: "number", minimum: -360, maximum: 360 },
      towards: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 3 },
      worm: { type: "integer", minimum: 0, maximum: 15, description: "Only this WORMn's turn (default the human turn in play)." },
      tolerance: { type: "number", minimum: 1, maximum: 90 },
      waitSec: { type: "number", minimum: 1, maximum: 300, description: "Wait at most this long for the turn (default 60)." },
      units: unitsProp,
    } },
    async handler(s: Session, a: { yaw?: number; towards?: number[]; worm?: number; tolerance?: number; waitSec?: number; units?: "map" | "world" }) {
      if (!(await has(s, "input.drive"))) throw new ToolError("game_face needs the game's input.drive (Melange 0.13+ with the Controls module)");
      if ((a.yaw === undefined) === (a.towards === undefined)) throw new ToolError("give yaw or towards");
      const k = a.units === "world" ? 1 : W;
      const w = await turnOf(s, a.worm, a.waitSec ?? 60);
      const want = a.yaw ?? yawToward([w.pos[0], w.pos[2]], [a.towards![0] * k, a.towards![a.towards!.length - 1] * k]);
      if (want === null) throw new ToolError("towards is where the worm stands: no heading");
      const r = await face(s, want, a.tolerance ?? 12, 12000);
      return { worm: `WORM${w.slot}`, wanted: r2(wrapDeg(want)), from: r2(wrapDeg(w.yaw)), ...r, facing: facingOf(r.yaw).map(r2),
        note: "yaw: degrees, 0 toward +z, 90 toward +x; the worm's facing is (sin yaw, cos yaw)" + (r.ok ? "" : "; NOT within tolerance: the turn left play, or the inputs did not turn it") };
    },
  },
  {
    name: "game_use_tool",
    description: "Use a movement tool with the active worm of the running Test toward a target and answer where the worm landed: the in-game check of check_tool_reach. `weapon: \"jetpack\"`: the worm takes the jetpack (game_select_weapon: the scheme must give one, or game_give_weapons {kit: \"movement\"}), turns to the target (game_face), then flies: Input.FireUtil is the thrust and the walk inputs steer (Input.MoveForward and its three fellows, camera-relative: what the game accepts in flight), posted from the game's input tick and chosen again every 120 ms from the worm's place: a lob, because a jetpack only pushes while it thrusts: straight up until the worm is half of `clearance` (default 3 map units) over the target's height (a worm steered at a wall from its foot is held against it and burns its tank there), then thrust and steering together until the speed it has would carry it to the target as it falls, then no thrust: it coasts over and drops once within `arrive` (default 1.5) of it in plan. Thrust is capped at 7.3 s (a tank is 7.5). It ends when the worm rests, the turn leaves play, or `maxSec` (default 14; a full tank is about 7.5 s of thrust). `target`: [x, y, z], y the ground to land on. Answers the landing, its distance from the target in plan and in height, the highest point and the time: `reached` when it rests within `arrive` + 1.5 of the target and within 1.5 of its height. `weapon: \"rope\"`: the worm turns to the target, takes the ninja rope, raises its aim `rope.aimSec` and fires (the hook holds on what the aim meets: a wall's face, not open sky over its top), then works the rope by `rope.steps`, the rope's own messages sent in the match VM (shorten reels in, lengthen lets out, swing* swings; default: shorten until still, then swing forward), lets go and waits to rest; the answer's shots give where it hung after the fire, each step's end, the highest it was and where it landed, and `reached` when that is within `arrive` of the target. `weapon: \"teleport\" | \"girder\"` are refused with what is known: the build has no way to place their cursor from a script yet (see docs/erg-mcp.md, Dustcrown round 5).",
    inputSchema: { type: "object", additionalProperties: false, required: ["weapon", "target"], properties: {
      weapon: { type: "string", enum: ["jetpack", "teleport", "rope", "girder"] },
      target: { type: "array", items: { type: "number" }, minItems: 3, maxItems: 3 },
      rope: { type: "object", additionalProperties: false, description: "weapon rope: how it is driven. aimSec: how long the aim is raised from level before the shot (Input.AimUp held; default 1.2: steeply up at a wall a couple of units away). steps: what the worm does on the rope, in order, each {do: shorten | lengthen | swingForward | swingBackward | swingLeft | swingRight | wait | jump (the jump key with the walk-forward key, held sec: a hop off the rope), sec (at most 6), until: \"still\" (stop early once the worm has not moved 0.15 map units in 0.6 s)}: the rope's own messages, NinjaRope.Shorten.On / .Off and the rest, sent in the match VM. Default: shorten until still (at most 4 s), swing forward 0.8 s. release (default true): let go at the end (NinjaRope.Kill) and wait for the worm to rest. refire: after the release, fire again and run the steps again this many times (default 0; a scheme gives a rope several shots), each from where the worm then is.",
        properties: { aimSec: { type: "number", minimum: 0, maximum: 5 }, release: { type: "boolean" }, refire: { type: "integer", minimum: 0, maximum: 5 },
          steps: { type: "array", maxItems: 16, items: { type: "object", additionalProperties: false, required: ["do"], properties: {
            do: { type: "string", enum: ["shorten", "lengthen", "swingForward", "swingBackward", "swingLeft", "swingRight", "wait", "jump"] }, sec: { type: "number", minimum: 0.05, maximum: 6 }, until: { type: "string", enum: ["still"] } } } } } },
      worm: { type: "integer", minimum: 0, maximum: 15 },
      clearance: { type: "number", minimum: 0.5, maximum: 40 }, arrive: { type: "number", minimum: 0.3, maximum: 20 }, maxSec: { type: "number", minimum: 2, maximum: 40 },
      waitSec: { type: "number", minimum: 1, maximum: 300 },
      units: unitsProp,
    } },
    async handler(s: Session, a: { weapon: "jetpack" | "teleport" | "rope" | "girder"; target: number[]; worm?: number; clearance?: number; arrive?: number; maxSec?: number; waitSec?: number; units?: "map" | "world"; rope?: RopePlan }) {
      if (a.weapon === "rope") return useRope(s, a);
      if (a.weapon !== "jetpack") throw new ToolError(`game_use_tool cannot drive the ${a.weapon} yet: ${a.weapon === "teleport" ? "the teleport's target is a cursor the player moves, and no input or message of this build places it from a script (game_park_worm puts a worm on a spot without the weapon)"
        : a.weapon === "girder" ? "Girder.Place and the Girder.Move messages sent from the match VM did nothing in round 3, and the fire inputs leave the cursor on the worm, where a girder is refused"
        : ""}. The jetpack and the rope are driven`);
      if (!(await has(s, "input.drive"))) throw new ToolError("game_use_tool needs the game's input.drive (Melange 0.13+ with the Controls module)");
      const k = a.units === "world" ? 1 : W, out = (p: number[]) => p.map((v) => r2(v / k));
      const target = a.target.map((v) => v * k), clear = (a.clearance ?? 3 * W / k) * k, arrive = (a.arrive ?? 1.5 * W / k) * k;
      const w = await turnOf(s, a.worm, a.waitSec ?? 60);
      const sel = await selectWeapon(s, "kUtilityJetpack");
      const want = yawToward([w.pos[0], w.pos[2]], [target[0], target[2]]);
      const faced = want === null ? null : await face(s, want, 15, 8000);
      const t0 = Date.now(), maxMs = (a.maxSec ?? 14) * 1000, FUEL_MS = 7300;
      // the fall's acceleration is measured in the flight (the samples without thrust); the first guess is 15 map units / s2
      let last = w.pos, lastT = t0, vel = [0, 0, 0], g = 15 * W, maxY = w.pos[1], rest = 0, why = "maxSec", thrustMs = 0, flew = false, climbed = false, wasThrust = false;
      const phases: string[] = [];
      try {
        while (Date.now() - t0 < maxMs) {
          const now = await active(s);
          if (!now || now.slot !== w.slot || now.phase !== "play") { why = "the turn left play"; break; }
          if (!now.alive) { why = "the worm died"; break; }
          const t = Date.now(), dt = Math.max(1, t - lastT) / 1000;
          const v = [0, 1, 2].map((i) => (now.pos[i] - last[i]) / dt);
          // free fall between two samples without thrust: what the speed lost is the fall's acceleration
          if (!wasThrust && flew && rest === 0 && Math.abs(v[1]) > 20) { const gm = (vel[1] - v[1]) / dt; if (gm > 4 * W && gm < 40 * W) g = 0.7 * g + 0.3 * gm; }
          vel = v;
          const speed = Math.hypot(v[0], v[1], v[2]);
          maxY = Math.max(maxY, now.pos[1]);
          if (now.pos[1] > w.pos[1] + 0.5 * W) flew = true;
          if (now.pos[1] >= target[1] + 0.5 * clear) climbed = true;   // once up, it steers on even when it sinks
          const st = jetStep(now.pos, vel, target, { clear, arrive, g, band: 12 * W, maxFall: 14 * W, climbed });
          const thrust = st.thrust && thrustMs < FUEL_MS;
          if (phases[phases.length - 1] !== st.phase) phases.push(st.phase);
          // at rest after a flight (or over the target and still): done
          rest = speed < 4 && !thrust ? rest + dt * 1000 : 0;
          if (rest > 500 && (flew || st.over) && t - t0 > 800) { why = st.over ? "landed" : "at rest"; last = now.pos; break; }
          const inputs = [...(thrust ? ["FireUtil"] : []), ...(st.steer ? walkKeys(st.steer[0], st.steer[1], now.fx, now.fz).map((x) => KEY[x] ?? x) : [])];
          if (thrust) thrustMs += dt * 1000;
          wasThrust = thrust;
          await drive(s, inputs, 400);
          last = now.pos; lastT = t;
          await sleep(100);
        }
      } finally {
        await drive(s, [], 0).catch(() => undefined);
      }
      await sleep(700);
      const end = (await active(s))?.pos ?? last;
      const plan = Math.hypot(end[0] - target[0], end[2] - target[2]), dy = end[1] - target[1];
      return { worm: `WORM${w.slot}`, weapon: "jetpack", selected: sel.name ?? sel.weapon, ...(faced ? { faced: { yaw: faced.yaw, error: faced.error, method: faced.method } } : {}),
        from: out(w.pos), target: out(target), landed: out(end), offTarget: r2(plan / k), heightOff: r2(dy / k), highest: r2(maxY / k), climb: r2((maxY - w.pos[1]) / k),
        thrustSec: r2(thrustMs / 1000), seconds: r2((Date.now() - t0) / 1000), stop: why, flew, phases, fall: r2(g / k),
        reached: plan <= arrive + 1.5 * W && Math.abs(dy) <= 1.5 * W, units: a.units ?? "map",
        note: "FireUtil is the thrust, the walk inputs steer (camera-relative); a shot shows it (game_shots {cams: [{preset: \"worm\"}]})" + (flew ? "" : "; the worm never left the ground: no jetpack in the inventory (game_give_weapons), or the pick was refused") };
    },
  },
  {
    name: "game_riser_test",
    description: "Can a worm get up a riser on foot? In the running Test the active worm is parked at the riser's foot (`at`: [x, z] or [x, y, z], where it stands; game_park_worm), turned to it (`towards`: a point on top of the riser, or `yaw`) and tries, one after another from the same spot: a walk into it (the walk inputs held `holdSec`, default 1.6), a jump at it (Jump with the walk held) and a backflip (Input.JumpBack, facing the riser: in this build it carries the worm the way it faces and rises as a jump does, 2.5 map units). Its place is read every 60 ms: each try answers the highest it rose over the foot (`maxRise`), where it ended (`endRise`) and `climbed` (it rests within 0.3 of the top). The riser is found with the game's own land sweep: the ground every quarter unit along the heading out to `reach` (default 4 map units); the first column 0.75 or more over the foot is the riser (`riserAt`: how far from the foot) and its ground the top. `climbed: false` with `highestReached` under `riserHeight` is the proof that no walking route goes up there (check_reachability's finding, in the game). Needs the worm's own turn (a turn's inputs drive the active worm): game_end_turns {untilWorm} first; the three tries take about 12 s of the turn.",
    inputSchema: { type: "object", additionalProperties: false, required: ["at"], properties: {
      at: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 3 },
      towards: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 3 },
      yaw: { type: "number", minimum: -360, maximum: 360 },
      worm: { type: "integer", minimum: 0, maximum: 15 },
      tries: { type: "array", minItems: 1, maxItems: 3, items: { type: "string", enum: ["walk", "jump", "backflip"] } },
      reach: { type: "number", minimum: 0.3, maximum: 10 }, holdSec: { type: "number", minimum: 0.3, maximum: 5 },
      waitSec: { type: "number", minimum: 1, maximum: 300 },
      units: unitsProp,
    } },
    async handler(s: Session, a: { at: number[]; towards?: number[]; yaw?: number; worm?: number; tries?: ("walk" | "jump" | "backflip")[]; reach?: number; holdSec?: number; waitSec?: number; units?: "map" | "world" }) {
      for (const m of ["input.drive", "level.test.placeWorm", "level.test.landRay"]) if (!(await has(s, m))) throw new ToolError(`game_riser_test needs the game's ${m} (Test with this branch's Melange)`);
      if ((a.yaw === undefined) === (a.towards === undefined)) throw new ToolError("give towards (a point on top of the riser) or yaw");
      const k = a.units === "world" ? 1 / W : 1;                     // call units -> map
      const at = a.at.map((v) => v * k), foot2 = [at[0], at[at.length - 1]];
      const want = a.yaw ?? yawToward(foot2, [a.towards![0] * k, a.towards![a.towards!.length - 1] * k]);
      if (want === null) throw new ToolError("towards is the foot itself: no heading");
      const f = facingOf(want), reach = (a.reach ?? 4 / k) * k;
      const w = await turnOf(s, a.worm, a.waitSec ?? 60);
      const tries: RiserTry[] = [], notes: string[] = [];
      let footY = NaN, topY = NaN, riserAt = NaN;
      for (const kind of a.tries ?? ["walk", "jump", "backflip"]) {
        const p = await park(s, { worm: w.slot, at: at.length === 3 ? at : foot2, units: "map" });
        if (Number.isNaN(footY)) {
          footY = p.at[1];
          // the riser: the game's ground every quarter unit along the heading out to `reach`, read from well over the
          // worm; the first column a step (0.75) or more over the foot is the riser, and its ground the top
          const ds = Array.from({ length: Math.max(1, Math.round(reach / 0.25)) }, (_, i) => (i + 1) * 0.25);
          const hits = await rays(s, ds.map((d) => { const x = (foot2[0] + f[0] * d) * W, z = (foot2[1] + f[1] * d) * W; return [x, (footY + 60) * W, z, x, (footY - 5) * W, z]; }));
          const i = hits.findIndex((h) => h !== null && h[1] / W >= footY + 0.75);
          if (i >= 0) { topY = hits[i]![1] / W; riserAt = ds[i]; }
          else notes.push(`the land sweep found no ground 0.75 or more over the foot within ${r2(reach)} along the heading: no riser there (reach looks farther)`);
        }
        const now = await active(s);
        if (!now || now.slot !== w.slot || now.phase !== "play") { notes.push(`the turn left play before the ${kind}`); break; }
        // every try faces the riser: in this build a backflip (Input.JumpBack) carried the worm the way it faced, 4 map units
        // and 2.5 up, as far up as a jump (Dustcrown round 4: with its back to the riser it flipped away from it)
        const fc = await face(s, want, 15, 6000);
        if (!fc.ok) notes.push(`${kind}: the worm faces ${fc.yaw}, ${Math.abs(fc.error)} degrees off`);
        const cur = (await active(s)) ?? now, fwd = walkKeys(f[0], f[1], cur.fx, cur.fz).map((x) => KEY[x] ?? x), hold = (a.holdSec ?? 1.6) * 1000;
        if (kind === "walk") await drive(s, fwd, hold);
        else if (kind === "jump") { await drive(s, [...fwd, "Jump"], 250); setTimeout(() => { void drive(s, fwd, hold).catch(() => undefined); }, 260); }
        else await drive(s, ["JumpBack"], 250);
        const seen = await watch(s, w.slot, hold + 900);
        await drive(s, [], 0).catch(() => undefined);
        await sleep(300);
        const end = (await active(s))?.pos ?? seen.end;
        tries.push({ kind, startY: footY, maxY: seen.maxY / W, endY: end[1] / W, endAt: end.map((v) => v / W), moved: Math.hypot(end[0] / W - foot2[0], end[2] / W - foot2[1]),
          toward: (end[0] / W - foot2[0]) * f[0] + (end[2] / W - foot2[1]) * f[1], yaw: wrapDeg(cur.yaw) });
        if (!seen.alive) { notes.push(`the worm died in the ${kind}`); break; }
      }
      if (!tries.length) throw new ToolError(`no try was made: ${notes.join("; ")}`);
      const v = riserVerdict(footY, Number.isNaN(topY) ? footY + 1e6 : topY, tries);
      const u = (x: number) => r2(x / k);
      return { worm: `WORM${w.slot}`, foot: [u(foot2[0]), u(footY), u(foot2[1])], heading: r2(wrapDeg(want)), top: Number.isNaN(topY) ? null : u(topY), riserAt: Number.isNaN(riserAt) ? null : u(riserAt),
        riserHeight: Number.isNaN(topY) ? null : u(v.riserHeight), tries: v.tries.map((t, i) => ({ ...t, maxRise: u(t.maxRise), endRise: u(t.endRise), moved: u(t.moved), ...(t.toward !== undefined ? { toward: u(t.toward) } : {}), endAt: tries[i].endAt.map(u) })),
        climbed: Number.isNaN(topY) ? null : v.climbed, highestReached: u(v.highestReached), ...(Number.isNaN(topY) ? {} : { shortBy: u(v.shortBy) }), units: a.units ?? "map", ...(notes.length ? { notes } : {}),
        note: "each try starts from the same spot (parked again); toward: how far it ended along the heading (under 0: away from the riser); yaw: the worm's facing at the try's start; climbed: the worm rests within 0.3 of the top; highestReached under riserHeight with climbed false: no way up on foot here" };
    },
  },
  {
    name: "game_start_level",
    description: "Start an INSTALLED pack level in the running game, in a named game style, without the menus: the level a map pack registered (its built files, its one mesh bank and its pinned time of day load as they do from the Versus screen: a Test of the project loads none of them) is armed and a Quick Game of `teams` started on it (level.test {level}, Melange 0.15+), and `scheme` (a style's title or key as a plugin's schemes file gives it: \"Sandworms\", or \"FETXT.Scheme.Sandworms\") is set in the match: its Name and its plain fields (TurnTime, RoundTime, SuddenDeath, WaterSpeed, ...) are written into GM.SchemeData when the match VM is made, before the level's set-up reads them, so a plugin that keys on the style (Sandtide plays only in Sandworms) runs unpatched. The style's per-weapon ammo and delays are NOT set (the Quick Game's weapons play: game_give_weapons gives what a check needs), and the map list's picture is not seen on this route. `modId` and `level` name the level (<modId>_<level>, or `level` alone as the full key levels_live lists). The game must be at its main menu (offline). Waits for the match and answers the states, what the match applied (time of day, the style read back from GM.SchemeData, the sim mods loaded) and the mesh banks' line of the game's log.",
    inputSchema: { type: "object", additionalProperties: false, required: ["level"], properties: {
      modId: { type: "string" }, level: { type: "string" },
      scheme: { type: "string", description: "A game style's title or key (searched in the plugins' schemes files under gameDir\\Mods)." },
      teams: { type: "array", minItems: 2, maxItems: 4, items: { type: "object", additionalProperties: false, properties: { human: { type: "boolean" }, worms: { type: "integer", minimum: 1, maximum: 4 } } } },
      gameDir: { type: "string", description: "The game's folder, to find the style's file and the log (default MELANGE_GAME_DIR)." },
      timeoutSec: { type: "integer", minimum: 5, maximum: 300 },
    } },
    async handler(s: Session, a: { modId?: string; level: string; scheme?: string; teams?: { human?: boolean; worms?: number }[]; gameDir?: string; timeoutSec?: number }) {
      const conn = await s.ensure(), client = await s.client();
      if (!conn.methods.includes("level.test")) throw new ToolError("no level.test here: the game must run, at its main menu (Melange.exe --serve has no match)");
      const key = a.modId && !a.level.includes(".") ? `Multi.${a.level.startsWith(`${a.modId}_`) ? a.level : `${a.modId}_${a.level}`}` : a.level;
      const game = a.gameDir ?? process.env.MELANGE_GAME_DIR;
      // the style: a plugin's schemes file whose title or key matches
      let scheme: { name: string; fields: Record<string, number | boolean>; from: string; weapons: number } | undefined;
      if (a.scheme !== undefined) {
        if (!game || !existsSync(join(game, "Mods"))) throw new ToolError("scheme: give gameDir (or set MELANGE_GAME_DIR) to find the style's file under its Mods");
        const want = a.scheme.toLowerCase(), seen: string[] = [];
        for (const id of readdirSync(join(game, "Mods"))) {
          const sp = join(game, "Mods", id, "spice.json");
          if (id.startsWith(".") || !existsSync(sp)) continue;
          let files: string[] = [];
          try { files = ((JSON.parse(readFileSync(sp, "utf8").replace(/^﻿/, "")) as { schemes?: unknown[] }).schemes ?? []).map((x) => (typeof x === "string" ? x : (x as { file?: string })?.file ?? "")).filter(Boolean); } catch { continue; }
          for (const f of files) {
            let j: { key?: string; title?: string; fields?: Record<string, unknown>; weapons?: Record<string, unknown> };
            try { j = JSON.parse(readFileSync(join(game, "Mods", id, f), "utf8").replace(/^﻿/, "")); } catch { continue; }
            if (typeof j.key !== "string") continue;
            seen.push(`${j.title ?? "?"} (${j.key}, ${id})`);
            if (j.key.toLowerCase() === want || (j.title ?? "").toLowerCase() === want || j.key.toLowerCase().endsWith(`.${want}`)) {
              const fields: Record<string, number | boolean> = {};
              for (const [n, v] of Object.entries(j.fields ?? {})) if ((typeof v === "number" || typeof v === "boolean") && /^[A-Za-z][A-Za-z0-9]{0,31}$/.test(n) && n !== "Name") fields[n] = v;
              scheme = { name: j.key, fields, from: `${id}/${f}`, weapons: Object.keys(j.weapons ?? {}).length };
            }
          }
        }
        if (!scheme) throw new ToolError(`no game style '${a.scheme}' in the plugins of ${game} (found: ${seen.join(", ") || "none"}); is its plugin installed?`);
      }
      const t0 = Date.now(), states: { state: string; ms: number }[] = [];
      const started = await client.call<{ key: string; state: string; packLevel?: boolean; scheme?: string }>("level.test", { level: key, ...(a.teams ? { teams: a.teams } : {}), ...(scheme ? { scheme: { name: scheme.name, fields: scheme.fields } } : {}) }, 120000)
        .catch((e: Error) => {
          if (/give project/.test(e.message)) throw new ToolError(`this game's level.test takes no pack level (Melange 0.15+): ${e.message}`);
          throw new ToolError(`level.test refused ${key}: ${e.message}`);
        });
      states.push({ state: started.state, ms: Date.now() - t0 });
      // the match: the scene turns to match and a turn starts
      const limit = (a.timeoutSec ?? 90) * 1000;
      let st = await gameState(s).catch(() => null);
      while (!(st && st.scene === "match" && st.turn !== undefined) && Date.now() - t0 < limit) { await sleep(500); st = await gameState(s).catch(() => null); }
      if (!st || st.scene !== "match") throw new ToolError(`${key} did not reach a match within ${Math.round(limit / 1000)} s (the game is at ${st?.scene ?? "?"}): is the pack built and enabled (levels_live)?`);
      states.push({ state: "playing", ms: Date.now() - t0 });
      const applied = await client.call<{ key: string; tod: unknown; scheme?: { name: string; applied: boolean; error: string }; simMods?: string[] }>("level.test.applied", {}, 15000).catch(() => null);
      // the style as the match holds it, read in the match VM once it ticks
      let read: Record<string, string> | undefined;
      if (scheme) {
        const names = ["Name", ...Object.keys(scheme.fields)];
        for (let i = 0; i < 20 && !read; i++) {
          try {
            const t = await tickLua(s, `local q = QueryContainer("GM.SchemeData") return ${names.map((n) => `"${n}=" .. tostring(q.${n})`).join(' .. ";" .. ')}`);
            read = Object.fromEntries(t.split(";").map((x) => { const j = x.indexOf("="); return [x.slice(0, j), x.slice(j + 1)]; }));
          } catch { await sleep(500); }
        }
      }
      // the mesh banks this launch loaded, from the game's log
      let banks: string | undefined, pin: string | undefined;
      if (game && existsSync(join(game, "Melange", "Melange.log"))) {
        const lines = readFileSync(join(game, "Melange", "Melange.log"), "utf8").split(/\r?\n/);
        banks = lines.filter((l) => /\[meshes\] \d+ bank\(s\) loaded/.test(l)).pop()?.replace(/^.*\[meshes\] /, "");
        pin = lines.filter((l) => l.includes("time of day pinned") && l.includes(key.replace(/^Multi\./, ""))).pop()?.replace(/^.*\[levels\] /, "");
      }
      const styleOk = !scheme || (read?.Name === scheme.name);
      return { key, started: started.state, states, turn: st.turn, teams: a.teams ?? "the Quick Game's (a human and a computer team)",
        ...(scheme ? { scheme: { name: scheme.name, from: scheme.from, set: scheme.fields, applied: applied?.scheme ?? null, readBack: read ?? null, ok: styleOk,
          notSet: `${scheme.weapons} weapon entries (ammo and delays): the Quick Game's weapons play; game_give_weapons gives what a check needs` } } : {}),
        applied: applied ? { tod: applied.tod, simMods: applied.simMods } : null, ...(pin ? { timeOfDayPin: pin } : {}), ...(banks ? { meshBanks: banks } : {}),
        ok: styleOk, note: "an installed pack level in a Quick Game: its built level, mesh bank and pinned time of day load as from the menus; the map list's picture and the style's weapon table are not on this route" };
    },
  },
  {
    name: "game_shot_compare",
    description: "Catch a look that depends on which plugins are on: one pose shot per plugin set, kept across game launches, and diffed. Plugins load at launch, so a compare is several launches (test_plugins {only: [...]} between them): each call takes the pose (`cam`: {pos, target, fov} in `units`) in the running match, files the picture under `name` with the plugins that are on now (mods.list), and compares it with the picture of every other plugin set filed under the same name: the mean difference per channel, the share of pixels that differ and their box, and a picture of them (<name>__<setA>__vs__<setB>.png: the first dimmed, the changed pixels magenta). The files are in Documents\\Melange\\erg-mcp\\shot-compare. Shoot after the same number of turns in each launch and with the camera on scenery, not on worms or the sea, which move. `list: true` answers the sets filed under the name; `clear: true` forgets them. Dustcrown's boulders drew differently with Kindjal on: lint_mesh_banks says which banks move, this shows what changed.",
    inputSchema: { type: "object", additionalProperties: false, required: ["name"], properties: {
      name: { type: "string", description: "The compare's name (letters, digits, - and _)." },
      cam: { type: "object", additionalProperties: false, required: ["pos", "target"], properties: { pos: { type: "array", items: { type: "number" }, minItems: 3, maxItems: 3 }, target: { type: "array", items: { type: "number" }, minItems: 3, maxItems: 3 }, fov: { type: "number", minimum: 5, maximum: 150 } } },
      set: { type: "string", description: "A label for the plugin set (default the ids that are on, joined with +)." },
      width: { type: "integer", minimum: 160, maximum: 1920 }, threshold: { type: "integer", minimum: 1, maximum: 765 },
      list: { type: "boolean" }, clear: { type: "boolean" }, images: { type: "boolean" },
      units: unitsProp,
    } },
    async handler(s: Session, a: { name: string; cam?: { pos: number[]; target: number[]; fov?: number }; set?: string; width?: number; threshold?: number; list?: boolean; clear?: boolean; images?: boolean; units?: "map" | "world" }) {
      if (!/^[A-Za-z0-9_-]{1,48}$/.test(a.name)) throw new ToolError("name: letters, digits, - and _ (at most 48)");
      const dir = shotDir(), idx = join(dir, `${a.name}.json`);
      type Entry = { set: string; mods: string[]; file: string; at: string; width: number; height: number; cam: { pos: number[]; target: number[]; fov?: number } };
      const load = (): Entry[] => (existsSync(idx) ? (JSON.parse(readFileSync(idx, "utf8")) as { sets: Entry[] }).sets : []);
      if (a.clear) { if (existsSync(idx)) writeFileSync(idx, JSON.stringify({ sets: [] })); return { name: a.name, cleared: true }; }
      if (a.list) return { name: a.name, sets: load().map(({ set, mods, file, at }) => ({ set, mods, file, at })), folder: dir };
      if (!a.cam) throw new ToolError("give cam ({pos, target}), or list / clear");
      if (!(await has(s, "render.camera")) || !(await has(s, "render.shot"))) throw new ToolError("game_shot_compare needs the game's render.camera and render.shot");
      const st = await gameState(s);
      if (st.scene !== "match") throw new ToolError(`no match is running (scene ${st.scene})`);
      const mods = await (await s.client()).call<{ id: string; on: boolean; state: string }[]>("mods.list", {}, 15000).then((l) => l.filter((m) => m.on && m.state === "enabled").map((m) => m.id).sort(), () => [] as string[]);
      const set = (a.set ?? (mods.join("+") || "none")).replace(/[^A-Za-z0-9_+.-]/g, "_").slice(0, 80);
      const k = a.units === "world" ? 1 : W, pose = { pos: a.cam.pos.map((v) => v * k), target: a.cam.target.map((v) => v * k), fov: a.cam.fov ?? 60 };
      const prev = load().filter((e) => e.set !== set);
      const same = prev.find((e) => JSON.stringify(e.cam) !== JSON.stringify(pose));
      if (same) throw new ToolError(`'${a.name}' was shot from another pose for the set ${same.set} (${JSON.stringify(same.cam)}): the same cam in every launch, or clear: true to start over`);
      const cs = await setCamera(s, pose, 40);
      const p = await shot(s, `cmp_${a.name}_${set}`.replace(/[^A-Za-z0-9_.-]/g, "_"), { clean: true, width: a.width ?? 960, minFrame: cs.frame !== undefined ? cs.frame + 3 : undefined });
      await clearCamera(s).catch(() => undefined);
      mkdirSync(dir, { recursive: true });
      const file = join(dir, `${a.name}__${set}.png`);
      writeFileSync(file, p.png);
      const mine: Entry = { set, mods, file, at: new Date().toISOString(), width: p.width, height: p.height, cam: pose };
      writeFileSync(idx, JSON.stringify({ sets: [...prev, mine] }, null, 1));
      const a0 = decodePng(p.png), parts: Content[] = [];
      const compares = prev.map((e) => {
        if (!existsSync(e.file)) return { against: e.set, error: "its picture is gone" };
        const b0 = decodePng(readFileSync(e.file));
        if (b0.w !== a0.w || b0.h !== a0.h) return { against: e.set, error: `its picture is ${b0.w} x ${b0.h}, this one ${a0.w} x ${a0.h}: the same width in every launch` };
        const d = imageDiff(b0.rgb, a0.rgb, a0.w, a0.h, a.threshold ?? 48);
        const diffFile = join(dir, `${a.name}__${e.set}__vs__${set}.png`), png = encodePng(a0.w, a0.h, d.rgb);
        writeFileSync(diffFile, png);
        if (a.images) parts.push(image(png));
        return { against: e.set, mods: e.mods, onlyHere: mods.filter((m) => !e.mods.includes(m)), onlyThere: e.mods.filter((m) => !mods.includes(m)), meanDiff: d.mean, changedShare: d.changed, box: d.box, diff: diffFile,
          verdict: d.changed < 0.002 ? "the same picture" : d.changed < 0.03 ? "a small part differs" : "the pictures differ" };
      });
      const out = { name: a.name, set, mods, shot: file, frame: p.frame, turn: st.turn, compares,
        ...(compares.length ? {} : { next: "this is the first set under this name: close the game, test_plugins {only: [...]} the next set, launch, and call it again with the same cam" }),
        note: "changedShare: the share of pixels whose channels differ by more than threshold in sum; worms, the sea and particles move between launches, so point the camera at what is compared" };
      return a.images ? { content: [image(p.png), ...parts, { type: "text", text: JSON.stringify(out) }] } : out;
    },
  },
  {
    name: "game_spawn_deals",
    description: "The spawn-area deals of a network game, peer by peer: each machine deals the teams over a level's spawn areas itself from the match's seed (spawns.mode areas; Melange logs `spawn areas: seed ..., N teams -> areas [...]` at each match start), so the peers of one match must log the same seed and the same areas, or their worms stand in different places and the match desyncs at its first turn. Reads the Melange.log of each peer (`gameDirs`: game folders, their Melange\\Melange.log; or `logs`: the files) and compares their last `last` deals (default 1), counted from each log's end. Answers per match whether the seeds and the areas agree, with each peer's line. With one log it lists that game's deals (two matches with different seeds should deal different areas). Read-only: the two-peer LocalNet game itself is started with the private harness (melange-private\\scripts\\e2e.ps1: config, launch, host, join), which erg-mcp does not drive.",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      gameDirs: { type: "array", minItems: 1, maxItems: 4, items: { type: "string" } },
      logs: { type: "array", minItems: 1, maxItems: 4, items: { type: "string" } },
      last: { type: "integer", minimum: 1, maximum: 50 },
    } },
    handler(_s: Session, a: { gameDirs?: string[]; logs?: string[]; last?: number }) {
      const files = [...(a.gameDirs ?? []).map((d) => join(d, "Melange", "Melange.log")), ...(a.logs ?? [])];
      if (!files.length) throw new ToolError("give gameDirs (the peers' game folders) or logs (their Melange.log files)");
      const peers = files.map((f) => {
        if (!existsSync(f)) throw new ToolError(`${f}: no such log`);
        return { name: f, deals: parseSpawnDeals(readFileSync(f, "utf8")) };
      });
      if (peers.length === 1) {
        const d = peers[0].deals.slice(-(a.last ?? 10));
        return { peer: peers[0].name, deals: d, count: peers[0].deals.length, distinctSeeds: new Set(d.map((x) => x.seed)).size, distinctDeals: new Set(d.map((x) => `${x.teams}:${x.areas.join(",")}`)).size,
          note: "one peer: its matches' deals in order; give two peers' logs to compare a network match" };
      }
      return { ...compareDeals(peers, a.last ?? 1), note: "agree: every peer logged the same seed and the same areas for the match; matches are counted from each log's end (fromEnd 1 is the newest)" };
    },
  },
];
