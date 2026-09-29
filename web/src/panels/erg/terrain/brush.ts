// Brushes: a box or an ellipsoid measured in the voxels of the frame under the cursor (the anchor), reaching into every
// frame it overlaps. A voxel is inside when its centre is. Carve clears the solid bits and keeps the material; fill
// writes the brush material with no second material or blend; paint changes a solid voxel's material only.
import { apply, multiply, type Vec3 } from "../../../sdk/erg";
import type { Hit } from "./pick";
import { carved, filled, index, isSolid, material, overlaps, painted, transformBox, type Box, type GridFrame } from "./voxel";

export type BrushMode = "carve" | "fill" | "paint";
export type BrushShape = "box" | "sphere";
/** "column": fill takes the material of the highest solid voxel in the same column of the frame (or the voxel's own). */
export type BrushMaterial = number | "column";

export interface Brush { mode: BrushMode; shape: BrushShape; size: Vec3; material: BrushMaterial; }

export const MAX_BRUSH = 32;

export interface Anchor { grid: GridFrame; center: Vec3; }

/** Carve and paint centre on the hit voxel; fill on the empty voxel in front of the hit face. */
export function anchorAt(hit: Hit, mode: BrushMode): Anchor {
  const n = mode === "fill" ? hit.normal : [0, 0, 0];
  return { grid: hit.grid, center: [0, 1, 2].map((a) => hit.cell[a] + n[a] + 0.5) as Vec3 };
}

export function brushBox(a: Anchor, b: Brush): Box {
  return { min: a.center.map((c, i) => c - b.size[i] / 2) as Vec3, max: a.center.map((c, i) => c + b.size[i] / 2) as Vec3 };
}

function inside(p: Vec3, a: Anchor, b: Brush): boolean {
  if (b.shape === "box") {
    for (let i = 0; i < 3; i++) if (p[i] < a.center[i] - b.size[i] / 2 || p[i] >= a.center[i] + b.size[i] / 2) return false;
    return true;
  }
  let s = 0;
  for (let i = 0; i < 3; i++) {
    const r = b.size[i] / 2, d = p[i] - a.center[i];
    s += (d * d) / (r * r);
  }
  return s <= 1 + 1e-9;
}

/** The world box the brush covers (.xan units), for the cursor preview. */
export const worldBox = (a: Anchor, b: Brush) => transformBox(a.grid.toWorld, brushBox(a, b));

function columnMaterial(g: GridFrame, words: Uint32Array, x: number, z: number, own: number): number {
  for (let y = g.frame.size[1] - 1; y >= 0; y--) {
    const v = words[index(g.frame, x, y, z)];
    if (isSolid(v)) return material(v);
  }
  return material(own);
}

function edit(b: Brush, g: GridFrame, words: Uint32Array, x: number, z: number, v: number): number {
  switch (b.mode) {
    case "carve": return isSolid(v) ? carved(v) : v;
    case "fill": return isSolid(v) ? v : filled(b.material === "column" ? columnMaterial(g, words, x, z, v) : b.material);
    case "paint": return isSolid(v) && b.material !== "column" ? painted(v, b.material) : v;
  }
}

/** Changed words per blob ref: index -> [before, after], against the current words. Nothing is written. */
export function strokeChanges(grids: GridFrame[], voxels: Map<number, Uint32Array>, a: Anchor, b: Brush): Map<number, Map<number, [number, number]>> {
  const out = new Map<number, Map<number, [number, number]>>();
  const box = brushBox(a, b);
  const world = transformBox(a.grid.toWorld, box);
  for (const g of grids) {
    if (!overlaps(world, g.bounds)) continue;
    const words = voxels.get(g.ref);
    if (!words) continue;
    const same = g === a.grid;
    const toAnchor = same ? null : multiply(a.grid.toLocal, g.toWorld);
    const local = same ? box : transformBox(multiply(g.toLocal, a.grid.toWorld), box);
    const size = g.frame.size;
    const lo = [0, 1, 2].map((i) => Math.max(0, Math.ceil(local.min[i] - 0.5)));
    const hi = [0, 1, 2].map((i) => Math.min(size[i] - 1, Math.floor(local.max[i] - 0.5)));
    let changes: Map<number, [number, number]> | undefined;
    for (let z = lo[2]; z <= hi[2]; z++)
      for (let x = lo[0]; x <= hi[0]; x++)
        for (let y = lo[1]; y <= hi[1]; y++) {
          const c: Vec3 = [x + 0.5, y + 0.5, z + 0.5];
          if (!inside(toAnchor ? apply(toAnchor, c) : c, a, b)) continue;
          const i = index(g.frame, x, y, z), v = words[i], after = edit(b, g, words, x, z, v);
          if (after === v) continue;
          (changes ??= new Map()).set(i, [v, after]);
        }
    if (changes) out.set(g.ref, changes);
  }
  return out;
}
