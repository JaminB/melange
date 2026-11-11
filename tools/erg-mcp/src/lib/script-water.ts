// The water the project's level script makes, turn by turn, for check_flood, place_spawn_knots and the factory check.
// Melange.exe's server runs the whole script in a stock Lua 5.0 with a stand-in for the match's wum table
// (level.script.water, src/oasis/standalone/water_sim.cpp): every waterSchedule call, those set from a turnStarted handler
// (a second phase), wum.sim.every/after stepping Water.Level, Water.RiseSpeed.Current. A server without it (the game's
// own) leaves the static reading: the first top-level wum.sim.waterSchedule{...} (lib/flood.ts), with a warning when
// the script has more than one. Map units out.
import { ToolError } from "../registry";
import type { Session } from "../session";
import { scheduleFromScript, waterByTurn, type WaterSchedule } from "./flood";
import { WORLD_PER_MAP } from "./voxel";

export interface ScriptWater {
  waterAt: (turn: number) => number;   // map units, the water once turn `turn` has started
  from: string;
  simulated: boolean;
  turns: number;                       // the turns simulated (later turns keep the last level)
  warnings: string[];
  schedules?: unknown[];               // simulated: each waterSchedule call and the turn it ran in
  errors?: unknown[];                  // simulated: what raised in the script, with the turn
  schedule?: WaterSchedule;            // static: the schedule read
}

interface SimAnswer { levels: { turn: number; world: number; end: number }[]; schedules: unknown[]; errors: unknown[]; errorCount: number; calls: Record<string, number>; }

/** waterSchedule calls in the script's code (comments left out). */
export function scheduleCalls(script: string): number {
  const code = script.replace(/--\[(=*)\[[\s\S]*?\]\1\]/g, "").replace(/--[^\n]*/g, "");
  return (code.match(/wum\.sim\.waterSchedule\s*(?:\(\s*)?\{/g) ?? []).length;
}

/** The script's water by turn from `start` (map units), or null when the script sets none. Throws (ToolError) when the
 * static reading cannot work a schedule out, and with `strict` when the script sets the water from its own code and the
 * server cannot run it (check_flood's schedule "script"; the spawn and factory checks just go without). `teams`: the
 * teams of a round (wum.sim.round). */
export async function scriptWater(s: Session, start: number, turns = 200, teams = 2, strict = false): Promise<ScriptWater | null> {
  const text = (await s.requireProject().erg.script()) ?? "";
  if (!text.trim()) return null;
  let methods: string[] = [], simFailed: string | undefined;
  try {
    methods = (await s.ensure()).methods;
  } catch {
    // no server: the static reading
  }
  let r: SimAnswer | null = null;
  if (methods.includes("level.script.water"))
    // turnsPerRound: the round of wum.sim.round and sim.turnStarted goes up once every team has had a turn
    r = await (await s.client()).call<SimAnswer>("level.script.water", { text, turns, start: start * WORLD_PER_MAP, turnsPerRound: teams }, 30000)
      .catch((e: Error) => { simFailed = e.message; return null; });
  if (r) {
    const levels = r.levels.map((l) => l.world / WORLD_PER_MAP);
    if (!r.schedules.length && levels.every((v) => v === start)) return null;
    const warnings: string[] = [];
    if (r.errorCount) warnings.push(`the script raised ${r.errorCount} time(s) in the simulation (errors lists them): the game would log them the same way`);
    return {
      waterAt: (t: number) => (t < 1 ? start : levels[Math.min(t, levels.length) - 1]),
      from: "the project's script, run turn by turn (Melange.exe's level.script.water: every waterSchedule call, turnStarted handlers, timers and Water.Level writes)",
      simulated: true, turns: levels.length, warnings, schedules: r.schedules, ...(r.errors.length ? { errors: r.errors } : {}),
    };
  }
  const sc = scheduleFromScript(text);
  if (!sc) {
    // a script that sets the water itself (a handler or a timer writing Water.Level) needs the simulation
    if (strict && /setData\s*\(\s*["']Water\.(Level|RiseSpeed)/.test(text.replace(/--[^\n]*/g, "")))
      throw new ToolError(`the script sets the water from its own code (setData("Water.Level") in a handler or timer), which only Melange.exe's server runs (level.script.water)${simFailed ? ` and it could not (${simFailed})` : ""}: connect through MELANGE_GAME_DIR (the game closed), or pass flood.levels or flood.rounds`);
    return null;
  }
  const n = scheduleCalls(text);
  return {
    waterAt: waterByTurn(start, { schedule: sc }), from: "the project's script", simulated: false, turns, schedule: sc,
    warnings: [
      ...(simFailed ? [`the server could not run the script (${simFailed}): read statically instead`] : []),
      ...(n > 1 ? [`the script has ${n} wum.sim.waterSchedule calls, and only the first is read here: this server cannot run the script (Melange.exe's level.script.water does; connect through MELANGE_GAME_DIR), so pass levels for a script with phases`] : []),
    ],
  };
}
