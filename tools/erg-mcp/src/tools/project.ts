// Tool group "project": the connection, projects (list, new, open, save, close), undo and redo, and describe_level.
import { WORLD_PER_XAN, isSolid, type Detail, type Role, type Vec3 } from "../core";
import { grids, limitProblems, tidy, toWorld, usage } from "../lib/voxel";
import { ToolError, type Tool } from "../registry";
import type { Session } from "../session";

const discard = { type: "boolean", description: "Drop unsaved edits of the open project instead of refusing." } as const;

const masked = (url: string) => url.replace(/k=[^&]+/, "k=...");

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
  const voxelFrames = grids(store).map((g) => {
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
      return { connected: true, via: c.via, url: masked(c.url), server: c.server, ergMethods: c.methods.filter((m) => m.startsWith("level")),
        test: c.methods.includes("level.test") ? "level.test is available (the game is running)" : "no level.test: start the game for Test" };
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
      return { bases, projects: l.projects, open: s.project?.id ?? null };
    },
  },
  {
    name: "new_project",
    description: "Start a project from a base level (list_bases). The server may pick another id when the slug is taken; the answer has the id. Does not open it.",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["base", "slug", "title"],
      properties: {
        base: { type: "string", description: "Base key from list_bases, e.g. Multi.DinerMight." },
        slug: { type: "string", description: "Project id wish: 1-24 characters a-z, 0-9." },
        title: { type: "string", description: "Level title: 1-40 printable ASCII characters." },
        source: { type: "string", enum: ["game", "pack"], description: "Where the base comes from (default: the game's)." },
      },
    },
    async handler(s, a: { base: string; slug: string; title: string; source?: "game" | "pack" }) {
      if (!/^[a-z0-9]{1,24}$/.test(a.slug)) throw new ToolError("slug must be 1-24 characters a-z, 0-9");
      const r = await (await s.levels()).create(a.base, a.slug, a.title, a.source);
      const { patch: _patch, ...info } = r;
      return { created: info, next: `open_project {id: "${info.id}"}` };
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
    description: "The open project: voxel frames, largest first (id, size, position and bounds in map and world units, solid count), the level's extent, spawns, objects, water, theme, time of day, and usage against the patch limits (64 added blocks of at most 32 voxels a side, 2000 runs per frame, 20000 ops, 4 MB, 256 objects).",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        allFrames: { type: "boolean", description: "Also list frames without voxels (folders, markers)." },
        search: { type: "string", description: "Only voxel frames whose name contains this (any case)." },
        minVoxels: { type: "integer", minimum: 0, description: "Only voxel frames with at least this many voxels." },
        limit: { type: "integer", minimum: 0, maximum: 5000, description: "Voxel frames to list (default 100)." },
      },
    },
    handler: (s, a: FrameQuery) => describe(s, a),
  },
  {
    name: "save",
    description: "Save the open project (level.save with the editor's patch). Refused before anything is sent while the patch breaks a server limit or the patch validator objects (describe_level lists the problems). Answers the server's warnings.",
    inputSchema: { type: "object", additionalProperties: false, properties: {} },
    async handler(s) {
      const r = await s.save();
      return { ...r, ...state(s), patchBytes: usage(s.store).patchBytes.used };
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
