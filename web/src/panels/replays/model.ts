// wormsign.library entries (melange/wormsign.h's ReplayInfo) as the page shows them. The standalone server's
// wormsign.library (src/oasis/standalone/wormsign_provider.cpp) sends the same shape, minus inputs/remoteInputs
// and always pinned:false, since it has no live session to ask.
export interface ReplayEntry {
  name: string;
  bytes: number;
  ticks: number;
  inputs: number;
  remoteInputs: number;
  startUnix: number;
  exeBuild: string;
  melange: string;
  land: string;
  contentHash: string;
  online: boolean;
  complete: boolean;
  pinned: boolean;
  flagged: boolean;
}

export function entriesOf(v: unknown): ReplayEntry[] {
  if (!Array.isArray(v)) return [];
  const out: ReplayEntry[] = [];
  for (const x of v) {
    if (!x || typeof x !== "object" || typeof (x as { name?: unknown }).name !== "string") continue;
    const o = x as Record<string, unknown>;
    const num = (k: string) => (typeof o[k] === "number" ? (o[k] as number) : 0);
    const str = (k: string) => (typeof o[k] === "string" ? (o[k] as string) : "");
    const bool = (k: string) => o[k] === true;
    out.push({
      name: o.name as string, bytes: num("bytes"), ticks: num("ticks"), inputs: num("inputs"), remoteInputs: num("remoteInputs"),
      startUnix: num("startUnix"), exeBuild: str("exeBuild"), melange: str("melange"), land: str("land"),
      contentHash: str("contentHash"), online: bool("online"), complete: bool("complete"), pinned: bool("pinned"),
      flagged: bool("flagged"),
    });
  }
  return out;
}

export function sortNewestFirst(list: readonly ReplayEntry[]): ReplayEntry[] {
  return [...list].sort((a, b) => b.startUnix - a.startUnix || a.name.localeCompare(b.name));
}

export function withEntry(list: readonly ReplayEntry[], e: ReplayEntry): ReplayEntry[] {
  const i = list.findIndex((x) => x.name === e.name);
  if (i < 0) return sortNewestFirst([...list, e]);
  const next = [...list];
  next[i] = e;
  return sortNewestFirst(next);
}

export function formatBytes(n: number): string {
  if (n < 1024) return `${n} B`;
  const units = ["KB", "MB", "GB"];
  let v = n / 1024, i = 0;
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024;
    i++;
  }
  return `${v.toFixed(1)} ${units[i]}`;
}

// Ticks are 20 ms buckets (melange/wormsign.h's kTickMs).
export function formatDuration(ticks: number): string {
  const totalS = Math.round((ticks * 20) / 1000);
  const h = Math.floor(totalS / 3600), m = Math.floor((totalS % 3600) / 60), s = totalS % 60;
  const ss = String(s).padStart(2, "0");
  return h ? `${h}:${String(m).padStart(2, "0")}:${ss}` : `${m}:${ss}`;
}

export function formatWhen(startUnix: number): string {
  return startUnix ? new Date(startUnix * 1000).toLocaleString() : "";
}
