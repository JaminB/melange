// The heightfield expression language: arithmetic over a column's coordinates, parsed and compiled here (never eval).
//   numbers, + - * / ^ (right associative), unary -, parentheses
//   variables: x z (from the region's centre), u v (0..1 across it), r theta (polar about the centre), W D (its size),
//              R (half the shorter side), pi e
//   functions: min max (2+ args), abs sqrt sin cos tan exp log floor ceil round pow atan2 clamp(v, lo, hi)
//              lerp(a, b, t) step(edge, v) smoothstep(e0, e1, v) noise2(x, z, seed) fbm(x, z, seed, octaves)
//              hypot(a, b[, c]) sign(v) mod(a, b) fract(v) select(c, a, b) (a where c > 0, else b)
//   bindings: statements before the expression, each ending in ';':
//              let name = expr;                  a value computed once per column
//              fn name(a, b) = expr;             a function of its parameters (and the column's variables and the lets)
//             A name is bound once and used after it is bound; a function may call functions bound before it (so no
//             recursion). For example: let cone = 44*exp(-(r/26)^2); fn bump(cx, cz, h) = h*exp(-((x-cx)^2+(z-cz)^2)/50);
//             cone + bump(30, 0, 6) + bump(-20, 10, 4)
//              dunes(x, z, dirDeg, wavelength, height[, asym[, seed]])  transverse dunes, 0..height (see dunes below)
//              ridged(x, z, scale[, octaves[, seed]])  ridged multifractal noise, 0..1, sharp crest lines
// noise2 is Perlin gradient noise, about -1..1 and 0 on whole coordinates; fbm sums octaves of it (lacunarity 2, gain 0.5).

export const VARIABLES = ["x", "z", "u", "v", "r", "theta", "W", "D", "R", "pi", "e"] as const;
export type Variable = (typeof VARIABLES)[number];
export type Vars = Record<Variable, number>;

export const MAX_SOURCE = 8000, MAX_NODES = 4000;
const MAX_DEPTH = 64, MAX_BINDINGS = 64;

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

/** Ridged multifractal noise at (x / scale, z / scale), 0..1: (1 - |noise|) squared per octave, so the crest lines are
 * sharp and the hollows broad; octaves 1-8 (default 4, each half the weight and twice the frequency). */
export function ridged(x: number, z: number, scale = 1, octaves = 4, seed = 0): number {
  const n = Math.max(1, Math.min(8, Math.round(octaves))), k = scale > 0 ? 1 / scale : 1;
  let sum = 0, amp = 1, freq = k, norm = 0;
  for (let i = 0; i < n; i++) {
    const r = 1 - Math.abs(noise2(x * freq, z * freq, seed + i * 101));
    sum += r * r * amp;
    norm += amp;
    amp *= 0.5;
    freq *= 2;
  }
  return Math.min(1, Math.max(0, sum / norm));
}

/** Transverse dunes, 0..height: crests run across the wind direction dirDeg (degrees from +x towards +z, the way the
 * wind blows, so the windward slope faces back along it), one every wavelength. asym 0..1 (default 0.5) sets the
 * profile: 0 a symmetric cosine, 1 a long windward ramp (85% of the wavelength) ending in a short slip face (15%); the
 * crest line wanders by noise (up to about a third of a wavelength) and the crest height varies 80-100% along it, both
 * set by seed. */
export function dunes(x: number, z: number, dirDeg: number, wavelength: number, height: number, asym = 0.5, seed = 0): number {
  if (!(wavelength > 0)) return 0;
  const a = (dirDeg * Math.PI) / 180, c = Math.cos(a), s = Math.sin(a);
  const along = x * c + z * s, across = -x * s + z * c;
  const t = along / wavelength + 0.35 * fbm(across / (wavelength * 1.5), along / (wavelength * 6), seed, 3);
  const p = t - Math.floor(t);
  const w = 0.5 + 0.35 * Math.min(1, Math.max(0, asym));
  const shape = p < w ? 0.5 - 0.5 * Math.cos((Math.PI * p) / w) : 0.5 + 0.5 * Math.cos((Math.PI * (p - w)) / (1 - w));
  const vary = 0.9 + 0.1 * noise2(across / (wavelength * 2.5), along / (wavelength * 8), seed + 7);
  return height * vary * shape;
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
  dunes: { args: [4, 7], fn: (x, z, dir, wl, h = 1, asym = 0.5, seed = 0) => dunes(x, z, dir, wl, h, asym, seed) },
  ridged: { args: [3, 5], fn: (x, z, scale, oct = 4, seed = 0) => ridged(x, z, scale, oct, seed) },
  hypot: { args: [2, 3], fn: (...a) => Math.hypot(...a) },
  sign: { args: [1, 1], fn: Math.sign },
  mod: { args: [2, 2], fn: (a, b) => (b === 0 ? 0 : a - b * Math.floor(a / b)) },
  fract: { args: [1, 1], fn: (v) => v - Math.floor(v) },
  select: { args: [3, 3], fn: (c, a, b) => (c > 0 ? a : b) },
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
    if ("+-*/^(),;=".includes(c)) { out.push({ k: "op", v: c, at: i }); i++; continue; }
    throw new ExprError(`unexpected '${c}'`, i);
  }
  out.push({ k: "end", at: src.length });
  return out;
}

type Fn = (v: Vars) => number;

const KEYWORDS = ["let", "fn"];

/** The bindings of an expression: its `let` and `fn` statements up to the last ';' (every statement ends in one, and a
 * ';' stands nowhere else), or "" when it has none. A mask compiled with them in front sees the same names. */
export function bindingsOf(src: string): string {
  const i = src.lastIndexOf(";");
  return i < 0 ? "" : src.slice(0, i + 1);
}

/** Compiles an expression to a function of the column's variables, or throws an ExprError saying where it is wrong.
 * `extra` names more variables the caller puts in the object it passes (a mask's h, y and d). */
export function compile(src: string, extra: readonly string[] = []): Fn {
  const known: readonly string[] = [...VARIABLES, ...extra];
  if (typeof src !== "string" || !src.trim()) throw new ExprError("the expression is empty", 0);
  if (src.length > MAX_SOURCE) throw new ExprError(`the expression is longer than ${MAX_SOURCE} characters`, MAX_SOURCE);
  const ts = tokens(src);
  let p = 0, nodes = 0;
  const peek = () => ts[p];
  const isOp = (v: string) => { const t = ts[p]; return t.k === "op" && t.v === v; };
  const isWord = (v: string) => { const t = ts[p]; return t.k === "id" && t.v === v; };
  const expect = (v: string) => {
    if (!isOp(v)) throw new ExprError(`expected '${v}'`, ts[p].at);
    p++;
  };
  const count = (at: number, depth: number) => {
    if (++nodes > MAX_NODES) throw new ExprError(`the expression has more than ${MAX_NODES} parts`, at);
    if (depth > MAX_DEPTH) throw new ExprError(`the expression nests deeper than ${MAX_DEPTH}`, at);
  };

  // Bindings live in one slot array per compiled expression: a let's value, or a function parameter's while the
  // function's body runs (saved and restored round the call, so nested calls see their own arguments).
  const slots: number[] = [];
  const names = new Map<string, number>();                         // let name -> slot
  const fns = new Map<string, { params: number[]; body: Fn }>();
  const lets: { slot: number; fn: Fn }[] = [];
  let scope: Map<string, number> | null = null;                    // a function's parameters while its body is parsed

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
  function args(d: number): Fn[] {
    const out: Fn[] = [];
    if (!isOp(")")) {
      out.push(sum(d + 1));
      while (isOp(",")) { p++; out.push(sum(d + 1)); }
    }
    expect(")");
    return out;
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
        const user = fns.get(t.v);
        if (user) {
          p++;
          const a = args(d);
          if (a.length !== user.params.length) throw new ExprError(`${t.v} takes ${user.params.length} arguments, not ${a.length}`, t.at);
          const { params, body } = user;
          return (v) => {
            const vals = a.map((f) => f(v)), saved = params.map((s) => slots[s]);
            params.forEach((s, i) => { slots[s] = vals[i]; });
            const r = body(v);
            params.forEach((s, i) => { slots[s] = saved[i]; });
            return r;
          };
        }
        const f = FUNCS[t.v];
        if (!f) throw new ExprError(`unknown function '${t.v}' (known: ${[...FUNCTIONS, ...fns.keys()].join(", ")})`, t.at);
        p++;
        const a = args(d);
        if (a.length < f.args[0] || a.length > f.args[1])
          throw new ExprError(`${t.v} takes ${f.args[0] === f.args[1] ? f.args[0] : `${f.args[0]}-${f.args[1]}`} arguments, not ${a.length}`, t.at);
        const fn = f.fn;
        if (a.length === 1) { const [x] = a; return (v) => fn(x(v)); }
        if (a.length === 2) { const [x, y] = a; return (v) => fn(x(v), y(v)); }
        if (a.length === 3) { const [x, y, z] = a; return (v) => fn(x(v), y(v), z(v)); }
        return (v) => fn(...a.map((x) => x(v)));
      }
      const slot = scope?.get(t.v) ?? names.get(t.v);
      if (slot !== undefined) return () => slots[slot];
      if (!known.includes(t.v))
        throw new ExprError(`unknown name '${t.v}' (variables: ${[...known, ...names.keys()].join(", ")})`, t.at);
      const name = t.v as Variable;
      return (v) => v[name];
    }
    throw new ExprError(t.k === "end" ? "the expression ends early" : `unexpected '${t.v}'`, t.at);
  }
  /** A new name for a binding: not a variable, a function, a keyword or one already bound. */
  function fresh(): string {
    const t = peek();
    if (t.k !== "id") throw new ExprError("expected a name", t.at);
    if (known.includes(t.v) || FUNCS[t.v] || KEYWORDS.includes(t.v) || names.has(t.v) || fns.has(t.v) || scope?.has(t.v))
      throw new ExprError(`'${t.v}' is already a name (variables, functions and earlier bindings cannot be bound again)`, t.at);
    p++;
    return t.v;
  }

  // statements: (let name = sum ; | fn name(params) = sum ;)*
  while (isWord("let") || isWord("fn")) {
    const kw = (ts[p++] as { v: string }).v, at = ts[p].at;
    if (names.size + fns.size >= MAX_BINDINGS) throw new ExprError(`more than ${MAX_BINDINGS} bindings`, at);
    const name = fresh();
    if (kw === "let") {
      expect("=");
      const fn = sum(0);
      expect(";");
      const slot = slots.push(0) - 1;
      names.set(name, slot);
      lets.push({ slot, fn });
    } else {
      expect("(");
      const params: number[] = [];
      scope = new Map();
      if (!isOp(")")) {
        for (;;) {
          const pn = fresh();
          const slot = slots.push(0) - 1;
          scope.set(pn, slot);
          params.push(slot);
          if (!isOp(",")) break;
          p++;
        }
      }
      expect(")");
      expect("=");
      const body = sum(0);
      expect(";");
      scope = null;
      fns.set(name, { params, body });
    }
  }
  const fn = sum(0);
  if (peek().k !== "end") throw new ExprError(`unexpected '${(peek() as { v: string }).v}'`, peek().at);
  const finite = (r: number) => (Number.isFinite(r) ? r : 0);
  if (!lets.length) return (v) => finite(fn(v));
  return (v) => {
    for (const l of lets) slots[l.slot] = finite(l.fn(v));
    return finite(fn(v));
  };
}
