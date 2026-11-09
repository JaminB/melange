// Tool group "project": the connection, projects (list, new, open, save, close), undo and redo, and describe_level.
import { WORLD_PER_XAN, engineWarnings, isSolid, type Detail, type Role, type Vec3 } from "../core";
import { grids, limitProblems, tidy, toWorld, usage } from "../lib/voxel";
import { ToolError, type Tool } from "../registry";
import type { Session } from "../session";
import { currentHash, factoryReport, spawnTreadWarnings } from "./place";
import { backdropOf, coverLevel, waterBackdropWarning } from "../lib/backdrop";

const discard = { type: "boolean", description: "Drop unsaved edits of the open project instead of refusing." } as const;

const masked = (url: string) => url.replace(/k=[^&]+/, "k=...");

const PROJECTS_NOTE = "the server's projects folder ([Erg] ProjectsDir in its game's Melange.ini, else Documents\\Melange\\erg\\projects). " +
  "Test in the game reads the game's own folder, so a project made through Melange.exe --serve of another install is seen there only when both use the same folder.";

/** The server's projects folder (level.list's projectsDir), or null from an older server. */
async function projectsDir(s: Session): Promise<string | null> {
  try {
    const dir = ((await (await s.levels()).list()) as { projectsDir?: unknown }).projectsDir;
    return typeof dir === "string" ? dir : null;
  } catch {
    return null;
  }
}


function state(s: Session) {
  const p = s.project;
  if (!p) return { open: null };
  const top = p.store.stack.peek();
  return { open: p.id, dirty: p.store.dirty, canUndo: p.store.stack.canUndo, canRedo: p.store.stack.canRedo, undoDepth: p.store.stack.depth,
    next: top ? `undo: ${top.label}` : null };
}

export interface FrameQuery { allFrames?: boolean; search?: string; minVoxels?: number; limit?: number; }

/** Level maps are many small voxel frames (Multi.DinerMight: 534), so the list is the largest first, filtered and capped. */
export function describe(s: Session, q: FrameQuery = {}) {
  const { store, info } = s.requireProject();
  const scene = store.scene;
  const pos = (d: Pick<Detail, "frame" | "pos">) => {
    const m = store.frames.detailWorld(d);
    return { map: tidy(m), world: tidy(toWorld(m)) };
  };
  const trims = new Map((scene.trims ?? []).map((t) => [t.frame, t]));
  const voxelFrames = grids(store).map((g) => {
    const trim = trims.get(g.id);
    let solid = 0;
    for (const w of g.words) if (isSolid(w)) solid++;
    const origin = store.frames.worldOf(g.id);
    const at: Vec3 = origin ? [origin[3], origin[7], origin[11]] : g.frame.pos;
    return {
      id: g.id, name: g.frame.name, parent: g.frame.parent, ...(g.frame.new ? { new: true } : {}),
      size: g.size, voxels: g.words.length, solid,
      pos: { map: tidy(at), world: tidy(toWorld(at)) },
      bounds: { map: { min: tidy(g.bounds.min), max: tidy(g.bounds.max) }, world: { min: tidy(toWorld(g.bounds.min)), max: tidy(toWorld(g.bounds.max)) } },
      ...(g.frame.rot.some((r) => r) ? { rot: tidy(g.frame.rot) } : {}),
      ...(g.frame.scale.some((v) => v !== 1) ? { scale: tidy(g.frame.scale) } : {}),
      // trim_base_frames: the box the built level keeps, and the game's cells it costs
      ...(trim ? { trim: { min: trim.min, size: trim.size, cells: trim.size[0] * trim.size[1] * trim.size[2] } } : {}),
    };
  });
  const withVoxels = new Set(voxelFrames.map((f) => f.id));
  const extent = voxelFrames.reduce((b, f) => ({ min: b.min.map((v, a) => Math.min(v, f.bounds.map.min[a])) as Vec3,
    max: b.max.map((v, a) => Math.max(v, f.bounds.map.max[a])) as Vec3 }), { min: [Infinity, Infinity, Infinity] as Vec3, max: [-Infinity, -Infinity, -Infinity] as Vec3 });
  const needle = q.search?.toLowerCase();
  const listed = voxelFrames.filter((f) => f.voxels >= (q.minVoxels ?? 0) && (!needle || f.name.toLowerCase().includes(needle)))
    .sort((a, b) => b.voxels - a.voxels || a.id - b.id);
  const limit = q.limit ?? 100;
  const others = scene.frames.filter((f) => !withVoxels.has(f.id));
  const roles: Partial<Record<Role, number>> = {};
  for (const d of scene.details) roles[d.role] = (roles[d.role] ?? 0) + 1;
  const knotOf = new Map(scene.details.filter((d) => d.src === null).map((d) => [d.name, d]));
  const water = scene.water.level;
  return {
    project: { id: info.id, title: scene.title, stem: scene.stem, base: scene.base.key, source: scene.base.source, file: scene.base.file },
    ...state(s),
    units: { worldPerMap: WORLD_PER_XAN, note: "map units are the level file's (1 voxel = 1 map unit); world units = map x 20, as the Erg panel shows them" },
    format: scene.format,
    theme: scene.databank.theme, timeOfDay: scene.databank.timeOfDay, materialFile: scene.databank.materialFile,
    water: { world: water, map: water === null ? null : water / WORLD_PER_XAN, note: water === null ? "the level's own water height" : undefined },
    spawnMode: scene.spawns.mode, surround: scene.hmp.mode, survivor: !!scene.kind?.survivor, script: !!scene.script?.present,
    extent: voxelFrames.length ? { map: { min: tidy(extent.min), max: tidy(extent.max) }, world: { min: tidy(toWorld(extent.min)), max: tidy(toWorld(extent.max)) } } : null,
    voxelFrames: { total: voxelFrames.length, matching: listed.length, listed: Math.min(limit, listed.length),
      solid: voxelFrames.reduce((n, f) => n + f.solid, 0), order: "largest first" },
    frames: listed.slice(0, limit),
    otherFrames: q.allFrames
      ? others.map((f) => ({ id: f.id, name: f.name, parent: f.parent, folder: f.folder, pos: tidy(f.pos) }))
      : { count: others.length, folders: others.filter((f) => f.folder).length, note: "describe_level {allFrames: true} lists them" },
    spawns: scene.details.filter((d) => d.role === "spawn").map((d) => ({ id: d.id, name: d.name, frame: d.frame, ...pos(d) })),
    objects: (scene.objects ?? []).map((o) => {
      const d = knotOf.get(o.knot);
      return { ...o, ...(d ? { detail: d.id, frame: d.frame, ...pos(d) } : {}) };
    }),
    // oil drums, mines and other object details that are not a level object's knot (at most 200)
    placed: scene.details.filter((d) => d.role === "object" && !(d.src === null && scene.objects?.some((o) => o.knot === d.name)))
      .slice(0, 200).map((d) => ({ id: d.id, name: d.name, resource: d.resource, ...pos(d) })),
    details: { total: scene.details.length, byRole: roles },
    limits: usage(store),
    problems: limitProblems(store),
    // what the game would assert or crash on: saved with a warning, refused by export, build and test
    engineWarnings: engineWarnings(scene),
  };
}

export const tools: Tool[] = [
  {
    name: "connect",
    description: "Connect (or reconnect) to the Oasis server that serves Erg. With no arguments: --url, ERG_MCP_URL, Documents\\Melange\\oasis_url.txt (the running game), then Melange.exe --serve for MELANGE_GAME_DIR. Other tools connect on first use the same way.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        url: { type: "string", description: "Launch URL http://127.0.0.1:<port>/?k=<token> (or ws://.../ws?k=<token>)." },
        gameDir: { type: "string", description: "A game folder: start its Melange.exe --serve (or MELANGE_EXE) for it (closed game only)." },
        discard,
      },
    },
    async handler(s, a: { url?: string; gameDir?: string; discard?: boolean }) {
      // No exe argument: which program runs is the user's choice (MELANGE_EXE), never the model's.
      const c = await s.connect({ url: a.url, gameDir: a.gameDir }, a.discard);
      const dir = await projectsDir(s);
      // a served Melange.exe is the game folder's own unless MELANGE_EXE names another: often older than this erg-mcp
      const warnings = c.via.startsWith("Melange.exe --serve") && !process.env.MELANGE_EXE
        ? ["MELANGE_EXE is unset, so the game folder's own Melange.exe serves: when it is older than this erg-mcp it refuses newer project formats (open_project of a team-spawn project fails with spawns.mode: must be \"random\" or \"knots\"); set MELANGE_EXE to the Melange.exe built with this erg-mcp"] : [];
      return { connected: true, via: c.via, url: masked(c.url), server: c.server, ...(c.build ? { build: c.build } : {}), ...(warnings.length ? { warnings } : {}), ergMethods: c.methods.filter((m) => m.startsWith("level")),
        test: c.methods.includes("level.test") ? "level.test is available (the game is running)" : "no level.test: start the game for Test",
        ...(dir ? { projectsDir: dir, projectsNote: PROJECTS_NOTE } : {}) };
    },
  },
  {
    name: "list_bases",
    description: "The levels a project can start from (the game's multiplayer maps and installed map packs) and the existing projects.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        search: { type: "string", description: "Keep bases whose key, title or stem contains this (any case)." },
        theme: { type: "string", description: "Keep bases of this theme, e.g. CAMELOT." },
        source: { type: "string", enum: ["game", "pack"] },
      },
    },
    async handler(s, a: { search?: string; theme?: string; source?: "game" | "pack" }) {
      const l = await (await s.levels()).list();
      const q = a.search?.toLowerCase();
      const bases = l.bases.filter((b) => (!q || `${b.key} ${b.title} ${b.stem}`.toLowerCase().includes(q)) &&
        (!a.theme || b.theme.toUpperCase() === a.theme.toUpperCase()) && (!a.source || b.source === a.source));
      const dir = (l as { projectsDir?: unknown }).projectsDir;
      return { bases, projects: l.projects, open: s.project?.id ?? null, ...(typeof dir === "string" ? { projectsDir: dir } : {}) };
    },
  },
  {
    name: "new_project",
    description: "Start a project from a base level (list_bases). Refused when a project with that slug exists (delete_project removes one, or pass allowRename to take the server's next free id, such as caldera2; the answer then warns). Does not open it.",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["base", "slug", "title"],
      properties: {
        base: { type: "string", description: "Base key from list_bases, e.g. Multi.DinerMight." },
        slug: { type: "string", description: "Project id wish: 1-24 characters a-z, 0-9." },
        title: { type: "string", description: "Level title: 1-40 printable ASCII characters." },
        source: { type: "string", enum: ["game", "pack"], description: "Where the base comes from (default: the game's)." },
        allowRename: { type: "boolean", description: "When the slug is taken, let the server pick the next free id (slug2, slug3, ...) instead of refusing." },
      },
    },
    async handler(s, a: { base: string; slug: string; title: string; source?: "game" | "pack"; allowRename?: boolean }) {
      if (!/^[a-z0-9]{1,24}$/.test(a.slug)) throw new ToolError("slug must be 1-24 characters a-z, 0-9");
      const levels = await s.levels();
      const list = await levels.list();
      const dir = (list as { projectsDir?: unknown }).projectsDir;
      const where = typeof dir === "string" ? ` in ${dir}` : "";
      if (!a.allowRename && list.projects.some((p) => p.id === a.slug))
        throw new ToolError(`project '${a.slug}' exists${where}: open_project it, delete_project it first, or pass allowRename: true for a new id`);
      const r = await levels.create(a.base, a.slug, a.title, a.source);
      const { patch: _patch, ...info } = r;
      return { created: info, ...(info.id !== a.slug ? { warning: `'${a.slug}' is taken: the project's id is '${info.id}'` } : {}),
        ...(typeof dir === "string" ? { projectsDir: dir } : {}), next: `open_project {id: "${info.id}"}` };
    },
  },
  {
    name: "delete_project",
    description: "Delete a project: its folder (patch, script, metadata) under the server's projects folder. Refused while any server (this one, the game or Melange.exe) has it open: close it first. Cannot be undone. Exported mods are not touched.",
    inputSchema: { type: "object", additionalProperties: false, required: ["id"], properties: { id: { type: "string", description: "Project id (list_bases lists them)." } } },
    async handler(s, a: { id: string }) {
      if (s.project?.id === a.id) throw new ToolError(`project '${a.id}' is open here: close it first`);
      const conn = await s.ensure();
      if (!conn.methods.includes("level.delete")) throw new ToolError("this server has no level.delete (an older Melange): delete the project's folder by hand");
      const r = await (await s.client()).call<{ deleted: string }>("level.delete", { project: a.id }, 30000);
      const dir = await projectsDir(s);
      return { deleted: r.deleted, ...(dir ? { projectsDir: dir } : {}) };
    },
  },
  {
    name: "open_project",
    description: "Open a project for editing: loads its base and current state with every voxel blob. Closes the project open before (refused when it has unsaved edits, unless discard).",
    inputSchema: { type: "object", additionalProperties: false, required: ["id"], properties: { id: { type: "string" }, discard } },
    async handler(s, a: { id: string; discard?: boolean }) {
      const t0 = Date.now();
      await s.open(a.id, a.discard);
      const d = describe(s, { limit: 0 });
      return { opened: a.id, ms: Date.now() - t0, project: d.project, theme: d.theme, timeOfDay: d.timeOfDay, water: d.water,
        voxelFrames: d.voxelFrames.total, extent: d.extent, spawns: d.spawns.length, objects: d.objects.length, details: d.details.total, problems: d.problems,
        next: "describe_level for frames, bounds and limits" };
    },
  },
  {
    name: "describe_level",
    description: "The open project: voxel frames, largest first (id, size, position and bounds in map and world units, solid count), the level's extent, spawns, objects, water, theme, time of day, usage against the patch limits (64 added blocks of at most 31 voxels a side, 2000 runs per frame, 20000 ops, 4 MB, 256 objects), the game's voxel cell budget (limits.voxelCells: X*Y*Z of every voxel frame, at most 65535, with cells used and free) and engineWarnings.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        allFrames: { type: "boolean", description: "Also list frames without voxels (folders, markers)." },
        search: { type: "string", description: "Only voxel frames whose name contains this (any case)." },
        minVoxels: { type: "integer", minimum: 0, description: "Only voxel frames with at least this many voxels." },
        limit: { type: "integer", minimum: 0, maximum: 5000, description: "Voxel frames to list (default 100)." },
        backdrop: { type: "boolean", description: "Also list the base's backdrop (list_backdrop: ground planes and distant set pieces, with their bounds and the water that covers the planes), to hide with remove_backdrop." },
      },
    },
    async handler(s, a: FrameQuery & { backdrop?: boolean }) {
      const { backdrop, ...q } = a;
      const d = describe(s, q);
      if (!backdrop) return d;
      const b = await backdropOf(s, s.store.scene, grids(s.store));
      const c = coverLevel(b.pieces, b.extent), r = (v: number) => Math.round(v * 100) / 100;
      return { ...(d as object), backdrop: { count: b.pieces.length, coverLevel: c ? { map: r(c.level), world: r(c.level * 20), piece: c.piece.detail.id } : null,
        pieces: b.pieces.map((p) => ({ id: p.detail.id, name: p.detail.name, resource: p.detail.resource, kind: p.kind, top: { map: r(p.bounds.max[1]), world: r(p.bounds.max[1] * 20) },
          size: [0, 1, 2].map((i) => r(p.bounds.max[i] - p.bounds.min[i])) })), note: "map units for size; remove_backdrop removes them, list_backdrop has their bounds" } };
    },
  },
  {
    name: "save",
    description: "Save the open project (level.save with the editor's patch). Refused before anything is sent while the patch breaks a server limit or the patch validator objects (describe_level lists the problems). Answers the server's warnings, the mine factory's check (factory) and `spawns`: spawn knots a worm cannot stand on (inside solid, no ground within 3 map units under it, on an edge narrower than a worm), each with the nearest point that passes (place_spawn_knots {snap: \"tread\"} moves them there).",
    inputSchema: { type: "object", additionalProperties: false, properties: {} },
    async handler(s) {
      const r = await s.save();
      // the mine factory as the game will place it: a factory the match loses at load is saved all the same, and said so
      const factory = await factoryReport(s, "world").catch(() => [] as string[]);
      const sea = await waterBackdropWarning(s).catch(() => null);
      // spawn knots a worm cannot stand on (in solid, no ground near under it, on an edge): saved all the same, and said so
      const spawns = (() => { try { return spawnTreadWarnings(s.store); } catch { return [] as string[]; } })();
      // the hash test {expectHash} checks the game's project against (the patch and the script, not the stem or title)
      const projectHash = await currentHash(s).catch(() => undefined);
      return { ...r, ...(projectHash ? { projectHash } : {}), ...(factory.length ? { factory } : {}), ...(sea?.warning ? { water: sea } : {}), ...(spawns.length ? { spawns } : {}), ...state(s), patchBytes: usage(s.store).patchBytes.used };
    },
  },
  {
    name: "undo",
    description: "Undo the last edit of the open project (the editor's undo stack, up to 500 steps).",
    inputSchema: { type: "object", additionalProperties: false, properties: {} },
    handler(s) {
      const st = s.store;
      const label = st.stack.peek()?.label;
      if (!st.undo()) throw new ToolError("nothing to undo");
      return { undone: label, ...state(s) };
    },
  },
  {
    name: "redo",
    description: "Redo the last undone edit of the open project.",
    inputSchema: { type: "object", additionalProperties: false, properties: {} },
    handler(s) {
      const st = s.store;
      if (!st.redo()) throw new ToolError("nothing to redo");
      return { redone: st.stack.peek()?.label, ...state(s) };
    },
  },
  {
    name: "close",
    description: "Close the open project (refused when it has unsaved edits, unless discard).",
    inputSchema: { type: "object", additionalProperties: false, properties: { discard } },
    async handler(s, a: { discard?: boolean }) {
      const id = await s.close(a.discard);
      return id ? { closed: id } : { closed: null, note: "no project was open" };
    },
  },
];
