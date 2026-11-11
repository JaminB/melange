// Helpers of the place tool group: points in map or world units, dropping to the ground, how a detail is reported, and
// applying an edit only when the patch stays valid (so a refused edit leaves nothing on the undo stack).
import {
  dropPoint, objectOf, toPatch, validatePatch, validatePatchObjects, type Command, type Detail, type EditorStore, type Scene, type Vec3,
} from "../core";
import { ToolError, type Schema } from "../registry";
import { grids, tidy, toMap, toWorld } from "./voxel";

export type Units = "world" | "map";

export const unitsSchema: Schema = {
  type: "string", enum: ["world", "map"],
  description: "Unit of positions in the arguments and the answer: world (default, what the Erg panel shows) or map (the level file's: world / 20).",
};
export const vec3 = (description: string): Schema => ({ type: "array", items: { type: "number" }, minItems: 3, maxItems: 3, description });

export const DEG = 180 / Math.PI;

/** A point in the arguments, in map units. */
export const pointToMap = (p: Vec3, units: Units = "world"): Vec3 => (units === "map" ? [p[0], p[1], p[2]] : toMap(p));
/** A map-unit point or offset in the units the caller works in. */
export const pointOut = (p: Vec3, units: Units = "world", places = 4): Vec3 => tidy(units === "map" ? p : toWorld(p), places);

/** The top of the terrain in map units: where an (x, z) drop starts from. */
export function topOf(store: EditorStore): number {
  let top = 0;
  for (const g of grids(store)) top = Math.max(top, g.bounds.max[1]);
  return top + 1;
}

export interface PointArgs { pos?: number[]; xz?: number[]; drop?: boolean; }

/** Where a tool argument puts a detail, in map units: `pos` as given (dropped to the ground with `drop`), or `xz`, which
 * starts above the whole level and always drops. Over empty space a drop fails. */
export function resolvePoint(store: EditorStore, a: PointArgs, units: Units = "world", what = "the point"): { map: Vec3; dropped: boolean } {
  if ((a.pos === undefined) === (a.xz === undefined)) throw new ToolError(`${what}: give exactly one of pos [x, y, z] or xz [x, z]`);
  const drop = a.xz !== undefined || !!a.drop;
  const raw: Vec3 = a.pos ? [a.pos[0], a.pos[1], a.pos[2]] : [a.xz![0], units === "map" ? topOf(store) : topOf(store) * 20, a.xz![1]];
  const map = pointToMap(raw, units);
  if (!drop) return { map, dropped: false };
  const hit = dropPoint(store.scene, store.frames, (id) => store.voxelsOf({ id }), map);
  if (!hit) throw new ToolError(`${what}: nothing below (${pointOut(map, units).join(", ")}) to drop onto (over empty space)`);
  return { map: hit, dropped: true };
}

/** Problems the patch of `scene` has (shape, and objects against the base): what store.problems() says of the live scene. */
function problemsOf(store: EditorStore, scene: Scene): string[] {
  const p = toPatch(store.base, scene, { base: store.baseVoxels, edited: store.voxels }, { base: store.baseSurround, edited: store.surround });
  const shape = validatePatch(p, JSON.stringify(p).length).errors;
  return shape.length || !p.objects?.length ? shape : validatePatchObjects(p, store.base).errors;
}

/** Runs the command as one undo step, unless it would make the patch invalid: it is tried on a copy of the scene first. */
export function commit(store: EditorStore, cmd: Command): void {
  const had = new Set(problemsOf(store, store.scene));
  const probe = structuredClone(store.scene);
  cmd.do(probe);
  const added = problemsOf(store, probe).filter((e) => !had.has(e));
  if (added.length) throw new ToolError(`refused, the patch would be invalid: ${added.slice(0, 5).join("; ")}`);
  store.exec(cmd);
}

export function findDetail(store: EditorStore, id: number): Detail {
  const d = store.detail(id);
  if (!d) throw new ToolError(`no detail ${id} (list_objects lists spawns and objects)`);
  return d;
}

/** One detail as the tools report it: position in the level (units) and in its frame, rotation in degrees. */
export function detailInfo(store: EditorStore, d: Detail, units: Units = "world") {
  const o = objectOf(store.scene, d);
  const level = store.frames.detailWorld(d);
  return {
    id: d.id, name: d.name, resource: d.resource, role: d.role, frame: d.frame, added: d.src === null,
    pos: pointOut(level, units), localPos: pointOut(d.pos, units),
    ...(d.rot.some((r) => r) ? { rotDeg: tidy(d.rot.map((r) => r * DEG) as Vec3, 3) } : {}),
    ...(d.scale.some((s) => s !== 1) ? { scale: tidy(d.scale) } : {}),
    ...(o ? { object: o } : {}),
  };
}
