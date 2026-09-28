// The shell's remembered state: which panel is open, the one beside it, and the theme. Stored in localStorage;
// every access is guarded because storage can be missing or throw (private windows, blocked site data).
export type Theme = "system" | "light" | "dark";

export interface Layout {
  primary?: string;
  secondary?: string;
  direction: "row" | "column";
  theme: Theme;
}

export const LAYOUT_KEY = "oasis.layout";
export const DEFAULT_LAYOUT: Layout = { direction: "row", theme: "system" };

const ID = /^[a-z0-9][a-z0-9._-]{0,47}$/;

export function sanitize(raw: unknown): Layout {
  const o = raw && typeof raw === "object" ? (raw as Record<string, unknown>) : {};
  const id = (v: unknown) => (typeof v === "string" && ID.test(v) ? v : undefined);
  const l: Layout = {
    primary: id(o.primary),
    secondary: id(o.secondary),
    direction: o.direction === "column" ? "column" : "row",
    theme: o.theme === "light" || o.theme === "dark" ? o.theme : "system",
  };
  if (l.secondary === l.primary) l.secondary = undefined;
  return l;
}

export interface Store { getItem(k: string): string | null; setItem(k: string, v: string): void; }

export function loadLayout(store: Store | undefined): Layout {
  try {
    const s = store?.getItem(LAYOUT_KEY);
    return s ? sanitize(JSON.parse(s)) : { ...DEFAULT_LAYOUT };
  } catch {
    return { ...DEFAULT_LAYOUT };
  }
}

export function saveLayout(store: Store | undefined, l: Layout): void {
  try {
    store?.setItem(LAYOUT_KEY, JSON.stringify(sanitize(l)));
  } catch {
    /* storage unavailable */
  }
}

// Opening a panel: in the main area, or beside the main one. Opening the side panel in the main area swaps them.
export function openPanel(l: Layout, id: string, beside = false): Layout {
  if (beside) {
    if (id === l.primary) return l;
    return { ...l, secondary: id };
  }
  if (id === l.secondary) return { ...l, primary: id, secondary: l.primary };
  return { ...l, primary: id };
}

export function closeSecondary(l: Layout): Layout {
  return { ...l, secondary: undefined };
}

// Resolves remembered ids against the registered panels (a panel may have gone away).
export function resolve(l: Layout, ids: string[]): Layout {
  const has = (x?: string) => (x && ids.includes(x) ? x : undefined);
  const primary = has(l.primary) ?? ids[0];
  const secondary = has(l.secondary);
  return { ...l, primary, secondary: secondary === primary ? undefined : secondary };
}

// Palette matching: every query word must appear in the label, in any order; earlier matches rank higher.
export function matchScore(label: string, query: string): number {
  const words = query.toLowerCase().split(/\s+/).filter(Boolean);
  const text = label.toLowerCase();
  let score = 0;
  for (const w of words) {
    const at = text.indexOf(w);
    if (at < 0) return -1;
    score += at === 0 || /\W/.test(text[at - 1]) ? 0 : at + 10;
  }
  return score;
}
