// What a crater shows. A structure's voxels are all the material its faces were asked in, bands and trim included, so a
// blast that digs into it opens onto whatever band lies there, drawn by its TOP texture: Dustcrown's plinth has a base
// band of the Arabian theme's bricks, whose sides are brick and whose top is a white and blue tile, and a crater through
// the ledge showed a lilac and blue floor that read as a hole to the sky (round 4). Two things here, both pure:
//   withCore       a structure plan's fills take a core material wherever a cell is deeper than `depth` under the plan's
//                  own surface (every axis; the ground under the plan counts as solid), so a hole opens onto bedrock
//   exposedCore    trial craters cut into the level's voxels as they are (nothing is edited): the cells each crater
//                  uncovers, by material and by the face it uncovers (a top, a side, an underside)
//   colourReads    whether a colour reads as earth, or as sky, ice or water (blue, violet, near white)
import { cornerOffset, isSolid, material, type Vec3 } from "../core";
import type { Grid } from "./voxel";
import type { Shape } from "./structures-shapes";

/** The plan's fills with `core` as the material of every cell deeper than `depth` (map units) under the plan's surface.
 * The surface is the plan's own: a cell is deep when the plan is solid `depth` away along each axis, up, down and to
 * the four sides (under the plan's lowest fill counts as solid: it stands on the ground). Carves are honoured, so a
 * doorway's and a niche's walls keep their face material `depth` deep. */
export function withCore(shapes: Shape[], core: number, depth: number): Shape[] {
  const solids = shapes.filter((s) => s.op !== "paint");
  if (!solids.some((s) => s.op === "fill")) return shapes;
  const floorY = Math.min(...solids.filter((s) => s.op === "fill").map((s) => s.min[1]));
  const solid = (x: number, y: number, z: number) => {
    if (y < floorY) return true;
    let on = false;
    for (const s of solids) {
      if (x < s.min[0] || x >= s.max[0] || y < s.min[1] || y >= s.max[1] || z < s.min[2] || z >= s.max[2] || !s.hit(x, y, z)) continue;
      on = s.op === "fill";
    }
    return on;
  };
  const deep = (x: number, y: number, z: number) =>
    solid(x + depth, y, z) && solid(x - depth, y, z) && solid(x, y + depth, z) && solid(x, y - depth, z) && solid(x, y, z + depth) && solid(x, y, z - depth);
  return shapes.map((s) => {
    if (s.op !== "fill") return s;
    const base = s.matAt;
    return { ...s, matAt: (x: number, y: number, z: number) => (deep(x, y, z) ? core : base ? base(x, y, z) : s.mat) };
  });
}

export type Reads = "earth" | "blue" | "white";

/** How a colour reads on a crater's floor: blue (a hue from cyan to violet with some saturation: sky, water, ice, the
 * lilac of a pale tile), white (bright and nearly grey: snow, plaster, sky haze) or earth (everything else). */
export function colourReads(rgb: [number, number, number]): Reads {
  const r = rgb[0] / 255, g = rgb[1] / 255, b = rgb[2] / 255, hi = Math.max(r, g, b), lo = Math.min(r, g, b), d = hi - lo;
  const sat = hi > 0 ? d / hi : 0;
  let hue = 0;
  if (d > 1e-6) hue = hi === r ? ((g - b) / d + 6) % 6 : hi === g ? (b - r) / d + 2 : (r - g) / d + 4;
  hue *= 60;
  // a pale cool tint (lilac, ice) is blue long before it is saturated: the cooler the hue the less it takes
  if (hue >= 165 && hue <= 300 && sat >= (hi > 0.75 ? 0.04 : 0.12)) return "blue";
  if (hi >= 0.82 && sat < 0.08) return "white";
  return "earth";
}

export type Face = "top" | "side" | "bottom";
export interface CraterFind { material: number; face: Face; voxels: number; craters: number; firstAt: Vec3; }

/** Trial craters: at each point a sphere of `radius`, then `passes` - 1 more, each 0.8 of a radius under the last (a
 * second and third hit in the same hole). Solid cells inside any of them are carved (frames tagged PERM never are, as in
 * the game); a solid cell left with a carved neighbour is uncovered, by its top when the cell above went, else by a side,
 * else from below. Answers what is uncovered by material and face, and how many cells each point carved. */
export function exposedCore(gs: Grid[], points: Vec3[], radius: number, passes: number): { finds: CraterFind[]; carved: number[] } {
  const finds = new Map<string, CraterFind>(), carvedPer: number[] = [];
  const reach = radius * (1 + 0.8 * (passes - 1));
  points.forEach((p, pi) => {
    let carvedHere = 0;
    const seen = new Set<string>();
    for (const g of gs) {
      const b = g.bounds, lift = g.frame.corners ? [Math.min(...g.frame.corners), Math.max(...g.frame.corners)] : [0, 0];
      const sy = Math.hypot(g.toMap[1], g.toMap[5], g.toMap[9]);
      if (b.max[0] < p[0] - radius || b.min[0] > p[0] + radius || b.max[2] < p[2] - radius || b.min[2] > p[2] + radius ||
        b.max[1] + lift[1] * sy < p[1] - reach || b.min[1] + lift[0] * sy > p[1] + radius) continue;
      if (/PERM/i.test(g.frame.name)) continue;
      const [X, Y, Z] = g.size, m = g.toMap, f = g.fromMap;
      // the cells whose centres can lie in the craters: the box of the capsule in grid space
      const lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
      for (let k = 0; k < 8; k++) {
        const q = [p[0] + (k & 1 ? radius : -radius), p[1] + (k & 2 ? radius : -reach), p[2] + (k & 4 ? radius : -radius)];
        for (let a = 0; a < 3; a++) { const v = f[a * 4] * q[0] + f[a * 4 + 1] * q[1] + f[a * 4 + 2] * q[2] + f[a * 4 + 3]; lo[a] = Math.min(lo[a], v); hi[a] = Math.max(hi[a], v); }
      }
      lo[1] -= lift[1]; hi[1] -= lift[0];
      const x0 = Math.max(0, Math.floor(lo[0]) - 1), x1 = Math.min(X - 1, Math.ceil(hi[0]) + 1), y0 = Math.max(0, Math.floor(lo[1]) - 1), y1 = Math.min(Y - 1, Math.ceil(hi[1]) + 1);
      const z0 = Math.max(0, Math.floor(lo[2]) - 1), z1 = Math.min(Z - 1, Math.ceil(hi[2]) + 1);
      if (x0 > x1 || y0 > y1 || z0 > z1) continue;
      const centre = (x: number, y: number, z: number): Vec3 => {
        const cy = y + 0.5 + (g.frame.corners ? cornerOffset(g.frame, x + 0.5, z + 0.5) : 0);
        return [m[0] * (x + 0.5) + m[1] * cy + m[2] * (z + 0.5) + m[3], m[4] * (x + 0.5) + m[5] * cy + m[6] * (z + 0.5) + m[7], m[8] * (x + 0.5) + m[9] * cy + m[10] * (z + 0.5) + m[11]];
      };
      const gone = (x: number, y: number, z: number) => {
        if (x < 0 || y < 0 || z < 0 || x >= X || y >= Y || z >= Z || !isSolid(g.words[(z * X + x) * Y + y])) return false;
        const c = centre(x, y, z), dx = c[0] - p[0], dz = c[2] - p[2];
        if (dx * dx + dz * dz > radius * radius) return false;
        for (let i = 0; i < passes; i++) { const dy = c[1] - (p[1] - 0.8 * radius * i); if (dx * dx + dy * dy + dz * dz <= radius * radius) return true; }
        return false;
      };
      for (let z = z0; z <= z1; z++) for (let x = x0; x <= x1; x++) for (let y = y0; y <= y1; y++) {
        const w = g.words[(z * X + x) * Y + y];
        if (!isSolid(w)) continue;
        if (gone(x, y, z)) { carvedHere++; continue; }
        const face: Face | null = gone(x, y + 1, z) ? "top" : gone(x + 1, y, z) || gone(x - 1, y, z) || gone(x, y, z + 1) || gone(x, y, z - 1) ? "side" : gone(x, y - 1, z) ? "bottom" : null;
        if (!face) continue;
        const key = `${material(w)}|${face}`;
        let fd = finds.get(key);
        if (!fd) finds.set(key, (fd = { material: material(w), face, voxels: 0, craters: 0, firstAt: centre(x, y, z) }));
        fd.voxels++;
        if (!seen.has(key)) { seen.add(key); fd.craters++; }
      }
    }
    carvedPer[pi] = carvedHere;
  });
  return { finds: [...finds.values()].sort((a, b) => b.voxels - a.voxels), carved: carvedPer };
}
