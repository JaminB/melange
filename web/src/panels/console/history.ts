// Console input history (newest last), with a browsing cursor that keeps the unsent draft. Stored per browser.
export const HISTORY_KEY = "oasis.console.history";

export class History {
  private items: string[];
  private cursor = -1;       // -1: not browsing
  private draft = "";

  constructor(items: string[] = [], readonly cap = 200) {
    this.items = items.filter((x) => typeof x === "string" && x.trim() !== "").slice(-cap);
  }

  list(): readonly string[] { return this.items; }

  add(code: string) {
    this.cursor = -1;
    if (!code.trim()) return;
    if (this.items[this.items.length - 1] !== code) this.items.push(code);
    if (this.items.length > this.cap) this.items.splice(0, this.items.length - this.cap);
  }

  // The previous entry, or undefined at the oldest. `current` is kept as the draft when browsing starts.
  older(current: string): string | undefined {
    if (!this.items.length) return undefined;
    if (this.cursor === -1) {
      this.draft = current;
      this.cursor = this.items.length - 1;
      return this.items[this.cursor];
    }
    if (this.cursor === 0) return undefined;
    return this.items[--this.cursor];
  }

  // The next entry; past the newest returns the draft and stops browsing. Undefined when not browsing.
  newer(): string | undefined {
    if (this.cursor === -1) return undefined;
    if (this.cursor < this.items.length - 1) return this.items[++this.cursor];
    this.cursor = -1;
    return this.draft;
  }

  reset() { this.cursor = -1; }
}

export function loadHistory(): string[] {
  try {
    const v = JSON.parse(localStorage.getItem(HISTORY_KEY) ?? "[]");
    return Array.isArray(v) ? v.filter((x): x is string => typeof x === "string") : [];
  } catch {
    return [];
  }
}

export function saveHistory(items: readonly string[]) {
  try {
    localStorage.setItem(HISTORY_KEY, JSON.stringify(items));
  } catch {
    /* storage unavailable */
  }
}

// Where a completion replaces text: after the last '.' or ':' of the identifier chain before the cursor.
export function completionWord(before: string): { word: string; from: number } | undefined {
  const m = /[A-Za-z_][A-Za-z0-9_.:]*$/.exec(before);
  if (!m) return undefined;
  const word = m[0];
  const cut = Math.max(word.lastIndexOf("."), word.lastIndexOf(":"));
  return { word, from: before.length - word.length + cut + 1 };
}

export type Target = { kind: "client" } | { kind: "match" } | { kind: "mod"; mod: string };

export function targetParams(t: Target): { target: string; mod?: string } {
  return t.kind === "mod" ? { target: "mod", mod: t.mod } : { target: t.kind };
}

export function targetLabel(t: Target): string {
  return t.kind === "mod" ? t.mod : t.kind;
}

export function parseTarget(v: string): Target {
  if (v === "match") return { kind: "match" };
  if (v.startsWith("mod:") && v.length > 4) return { kind: "mod", mod: v.slice(4) };
  return { kind: "client" };
}

export function targetValue(t: Target): string {
  return t.kind === "mod" ? `mod:${t.mod}` : t.kind;
}
