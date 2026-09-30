// Pure logic for the Test button and the Export dialog. No DOM, no client: unit-tested directly. The button is
// disabled with a reason when the game isn't running, isn't at the frontend, or is in a lobby; the server is
// authoritative and may still refuse for a reason this cannot see.

export interface GameState { connected: boolean; inMatch: boolean; inLobby: boolean; }

export interface Availability { ok: boolean; reason?: string; }

export function testAvailability(s: GameState): Availability {
  if (!s.connected) return { ok: false, reason: "not connected to the game" };
  if (s.inLobby) return { ok: false, reason: "leave the lobby to test a level" };
  if (s.inMatch) return { ok: false, reason: "finish or quit the current match first" };
  return { ok: true };
}

// The state machine of melange::levels::TestState, mirrored for the status line. "starting"/"playing" are terminal
// in the sense that the panel stops showing a spinner; "failed" surfaces `detail` as the error.
export type TestPhase = "idle" | "registering" | "registered" | "armed" | "starting" | "playing" | "ended" | "failed";
export interface TestStatus { phase: TestPhase; key: string; detail: string; busy: boolean; }

export const IDLE_STATUS: TestStatus = { phase: "idle", key: "", detail: "", busy: false };

const BUSY: ReadonlySet<TestPhase> = new Set(["registering", "registered", "armed", "starting"]);

export function reduceTestEvent(prev: TestStatus, ev: { state: string; key: string; detail: string }): TestStatus {
  const phase = (["idle", "registering", "registered", "armed", "starting", "playing", "ended", "failed"] as const)
    .includes(ev.state as TestPhase) ? (ev.state as TestPhase) : prev.phase;
  return { phase, key: ev.key, detail: ev.detail, busy: BUSY.has(phase) };
}

export function statusLine(s: TestStatus): string {
  switch (s.phase) {
    case "idle": return "";
    case "registering": return "Registering the level…";
    case "registered": return "Registered. Arming…";
    case "armed": return "Armed. Press Quick Game to play it.";
    case "starting": return "Starting…";
    case "playing": return `Playing ${s.key}`;
    case "ended": return "Test ended.";
    case "failed": return s.detail ? `Test failed: ${s.detail}` : "Test failed.";
  }
}

// ---- Export dialog ------------------------------------------------------------------------------------------

export type ExportMode = "install" | "source";
export interface ExportForm { modId: string; name: string; version: string; mode: ExportMode; }

export function prefixOf(modId: string): string { return modId.replace(/-/g, "_"); }

export function validSlug(slug: string): boolean { return /^[a-z0-9]{1,24}$/.test(slug); }

export function validModId(modId: string): boolean {
  const p = prefixOf(modId);
  return p.length > 0 && p.length <= 46 && /^[a-z0-9_]+$/.test(p) && p !== "ergtest";
}

export function validVersion(v: string): boolean { return /^\d+\.\d+\.\d+$/.test(v); }

// Mirrors the server's own checks closely enough to give instant feedback; the export call re-validates for real.
export function validateExportForm(f: ExportForm): string[] {
  const errors: string[] = [];
  if (!f.modId) errors.push("a mod id is required");
  else if (!validModId(f.modId)) errors.push("the mod id becomes a pack prefix of a-z, 0-9 and '-'; 'ergtest' is reserved");
  if (!f.name.trim()) errors.push("a name is required");
  if (!f.version) errors.push("a version is required");
  else if (!validVersion(f.version)) errors.push("the version must look like 1.0.0");
  return errors;
}

export const EXPORT_NOTICE: Record<ExportMode, string> = {
  install: "This writes modified copies of game files into your Mods folder, for your own machine.",
  source: "This writes only your edits (a patch). Recipients build it against their own install — the way to share a map.",
};
