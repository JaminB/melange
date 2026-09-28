// The log panel's data: records from the `log` channel or a past session's events.jsonl, a capped store, and the
// level/category/text filter. No DOM here, so it is unit-tested directly.
export const LEVELS = ["trace", "debug", "info", "warn", "error", "fatal"] as const;
export const DEFAULT_CAP = 50000;

export interface LogRow {
  n: number;           // arrival order in this store
  seq: number;
  lvl: number;         // index into LEVELS
  cat: string;
  ts: string;          // wall clock when known, else the game time in ms
  j: unknown;          // the jlog record: an object, or its JSON text until first needed
  msg?: string;
  hay?: string;
}

export function levelOf(v: unknown): number {
  if (typeof v === "number" && v >= 0 && v < LEVELS.length) return Math.floor(v);
  if (typeof v === "string") {
    const i = LEVELS.indexOf(v.toLowerCase() as (typeof LEVELS)[number]);
    if (i >= 0) return i;
    if (/^warn/i.test(v)) return 3;
    if (/^err/i.test(v)) return 4;
  }
  return 2;
}

function tsOf(v: unknown, rec?: Record<string, unknown>): string {
  if (rec && typeof rec.wall === "string") return rec.wall;
  if (typeof v === "string") return v;
  if (typeof v === "number") return String(v);
  if (rec && typeof rec.t === "number") return String(rec.t);
  return "";
}

// One `log` channel item: {seq, lvl, cat, ts, j}.
export function fromEvent(d: unknown): Omit<LogRow, "n"> | undefined {
  if (!d || typeof d !== "object") return undefined;
  const o = d as Record<string, unknown>;
  const seq = typeof o.seq === "number" ? o.seq : NaN;
  if (!Number.isFinite(seq)) return undefined;
  const rec = o.j && typeof o.j === "object" ? (o.j as Record<string, unknown>) : undefined;
  return { seq, lvl: levelOf(o.lvl), cat: typeof o.cat === "string" ? o.cat : "", ts: tsOf(o.ts, rec), j: o.j };
}

// One line of a session's events.jsonl.
export function fromLine(line: string): Omit<LogRow, "n"> | undefined {
  if (!line.trim()) return undefined;
  let rec: Record<string, unknown>;
  try {
    rec = JSON.parse(line);
  } catch {
    return undefined;
  }
  if (!rec || typeof rec !== "object") return undefined;
  return { seq: typeof rec.seq === "number" ? rec.seq : 0, lvl: levelOf(rec.lvl), cat: typeof rec.cat === "string" ? rec.cat : "",
    ts: tsOf(undefined, rec), j: rec };
}

export function record(r: LogRow): Record<string, unknown> | undefined {
  if (typeof r.j === "string") {
    try {
      r.j = JSON.parse(r.j);
    } catch {
      return undefined;
    }
  }
  return r.j && typeof r.j === "object" ? (r.j as Record<string, unknown>) : undefined;
}

export function message(r: LogRow): string {
  if (r.msg !== undefined) return r.msg;
  const rec = record(r);
  let m = "";
  if (rec) {
    m = typeof rec.msg === "string" ? rec.msg : "";
    if (rec.data !== undefined) {
      const d = JSON.stringify(rec.data);
      m += (m ? " " : "") + (d.length > 300 ? `${d.slice(0, 300)}…` : d);
    }
  } else if (typeof r.j === "string") {
    m = r.j;
  }
  r.msg = m;
  return m;
}

export interface LogFilter { minLevel: number; cats: string[]; text: string; }
export const NO_FILTER: LogFilter = { minLevel: 0, cats: [], text: "" };

export function matches(r: LogRow, f: LogFilter): boolean {
  if (r.lvl < f.minLevel) return false;
  if (f.cats.length && !f.cats.includes(r.cat)) return false;
  if (!f.text) return true;
  if (r.hay === undefined) r.hay = `${r.cat} ${message(r)}`.toLowerCase();
  return r.hay.includes(f.text.toLowerCase());
}

// All records up to `cap` (oldest dropped first) and the filtered view of them.
export class LogStore {
  readonly all: LogRow[] = [];
  readonly view: LogRow[] = [];
  readonly cats = new Map<string, number>();
  private next = 1;
  private filter: LogFilter = NO_FILTER;
  lastSeq = 0;
  trimmed = 0;

  constructor(readonly cap = DEFAULT_CAP) {}

  // Live records: those at or below the last seen seq (a backlog replayed after a reconnect) are skipped.
  addLive(items: Omit<LogRow, "n">[]): number {
    const fresh = items.filter((x) => x.seq > this.lastSeq);
    if (fresh.length) this.lastSeq = fresh[fresh.length - 1].seq;
    return this.add(fresh);
  }

  add(items: Omit<LogRow, "n">[]): number {
    for (const x of items) {
      const r = x as LogRow;
      r.n = this.next++;
      this.all.push(r);
      this.cats.set(r.cat, (this.cats.get(r.cat) ?? 0) + 1);
      if (matches(r, this.filter)) this.view.push(r);
    }
    const over = this.all.length - this.cap;
    if (over > 0) {
      const firstKept = this.all[over].n;
      this.all.splice(0, over);
      this.trimmed += over;
      let k = 0;
      while (k < this.view.length && this.view[k].n < firstKept) k++;
      if (k) this.view.splice(0, k);
    }
    return items.length;
  }

  setFilter(f: LogFilter) {
    this.filter = f;
    this.view.length = 0;
    for (const r of this.all) if (matches(r, f)) this.view.push(r);
  }

  clear() {
    this.all.length = 0;
    this.view.length = 0;
    this.cats.clear();
    this.trimmed = 0;
  }
}

export function parseJsonl(text: string, cap = DEFAULT_CAP): { rows: Omit<LogRow, "n">[]; bad: number; skipped: number } {
  const lines = text.split("\n");
  const rows: Omit<LogRow, "n">[] = [];
  let bad = 0;
  for (const l of lines) {
    if (!l.trim()) continue;
    const r = fromLine(l);
    if (r) rows.push(r);
    else bad++;
  }
  const skipped = Math.max(0, rows.length - cap);
  return { rows: skipped ? rows.slice(skipped) : rows, bad, skipped };
}

// log.sessions entries: {id, files, bytes}; `files` is a list of names or a count.
export interface SessionInfo { id: string; files: string[]; bytes: number; }
export function sessionsOf(v: unknown): SessionInfo[] {
  if (!Array.isArray(v)) return [];
  const out: SessionInfo[] = [];
  for (const s of v) {
    if (!s || typeof s !== "object" || typeof (s as { id?: unknown }).id !== "string") continue;
    const o = s as { id: string; files?: unknown; bytes?: unknown };
    const files = Array.isArray(o.files) ? o.files.filter((f): f is string => typeof f === "string") : ["events.jsonl"];
    out.push({ id: o.id, files, bytes: typeof o.bytes === "number" ? o.bytes : 0 });
  }
  return out;
}

export function sessionUrl(id: string, file: string): string {
  return `/logs/${encodeURIComponent(id)}/${encodeURIComponent(file)}`;
}

export function formatBytes(n: number): string {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  return `${(n / 1024 / 1024).toFixed(1)} MB`;
}

export function shortTime(ts: string): string {
  const m = /T(\d\d:\d\d:\d\d(?:\.\d+)?)/.exec(ts);
  return m ? m[1] : ts;
}
