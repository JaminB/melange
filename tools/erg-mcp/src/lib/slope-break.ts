// Detail layer round 10 (the round-9 judge's tool gap: "a placement filter for contact normal and slope break (cliff foot vs
// face) in the scatter tools"). Pure code, map units, no session: the ground is a function (x, z) -> {y, slope, normal}.
//
// slopeBreaks: the FOOT of each steep face, where talus comes to rest. A grid cell is a foot when its own ground is gentle
// (slope <= footMax) and the ground uphill of it (along the horizontal normal reversed, `probe` away, and twice that) is a
// face (slope >= faceMin) standing at least `rise` above it. Runs of such cells are chained along the contour. Each site
// answers the face's height above it, the downhill direction (where a fan spreads) and the face's mean slope.
//
// talusFan: one fan of rock below a foot site: big blocks at the fan's toe (downhill), small ones at its apex (against the
// face), a skirt of half-buried pieces round the rim; sizes graded by the distance from the face.
//
// contactAngle: the angle between the ground's normal at a point and up (the "contact normal" a prop rests on).
export interface Ground { y: number; slope: number; normal: [number, number, number] }
export type GroundFn = (x: number, z: number) => Ground | null;

export interface BreakOpts {
  box: [number, number, number, number];
  step?: number;        // grid spacing (default 1.5)
  faceMin?: number;     // degrees: the ground uphill must be at least this steep (default 45)
  footMax?: number;     // degrees: the foot itself at most this (default 32)
  probe?: number;       // how far uphill to look (default 2.5)
  rise?: number;        // the face must stand this much above the foot (default 2)
  minY?: number;        // skip ground under this (the sea)
  maxY?: number;
  keep?: (x: number, z: number) => boolean;
  link?: number;        // chain sites within this (default step * 1.6)
}
export interface BreakSite { x: number; z: number; y: number; slope: number; faceSlope: number; faceRise: number; down: [number, number]; run: number }

export function slopeBreaks(g: GroundFn, o: BreakOpts): { sites: BreakSite[]; runs: number; cells: number } {
  const step = o.step ?? 1.5, faceMin = o.faceMin ?? 45, footMax = o.footMax ?? 32, probe = o.probe ?? 2.5, rise = o.rise ?? 2;
  const [x0, z0, x1, z1] = o.box, sites: Omit<BreakSite, "run">[] = [];
  let cells = 0;
  for (let x = x0; x <= x1 + 1e-9; x += step) for (let z = z0; z <= z1 + 1e-9; z += step) {
    cells++;
    if (o.keep && !o.keep(x, z)) continue;
    const here = g(x, z);
    if (!here || here.slope > footMax) continue;
    if (o.minY !== undefined && here.y < o.minY) continue;
    if (o.maxY !== undefined && here.y > o.maxY) continue;
    // uphill: the steepest ascent from here is -normal's horizontal part; a flat foot has none, so look round for the face
    let best: { d: [number, number]; slope: number; rise: number } | null = null;
    const dirs: [number, number][] = [];
    // the uphill direction and 35 degrees either side of it; on flat ground (no uphill) eight directions
    const hl = Math.hypot(here.normal[0], here.normal[2]);
    if (hl > 0.02) {
      const ux = -here.normal[0] / hl, uz = -here.normal[2] / hl, c = Math.cos(0.6), s = Math.sin(0.6);
      dirs.push([ux, uz], [ux * c - uz * s, ux * s + uz * c], [ux * c + uz * s, -ux * s + uz * c]);
    } else for (let k = 0; k < 8; k++) dirs.push([Math.cos(k * Math.PI / 4), Math.sin(k * Math.PI / 4)]);
    for (const d of dirs) {
      const a = g(x + d[0] * probe, z + d[1] * probe), b = g(x + d[0] * probe * 2, z + d[1] * probe * 2);
      if (!a || !b) continue;
      const s = Math.max(a.slope, b.slope), r = b.y - here.y;
      if (s >= faceMin && r >= rise && (!best || r > best.rise)) best = { d, slope: s, rise: r };
    }
    if (!best) continue;
    sites.push({ x, z, y: here.y, slope: here.slope, faceSlope: best.slope, faceRise: best.rise, down: [-best.d[0], -best.d[1]] });
  }
  // chain into runs (connected within link)
  const link = o.link ?? step * 1.6, run = new Array<number>(sites.length).fill(-1);
  let runs = 0;
  for (let i = 0; i < sites.length; i++) {
    if (run[i] >= 0) continue;
    const stack = [i]; run[i] = runs;
    while (stack.length) {
      const k = stack.pop()!;
      for (let j = 0; j < sites.length; j++) if (run[j] < 0 && Math.hypot(sites[j].x - sites[k].x, sites[j].z - sites[k].z) <= link) { run[j] = runs; stack.push(j); }
    }
    runs++;
  }
  return { sites: sites.map((s, i) => ({ ...s, run: run[i] })), runs, cells };
}

export interface FanPiece { x: number; z: number; size: number; kind: "toe" | "body" | "apex" | "skirt"; embed: number }
/** A fan of `n` pieces below a foot site (apex at the site, spreading `length` downhill over `spread` radians). */
export function talusFan(site: { x: number; z: number; down: [number, number] }, o: { n: number; length: number; spread?: number; big: number; small: number; skirt?: number; R: () => number }): FanPiece[] {
  const spread = o.spread ?? 0.9, R = o.R, out: FanPiece[] = [];
  const base = Math.atan2(site.down[1], site.down[0]);
  for (let i = 0; i < o.n; i++) {
    // t: 0 at the apex (the face), 1 at the toe; more pieces toward the toe (the fan widens)
    const t = Math.sqrt((i + 0.5) / o.n) * (0.85 + 0.15 * R());
    const a = base + (R() - 0.5) * spread * (0.4 + 0.6 * t), d = t * o.length;
    const size = (o.small + (o.big - o.small) * t) * (0.8 + 0.4 * R());
    out.push({ x: site.x + Math.cos(a) * d, z: site.z + Math.sin(a) * d, size, kind: t > 0.75 ? "toe" : t < 0.3 ? "apex" : "body", embed: 0.35 + 0.15 * R() });
  }
  // the skirt: small, half-buried pieces round the toe's rim
  for (let i = 0, k = o.skirt ?? Math.ceil(o.n * 0.6); i < k; i++) {
    const a = base + (R() - 0.5) * spread * 1.3, d = o.length * (0.95 + 0.3 * R());
    out.push({ x: site.x + Math.cos(a) * d, z: site.z + Math.sin(a) * d, size: o.small * (0.6 + 0.5 * R()), kind: "skirt", embed: 0.55 + 0.1 * R() });
  }
  return out;
}

/** Degrees between the ground's normal and up. */
export function contactAngle(n: readonly number[]): number {
  const l = Math.hypot(n[0], n[1], n[2]) || 1;
  return Math.acos(Math.max(-1, Math.min(1, n[1] / l))) * 180 / Math.PI;
}
