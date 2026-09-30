// What the palette can place, and where a new detail goes: its frame and its position in that frame.
import {
  AddDetail, AddObjects, KNOT_RESOURCE, LIMITS, validContentsName, type ObjectSpec, type ObjectType, type Role, type Scene, type Vec3,
} from "../../../sdk/erg";
import { snapVec, type Frames } from "./geometry";
import { KNOTS } from "./checks";

export interface PaletteEntry {
  id: string; label: string; name: string; resource: string; role: Role; preview?: string; object?: ObjectType;
}

// Duplicating or placing scenery ships only once a copied scenery detail is shown to render and collide in the game;
// until then the palette lists markers and objects only, and duplicate refuses scenery.
export const SCENERY_COPIES = false;

export const BUILTIN: PaletteEntry[] = [
  { id: "knot", label: "Spawn knot", name: "WORM", resource: "CheesyGrinWorm", role: "spawn" },
  { id: "oildrum", label: "Oil drum", name: "oildrum", resource: "OilDrum", role: "object" },
  { id: "mine", label: "Mine", name: "mine", resource: "Mine", role: "object" },
  { id: "crate", label: "Crate", name: "CRATE_", resource: KNOT_RESOURCE, role: "object", object: "crate" },
  { id: "telepads", label: "Telepad pair", name: "TP_", resource: KNOT_RESOURCE, role: "object", object: "telepad" },
  { id: "trigger", label: "Trigger", name: "TRIG_", resource: KNOT_RESOURCE, role: "object", object: "trigger" },
  { id: "minefactory", label: "Mine factory", name: "minefactory", resource: KNOT_RESOURCE, role: "object", object: "minefactory" },
];

/** The second pad of a pair sits this far from the first, along x in the frame (.xan units). */
export const PAD_SPACING = 2;

export function canCopy(role: Role): boolean {
  return SCENERY_COPIES || role !== "scenery";
}

/** Server palette entries (level.palette) merged over the built-ins; scenery only when copies are allowed. */
export function paletteFrom(server: unknown): PaletteEntry[] {
  const out = BUILTIN.map((e) => ({ ...e }));
  if (!Array.isArray(server)) return out;
  for (const x of server) {
    if (!x || typeof x !== "object") continue;
    const { name, resource, role, preview } = x as Record<string, unknown>;
    if (typeof name !== "string" || typeof resource !== "string" || typeof role !== "string") continue;
    const known = out.find((e) => (!e.object && e.name.toLowerCase() === name.toLowerCase()) || (e.id === "knot" && role === "spawn"));
    if (known) {
      if (typeof preview === "string" && preview) known.preview = preview;
      continue;
    }
    if (!canCopy(role as Role) || role === "spawn" || role === "object") continue;
    out.push({ id: `p:${name}:${resource}`, label: resource || name, name, resource, role: role as Role,
      preview: typeof preview === "string" && preview ? preview : undefined });
  }
  return out;
}

export function nextKnot(scene: Scene): string | null {
  const have = new Set(scene.details.map((d) => d.name));
  return KNOTS.find((k) => !have.has(k)) ?? null;
}

/** Crate contents the install offers (level.objects); empty until the server answers. */
export interface ObjectCatalog { weapons: string[]; utilities: string[]; error?: string; }

export function catalogOf(r: unknown): ObjectCatalog {
  const o = (r && typeof r === "object" ? r : {}) as Record<string, unknown>;
  const names = (v: unknown) => (Array.isArray(v) ? v.filter((x): x is string => validContentsName(x)) : []);
  return { weapons: names(o.weapons), utilities: names(o.utilities), error: typeof o.error === "string" ? o.error : undefined };
}

/** The level object whose knot this added detail is. */
export function objectOf(scene: Scene, d: { src: number | null; name: string }): ObjectSpec | undefined {
  return d.src === null ? scene.objects?.find((o) => o.knot === d.name) : undefined;
}

/** The lowest free knot of a type (a telepad: in its group), or null when none is left. */
export function nextObjectKnot(scene: Scene, type: ObjectType, group = 0, taken: string[] = []): string | null {
  const used = new Set([...scene.details.map((d) => d.name), ...(scene.objects ?? []).map((o) => o.knot), ...taken]);
  if (type === "minefactory") return used.has("minefactory") ? null : "minefactory";
  const prefix = type === "crate" ? "CRATE_" : type === "trigger" ? "TRIG_" : `TP_${group}_`;
  for (let n = 0; n <= 255; n++) if (!used.has(prefix + n)) return prefix + n;
  return null;
}

/** The lowest telepad group no pad uses yet, or null when all eight are taken. */
export function nextTelepadGroup(scene: Scene): number | null {
  const used = new Set((scene.objects ?? []).flatMap((o) => (o.type === "telepad" ? [o.group] : [])));
  for (let g = 1; g <= LIMITS.telepadGroups; g++) if (!used.has(g)) return g;
  return null;
}

/** A new object's settings: a health crate (it needs no install name), a trigger numbered as its knot. */
export function defaultObject(type: ObjectType, knot: string, group = 0): ObjectSpec {
  switch (type) {
    case "crate": return { knot, type, crate: { kind: "health", amount: 25, hitpoints: 25, parachute: false } };
    case "telepad": return { knot, type, group };
    case "trigger":
      return { knot, type, trigger: { index: Number(knot.slice(5)), radius: 60, teamCollect: 0, teamDestroy: 4, hitpoints: 1, wormCollect: false } };
    case "minefactory": return { knot, type };
  }
}

function placeObject(scene: Scene, frames: Frames, e: PaletteEntry, world: Vec3, step: number | null): AddObjects | string {
  const type = e.object!;
  if ((scene.objects?.length ?? 0) + (type === "telepad" ? 2 : 1) > LIMITS.objects) return `a level holds at most ${LIMITS.objects} objects`;
  const frame = targetFrame(scene, "object");
  const pos = snapVec(frames.toLocal(frame, world), step);
  if (type === "telepad") {
    const g = nextTelepadGroup(scene);
    if (g === null) return `all ${LIMITS.telepadGroups} telepad groups are used`;
    const a = nextObjectKnot(scene, type, g), b = a && nextObjectKnot(scene, type, g, [a]);
    if (!a || !b) return `telepad group ${g} has no free knot`;
    return new AddObjects(frame, [defaultObject(type, a, g), defaultObject(type, b, g)],
      [pos, [pos[0] + PAD_SPACING, pos[1], pos[2]]], "Add telepad pair");
  }
  const knot = nextObjectKnot(scene, type);
  if (!knot) return type === "minefactory" ? "the level already has a mine factory" : `no free ${e.label.toLowerCase()} knot is left`;
  return new AddObjects(frame, [defaultObject(type, knot)], [pos], `Add ${e.label.toLowerCase()}`);
}

const FOLDER_HINTS: Partial<Record<Role, RegExp>> = { spawn: /worm|spawn/i, object: /barrel|mine|drum|object|crate/i };

/** The frame a new detail of this role goes into: a folder already holding that role, a folder named for it, any
 * folder, then the root. */
export function targetFrame(scene: Scene, role: Role): number {
  const folders = scene.frames.filter((f) => f.folder);
  const holding = new Map<number, number>();
  for (const d of scene.details) if (d.role === role) holding.set(d.frame, (holding.get(d.frame) ?? 0) + 1);
  const best = folders.filter((f) => holding.has(f.id)).sort((a, b) => holding.get(b.id)! - holding.get(a.id)!)[0];
  if (best) return best.id;
  const hint = FOLDER_HINTS[role];
  const named = hint && folders.find((f) => hint.test(f.name));
  if (named) return named.id;
  if (folders[0]) return folders[0].id;
  return (scene.frames.find((f) => f.parent === null) ?? scene.frames[0]).id;
}

/** The add command for a palette entry at a level position, or a reason it cannot be placed. */
export function place(scene: Scene, frames: Frames, e: PaletteEntry, world: Vec3, step: number | null): AddDetail | AddObjects | string {
  if (e.object) return placeObject(scene, frames, e, world, step);
  let name = e.name;
  if (e.id === "knot") {
    const k = nextKnot(scene);
    if (!k) return "all eight spawn knots (WORM0-WORM7) already exist";
    name = k;
  }
  if (!canCopy(e.role)) return "scenery cannot be placed in this version";
  const frame = targetFrame(scene, e.role);
  const pos = snapVec(frames.toLocal(frame, world), step);
  return new AddDetail(frame, { name, resource: e.resource, pos });
}
