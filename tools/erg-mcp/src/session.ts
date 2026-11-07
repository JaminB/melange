// What one erg-mcp process holds: the Oasis connection and at most one open project. A project is the editor's own
// EditorStore (pinned base, working scene, voxels, CommandStack), so every edit goes through the panel's commands and
// save() sends exactly the patch the panel would (toPatch).
import {
  EditorStore, createLevelService, openProject, type ErgSession, type LevelService, type Loaded, type OasisClient,
  type ProjectInfo,
} from "./core";
import { connect as openConnection, type ConnectOptions, type Connection } from "./oasis";
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
    const opened = await openProject(client, id, info.base);
    const store = new EditorStore(id, opened.base, opened.current);
    this.project = { id, info, store, erg: opened.session, base: opened.base };
    return this.project;
  }

  async save(): Promise<{ saved: boolean; warnings: string[]; problems: string[] }> {
    const p = this.requireProject();
    const patch = p.store.patch();
    const text = JSON.stringify(patch);
    const problems = p.store.problems();
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
