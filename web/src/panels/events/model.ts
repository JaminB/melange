// The events panel's data: bus message names, the name patterns a subscription asks for, received events and
// per-name counts. No DOM here.
export interface BusName { id: number; name: string; posts: number; deliveries: number; }

export function namesOf(v: unknown): BusName[] {
  if (!Array.isArray(v)) return [];
  const out: BusName[] = [];
  for (const x of v) {
    if (!x || typeof x !== "object" || typeof (x as { name?: unknown }).name !== "string") continue;
    const o = x as Record<string, unknown>;
    const num = (k: string) => (typeof o[k] === "number" ? (o[k] as number) : 0);
    out.push({ id: num("id"), name: o.name as string, posts: num("posts"), deliveries: num("deliveries") });
  }
  return out.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
}

// An exact message name, or "Prefix.*" for every name under Prefix.
export function validPattern(p: string): boolean {
  return /^[A-Za-z0-9_]+(\.[A-Za-z0-9_]+)*(\.\*)?$/.test(p);
}

export function matchesPattern(name: string, pattern: string): boolean {
  if (pattern.endsWith(".*")) return name.startsWith(pattern.slice(0, -1));
  return name === pattern;
}

export function covered(name: string, patterns: readonly string[]): boolean {
  return patterns.some((p) => matchesPattern(name, p));
}

export function toggle(patterns: readonly string[], p: string, on: boolean): string[] {
  const rest = patterns.filter((x) => x !== p);
  return on ? [...rest, p].sort() : rest;
}

// Groups of names by their first segment ("GameLogic.Turn.Started" -> "GameLogic"), for "all of X" shortcuts.
export function prefixes(names: readonly BusName[]): string[] {
  const s = new Set<string>();
  for (const n of names) {
    const i = n.name.indexOf(".");
    if (i > 0) s.add(n.name.slice(0, i) + ".*");
  }
  return [...s].sort();
}

export interface BusEvent {
  n: number;
  seq: number;
  at: number;            // arrival, ms since the page loaded
  frame: number;
  name: string;
  cls: string;
  path: string;
  handle: string;
  d: unknown;
  hasD: boolean;
}

export function fromEvent(v: unknown, n: number, at: number): BusEvent | undefined {
  if (!v || typeof v !== "object") return undefined;
  const o = v as Record<string, unknown>;
  if (typeof o.name !== "string") return undefined;
  const s = (k: string) => (typeof o[k] === "string" ? (o[k] as string) : o[k] === undefined || o[k] === null ? "" : String(o[k]));
  return {
    n, at, name: o.name,
    seq: typeof o.seq === "number" ? o.seq : 0,
    frame: typeof o.frame === "number" ? o.frame : 0,
    cls: s("cls"), path: s("path"), handle: s("handle"),
    d: o.d, hasD: "d" in o,
  };
}

export class EventStore {
  readonly rows: BusEvent[] = [];
  private next = 1;
  trimmed = 0;
  constructor(readonly cap = 20000) {}

  add(values: unknown[], at: number): number {
    let added = 0;
    for (const v of values) {
      const e = fromEvent(v, this.next, at);
      if (!e) continue;
      this.next++;
      this.rows.push(e);
      added++;
    }
    const over = this.rows.length - this.cap;
    if (over > 0) {
      this.rows.splice(0, over);
      this.trimmed += over;
    }
    return added;
  }

  clear() {
    this.rows.length = 0;
    this.trimmed = 0;
  }
}

// bus.counts deltas ({name: count} once a second) summed per name, with the latest per-second rate.
export interface CountRow { name: string; total: number; rate: number; }
export class Counts {
  private readonly m = new Map<string, CountRow>();
  private last = 0;

  apply(delta: unknown, now: number) {
    if (!delta || typeof delta !== "object") return;
    const secs = this.last ? Math.max(0.25, (now - this.last) / 1000) : 1;
    this.last = now;
    for (const r of this.m.values()) r.rate = 0;
    for (const [name, v] of Object.entries(delta as Record<string, unknown>)) {
      if (typeof v !== "number" || !Number.isFinite(v)) continue;
      const r = this.m.get(name) ?? { name, total: 0, rate: 0 };
      r.total += v;
      r.rate = v / secs;
      this.m.set(name, r);
    }
  }

  rows(): CountRow[] {
    return [...this.m.values()].sort((a, b) => b.rate - a.rate || b.total - a.total || (a.name < b.name ? -1 : 1));
  }

  clear() {
    this.m.clear();
    this.last = 0;
  }
}
