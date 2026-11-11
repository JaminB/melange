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

// ------------------------------------------------------------------ constants of a level script

/** The script with comments and the insides of strings blanked (same length, so positions hold). */
function blank(script: string): string {
  return script
    .replace(/--\[(=*)\[[\s\S]*?\]\1\]/g, (m) => m.replace(/[^\n]/g, " "))
    .replace(/--[^\n]*/g, (m) => " ".repeat(m.length))
    .replace(/"(?:[^"\\\n]|\\.)*"|'(?:[^'\\\n]|\\.)*'/g, (m) => m[0] + " ".repeat(m.length - 2) + m[0]);
}

/** A number expression of Lua: numbers, names bound in `env`, + - * / % ^ (right associative), unary -, parentheses. */
export function evalNumber(src: string, env: Map<string, number>): number | null {
  const toks = src.match(/\d+(?:\.\d*)?(?:[eE][-+]?\d+)?|\.\d+|[A-Za-z_]\w*|[-+*/%^()]|\S/g) ?? [];
  let i = 0;
  const peek = () => toks[i];
  const atom = (): number | null => {
    const t = toks[i++];
    if (t === undefined) return null;
    if (t === "(") { const v = sum(); return toks[i++] === ")" ? v : null; }
    if (t === "-") { const v = power(); return v === null ? null : -v; }
    if (/^[\d.]/.test(t)) return Number(t);
    if (/^[A-Za-z_]/.test(t)) return env.has(t) ? env.get(t)! : null;
    return null;
  };
  const power = (): number | null => {
    const a = atom();
    if (a === null) return null;
    if (peek() === "^") { i++; const b = power(); return b === null ? null : a ** b; }
    return a;
  };
  const product = (): number | null => {
    let a = power();
    while (a !== null && (peek() === "*" || peek() === "/" || peek() === "%")) {
      const op = toks[i++], b = power();
      if (b === null) return null;
      a = op === "*" ? a * b : op === "/" ? a / b : a - Math.floor(a / b) * b;
    }
    return a;
  };
  const sum = (): number | null => {
    let a = product();
    while (a !== null && (peek() === "+" || peek() === "-")) {
      const op = toks[i++], b = product();
      if (b === null) return null;
      a = op === "+" ? a + b : a - b;
    }
    return a;
  };
  const v = sum();
  return v !== null && i === toks.length && Number.isFinite(v) ? v : null;
}

/** The script's constants: top-level `local NAME = <number expression>` (or `local A, B = 1, 2`) whose names are never
 * assigned again anywhere, folded in order (an expression may use the constants above it). Code inside functions and
 * blocks is not read, and a name assigned anywhere else is not a constant. */
export function scriptConstants(script: string): Map<string, number> {
  const code = blank(script), env = new Map<string, number>();
  // block depth: function, if, do (for and while open with their do), repeat open; end and until close
  const words = /\b(function|if|do|repeat|end|until|local)\b|\n/g;
  let depth = 0;
  const locals: { names: string[]; values: string[] }[] = [];
  for (let m: RegExpExecArray | null; (m = words.exec(code)); ) {
    const w = m[1];
    if (w === "function" || w === "if" || w === "do" || w === "repeat") depth++;
    else if (w === "end" || w === "until") depth = Math.max(0, depth - 1);
    else if (w === "local" && depth === 0) {
      // the statement runs to the end of its line
      const rest = code.slice(m.index + 5).split("\n")[0];
      const st = /^\s+([A-Za-z_]\w*(?:\s*,\s*[A-Za-z_]\w*)*)\s*=\s*(.+?)\s*;?\s*$/.exec(rest);
      if (st && !/\bfunction\b/.test(st[2])) locals.push({ names: st[1].split(",").map((x) => x.trim()), values: st[2].split(",").map((x) => x.trim()) });
    }
  }
  // a name assigned anywhere after its local (NAME = ..., not ==, <=, >=, ~=) is a variable, not a constant
  const reassigned = (name: string) => {
    const re = new RegExp(`(^|[^.\\w])${name}\\s*=(?!=)`, "g");
    let n = 0;
    for (const x of code.matchAll(re)) if (!/local\s+[\w\s,]*$/.test(code.slice(Math.max(0, x.index! - 80), x.index! + x[1].length))) n++;
    return n > 0;
  };
  for (const l of locals)
    l.names.forEach((name, k) => {
      if (l.values[k] === undefined || reassigned(name)) return;
      const v = evalNumber(l.values[k], env);
      if (v !== null) env.set(name, v);
    });
  return env;
}

/** The schedule a level script sets: the first wum.sim.waterSchedule{...} whose fields are numbers, number expressions of
 * the script's top-level constants (local FIRST = 5 ... startTurn = FIRST, cap = CAP * 2) or the units string. Null when
 * the script has none; a ToolError naming the field when one cannot be worked out. */
export function scheduleFromScript(script: string): WaterSchedule | null {
  const code = script.replace(/--\[(=*)\[[\s\S]*?\]\1\]/g, "").replace(/--[^\n]*/g, "");
  const m = /wum\.sim\.waterSchedule\s*(?:\(\s*)?\{([^}]*)\}/.exec(code);
  if (!m) return null;
  const env = scriptConstants(script);
  const out: Record<string, number | string> = {};
  for (const part of m[1].split(",")) {
    const t = part.trim();
    if (!t) continue;
    const kv = /^([A-Za-z_]\w*)\s*=\s*(.+)$/.exec(t);
    if (!kv) throw new ToolError(`the script's waterSchedule has a field check_flood cannot read: '${t}' (pass schedule instead)`);
    const v = kv[2].trim(), str = /^["'](\w+)["']$/.exec(v);
    if (str) { out[kv[1]] = str[1]; continue; }
    const n = evalNumber(v, env);
    if (n === null)
      throw new ToolError(`the script's waterSchedule sets ${kv[1]} = ${v}, not a literal number or an expression of the script's top-level constants ` +
        `(local NAME = <number> outside any function, never assigned again${env.size ? `; found ${[...env.keys()].join(", ")}` : "; found none"}): pass schedule instead`);
    out[kv[1]] = n;
  }
  if (typeof out.step !== "number" || typeof out.cap !== "number") throw new ToolError("the script's waterSchedule has no literal step and cap: pass schedule instead");
  if (out.units !== undefined && out.units !== "map" && out.units !== "world") throw new ToolError("the script's waterSchedule units are neither map nor world");
  return out as unknown as WaterSchedule;
}
