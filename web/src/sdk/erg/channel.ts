// The "erg" channel: Test state changes and level starts, as the game publishes them.
import type { Client } from "../client";

export const TEST_STATES = ["idle", "registering", "registered", "armed", "starting", "playing", "ended", "failed"] as const;
export type TestStateName = (typeof TEST_STATES)[number];
export type LevelSource = "vanilla" | "pack" | "test";

export interface TestEvent { kind: "test"; state: TestStateName; key: string; detail: string; }
export interface LevelEvent { kind: "level"; level: string; stem: string; source: LevelSource; online: boolean; water: number | null; }
export type ErgEvent = TestEvent | LevelEvent;

const SOURCES: readonly string[] = ["vanilla", "pack", "test"];
const str = (v: unknown): v is string => typeof v === "string";

export function parseErgEvent(m: unknown): ErgEvent | null {
  if (!m || typeof m !== "object") return null;
  const o = m as Record<string, unknown>;
  if (str(o.state)) {
    if (!(TEST_STATES as readonly string[]).includes(o.state)) return null;
    return { kind: "test", state: o.state as TestStateName, key: str(o.key) ? o.key : "", detail: str(o.detail) ? o.detail : "" };
  }
  if (str(o.level)) {
    const source = str(o.source) && SOURCES.includes(o.source) ? (o.source as LevelSource) : "vanilla";
    const water = typeof o.water === "number" && Number.isFinite(o.water) ? o.water : null;
    return { kind: "level", level: o.level, stem: str(o.stem) ? o.stem : "", source, online: o.online === true, water };
  }
  return null;
}

// Test states worth telling the user about, in words.
export function describeTest(e: TestEvent, title = e.key): string {
  switch (e.state) {
    case "registering": return `Registering ${title}...`;
    case "registered": return `${title} is registered`;
    case "armed": return `Ready: the next offline match plays ${title}`;
    case "starting": return `Starting ${title}...`;
    case "playing": return `Playing ${title}`;
    case "ended": return `${title}: match over`;
    case "failed": return `Test failed${e.detail ? `: ${e.detail}` : ""}`;
    default: return e.detail ? `Test cancelled: ${e.detail}` : "";
  }
}

export function onErgEvents(client: Client, fn: (e: ErgEvent) => void): () => void {
  return client.subscribe<unknown>("erg", undefined, (m) => {
    const e = parseErgEvent(m);
    if (e) fn(e);
  });
}
