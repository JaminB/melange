// mods.list entries as the page shows them (the game's Mods panel and Melange.exe's Plugins page). Deep Desert can
// only be revoked here; granting stays in the game.
export interface ModInfo {
  id: string; name: string; version: string; authors: string; dir: string;
  kind: "client" | "content"; state: string; reason: string;
  on: boolean; restartRequired: boolean; implicitManifest: boolean; hasClient: boolean; hasSim: boolean;
  deepDesert: { declared: boolean; granted: boolean };
  order: number;
  // "store": installed from the Store (a record in Mods\.store\installed.json, or a map pack a Store importer made);
  // "local": put in Mods\ by hand. Undefined from a server that does not say: never hidden.
  source?: "store" | "local";
  sandbox?: { loaded: boolean; error: string; callbacks: number; disabledCallbacks: number; faults: number; bytes: number };
}

export function modsOf(v: unknown): ModInfo[] {
  if (!Array.isArray(v)) return [];
  const out: ModInfo[] = [];
  for (const x of v) {
    if (!x || typeof x !== "object" || typeof (x as { id?: unknown }).id !== "string") continue;
    const o = x as Record<string, unknown>;
    const str = (k: string) => (typeof o[k] === "string" ? (o[k] as string) : "");
    const bool = (k: string) => o[k] === true;
    const dd = o.deepDesert && typeof o.deepDesert === "object" ? (o.deepDesert as Record<string, unknown>) : {};
    out.push({
      id: o.id as string, name: str("name"), version: str("version"), authors: str("authors"), dir: str("dir"),
      kind: o.kind === "content" ? "content" : "client", state: str("state") || "unknown", reason: str("reason"),
      on: bool("on"), restartRequired: bool("restartRequired") || o.state === "restart-required",
      implicitManifest: bool("implicitManifest"), hasClient: bool("hasClient"), hasSim: bool("hasSim"),
      deepDesert: { declared: dd.declared === true, granted: dd.declared === true && dd.granted === true },
      order: typeof o.order === "number" ? o.order : -1,
      source: o.source === "store" || o.source === "local" ? o.source : undefined,
      sandbox: o.sandbox && typeof o.sandbox === "object" ? (o.sandbox as ModInfo["sandbox"]) : undefined,
    });
  }
  return out;
}

export const STATE_TEXT: Record<string, string> = {
  "enabled": "Enabled",
  "disabled": "Disabled",
  "blocked": "Blocked",
  "pending-consent": "Waiting for Deep Desert consent",
  "incompatible": "Incompatible",
  "restart-required": "Restart required",
};

export function stateText(s: string): string {
  return STATE_TEXT[s] ?? s;
}

export type Tone = "ok" | "muted" | "bad" | "warn" | "info";
export function stateTone(s: string): Tone {
  if (s === "enabled") return "ok";
  if (s === "disabled") return "muted";
  if (s === "blocked" || s === "incompatible") return "bad";
  if (s === "pending-consent") return "warn";
  return "info";
}

// What the Deep Desert cell offers: a revoke button, or a pointer to the game for granting.
export function deepDesertAction(m: ModInfo): "revoke" | "grant-in-game" | "none" {
  if (!m.deepDesert.declared) return "none";
  return m.deepDesert.granted ? "revoke" : "grant-in-game";
}

// Replaces one mod in the list (after a call returned its new info).
export function withMod(list: readonly ModInfo[], m: ModInfo): ModInfo[] {
  const i = list.findIndex((x) => x.id === m.id);
  if (i < 0) return [...list, m];
  const next = [...list];
  next[i] = m;
  return next;
}

// mods.view: the "Show local plugins" choice (kept in thumper-state.json, shared by the game and Melange.exe) and what
// the compatibility sweep did to plugins that cannot load on this Melange.
export interface Notice {
  key: string; id: string; name: string; version: string;
  action: "quarantined" | "removed" | "updated" | "failed" | string;
  reason: string; melange: string; at: string; folder: string; detail: string; text: string;
}
export interface ModsView { showLocal: boolean; notices: Notice[] }

export function viewOf(v: unknown): ModsView {
  const o = v && typeof v === "object" ? (v as Record<string, unknown>) : {};
  const notices: Notice[] = [];
  for (const x of Array.isArray(o.notices) ? o.notices : []) {
    if (!x || typeof x !== "object") continue;
    const n = x as Record<string, unknown>;
    const str = (k: string) => (typeof n[k] === "string" ? (n[k] as string) : "");
    if (!str("key") || !str("id")) continue;
    notices.push({ key: str("key"), id: str("id"), name: str("name"), version: str("version"), action: str("action"), reason: str("reason"),
      melange: str("melange"), at: str("at"), folder: str("folder"), detail: str("detail"), text: str("text") || `${str("name") || str("id")}: ${str("reason")}` });
  }
  return { showLocal: o.showLocal === true, notices: notices.reverse() };   // newest first
}

// Local plugins are hidden unless asked for: display only, they keep their switch and still load.
export function visibleMods(list: readonly ModInfo[], showLocal: boolean): { shown: ModInfo[]; hidden: number; hiddenOn: number } {
  if (showLocal) return { shown: [...list], hidden: 0, hiddenOn: 0 };
  const local = list.filter((m) => m.source === "local");
  return { shown: list.filter((m) => m.source !== "local"), hidden: local.length, hiddenOn: local.filter((m) => m.on).length };
}

export function hiddenText(hidden: number, hiddenOn: number): string {
  return `${hidden} local plugin${hidden === 1 ? "" : "s"} hidden (${hiddenOn} on)`;
}

export function sourceText(m: ModInfo): string | undefined {
  return m.source === "store" ? "Store" : m.source === "local" ? "Local" : undefined;
}
