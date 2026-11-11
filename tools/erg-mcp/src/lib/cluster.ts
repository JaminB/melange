// Cluster planning for a dressing pass (detail layer round 5): a level artist lays rock as clusters with a size gradient,
// not sprinkles. Given anchors (an outcrop, a crag) and a ground sampler, plan the children each one sheds: "outcrop" a
// group of 2-4 smaller blocks on the anchor's downhill side (sizes falling off with distance), "scree" a fan of 4-8 flat
// slabs further down the slope (big at the toe), "skirt" debris round the anchor's foot at its contact line, so it reads
// seated, not dropped in. Pure: tools/cluster.ts feeds it the voxels' ground (surface_sample's maths).

export type ClusterMode = "outcrop" | "scree" | "skirt";
export interface Ground { y: number; normal: [number, number, number]; slope: number }
export interface Anchor { xz: [number, number]; size: number; radius?: number }
export interface Child { xz: [number, number]; height: number; width: number; archetype: "single" | "tor"; parent: number; ring: number; align: number; sink: number }
export interface ClusterOpts {
  mode: ClusterMode;
  count?: [number, number];        // children per anchor (min, max)
  sizes?: [number, number];        // child size as a fraction of the anchor's (nearest, furthest), or for scree absolute [min, max]
  spread?: number;                 // reach beyond the anchor's radius, in anchor radii (outcrop 1.6, scree 3, skirt 0.35)
  cone?: number;                   // half angle (degrees) round downhill the children fall in (outcrop 70, scree 35, skirt 180)
  flat?: number;                   // width over height of a child (outcrop 1.2, scree 2.4, skirt 1.4)
  minSize?: number;                // smallest child (map units)
  seed?: number;
}

export function rng(seed: number): () => number {
  let s = seed >>> 0;
  return () => ((s = (s * 1664525 + 1013904223) >>> 0) / 4294967296);
}

const DEFAULTS: Record<ClusterMode, Required<Omit<ClusterOpts, "mode" | "seed">>> = {
  outcrop: { count: [2, 4], sizes: [0.55, 0.3], spread: 1.6, cone: 70, flat: 1.2, minSize: 0.8 },
  scree: { count: [4, 8], sizes: [0.8, 1.6], spread: 3, cone: 30, flat: 2.4, minSize: 0.8 },
  skirt: { count: [4, 7], sizes: [0.22, 0.12], spread: 0.35, cone: 180, flat: 1.4, minSize: 0.6 },
};

/** The downhill direction (unit [x, z]) of a ground normal; [0, 0] on flat ground. */
export function downhill(n: [number, number, number]): [number, number] {
  const l = Math.hypot(n[0], n[2]);
  return l < 1e-4 ? [0, 0] : [n[0] / l, n[2] / l];
}

/** Plan the children of each anchor. `ground(x, z)` answers the surface or null (nothing below). Children whose ground
 * is missing are dropped; each child is placed outside the anchor's radius (outcrop, scree) or on it (skirt). */
export function planClusters(anchors: Anchor[], ground: (x: number, z: number) => Ground | null, o: ClusterOpts): Child[] {
  const d = { ...DEFAULTS[o.mode], ...Object.fromEntries(Object.entries(o).filter(([, v]) => v !== undefined)) } as Required<ClusterOpts>;
  const R = rng(o.seed ?? 1);
  const out: Child[] = [];
  anchors.forEach((a, ai) => {
    const g0 = ground(a.xz[0], a.xz[1]);
    if (!g0) return;
    let [dx, dz] = downhill(g0.normal);
    if (dx === 0 && dz === 0) { const t = R() * Math.PI * 2; dx = Math.cos(t); dz = Math.sin(t); }
    const base = Math.atan2(dz, dx), rad = a.radius ?? a.size * 0.6;
    const n = d.count[0] + Math.floor(R() * (d.count[1] - d.count[0] + 1));
    for (let i = 0; i < n; i++) {
      const t = n > 1 ? i / (n - 1) : 0;                         // 0 nearest .. 1 furthest
      const cone = (d.cone * Math.PI) / 180;
      const ang = o.mode === "skirt" ? base + (i / n) * Math.PI * 2 + (R() - 0.5) * 0.6 : base + (R() * 2 - 1) * cone * (0.4 + 0.6 * t);
      let dist: number, size: number;
      if (o.mode === "scree") {
        // the fan starts past the anchor's foot and runs down the slope; the toe's slabs are the biggest
        dist = rad * (1.1 + d.spread * (0.15 + 0.85 * t)) + R() * 0.5;
        size = d.sizes[0] + (d.sizes[1] - d.sizes[0]) * Math.min(1, t * 0.8 + R() * 0.35);
      } else if (o.mode === "skirt") {
        // on the contact line, a little more downhill (debris rolls)
        const down = Math.max(0, Math.cos(ang - base));
        dist = rad * (0.85 + d.spread * (R() + 0.6 * down));
        size = a.size * (d.sizes[0] + (d.sizes[1] - d.sizes[0]) * R());
      } else {
        dist = rad * (1.0 + (d.spread - 1) * t) + R() * rad * 0.3;
        size = a.size * (d.sizes[0] + (d.sizes[1] - d.sizes[0]) * t) * (0.85 + R() * 0.3);
      }
      size = Math.max(d.minSize, size);
      const x = a.xz[0] + Math.cos(ang) * dist, z = a.xz[1] + Math.sin(ang) * dist;
      if (!ground(x, z)) continue;
      out.push({ xz: [x, z], height: size, width: size * d.flat * (0.85 + R() * 0.3), archetype: o.mode === "outcrop" && R() < 0.35 ? "tor" : "single", parent: ai, ring: t,
        align: o.mode === "outcrop" ? 0.7 : 0.95, sink: o.mode === "scree" ? 0.12 : o.mode === "skirt" ? 0.3 : 0.25 });
    }
  });
  return out;
}
