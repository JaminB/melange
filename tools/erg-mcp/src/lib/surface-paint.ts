// Painting the ground's surface over every frame, for paint_path and paint_region: a solid voxel is repainted when its
// centre lies in the area (a band along a path, a shape, an expression mask) and within `depth` under the top of the
// ground in its column, the ground being the highest solid voxel there over every voxel frame. So a road or a lava flow
// follows the surface across base frames and added blocks alike, and a sample over air paints nothing instead of failing.
// One undo step through VoxelEdit (the panel's checks: runs per frame, the patch budget).
//
// `blend` paints the edge per corner instead of per voxel: a surface voxel whose centre is outside the area but some of
// whose corners are inside it (within `falloff` more) takes the material as its second material on those corners (the
// game's per-corner blend, as the panel's 2nd-material brush and heightfield `blend`), so the game draws a gradient
// across the boundary voxel instead of a step; on a 2.5-unit skin that is the difference between a smooth lava edge and
// a staircase of red and black squares.
import { CORNER_BIT, bordered, cornerOffset, maskOf, painted, type EditorStore, type Vec3 } from "../core";
import { ToolError } from "../registry";
import { compile, ExprError, fbm, noise2 } from "./heightfield-expr";
import { spansAt } from "./inspect-field";
import { VoxelEdit, WORLD_PER_MAP, grids, isSolid, material } from "./voxel";

// A circle may wander: `wobble` makes its radius radius x (1 + amount x fbm round the circle, `lobes` bumps): an
// organic pool or blob (paint_blob) instead of a disc.
export type Wobble = { amount: number; lobes?: number; seed?: number };
export type Shape = { x: number; z: number; radius: number; wobble?: Wobble } | { min: [number, number]; max: [number, number] } | { points: [number, number][] };

export interface SurfacePaint {
  material: number;
  depth: number;                            // map units under the ground's top that are painted (1: the top voxel)
  // what is measured against depth: the voxel's centre (the default, the rule since the tool was made) or its top face.
  // With "centre" a voxel taller than 2 x depth is never painted (a 3-unit plinth voxel and depth 1); "top" paints every
  // voxel whose top face is less than depth under the ground's top, so the top voxel always is, whatever its frame's scale.
  depthFrom?: "centre" | "top";
  path?: Vec3[];                            // map units; the band is `width` across (x, z only)
  width?: number;                           // map units
  taper?: number;                           // the band's width at the end as a multiple of width
  shapes?: Shape[];                         // map units: paint inside any of these
  mask?: string;                            // an expression of the column: paint where it is above 0
  centre?: [number, number];                // map x, z the mask's r and theta are about (default 0, 0)
  units: "map" | "world";                   // of the mask's x, z, h, y and d
  jitter?: { amount: number; scale?: number; seed?: number };   // moves the band's edge (map units) by noise
  blend?: { falloff: number };              // per-corner edge (map units past the edge a corner still counts)
  exclude?: Shape[];                        // map units: never paint inside these
  excludeMask?: string;                     // never where this expression is above 0
  only?: number[];                          // repaint only voxels of these materials
  except?: number[];                        // never repaint these
  dryRun?: boolean;
  label: string;
}

const MASK_VARS = ["h", "y", "d", "s"] as const;

/** The slope in degrees of column (x, z) of a frame from its four corner offsets (0 for a frame without corners): the
 * steepest of the four triangles its top quad can split into (either diagonal), since the game draws a steep triangle of
 * a draped skin with its material's side texture (Caldera's pale slivers: docs/erg-mcp.md, detail layer). */
export function frameColumnSlope(f: { size: number[]; corners?: ArrayLike<number> | null }, x: number, z: number): number {
  const c = f.corners;
  if (!c) return 0;
  const W = f.size[0] + 1, a = c[z * W + x], b = c[z * W + x + 1], e = c[(z + 1) * W + x], g = c[(z + 1) * W + x + 1];
  const t = Math.max(Math.hypot(b - a, e - a), Math.hypot(b - a, g - b), Math.hypot(g - e, e - a), Math.hypot(g - e, g - b));
  return (Math.atan(t) * 180) / Math.PI;
}

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

/** A circle's radius towards (x, z): its radius, or with a wobble the radius at that bearing. */
function radiusAt(s: { x: number; z: number; radius: number; wobble?: Wobble }, x: number, z: number): number {
  if (!s.wobble || !s.wobble.amount) return s.radius;
  const th = Math.atan2(z - s.z, x - s.x), f = (s.wobble.lobes ?? 5) / (2 * Math.PI);
  return s.radius * Math.max(0.05, 1 + s.wobble.amount * fbm(f * Math.cos(th) + 17.3, f * Math.sin(th) + 5.1, s.wobble.seed ?? 1, 3));
}

function inShape(s: Shape, x: number, z: number): boolean {
  if ("radius" in s) { const r = radiusAt(s, x, z); return (x - s.x) ** 2 + (z - s.z) ** 2 <= r * r; }
  if ("min" in s) return x >= Math.min(s.min[0], s.max[0]) && x <= Math.max(s.min[0], s.max[0]) && z >= Math.min(s.min[1], s.max[1]) && z <= Math.max(s.min[1], s.max[1]);
  let inside = false;
  const p = s.points;
  for (let i = 0, j = p.length - 1; i < p.length; j = i++)
    if ((p[i][1] > z) !== (p[j][1] > z) && x < ((p[j][0] - p[i][0]) * (z - p[i][1])) / (p[j][1] - p[i][1]) + p[i][0]) inside = !inside;
  return inside;
}

function shapeBox(s: Shape): [number, number, number, number] {
  if ("radius" in s) { const r = s.radius * (1 + Math.abs(s.wobble?.amount ?? 0)); return [s.x - r, s.z - r, s.x + r, s.z + r]; }
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
  const compiled = (src: string, what: string) => {
    try {
      return compile(src, MASK_VARS) as unknown as (v: Record<string, number>) => number;
    } catch (e) {
      if (e instanceof ExprError) throw new ToolError(`${what}: ${e.message}`);
      throw e;
    }
  };
  const mask = o.mask ? compiled(o.mask, "mask") : null, excludeMask = o.excludeMask ? compiled(o.excludeMask, "excludeMask") : null;
  const cum = [0];
  if (o.path) for (let i = 1; i < o.path.length; i++) cum.push(cum[i - 1] + Math.hypot(o.path[i][0] - o.path[i - 1][0], o.path[i][2] - o.path[i - 1][2]));
  const fall = o.blend ? Math.max(0, o.blend.falloff) : 0;
  const jit = o.jitter?.amount ?? 0, maxHalf = (o.width ?? 0) / 2 * Math.max(1, o.taper ?? 1) + Math.abs(jit) + fall + (o.blend ? 2 : 0);
  // the area's box in x, z: the path's, the shapes', or (a mask alone) the whole level
  let box: [number, number, number, number] | null = null;
  const grow = (b: [number, number, number, number]) => { box = box ? [Math.min(box[0], b[0]), Math.min(box[1], b[1]), Math.max(box[2], b[2]), Math.max(box[3], b[3])] : b; };
  if (o.path) for (const p of o.path) grow([p[0] - maxHalf, p[2] - maxHalf, p[0] + maxHalf, p[2] + maxHalf]);
  for (const s of o.shapes ?? []) grow(shapeBox(s));
  if (box && o.blend) { const b: [number, number, number, number] = box; box = [b[0] - fall - 2, b[1] - fall - 2, b[2] + fall + 2, b[3] + fall + 2]; }
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
  const vars: Record<string, number> = { x: 0, z: 0, u: 0, v: 0, r: 0, theta: 0, W: 0, D: 0, R: 0, pi: Math.PI, e: Math.E, h: 0, y: 0, d: 0, s: 0 };
  const only = o.only ? new Set(o.only) : null, except = new Set(o.except ?? []);
  const edit = new VoxelEdit(store);
  const per = new Map<number, number>();
  let looked = 0, blended = 0;
  // surface voxels in the area that only the depth rule left out, and the tallest of them (for the refusal's reason)
  let tooDeep = 0, tallest = 0;
  // whether a point of a voxel in a column whose ground top is `top` is in the area; `extra` widens a path's band and
  // a shape's edge (map units), for the blend's corners. d: the distance to the path (0 without one).
  const inArea = (px: number, py: number, pz: number, top: number, extra: number): { d: number } | null => {
    let d = 0;
    if (o.path) {
      const q = toPath(o.path, cum, px, pz);
      const half = (o.width! / 2) * (1 + ((o.taper ?? 1) - 1) * q.t) + (jit ? jit * noise2(px / (o.jitter!.scale ?? 8), pz / (o.jitter!.scale ?? 8), o.jitter!.seed ?? 0) : 0);
      if (q.d > half + extra) return null;
      d = q.d;
    }
    if (o.shapes?.length && !o.shapes.some((s) => inShape(s, px, pz) || (extra > 0 && inShape(grown(s, extra), px, pz)))) return null;
    if (o.exclude?.some((s) => inShape(s, px, pz))) return null;
    if (mask || excludeMask) {
      vars.x = px * k; vars.z = pz * k; vars.h = top * k; vars.y = py * k; vars.d = d * k;
      vars.r = Math.hypot(px - cx0, pz - cz0) * k; vars.theta = Math.atan2(pz - cz0, px - cx0);
      if (mask && !(mask(vars) > 0)) return null;
      if (excludeMask && excludeMask(vars) > 0) return null;
    }
    return { d };
  };
  for (const g of gs) {
    const b = g.bounds;
    if (box && (b.max[0] < box[0] || b.min[0] > box[2] || b.max[2] < box[1] || b.min[2] > box[3])) continue;
    const m = g.toMap, [X, Y, Z] = g.size;
    const tall = Math.hypot(m[1], m[5], m[9]), lift = o.depthFrom === "top" ? tall / 2 : 0;
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
          vars.s = frameColumnSlope(g.frame, x, z);
          const top = groundTop(px, pz);
          if (o.depthFrom === "top" ? py + lift <= top - o.depth + 1e-9 : py < top - o.depth) {
            // the column's top voxel, in the area, left out by the centre rule alone: counted, so a refusal can say why
            if (o.depthFrom !== "top" && py + tall / 2 >= top - 1e-6 && inArea(px, py, pz, top, 0)) { tooDeep++; tallest = Math.max(tallest, tall); }
            continue;
          }
          if (inArea(px, py, pz, top, 0)) {
            // a voxel with a second material of its own (an earlier blend) keeps it: written as such a word
            edit.set(g.id, x, y, z, painted(w, o.material), maskOf(w) !== 0);
            per.set(g.id, (per.get(g.id) ?? 0) + 1);
            continue;
          }
          if (!o.blend) continue;
          // the edge: the corners of this voxel that are in the area (within falloff), as second-material corners
          let bits = 0;
          for (let ky = 0; ky < 2; ky++)
            for (let kz = 0; kz < 2; kz++)
              for (let kx = 0; kx < 2; kx++) {
                const qx = m[0] * (x + kx) + m[1] * (y + ky) + m[2] * (z + kz) + m[3], qz = m[8] * (x + kx) + m[9] * (y + ky) + m[10] * (z + kz) + m[11];
                const qy = m[4] * (x + kx) + m[5] * (y + ky) + m[6] * (z + kz) + m[7];
                if (inArea(qx, qy, qz, top, fall)) bits |= 1 << CORNER_BIT[ky][kz][kx];
              }
          if (!bits) continue;
          edit.set(g.id, x, y, z, bordered(w, o.material, bits), true);
          per.set(g.id, (per.get(g.id) ?? 0) + 1);
          blended++;
        }
  }
  const why = edit.check();
  if (why) throw new ToolError(why);
  const changed = edit.size;
  const r2 = (v: number) => Math.round(v * 100) / 100;
  // depth is measured to a voxel's centre: a frame whose voxels are taller than 2 x depth has no centre within it
  const deepWhy = tooDeep ? `${tooDeep} top voxel${tooDeep === 1 ? "" : "s"} in the area ${tooDeep === 1 ? "is" : "are"} up to ${r2(tallest)} map units tall, so ${tooDeep === 1 ? "its" : "their"} centre is ${r2(tallest / 2)} under the ground's top, deeper than depth ${r2(o.depth)} (measured to the voxel's centre): give depth >= ${r2(tallest / 2)} map units, or depthFrom: "top"` : "";
  if (!changed) throw new ToolError(tooDeep ? `nothing painted: ${deepWhy}` : looked ? "nothing to repaint: no solid surface voxel of another material is in the area (or every one is already that material)" : "no voxel frame has solid ground in the area");
  if (!o.dryRun) edit.commit(o.label);
  const name = (id: number) => store.scene.frames.find((f) => f.id === id)?.name ?? String(id);
  return { changed, ...(tooDeep ? { skippedByDepth: { topVoxels: tooDeep, tallest: r2(tallest), why: deepWhy } } : {}), ...(o.blend ? { blendedEdge: blended } : {}), frames: [...per].sort((a, b) => b[1] - a[1]).slice(0, 60).map(([id, n]) => ({ id, name: name(id), changed: n })), ...(o.dryRun ? { dryRun: true } : { label: o.label }) };
}

/** A shape grown by `by` map units (circles and rectangles; a polygon is left as it is). */
function grown(s: Shape, by: number): Shape {
  if ("radius" in s) return { x: s.x, z: s.z, radius: s.radius + by, ...(s.wobble ? { wobble: { ...s.wobble, amount: (s.wobble.amount * s.radius) / (s.radius + by) } } : {}) };
  if ("min" in s) return { min: [Math.min(s.min[0], s.max[0]) - by, Math.min(s.min[1], s.max[1]) - by], max: [Math.max(s.min[0], s.max[0]) + by, Math.max(s.min[1], s.max[1]) + by] };
  return s;
}
