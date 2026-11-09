// One open Erg project over the Oasis connection: load (the scene, then one bin frame per blob), save, Test.
import type { Client } from "../client";
import { validatePatch, validateScene, type Patch, type Scene } from "./scene";

export type TimeOfDay = "DAY" | "EVENING" | "NIGHT";
export interface ScriptProblem { line: number; message: string; }
/** A Test team: human (default true) and its worms (1-4; the Quick Game's own four when left out). */
export interface TestTeam { human?: boolean; worms?: number; }
/** teams: the Quick Game start, 2-4 human teams or a human then a computer team (the default, QuickStartHvC). */
export interface TestOptions { tod?: TimeOfDay; cleanMods?: boolean; postfx?: boolean; teams?: TestTeam[]; }

export interface ErgSession {
  load(project: string): Promise<{ scene: Scene; blobs: Map<number, ArrayBuffer> }>;
  save(patch: Patch): Promise<{ saved: boolean; warnings: string[] }>;
  /** cleanMods: no sim mod runs in the Test match (only the level script) and post-FX is off; postfx: false turns
   * post-FX off alone (or keeps it, with cleanMods, when true). cleanRoom in the answer says what the game applied;
   * a game without clean rooms leaves it out. */
  test(opts?: TestOptions): Promise<{ key: string; state: string; cleanRoom?: { simMods: boolean; postfx: boolean } }>;
  onTest(fn: (s: { state: string; key: string; detail: string }) => void): () => void;
  /** The project's level script (script.lua), "" when it has none. */
  script(): Promise<string>;
  /** syntaxChecked: false when the server cannot compile Lua (oasis.exe); the game checks it on Test. */
  saveScript(text: string): Promise<{ saved: boolean; problems: ScriptProblem[]; syntaxChecked?: boolean }>;
}

export class ErgError extends Error {
  constructor(message: string, readonly details: string[] = []) {
    super(message);
    this.name = "ErgError";
  }
}

export interface SessionOptions { blobTimeoutMs?: number; callTimeoutMs?: number; }

export function createErgSession(client: Client, opts: SessionOptions = {}): ErgSession {
  const blobTimeout = opts.blobTimeoutMs ?? 30000, callTimeout = opts.callTimeoutMs ?? 30000;
  let project: string | undefined;
  return {
    async load(id: string) {
      const scene = await client.call<Scene>("level.load", { project: id }, callTimeout);
      const v = validateScene(scene);
      if (!v.ok) throw new ErgError("the server sent an invalid scene", v.errors);
      project = id;
      const blobs = new Map<number, ArrayBuffer>();
      await Promise.all(scene.blobs.map((b) => new Promise<void>((resolve, reject) => {
        const timer = setTimeout(() => {
          off();
          reject(new ErgError(`blob ${b.ref} did not arrive within ${blobTimeout} ms`));
        }, blobTimeout);
        const off = client.onBinary(b.ref, (data) => {
          off();
          clearTimeout(timer);
          if (data.byteLength !== b.bytes) reject(new ErgError(`blob ${b.ref}: ${data.byteLength} bytes, expected ${b.bytes}`));
          else {
            blobs.set(b.ref, data);
            resolve();
          }
        });
      })));
      return { scene, blobs };
    },
    async save(patch: Patch) {
      if (!project) throw new ErgError("no project is open");
      const v = validatePatch(patch, JSON.stringify(patch).length);
      if (!v.ok) throw new ErgError("the patch is invalid", v.errors);
      return client.call<{ saved: boolean; warnings: string[] }>("level.save", { project, patch }, callTimeout);
    },
    async test(opts?: TestOptions) {
      if (!project) throw new ErgError("no project is open");
      const params: Record<string, unknown> = { project };
      if (opts?.tod) params.tod = opts.tod;
      if (opts?.cleanMods !== undefined) params.cleanMods = opts.cleanMods;
      if (opts?.postfx !== undefined) params.postfx = opts.postfx;
      if (opts?.teams) params.teams = opts.teams;
      return client.call<{ key: string; state: string; cleanRoom?: { simMods: boolean; postfx: boolean } }>("level.test", params, callTimeout);
    },
    async script() {
      if (!project) throw new ErgError("no project is open");
      const r = await client.call<{ text: string }>("level.script.get", { project }, callTimeout);
      return typeof r?.text === "string" ? r.text : "";
    },
    async saveScript(text: string) {
      if (!project) throw new ErgError("no project is open");
      return client.call<{ saved: boolean; problems: ScriptProblem[]; syntaxChecked?: boolean }>("level.script.put", { project, text }, callTimeout);
    },
    onTest(fn) {
      return client.subscribe<{ state?: string; key?: string; detail?: string }>("erg", undefined, (m) => {
        if (m && typeof m.state === "string") fn({ state: m.state, key: m.key ?? "", detail: m.detail ?? "" });
      });
    },
  };
}
