// What one erg-mcp process holds: the Oasis connection and at most one open project. A project is the editor's own
// EditorStore (pinned base, working scene, voxels, CommandStack), so every edit goes through the panel's commands and
// save() sends exactly the patch the panel would (toPatch).
import {
  EditorStore, createLevelService, openProject, type ErgSession, type LevelService, type Loaded, type OasisClient,
  type ProjectInfo,
} from "./core";
import { connect as openConnection, type ConnectOptions, type Connection } from "./oasis";
import { limitProblems } from "./lib/voxel";
import { ToolError } from "./registry";

const TIMEOUT = 30000;

export interface Project {
  id: string;
  info: ProjectInfo;
  store: EditorStore;          // scene, voxels, baseVoxels, stack, frames, patch(), problems(), exec/undo/redo
  erg: ErgSession;             // level.save, level.script.*, level.test for this project
  base: Loaded;                // the pinned base as loaded (scene + blobs), read-only
}

export class Session {
  conn?: Connection;
  project?: Project;
  private levelService?: LevelService;

  /** (Re)connects; an open project is dropped first (refused when it has unsaved edits, unless discard). */
  async connect(o: ConnectOptions = {}, discard = false): Promise<Connection> {
    if (this.project) await this.close(discard);
    this.conn?.client.close();
    this.conn = undefined;
    this.levelService = undefined;
    this.conn = await openConnection(o);
    return this.conn;
  }

  /** The connection, made on first use from the default sources. */
  async ensure(): Promise<Connection> {
    if (this.conn && this.conn.client.state !== "offline") return this.conn;
    return this.connect();
  }

  async client(): Promise<OasisClient> { return (await this.ensure()).client; }

  async levels(): Promise<LevelService> {
    const c = await this.client();
    return (this.levelService ??= createLevelService(c, { callTimeoutMs: TIMEOUT, blobTimeoutMs: TIMEOUT }));
  }

  /** The open project, or a ToolError naming the tool to call first. */
  requireProject(): Project {
    if (!this.project) throw new ToolError("no project is open: call open_project first (new_project makes one)");
    return this.project;
  }

  get store(): EditorStore { return this.requireProject().store; }

  async open(id: string, discard = false): Promise<Project> {
    if (this.project) await this.close(discard);
    const client = await this.client();
    const list = await (await this.levels()).list();
    const info = list.projects.find((p) => p.id === id);
    if (!info) throw new ToolError(`no project '${id}' (list_bases lists the projects)`);
    const opened = await openProject(client, id, info.base).catch((e: Error) => { throw olderServer(e, client.welcome()?.build); });
    const store = new EditorStore(id, opened.base, opened.current);
    this.project = { id, info, store, erg: opened.session, base: opened.base };
    return this.project;
  }

  async save(): Promise<{ saved: boolean; warnings: string[]; problems: string[] }> {
    const p = this.requireProject();
    const patch = p.store.patch();
    const text = JSON.stringify(patch);
    // The limits and the patch validator are checked here, before anything is sent: undo what breaks them.
    const problems = limitProblems(p.store);
    if (problems.length) throw Object.assign(new ToolError("not saved: the patch breaks the server's limits or rules"), { details: problems });
    const r = await p.erg.save(patch);
    if (r.saved === false) throw new ToolError("the server did not save the project");
    p.store.markSaved(text);
    return { saved: true, warnings: r.warnings ?? [], problems };
  }

  /** Closes the open project on the server (level.close) and here. Unsaved edits are refused unless discard. */
  async close(discard = false): Promise<string | null> {
    const p = this.project;
    if (!p) return null;
    if (p.store.dirty && !discard) throw new ToolError(`project '${p.id}' has unsaved edits: save first, or pass discard: true`);
    this.project = undefined;
    try {
      await (await this.levels()).close(p.id);
    } catch {
      // the server drops its lock with the connection anyway
    }
    return p.id;
  }
}

/** A project the server refuses to load because its format is newer than the server (an older Melange.exe, such as a
 * game folder's own, refuses a patch that sets spawns.mode "teams" with `spawns.mode: must be "random" or "knots"`, and
 * one with spawn areas with `spawns: unknown key 'areas'`): the error says so, and which program to run instead. Other
 * errors pass through. */
export function olderServer(e: Error, build?: string): Error {
  const m = e.message ?? String(e);
  if (!/must be|unknown (key|field|op)|not allowed/i.test(m)) return e;
  const teams = /spawns\.mode: must be "?random"? or "?knots"?/.test(m);
  const areas = /spawns: unknown key '(areas|pattern|opposite)'/.test(m) || /spawns\.mode: must be "?random"?, "?knots"? or "?teams"?/.test(m);
  return new ToolError(`the server${build ? ` (Melange ${build})` : ""} refused the project: ${m}. ` + (teams
    ? "That server is older than the project's format: this project uses team spawns (spawns.mode \"teams\", place_spawn_knots {mode: \"teams\"}), which it does not know. "
    : areas ? "That server is older than the project's format: this project uses spawn areas (spawns.mode \"areas\", place_spawn_knots {mode: \"areas\"}), which it does not know. "
    : "The server may be older than the project's format (a newer erg-mcp wrote something it does not know). ") +
    "Connect to a Melange.exe of the same branch as this erg-mcp: set MELANGE_EXE to it for connect {gameDir}, or run that game.");
}
