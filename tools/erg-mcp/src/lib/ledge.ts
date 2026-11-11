// Surface-aligned rock for a dressing pass (detail layer round 6; tools/ledge.ts): a level artist builds strata out of
// flattened slabs laid along a face's contour, half sunk into it, at a few repeated heights, and crowns ridges and cliff
// tops with blocks along the crest. Pure: the tool feeds it the voxels' ground.
//   surfaceFrame   the rotation whose x runs along the contour, z points out of the face (pitched toward the normal) and y
//                  is the remaining up-ish axis
//   alignedPose    a scenery copy's pose for a slab of [length, thickness, depth] at a surface point, sunk `embed` of its
//                  depth into the face along its outward axis
//   planBands      the steep faces of a region (connected cells over minSlope), 2-3 band heights per face (or absolute
//                  heights / a spacing shared by every face), the contour polylines at those heights, and the slabs laid
//                  along them
//   findCrests     ridge-crest and cliff-top polylines with each point's outward direction
//   perchedFromView  a clump's underside showing from given cameras: its low vertices over the ground beneath them on the
//                  camera-facing side
import { apply, eulerOf, mul, rng, rotMat, type Box, type M3, type V3 } from "./scatter";
export type { V3, M3, Box };

export interface Ground { y: number; normal: V3; slope: number }
export type GroundFn = (x: number, z: number) => Ground | null;
export type Region = { box: [number, number, number, number] } | { centre: [number, number]; radius: number };

const norm = (v: V3): V3 => { const l = Math.hypot(v[0], v[1], v[2]) || 1; return [v[0] / l, v[1] / l, v[2] / l]; };
const cross = (a: V3, b: V3): V3 => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const dot = (a: V3, b: V3) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];

/** The face frame (columns x, y, z) at a ground normal: z the face's outward direction, the horizontal downhill pitched
 * toward the true normal by `pitch` (0 a level shelf whose back is a vertical wall in the face, 1 a plate lying on the
 * face); x along the contour (horizontal; `tangent` [tx, tz] picks the direction along a polyline); y = z cross x. On
 * flat ground the outward direction is `fallback` (default +x). */
export function surfaceFrame(normal: V3, pitch = 0.25, tangent?: [number, number], fallback: [number, number] = [1, 0]): M3 {
  const n = norm(normal), hl = Math.hypot(n[0], n[2]);
  const hout: V3 = hl > 1e-3 ? [n[0] / hl, 0, n[2] / hl] : norm([fallback[0], 0, fallback[1]]);
  const out = norm([hout[0] + (n[0] - hout[0]) * pitch, hout[1] + (n[1] - hout[1]) * pitch, hout[2] + (n[2] - hout[2]) * pitch]);
  let x: V3 = norm([out[2], 0, -out[0]]);                                // up x out: horizontal, along the contour
  if (tangent) {
    const t: V3 = [tangent[0], 0, tangent[1]], tp: V3 = [t[0] - out[0] * dot(t, out), t[1] - out[1] * dot(t, out), t[2] - out[2] * dot(t, out)];
    if (Math.hypot(...tp) > 1e-3) x = norm(tp);
  }
  const y = cross(out, x);
  return [[x[0], y[0], out[0]], [x[1], y[1], out[1]], [x[2], y[2], out[2]]];
}

export interface AlignedPose { pos: V3; rot: V3; scale: V3; centre: V3; out: V3; front: number; top: number; bottom: number }

/** The pose of one copy of `mesh` drawn as a slab `size` [length along the contour, thickness, depth out of the face]
 * (map units) at the surface point `at`, in the face frame `frame` (surfaceFrame), with `embed` of its depth behind the
 * surface along the outward axis and `lift` up the frame's y. `jitter` [about x, y, z] degrees turns it in its own frame.
 * pos is the detail's origin (the mesh's box centre offset), rot degrees in the engine's order, scale per axis. Also its
 * box centre, outward axis, how far it stands out of the surface (`front`) and its top and bottom y. */
export function alignedPose(mesh: Box, at: V3, frame: M3, size: V3, embed = 0.5, lift = 0, jitter: V3 = [0, 0, 0]): AlignedPose {
  const ms: V3 = [0, 1, 2].map((i) => Math.max(1e-6, mesh.max[i] - mesh.min[i])) as V3;
  const mid: V3 = [0, 1, 2].map((i) => (mesh.min[i] + mesh.max[i]) / 2) as V3;
  const r = jitter.some((v) => v !== 0) ? mul(frame, rotMat(jitter)) : frame;
  const s: V3 = [size[0] / ms[0], size[1] / ms[1], size[2] / ms[2]];
  const out: V3 = [frame[0][2], frame[1][2], frame[2][2]], up: V3 = [frame[0][1], frame[1][1], frame[2][1]];
  const k = size[2] * (0.5 - embed);
  const centre: V3 = [at[0] + out[0] * k + up[0] * lift, at[1] + out[1] * k + up[1] * lift, at[2] + out[2] * k + up[2] * lift];
  const off = apply(r, [mid[0] * s[0], mid[1] * s[1], mid[2] * s[2]]);
  let top = -Infinity, bottom = Infinity;
  for (let c = 0; c < 8; c++) {
    const p = apply(r, [((c & 1) ? 0.5 : -0.5) * size[0], ((c & 2) ? 0.5 : -0.5) * size[1], ((c & 4) ? 0.5 : -0.5) * size[2]]);
    top = Math.max(top, centre[1] + p[1]); bottom = Math.min(bottom, centre[1] + p[1]);
  }
  return { pos: [centre[0] - off[0], centre[1] - off[1], centre[2] - off[2]], rot: eulerOf(r), scale: s, centre, out, front: size[2] * (1 - embed), top, bottom };
}

// ---------------------------------------------------------------- the region's grid

interface Cell { i: number; j: number; x: number; z: number; g: Ground }

function gridOf(ground: GroundFn, region: Region, step: number, keep: (x: number, z: number) => boolean): Map<string, Cell> {
  const [x0, z0, x1, z1] = "box" in region ? region.box
    : [region.centre[0] - region.radius, region.centre[1] - region.radius, region.centre[0] + region.radius, region.centre[1] + region.radius];
  const cells = new Map<string, Cell>();
  for (let i = 0, x = x0; x <= x1 + 1e-9; i++, x = x0 + i * step) for (let j = 0, z = z0; z <= z1 + 1e-9; j++, z = z0 + j * step) {
    if ("centre" in region && Math.hypot(x - region.centre[0], z - region.centre[1]) > region.radius) continue;
    if (!keep(x, z)) continue;
    const g = ground(x, z);
    if (g) cells.set(`${i},${j}`, { i, j, x, z, g });
  }
  return cells;
}

/** Order points into polylines: greedy nearest neighbours within `link`, grown from both ends. */
export function chain<T extends { x: number; z: number }>(pts: T[], link: number): T[][] {
  const left = new Set(pts.map((_, i) => i)), lines: T[][] = [];
  const nearest = (p: T) => {
    let bi = -1, bd = link;
    for (const i of left) { const d = Math.hypot(pts[i].x - p.x, pts[i].z - p.z); if (d <= bd) { bd = d; bi = i; } }
    return bi;
  };
  while (left.size) {
    const first = left.values().next().value as number;
    left.delete(first);
    const line = [pts[first]];
    for (let end = 0; end < 2; end++) {
      for (;;) {
        const tip = end ? line[line.length - 1] : line[0], n = nearest(tip);
        if (n < 0) break;
        left.delete(n);
        if (end) line.push(pts[n]); else line.unshift(pts[n]);
      }
    }
    lines.push(line);
  }
  return lines;
}

const lengthOf = (l: { x: number; z: number }[]) => l.reduce((s, p, i) => (i ? s + Math.hypot(p.x - l[i - 1].x, p.z - l[i - 1].z) : 0), 0);

// ---------------------------------------------------------------- strata bands

export interface BandOpts {
  step?: number;                 // grid spacing (map), default 1.5
  minSlope?: number;             // a face's cells are at least this steep (default 52)
  minCells?: number;             // a face has at least this many cells (default 12)
  count?: number;                // bands per face when no heights or spacing (default 3, fewer on a short face)
  heights?: number[];            // absolute band heights, the same on every face
  spacing?: number;              // or bands every `spacing` from `offset`, the same on every face (strata are level)
  offset?: number;
  minGap?: number;               // least height between two bands of a face (default 3)
  length?: [number, number];     // slab length range (default [2.6, 4.2])
  gap?: [number, number];        // gap between slabs along a band (default [0.2, 0.9])
  breaks?: number;               // chance a slab is left out (a broken band, default 0.15)
  minRun?: number;               // least length of a contour polyline to dress (default 4)
  seed?: number;
  keep?: (x: number, z: number) => boolean;
}

export interface Slab { at: V3; normal: V3; tangent: [number, number]; length: number; face: number; band: number; height: number }
export interface BandPlan {
  faces: { face: number; cells: number; yMin: number; yMax: number; heights: number[] }[];
  bands: { face: number; band: number; height: number; points: [number, number, number][] }[];
  slabs: Slab[];
}

/** The strata of a region's steep faces: faces are connected cells (8-neighbour) over minSlope; each gets its band heights
 * (absolute, a shared spacing, or `count` spread over its 10-90% height range); a band's contour points are the face's
 * cells moved down their fall line onto the band height; they are chained into polylines and slabs laid along each (a
 * length, then a gap; `breaks` of them left out), each at the ground there, its tangent along the polyline. */
export function planBands(ground: GroundFn, region: Region, o: BandOpts = {}): BandPlan {
  const step = o.step ?? 1.5, minSlope = o.minSlope ?? 52, R = rng(o.seed ?? 7);
  const cells = gridOf(ground, region, step, o.keep ?? (() => true));
  const steep = [...cells.values()].filter((c) => c.g.slope >= minSlope);
  const isSteep = new Set(steep.map((c) => `${c.i},${c.j}`));
  // faces: connected components
  const faceOf = new Map<string, number>(), faces: Cell[][] = [];
  for (const c of steep) {
    const k0 = `${c.i},${c.j}`;
    if (faceOf.has(k0)) continue;
    const id = faces.length, list: Cell[] = [], stack = [c];
    faceOf.set(k0, id);
    while (stack.length) {
      const q = stack.pop()!;
      list.push(q);
      for (let di = -1; di <= 1; di++) for (let dj = -1; dj <= 1; dj++) {
        const k = `${q.i + di},${q.j + dj}`;
        if ((di || dj) && isSteep.has(k) && !faceOf.has(k)) { faceOf.set(k, id); stack.push(cells.get(k)!); }
      }
    }
    faces.push(list);
  }
  const plan: BandPlan = { faces: [], bands: [], slabs: [] };
  const [lmin, lmax] = o.length ?? [2.6, 4.2], [gmin, gmax] = o.gap ?? [0.2, 0.9], minGap = o.minGap ?? 3;
  faces.forEach((list, fi) => {
    if (list.length < (o.minCells ?? 12)) return;
    const ys = list.map((c) => c.g.y).sort((a, b) => a - b), q = (f: number) => ys[Math.floor(f * (ys.length - 1))];
    const lo = q(0.1), hi = q(0.9);
    let hs: number[];
    if (o.heights) hs = o.heights.filter((h) => h >= lo && h <= hi);
    else if (o.spacing) { hs = []; const off = o.offset ?? 0; for (let h = off + Math.ceil((lo - off) / o.spacing) * o.spacing; h <= hi; h += o.spacing) hs.push(h); }
    else {
      const n = Math.max(1, Math.min(o.count ?? 3, Math.floor((hi - lo) / minGap) + 1));
      hs = Array.from({ length: n }, (_, k) => lo + ((k + 0.5) / n) * (hi - lo));
    }
    hs = hs.filter((h, k) => !k || h - hs[k - 1] >= minGap - 1e-9);
    plan.faces.push({ face: fi, cells: list.length, yMin: ys[0], yMax: ys[ys.length - 1], heights: hs });
    hs.forEach((h, bi) => {
      // the cells whose fall line crosses h within a cell: moved down (or up) the fall line onto h
      const pts: { x: number; z: number; y: number }[] = [];
      for (const c of list) {
        const tan = Math.tan((Math.min(c.g.slope, 85) * Math.PI) / 180), reach = step * tan * 0.75;
        if (Math.abs(c.g.y - h) > reach) continue;
        const hl = Math.hypot(c.g.normal[0], c.g.normal[2]) || 1, dx = c.g.normal[0] / hl, dz = c.g.normal[2] / hl;
        const d = (c.g.y - h) / tan;                                      // downhill lowers the ground
        const x = c.x + dx * d, z = c.z + dz * d, g = ground(x, z);
        if (!g || Math.abs(g.y - h) > Math.max(0.6, reach * 0.5) || g.slope < minSlope * 0.8 || !(o.keep ?? (() => true))(x, z)) continue;
        pts.push({ x, z, y: g.y });
      }
      // thin to about one point per step/1.5 so the chain follows the contour
      const thin: typeof pts = [];
      for (const p of pts) if (!thin.some((t) => Math.hypot(t.x - p.x, t.z - p.z) < step * 0.6)) thin.push(p);
      for (const line of chain(thin, step * 2.2)) {
        if (lengthOf(line) < (o.minRun ?? 4)) continue;
        plan.bands.push({ face: fi, band: bi, height: h, points: line.map((p) => [p.x, p.y, p.z]) });
        // walk the polyline: a slab, a gap, ...
        const cum = [0];
        for (let i = 1; i < line.length; i++) cum.push(cum[i - 1] + Math.hypot(line[i].x - line[i - 1].x, line[i].z - line[i - 1].z));
        const total = cum[cum.length - 1], at = (s: number) => {
          let i = 1;
          while (i < line.length - 1 && cum[i] < s) i++;
          const t = (s - cum[i - 1]) / Math.max(1e-9, cum[i] - cum[i - 1]);
          return { x: line[i - 1].x + (line[i].x - line[i - 1].x) * t, z: line[i - 1].z + (line[i].z - line[i - 1].z) * t };
        };
        for (let s = R() * gmax; s < total - lmin * 0.6;) {
          const L = Math.min(lmin + R() * (lmax - lmin), total - s);
          const a = at(s), b = at(s + L), m = at(s + L / 2), g = ground(m.x, m.z);
          if (g && R() >= (o.breaks ?? 0.15)) {
            const tl = Math.hypot(b.x - a.x, b.z - a.z) || 1;
            plan.slabs.push({ at: [m.x, g.y, m.z], normal: g.normal, tangent: [(b.x - a.x) / tl, (b.z - a.z) / tl], length: Math.max(L, tl), face: fi, band: bi, height: h });
          }
          s += L + gmin + R() * (gmax - gmin);
        }
      }
    });
  });
  return plan;
}

// ---------------------------------------------------------------- crests and cliff tops

export interface CrestOpts {
  step?: number;                 // grid spacing (default 1.5)
  reach?: number;                // how far either side a crest is compared (default 2 steps)
  rise?: number;                 // a crest stands this much over both sides at `reach` (default 0.35 reach)
  cliffSlope?: number;           // a cliff top drops at least this steeply within `reach` (default 55)
  topSlope?: number;             // and its own ground is under this (default 40)
  kinds?: ("crest" | "cliffTop")[];
  minRun?: number;               // least polyline length (default 5)
  keep?: (x: number, z: number) => boolean;
}
export interface CrestLine { kind: "crest" | "cliffTop"; length: number; points: { x: number; y: number; z: number; out: [number, number]; drop: number }[] }

/** Ridge crests (higher than both sides across, by `rise` at `reach`, in one of four directions) and cliff tops (gentle
 * ground with a drop steeper than cliffSlope within `reach` in one of eight directions), chained into polylines. Each point
 * carries its outward direction (the side it falls away to: for a crest the steeper side) and the drop there. */
export function findCrests(ground: GroundFn, region: Region, o: CrestOpts = {}, height?: (x: number, z: number) => number | null): CrestLine[] {
  const step = o.step ?? 1.5, reach = o.reach ?? step * 2, rise = o.rise ?? reach * 0.35;
  const cliffTan = Math.tan(((o.cliffSlope ?? 55) * Math.PI) / 180), topSlope = o.topSlope ?? 40, kinds = o.kinds ?? ["crest", "cliffTop"];
  const cells = gridOf(ground, region, step, o.keep ?? (() => true));
  const found: Record<"crest" | "cliffTop", { x: number; z: number; y: number; out: [number, number]; drop: number }[]> = { crest: [], cliffTop: [] };
  // the comparisons need heights only: `height` (one drop per point) when the caller has it, else the ground's y
  const yAt = height ? (x: number, z: number) => height(x, z) ?? -Infinity : (x: number, z: number) => ground(x, z)?.y ?? -Infinity;
  for (const c of cells.values()) {
    const y = c.g.y;
    if (kinds.includes("crest")) {
      let best: { out: [number, number]; drop: number } | null = null;
      for (const [dx, dz] of [[1, 0], [0, 1], [Math.SQRT1_2, Math.SQRT1_2], [Math.SQRT1_2, -Math.SQRT1_2]]) {
        const a = yAt(c.x + dx * reach, c.z + dz * reach), b = yAt(c.x - dx * reach, c.z - dz * reach);
        if (y - a >= rise && y - b >= rise) {
          const drop = Math.min(y - a, y - b);
          if (!best || drop > best.drop) best = { out: a < b ? [dx, dz] : [-dx, -dz], drop };
        }
      }
      if (best) found.crest.push({ x: c.x, z: c.z, y, ...best });
    }
    if (kinds.includes("cliffTop") && c.g.slope < topSlope) {
      let best: { out: [number, number]; drop: number } | null = null;
      for (let k = 0; k < 8; k++) {
        const a = (k / 8) * Math.PI * 2, dx = Math.cos(a), dz = Math.sin(a), d = y - yAt(c.x + dx * reach, c.z + dz * reach);
        if (Number.isFinite(d) && d >= reach * cliffTan && (!best || d > best.drop)) best = { out: [dx, dz], drop: d };
      }
      if (best) found.cliffTop.push({ x: c.x, z: c.z, y, ...best });
    }
  }
  const lines: CrestLine[] = [];
  for (const kind of kinds) for (const line of chain(found[kind], step * 1.6)) {
    const length = lengthOf(line);
    if (length >= (o.minRun ?? 5)) lines.push({ kind, length, points: line });
  }
  return lines.sort((a, b) => b.length - a.length);
}

// ---------------------------------------------------------------- perched from the camera's view

export interface ViewCam { name: string; pos: V3 }
export interface PerchResult { gap: number; share: number; cams: string[] }

/** Whether a clump's underside shows from the cameras: of its vertices in the lowest `low` (default 0.35) of its height, the
 * ones standing more than `hover` over the ground beneath them (or with nothing beneath) and facing a camera (on the
 * camera's side of the clump's centre, horizontally, or under a camera that looks up at them). Answers the largest such gap,
 * the share of low vertices exposed, and the cameras that see them. */
export function perchedFromView(verts: V3[], ground: (x: number, z: number) => number | null, cams: ViewCam[], hover = 0.3, low = 0.35): PerchResult {
  if (!verts.length) return { gap: 0, share: 0, cams: [] };
  let y0 = Infinity, y1 = -Infinity, cx = 0, cz = 0;
  for (const v of verts) { y0 = Math.min(y0, v[1]); y1 = Math.max(y1, v[1]); cx += v[0]; cz += v[2]; }
  cx /= verts.length; cz /= verts.length;
  const cut = y0 + (y1 - y0) * low, lows = verts.filter((v) => v[1] <= cut);
  const seen = new Set<string>();
  let gap = 0, exposed = 0;
  for (const v of lows) {
    const gy = ground(v[0], v[2]), d = gy === null ? Infinity : v[1] - gy;
    if (d <= hover) continue;
    let any = false;
    for (const c of cams) {
      const fx = c.pos[0] - cx, fz = c.pos[2] - cz;
      if ((v[0] - cx) * fx + (v[2] - cz) * fz > 0 || c.pos[1] < v[1]) { seen.add(c.name); any = true; }
    }
    if (any) { exposed++; gap = Math.max(gap, d); }
  }
  return { gap, share: lows.length ? exposed / lows.length : 0, cams: [...seen] };
}
