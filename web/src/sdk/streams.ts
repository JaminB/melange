// Payload types and filter helpers for the streams channels: log, net, bus, bus.counts, lobby, stats. Mirrors
// src/oasis/streams/wire.cpp so a panel's filter logic matches what the server actually does.

export interface LogEvent { seq: number; lvl: "trace" | "debug" | "info" | "warn" | "error" | "fatal"; cat: string; ts: number; j: unknown; }
export interface LogFilter { minLevel?: LogEvent["lvl"]; cats?: string[]; text?: string; }

export interface BusEvent { seq: number; frame: number; name: string; cls: string; path: "post" | "deliver"; handle: number; d?: unknown; }
export interface BusFilter { names: string[]; path?: "post" | "deliver"; decode?: boolean; }
export type BusCounts = Record<string, number>;

export interface LobbyPeer { steamId: string; name: string; status: "unknown" | "vanilla" | "melangeVanilla" | "match" | "mismatch"; hash16: string; version: string; }
export interface LobbyState {
  inLobby: boolean;
  local: { hash: string; contentMods: number; modMessages: number; vanilla: boolean };
  peers: LobbyPeer[];
}

export interface StatsEvent {
  clients: number; channels: number; methods: number;
  framesOut: number; bytesOut: number; bytesIn: number; dropped: number; authFailures: number; rpcCalls: number;
  busyMsP50: number; busyMsP95: number; fps: number; frames: number;
}

const kLevelOrder: LogEvent["lvl"][] = ["trace", "debug", "info", "warn", "error", "fatal"];

// True if `lvl` is at or above `min` (default: everything passes, matching the server's default).
export function levelAtLeast(lvl: LogEvent["lvl"], min?: LogEvent["lvl"]): boolean {
  if (!min) return true;
  return kLevelOrder.indexOf(lvl) >= kLevelOrder.indexOf(min);
}

// A `log`/`net` event a LogFilter would let through, client-side (e.g. to preview a filter before subscribing).
export function matchesLog(f: LogFilter, e: Pick<LogEvent, "lvl" | "cat" | "j">): boolean {
  if (!levelAtLeast(e.lvl, f.minLevel)) return false;
  if (f.cats && f.cats.length && !f.cats.includes(e.cat)) return false;
  if (f.text && !JSON.stringify(e.j).includes(f.text)) return false;
  return true;
}

// One bus filter pattern: an exact name, or "Prefix.*" (at least one character before the ".*").
export function isBusPrefixPattern(pattern: string): boolean {
  return pattern.length >= 3 && pattern.endsWith(".*");
}

export function busPrefixOf(pattern: string): string {
  return isBusPrefixPattern(pattern) ? pattern.slice(0, -1) : pattern;  // "Foo." from "Foo.*"
}

export function busNameMatches(name: string, pattern: string): boolean {
  return isBusPrefixPattern(pattern) ? name.startsWith(busPrefixOf(pattern)) : name === pattern;
}

export function anyBusNameMatches(name: string, patterns: string[]): boolean {
  return patterns.some((p) => busNameMatches(name, p));
}

export const STREAM_CHANNELS = ["log", "net", "bus", "bus.counts", "lobby", "stats"] as const;
