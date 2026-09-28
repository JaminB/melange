// mods.list entries as the page shows them. Deep Desert can only be revoked here; granting stays in the game.
export interface ModInfo {
  id: string; name: string; version: string; authors: string; dir: string;
  kind: "client" | "content"; state: string; reason: string;
  on: boolean; restartRequired: boolean; implicitManifest: boolean; hasClient: boolean; hasSim: boolean;
  deepDesert: { declared: boolean; granted: boolean };
  order: number;
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
