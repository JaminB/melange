// The heightfield expression language: arithmetic over a column's coordinates, parsed and compiled here (never eval).
//   numbers, + - * / ^ (right associative), unary -, parentheses
//   variables: x z (from the region's centre), u v (0..1 across it), r theta (polar about the centre), W D (its size),
//              R (half the shorter side), pi e
//   functions: min max (2+ args), abs sqrt sin cos tan exp log floor ceil round pow atan2 clamp(v, lo, hi)
//              lerp(a, b, t) step(edge, v) smoothstep(e0, e1, v) noise2(x, z, seed) fbm(x, z, seed, octaves)
// noise2 is Perlin gradient noise, about -1..1 and 0 on whole coordinates; fbm sums octaves of it (lacunarity 2, gain 0.5).

export const VARIABLES = ["x", "z", "u", "v", "r", "theta", "W", "D", "R", "pi", "e"] as const;
export type Variable = (typeof VARIABLES)[number];
export type Vars = Record<Variable, number>;

const MAX_SOURCE = 2000, MAX_DEPTH = 64, MAX_NODES = 500;

export class ExprError extends Error {
  constructor(message: string, readonly at: number) {
    super(`${message} (at character ${at + 1})`);
    this.name = "ExprError";
  }
}

// ------------------------------------------------------------------ noise

function hash(x: number, z: number, seed: number): number {
  let h = (Math.imul(x | 0, 0x27d4eb2d) ^ Math.imul(z | 0, 0x165667b1) ^ Math.imul(seed | 0, 0x9e3779b9)) >>> 0;
  h = Math.imul(h ^ (h >>> 15), 0x85ebca6b) >>> 0;
  h = Math.imul(h ^ (h >>> 13), 0xc2b2ae35) >>> 0;
  return (h ^ (h >>> 16)) >>> 0;
}

const fade = (t: number) => t * t * t * (t * (t * 6 - 15) + 10);

function grad(ix: number, iz: number, seed: number, dx: number, dz: number): number {
  const a = (hash(ix, iz, seed) / 4294967296) * Math.PI * 2;
  return Math.cos(a) * dx + Math.sin(a) * dz;
}

/** Perlin gradient noise at (x, z), about -1..1. The same inputs always give the same value. */
export function noise2(x: number, z: number, seed = 0): number {
  if (!Number.isFinite(x) || !Number.isFinite(z)) return 0;
  const s = Math.round(seed);
  const x0 = Math.floor(x), z0 = Math.floor(z), fx = x - x0, fz = z - z0;
  const n00 = grad(x0, z0, s, fx, fz), n10 = grad(x0 + 1, z0, s, fx - 1, fz);
  const n01 = grad(x0, z0 + 1, s, fx, fz - 1), n11 = grad(x0 + 1, z0 + 1, s, fx - 1, fz - 1);
  const u = fade(fx), w = fade(fz);
  const a = n00 + (n10 - n00) * u, b = n01 + (n11 - n01) * u;
  return (a + (b - a) * w) * Math.SQRT2;
}

/** Octaves of noise2 (1-8), normalised to about -1..1. */
export function fbm(x: number, z: number, seed = 0, octaves = 4): number {
  const n = Math.max(1, Math.min(8, Math.round(octaves)));
  let sum = 0, amp = 1, freq = 1, norm = 0;
  for (let i = 0; i < n; i++) {
    sum += noise2(x * freq, z * freq, seed + i * 101) * amp;
    norm += amp;
    amp *= 0.5;
    freq *= 2;
  }
  return sum / norm;
}

// ------------------------------------------------------------------ functions

const clamp = (v: number, lo: number, hi: number) => Math.min(Math.max(v, lo), hi);

const FUNCS: Record<string, { args: [number, number]; fn: (...a: number[]) => number }> = {
  min: { args: [2, 16], fn: (...a) => Math.min(...a) },
  max: { args: [2, 16], fn: (...a) => Math.max(...a) },
  abs: { args: [1, 1], fn: Math.abs },
  sqrt: { args: [1, 1], fn: (v) => Math.sqrt(Math.max(0, v)) },
  sin: { args: [1, 1], fn: Math.sin },
  cos: { args: [1, 1], fn: Math.cos },
  tan: { args: [1, 1], fn: Math.tan },
  exp: { args: [1, 1], fn: Math.exp },
  log: { args: [1, 1], fn: (v) => (v > 0 ? Math.log(v) : 0) },
  floor: { args: [1, 1], fn: Math.floor },
  ceil: { args: [1, 1], fn: Math.ceil },
  round: { args: [1, 1], fn: Math.round },
  pow: { args: [2, 2], fn: Math.pow },
  atan2: { args: [2, 2], fn: Math.atan2 },
  clamp: { args: [3, 3], fn: clamp },
  lerp: { args: [3, 3], fn: (a, b, t) => a + (b - a) * t },
  step: { args: [2, 2], fn: (edge, v) => (v < edge ? 0 : 1) },
  smoothstep: { args: [3, 3], fn: (e0, e1, v) => {
    if (e0 === e1) return v < e0 ? 0 : 1;
    const t = clamp((v - e0) / (e1 - e0), 0, 1);
    return t * t * (3 - 2 * t);
  } },
  noise2: { args: [2, 3], fn: (x, z, seed = 0) => noise2(x, z, seed) },
  fbm: { args: [2, 4], fn: (x, z, seed = 0, octaves = 4) => fbm(x, z, seed, octaves) },
};
export const FUNCTIONS = Object.keys(FUNCS);

// ------------------------------------------------------------------ parser

type Tok = { k: "num"; v: number; at: number } | { k: "id"; v: string; at: number } | { k: "op"; v: string; at: number } | { k: "end"; at: number };

function tokens(src: string): Tok[] {
  const out: Tok[] = [];
  let i = 0;
  while (i < src.length) {
    const c = src[i];
    if (/\s/.test(c)) { i++; continue; }
    const num = /^(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?/.exec(src.slice(i));
    if (num) { out.push({ k: "num", v: Number(num[0]), at: i }); i += num[0].length; continue; }
    const id = /^[A-Za-z_][A-Za-z0-9_]*/.exec(src.slice(i));
    if (id) { out.push({ k: "id", v: id[0], at: i }); i += id[0].length; continue; }
    if ("+-*/^(),".includes(c)) { out.push({ k: "op", v: c, at: i }); i++; continue; }
    throw new ExprError(`unexpected '${c}'`, i);
  }
  out.push({ k: "end", at: src.length });
  return out;
}

type Fn = (v: Vars) => number;

/** Compiles an expression to a function of the column's variables, or throws an ExprError saying where it is wrong. */
export function compile(src: string): Fn {
  if (typeof src !== "string" || !src.trim()) throw new ExprError("the expression is empty", 0);
  if (src.length > MAX_SOURCE) throw new ExprError(`the expression is longer than ${MAX_SOURCE} characters`, MAX_SOURCE);
  const ts = tokens(src);
  let p = 0, nodes = 0;
  const peek = () => ts[p];
  const isOp = (v: string) => { const t = ts[p]; return t.k === "op" && t.v === v; };
  const expect = (v: string) => {
    if (!isOp(v)) throw new ExprError(`expected '${v}'`, ts[p].at);
    p++;
  };
  const count = (at: number, depth: number) => {
    if (++nodes > MAX_NODES) throw new ExprError(`the expression has more than ${MAX_NODES} parts`, at);
    if (depth > MAX_DEPTH) throw new ExprError(`the expression nests deeper than ${MAX_DEPTH}`, at);
  };

  // sum := product (('+'|'-') product)*; product := unary (('*'|'/') unary)*; unary := '-' unary | power;
  // power := atom ('^' unary)?
  function sum(d: number): Fn {
    let left = product(d);
    while (isOp("+") || isOp("-")) {
      const op = (ts[p++] as { v: string }).v, l = left, r = product(d);
      count(ts[p - 1].at, d);
      left = op === "+" ? (v) => l(v) + r(v) : (v) => l(v) - r(v);
    }
    return left;
  }
  function product(d: number): Fn {
    let left = unary(d);
    while (isOp("*") || isOp("/")) {
      const op = (ts[p++] as { v: string }).v, l = left, r = unary(d);
      count(ts[p - 1].at, d);
      left = op === "*" ? (v) => l(v) * r(v) : (v) => { const b = r(v); return b === 0 ? 0 : l(v) / b; };
    }
    return left;
  }
  function unary(d: number): Fn {
    if (isOp("-")) { p++; const a = unary(d + 1); return (v) => -a(v); }
    if (isOp("+")) { p++; return unary(d + 1); }
    return power(d);
  }
  function power(d: number): Fn {
    const base = atom(d);
    if (!isOp("^")) return base;
    p++;
    const ex = unary(d + 1);
    return (v) => Math.pow(base(v), ex(v));
  }
  function atom(d: number): Fn {
    const t = peek();
    count(t.at, d);
    if (t.k === "num") { p++; const n = t.v; return () => n; }
    if (t.k === "op" && t.v === "(") {
      p++;
      const inner = sum(d + 1);
      expect(")");
      return inner;
    }
    if (t.k === "id") {
      p++;
      if (isOp("(")) {
        const f = FUNCS[t.v];
        if (!f) throw new ExprError(`unknown function '${t.v}' (known: ${FUNCTIONS.join(", ")})`, t.at);
        p++;
        const args: Fn[] = [];
        if (!isOp(")")) {
          args.push(sum(d + 1));
          while (isOp(",")) { p++; args.push(sum(d + 1)); }
        }
        expect(")");
        if (args.length < f.args[0] || args.length > f.args[1])
          throw new ExprError(`${t.v} takes ${f.args[0] === f.args[1] ? f.args[0] : `${f.args[0]}-${f.args[1]}`} arguments, not ${args.length}`, t.at);
        const fn = f.fn;
        if (args.length === 1) { const [a] = args; return (v) => fn(a(v)); }
        if (args.length === 2) { const [a, b] = args; return (v) => fn(a(v), b(v)); }
        if (args.length === 3) { const [a, b, c] = args; return (v) => fn(a(v), b(v), c(v)); }
        return (v) => fn(...args.map((a) => a(v)));
      }
      if (!(VARIABLES as readonly string[]).includes(t.v))
        throw new ExprError(`unknown name '${t.v}' (variables: ${VARIABLES.join(", ")})`, t.at);
      const name = t.v as Variable;
      return (v) => v[name];
    }
    throw new ExprError(t.k === "end" ? "the expression ends early" : `unexpected '${t.v}'`, t.at);
  }

  const fn = sum(0);
  if (peek().k !== "end") throw new ExprError(`unexpected '${(peek() as { v: string }).v}'`, peek().at);
  return (v) => {
    const r = fn(v);
    return Number.isFinite(r) ? r : 0;
  };
}
