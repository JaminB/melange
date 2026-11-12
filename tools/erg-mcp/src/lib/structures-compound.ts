// build_compound's geometry: one structure from an ordered list of solids (box, cylinder, cone, dome, tiers) and the carve
// of each, on the Shape interface of structures-shapes.ts. Pure functions in local coordinates (the structure's origin on
// the ground, y up) and map units; the tool scales the caller's units and places the result.
import type { Vec3 } from "../core";
import { onionProfile, place, styleMat, type MatBand, type MatStyle, type Shape, type Trim } from "./structures-shapes";

export type CompoundKind = "box" | "cylinder" | "cone" | "dome" | "tiers";
export type DomeProfile = "round" | "onion";

export type CompoundBand = MatBand;

/** One entry of the list, every length in map units. */
export interface CompoundSpec {
  kind: CompoundKind;
  carve: boolean;
  at: Vec3;                                  // the shape's base centre, relative to the compound's origin
  material: number;
  part?: number;
  bands?: CompoundBand[];                    // material by height above the shape's base (fill only): the first band holding y wins
  trim?: Trim;                               // fill only: top (the topmost layer of the shape) and course (a stripe a voxel high), by height above the shape's base
  size?: [number, number];                   // box and tiers: footprint [x, z] (tiers: the base step's)
  height?: number;                           // box, cylinder, cone, dome
  radius?: number;                           // cylinder, cone (base), dome (the base circle), tiers with round (the base step's)
  topRadius?: number;                        // cone: the radius at the top (default 0, a point)
  profile?: DomeProfile;
  steps?: number; riser?: number; ledge?: number; round?: boolean;   // tiers
}

/** The width of an onion dome's profile (1 at the base circle's radius) at t, 0 at its foot to 1 at its tip: a neck, a
 * bulge a third of the way up that is a tenth wider than the base, then a taper to a point. */
export function onionWidth(t: number): number {
  return onionProfile(t);
}

/** The height of a tiers shape, in map units. */
export const tiersHeight = (c: { steps: number; riser: number }) => c.steps * c.riser;

/** The shape of one entry in local coordinates, with its own material bands and part. */
export function compoundShape(c: CompoundSpec): Shape {
  const [cx, y0, cz] = c.at;
  const op = c.carve ? "carve" : "fill";
  let min: Vec3, max: Vec3, hit: (x: number, y: number, z: number) => boolean;
  if (c.kind === "box") {
    const hx = c.size![0] / 2, hz = c.size![1] / 2, h = c.height!;
    min = [cx - hx, y0, cz - hz]; max = [cx + hx, y0 + h, cz + hz];
    hit = () => true;
  } else if (c.kind === "cylinder") {
    const r = c.radius!, h = c.height!;
    min = [cx - r, y0, cz - r]; max = [cx + r, y0 + h, cz + r];
    hit = (x, _y, z) => (x - cx) ** 2 + (z - cz) ** 2 < r * r;
  } else if (c.kind === "cone") {
    const r0 = c.radius!, r1 = c.topRadius ?? 0, h = c.height!, rm = Math.max(r0, r1);
    min = [cx - rm, y0, cz - rm]; max = [cx + rm, y0 + h, cz + rm];
    hit = (x, y, z) => { const r = r0 + (r1 - r0) * ((y - y0) / h); return (x - cx) ** 2 + (z - cz) ** 2 < r * r; };
  } else if (c.kind === "dome") {
    const r = c.radius!, h = c.height!;
    if (c.profile === "onion") {
      min = [cx - r * 1.1, y0, cz - r * 1.1]; max = [cx + r * 1.1, y0 + h, cz + r * 1.1];
      hit = (x, y, z) => { const w = r * onionWidth((y - y0) / h); return (x - cx) ** 2 + (z - cz) ** 2 < w * w; };
    } else if (h <= r) {
      // a sphere cap: the sphere of radius R through the base circle and the apex, its centre R - h under the base
      const R = (r * r + h * h) / (2 * h), yc = y0 + h - R;
      min = [cx - r, y0, cz - r]; max = [cx + r, y0 + h, cz + r];
      hit = (x, y, z) => (x - cx) ** 2 + (z - cz) ** 2 + (y - yc) ** 2 < R * R;
    } else {
      // taller than it is wide: the upper half of an ellipsoid
      min = [cx - r, y0, cz - r]; max = [cx + r, y0 + h, cz + r];
      hit = (x, y, z) => ((x - cx) ** 2 + (z - cz) ** 2) / (r * r) + ((y - y0) / h) ** 2 < 1;
    }
  } else {
    const steps = c.steps!, riser = c.riser!, ledge = c.ledge!;
    const hx = c.round ? c.radius! : c.size![0] / 2, hz = c.round ? c.radius! : c.size![1] / 2;
    min = [cx - hx, y0, cz - hz]; max = [cx + hx, y0 + steps * riser, cz + hz];
    hit = (x, y, z) => {
      const i = Math.min(steps - 1, Math.floor((y - y0) / riser)), d = i * ledge;
      return c.round ? (x - cx) ** 2 + (z - cz) ** 2 < (hx - d) ** 2 : Math.abs(x - cx) < hx - d && Math.abs(z - cz) < hz - d;
    };
  }
  const full = hit;
  const s: Shape = { op, mat: c.material, min, max, hit: (x, y, z) => x >= min[0] && x < max[0] && y >= min[1] && y < max[1] && z >= min[2] && z < max[2] && full(x, y, z) };
  if (c.part !== undefined) s.part = c.part;
  const st: MatStyle = { bands: c.bands, trim: c.trim };
  if (!c.carve && (c.bands?.length || c.trim?.top !== undefined || c.trim?.course)) {
    const h = max[1] - y0;
    s.matAt = (_x, y) => styleMat(c.material, st, y - y0, h);
  }
  return s;
}

/** The structure's shapes in the list's order (a later carve opens an earlier fill, a later fill closes a carve), yawed
 * about the origin and moved to `at`. */
export function compound(specs: CompoundSpec[], at: Vec3, yawDeg: number): Shape[] {
  return place(specs.map(compoundShape), at, yawDeg);
}
