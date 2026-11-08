// The faces of a voxel frame as triangles in the level, the way render_view draws them: every face of a solid cell whose
// neighbour in the same grid is empty, each corner through the grid's matrix and, for a draped block, moved up by the
// block's corner offset there (gridCorner). A sheared quad is not flat, so it is split along its shorter diagonal (the
// split that folds it least), and its two triangles carry their own normals: a sheared top is lit and culled as the
// slope it is, not as the flat face its grid says. A triangle with no area (a corner offset that closes a face) is
// left out. Pure; map units.
import type { Vec3 } from "../core";
import { isSolid, material, type Grid } from "./voxel";

/** Where a grid corner (integer lattice point x, y, z of the frame's voxels) is in the level, written to out[at..at+2].
 * Every face vertex goes through here, so a per-frame corner offset (a sheared column) belongs in this one function. */
export function gridCorner(g: Grid, x: number, y: number, z: number, out: Float64Array, at: number) {
  const m = g.toMap, c = g.frame?.corners;
  // A draped block's corner offsets (heightfield_drape) move its columns up, in its own voxels: faces follow them.
  if (c) y += c[Math.round(z) * (g.size[0] + 1) + Math.round(x)] ?? 0;
  out[at] = m[0] * x + m[1] * y + m[2] * z + m[3];
  out[at + 1] = m[4] * x + m[5] * y + m[6] * z + m[7];
  out[at + 2] = m[8] * x + m[9] * y + m[10] * z + m[11];
}

// The six faces of a cell: the neighbour's offset, and the face's four corners as offsets from the cell's low corner,
// counter-clockwise seen from outside (from the neighbour).
export const FACES: { d: Vec3; c: Vec3[] }[] = [
  { d: [1, 0, 0], c: [[1, 0, 0], [1, 1, 0], [1, 1, 1], [1, 0, 1]] },
  { d: [-1, 0, 0], c: [[0, 0, 0], [0, 0, 1], [0, 1, 1], [0, 1, 0]] },
  { d: [0, 1, 0], c: [[0, 1, 0], [0, 1, 1], [1, 1, 1], [1, 1, 0]] },
  { d: [0, -1, 0], c: [[0, 0, 0], [1, 0, 0], [1, 0, 1], [0, 0, 1]] },
  { d: [0, 0, 1], c: [[0, 0, 1], [1, 0, 1], [1, 1, 1], [0, 1, 1]] },
  { d: [0, 0, -1], c: [[0, 0, 0], [0, 1, 0], [1, 1, 0], [1, 0, 0]] },
];

/** The smallest triangle area kept (map units squared). */
export const MIN_AREA = 1e-9;

/** The two triangles of a face's four level points (pts[0..11]) as corner indices: split along the shorter diagonal. */
export function splitQuad(pts: Float64Array): [number, number, number, number, number, number] {
  const d02 = (pts[6] - pts[0]) ** 2 + (pts[7] - pts[1]) ** 2 + (pts[8] - pts[2]) ** 2;
  const d13 = (pts[9] - pts[3]) ** 2 + (pts[10] - pts[4]) ** 2 + (pts[11] - pts[5]) ** 2;
  return d02 <= d13 + 1e-12 ? [0, 1, 2, 0, 2, 3] : [0, 1, 3, 1, 2, 3];
}

/** A triangle's normal (unit; the winding's right-hand side) and area, from three level points of pts. */
export function triangleNormal(pts: Float64Array, a: number, b: number, c: number): { n: Vec3; area: number } {
  const ux = pts[b * 3] - pts[a * 3], uy = pts[b * 3 + 1] - pts[a * 3 + 1], uz = pts[b * 3 + 2] - pts[a * 3 + 2];
  const vx = pts[c * 3] - pts[a * 3], vy = pts[c * 3 + 1] - pts[a * 3 + 1], vz = pts[c * 3 + 2] - pts[a * 3 + 2];
  const nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx, l = Math.hypot(nx, ny, nz);
  return { n: l ? [nx / l, ny / l, nz / l] : [0, 0, 0], area: l / 2 };
}

export interface MeshTriangle {
  p: [Vec3, Vec3, Vec3];      // level points, counter-clockwise seen from outside
  n: Vec3;                    // unit normal
  area: number;
  face: number;               // FACES index: the cell side it belongs to
  out: Vec3;                  // that side's outward direction in the level (unsheared)
  cell: Vec3;                 // the cell
  material: number;
}

/** Every exposed face of a grid as triangles (a draped block's sheared), degenerate ones left out and counted. */
export function frameMesh(g: Grid): { triangles: MeshTriangle[]; degenerate: number } {
  const [X, Y, Z] = g.size, w = g.words, m = g.toMap, out: MeshTriangle[] = [];
  const pts = new Float64Array(12);
  let degenerate = 0;
  const axisOut = FACES.map(({ d }): Vec3 => {
    const v: Vec3 = [m[0] * d[0] + m[1] * d[1] + m[2] * d[2], m[4] * d[0] + m[5] * d[1] + m[6] * d[2], m[8] * d[0] + m[9] * d[1] + m[10] * d[2]];
    const l = Math.hypot(v[0], v[1], v[2]) || 1;
    return [v[0] / l, v[1] / l, v[2] / l];
  });
  for (let z = 0; z < Z; z++)
    for (let x = 0; x < X; x++)
      for (let y = 0; y < Y; y++) {
        const wd = w[(z * X + x) * Y + y];
        if (!isSolid(wd)) continue;
        for (let k = 0; k < 6; k++) {
          const d = FACES[k].d, nx = x + d[0], ny = y + d[1], nz = z + d[2];
          if (nx >= 0 && ny >= 0 && nz >= 0 && nx < X && ny < Y && nz < Z && isSolid(w[(nz * X + nx) * Y + ny])) continue;
          const c = FACES[k].c;
          for (let v = 0; v < 4; v++) gridCorner(g, x + c[v][0], y + c[v][1], z + c[v][2], pts, v * 3);
          const idx = splitQuad(pts);
          for (let t = 0; t < 6; t += 3) {
            const { n, area } = triangleNormal(pts, idx[t], idx[t + 1], idx[t + 2]);
            if (area < MIN_AREA) { degenerate++; continue; }
            const P = (i: number): Vec3 => [pts[i * 3], pts[i * 3 + 1], pts[i * 3 + 2]];
            out.push({ p: [P(idx[t]), P(idx[t + 1]), P(idx[t + 2])], n, area, face: k, out: axisOut[k], cell: [x, y, z], material: material(wd) });
          }
        }
      }
  return { triangles: out, degenerate };
}
