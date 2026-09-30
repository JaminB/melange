// The level script (script.lua) of an open project: its text, whether it is saved, and the problems the server (or
// the same checks here, before a round trip) found in it.
import type { ErgSession, ScriptProblem } from "../../../sdk/erg/session";

export const MAX_SCRIPT_BYTES = 256 * 1024;

/** The server's byte rules, checked here first: at most 256 KB, no byte order mark, no ESC or NUL. */
export function localProblems(text: string): ScriptProblem[] {
  if (new TextEncoder().encode(text).length > MAX_SCRIPT_BYTES) return [{ line: 1, message: "the level script is larger than 256 KB" }];
  if (text.charCodeAt(0) === 0xfeff) return [{ line: 1, message: "the level script starts with a byte order mark" }];
  const lines = text.split("\n");
  for (let i = 0; i < lines.length; i++) {
    if (lines[i].includes("\u001b")) return [{ line: i + 1, message: "the level script holds an ESC byte (compiled Lua is refused)" }];
    if (lines[i].includes("\u0000")) return [{ line: i + 1, message: "the level script holds a NUL byte" }];
  }
  return [];
}

export type ScriptSession = Pick<ErgSession, "script" | "saveScript">;

export class ScriptDoc {
  text = "";
  saved = "";
  problems: ScriptProblem[] = [];
  /** False after a save to a server that checks only bytes (oasis.exe without the game). */
  syntaxChecked = true;
  loaded = false;
  busy = false;
  error: string | undefined;
  private listeners = new Set<() => void>();

  constructor(private readonly session: ScriptSession) {}

  get dirty() { return this.text !== this.saved; }

  on(fn: () => void): () => void {
    this.listeners.add(fn);
    return () => { this.listeners.delete(fn); };
  }

  private emit() { for (const fn of this.listeners) fn(); }

  async load(): Promise<void> {
    this.error = undefined;
    try {
      const t = await this.session.script();
      this.text = this.saved = t;
      this.problems = [];
      this.loaded = true;
    } catch (e) {
      this.error = e instanceof Error ? e.message : String(e);
    }
    this.emit();
  }

  edit(text: string) {
    this.text = text;
    this.problems = localProblems(text);
    this.emit();
  }

  /** Saves the current text; true when it is saved (or nothing changed). */
  async save(): Promise<boolean> {
    if (!this.dirty) return true;
    const text = this.text;
    const local = localProblems(text);
    if (local.length) {
      this.problems = local;
      this.emit();
      return false;
    }
    this.busy = true;
    this.error = undefined;
    this.emit();
    try {
      const r = await this.session.saveScript(text);
      if (r.saved) this.saved = text;
      if (this.text === text) this.problems = r.problems ?? [];
      this.syntaxChecked = r.syntaxChecked !== false;
      return r.saved;
    } catch (e) {
      this.error = e instanceof Error ? e.message : String(e);
      return false;
    } finally {
      this.busy = false;
      this.emit();
    }
  }
}

export interface ApiEntry { name: string; text: string; }

/** The short reference shown beside the editor; docs/erg.md has the full one. */
export const LEVEL_API: ApiEntry[] = [
  { name: "wum.level.stem, wum.level.key", text: "This level's file stem and registry key (Multi.<stem>)." },
  { name: "wum.level.knots", text: "Read-only: knot name → kind, for the level's named knots (no positions)." },
  { name: "wum.level.trigger(knot [, opts])", text: "A trigger at a knot. opts: index, radius, teamCollect, teamDestroy, hitpoints, wormCollect." },
  { name: "wum.level.crate(knot [, opts])", text: "A crate at a knot. opts: kind (weapon|health|utility), contents, count, amount, hitpoints, parachute." },
  { name: "wum.events.on(name, fn)", text: "Engine messages (GameLogic.Turn.Ended, Trigger.Collected, ...), \"tick\" and \"sim.turnStarted\"." },
  { name: "wum.sim.after/every(ticks, fn)", text: "Timers in simulation ticks (50 per second); wum.sim.cancel(h) stops one." },
  { name: "wum.sim.send(name [, v])", text: "Send an engine message; returns true, or nil and a reason." },
  { name: "wum.sim.getData(id), setData(id, v)", text: "Read and write the game's data values." },
  { name: "wum.sim.random([m [, n]])", text: "The match-seeded random stream (math.random is the same stream)." },
  { name: "wum.log.info/warn/error(...)", text: "Log lines tagged with the level and the tick." },
];
