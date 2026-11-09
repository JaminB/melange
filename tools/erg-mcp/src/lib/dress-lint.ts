// A dressing pass's lint (detail layer round 5): the pieces of a decoration layer grouped into clumps (pieces whose
// boxes touch, within `gap`), then (1) mine lookalikes: a clump smaller than `minSize` across, dark, on walkable dry
// ground (a worm player reads a small dark lump as a mine), (2) contact: a clump whose lowest point is over the ground
// under it (hovering) or whose top is under it (buried), (3) density per band: clumps and covered area per 1000 square
// map units of land in each band. Pure: tools/cluster.ts feeds it the placed meshes and the ground.

export interface Box { min: [number, number, number]; max: [number, number, number] }
export interface Piece { id: number; box: Box; lum: number }
export interface Clump { ids: number[]; box: Box; size: number; lum: number; centre: [number, number] }

/** Clumps: pieces whose boxes come within `gap` of each other (single linkage). */
export function clumps(pieces: Piece[], gap = 0.4): Clump[] {
  const parent = pieces.map((_, i) => i);
  const find = (i: number): number => (parent[i] === i ? i : (parent[i] = find(parent[i])));
  const near = (a: Box, b: Box) => [0, 1, 2].every((k) => a.min[k] - gap <= b.max[k] && b.min[k] - gap <= a.max[k]);
  // a coarse grid keeps it near linear
  const cell = 6, grid = new Map<string, number[]>();
  pieces.forEach((p, i) => {
    for (let x = Math.floor(p.box.min[0] / cell); x <= Math.floor(p.box.max[0] / cell); x++)
      for (let z = Math.floor(p.box.min[2] / cell); z <= Math.floor(p.box.max[2] / cell); z++) {
        const k = `${x},${z}`;
        for (const j of grid.get(k) ?? []) if (near(p.box, pieces[j].box)) parent[find(i)] = find(j);
        (grid.get(k) ?? grid.set(k, []).get(k)!).push(i);
      }
  });
  const groups = new Map<number, number[]>();
  pieces.forEach((_, i) => (groups.get(find(i)) ?? groups.set(find(i), []).get(find(i))!).push(i));
  return [...groups.values()].map((g) => {
    const box: Box = { min: [Infinity, Infinity, Infinity], max: [-Infinity, -Infinity, -Infinity] };
    let lw = 0, w = 0;
    for (const i of g) {
      const b = pieces[i].box, v = (b.max[0] - b.min[0]) * (b.max[1] - b.min[1]) * (b.max[2] - b.min[2]) + 1e-6;
      for (let k = 0; k < 3; k++) { box.min[k] = Math.min(box.min[k], b.min[k]); box.max[k] = Math.max(box.max[k], b.max[k]); }
      lw += pieces[i].lum * v; w += v;
    }
    const size = Math.max(box.max[0] - box.min[0], box.max[1] - box.min[1], box.max[2] - box.min[2]);
    return { ids: g.map((i) => pieces[i].id), box, size, lum: lw / w, centre: [(box.min[0] + box.max[0]) / 2, (box.min[2] + box.max[2]) / 2] as [number, number] };
  });
}

export interface GroundAt { y: number; slope: number }
export interface LintOpts { minSize?: number; darkLum?: number; maxSlope?: number; water?: number; hover?: number }

/** A clump of under minSize across, darker than darkLum (0-1), on ground under maxSlope and above the water. */
export function mineSuspects(cs: Clump[], ground: (x: number, z: number) => GroundAt | null, o: LintOpts = {}) {
  const minSize = o.minSize ?? 1.5, dark = o.darkLum ?? 0.12, maxSlope = o.maxSlope ?? 40, water = o.water ?? -Infinity;
  return cs.filter((c) => {
    if (c.size >= minSize || c.lum >= dark) return false;
    const g = ground(c.centre[0], c.centre[1]);
    return !!g && g.slope <= maxSlope && g.y > water;
  });
}

/** Contact: the gap between the clump's lowest point and the highest ground under its footprint (centre and the box's
 * corners pulled in by a quarter): over `hover` it floats; its top under the lowest ground means it is buried. */
export function contact(c: Clump, ground: (x: number, z: number) => GroundAt | null, hover = 0.15) {
  const [x0, x1, z0, z1] = [c.box.min[0], c.box.max[0], c.box.min[2], c.box.max[2]];
  const qx = (x1 - x0) / 4, qz = (z1 - z0) / 4;
  const ys = [[c.centre[0], c.centre[1]], [x0 + qx, z0 + qz], [x1 - qx, z0 + qz], [x0 + qx, z1 - qz], [x1 - qx, z1 - qz]]
    .map(([x, z]) => ground(x, z)?.y).filter((y): y is number => y !== undefined);
  if (!ys.length) return { state: "no ground" as const, gap: 0 };
  const lo = Math.min(...ys), hi = Math.max(...ys), centreY = ys[0];
  const gap = c.box.min[1] - centreY;
  if (gap > hover) return { state: "hovering" as const, gap };
  if (c.box.max[1] < lo) return { state: "buried" as const, gap };
  return { state: "seated" as const, gap, showing: c.box.max[1] - hi };
}
