// Tool group "moves": test moves and checks on a running Test that the rounds of Dustcrown had to do by hand or could
// not do (round 3's gaps). game_face turns the active worm (a jetpack flies the way the worm faces: the SAND S flight
// went the wrong way); game_use_tool flies a jetpack to a point and says where the worm landed; game_riser_test tries a
// walk, a jump and a backflip at a riser and says how high the worm got; game_start_level starts an installed pack level
// in a named game style; game_shot_compare keeps a pose's shot per plugin set and diffs them across launches;
// game_spawn_deals reads the spawn-area deal each peer of a network game logged and says whether they agree. The pure
// parts are lib/harness-moves.ts. Positions are map units unless `units` says world.
import { execFileSync } from "node:child_process";
import { appendFileSync, existsSync, mkdirSync, readFileSync, readdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { aimAt, aimDir, bandGroups, compareDeals, comparePositions, facingOf, gameLine, girderHold, imageDiff, jetStep, landscapeIndex, localnetRoute, muzzleOf, parseSpawnDeals, riserVerdict, solveHook, turnPulse, versusRoute, wrapDeg, yawToward, type BandSurface, type FanHit, type RiserTry, type VersusTeam } from "../lib/harness-moves";
import { landBounds, heightLine } from "../lib/height-line";
import { nodeAt, surfaces } from "../lib/reach";
import { extentXZ, waterMap } from "../lib/inspect-field";
import { grids, isSolid } from "../lib/voxel";
import { decodePng, encodePng } from "../lib/inspect-png";
import { colourReads, pixelReads } from "../lib/exposed-core";
import { documentsDir, open as openOasis, urlFile } from "../oasis";
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

/** The game's data values by a name prefix (state.vars, at most one call a second: the calls are spaced here). */
let lastVars = 0;
async function gameVars(s: Session, prefix: string): Promise<Record<string, unknown>> {
  const wait = lastVars + 1100 - Date.now();
  if (wait > 0) await sleep(wait);
  const r = await (await s.client()).call<{ name: string; value: unknown }[]>("state.vars", { prefix }, 15000).finally(() => { lastVars = Date.now(); });
  return Object.fromEntries(r.map((v) => [v.name, v.value]));
}

/** The rope's length, world units (Ninja.MaxLength, Data\Tweak\TWEAK.XOM). */
const ROPE_LENGTH = 450;
/** Where the rope leaves a worm, over its feet (world units): the hook lands where a ray from here at the aim meets land. */
const MUZZLE_UP = 0.5 * W;

/** game_use_tool {weapon: "girder"}: the girder steered to a point and set down. Picking the girder puts the game in its
 * UtilityGirder control group, where the keys move the girder (Girder.Move.*.Pressed / .Released), lift it (Girder.Raise,
 * Girder.Lower) and set it down (Girder.Place); the drive posts those as a key does. The girder's place is the game's
 * own Girder.Position (state.vars, read once a second), so the steering is closed: two short pulses measure what Forward
 * and Right move it by (they follow the girder camera), then each step holds the inputs for as long as the rest of the
 * way takes. Round 3 sent the same messages from the match VM and nothing moved: they are the input service's. */
async function useGirder(s: Session, a: { target: number[]; worm?: number; arrive?: number; waitSec?: number; maxSec?: number; units?: "map" | "world"; girder?: { place?: boolean; lift?: number } }) {
  for (const m of ["input.drive", "state.vars", "level.test.landRay"]) if (!(await has(s, m))) throw new ToolError(`game_use_tool {weapon: "girder"} needs the game's ${m} (Test with this branch's Melange)`);
  const k = a.units === "world" ? 1 : W, out = (p: number[]) => p.map((v) => r2(v / k));
  const target = a.target.map((v) => v * k), tol = (a.arrive ?? 0.6 * W / k) * k, lift = (a.girder?.lift ?? 0) * k;
  const w = await turnOf(s, a.worm, a.waitSec ?? 60);
  const sel = await selectWeapon(s, "kUtilityGirder");
  await sleep(500);
  const groups = await evalClient(s, `return table.concat(wum.input.groups() or {}, ",")`).catch(() => "");
  if (!/UtilityGirder/.test(groups)) throw new ToolError(`the girder was not taken (the control groups are ${groups || "unknown"}, not UtilityGirder): the scheme may give none (game_give_weapons {kit: "movement"}), or the turn is not this worm's`);
  const read = async () => {
    const v = await gameVars(s, "Girder.");
    const p = v["Girder.Position"];
    if (!Array.isArray(p) || p.length !== 3) throw new ToolError("the game answers no Girder.Position");
    return { pos: p as number[], placed: Number(v["Girder.TotalNumberPlaced"] ?? 0) };
  };
  const hold = async (inputs: string[], ms: number) => { await drive(s, inputs, ms); await sleep(ms + 150); };
  const t0 = Date.now(), maxMs = (a.maxSec ?? 40) * 1000, steps: Record<string, unknown>[] = [], notes: string[] = [];
  let g = await read();
  const start = g, placed0 = g.placed, maxDist = Number((await gameVars(s, "Weapon.Girder."))["Weapon.Girder.MaxDistance"] ?? 300);
  // the two axes, per millisecond in plan, from a pulse each (the rest of a pulse's move is kept)
  const PULSE = 300;
  const pulse = async (input: string) => { const b = g.pos; await hold([input], PULSE); g = await read(); return [(g.pos[0] - b[0]) / PULSE, (g.pos[2] - b[2]) / PULSE, (g.pos[1] - b[1]) / PULSE]; };
  let fAx = await pulse("Girder.Move.Forward"), rAx = await pulse("Girder.Move.Right");
  if (Math.hypot(fAx[0], fAx[1]) < 1e-5 || Math.hypot(rAx[0], rAx[1]) < 1e-5) {
    // a girder held at its range's edge does not move outward: measure the other way
    const fb = await pulse("Girder.Move.Backward"), rl = await pulse("Girder.Move.Left");
    if (Math.hypot(fAx[0], fAx[1]) < 1e-5) fAx = fb.map((v) => -v);
    if (Math.hypot(rAx[0], rAx[1]) < 1e-5) rAx = rl.map((v) => -v);
  }
  let upRate = 0, why = "maxSec";
  while (Date.now() - t0 < maxMs) {
    const ex = target[0] - g.pos[0], ez = target[2] - g.pos[2], ey = target[1] + lift - g.pos[1];
    steps.push({ at: out(g.pos), offPlan: r2(Math.hypot(ex, ez) / k), offHeight: r2(ey / k) });
    if (Math.hypot(ex, ez) <= tol && Math.abs(ey) <= tol) { why = "at the target"; break; }
    const now = await active(s);
    if (!now || now.slot !== w.slot || now.phase !== "play") { why = "the turn left play"; break; }
    if (Math.hypot(ex, ez) > tol) {
      const h = girderHold([ex, ez], fAx, rAx);
      if (!h) { why = "the girder's two moves are parallel: it cannot be steered"; break; }
      const before = g.pos;
      // one axis at a time, at most 2.5 s a hold: the game's own speed, re-read after each
      const fm = Math.max(-2500, Math.min(2500, h.forwardMs)), rm = Math.max(-2500, Math.min(2500, h.rightMs));
      if (Math.abs(fm) >= 30) await hold([fm > 0 ? "Girder.Move.Forward" : "Girder.Move.Backward"], Math.abs(fm));
      if (Math.abs(rm) >= 30) await hold([rm > 0 ? "Girder.Move.Right" : "Girder.Move.Left"], Math.abs(rm));
      g = await read();
      if (Math.hypot(g.pos[0] - before[0], g.pos[2] - before[2]) < 0.02 * W && Math.abs(fm) + Math.abs(rm) > 200) { why = `the girder does not move further (${r2(Math.hypot(g.pos[0] - w.pos[0], g.pos[2] - w.pos[2]) / W)} map units from the worm; Weapon.Girder.MaxDistance is ${r2(maxDist / W)})`; break; }
      continue;
    }
    // the height: Raise and Lower, their rate from the first pulse
    const dir = ey > 0 ? "Girder.Raise" : "Girder.Lower", by = g.pos[1];
    const ms = upRate > 1e-6 ? Math.max(60, Math.min(2500, Math.abs(ey) / upRate)) : 300;
    await hold([dir], ms);
    g = await read();
    const moved = Math.abs(g.pos[1] - by);
    if (moved < 0.01 * W) { notes.push(`${dir} did not move the girder's height (it rests on what is under it): placed at the height it has`); steps.push({ at: out(g.pos), offPlan: r2(Math.hypot(target[0] - g.pos[0], target[2] - g.pos[2]) / k), offHeight: r2((target[1] + lift - g.pos[1]) / k) }); why = "at the target in plan"; break; }
    upRate = moved / ms;
  }
  await drive(s, [], 0).catch(() => undefined);
  let placed = false, top: number | null = null;
  if (a.girder?.place !== false && /^at the target/.test(why)) {
    await hold(["Girder.Place"], 200);
    await sleep(900);
    const after = await gameVars(s, "Girder.");
    placed = Number(after["Girder.TotalNumberPlaced"] ?? 0) > placed0;
    if (!placed) notes.push("Girder.Place was sent and the game's count of girders did not rise: the spot is refused (the girder drew red: inside land or a worm, or with nothing to rest on)");
    const [hit] = await rays(s, [[g.pos[0], g.pos[1] + 6 * W, g.pos[2], g.pos[0], g.pos[1] - 6 * W, g.pos[2]]]);
    top = hit ? hit[1] : null;
  }
  const off = Math.hypot(g.pos[0] - target[0], g.pos[1] - target[1] - lift, g.pos[2] - target[2]);
  return { weapon: "girder", worm: `WORM${w.slot}`, selected: sel.name ?? sel.weapon, from: out(start.pos), target: out(target), at: out(g.pos), offTarget: r2(off / k), placed, reached: placed && off <= tol * 1.5,
    ...(top !== null ? { topAfter: r2(top / k) } : {}), stop: why, seconds: r2((Date.now() - t0) / 1000),
    steering: { forwardPerSec: [r2((fAx[0] * 1000) / k), r2((fAx[1] * 1000) / k)], rightPerSec: [r2((rAx[0] * 1000) / k), r2((rAx[1] * 1000) / k)], ...(upRate ? { raisePerSec: r2((upRate * 1000) / k) } : {}) },
    wormDistance: r2(Math.hypot(g.pos[0] - w.pos[0], g.pos[2] - w.pos[2]) / k), maxDistance: r2(maxDist / k), steps: steps.slice(-12), units: a.units ?? "map",
    ...(notes.length ? { notes } : {}),
    note: "the girder is steered by its own inputs (Girder.Move.Forward / Backward / Left / Right, Girder.Raise / Lower: the input service's messages while the girder is held) to the game's own Girder.Position, read once a second, and set down with Girder.Place; placed: the game's Girder.TotalNumberPlaced rose. A shot shows it (game_shots {cams: [{pos, target}]})" };
}

/** The multi levels of the enabled packs of a game folder: their mod, slug and title (the landscape list shows titles). */
function packLevels(game: string): { mod: string; slug: string; title: string }[] {
  const titles: { mod: string; slug: string; title: string }[] = [];
  let enabled: Record<string, boolean> = {};
  try { enabled = JSON.parse(readFileSync(join(game, "Mods", "thumper-state.json"), "utf8")).enabled ?? {}; } catch { /* no state: every mod counts */ }
  for (const id of readdirSync(join(game, "Mods"))) {
    const sp = join(game, "Mods", id, "spice.json");
    if (id.startsWith(".") || !existsSync(sp) || enabled[id] === false) continue;
    try { for (const l of (JSON.parse(readFileSync(sp, "utf8").replace(/^\uFEFF/, "")) as { levels?: { slug?: string; title?: string; type?: string }[] }).levels ?? []) if (l.title && (l.type ?? "multi") === "multi") titles.push({ mod: id, slug: l.slug ?? "", title: l.title }); } catch { /* not a manifest */ }
  }
  return titles;
}

/** The process id of the WormsMayhem.exe that runs from a game folder, or 0. */
function gamePid(game: string): number {
  try {
    const out = execFileSync("powershell", ["-NoProfile", "-Command", "Get-CimInstance Win32_Process -Filter \"Name='WormsMayhem.exe'\" | ForEach-Object { \"$($_.ProcessId)|$($_.ExecutablePath)\" }"], { encoding: "utf8", timeout: 20000 });
    const want = join(game, "WormsMayhem.exe").toLowerCase().replace(/\//g, "\\");
    const hit = out.split(/\r?\n/).map((l) => l.split("|")).find((v) => (v[1] ?? "").toLowerCase().replace(/\//g, "\\") === want);
    return hit ? +hit[0] : 0;
  } catch { return 0; }
}

/** Whether a game folder's Melange.ini turns the private build's Automation queue on. */
function automationOn(game: string): boolean {
  let ini = "";
  try { ini = readFileSync(join(game, "Melange.ini"), "utf8"); } catch { return false; }
  return /^\s*Enabled\s*=\s*1/m.test(/\[Automation\]([^[]*)/.exec(ini)?.[1] ?? "");
}

/** Automation commands queued to a game (its Melange\automation.<pid>.txt) and waited for: the log lines since. */
async function automate(game: string, pid: number, cmds: string[], timeoutMs: number): Promise<{ done: boolean; lines: string[] }> {
  const log = join(game, "Melange", "Melange.log"), marker = `ergauto-${Date.now().toString(36)}${Math.floor(Math.random() * 1e4)}`;
  const from = existsSync(log) ? readFileSync(log, "utf8").split(/\r?\n/).length : 0;
  appendFileSync(join(game, "Melange", `automation.${pid}.txt`), [...cmds, `log ${marker}`].join("\n") + "\n");
  const t0 = Date.now();
  for (;;) {
    await sleep(1000);
    const lines = readFileSync(log, "utf8").split(/\r?\n/).slice(from);
    if (lines.some((l) => l.includes(`LOG ${marker}`))) return { done: true, lines };
    if (Date.now() - t0 > timeoutMs) return { done: false, lines };
  }
}

interface RopePlan { aimSec?: number; hook?: number[]; solve?: boolean | { over?: number; reach?: number }; releaseOver?: boolean; release?: boolean; refire?: number; steps?: { do: "shorten" | "lengthen" | "swingForward" | "swingBackward" | "swingLeft" | "swingRight" | "wait" | "jump"; sec?: number; until?: "still" }[] }
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
  // a solved aim (hook or solve): the aim is written, not swept, and the steps default to a climb that lets go over the ledge
  const solved = plan.hook !== undefined || !!plan.solve;
  if (plan.hook !== undefined && plan.solve) throw new ToolError("rope: give hook (the point to hook) or solve (find it), not both");
  if (solved && plan.aimSec !== undefined) throw new ToolError("rope: aimSec is the swept aim; with hook or solve the aim is solved");
  const steps = plan.steps ?? (solved ? [{ do: "shorten" as const, sec: 6, until: "still" as const }] : [{ do: "shorten" as const, sec: 4, until: "still" as const }, { do: "swingForward" as const, sec: 0.8 }]);
  const releaseOver = plan.releaseOver ?? solved;
  const w = await turnOf(s, a.worm, a.waitSec ?? 60);
  // the worm turns first: a turn after the pick left the rope unfired (round 5)
  const want = yawToward([w.pos[0], w.pos[2]], plan.hook ? [plan.hook[0] * k, plan.hook[2] * k] : [target[0], target[2]]);
  const faced = want === null ? null : await face(s, want, solved ? 4 : 15, 8000);
  const sel = await selectWeapon(s, "kUtilityNinjaRope");
  await sleep(300);
  const here = async () => (await active(s))?.pos ?? null;
  const msg = (m: string) => tickLua(s, `SendMessage("${m}") return "sent"`);
  const shots: Record<string, unknown>[] = [], notes: string[] = [];
  let pos = w.pos, why = "done";
  /** The aim solved for a shot from `p`: the pitch written into the worm's WeaponAngle, and where the hook should land. */
  const solveAim = async (p: number[]): Promise<{ pitch: number; hook: number[] | null; how: string } | { fail: string }> => {
    const now = await active(s), yaw = now?.yaw ?? want ?? 0, mz = muzzleOf(p, MUZZLE_UP);
    const ray = async (pitch: number) => { const d = aimDir(yaw, pitch); return (await rays(s, [[...mz, mz[0] + d[0] * ROPE_LENGTH, mz[1] + d[1] * ROPE_LENGTH, mz[2] + d[2] * ROPE_LENGTH]]))[0]; };
    if (plan.hook) {
      const at = plan.hook.map((v) => v * k), aim = aimAt(mz, at);
      if (aim.distance > ROPE_LENGTH) return { fail: `the hook point is ${r2(aim.distance / k)} from the worm, further than the rope (${r2(ROPE_LENGTH / k)})` };
      return { pitch: aim.pitch, hook: await ray(aim.pitch), how: "aimed at the hook point" };
    }
    // a fan of rays at the worm's own yaw, from the target's elevation up, a degree apart
    // and the same fan from just over its feet: the line the worm itself is pulled along
    const lo = aimAt(mz, target).pitch, fan: FanHit[] = [], body: FanHit[] = [], segs: number[][] = [], low: number[][] = [], pitches: number[] = [], ft = [p[0], p[1] + 0.1 * W, p[2]];
    for (let deg = Math.max(-20, Math.floor((lo * 180) / Math.PI)); deg <= 88; deg++) {
      const pt = (deg * Math.PI) / 180, d = aimDir(yaw, pt);
      pitches.push(pt);
      segs.push([...mz, mz[0] + d[0] * ROPE_LENGTH, mz[1] + d[1] * ROPE_LENGTH, mz[2] + d[2] * ROPE_LENGTH]);
      low.push([...ft, ft[0] + d[0] * ROPE_LENGTH, ft[1] + d[1] * ROPE_LENGTH, ft[2] + d[2] * ROPE_LENGTH]);
    }
    (await rays(s, segs)).forEach((at, i) => fan.push({ pitch: pitches[i], at }));
    (await rays(s, low)).forEach((at, i) => body.push({ pitch: pitches[i], at }));
    const so = typeof plan.solve === "object" ? plan.solve : {};
    const r = solveHook(mz, target, fan, { over: (so.over ?? 0.75 * W / k) * k, reach: (so.reach ?? 4 * W / k) * k, lipBack: 0.5 * W }, body);
    if (!r.hit) return { fail: r.why };
    return { pitch: r.hit.pitch, hook: r.hit.at, how: `the lowest ray that meets land over the ledge: ${r2(r.height / k)} over the target, ${r2(r.plan / k)} from it in plan` };
  };
  for (let shotN = 0; shotN <= (plan.refire ?? 0); shotN++) {
    const from = pos;
    let aim: { pitch: number; hook: number[] | null; how: string } | null = null;
    if (solved) {
      const r = await solveAim(pos);
      if ("fail" in r) { why = `no hook: ${r.fail}`; shots.push({ from: out(from), solved: false, why: r.fail }); break; }
      aim = r;
      // the aim itself: the worm's own WeaponAngle, written at a sim tick (AimUp and AimDown move the same number)
      await tickLua(s, `local lock, c = EditContainer("Worm.Data${String(w.slot).padStart(2, "0")}") c.WeaponAngle = ${aim.pitch.toFixed(5)} CloseContainer(lock) return "aimed"`);
      await sleep(150);
    } else if ((plan.aimSec ?? 1.2) > 0 && shotN === 0) await drive(s, ["AimUp"], (plan.aimSec ?? 1.2) * 1000).then(() => sleep((plan.aimSec ?? 1.2) * 1000 + 100));   // the aim comes up from where it is
    // the utility's fire throws the hook
    await drive(s, ["FireUtil"], 250);
    await sleep(900);
    await drive(s, [], 0).catch(() => undefined);
    const hooked = await here();
    if (!hooked) { why = "the turn left play"; break; }
    const trace: { do: string; sec: number; at: number[] }[] = [];
    let maxY = hooked[1], letGo: number[] | null = null;
    // the timed release: over the ledge (higher than the target, and near it in plan) the rope is let go at once
    const overLedge = (p: number[]) => releaseOver && p[1] >= target[1] + 0.25 * W && Math.hypot(p[0] - target[0], p[2] - target[2]) <= Math.max(arrive, 2.5 * W);
    for (const st of steps) {
      if (letGo) break;
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
        if (overLedge(p)) { letGo = p; last = p; break; }
        const moved = Math.hypot(p[0] - last[0], p[1] - last[1], p[2] - last[2]);
        stillFor = moved < 0.05 * W ? stillFor + (Date.now() - lastT) : 0;
        last = p; lastT = Date.now();
        // still: not before the rope has had time to take up its slack (a long rope starts slowly)
        if (st.until === "still" && stillFor >= 600 && Date.now() - t0 >= Math.min(ms, 1500)) break;
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
    shots.push({ from: out(from), ...(aim ? { aim: { pitchDeg: r2((aim.pitch * 180) / Math.PI), how: aim.how, hook: aim.hook ? out(aim.hook) : null, ...(aim.hook ? {} : { warning: "no land along the aim within the rope's length: the hook finds nothing" }) } } : {}),
      ...(letGo ? { letGoAt: out(letGo) } : {}), afterFire: out(hooked), ...(rose < 0.05 * W && pulled < 0.05 * W && !aim ? { hooked: "the worm did not move at the shot: the hook may have found nothing (a higher or lower aim, or nearer the wall)" } : {}),
      steps: trace, maxY: r2(maxY / k), ...(plan.release !== false ? { landed: out(pos) } : { hangsAt: out(pos) }) });
    const now = await active(s);
    if (!now || now.slot !== w.slot || now.phase !== "play") { why = "the turn left play"; break; }
    if (!now.alive) { why = "the worm died"; break; }
  }
  const off = Math.hypot(pos[0] - target[0], pos[1] - target[1], pos[2] - target[2]);
  if (sel && (sel as { selected?: boolean }).selected === false) notes.push("the rope was not selected (the scheme may give none: game_give_weapons {kit: \"movement\"})");
  return { weapon: "rope", worm: w.slot, from: out(w.pos), target: out(target), ...(faced ? { faced: { yaw: faced.yaw, error: faced.error, ok: faced.ok } } : {}),
    ...(solved ? { aim: plan.hook ? "hook" : "solve" } : { aimSec: plan.aimSec ?? 1.2 }), shots, end: out(pos), climbed: r2((pos[1] - w.pos[1]) / k), offTarget: r2(off / k), reached: off <= arrive, stop: why, units: a.units ?? "map",
    ...(notes.length ? { notes } : {}),
    note: (solved ? "the aim is solved, not swept: the pitch from the worm's rope hand (half a map unit over its feet) to the hook is written into the worm's WeaponAngle at a sim tick, and the hook is where the game's land sweep along that aim meets land (hook); with solve the hook is the lowest ray of a fan toward the target that meets land over the ledge. The rope is let go as soon as the worm is over the ledge (letGoAt). Then it is" : "the rope is fired with the utility's fire after the aim was raised aimSec, then") + " worked by its own messages in the match VM (NinjaRope.Shorten / Lengthen / Swing*.On and .Off) and let go with NinjaRope.Kill; afterFire is where the worm hung once the hook held, landed where it rested. reached: within `arrive` of the target" };
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
    description: "Use a movement tool with the active worm of the running Test toward a target and answer where the worm landed: the in-game check of check_tool_reach. `weapon: \"jetpack\"`: the worm takes the jetpack (game_select_weapon: the scheme must give one, or game_give_weapons {kit: \"movement\"}), turns to the target (game_face), then flies: Input.FireUtil is the thrust and the walk inputs steer (Input.MoveForward and its three fellows, camera-relative: what the game accepts in flight), posted from the game's input tick and chosen again every 120 ms from the worm's place: a lob, because a jetpack only pushes while it thrusts: straight up until the worm is half of `clearance` (default 3 map units) over the target's height (a worm steered at a wall from its foot is held against it and burns its tank there), then thrust and steering together until the speed it has would carry it to the target as it falls, then no thrust: it coasts over and drops once within `arrive` (default 1.5) of it in plan. Thrust is capped at 7.3 s (a tank is 7.5). It ends when the worm rests, the turn leaves play, or `maxSec` (default 14; a full tank is about 7.5 s of thrust). `target`: [x, y, z], y the ground to land on. Answers the landing, its distance from the target in plan and in height, the highest point and the time: `reached` when it rests within `arrive` + 1.5 of the target and within 1.5 of its height. `weapon: \"rope\"`: the worm turns to the target, takes the ninja rope, raises its aim `rope.aimSec` and fires (the hook holds on what the aim meets: a wall's face, not open sky over its top), then works the rope by `rope.steps`, the rope's own messages sent in the match VM (shorten reels in, lengthen lets out, swing* swings; default: shorten until still, then swing forward), lets go and waits to rest; the answer's shots give where it hung after the fire, each step's end, the highest it was and where it landed, and `reached` when that is within `arrive` of the target. `weapon: \"girder\"`: the worm takes the girder and it is steered to `target` (where its middle goes; `girder.lift` holds it that far over the target's y) by its own inputs, Girder.Move.Forward / Backward / Left / Right and Girder.Raise / Lower held through input.drive, with the game's own Girder.Position read once a second (state.vars) to close the loop, then set down with Girder.Place (`girder.place: false` only steers). It answers where the girder is, `placed` (the game's count of girders rose; false when the spot is refused: the girder draws red inside land or a worm), the top of the ground there afterwards and the steering it measured. The game's girder is a platform about 4.5 x 4.5 map units that floats where it is put, within Weapon.Girder.MaxDistance (15 map units) of the worm; no input turns it. With `rope.hook` or `rope.solve` the rope's aim is solved instead of swept. `weapon: \"teleport\"` is refused: this game has no teleport weapon.",
    inputSchema: { type: "object", additionalProperties: false, required: ["weapon", "target"], properties: {
      weapon: { type: "string", enum: ["jetpack", "teleport", "rope", "girder"] },
      target: { type: "array", items: { type: "number" }, minItems: 3, maxItems: 3 },
      rope: { type: "object", additionalProperties: false, description: "weapon rope: how it is driven. aimSec: how long the aim is raised from level before the shot (Input.AimUp held; default 1.2: steeply up at a wall a couple of units away). steps: what the worm does on the rope, in order, each {do: shorten | lengthen | swingForward | swingBackward | swingLeft | swingRight | wait | jump (the jump key with the walk-forward key, held sec: a hop off the rope), sec (at most 6), until: \"still\" (stop early once the worm has not moved 0.15 map units in 0.6 s)}: the rope's own messages, NinjaRope.Shorten.On / .Off and the rest, sent in the match VM. Default: shorten until still (at most 4 s), swing forward 0.8 s. release (default true): let go at the end (NinjaRope.Kill) and wait for the worm to rest. refire: after the release, fire again and run the steps again this many times (default 0; a scheme gives a rope several shots), each from where the worm then is.",
        properties: { aimSec: { type: "number", minimum: 0, maximum: 5 }, release: { type: "boolean" }, refire: { type: "integer", minimum: 0, maximum: 5 },
          hook: { type: "array", items: { type: "number" }, minItems: 3, maxItems: 3, description: "A solved aim instead of aimSec: the point the hook should land on [x, y, z] (the wall under a lip, an overhang, the next riser's face). The worm turns to it, the pitch from its rope hand to the point is written into its own aim (the worm container's WeaponAngle, the number AimUp and AimDown move) and the shot's answer gives `aim`: the pitch and where the game's land sweep along that aim meets land (the hook as the game will find it)." },
          solve: { description: "A solved aim with the hook found too: true, or {over (how far over the target's height the hook must be; default 0.75 map units), reach (how far from the target in plan; default 4 map units)}. A fan of rays from the worm toward the target, a degree apart from the target's elevation up, each as long as the rope; the hook is the lowest ray that meets land over the ledge and not short of it (a hook on the riser's own face hangs the worm on the face). When none does the answer says why (stand further back, nothing over the ledge).",
            anyOf: [{ type: "boolean" }, { type: "object", additionalProperties: false, properties: { over: { type: "number", minimum: 0 }, reach: { type: "number", minimum: 0.1 } } }] },
          releaseOver: { type: "boolean", description: "Let the rope go the moment the worm is over the ledge: higher than the target by a quarter map unit and within `arrive` (at least 2.5 map units) of it in plan, whatever step is running (default true with hook or solve, else false)." },
          steps: { type: "array", maxItems: 16, items: { type: "object", additionalProperties: false, required: ["do"], properties: {
            do: { type: "string", enum: ["shorten", "lengthen", "swingForward", "swingBackward", "swingLeft", "swingRight", "wait", "jump"] }, sec: { type: "number", minimum: 0.05, maximum: 6 }, until: { type: "string", enum: ["still"] } } } } } },
      girder: { type: "object", additionalProperties: false, description: "weapon girder: place (default true): set it down once it is at the target (false: steer only and answer where it is); lift: how far over the target's y the girder's own position is held (units; default 0: the target is where the girder's middle goes).",
        properties: { place: { type: "boolean" }, lift: { type: "number" } } },
      worm: { type: "integer", minimum: 0, maximum: 15 },
      clearance: { type: "number", minimum: 0.5, maximum: 40 }, arrive: { type: "number", minimum: 0.3, maximum: 20 }, maxSec: { type: "number", minimum: 2, maximum: 40 },
      waitSec: { type: "number", minimum: 1, maximum: 300 },
      units: unitsProp,
    } },
    async handler(s: Session, a: { weapon: "jetpack" | "teleport" | "rope" | "girder"; target: number[]; worm?: number; clearance?: number; arrive?: number; maxSec?: number; waitSec?: number; units?: "map" | "world"; rope?: RopePlan; girder?: { place?: boolean; lift?: number } }) {
      if (a.weapon === "rope") return useRope(s, a);
      if (a.weapon === "girder") return useGirder(s, a);
      // Worms Ultimate Mayhem has no teleport to fire: its weapon table (ids 1-68) holds kUtilityJetpack, kUtilityNinjaRope,
      // kUtilityGirder and kUtilityParachute and no teleport (level.test.selectWeapon refuses kUtilityTeleport and
      // kWeaponTeleport; the binary's Weapon.CreateTeleportCursor and the inventory's Teleport count are leftovers of
      // Worms 3D). A worm changes place without walking only by a telepad of the level or by the three tools above
      if (a.weapon === "teleport") throw new ToolError("this game has no teleport weapon: its weapon table holds no kUtilityTeleport or kWeaponTeleport (level.test.selectWeapon refuses both; the inventory's Teleport count and the binary's Weapon.CreateTeleportCursor are leftovers the game never uses), so a teleport cannot be tested or required of a map. What moves a worm without a walk is the jetpack, the rope and the girder (all driven here), and a level's own telepads (place_object {kind: \"telepad\"}); game_park_worm puts a worm on a spot for a check");
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
  {
    name: "game_line_clearance",
    description: "Where a height line (Sandtide's sand line) falls against the ground as the GAME reads it, in the running Test: check_height_line's in-game twin. The line is `y`, or a `fraction` of the land's box (`preset: \"sandtide\"`: 0.25) over its base computed from the game's own numbers (Land.MinBounds and Land.MaxBounds, the box the engine built, and the match's water: the base is the water when the land reaches under it, else the land's lowest point). Then the game's land sweep (level.test.landRay) reads every column of a lattice (`step`, default 1 map unit) over the land: the top of the ground, and under anything that stands over the band the surfaces inside it (a court under a roof, a ledge under an overhang: the sweep is restarted under each hit), each kept when a worm has room to stand on it (`headroom`, default 1.5 map units of air). Answers every standable surface within `band` (default 40 world units) of the line, grouped, with the nearest one's distance to the line above and below it, `clear` when none is in the band, and (with the project open) `tools`: check_height_line's own line and its count of surfaces in the band for the same lattice, and where the two disagree. Read-only; a Test only.",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      preset: { type: "string", enum: ["sandtide"] }, fraction: { type: "number", minimum: 0.01, maximum: 1 }, y: { type: "number", description: "A fixed line height (units)." },
      band: { type: "number", minimum: 0, description: "How close to the line counts (units; default 40 world = 2 map units)." },
      step: { type: "number", minimum: 0.25, maximum: 8, description: "Column spacing (units; default 1 map unit)." },
      headroom: { type: "number", minimum: 0.25, description: "Air a worm needs over a surface (units; default 1.5 map units)." },
      x0: { type: "number" }, z0: { type: "number" }, x1: { type: "number" }, z1: { type: "number" },
      limit: { type: "integer", minimum: 0, maximum: 200, description: "Groups listed (default 20), the nearest to the line first." },
      units: unitsProp,
    } },
    async handler(s: Session, a: { preset?: "sandtide"; fraction?: number; y?: number; band?: number; step?: number; headroom?: number; x0?: number; z0?: number; x1?: number; z1?: number; limit?: number; units?: "map" | "world" }) {
      for (const m of ["level.test.landRay", "state.vars"]) if (!(await has(s, m))) throw new ToolError(`game_line_clearance needs the game's ${m} (a running Test)`);
      if ([a.preset !== undefined, a.fraction !== undefined, a.y !== undefined].filter(Boolean).length !== 1) throw new ToolError("give one of preset (\"sandtide\"), fraction or y");
      const k = a.units === "world" ? 1 : W, o1 = (v: number) => r2(v / k), both = (v: number) => ({ map: r2(v / W), world: r2(v) });
      // the game's own land box and water
      const land = await gameVars(s, "Land."), st = await gameState(s);
      const lo = land["Land.MinBounds"] as number[] | undefined, hi = land["Land.MaxBounds"] as number[] | undefined;
      if (!Array.isArray(lo) || !Array.isArray(hi)) throw new ToolError("the game answers no Land.MinBounds / Land.MaxBounds: is a match playing?");
      const water = typeof st.water === "number" ? st.water : null;
      const gl = a.y !== undefined ? { line: a.y * k, base: NaN, baseKind: "given" as const } : gameLine(lo[1], hi[1], water, a.preset === "sandtide" ? 0.25 : a.fraction!);
      const band = (a.band ?? 40 / k) * k, step = (a.step ?? W / k) * k, headroom = (a.headroom ?? 1.5 * W / k) * k;
      const x0 = a.x0 !== undefined ? a.x0 * k : lo[0], x1 = a.x1 !== undefined ? a.x1 * k : hi[0], z0 = a.z0 !== undefined ? a.z0 * k : lo[2], z1 = a.z1 !== undefined ? a.z1 * k : hi[2];
      const cols: number[][] = [];
      for (let z = Math.min(z0, z1) + step / 2; z < Math.max(z0, z1); z += step) for (let x = Math.min(x0, x1) + step / 2; x < Math.max(x0, x1); x += step) cols.push([x, z]);
      if (!cols.length) throw new ToolError("the region is empty");
      if (cols.length > 90000) throw new ToolError(`${cols.length} columns is too many (at most 90000): a larger step or a smaller region`);
      const top = hi[1] + 5 * W, lineLo = gl.line - band, lineHi = gl.line + band;
      // pass 1: the top of every column
      const tops = await rays(s, cols.map(([x, z]) => [x, top, z, x, lineLo - 1, z]));
      const surf: BandSurface[] = [];
      let groundCols = 0, raysCast = cols.length;
      // pass 2: under what stands over the band, the surfaces inside it: the sweep restarted half a map unit under
      // each hit until it leaves the solid, and a surface kept when the air over it is headroom high
      const deeper: { x: number; z: number; from: number }[] = [];
      tops.forEach((h, i) => {
        if (!h) return;
        groundCols++;
        const [x, z] = cols[i];
        if (h[1] <= lineHi && h[1] >= lineLo && (water === null || h[1] > water)) surf.push({ x, z, y: h[1], covered: false });
        else if (h[1] > lineHi) deeper.push({ x, z, from: h[1] });
      });
      const STEP = 0.5 * W;
      let front = deeper.map((d) => ({ ...d, y: Math.min(d.from - STEP, lineHi + headroom), roof: d.from }));
      for (let pass = 0; pass < 80 && front.length; pass++) {
        const hits = await rays(s, front.map((f) => [f.x, f.y, f.z, f.x, lineLo - 1, f.z]));
        raysCast += front.length;
        const next: typeof front = [];
        front.forEach((f, i) => {
          const h = hits[i];
          if (!h) return;                                   // nothing more under it down to the band's floor
          if (f.y - h[1] < 0.02 * W) { if (f.y - STEP > lineLo) next.push({ ...f, y: f.y - STEP }); return; }   // started in solid: go on down
          // open air from f.y down to the hit: a floor under a roof
          if (h[1] <= lineHi && h[1] >= lineLo && f.y - h[1] >= headroom - STEP && (water === null || h[1] > water)) surf.push({ x: f.x, z: f.z, y: h[1], covered: true });
          if (h[1] - STEP > lineLo) next.push({ ...f, y: h[1] - STEP });
        });
        front = next;
      }
      const groups = bandGroups(surf, gl.line, 4 * W);
      const above = surf.filter((p) => p.y >= gl.line).map((p) => p.y - gl.line), below = surf.filter((p) => p.y < gl.line).map((p) => gl.line - p.y);
      // the tools' own reading for the same lattice, with the project open
      let tools: Record<string, unknown> | undefined;
      if (s.project) {
        const gs = grids(s.store).filter((g) => g.words.some(isSolid)), lb = landBounds(gs), ext = extentXZ(gs);
        if (lb && ext) {
          const hl = a.y !== undefined ? { line: (a.y * k) / W } : heightLine(lb, waterMap(s.store), { fraction: a.preset === "sandtide" ? 0.25 : a.fraction });
          const S = surfaces(gs, { x0: Math.min(x0, x1) / W, z0: Math.min(z0, z1) / W, x1: Math.max(x0, x1) / W, z1: Math.max(z0, z1) / W }, step / W, headroom / W);
          let n = 0;
          const own = new Set<string>();
          for (let i = 0; i < S.nodes; i++) { const p = nodeAt(S, i); if (Math.abs(p[1] - hl.line) <= band / W) { n++; own.add(`${Math.floor(p[0] / 4)},${Math.floor(p[2] / 4)}`); } }
          const game = new Set(surf.map((p) => `${Math.floor(p.x / W / 4)},${Math.floor(p.z / W / 4)}`));
          const onlyGame = [...game].filter((c) => !own.has(c)), onlyTools = [...own].filter((c) => !game.has(c));
          const cell = (c: string) => c.split(",").map((v) => o1((+v * 4 + 2) * W));
          tools = { line: both(hl.line * W), lineApart: both(gl.line - hl.line * W), surfacesInBand: n, groups: own.size,
            agrees: Math.abs(gl.line - hl.line * W) <= 0.05 * W && !onlyGame.length && !onlyTools.length,
            ...(onlyGame.length ? { onlyInGame: onlyGame.slice(0, 12).map(cell) } : {}), ...(onlyTools.length ? { onlyInTools: onlyTools.slice(0, 12).map(cell) } : {}),
            landBounds: { minY: both(lb.minY * W), maxY: both(lb.maxY * W) },
            note: "check_height_line's line and its surfaces within the band on the same lattice; lineApart: the game's line less the tools' (the game's land box against the project's frames); onlyInGame / onlyInTools: 4-unit cells where one reads a surface in the band and the other none" };
        }
      }
      const limit = a.limit ?? 20;
      return {
        clear: surf.length === 0,
        summary: surf.length ? `${surf.length} standable surface columns lie within ${o1(band)} of the line (${groups.length} groups): the nearest is ${o1(Math.min(...surf.map((p) => Math.abs(p.y - gl.line))))} from it` : `no standable surface lies within ${o1(band)} of the line`,
        line: both(gl.line), ...(a.y === undefined ? { base: { ...both(gl.base), kind: gl.baseKind }, fraction: a.preset === "sandtide" ? 0.25 : a.fraction } : {}),
        gameLand: { minY: both(lo[1]), maxY: both(hi[1]), water: water === null ? null : both(water) },
        band: both(band),
        nearest: { above: above.length ? both(Math.min(...above)) : null, below: below.length ? both(Math.min(...below)) : null },
        surfaces: { inBand: surf.length, covered: surf.filter((p) => p.covered).length, groups: groups.slice(0, limit).map((g) => ({ at: g.at.map(o1), columns: g.columns, y: { min: both(g.minY), max: both(g.maxY) }, toLine: both(g.nearest), ...(g.covered ? { covered: g.covered } : {}) })),
          ...(groups.length > limit ? { more: groups.length - limit } : {}) },
        scan: { columns: cols.length, reachingBand: groundCols, standingOverBand: deeper.length, rays: raysCast, step: o1(step), headroom: o1(headroom), units: a.units ?? "map" },
        ...(tools ? { tools } : {}),
        note: "the game's own land sweep, column by column: toLine is a group's nearest surface less the line (over 0: above it, where Sandtide's curse does not land; under 0: below it). covered: surfaces under something (found by restarting the sweep under each hit). Sandtide logs its own line (`sandtide: sand line at Y`) when it is on",
      };
    },
  },
  {
    name: "game_start_versus",
    description: "Start a Versus > Deathmatch match on an INSTALLED level through the game's own menus, as a player does: main menu, Local Game, Versus, Deathmatch, the teams, the landscape from the list, the grenade. game_start_level arms a level and starts a Quick Game without the menus; this is the route the frontend takes, with its own team set-up, its landscape list (the level must be listed under Prebuilt) and its random time of day (which a pack's pinned one then overrides, and the log says so). The keys are sent by the private build's Automation module (`[Automation] Enabled=1` and `RunInBackground=1` in the game's Melange.ini: a file queue the game reads, so it needs no window focus); a public build has none and the tool says so. The game must be at its MAIN MENU (after a launch: `dismiss: 1` presses RETURN first when the language prompt shows; after a match, leave it first). `teams`: 2 to 4, each {human: true} (a player team of the save, in the save's order) or {human: false, cpu: \"easy\" | \"normal\" | \"hard\"}. `level`: its title as the list shows it (default: the pack's level title from Mods\\<modId>\\spice.json). It answers the keys sent, then waits for the match and reads the game's log: the level that loaded, its time of day line and its spawn-area deal.",
    inputSchema: { type: "object", additionalProperties: false, required: ["teams"], properties: {
      modId: { type: "string" }, level: { type: "string", description: "The level's title in the landscape list (or its slug with modId)." },
      teams: { type: "array", minItems: 2, maxItems: 4, items: { type: "object", additionalProperties: false, properties: { human: { type: "boolean" }, cpu: { type: "string", enum: ["easy", "normal", "hard"] } } } },
      humanTeams: { type: "integer", minimum: 0, maximum: 16, description: "How many player teams the save has (default 4: the game's own four)." },
      landscapeIndex: { type: "integer", minimum: 0, maximum: 200, description: "Override: how many DOWNs from the top of the landscape list (default: worked out from the installed packs' titles)." },
      dismiss: { type: "integer", minimum: 0, maximum: 3, description: "RETURNs pressed first, 4 s apart (a language prompt at the first launch)." },
      gameDir: { type: "string" }, pid: { type: "integer", description: "The game's process id (default: the WormsMayhem.exe that runs from gameDir)." },
      dryRun: { type: "boolean", description: "Answer the key presses and send nothing." },
      timeoutSec: { type: "integer", minimum: 10, maximum: 300 },
    } },
    async handler(s: Session, a: { modId?: string; level?: string; teams: VersusTeam[]; humanTeams?: number; landscapeIndex?: number; dismiss?: number; gameDir?: string; pid?: number; dryRun?: boolean; timeoutSec?: number }) {
      const game = a.gameDir ?? process.env.MELANGE_GAME_DIR;
      if (!game || !existsSync(join(game, "Mods"))) throw new ToolError("give gameDir (or set MELANGE_GAME_DIR): the game's folder");
      // the enabled packs' level titles, and the wanted one's
      const titles = packLevels(game);
      const mine = titles.find((t) => (!a.modId || t.mod === a.modId) && (a.level === undefined || t.title.toLowerCase() === a.level.toLowerCase() || t.slug === a.level));
      const title = mine?.title ?? a.level;
      if (!title) throw new ToolError(`no level to pick: give level (a title in the landscape list)${a.modId ? `; ${a.modId} has no enabled multi level in ${game}\\Mods` : ""}`);
      const index = a.landscapeIndex ?? landscapeIndex(title, titles.map((t) => t.title));
      if (index < 0) throw new ToolError(`'${title}' is not among the prebuilt landscapes this tool knows (the game's four and the enabled packs': ${titles.map((t) => t.title).join(", ") || "none"}): is the pack installed and enabled? landscapeIndex overrides`);
      let keys: string[];
      try { keys = versusRoute(a.teams, index, { humanTeams: a.humanTeams, dismiss: a.dismiss }); } catch (e) { throw new ToolError((e as Error).message); }
      const plan = { level: title, ...(mine ? { modId: mine.mod, key: `Multi.${mine.mod}_${mine.slug}` } : {}), landscapeIndex: index, teams: a.teams.length, keyPresses: keys.filter((k) => k.startsWith("key")).length,
        seconds: Math.round(keys.filter((k) => k.startsWith("wait")).reduce((n, k) => n + +k.split(" ")[1], 0) / 1000) };
      if (a.dryRun) return { dryRun: true, ...plan, keys };
      // the Automation queue: a private module of the game's build
      if (!automationOn(game)) throw new ToolError(`the game's Melange.ini has no [Automation] Enabled=1: the menus are driven through the private build's Automation module (a public build has none; game_start_level starts a Quick Game without the menus)`);
      const st = await gameState(s);
      if (st.scene === "match") throw new ToolError("a match is running: the route starts at the main menu (end_match leaves a Test; a Versus match is left from its pause menu, or restart the game)");
      const pid = a.pid ?? gamePid(game);
      if (!pid) throw new ToolError(`no WormsMayhem.exe runs from ${game}: give pid`);
      const log = join(game, "Melange", "Melange.log"), marker = `ergversus-${Date.now().toString(36)}`;
      const lines0 = existsSync(log) ? readFileSync(log, "utf8").split(/\r?\n/).length : 0;
      appendFileSync(join(game, "Melange", `automation.${pid}.txt`), [...keys, `log ${marker}`].join("\n") + "\n");
      const t0 = Date.now(), limit = ((a.timeoutSec ?? 60) + plan.seconds) * 1000;
      let sent = false, scene = "", tail: string[] = [];
      while (Date.now() - t0 < limit) {
        await sleep(1500);
        tail = readFileSync(log, "utf8").split(/\r?\n/).slice(lines0);
        if (!sent && tail.some((l) => l.includes(`LOG ${marker}`))) sent = true;
        if (sent) { scene = (await gameState(s).catch(() => ({ scene: "" }))).scene ?? ""; if (scene === "match" && tail.some((l) => /spawn areas: seed|\[sim\] \d+ forwarded/.test(l))) break; }
      }
      const pick = (re: RegExp) => { const l = [...tail].reverse().find((x) => re.test(x)); return l ? l.replace(/^.*?INFO\s+/, "") : null; };
      const loaded = pick(/\[levels\] land mask fix on \(|\[levels\] camera lift on \(/), key = /\((Multi\.[^)]+)\)/.exec(loaded ?? "")?.[1] ?? null;
      const right = !!key && (!mine || key === `Multi.${mine.mod}_${mine.slug}`);
      return { started: scene === "match", ...plan, sent, pid, loaded: key,
        ...(scene === "match" && !key ? { warning: "a match started and the log names no pack level: the list's highlight was not on the level (a pack's level logs `land mask fix on (Multi.<mod>_<slug>)`); check landscapeIndex with a shot" } : {}),
        ...(key && !right ? { warning: `the match loaded ${key}, not ${mine ? `Multi.${mine.mod}_${mine.slug}` : title}: the landscape list's order differs from what was worked out (landscapeIndex overrides)` } : {}),
        timeOfDay: pick(/time of day pinned/), spawnDeal: pick(/spawn areas: seed/), seconds: Math.round((Date.now() - t0) / 1000),
        ...(scene !== "match" ? { note: sent ? "the keys were sent and no match is running: the game was not at its main menu, or a prompt was in the way (dismiss), or the save has other teams (humanTeams). game_shots or a window grab shows where the menus are" : "the game did not read the Automation queue in time (is this the private build, and the pid the game's?)" } : {}) };
    },
  },
  {
    name: "game_localnet_match",
    description: "A two-peer network match on an installed level, driven on two running games of this machine over LocalNet (the private build's lobby and P2P over localhost), and the two peers' spawn deals and worm positions compared: the check that both machines deal a level's spawn areas alike (spawns.mode areas: each peer deals from the match's seed itself). `host` and `join`: {gameDir, pid} of two games at their MAIN MENUS, each with `[LocalNet] Enabled=1` and its own Name, `[Automation] Enabled=1` and `RunInBackground=1` in its Melange.ini, the same build and the same enabled mods with the level's pack among them (the testenv's business: back up what you change, `[Wormsign] Record=0`, and the Steam save both write). The host goes Network Game, its team, Create Match (Deathmatch, Standard, 2 players), and in the lobby picks the landscape from the list; the joiner goes Network Game, its team, Custom Match and joins the match found; both press the grenade. Then it waits for both to be in the game and answers `deal` (each peer's `spawn areas: seed ..., N teams -> areas [...]` log line, compared: agree) and `positions` (every worm's place as each peer reads it, through each game's Oasis: started here when a peer's is not up, and Documents\\Melange\\oasis_url.txt put back). dryRun answers the key presses. The match is left running: stop the games (or play on).",
    inputSchema: { type: "object", additionalProperties: false, required: ["host", "join"], properties: {
      host: { type: "object", additionalProperties: false, required: ["gameDir"], properties: { gameDir: { type: "string" }, pid: { type: "integer" }, team: { type: "integer", minimum: 0, maximum: 15, description: "Its place in the Select Team list (default 0)." }, url: { type: "string", description: "Its Oasis launch URL, when it is up already." } } },
      join: { type: "object", additionalProperties: false, required: ["gameDir"], properties: { gameDir: { type: "string" }, pid: { type: "integer" }, team: { type: "integer", minimum: 0, maximum: 15, description: "Default 1." }, url: { type: "string" } } },
      modId: { type: "string" }, level: { type: "string", description: "The level's title in the landscape list (default: modId's level)." },
      landscapeIndex: { type: "integer", minimum: 0, maximum: 200 },
      positions: { type: "boolean", description: "Compare the worms' positions through each peer's Oasis (default true)." },
      dryRun: { type: "boolean" }, timeoutSec: { type: "integer", minimum: 20, maximum: 600 },
    } },
    async handler(_s: Session, a: { host: { gameDir: string; pid?: number; team?: number; url?: string }; join: { gameDir: string; pid?: number; team?: number; url?: string }; modId?: string; level?: string; landscapeIndex?: number;
      positions?: boolean; dryRun?: boolean; timeoutSec?: number }) {
      for (const p of [a.host, a.join]) if (!existsSync(join(p.gameDir, "Mods"))) throw new ToolError(`${p.gameDir}: not a game folder (no Mods)`);
      if (a.host.gameDir.toLowerCase() === a.join.gameDir.toLowerCase()) throw new ToolError("host and join are two game folders");
      const titles = packLevels(a.host.gameDir);
      const mine = titles.find((t) => (!a.modId || t.mod === a.modId) && (a.level === undefined || t.title.toLowerCase() === a.level.toLowerCase() || t.slug === a.level));
      const title = mine?.title ?? a.level;
      if (!title) throw new ToolError("no level to pick: give level (a title in the landscape list) or modId");
      const index = a.landscapeIndex ?? landscapeIndex(title, titles.map((t) => t.title));
      if (index < 0) throw new ToolError(`'${title}' is not among the host's prebuilt landscapes (the game's four and its enabled packs': ${titles.map((t) => t.title).join(", ") || "none"})`);
      const key = mine ? `Multi.${mine.mod}_${mine.slug}` : null;
      const hostKeys = localnetRoute("host", { team: a.host.team ?? 0, landscape: index }), bootKeys = localnetRoute("joinBoot", { team: a.join.team ?? 1 }), joinKeys = localnetRoute("joinFind");
      if (a.dryRun) return { dryRun: true, level: title, key, landscapeIndex: index, host: hostKeys, joinBoot: bootKeys, joinFind: joinKeys, hostReady: localnetRoute("hostReady"), joinReady: localnetRoute("joinReady") };
      // the join side must have the level too, or its lobby refuses the map
      if (mine && !packLevels(a.join.gameDir).some((t) => t.mod === mine.mod && t.slug === mine.slug)) throw new ToolError(`the joiner's folder has no enabled ${mine.mod} pack with the level ${mine.slug}: both peers need the same packs`);
      const peers = [{ name: "host", ...a.host }, { name: "join", ...a.join }].map((p) => {
        if (!automationOn(p.gameDir)) throw new ToolError(`${p.gameDir}: its Melange.ini has no [Automation] Enabled=1 (the private build's key queue)`);
        const ini = (() => { try { return readFileSync(join(p.gameDir, "Melange.ini"), "utf8"); } catch { return ""; } })();
        if (!/^\s*Enabled\s*=\s*1/m.test(/\[LocalNet\]([^[]*)/.exec(ini)?.[1] ?? "")) throw new ToolError(`${p.gameDir}: its Melange.ini has no [LocalNet] Enabled=1: without it the game would open a real Steam lobby`);
        const pid = p.pid ?? gamePid(p.gameDir);
        if (!pid) throw new ToolError(`no WormsMayhem.exe runs from ${p.gameDir}: start it (and give pid)`);
        return { ...p, pid, log: join(p.gameDir, "Melange", "Melange.log") };
      });
      const [H, J] = peers, t0 = Date.now(), limit = (a.timeoutSec ?? 120) * 1000;
      const lobbies = (p: typeof H) => (readFileSync(p.log, "utf8").match(/-> WaitingGameStart\(lobby\)/g) ?? []).length;
      const ingame = (p: typeof H) => (readFileSync(p.log, "utf8").match(/-> InGame/g) ?? []).length;
      const waitFor = async (what: string, test: () => boolean, ms: number) => { const t = Date.now(); while (!test()) { if (Date.now() - t > ms) throw new ToolError(`${what} did not happen within ${Math.round(ms / 1000)} s: a window grab of each game shows where its menus are (was each at its main menu?)`); await sleep(1000); } };
      const steps: string[] = [];
      const l0 = [lobbies(H), lobbies(J)], g0 = [ingame(H), ingame(J)];
      // both leave their main menus at once (a game idle there starts its demo match within a minute)
      const [hr, br] = await Promise.all([automate(H.gameDir, H.pid, hostKeys, 120000), automate(J.gameDir, J.pid, bootKeys, 60000)]);
      if (!br.done) throw new ToolError("the joiner did not read its Automation queue (is it the private build, and the pid the game's?)");
      if (!hr.done) throw new ToolError("the host did not read its Automation queue (is it the private build, and the pid the game's?)");
      await waitFor("the host's lobby", () => lobbies(H) > l0[0], 30000);
      steps.push("the host is in its lobby with the landscape picked");
      const refused = hr.lines.concat(readFileSync(H.log, "utf8").split(/\r?\n/).slice(-40)).find((l) => /is not a level registered/.test(l));
      const jr = await automate(J.gameDir, J.pid, joinKeys, 120000);
      if (!jr.done) throw new ToolError("the joiner did not read its Automation queue");
      await waitFor("the joiner's lobby", () => lobbies(J) > l0[1], 40000);
      steps.push("the joiner is in the lobby");
      await automate(J.gameDir, J.pid, localnetRoute("joinReady"), 30000);
      await automate(H.gameDir, H.pid, localnetRoute("hostReady"), 30000);
      await waitFor("the match on both peers", () => ingame(H) > g0[0] && ingame(J) > g0[1], limit);
      steps.push("both peers are in the game");
      await sleep(2500);
      const dealOf = (p: typeof H) => ({ name: p.name, deals: parseSpawnDeals(readFileSync(p.log, "utf8")) });
      const deal = compareDeals([dealOf(H), dealOf(J)], 1);
      const loaded = peers.map((p) => { const m = [...readFileSync(p.log, "utf8").matchAll(/land mask fix on \((Multi\.[^)]+)\)/g)].pop(); return m ? m[1] : null; });
      // the worms as each peer reads them, through its own Oasis
      let positions: Record<string, unknown> | undefined;
      if (a.positions !== false) {
        // the shared url file (a game's launch deletes it, so it may not be there yet): kept and put back
        const dd = documentsDir(), uf = urlFile() ?? (dd ? join(dd, "Melange", "oasis_url.txt") : null), keep = uf && existsSync(uf) ? readFileSync(uf, "utf8") : null;
        const WORMS = `local o = "" for _, w in ipairs(wum.game.worms()) do o = o .. string.format("%d,%d,%.2f,%.2f,%.2f;", w.slot, w.team, w.pos.x, w.pos.y, w.pos.z) end return o`;
        const read = async (p: typeof H) => {
          let url = p.url;
          if (!url) {
            // oasis.start writes the shared url file: read this peer's from it at once
            await automate(p.gameDir, p.pid, ["oasis.start", "wait 800"], 20000);
            await sleep(800);
            url = uf && existsSync(uf) ? readFileSync(uf, "utf8").trim() : undefined;
          }
          if (!url) throw new Error("no Oasis URL");
          const c = await openOasis(url, p.name);
          try {
            const r = await c.client.call<{ ok: boolean; text: string }>("lua.eval", { target: "client", code: WORMS }, 15000);
            return (r.text ?? "").split(";").filter(Boolean).map((x) => x.split(",").map(Number)).map((v) => ({ slot: v[0], team: v[1], pos: [r2(v[2] / W), r2(v[3] / W), r2(v[4] / W)] }));
          } finally { (c.client as { close?: () => void }).close?.(); }
        };
        try {
          const hw = await read(H), jw = await read(J), cmp = comparePositions(hw, jw);
          positions = { ...cmp, host: hw.map((w) => `${w.slot}:t${w.team}:(${w.pos.join(", ")})`).join(" "), units: "map" };
        } catch (e) { positions = { error: `the worms could not be read through the peers' Oasis: ${(e as Error).message}` }; }
        finally { if (uf && keep !== null) writeFileSync(uf, keep); }
      }
      return { ok: deal.agree && (!positions || positions.agree !== false) && (!key || loaded.every((k) => k === key)), level: title, key, loaded: { host: loaded[0], join: loaded[1] },
        deal, ...(positions ? { positions } : {}), steps, seconds: Math.round((Date.now() - t0) / 1000),
        ...(refused ? { hostLobbyNote: refused.replace(/^.*?(INFO|WARN)\s+/, "") + " (the lobby's remembered landscape, before this one was picked)" } : {}),
        ...(key && !loaded.every((k) => k === key) ? { warning: `a peer loaded ${loaded.join(" / ")}, not ${key}: the landscape list's order differs from what was worked out (landscapeIndex overrides)` } : {}),
        note: "deal.agree: both peers logged the same seed and the same areas for the match; positions.agree: every worm stands at the same place on both. The match is still running on both games" };
    },
  },
];
