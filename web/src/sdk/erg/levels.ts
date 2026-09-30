// The rest of the level service beside an open session: bases and projects, new projects, themes, the palette, export,
// building a source pack, and loading a base read-only.
import type { Client } from "../client";
import { validateScene, type Patch, type Role, type Scene } from "./scene";
import { ErgError } from "./session";

export interface BaseInfo {
  key: string; stem: string; title: string; source: "game" | "pack"; theme: string;
  mod?: string; built?: boolean;               // pack levels only
}
export interface ProjectInfo { id: string; title: string; stem: string; base: string; modified: string; built: boolean; }
export interface LevelList { bases: BaseInfo[]; projects: ProjectInfo[]; }
export interface Themes { themes: string[]; timesOfDay: string[]; materialFiles: string[]; }
export interface PaletteEntry { name: string; resource: string; role: Role; preview: string | null; }
export interface ExportResult { dir: string; files: string[]; restartRequired: boolean; }
export interface BuildResult { modId: string; dir: string; levels: { slug: string; stem: string; files: string[] }[]; skipped: string[]; }
export type ExportMode = "install" | "source";

export interface LevelService {
  list(): Promise<LevelList>;
  create(base: string, slug: string, title: string, source?: "game" | "pack"): Promise<ProjectInfo & { patch: Patch }>;
  loadBase(key: string, source?: "game" | "pack"): Promise<{ scene: Scene; blobs: Map<number, ArrayBuffer> }>;
  themes(): Promise<Themes>;
  palette(theme: string): Promise<PaletteEntry[]>;
  exportProject(project: string, modId: string, name: string, version: string, mode: ExportMode): Promise<ExportResult>;
  buildMod(modId: string): Promise<BuildResult>;
  close(project: string): Promise<void>;
}

export interface LevelServiceOptions { blobTimeoutMs?: number; callTimeoutMs?: number; }

/** Waits for one bin frame per scene blob (they follow the level.load result) and checks each size. */
export function receiveBlobs(client: Client, scene: Scene, timeoutMs = 30000): Promise<Map<number, ArrayBuffer>> {
  const blobs = new Map<number, ArrayBuffer>();
  return Promise.all(scene.blobs.map((b) => new Promise<void>((resolve, reject) => {
    const timer = setTimeout(() => {
      off();
      reject(new ErgError(`blob ${b.ref} did not arrive within ${timeoutMs} ms`));
    }, timeoutMs);
    const off = client.onBinary(b.ref, (data) => {
      off();
      clearTimeout(timer);
      if (data.byteLength !== b.bytes) reject(new ErgError(`blob ${b.ref}: ${data.byteLength} bytes, expected ${b.bytes}`));
      else {
        blobs.set(b.ref, data);
        resolve();
      }
    });
  }))).then(() => blobs);
}

export function createLevelService(client: Client, opts: LevelServiceOptions = {}): LevelService {
  const callTimeout = opts.callTimeoutMs ?? 30000, blobTimeout = opts.blobTimeoutMs ?? 30000;
  const call = <T>(m: string, p: object, ms = callTimeout) => client.call<T>(m, p, ms);
  return {
    list: () => call<LevelList>("level.list", {}),
    create: (base, slug, title, source) => call<ProjectInfo & { patch: Patch }>("level.new", source ? { base, slug, title, source } : { base, slug, title }),
    async loadBase(key, source) {
      const scene = await call<Scene>("level.load", source ? { base: key, source } : { base: key });
      const v = validateScene(scene);
      if (!v.ok) throw new ErgError("the server sent an invalid scene", v.errors);
      return { scene, blobs: await receiveBlobs(client, scene, blobTimeout) };
    },
    themes: () => call<Themes>("level.themes", {}),
    palette: async (theme) => (await call<{ theme: string; entries: PaletteEntry[] }>("level.palette", { theme })).entries,
    exportProject: (project, modId, name, version, mode) => call<ExportResult>("level.export", { project, modId, name, version, mode }, 120000),
    buildMod: (modId) => call<BuildResult>("level.build", { modId }, 120000),
    async close(project) {
      await call("level.close", { project });
    },
  };
}
