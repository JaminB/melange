// Tool group "place": spawn knots, level objects (crates, telepads, triggers, the mine factory) and placed mines and
// oil drums, moving, turning, scaling and removing them, the level's settings (water, theme, time of day, spawn and
// surround modes), the level script, and shipping the level (export, build, test, live packs). Placing uses the Erg
// panel's own model (model/placing.ts, ground.ts), so a placed object is the one the palette places.
import {
  AddDetail, AddObjects, BUILTIN, Group, KNOT_RESOURCE, KNOTS, LIMITS, PAD_SPACING, RemoveDetail, SetDetail, SetLevel, THEMES, TIMES_OF_DAY,
  catalogOf, checkScene, defaultObject, nextObjectKnot, nextTelepadGroup, objectOf, place, targetFrame,
  type Command, type CrateSpec, type Detail, type DetailFields, type EditorStore, type LevelSettings, type ObjectSpec, type ObjectType,
  type TimeOfDay, type Vec3,
} from "../core";
import {
  DEG, commit, detailInfo, findDetail, pointOut, pointToMap, resolvePoint, unitsSchema, vec3, type PointArgs, type Units,
} from "../lib/place-geom";
import { resolveFile, themeFile } from "../lib/materials";
import { runTest } from "../lib/place-wait";
import { tidy } from "../lib/voxel";
import { ToolError, type Schema, type Tool } from "../registry";
import type { Session } from "../session";
import { validateExportForm } from "../../../../web/src/panels/erg/test/model";

const pointProps: Record<string, Schema> = {
  pos: vec3("Position [x, y, z] in the level (units, default world). Give this or xz."),
  xz: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2, description: "[x, z]: start above the whole level and drop straight down onto the terrain (the panel's Drop to ground). Give this or pos." },
  drop: { type: "boolean", description: "With pos: also drop the point straight down onto the terrain below it (an error over empty space)." },
};
const idsProps: Record<string, Schema> = {
  id: { type: "integer", description: "A detail id (list_objects)." },
  ids: { type: "array", items: { type: "integer" }, minItems: 1, maxItems: 256, description: "Several detail ids." },
};

function idsOf(a: { id?: number; ids?: number[] }): number[] {
  const ids = [...(a.id === undefined ? [] : [a.id]), ...(a.ids ?? [])];
  if (!ids.length) throw new ToolError("give id or ids");
  return [...new Set(ids)];
}

const infos = (s: Session, ids: number[], units: Units) => ids.map((id) => detailInfo(s.store, s.store.detail(id)!, units));

/** The editor's checks, errors first and at most 30 lines (a base level has dozens of details outside its voxel frames). */
function issuesOf(store: EditorStore, max = 30): string[] {
  const all = checkScene(store.scene, store.frames).sort((a, b) => Number(b.level === "error") - Number(a.level === "error")).map((i) => `${i.level}: ${i.text}`);
  return all.length > max ? [...all.slice(0, max), `... and ${all.length - max} more`] : all;
}

// ------------------------------------------------------------------ spawns

interface SpawnArg extends PointArgs { worm: number; }

function spawnState(store: EditorStore, units: Units) {
  const scene = store.scene;
  const spawns = scene.details.filter((d) => d.role === "spawn").map((d) => detailInfo(store, d, units));
  const have = new Set(scene.details.map((d) => d.name));
  return { spawnMode: scene.spawns.mode, spawns, missingKnots: KNOTS.filter((k) => !have.has(k)) };
}

function placeSpawnKnots(s: Session, a: { mode?: "random" | "knots"; spawns?: SpawnArg[]; units?: Units }) {
  const store = s.store, scene = store.scene, units = a.units ?? "world";
  const mode = a.mode ?? "knots";
  const cmds: Command[] = [];
  if (scene.spawns.mode !== mode) cmds.push(new SetLevel({ spawns: mode }, `Spawn mode ${mode}`));
  const seen = new Set<number>();
  const done: { worm: number; action: "added" | "moved"; dropped: boolean; pos: Vec3 }[] = [];
  for (const sp of a.spawns ?? []) {
    if (seen.has(sp.worm)) throw new ToolError(`WORM${sp.worm} is given twice`);
    seen.add(sp.worm);
    const { map, dropped } = resolvePoint(store, sp, units, `WORM${sp.worm}`);
    const name = KNOTS[sp.worm];
    const have = scene.details.find((d) => d.name === name);
    if (have) {
      cmds.push(new SetDetail(have.id, { pos: tidy(store.frames.toLocal(have.frame, map), 6) }, `Move ${name}`));
      done.push({ worm: sp.worm, action: "moved", dropped, pos: pointOut(map, units) });
    } else {
      const frame = targetFrame(scene, "spawn");
      cmds.push(new AddDetail(frame, { name, resource: KNOT_RESOURCE, pos: tidy(store.frames.toLocal(frame, map), 6) }));
      done.push({ worm: sp.worm, action: "added", dropped, pos: pointOut(map, units) });
    }
  }
  if (!cmds.length) return { mode, changed: false, note: "nothing to change", ...spawnState(store, units) };
  commit(store, cmds.length === 1 ? cmds[0] : new Group(a.spawns?.length ? "Place spawn knots" : `Spawn mode ${mode}`, cmds));
  return { mode, changed: true, placed: done, ...spawnState(store, units), issues: issuesOf(store).filter((t) => /WORM|spawn/i.test(t)) };
}

// ------------------------------------------------------------------ objects

const OBJECT_TYPES = ["crate", "health", "telepad", "trigger", "mine", "oildrum", "minefactory"] as const;
type PlaceType = (typeof OBJECT_TYPES)[number];

interface PlaceArgs extends PointArgs {
  type: PlaceType; units?: Units; kind?: "weapon" | "health" | "utility"; contents?: string; count?: number; amount?: number;
  hitpoints?: number; parachute?: boolean; pos2?: number[]; index?: number; radius?: number; teamCollect?: number; teamDestroy?: number; wormCollect?: boolean;
}

function crateOf(a: PlaceArgs): CrateSpec {
  const kind = a.type === "health" ? "health" : a.kind ?? "health";
  const hitpoints = a.hitpoints ?? 25, parachute = a.parachute ?? false;
  if (kind === "health") return { kind, amount: a.amount ?? 25, hitpoints, parachute };
  if (!a.contents) throw new ToolError(`a ${kind} crate needs contents (the weapon or utility name)`);
  return { kind, contents: a.contents, ...(a.count !== undefined ? { count: a.count } : {}), hitpoints, parachute };
}

/** The install's own name for crate contents (level.objects lists kWeaponBazooka, kUtilityJetpack; export refuses any
 * other spelling). A short name that matches one entry is replaced by it; anything else is kept with a warning. */
async function contentsName(s: Session, c: CrateSpec): Promise<{ contents?: string; note?: string }> {
  if (c.kind === "health") return {};
  try {
    const cat = catalogOf(await (await s.client()).call("level.objects", {}));
    const list = c.kind === "weapon" ? cat.weapons : cat.utilities;
    if (!list.length || list.includes(c.contents)) return {};
    const short = (n: string) => n.replace(/^k(Weapon|Utility)/, "").toLowerCase();
    const same = list.filter((n) => short(n) === short(c.contents));
    if (same.length === 1) return { contents: same[0], note: `'${c.contents}' is called ${same[0]} in this install` };
    return { note: `'${c.contents}' is not among the install's ${c.kind === "weapon" ? "weapons" : "utilities"} (${list.slice(0, 12).join(", ")}${list.length > 12 ? ", ..." : ""}); export will refuse it` };
  } catch {
    return {};   // the catalogue is optional
  }
}

async function placeObject(s: Session, a: PlaceArgs) {
  const store = s.store, scene = store.scene, units = a.units ?? "world";
  const { map, dropped } = resolvePoint(store, a, units, a.type);
  let cmd: AddDetail | AddObjects, note: string | undefined;
  if (a.type === "mine" || a.type === "oildrum") {
    const r = place(scene, store.frames, BUILTIN.find((e) => e.id === a.type)!, map, null);
    if (typeof r === "string") throw new ToolError(r);
    cmd = r as AddDetail;
  } else {
    const type: ObjectType = a.type === "health" ? "crate" : a.type;
    const specs: ObjectSpec[] = [];
    const at: Vec3[] = [];
    const frame = targetFrame(scene, "object");
    const local = (p: Vec3) => tidy(store.frames.toLocal(frame, p), 6);
    if ((scene.objects?.length ?? 0) + (type === "telepad" ? 2 : 1) > LIMITS.objects) throw new ToolError(`a level holds at most ${LIMITS.objects} objects`);
    if (type === "telepad") {
      const g = nextTelepadGroup(scene);
      if (g === null) throw new ToolError(`all ${LIMITS.telepadGroups} telepad groups are used`);
      const k1 = nextObjectKnot(scene, type, g), k2 = k1 && nextObjectKnot(scene, type, g, [k1]);
      if (!k1 || !k2) throw new ToolError(`telepad group ${g} has no free knot`);
      specs.push(defaultObject(type, k1, g), defaultObject(type, k2, g));
      const second = a.pos2 ? resolvePoint(store, { pos: a.pos2, drop: a.drop }, units, "pos2").map : ([map[0] + PAD_SPACING, map[1], map[2]] as Vec3);
      at.push(local(map), local(second));
    } else {
      const knot = nextObjectKnot(scene, type);
      if (!knot) throw new ToolError(type === "minefactory" ? "the level already has a mine factory" : `no free ${type} knot is left`);
      const spec = defaultObject(type, knot);
      if (spec.type === "crate") {
        spec.crate = crateOf(a);
        const r = await contentsName(s, spec.crate);
        if (r.contents && spec.crate.kind !== "health") spec.crate.contents = r.contents;
        note = r.note;
      } else if (spec.type === "trigger") {
        const t = spec.trigger;
        if (a.index !== undefined) t.index = a.index;
        if (a.radius !== undefined) t.radius = a.radius;
        if (a.teamCollect !== undefined) t.teamCollect = a.teamCollect;
        if (a.teamDestroy !== undefined) t.teamDestroy = a.teamDestroy;
        if (a.hitpoints !== undefined) t.hitpoints = a.hitpoints;
        if (a.wormCollect !== undefined) t.wormCollect = a.wormCollect;
      }
      specs.push(spec);
      at.push(local(map));
    }
    cmd = new AddObjects(frame, specs, at, `Add ${a.type}`);
  }
  commit(store, cmd);
  const ids = cmd instanceof AddObjects ? cmd.ids : [cmd.detailId];
  return {
    placed: infos(s, ids, units), dropped, ...(note ? { warning: note } : {}),
    objects: `${scene.objects?.length ?? 0} of ${LIMITS.objects}`,
    issues: issuesOf(store).filter((t) => ids.some((id) => t.includes(`#${id} `))),
  };
}

function listObjects(s: Session, a: { role?: string; search?: string; limit?: number; units?: Units }) {
  const store = s.store, scene = store.scene, units = a.units ?? "world";
  const q = a.search?.toLowerCase();
  const roles = a.role ? [a.role] : ["spawn", "object"];
  const all = scene.details.filter((d) => roles.includes(d.role) && (!q || `${d.name} ${d.resource}`.toLowerCase().includes(q)));
  const listed = all.slice(0, a.limit ?? 200).map((d) => detailInfo(store, d, units));
  return {
    spawnMode: scene.spawns.mode, water: scene.water.level, units, matching: all.length, listed: listed.length,
    spawns: listed.filter((d) => d.role === "spawn"),
    levelObjects: listed.filter((d) => d.object),
    placed: listed.filter((d) => d.role !== "spawn" && !d.object),
    counts: { objects: `${scene.objects?.length ?? 0} of ${LIMITS.objects}`, details: scene.details.length },
    issues: issuesOf(store),
  };
}

// ------------------------------------------------------------------ move, rotate, scale, remove, drop

/** SetDetail commands as one undo step (a single one stays plain). */
function setFields(s: Session, changes: [number, DetailFields][], label: string) {
  const cmds = changes.map(([id, f]) => new SetDetail(id, f, label));
  commit(s.store, cmds.length === 1 ? cmds[0] : new Group(label, cmds));
}

function moveDetails(s: Session, a: { id?: number; ids?: number[]; pos?: number[]; by?: number[]; drop?: boolean; units?: Units }) {
  const store = s.store, units = a.units ?? "world";
  const ids = idsOf(a);
  if ((a.pos === undefined) === (a.by === undefined)) throw new ToolError("give exactly one of pos (new position, one detail) or by (offset, any number)");
  if (a.pos && ids.length !== 1) throw new ToolError("pos moves one detail: use by for several");
  const changes = ids.map((id): [number, DetailFields] => {
    const d = findDetail(store, id);
    const from = store.frames.detailWorld(d);
    let to: Vec3 = a.pos ? pointToMap([a.pos[0], a.pos[1], a.pos[2]], units) : (() => {
      const b = pointToMap([a.by![0], a.by![1], a.by![2]], units);
      return [from[0] + b[0], from[1] + b[1], from[2] + b[2]] as Vec3;
    })();
    if (a.drop) to = resolvePoint(store, { pos: to, drop: true }, "map", `detail ${id}`).map;
    return [id, { pos: tidy(store.frames.toLocal(d.frame, to), 6) }];
  });
  setFields(s, changes, ids.length === 1 ? `Move ${store.detail(ids[0])!.name}` : "Move details");
  return { moved: infos(s, ids, units) };
}

function dropDetails(s: Session, a: { id?: number; ids?: number[]; units?: Units }) {
  const store = s.store, ids = idsOf(a);
  const changes = ids.map((id): [number, DetailFields] => {
    const d = findDetail(store, id);
    const hit = resolvePoint(store, { pos: [...store.frames.detailWorld(d)], drop: true }, "map", `detail ${id}`).map;
    return [id, { pos: tidy(store.frames.toLocal(d.frame, hit), 6) }];
  });
  setFields(s, changes, "Drop to ground");
  return { dropped: infos(s, ids, a.units ?? "world") };
}

function transformable(store: EditorStore, d: Detail, what: string) {
  if (store.translationOnly.has(d.frame)) throw new ToolError(`detail ${d.id} (${d.name}) is in a frame that swings in the game: only move works, not ${what}`);
}

function turn(s: Session, a: { id?: number; ids?: number[]; rot?: number[]; by?: number[]; units?: Units }) {
  const store = s.store, ids = idsOf(a);
  if ((a.rot === undefined) === (a.by === undefined)) throw new ToolError("give exactly one of rot (absolute, degrees) or by (added, degrees)");
  const changes = ids.map((id): [number, DetailFields] => {
    const d = findDetail(store, id);
    transformable(store, d, "rotate");
    const deg = a.rot ?? [d.rot[0] * DEG + a.by![0], d.rot[1] * DEG + a.by![1], d.rot[2] * DEG + a.by![2]];
    return [id, { rot: tidy([deg[0] / DEG, deg[1] / DEG, deg[2] / DEG], 6) }];
  });
  setFields(s, changes, "Rotate");
  return { rotated: infos(s, ids, a.units ?? "world") };
}

function resize(s: Session, a: { id?: number; ids?: number[]; scale?: number[]; by?: number; uniform?: number; units?: Units }) {
  const store = s.store, ids = idsOf(a);
  if ([a.scale, a.by, a.uniform].filter((v) => v !== undefined).length !== 1) throw new ToolError("give exactly one of scale ([x, y, z]), uniform (one number) or by (a factor)");
  const changes = ids.map((id): [number, DetailFields] => {
    const d = findDetail(store, id);
    transformable(store, d, "scale");
    const v: Vec3 = a.by !== undefined ? [d.scale[0] * a.by, d.scale[1] * a.by, d.scale[2] * a.by]
      : a.uniform !== undefined ? [a.uniform, a.uniform, a.uniform] : [a.scale![0], a.scale![1], a.scale![2]];
    if (v.some((x) => !(x > 0))) throw new ToolError("scale must be positive");
    return [id, { scale: tidy(v, 6) }];
  });
  setFields(s, changes, "Scale");
  return { scaled: infos(s, ids, a.units ?? "world") };
}

function removeDetails(s: Session, a: { id?: number; ids?: number[]; units?: Units }) {
  const store = s.store, ids = idsOf(a);
  const gone = ids.map((id) => {
    const d = findDetail(store, id);
    return { ...detailInfo(store, d, a.units ?? "world"), ...(objectOf(store.scene, d) ? { alsoRemoved: "its level object" } : {}) };
  });
  commit(store, ids.length === 1 ? new RemoveDetail(ids[0]) : new Group("Delete details", ids.map((id) => new RemoveDetail(id))));
  store.select([]);
  return { removed: gone, issues: issuesOf(store).filter((t) => !/outside every terrain/.test(t)) };
}

// ------------------------------------------------------------------ level settings

function setLevel(s: Session, settings: LevelSettings, label: string, report: () => object) {
  commit(s.store, new SetLevel(settings, label));
  return report();
}

// ------------------------------------------------------------------ shipping

/** Saves first when there are unsaved edits (export and Test work on the saved project, as in the panel). */
async function savedFirst(s: Session, why: string) {
  const p = s.requireProject();
  if (!p.store.dirty) return undefined;
  const r = await s.save();
  if (r.problems.length) throw new ToolError(`the project has problems and was not ${why}: ${r.problems.join("; ")}`);
  return r;
}

const modId: Schema = { type: "string", description: "Mod id: a-z, 0-9, '-' and '_' (it becomes the pack prefix; 'ergtest' is reserved)." };

export const tools: Tool[] = [
  {
    name: "place_spawn_knots",
    description: "Set the spawn mode and place or move the spawn knots WORM0-WORM7. mode 'knots' makes worm i start on knot i (a match needs all 8); 'random' leaves the game's random spawns (existing knots stay but are unused). Each spawn is a position [x,y,z] or xz (dropped onto the terrain). An existing WORMn is moved, a missing one added. One undo step. Answers the spawns and the knots still missing.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        mode: { type: "string", enum: ["random", "knots"], description: "Spawn mode (default knots)." },
        spawns: {
          type: "array", maxItems: 8, description: "The knots to place or move.",
          items: { type: "object", additionalProperties: false, required: ["worm"],
            properties: { worm: { type: "integer", minimum: 0, maximum: 7, description: "0-7: WORM0-WORM7." }, ...pointProps } },
        },
        units: unitsSchema,
      },
    },
    handler: (s, a) => placeSpawnKnots(s, a),
  },
  {
    name: "place_object",
    description: "Place an object as the Erg palette does: crate (kind weapon|health|utility, contents, count, amount, hitpoints, parachute), health (a health crate), telepad (a pair in a new group; pos2 for the second pad, default 2 map units along x), trigger (index, radius, teamCollect, teamDestroy, hitpoints, wormCollect), mine, oildrum, or minefactory (one per level). Position by pos [x,y,z] or xz (dropped onto the terrain). Crates, telepads, triggers and the mine factory make the project format v2 (at most 256 objects). One undo step.",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["type"],
      properties: {
        type: { type: "string", enum: OBJECT_TYPES },
        ...pointProps,
        pos2: vec3("telepad: position of the second pad (units like pos)."),
        kind: { type: "string", enum: ["weapon", "health", "utility"], description: "crate: what it holds (default health)." },
        contents: { type: "string", description: "weapon/utility crate: the weapon or utility name, e.g. Bazooka." },
        count: { type: "integer", minimum: 1, description: "weapon/utility crate: how many." },
        amount: { type: "number", minimum: 1, description: "health crate: hit points restored (default 25)." },
        hitpoints: { type: "number", description: "crate or trigger: its own hit points (crate default 25)." },
        parachute: { type: "boolean", description: "crate: falls on a parachute (default false)." },
        index: { type: "integer", minimum: 0, maximum: 255, description: "trigger: its index (default the knot's number)." },
        radius: { type: "number", description: "trigger: radius (default 60)." },
        teamCollect: { type: "integer", minimum: 0, description: "trigger: teams that collect it (default 0)." },
        teamDestroy: { type: "integer", minimum: 0, description: "trigger: teams that destroy it (default 4)." },
        wormCollect: { type: "boolean", description: "trigger: a worm collects it (default false)." },
        units: unitsSchema,
      },
    },
    handler: (s, a: PlaceArgs) => placeObject(s, a),
  },
  {
    name: "move",
    description: "Move details: one to a new position (pos), or any number by an offset (by). drop also drops the result onto the terrain below it. Positions are in the level, not the detail's frame. One undo step.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: { ...idsProps, pos: vec3("New position [x,y,z] (one detail)."), by: vec3("Offset [x,y,z] added to each detail."),
        drop: { type: "boolean", description: "Drop the new position onto the terrain." }, units: unitsSchema },
    },
    handler: (s, a) => moveDetails(s, a),
  },
  {
    name: "rotate",
    description: "Set (rot) or add to (by) the rotation of details, in degrees about x, y, z (applied z, then y, then x). Refused for details in frames that swing in the game.",
    inputSchema: { type: "object", additionalProperties: false, properties: { ...idsProps, rot: vec3("Absolute rotation, degrees."), by: vec3("Degrees to add.") , units: unitsSchema } },
    handler: (s, a) => turn(s, a),
  },
  {
    name: "scale",
    description: "Set the scale of details: scale [x,y,z], uniform (one number) or by (a factor multiplying the current scale). Refused for details in frames that swing in the game.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: { ...idsProps, scale: vec3("Absolute [x,y,z] scale."), uniform: { type: "number", description: "Absolute scale on all axes." },
        by: { type: "number", description: "Factor to multiply the scale by." }, units: unitsSchema },
    },
    handler: (s, a) => resize(s, a),
  },
  {
    name: "remove",
    description: "Delete details (an added level object's knot takes its object with it). One undo step.",
    inputSchema: { type: "object", additionalProperties: false, properties: { ...idsProps, units: unitsSchema } },
    handler: (s, a) => removeDetails(s, a),
  },
  {
    name: "drop_to_ground",
    description: "Drop details straight down onto the terrain below them, as the panel's Drop to ground. A detail over empty space is refused (move it above the ground first).",
    inputSchema: { type: "object", additionalProperties: false, properties: { ...idsProps, units: unitsSchema } },
    handler: (s, a) => dropDetails(s, a),
  },
  {
    name: "list_objects",
    description: "The open level's spawn knots, level objects (crates with contents, telepads, triggers, the mine factory) and placed mines and oil drums, with positions, plus the editor's checks (missing knots, objects under water, telepads without a partner). role lists another role instead (e.g. scenery, light).",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: { role: { type: "string", description: "Only this role (default spawn and object)." }, search: { type: "string", description: "Name or resource contains this (any case)." },
        limit: { type: "integer", minimum: 1, maximum: 5000, description: "Details to list (default 200)." }, units: unitsSchema },
    },
    handler: (s, a) => listObjects(s, a),
  },
  {
    name: "set_water",
    description: "Set the water level in world units (sea level 0; -1000 to 1000), or useDefault for the map's own water.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: { level: { type: "number", minimum: -1000, maximum: 1000, description: "Water height, in world units (map units with units: map)." },
        useDefault: { type: "boolean" }, units: unitsSchema },
    },
    handler(s, a: { level?: number; useDefault?: boolean; units?: Units }) {
      if ((a.level === undefined) === !a.useDefault) throw new ToolError("give exactly one of level or useDefault: true");
      const world = a.level === undefined ? null : a.units === "map" ? a.level * 20 : a.level;
      if (world !== null && Math.abs(world) > 1000) throw new ToolError("the water level is within -1000 and 1000 world units");
      return setLevel(s, { water: world }, "Change water", () => ({ water: { world, map: world === null ? null : world / 20 } }));
    },
  },
  {
    name: "set_theme",
    description: "Set the level's theme (the scenery set, sky and the folder textures come from; the terrain is untouched). The voxels' materials come from the level's material file, which a theme change leaves as it is unless materialFile is given: \"theme\" for the new theme's own file, or a file name (list_materials, preview_materials). One undo step.",
    inputSchema: { type: "object", additionalProperties: false, required: ["theme"], properties: { theme: { type: "string", enum: THEMES },
      materialFile: { type: "string", description: "Also set the material file: \"theme\" for the theme's own (ThemeCamelot\\ThemeCamelot.txt for CAMELOT), \"keep\" (default) to leave it, or a file name such as Maps\\dinermight.txt." } } },
    async handler(s, a: { theme: string; materialFile?: string }) {
      const keep = a.materialFile === undefined || a.materialFile === "keep";
      let file: string | undefined, note: string | undefined;
      if (!keep) {
        const r = await resolveFile(s, a.materialFile === "theme" ? themeFile(a.theme) : a.materialFile!);
        const files = await (await s.levels()).themes().then((t) => t.materialFiles, () => null);
        if (files && !files.includes(r.file)) throw new ToolError(`'${a.materialFile}' is not a material file of this install (list_materials lists them)`);
        file = r.file;
        note = r.note;
      }
      return setLevel(s, { databank: { theme: a.theme, ...(file ? { materialFile: file } : {}) } }, "Change theme", () => ({
        theme: s.store.scene.databank.theme, materialFile: s.store.scene.databank.materialFile, ...(note ? { note } : {}),
        ...(keep ? { materialNote: "the material file is unchanged: voxel materials keep their indices in it (materialFile: \"theme\" switches to the theme's own)" } : {}),
      }));
    },
  },

  {
    name: "set_time_of_day",
    description: "Set the level's time of day. Quick Game and network games load DAY whatever is set; test can pick its own.",
    inputSchema: { type: "object", additionalProperties: false, required: ["timeOfDay"], properties: { timeOfDay: { type: "string", enum: TIMES_OF_DAY } } },
    handler: (s, a: { timeOfDay: string }) => setLevel(s, { databank: { timeOfDay: a.timeOfDay } }, "Change time of day", () => ({ timeOfDay: s.store.scene.databank.timeOfDay })),
  },
  {
    name: "set_surround_mode",
    description: "How the area around the map is made: copy (the base's), none, flat, or paint (an editable surround; needs the base's surround loaded).",
    inputSchema: { type: "object", additionalProperties: false, required: ["mode"], properties: { mode: { type: "string", enum: ["copy", "none", "flat", "paint"] } } },
    handler(s, a: { mode: "copy" | "none" | "flat" | "paint" }) {
      if (a.mode === "paint" && !s.store.canPaintSurround) throw new ToolError("the base level's surround did not load: reopen the project to paint it");
      return setLevel(s, { hmp: a.mode }, `Surround ${a.mode}`, () => ({ surround: s.store.scene.hmp.mode }));
    },
  },
  {
    name: "get_script",
    description: "The level's Lua script (script.lua of the project); empty when it has none.",
    inputSchema: { type: "object", additionalProperties: false, properties: {} },
    async handler(s) {
      const text = await s.requireProject().erg.script();
      return { present: text.length > 0, lines: text ? text.split("\n").length : 0, text };
    },
  },
  {
    name: "put_script",
    description: "Replace the level's Lua script (written to the server at once; not an undo step and not part of save). Answers problems with line numbers. A script the server rejects is not saved; a syntax error is reported but saved. syntaxChecked is false when the server cannot compile Lua: the game then checks it on test.",
    inputSchema: { type: "object", additionalProperties: false, required: ["text"], properties: { text: { type: "string", description: "The whole script; empty removes it." } } },
    async handler(s, a: { text: string }) {
      const r = await s.requireProject().erg.saveScript(a.text);
      return { saved: r.saved, problems: r.problems, syntaxChecked: r.syntaxChecked ?? false,
        ...(r.syntaxChecked ? {} : { note: "the server cannot compile Lua here; the game checks the script on test" }) };
    },
  },
  {
    name: "export",
    description: "Export the open project as a mod under Mods\\: mode 'source' (your edits plus a build script, shareable; the default) or 'install' (built map files for this machine only: do not redistribute). Saves the project first when it has unsaved edits. Exporting a mod id again adds or updates the level in it.",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["modId", "name"],
      properties: { modId, name: { type: "string", description: "Display name (1-64 printable characters)." }, version: { type: "string", description: "x.y.z (default 1.0.0)." },
        mode: { type: "string", enum: ["source", "install"] }, survivor: { type: "boolean", description: "Also list the level in Survivor's Prebuilt maps (default false)." } },
    },
    async handler(s, a: { modId: string; name: string; version?: string; mode?: "source" | "install"; survivor?: boolean }) {
      const p = s.requireProject();
      const form = { modId: a.modId, name: a.name, version: a.version ?? "1.0.0", mode: a.mode ?? "source", survivor: !!a.survivor };
      const bad = validateExportForm(form);
      if (bad.length) throw new ToolError(bad.join("; "));
      const first = await savedFirst(s, "exported");
      const r = await (await s.client()).call<{ dir: string; files: string[]; restartRequired: boolean }>(
        "level.export", { project: p.id, ...form }, 120000);
      return { ...r, mode: form.mode, ...(first ? { savedFirst: true, warnings: first.warnings } : {}),
        ...(form.mode === "install" ? { note: "install exports contain modified game files: keep them on this machine" } : {}) };
    },
  },
  {
    name: "build",
    description: "Build the levels of a source-mode mod (made by export) into playable map files (level.build). The game must be closed.",
    inputSchema: { type: "object", additionalProperties: false, required: ["modId"], properties: { modId } },
    handler: async (s, a: { modId: string }) => (await s.client()).call("level.build", { modId: a.modId }, 120000),
  },
  {
    name: "test",
    description: "Play the project in the running game without exporting (level.test): saves unsaved edits, then follows the 'erg' channel (registering, registered, armed, starting, playing, ended, failed) and waits for 'playing' (or 'armed', where the game waits for Quick Game). Answers the states with timings. Needs the game running at its main menu; Melange.exe --serve has no Test.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: { tod: { type: "string", enum: TIMES_OF_DAY, description: "Time of day for this Test (default the project's)." },
        waitFor: { type: "string", enum: ["playing", "armed"], description: "State to wait for (default playing)." },
        timeoutSec: { type: "integer", minimum: 1, maximum: 600, description: "Stop waiting after this long (default 60)." },
        cleanMods: { type: "boolean", description: "Refuse when enabled content mods have sim code (they run in the Test beside the level script), for clean benchmark runs." } },
    },
    async handler(s, a: { tod?: TimeOfDay; waitFor?: "playing" | "armed"; timeoutSec?: number; cleanMods?: boolean }) {

      const p = s.requireProject();
      const conn = await s.ensure();
      if (!conn.methods.includes("level.test")) throw new ToolError("no level.test here: Test needs the game running at its main menu (Melange.exe --serve has none)");
      // Content mods with sim code run in every match, a Test too: name them, so a benchmark run knows it is not clean.
      let simMods: string[] = [];
      if (conn.methods.includes("mods.list")) {
        try {
          const mods = await (await s.client()).call<{ id: string; on: boolean; kind: string; hasSim?: boolean; state?: string }[]>("mods.list", {}, 15000);
          simMods = mods.filter((m) => m.on && m.kind === "content" && m.hasSim && m.state === "enabled").map((m) => m.id);
        } catch {
          // an older game: no list
        }
      }
      if (a.cleanMods && simMods.length)
        throw new ToolError(`content mods with sim code are enabled and would run in the Test: ${simMods.join(", ")}. Disable them in the game (they apply at the next launch) for a clean run, or leave out cleanMods`);
      const first = await savedFirst(s, "tested");
      const sec = a.timeoutSec ?? 60;
      const run = await runTest(p.erg, { tod: a.tod, goal: a.waitFor, timeoutMs: sec * 1000 });
      return { ...run, ...(first ? { savedFirst: true } : {}), ...(simMods.length ? { simMods, simModsNote: "these mods' sim code runs in this match beside the level script" } : {}),
        match: "Quick Game (QuickStartHvC: a human team against a computer team) with the game's own Quick Game scheme; the scheme, turn time and team sizes are the game's, not chosen here",
        next: "game_state, game_wait {turn}, game_camera while it plays",
        ...(run.timedOut ? { note: `no '${a.waitFor ?? "playing"}' state within ${sec} s: the Test may still be running; start the match from the game's menu if it is armed` } : {}) };
    },
  },
  {
    name: "levels_live",
    description: "Turn a map pack's levels live (on) or off in the running game (levels.live), so they show without a restart. Needs the game running offline at its main menu.",
    inputSchema: { type: "object", additionalProperties: false, required: ["modId", "on"], properties: { modId, on: { type: "boolean" } } },
    async handler(s, a: { modId: string; on: boolean }) {
      const conn = await s.ensure();
      if (!conn.methods.includes("levels.live")) throw new ToolError("no levels.live here: it needs the game running (Melange.exe --serve has none)");
      return (await s.client()).call("levels.live", { modId: a.modId, on: a.on });
    },
  },
];
