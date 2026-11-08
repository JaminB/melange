// Painting the ground's surface over every frame, for paint_path and paint_region: a solid voxel is repainted when its
// centre lies in the area (a band along a path, a shape, an expression mask) and within `depth` under the top of the
// ground in its column, the ground being the highest solid voxel there over every voxel frame. So a road or a lava flow
// follows the surface across base frames and added blocks alike, and a sample over air paints nothing instead of failing.
// One undo step through VoxelEdit (the panel's checks: runs per frame, the patch budget).
import { cornerOffset, painted, type EditorStore, type Vec3 } from "../core";
import { ToolError } from "../registry";
import { compile, ExprError, noise2 } from "./heightfield-expr";
import { spansAt } from "./inspect-field";
import { VoxelEdit, WORLD_PER_MAP, grids, isSolid, material } from "./voxel";

export type Shape = { x: number; z: number; radius: number } | { min: [number, number]; max: [number, number] } | { points: [number, number][] };

export interface SurfacePaint {
  material: number;
  depth: number;                            // map units under the ground's top that are painted (1: the top voxel)
  path?: Vec3[];                            // map units; the band is `width` across (x, z only)
  width?: number;                           // map units
  taper?: number;                           // the band's width at the end as a multiple of width
  shapes?: Shape[];                         // map units: paint inside any of these
  mask?: string;                            // an expression of the column: paint where it is above 0
  centre?: [number, number];                // map x, z the mask's r and theta are about (default 0, 0)
  units: "map" | "world";                   // of the mask's x, z, h, y and d
  jitter?: { amount: number; scale?: number; seed?: number };   // moves the band's edge (map units) by noise
  only?: number[];                          // repaint only voxels of these materials
  except?: number[];                        // never repaint these
  dryRun?: boolean;
  label: string;
}

const MASK_VARS = ["h", "y", "d"] as const;

/** Distance in x, z from p to the polyline, and how far along it (0-1) the nearest point is. */
function toPath(path: Vec3[], cum: number[], x: number, z: number): { d: number; t: number } {
  let best = Infinity, at = 0;
  for (let i = 1; i < path.length; i++) {
    const a = path[i - 1], b = path[i], ex = b[0] - a[0], ez = b[2] - a[2], len2 = ex * ex + ez * ez;
    const u = len2 > 0 ? Math.max(0, Math.min(1, ((x - a[0]) * ex + (z - a[2]) * ez) / len2)) : 0;
    const d = Math.hypot(x - (a[0] + ex * u), z - (a[2] + ez * u));
    if (d < best) { best = d; at = cum[i - 1] + u * (cum[i] - cum[i - 1]); }
  }
  if (path.length === 1) best = Math.hypot(x - path[0][0], z - path[0][2]);
  const total = cum[cum.length - 1];
  return { d: best, t: total > 0 ? at / total : 0 };
}

function inShape(s: Shape, x: number, z: number): boolean {
  if ("radius" in s) return (x - s.x) ** 2 + (z - s.z) ** 2 <= s.radius * s.radius;
  if ("min" in s) return x >= Math.min(s.min[0], s.max[0]) && x <= Math.max(s.min[0], s.max[0]) && z >= Math.min(s.min[1], s.max[1]) && z <= Math.max(s.min[1], s.max[1]);
  let inside = false;
  const p = s.points;
  for (let i = 0, j = p.length - 1; i < p.length; j = i++)
    if ((p[i][1] > z) !== (p[j][1] > z) && x < ((p[j][0] - p[i][0]) * (z - p[i][1])) / (p[j][1] - p[i][1]) + p[i][0]) inside = !inside;
  return inside;
}

function shapeBox(s: Shape): [number, number, number, number] {
  if ("radius" in s) return [s.x - s.radius, s.z - s.radius, s.x + s.radius, s.z + s.radius];
  if ("min" in s) return [Math.min(s.min[0], s.max[0]), Math.min(s.min[1], s.max[1]), Math.max(s.min[0], s.max[0]), Math.max(s.min[1], s.max[1])];
  const xs = s.points.map((q) => q[0]), zs = s.points.map((q) => q[1]);
  return [Math.min(...xs), Math.min(...zs), Math.max(...xs), Math.max(...zs)];
}

/** Repaints the surface; answers what changed per frame. */
export function paintSurface(store: EditorStore, o: SurfacePaint) {
  if (!(Number.isInteger(o.material) && o.material >= 0 && o.material <= 63)) throw new ToolError("material is a whole number 0-63");
  if (!o.path && !o.shapes?.length && !o.mask) throw new ToolError("give a path, shapes or a mask");
  if (o.path && !(o.width! > 0)) throw new ToolError("a path needs a width above 0");
  const k = o.units === "world" ? WORLD_PER_MAP : 1;
  let mask: ((v: Record<string, number>) => number) | null = null;
  if (o.mask) {
    try {
      mask = compile(o.mask, MASK_VARS) as unknown as (v: Record<string, number>) => number;
    } catch (e) {
      if (e instanceof ExprError) throw new ToolError(`mask: ${e.message}`);
      throw e;
    }
  }
  const cum = [0];
  if (o.path) for (let i = 1; i < o.path.length; i++) cum.push(cum[i - 1] + Math.hypot(o.path[i][0] - o.path[i - 1][0], o.path[i][2] - o.path[i - 1][2]));
  const jit = o.jitter?.amount ?? 0, maxHalf = (o.width ?? 0) / 2 * Math.max(1, o.taper ?? 1) + Math.abs(jit);
  // the area's box in x, z: the path's, the shapes', or (a mask alone) the whole level
  let box: [number, number, number, number] | null = null;
  const grow = (b: [number, number, number, number]) => { box = box ? [Math.min(box[0], b[0]), Math.min(box[1], b[1]), Math.max(box[2], b[2]), Math.max(box[3], b[3])] : b; };
  if (o.path) for (const p of o.path) grow([p[0] - maxHalf, p[2] - maxHalf, p[0] + maxHalf, p[2] + maxHalf]);
  for (const s of o.shapes ?? []) grow(shapeBox(s));
  const gs = grids(store);
  // ground tops by column, cached on a quarter-unit lattice (rotated frames' cells fall anywhere)
  const tops = new Map<number, number>();
  const groundTop = (x: number, z: number) => {
    const key = Math.round(x * 4) * 1e6 + Math.round(z * 4);
    let t = tops.get(key);
    if (t === undefined) {
      t = -Infinity;
      for (const s of spansAt(gs, x, z)) t = Math.max(t, s.top);
      tops.set(key, t);
    }
    return t;
  };
  const [cx0, cz0] = o.centre ?? [0, 0];
  const vars: Record<string, number> = { x: 0, z: 0, u: 0, v: 0, r: 0, theta: 0, W: 0, D: 0, R: 0, pi: Math.PI, e: Math.E, h: 0, y: 0, d: 0 };
  const only = o.only ? new Set(o.only) : null, except = new Set(o.except ?? []);
  const edit = new VoxelEdit(store);
  const per = new Map<number, number>();
  let looked = 0;
  for (const g of gs) {
    const b = g.bounds;
    if (box && (b.max[0] < box[0] || b.min[0] > box[2] || b.max[2] < box[1] || b.min[2] > box[3])) continue;
    const m = g.toMap, [X, Y, Z] = g.size;
    for (let z = 0; z < Z; z++)
      for (let x = 0; x < X; x++)
        for (let y = 0; y < Y; y++) {
          const w = g.words[(z * X + x) * Y + y];
          if (!isSolid(w) || material(w) === o.material || except.has(material(w)) || (only && !only.has(material(w)))) continue;
          const cx = x + 0.5, cy = y + 0.5, cz = z + 0.5;
          const px = m[0] * cx + m[1] * cy + m[2] * cz + m[3], pz = m[8] * cx + m[9] * cy + m[10] * cz + m[11];
          if (box && (px < box[0] || px > box[2] || pz < box[1] || pz > box[3])) continue;
          let py = m[4] * cx + m[5] * cy + m[6] * cz + m[7];
          if (g.frame.corners) py += cornerOffset(g.frame, cx, cz) * m[5];
          looked++;
          let d = 0;
          if (o.path) {
            const q = toPath(o.path, cum, px, pz);
            const half = (o.width! / 2) * (1 + ((o.taper ?? 1) - 1) * q.t) + (jit ? jit * noise2(px / (o.jitter!.scale ?? 8), pz / (o.jitter!.scale ?? 8), o.jitter!.seed ?? 0) : 0);
            if (q.d > half) continue;
            d = q.d;
          }
          if (o.shapes?.length && !o.shapes.some((s) => inShape(s, px, pz))) continue;
          const top = groundTop(px, pz);
          if (py < top - o.depth) continue;
          if (mask) {
            vars.x = px * k; vars.z = pz * k; vars.h = top * k; vars.y = py * k; vars.d = d * k;
            vars.r = Math.hypot(px - cx0, pz - cz0) * k; vars.theta = Math.atan2(pz - cz0, px - cx0);
            if (!(mask(vars) > 0)) continue;
          }
          edit.set(g.id, x, y, z, painted(w, o.material));
          per.set(g.id, (per.get(g.id) ?? 0) + 1);
        }
  }
  const why = edit.check();
  if (why) throw new ToolError(why);
  const changed = edit.size;
  if (!changed) throw new ToolError(looked ? "nothing to repaint: no solid surface voxel of another material is in the area (or every one is already that material)" : "no voxel frame has solid ground in the area");
  if (!o.dryRun) edit.commit(o.label);
  const name = (id: number) => store.scene.frames.find((f) => f.id === id)?.name ?? String(id);
  return { changed, frames: [...per].sort((a, b) => b[1] - a[1]).slice(0, 60).map(([id, n]) => ({ id, name: name(id), changed: n })), ...(o.dryRun ? { dryRun: true } : { label: o.label }) };
}
