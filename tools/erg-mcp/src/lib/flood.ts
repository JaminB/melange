// A rising sea for check_reachability and check_flood: the water level in each game turn, from a schedule shaped like the
// level script's wum.sim.waterSchedule{startTurn, every, step, cap, from, units}, from explicit levels by turn, or read out
// of the project's script. Pure; map units out.
import { ToolError } from "../registry";
import { WORLD_PER_MAP } from "./voxel";

/** wum.sim.waterSchedule's options (docs/erg.md, Rising water): step and cap required, units world unless "map". */
export interface WaterSchedule { startTurn?: number; every?: number; step: number; cap: number; from?: number; units?: "world" | "map"; }

/** The water (map y) at each game turn: `start` before the schedule, then as the script's schedule raises it at turn
 * starts (only ever up, never past cap), or `levels` ([{turn, level}]: from that turn on, in map units). */
export function waterByTurn(start: number, o: { schedule?: WaterSchedule; levels?: { turn: number; level: number }[] }): (turn: number) => number {
  if (o.schedule) {
    const s = o.schedule, k = s.units === "map" ? 1 : 1 / WORLD_PER_MAP;
    const first = s.startTurn ?? 1, every = s.every ?? 1;
    if (!(Number.isInteger(first) && first >= 1)) throw new ToolError("schedule.startTurn is a whole number of 1 or more");
    if (!(Number.isInteger(every) && every >= 1 && every <= 1000)) throw new ToolError("schedule.every is a whole number from 1 to 1000");
    if (!(s.step > 0)) throw new ToolError("schedule.step must be above 0");
    const step = s.step * k, cap = s.cap * k, from = s.from === undefined ? undefined : s.from * k;
    const memo: number[] = [];
    return (turn: number) => {
      // the level after the step at the start of `turn`: the steps so far are those at first, first + every, ... <= turn
      if (turn < first) return start;
      const n = Math.floor((turn - first) / every) + 1;
      if (memo[n] !== undefined) return memo[n];
      const raised = from === undefined ? start + n * step : from + (n - 1) * step;
      return (memo[n] = Math.max(start, Math.min(cap, raised)));
    };
  }
  const list = [...(o.levels ?? [])].sort((a, b) => a.turn - b.turn);
  return (turn: number) => {
    let w = start;
    for (const e of list) if (e.turn <= turn) w = Math.max(w, e.level);
    return w;
  };
}

/** The schedule a level script sets: the first wum.sim.waterSchedule{...} with number (and units string) fields. Null when
 * the script has none; a ToolError when its options are not literals. */
export function scheduleFromScript(script: string): WaterSchedule | null {
  const code = script.replace(/--\[(=*)\[[\s\S]*?\]\1\]/g, "").replace(/--[^\n]*/g, "");
  const m = /wum\.sim\.waterSchedule\s*(?:\(\s*)?\{([^}]*)\}/.exec(code);
  if (!m) return null;
  const out: Record<string, number | string> = {};
  for (const part of m[1].split(",")) {
    const t = part.trim();
    if (!t) continue;
    const kv = /^([A-Za-z_]\w*)\s*=\s*(.+)$/.exec(t);
    if (!kv) throw new ToolError(`the script's waterSchedule has a field check_flood cannot read: '${t}' (pass schedule instead)`);
    const v = kv[2].trim(), str = /^["'](\w+)["']$/.exec(v);
    if (str) out[kv[1]] = str[1];
    else if (/^-?\d+(\.\d+)?$/.test(v)) out[kv[1]] = Number(v);
    else throw new ToolError(`the script's waterSchedule sets ${kv[1]} = ${v}, not a literal: pass schedule instead`);
  }
  if (typeof out.step !== "number" || typeof out.cap !== "number") throw new ToolError("the script's waterSchedule has no literal step and cap: pass schedule instead");
  if (out.units !== undefined && out.units !== "map" && out.units !== "world") throw new ToolError("the script's waterSchedule units are neither map nor world");
  return out as unknown as WaterSchedule;
}
