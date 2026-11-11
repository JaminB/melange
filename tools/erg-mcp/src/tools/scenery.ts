// Tool group "scenery": the level's scenery details (the base map's meshes: buildings, props, glass and water volumes)
// and copies of another base's pieces for decoration. A scenery detail is drawn by the game but a copy has no collision
// (it is not part of the landscape): worms and weapons pass through it, so copies are decoration only. The Erg panel
// does not copy scenery for that reason; these tools do it on request and say so.
import { AddDetail, Frames, Group, RemoveDetail, targetFrame, type Detail, type Scene, type Vec3 } from "../core";
import { DEG, commit, pointOut, resolvePoint, unitsSchema, vec3, type Units } from "../lib/place-geom";
import { tidy, grids } from "../lib/voxel";
import { backdropOf, coverLevel, type BackdropPiece } from "../lib/backdrop";
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

/** A copy's scale: a number, [x, y, z], or either written as a string ("1.5", "1.5, 1.5, 2"); each 0.05-20. */
export function scaleOf(v: unknown, path: string): Vec3 {
  let x: unknown = v;
  if (typeof x === "string") {
    const parts = x.replace(/^\s*\[|\]\s*$/g, "").split(/[\s,]+/).filter(Boolean).map(Number);
    x = parts.length === 1 ? parts[0] : parts;
  }
  const ok = (n: unknown) => typeof n === "number" && Number.isFinite(n) && n >= 0.05 && n <= 20;
  if (ok(x)) return [x as number, x as number, x as number];
  if (Array.isArray(x) && x.length === 3 && x.every(ok)) return x as Vec3;
  throw new ToolError(`${path}: a scale is a number from 0.05 to 20 (uniform) or [x, y, z] of them, not ${JSON.stringify(v)}`);
}

/** What a piece is, from its detail name: the engine's tags (VISIBLE_, MORTAL_, ...) and numbers dropped, lower case
 * ("VISIBLE_Flag", "VISIBLE_flag2" -> "flag"). */
export function kindOf(name: string): string {
  let k = name;
  for (let i = 0; i < 4; i++) k = k.replace(/^(VISIBLE|MORTAL|INVISIBLE|SLIPPY|PERM|TEAMBASE|NOSHADOW|COLLIDE|NOCOLLIDE)_/i, "");
  k = k.replace(/[_\s-]*\d+$/, "").replace(/[_\s-]+/g, " ").trim().toLowerCase();
  return k || name.toLowerCase();
}

/** A backdrop piece for the answers, in `u`. */
function pieceOut(p: BackdropPiece, u: Units) {
  const k = u === "world" ? 20 : 1, r = (v: number) => Math.round(v * k * 100) / 100;
  const d = p.detail;
  return { id: d.id, name: d.name, resource: d.resource, kind: p.kind, base: d.src !== null,
    bounds: { min: p.bounds.min.map(r), max: p.bounds.max.map(r), ...(p.bounds.mesh ? {} : { note: "no mesh served: its position only" }) },
    size: [0, 1, 2].map((i) => r(p.bounds.max[i] - p.bounds.min[i])), top: r(p.bounds.max[1]) };
}

const BACKDROP_NOTE = "backdrop: scenery that is a flat piece at least 40 map units both ways (plane: a ground or table plane), wholly outside the voxel frames that hold ground (distant: buildings, barrels, set pieces round the level), or wider than half of them (large); bounds are each piece's preview mesh (the Erg panel's) through its detail's matrix. coverLevel is the lowest water that covers the planes reaching out past the level (under it the sea shows the plane)";

export const tools: Tool[] = [
  {
    name: "list_backdrop",
    description: "The open level's backdrop: the base's scenery details round and under the playing area, each with its kind (plane, distant, large), bounds and top from its mesh, and coverLevel: the lowest water that covers the base's ground planes (a sea under it shows the plane). remove_backdrop removes them. Only the level's details: Multi.DinerMight's distant set (the table plane, the barrel, the pink buildings, the mushroom) is not among them (nor its voxel frames), so it is not listed and cannot be removed by a patch.",
    inputSchema: { type: "object", additionalProperties: false, properties: { units: unitsSchema } },
    async handler(s, a: { units?: Units }) {
      const { store } = s.requireProject();
      const u = a.units ?? "world", k = u === "world" ? 20 : 1;
      const b = await backdropOf(s, store.scene, grids(store));
      const c = coverLevel(b.pieces, b.extent);
      return { units: u, count: b.pieces.length, pieces: b.pieces.map((p) => pieceOut(p, u)),
        coverLevel: c ? { level: Math.round(c.level * k * 100) / 100, piece: c.piece.detail.id, name: c.piece.detail.name } : null,
        water: store.scene.water.level === null ? "the level's own" : Math.round((store.scene.water.level / 20) * k * 100) / 100,
        meshes: { measured: b.meshes, positionOnly: b.without }, note: BACKDROP_NOTE,
        ...(b.pieces.length ? {} : { none: "no detail of this level is backdrop. The game's own distant sets (Multi.DinerMight's table plane, barrel, pink buildings and mushroom) are not details or voxel frames of the .xan, so no patch op reaches them; the base's databank (its Databank.CustomDetailBank, BUILDING on DinerMight, which set_theme does not change) is a likely source, not verified" }) };
    },
  },
  {
    name: "remove_backdrop",
    description: "Remove the backdrop list_backdrop finds (the base's ground planes and distant set pieces), all of it or `kinds` (plane, distant, large) or `ids`; `keep` spares ids. dryRun lists what would go. One undo step. The surround ring (the .hmp heightmap) is set_surround_mode none.",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      kinds: { type: "array", items: { type: "string", enum: ["plane", "distant", "large"] }, minItems: 1, maxItems: 3, description: "Only these kinds (default every kind)." },
      ids: { type: "array", items: { type: "integer" }, maxItems: 2000, description: "Only these backdrop pieces." },
      keep: { type: "array", items: { type: "integer" }, maxItems: 2000, description: "Never these." },
      dryRun: { type: "boolean" }, units: unitsSchema } },
    async handler(s, a: { kinds?: string[]; ids?: number[]; keep?: number[]; dryRun?: boolean; units?: Units }) {
      const { store } = s.requireProject();
      const u = a.units ?? "world";
      const b = await backdropOf(s, store.scene, grids(store));
      const list = b.pieces.filter((p) => (!a.kinds || a.kinds.includes(p.kind)) && (!a.ids || a.ids.includes(p.detail.id)) && !(a.keep ?? []).includes(p.detail.id));
      if (!list.length) throw new ToolError(b.pieces.length ? "no backdrop piece matches" : "this level has no backdrop (list_backdrop)");
      const out = list.map((p) => pieceOut(p, u));
      if (a.dryRun) return { dryRun: true, wouldRemove: out.length, pieces: out };
      commit(store, list.length === 1 ? new RemoveDetail(list[0].detail.id) : new Group(`Remove ${list.length} backdrop pieces`, list.map((p) => new RemoveDetail(p.detail.id))));
      return { removed: out.length, pieces: out, undo: "undo puts them back", note: "the sea now shows water down to its floor; set_backdrop puts another theme's backdrop round the level" };
    },
  },
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
    name: "list_scenery_types",
    description: "What kinds of scenery pieces the bases have, by name, so a piece is found without searching by hand: each kind (a detail's name without its engine tags and numbers: flag, pole, candle, tower, ...) with the resources that draw it in each base, e.g. 'flag in Multi.NiceToSiege: CAMELOT16, CAMELOT19', a count and an example detail id for copy_base_scenery. Default bases: every base of the open level's theme (or `theme`); or `bases`. `search` keeps kinds, names or resources containing it.",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      bases: { type: "array", items: { type: "string" }, minItems: 1, maxItems: 40, description: "Base keys (list_bases); default every base of the theme." },
      theme: { type: "string", description: "The theme whose bases are listed (default the open level's)." },
      source: { type: "string", enum: ["game", "pack"], description: "Where the bases come from (default game)." },
      search: { type: "string", description: "Kind, name or resource contains this (any case)." } } },
    async handler(s, a: { bases?: string[]; theme?: string; source?: "game" | "pack"; search?: string }) {
      const theme = a.theme ?? s.project?.store.scene.databank.theme;
      let keys = a.bases;
      if (!keys) {
        if (!theme) throw new ToolError("pass bases or theme, or open a project");
        const list = await (await s.levels()).list();
        keys = list.bases.filter((b) => b.theme === theme && b.source === (a.source ?? "game")).map((b) => b.key);
        if (!keys.length) throw new ToolError(`no ${a.source ?? "game"} base has theme ${theme} (list_bases lists them)`);
      }
      const q = a.search?.toLowerCase();
      const kinds = new Map<string, Map<string, { resources: Map<string, number>; example: Detail }>>();
      const failed: { base: string; error: string }[] = [];
      for (const key of keys.slice(0, 40)) {
        let scene: Scene;
        try {
          scene = await baseScene(s, key, a.source ?? "game");
        } catch (e) {
          failed.push({ base: key, error: e instanceof Error ? e.message : String(e) });
          continue;
        }
        for (const d of scene.details) {
          if (d.role !== "scenery") continue;
          const kind = kindOf(d.name);
          if (q && !`${kind} ${d.name} ${d.resource}`.toLowerCase().includes(q)) continue;
          const per = kinds.get(kind) ?? kinds.set(kind, new Map()).get(kind)!;
          const e = per.get(key) ?? per.set(key, { resources: new Map(), example: d }).get(key)!;
          e.resources.set(d.resource, (e.resources.get(d.resource) ?? 0) + 1);
        }
      }
      const out = [...kinds].map(([kind, per]) => {
        const inBases = [...per].map(([base, e]) => ({ base, resources: [...e.resources.keys()].sort(), count: [...e.resources.values()].reduce((n, v) => n + v, 0), exampleId: e.example.id, exampleName: e.example.name }));
        return { kind, count: inBases.reduce((n, b) => n + b.count, 0), in: inBases, text: inBases.map((b) => `${kind} in ${b.base}: ${b.resources.join(", ")}`).join("; ") };
      }).sort((x, y) => y.count - x.count);
      return { theme: theme ?? null, bases: keys, kinds: out.length, list: out.slice(0, 200), ...(failed.length ? { failed } : {}),
        note: "copy_base_scenery takes {base, copies: [{id: exampleId}]} or {resource}; copies draw but have no collision" };
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
          rot: vec3("Rotation in degrees about x, y, z (default the source's)."),
          scale: { description: "Scale (default the source's): a number (uniform, 0.05-20), [x, y, z], or the same as a string (\"1.5\")." },
        } } },
        allowOtherTheme: { type: "boolean", description: "Copy from a base of another theme than the level's (the game may not load its meshes)." },
        units: unitsSchema,
      },
    },
    async handler(s, a: { base?: string; source?: "game" | "pack"; copies: { id?: number; resource?: string; pos?: number[]; xz?: number[]; drop?: boolean; rot?: number[]; scale?: unknown }[];
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
        const scale: Vec3 = c.scale !== undefined ? scaleOf(c.scale, `copies[${i}].scale`) : [src.scale[0], src.scale[1], src.scale[2]];
        cmds.push(new AddDetail(frame, { name: src.name, resource: src.resource, pos: store.frames.toLocal(frame, at), rot, scale }));
        placed.push({ from: src.id, name: src.name, resource: src.resource, at: pointOut(at, u) });
      });
      commit(store, cmds.length === 1 ? cmds[0] : new Group(`Copy ${cmds.length} scenery pieces`, cmds));
      return { placed, frame, note: "decoration only: copies have no collision", undo: "undo removes them all" };
    },
  },
];
