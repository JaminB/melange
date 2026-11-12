// A boulder field's plan and the lint for scatter that makes stairs. Pure: the ground comes in as a function.
//
// Hand-scattered voxel boulders (overlapping domes of several voxel layers) stack into terraces: three or four discs one
// on another read as a staircase, and beside a wall they are one (Dustcrown round 4: piles by the plinth's riser). The
// plan here keeps each boulder to `maxTiers` voxel layers over the ground, keeps boulders apart so no two stack, and
// keeps the whole field a distance from risers. The lint reads what was built: per scatter block its tiers, the nearest
// riser, and whether its top leaves that riser a climb a jump clears.
export interface Boulder { x: number; z: number; ground: number; radius: number; height: number; tiers: number; material: number; }
export interface FieldSpec {
  at: [number, number]; radius: number; count: number; seed: number;
  size: [number, number];            // a boulder's radius, least and most (map units)
  height: [number, number];          // its height over the ground, least and most
  maxTiers: number;                  // voxel layers over the ground a boulder may show
  voxel: number;                     // map units per voxel
  jitter: number;                    // 0 an even ring pattern, 1 anywhere in the disc
  gap: number;                       // the least clear ground between two boulders (under 0 lets them overlap)
  keepOut: number;                   // no boulder's edge within this of a riser
  riserMin: number;                  // a rise of this much between ground samples one unit apart is a riser
  materials: number[];
  avoid: { x: number; z: number; radius: number }[];
}
export type Ground = (x: number, z: number) => number | null;

export function rngOf(seed: number): () => number {
  let s = (Math.abs(Math.floor(seed)) % 2147483646) + 1;
  return () => { s = (s * 16807) % 2147483647; return s / 2147483647; };
}

/** The distance from (x, z) to the nearest riser within `reach`: two ground samples a unit apart (or ground beside none)
 * that differ by riserMin or more. Infinity when there is none. */
export function riserNear(ground: Ground, x: number, z: number, reach: number, riserMin: number, step = 1): { distance: number; top: number; at: [number, number] } | null {
  const n = Math.ceil(reach / step);
  let best: { distance: number; top: number; at: [number, number] } | null = null;
  const cache = new Map<number, number | null>();
  const g = (i: number, j: number) => {
    const key = (i + 512) * 2048 + (j + 512);
    let v = cache.get(key);
    if (v === undefined) { v = ground(x + i * step, z + j * step); cache.set(key, v); }
    return v;
  };
  for (let j = -n; j <= n; j++) for (let i = -n; i <= n; i++) {
    const a = g(i, j);
    if (a === null) continue;
    for (const [di, dj] of [[1, 0], [0, 1]] as const) {
      const b = g(i + di, j + dj);
      if (b === null || Math.abs(a - b) < riserMin) continue;
      // the riser stands between the two samples
      const rx = (i + di / 2) * step, rz = (j + dj / 2) * step, d = Math.hypot(rx, rz);
      if (d <= reach && (!best || d < best.distance)) best = { distance: d, top: Math.max(a, b), at: [x + rx, z + rz] };
    }
  }
  return best;
}

/** The field's boulders: seeded candidates in the disc, each kept when it has ground, clears the boulders before it by
 * `gap`, lies outside every avoid circle and has no riser within keepOut of its edge. */
export function planField(f: FieldSpec, ground: Ground): { boulders: Boulder[]; rejected: { noGround: number; crowded: number; avoid: number; riser: number }; tries: number } {
  const R = rngOf(f.seed), out: Boulder[] = [], rejected = { noGround: 0, crowded: 0, avoid: 0, riser: 0 };
  const golden = Math.PI * (3 - Math.sqrt(5));
  let tries = 0;
  for (; tries < f.count * 40 && out.length < f.count; tries++) {
    // an even sunflower spot, moved toward a free one by jitter
    const i = tries % (f.count * 2), ra = f.radius * Math.sqrt((i + 0.5) / (f.count * 2)), th = i * golden;
    const fr = f.radius * Math.sqrt(R()), ft = R() * Math.PI * 2;
    const x = f.at[0] + (1 - f.jitter) * ra * Math.cos(th) + f.jitter * fr * Math.cos(ft), z = f.at[1] + (1 - f.jitter) * ra * Math.sin(th) + f.jitter * fr * Math.sin(ft);
    const radius = f.size[0] + R() * (f.size[1] - f.size[0]);
    const want = f.height[0] + R() * (f.height[1] - f.height[0]);
    const mat = f.materials[Math.floor(R() * f.materials.length) % f.materials.length];
    const g = ground(x, z);
    if (g === null) { rejected.noGround++; continue; }
    if (f.avoid.some((c) => Math.hypot(x - c.x, z - c.z) < c.radius + radius)) { rejected.avoid++; continue; }
    if (out.some((b) => Math.hypot(x - b.x, z - b.z) < radius + b.radius + f.gap)) { rejected.crowded++; continue; }
    if (f.keepOut > 0 && riserNear(ground, x, z, f.keepOut + radius, f.riserMin)) { rejected.riser++; continue; }
    // whole voxel layers over the ground, at most maxTiers
    const tiers = Math.max(1, Math.min(f.maxTiers, Math.round(want / f.voxel)));
    out.push({ x, z, ground: g, radius, height: tiers * f.voxel, tiers, material: mat });
  }
  return { boulders: out, rejected, tries };
}

export interface ScatterBlock {
  name: string; id: number;
  /** Each column of the block that stands over other ground: its top, and the top of the ground under it (not the block's). */
  columns: { x: number; z: number; top: number; ground: number | null; i: number; j: number }[];
  voxel: number;
}
export interface StairFind { id: number; name: string; at: [number, number]; tiers: number; lift: number; top: number;
  riser: { distance: number; top: number; at: [number, number]; left: number } | null; stack: boolean; foothold: boolean; }

/** Per scatter block: tiers (the most levels a climb over its columns steps up: the longest run of side-by-side columns
 * each higher than the last, from one that stands over the ground; a flat-topped boulder on a slope is one), its greatest lift, the
 * nearest riser of the ground within `near` of any of its columns and what the block leaves of it (riser top less the
 * block's top). stack: more tiers than maxTiers. foothold: a riser within `near` that the block's top brings within
 * `jump` while the ground under the block does not. */
export function lintStairs(blocks: ScatterBlock[], ground: Ground, o: { near: number; riserMin: number; jump: number; maxTiers: number }): StairFind[] {
  return blocks.map((b) => {
    let lift = 0, top = -Infinity, sx = 0, sz = 0, low = Infinity, tiers = 0;
    // lowest first, so a column's tier is one more than the highest tier among its lower neighbours
    const order = b.columns.map((_, i) => i).sort((p, q) => b.columns[p].top - b.columns[q].top);
    const tierAt = new Map<number, { t: number; top: number }>(), key = (i: number, j: number) => (i + 4096) * 16384 + (j + 4096), eps = 0.25 * b.voxel;
    for (const ci of order) {
      const c = b.columns[ci];
      sx += c.x; sz += c.z; top = Math.max(top, c.top);
      if (c.ground !== null) { low = Math.min(low, c.ground); lift = Math.max(lift, c.top - c.ground); }
      let t = c.ground === null || c.top - c.ground > eps ? 1 : 0;
      for (const [di, dj] of [[1, 0], [-1, 0], [0, 1], [0, -1]] as const) {
        const nb = tierAt.get(key(c.i + di, c.j + dj));
        if (nb && nb.t > 0 && nb.top < c.top - eps) t = Math.max(t, nb.t + 1);
      }
      tierAt.set(key(c.i, c.j), { t, top: c.top });
      tiers = Math.max(tiers, t);
    }
    const n = Math.max(1, b.columns.length), at: [number, number] = [sx / n, sz / n];
    let riser: StairFind["riser"] = null;
    // the nearest riser to any column (every fourth is enough for a block of small voxels)
    const stride = Math.max(1, Math.floor(b.columns.length / 64));
    for (let i = 0; i < b.columns.length; i += stride) {
      const c = b.columns[i], r = riserNear(ground, c.x, c.z, o.near, o.riserMin);
      if (r && (!riser || r.distance < riser.distance)) riser = { ...r, left: r.top - top };
    }
    const foothold = !!riser && riser.left > 0 && riser.left <= o.jump && riser.top - low > o.jump;
    return { id: b.id, name: b.name, at, tiers, lift, top, riser, stack: tiers > o.maxTiers, foothold };
  });
}
