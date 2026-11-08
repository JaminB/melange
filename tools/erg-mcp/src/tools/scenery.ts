// Tool group "scenery": the level's scenery details (the base map's meshes: buildings, props, glass and water volumes)
// and copies of another base's pieces for decoration. A scenery detail is drawn by the game but a copy has no collision
// (it is not part of the landscape): worms and weapons pass through it, so copies are decoration only. The Erg panel
// does not copy scenery for that reason; these tools do it on request and say so.
import { AddDetail, Frames, Group, RemoveDetail, targetFrame, type Detail, type Scene, type Vec3 } from "../core";
import { DEG, commit, pointOut, resolvePoint, unitsSchema, vec3, type Units } from "../lib/place-geom";
import { tidy } from "../lib/voxel";
import { ToolError, type Schema, type Tool } from "../registry";
import type { Session } from "../session";

const ROLES = new Set(["scenery", "other", "emitter", "collision", "marker", "light", "sound", "camera"]);

/** A base level's scene, loaded read-only once per connection. */
const bases = new WeakMap<object, Map<string, Promise<Scene>>>();
async function baseScene(s: Session, key: string, source: "game" | "pack"): Promise<Scene> {
  const client = await s.client();
  let per = bases.get(client);
  if (!per) bases.set(client, (per = new Map()));
  const k = `${source}:${key}`;
  if (!per.has(k)) per.set(k, (await s.levels()).loadBase(key, source).then((r) => r.scene));
  try {
    return await per.get(k)!;
  } catch (e) {
    per.delete(k);
    throw new ToolError(`could not load base '${key}': ${e instanceof Error ? e.message : String(e)}`);
  }
}

/** A detail's position in the level, map units, through its frames (the editor's own placement). */
const levelPos = (scene: Scene, d: Detail): Vec3 => new Frames(scene).detailWorld(d);

interface Filter { search?: string; role?: string; near?: { x: number; z: number; radius: number }; box?: { min: number[]; max: number[] }; }

function filtered(scene: Scene, a: Filter, units: Units, pos: (d: Detail) => Vec3) {
  const q = a.search?.toLowerCase(), k = units === "world" ? 1 / 20 : 1;
  return scene.details.filter((d) => {
    if (a.role ? d.role !== a.role : !ROLES.has(d.role)) return false;
    if (q && !`${d.name} ${d.resource}`.toLowerCase().includes(q)) return false;
    if (!a.near && !a.box) return true;
    const p = pos(d);
    if (a.near && Math.hypot(p[0] - a.near.x * k, p[2] - a.near.z * k) > a.near.radius * k) return false;
    if (a.box && [0, 1, 2].some((i) => p[i] < a.box!.min[i] * k || p[i] > a.box!.max[i] * k)) return false;
    return true;
  });
}

const filterProps: Record<string, Schema> = {
  search: { type: "string", description: "Name or resource contains this (any case)." },
  role: { type: "string", enum: [...ROLES], description: "Only this role (default: every role that is not a spawn or an object)." },
  near: { type: "object", additionalProperties: false, required: ["x", "z", "radius"], description: "Only details within radius of x, z (the call's units).",
    properties: { x: { type: "number" }, z: { type: "number" }, radius: { type: "number", minimum: 0 } } },
  box: { type: "object", additionalProperties: false, required: ["min", "max"], description: "Only details inside this box (the call's units).",
    properties: { min: vec3("[x, y, z]"), max: vec3("[x, y, z]") } },
};

export const tools: Tool[] = [
  {
    name: "list_scenery",
    description: "The open level's scenery and other non-gameplay details (meshes, glass and water volumes, emitters, lights, markers) with their position in the level, frame, resource and whether the base has it: filter by search, role, near a point or in a box. Use it to find a base's leftover meshes (for example a translucent box over a wiped map) and remove them with remove_scenery. Copies of scenery have no collision.",
    inputSchema: { type: "object", additionalProperties: false, properties: { ...filterProps, units: unitsSchema,
      limit: { type: "integer", minimum: 1, maximum: 2000, description: "Details to list (default 200), nearest first with near." } } },
    handler(s, a: Filter & { units?: Units; limit?: number }) {
      const { store } = s.requireProject();
      const u = a.units ?? "world";
      const pos = (d: Detail) => store.frames.detailWorld(d);
      let list = filtered(store.scene, a, u, pos);
      if (a.near) { const k = u === "world" ? 1 / 20 : 1; list = list.sort((x, y) => Math.hypot(pos(x)[0] - a.near!.x * k, pos(x)[2] - a.near!.z * k) - Math.hypot(pos(y)[0] - a.near!.x * k, pos(y)[2] - a.near!.z * k)); }
      const byRes = new Map<string, number>();
      for (const d of list) byRes.set(d.resource, (byRes.get(d.resource) ?? 0) + 1);
      const frameName = (id: number) => store.scene.frames.find((f) => f.id === id)?.name ?? null;
      return {
        total: list.length, byResource: Object.fromEntries([...byRes].sort((x, y) => y[1] - x[1]).slice(0, 60)), units: u,
        details: list.slice(0, a.limit ?? 200).map((d) => ({ id: d.id, name: d.name, resource: d.resource, role: d.role, frame: d.frame, frameName: frameName(d.frame),
          base: d.src !== null, at: pointOut(pos(d), u), rot: tidy(d.rot.map((v) => v * DEG) as Vec3), scale: tidy(d.scale) })),
      };
    },
  },
  {
    name: "remove_scenery",
    description: "Remove scenery details from the open level (base meshes included: the patch removes them from the built map), by ids or by the same filters as list_scenery (search, role, near, box; at least one). dryRun lists what would go. One undo step. The level's surround ring is set_surround_mode (none removes it).",
    inputSchema: { type: "object", additionalProperties: false, properties: { ...filterProps, units: unitsSchema,
      ids: { type: "array", items: { type: "integer" }, maxItems: 2000, description: "Detail ids (list_scenery)." }, dryRun: { type: "boolean" } } },
    handler(s, a: Filter & { units?: Units; ids?: number[]; dryRun?: boolean }) {
      const { store } = s.requireProject();
      const u = a.units ?? "world";
      if (!a.ids && !a.search && !a.role && !a.near && !a.box) throw new ToolError("name what to remove: ids, or search, role, near or box");
      const pos = (d: Detail) => store.frames.detailWorld(d);
      let list = a.ids ? a.ids.map((id) => { const d = store.detail(id); if (!d) throw new ToolError(`no detail ${id}`); return d; }) : filtered(store.scene, a, u, pos);
      list = list.filter((d) => d.role !== "spawn" && d.role !== "object");
      if (!list.length) throw new ToolError("nothing matches (spawns and objects are removed with remove)");
      const out = list.map((d) => ({ id: d.id, name: d.name, resource: d.resource, at: pointOut(pos(d), u) }));
      if (a.dryRun) return { dryRun: true, wouldRemove: out.length, details: out.slice(0, 200) };
      commit(store, list.length === 1 ? new RemoveDetail(list[0].id) : new Group(`Remove ${list.length} scenery details`, list.map((d) => new RemoveDetail(d.id))));
      return { removed: out.length, details: out.slice(0, 200), undo: "undo puts them back" };
    },
  },
  {
    name: "list_base_scenery",
    description: "The scenery pieces of a base level (default the open project's; e.g. Multi.StormTheCastle or Multi.NiceToSiege for Camelot castle pieces: walls, towers, flags, statues), grouped by resource with an example of each (detail id, name, position in that base, rotation, scale). copy_base_scenery places copies.",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      base: { type: "string", description: "Base key (list_bases); default the open project's base." }, source: { type: "string", enum: ["game", "pack"] },
      search: { type: "string", description: "Name or resource contains this (any case)." }, limit: { type: "integer", minimum: 1, maximum: 500, description: "Resources to list (default 100)." } } },
    async handler(s, a: { base?: string; source?: "game" | "pack"; search?: string; limit?: number }) {
      const key = a.base ?? s.project?.store.scene.base.key;
      if (!key) throw new ToolError("pass base, or open a project");
      const source = a.source ?? (a.base ? "game" : s.project!.store.scene.base.source);
      const scene = await baseScene(s, key, source);
      const q = a.search?.toLowerCase();
      const groups = new Map<string, Detail[]>();
      for (const d of scene.details) {
        if (d.role !== "scenery" || (q && !`${d.name} ${d.resource}`.toLowerCase().includes(q))) continue;
        groups.set(d.resource, [...(groups.get(d.resource) ?? []), d]);
      }
      const list = [...groups].sort((x, y) => y[1].length - x[1].length).slice(0, a.limit ?? 100).map(([resource, ds]) => ({
        resource, count: ds.length, names: [...new Set(ds.map((d) => d.name))].slice(0, 4),
        example: { id: ds[0].id, name: ds[0].name, at: tidy(levelPos(scene, ds[0])), rot: tidy(ds[0].rot.map((v) => v * DEG) as Vec3), scale: tidy(ds[0].scale) },
      }));
      return { base: key, theme: scene.databank.theme, resources: groups.size, units: "map", list,
        note: "copies draw but have no collision (decoration only); names tagged VISIBLE_ or MORTAL_ keep their engine meaning" };
    },
  },
  {
    name: "copy_base_scenery",
    description: "Place copies of a base level's scenery pieces in the open level, for decoration: each copy is `id` (a detail of that base, from list_base_scenery) or `resource`, at `pos` or `xz` (dropped onto the voxels), with its own rot (degrees) and scale (default the source's). Copies draw in the game but have NO collision: worms and shots pass through them, so build walls that matter with the structure tools. A piece of another theme than the level's may not be loaded by the game: refused unless allowOtherTheme. One undo step.",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["copies"],
      properties: {
        base: { type: "string", description: "Base key to copy from (default the open project's base)." }, source: { type: "string", enum: ["game", "pack"] },
        copies: { type: "array", minItems: 1, maxItems: 64, items: { type: "object", additionalProperties: false, properties: {
          id: { type: "integer", description: "A detail id of the base." }, resource: { type: "string", description: "Or a resource: its first piece in the base." },
          pos: vec3("[x, y, z] in the level."), xz: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2, description: "[x, z]: dropped onto the voxels below." },
          drop: { type: "boolean", description: "Drop pos onto the voxels below it." },
          rot: vec3("Rotation in degrees about x, y, z (default the source's)."), scale: { type: "number", minimum: 0.05, maximum: 20, description: "Uniform scale (default the source's)." },
        } } },
        allowOtherTheme: { type: "boolean", description: "Copy from a base of another theme than the level's (the game may not load its meshes)." },
        units: unitsSchema,
      },
    },
    async handler(s, a: { base?: string; source?: "game" | "pack"; copies: { id?: number; resource?: string; pos?: number[]; xz?: number[]; drop?: boolean; rot?: number[]; scale?: number }[];
      allowOtherTheme?: boolean; units?: Units }) {
      const { store } = s.requireProject();
      const key = a.base ?? store.scene.base.key;
      const scene = await baseScene(s, key, a.source ?? (a.base ? "game" : store.scene.base.source));
      if (scene.databank.theme !== store.scene.databank.theme && !a.allowOtherTheme)
        throw new ToolError(`${key} is ${scene.databank.theme} and this level ${store.scene.databank.theme}: the game may not load another theme's meshes. Pass allowOtherTheme to try (and Test it), or set_theme`);
      const u = a.units ?? "world";
      const frame = targetFrame(store.scene, "scenery");
      const cmds: AddDetail[] = [], placed: object[] = [];
      a.copies.forEach((c, i) => {
        const src = c.id !== undefined ? scene.details.find((d) => d.id === c.id) : scene.details.find((d) => d.resource.toLowerCase() === (c.resource ?? "").toLowerCase());
        if (!src) throw new ToolError(`copies[${i}]: ${c.id !== undefined ? `no detail ${c.id}` : `no piece with resource '${c.resource}'`} in ${key}`);
        if (src.role !== "scenery") throw new ToolError(`copies[${i}]: detail ${src.id} (${src.name}) is a ${src.role}, not scenery`);
        const at = resolvePoint(store, c, u, `copies[${i}]`).map;
        const rot: Vec3 = c.rot ? [c.rot[0] / DEG, c.rot[1] / DEG, c.rot[2] / DEG] : [src.rot[0], src.rot[1], src.rot[2]];
        const scale: Vec3 = c.scale !== undefined ? [c.scale, c.scale, c.scale] : [src.scale[0], src.scale[1], src.scale[2]];
        cmds.push(new AddDetail(frame, { name: src.name, resource: src.resource, pos: store.frames.toLocal(frame, at), rot, scale }));
        placed.push({ from: src.id, name: src.name, resource: src.resource, at: pointOut(at, u) });
      });
      commit(store, cmds.length === 1 ? cmds[0] : new Group(`Copy ${cmds.length} scenery pieces`, cmds));
      return { placed, frame, note: "decoration only: copies have no collision", undo: "undo removes them all" };
    },
  },
];
