// build_mesa's surface as a heightfield for heightfield_region: a plateau from an outline (circle, ellipse or polygon, with
// noise wobble), its cliff cut into strata steps (a riser of `step` voxels, a tread of a set width, a jitter per step), an
// optional talus skirt at the foot, and material rules: strata bands, a cap on the plateau, scree on the skirt. The
// outline's signed distance d (map units, above 0 inside) drives everything: step k starts at d = (k - 1) x tread.
// The heights go to the field as a `grid` of 2n + 1 samples across n columns, so each column centre lands on a sample
// (the field samples the grid bilinearly), which keeps the steps hard instead of blurred.
import { noise2 } from "./heightfield-expr";
import { sceneLattice, type Area, type FieldArgs, type MaterialRule } from "./heightfield-plan";
import { WORLD_PER_MAP } from "./voxel";
import { ToolError } from "../registry";
import type { EditorStore } from "../core";

export type MesaShape =
  | { kind: "circle"; radius: number }
  | { kind: "ellipse"; radii: [number, number]; rotation?: number }
  | { kind: "polygon"; points: [number, number][] };

export interface MesaArgs {
  units?: "map" | "world";
  at?: [number, number];
  floor: number;
  top: number;
  shape: MesaShape;
  wobble?: { amount: number; scale?: number; seed?: number };
  strata?: { step?: number; riserJitter?: number; bands?: { material: number; thickness: number }[]; bandJitter?: { amount: number; scale?: number; seed?: number };
    /** bands (default): the bands repeat as given, level and of even thickness. beds: sedimentary beds, each band's
     * thickness varied 0.4-1.6 x by `seed`, every edge wandering (bandJitter, default a third of the mean thickness)
     * and dithered (`feather`, default one voxel, blue noise), so a small mesa does not read as a striped cube. */
    style?: "bands" | "beds"; seed?: number; feather?: number };
  /** Gullies: notches weathered into the cliff. Noise along the outline cuts it back by up to `depth` (units) in
   * alcoves about `spacing` apart (default 3 x depth), all the way up, so the plan is lobed instead of a disc or box. */
  gullies?: { depth: number; spacing?: number; seed?: number };
  tread?: { walkable?: boolean; width?: number; jitter?: number; scale?: number; seed?: number };
  talus?: { height?: number; width?: number; material?: number; profile?: "linear" | "concave" };
  cap?: { material: number; depth?: number };
  /** The steps' tops as absolute level ys (call units), lowest first, in place of even risers: the mesa has one step per
   * entry and the plateau. Each is built on the nearest whole voxel (the answer's ledges say where). */
  ledgeAt?: number[];
  /** No step's top within `band` of the level y `y` (a hazard line: Sandtide's sand line): a step that would be built
   * there moves to the nearest voxel level outside the band that keeps a riser to its neighbours, or is left out. */
  avoidBand?: { y: number; band: number };
  voxelSize?: number;
  keepFloor?: boolean;
}

export const MIN_WALK_TREAD = 1.5;           // map units: the least a walkable tread can be (and 2 voxels)
const MAX_GRID = 511;                        // heightfield grid samples a side: 255 columns

const clamp1 = (v: number) => Math.max(-1, Math.min(1, v));
const round3 = (v: number) => Math.round(v * 1000) / 1000;

/** The signed distance from (x, z) to the polygon's outline, above 0 inside. */
function polygonDistance(pts: [number, number][], x: number, z: number): number {
  let best = Infinity, inside = false;
  for (let i = 0, j = pts.length - 1; i < pts.length; j = i++) {
    const [ax, az] = pts[j], [bx, bz] = pts[i];
    const ex = bx - ax, ez = bz - az, len2 = ex * ex + ez * ez;
    const t = len2 ? Math.max(0, Math.min(1, ((x - ax) * ex + (z - az) * ez) / len2)) : 0;
    best = Math.min(best, Math.hypot(x - (ax + ex * t), z - (az + ez * t)));
    if ((az > z) !== (bz > z) && x < ax + ((z - az) * ex) / ez) inside = !inside;
  }
  return inside ? best : -best;
}

function validate(a: MesaArgs) {
  const sh = a.shape;
  if (!sh || typeof sh !== "object") throw new ToolError("shape: {kind: circle | ellipse | polygon, ...}");
  if (sh.kind === "circle") {
    if (!(sh.radius > 0)) throw new ToolError("shape.radius is above 0");
  } else if (sh.kind === "ellipse") {
    if (!Array.isArray(sh.radii) || sh.radii.length !== 2 || !sh.radii.every((r) => r > 0)) throw new ToolError("shape.radii: [rx, rz], both above 0");
  } else if (sh.kind === "polygon") {
    if (!Array.isArray(sh.points) || sh.points.length < 3 || sh.points.length > 64 || !sh.points.every((p) => Array.isArray(p) && p.length === 2 && p.every(Number.isFinite)))
      throw new ToolError("shape.points: 3-64 points [x, z]");
  } else throw new ToolError("shape.kind: circle, ellipse or polygon");
  if (sh.kind !== "polygon" && !(Array.isArray(a.at) && a.at.length === 2 && a.at.every(Number.isFinite))) throw new ToolError("at: [x, z], the centre of a circle or ellipse");
  if (!Number.isFinite(a.floor) || !Number.isFinite(a.top)) throw new ToolError("floor and top are numbers");
  if (!(a.top > a.floor)) throw new ToolError("top is above floor");
}

export interface Mesa {
  args: FieldArgs;                           // for planFit / apply (the height as a grid, the material rules)
  info: Record<string, unknown>;             // what the answer says about the mesa
  circles: Area[];                           // circles covering the footprint (call units)
}

export function mesaField(store: EditorStore, a: MesaArgs, extra: Partial<FieldArgs>): Mesa {
  validate(a);
  const k = a.units === "world" ? 1 / WORLD_PER_MAP : 1;           // call units -> map units
  const s = a.voxelSize ?? 1;                                       // map units per voxel
  const kv = k / s, ku = 1 / k;                                    // voxels per call unit; map -> call units
  const sh = a.shape;
  const st = a.strata ?? {}, tr = a.tread ?? {}, ta = a.talus, wob = a.wobble;
  const walkable = tr.walkable ?? true;
  const step = st.step ?? 2;
  if (!(step >= 1 && step <= 16 && Number.isInteger(step))) throw new ToolError("strata.step is a whole number of voxels, 1-16");
  const riser0 = step * s;
  const rj = st.riserJitter ?? 0;
  if (!(rj >= 0 && rj <= 0.5)) throw new ToolError("strata.riserJitter is 0-0.5 (a fraction of a riser)");
  const wM = tr.width === undefined ? (walkable ? Math.max(4, 3 * s) : 0) : tr.width * k;
  // a walkable tread is two voxels at least: a small mesa takes finer voxels (voxelSize up to half the width), or
  // walkable: false, where a width is a setback only (each step that far inside the one under it: a stepped face with
  // ledges that are not promised walkable, down to a fraction of a voxel) and no width is a sheer wall
  if (walkable && wM < Math.max(MIN_WALK_TREAD, 2 * s)) throw new ToolError(`tread.width ${round3(wM * ku)} is too narrow to walk at voxelSize ${s}: a walkable tread is at least ${round3(Math.max(MIN_WALK_TREAD, 2 * s) * ku)} (2 voxels, and ${MIN_WALK_TREAD} map units). ` +
    `For a small mesa: voxelSize ${Math.max(0.5, Math.floor((wM / 2) * 4) / 4)} or finer makes this width walkable${wM < MIN_WALK_TREAD ? " (not here: it is under the least a worm stands on)" : ""}; tread.walkable: false with the same width keeps the stepped face as ledges that are not promised walkable, and with no width makes a sheer cliff`);
  if (!walkable && tr.width !== undefined && !(tr.width >= 0)) throw new ToolError("tread.width is 0 or more");
  const setback = !walkable && wM > 0;
  const jitM = Math.min(tr.jitter === undefined ? (walkable ? 0.25 * wM : 0) : tr.jitter * k, walkable || setback ? 0.45 * wM : 0.9 * s);
  if (tr.jitter !== undefined && !(tr.jitter >= 0)) throw new ToolError("tread.jitter is 0 or more");
  const jScale = (tr.scale ?? Math.max(6, 4 * s)) * k;
  const wobAmt = (wob?.amount ?? 0) * k, wobScale = (wob?.scale ?? 12) * k;
  if (wob && !(wob.amount >= 0 && (wob.scale === undefined || wob.scale > 0))) throw new ToolError("wobble: amount 0 or more, scale above 0");

  const Hm = (a.top - a.floor) * k;
  const talusH = ta ? Math.min((ta.height ?? 2 * riser0) * k, Hm - riser0) : 0;
  if (ta && !(ta.height === undefined || ta.height > 0)) throw new ToolError("talus.height is above 0");
  if (ta && ta.width !== undefined && !(ta.width > 0)) throw new ToolError("talus.width is above 0");
  const talusW = ta ? (ta.width ?? 2.5 * talusH * ku) * k : 0;
  if (ta && !(talusH > 0)) throw new ToolError("talus needs room: the mesa is no higher than one riser");
  const Hs = Hm - talusH;                                          // the cliff's height above the talus
  const nEven = Math.max(1, Math.round(Hs / riser0));
  if (nEven > 64) throw new ToolError(`${nEven} strata steps: raise strata.step`);
  // the steps' heights above the floor: the risers vary by riserJitter and always add up to the cliff
  const weights: number[] = [];
  for (let i = 0; i < nEven; i++) weights.push(1 + rj * clamp1(noise2(i * 3.7 + 0.5, 0.5, (tr.seed ?? 0) + 91)) );
  const wsum = weights.reduce((p, c) => p + c, 0);
  const level: number[] = [];
  let acc = 0;
  for (let i = 0; i < nEven; i++) { acc += weights[i]; level.push(talusH + (Hs * acc) / wsum); }
  level[nEven - 1] = Hm;
  // the floor on the voxel lattice, as sampleField counts it: a height h (map units over the floor) is built at builtY(h)
  const { t: tScene } = sceneLattice(store);
  const t = extra.floorLattice ? [tScene[0], a.floor * k, tScene[2]] : tScene;
  const keepFloor = a.keepFloor ?? true;
  const floorExact = (a.floor * k - t[1]) / s, lift = keepFloor ? floorExact - Math.round(floorExact) : 0;
  const vox = (hUnits: number) => Math.round(hUnits * kv + (hUnits * kv > 0 ? lift : 0));
  const builtY = (h: number) => (Math.round(floorExact) + vox(h * ku)) * s + t[1];
  // ledgeAt: the steps' tops as given, each on its nearest voxel; avoidBand: none within the band of a hazard line
  const yToH = (y: number) => y - a.floor * k;                     // a level y (map) as a height over the floor
  let ledgeNote: Record<string, unknown> | undefined;
  if (a.ledgeAt !== undefined) {
    if (!Array.isArray(a.ledgeAt) || a.ledgeAt.length > 63 || !a.ledgeAt.every(Number.isFinite)) throw new ToolError("ledgeAt: up to 63 level ys, lowest first");
    const hs = a.ledgeAt.map((y) => yToH(y * k));
    hs.forEach((h, i) => {
      const under = i ? hs[i - 1] : talusH;
      if (!(h - under >= s - 1e-9)) throw new ToolError(`ledgeAt[${i}] (${a.ledgeAt![i]}) is less than a voxel (${round3(s * ku)}) over ${i ? `ledgeAt[${i - 1}]` : ta ? "the talus' top" : "the floor"}`);
    });
    if (hs.length && !(Hm - hs[hs.length - 1] >= s - 1e-9)) throw new ToolError(`ledgeAt's last (${a.ledgeAt[a.ledgeAt.length - 1]}) is less than a voxel under top`);
    level.length = 0;
    level.push(...hs, Hm);
    ledgeNote = { asked: a.ledgeAt.slice(), built: hs.map((h) => round3(builtY(h) * ku)) };
  }
  const avoided: { from: number; to: number | null }[] = [];
  if (a.avoidBand !== undefined) {
    const av = a.avoidBand;
    if (!av || !Number.isFinite(av.y) || !(av.band >= 0)) throw new ToolError("avoidBand: {y, band}, band 0 or more");
    const Y = av.y * k, B = av.band * k, inBand = (y: number) => Math.abs(y - Y) < B - 1e-9;
    for (let i = 0; i < level.length - 1; i++) {
      const y = builtY(level[i]);
      if (!inBand(y)) continue;
      // the nearest voxel levels under and over the band, a riser clear of the neighbours (the step under as moved)
      const lowLim = builtY(i ? level[i - 1] : talusH) + s, highLim = builtY(level[i + 1]) - s;
      let dn = y, up = y;
      while (inBand(dn)) dn -= s;
      while (inBand(up)) up += s;
      const okDn = dn >= lowLim - 1e-9, okUp = up <= highLim + 1e-9;
      const to = okDn && okUp ? (y - dn <= up - y ? dn : up) : okDn ? dn : okUp ? up : null;
      avoided.push({ from: round3(y * ku), to: to === null ? null : round3(to * ku) });
      if (to === null) { level.splice(i, 1); i--; } else level[i] = level[i] + (to - y);
    }
  }
  const n = level.length;
  const inset = (i: number) => i * wM;                             // step i (0-based) starts this far inside the outline (0: sheer)

  // the outline
  let cx = 0, cz = 0, bx0: number, bx1: number, bz0: number, bz1: number;
  let dist: (x: number, z: number) => number;
  if (sh.kind === "circle") {
    [cx, cz] = a.at!.map((v) => v * k);
    const R = sh.radius * k;
    dist = (x, z) => R - Math.hypot(x - cx, z - cz);
    [bx0, bx1, bz0, bz1] = [cx - R, cx + R, cz - R, cz + R];
  } else if (sh.kind === "ellipse") {
    [cx, cz] = a.at!.map((v) => v * k);
    const [rx, rz] = sh.radii.map((v) => v * k), th = ((sh.rotation ?? 0) * Math.PI) / 180, c = Math.cos(th), sn = Math.sin(th);
    // first-order distance to the ellipse: f / |grad f| of f = rho - 1 (rho the scaled radius)
    dist = (x, z) => {
      const px = (x - cx) * c + (z - cz) * sn, pz = -(x - cx) * sn + (z - cz) * c;
      const rho = Math.hypot(px / rx, pz / rz);
      if (rho < 1e-9) return Math.min(rx, rz);
      const g = Math.hypot(px / (rx * rx), pz / (rz * rz)) / rho;
      return (1 - rho) / g;
    };
    const hx = Math.hypot(rx * c, rz * sn), hz = Math.hypot(rx * sn, rz * c);
    [bx0, bx1, bz0, bz1] = [cx - hx, cx + hx, cz - hz, cz + hz];
  } else {
    const pts = sh.points.map((p) => [p[0] * k, p[1] * k] as [number, number]);
    dist = (x, z) => polygonDistance(pts, x, z);
    bx0 = Math.min(...pts.map((p) => p[0])); bx1 = Math.max(...pts.map((p) => p[0]));
    bz0 = Math.min(...pts.map((p) => p[1])); bz1 = Math.max(...pts.map((p) => p[1]));
  }
  const gul = a.gullies;
  if (gul && !(gul.depth >= 0 && (gul.spacing === undefined || gul.spacing > 0))) throw new ToolError("gullies: depth 0 or more, spacing above 0");
  const gulD = (gul?.depth ?? 0) * k, gulS = (gul?.spacing ?? 3 * (gul?.depth ?? 1)) * k * 0.5, gulSeed = (gul?.seed ?? 0) + 733;
  const reach = wobAmt + talusW + 2 * s;                           // the footprint's margin round the outline's box
  bx0 -= reach; bx1 += reach; bz0 -= reach; bz1 += reach;

  // the field's rectangle, on the Scene frame's lattice (as sampleField snaps it), and the columns in it
  const i0 = Math.floor((bx0 - t[0]) / s), i1 = Math.ceil((bx1 - t[0]) / s), j0 = Math.floor((bz0 - t[2]) / s), j1 = Math.ceil((bz1 - t[2]) / s);
  const nx = i1 - i0, nz = j1 - j0;
  if (nx > (MAX_GRID - 1) / 2 || nz > (MAX_GRID - 1) / 2)
    throw new ToolError(`the mesa's footprint is ${nx} x ${nz} voxels: at most ${(MAX_GRID - 1) / 2} a side (raise voxelSize, or build it smaller)`);
  const cols = 2 * nx + 1, rows = 2 * nz + 1;
  const grid: number[][] = Array.from({ length: rows }, () => new Array<number>(cols).fill(0));
  const seed = tr.seed ?? 0;
  const hMap = new Float32Array(nx * nz);
  let plateauMax = -Infinity, plateauCols = 0;
  const pts: [number, number][] = [];
  for (let b = 0; b < nz; b++)
    for (let c = 0; c < nx; c++) {
      const x = (i0 + c + 0.5) * s + t[0], z = (j0 + b + 0.5) * s + t[2];
      let d = dist(x, z);
      if (wobAmt) d += wobAmt * clamp1(0.9 * (noise2(x / wobScale, z / wobScale, (wob?.seed ?? 1)) + 0.5 * noise2((2 * x) / wobScale, (2 * z) / wobScale, (wob?.seed ?? 1) + 5)));
      if (gulD) {
        // only the cut side of the noise, sharpened: narrow alcoves between broad buttresses
        const g = Math.max(0, clamp1(1.6 * noise2(x / gulS, z / gulS, gulSeed)) - 0.1) / 0.9;
        d -= gulD * g * g;
      }
      let h = 0, lv = -1;
      for (let i = 0; i < n; i++) {
        const j = jitM ? jitM * clamp1(noise2(x / jScale, z / jScale, seed + 17 * (i + 1))) : 0;
        if (d >= inset(i) + j) lv = i;
      }
      if (lv >= 0) h = level[lv];
      else if (ta) h = d >= 0 ? talusH : (() => {
        const e = -d / talusW;
        return e >= 1 ? 0 : talusH * (ta.profile === "concave" ? (1 - e) ** 1.6 : 1 - e);
      })();
      if (lv === n - 1) { plateauMax = Math.max(plateauMax, d); plateauCols++; }
      hMap[b * nx + c] = h;
      const hu = h * ku;
      grid[2 * b + 1][2 * c + 1] = hu;
      if (h > 0) pts.push([x, z]);
    }
  if (!plateauCols) throw new ToolError(`the plateau is gone: ${n} steps of ${round3(wM * ku)} tread need an outline at least ${round3(inset(n - 1) * ku)} deep (raise strata.step to ${Math.min(16, step + 1)} for fewer steps, reduce tread.width, use a finer voxelSize, or make the mesa larger)`);
  // the even samples (column corners) carry their neighbour's height; the field never reads them but through rounding
  for (let r = 0; r < rows; r++) for (let c = 0; c < cols; c += 2) grid[r][c] = grid[r][Math.min(cols - 1, c + 1)] ?? 0;
  for (let r = 0; r < rows; r += 2) for (let c = 0; c < cols; c++) grid[r][c] = grid[Math.min(rows - 1, r + 1)][c];

  // the floor's lift, as sampleField counts it (keepFloor), to place the rules' thresholds on the voxel the field rounds to
  const rules: MaterialRule[] = [];
  if (a.cap) {
    const depth = a.cap.depth ?? 1;
    if (!Number.isInteger(depth) || depth < 1 || depth > 8) throw new ToolError("cap.depth is 1-8 voxels");
    rules.push({ material: a.cap.material, surface: depth, mask: `h - ${(vox(Hm * ku) - 0.5) / kv}` });
  }
  if (ta?.material !== undefined) rules.push({ material: ta.material, mask: `${(vox(talusH * ku) + 0.5) / kv} - h` });
  const bands = st.bands ?? [];
  let bandCount = 0;
  if (bands.length) {
    bands.forEach((b, i) => {
      if (!Number.isInteger(b.material) || b.material < 0 || b.material > 63) throw new ToolError(`strata.bands[${i}].material: a material is 0-63`);
      if (!(b.thickness > 0)) throw new ToolError(`strata.bands[${i}].thickness is above 0`);
    });
    const period = bands.reduce((p, b) => p + b.thickness, 0);
    const beds = st.style === "beds";
    if (st.style !== undefined && st.style !== "bands" && !beds) throw new ToolError("strata.style: bands or beds");
    if (st.feather !== undefined && !(st.feather >= 0)) throw new ToolError("strata.feather is 0 or more");
    const bj = st.bandJitter ?? (beds ? { amount: period / bands.length / 3, scale: Math.max(6, 4 * s) * ku, seed: st.seed ?? 0 } : undefined);
    const fe = st.feather ?? (beds ? s * ku : 0);
    let y = 0;
    for (let guard = 0; ; guard++) {
      const b = bands[guard % bands.length];
      // a bed's thickness: the band's, varied 0.4-1.6 x by the seed (never under a voxel, so every bed shows)
      const th = beds ? Math.max(s * ku, b.thickness * (1 + 0.6 * clamp1(1.8 * noise2(guard * 2.3 + 0.7, 1.3, (st.seed ?? 0) + 311)))) : b.thickness;
      const last = y + th >= (Hm * ku) - 1e-6;
      if (rules.length > 62) throw new ToolError(`the strata bands make more than 60 rules: thicker bands (a period is ${round3(period)} for ${round3(Hm * ku)} of height)`);
      rules.push({ material: b.material, minHeight: y, ...(last ? {} : { maxHeight: y + th }),
        ...(bj ? { jitter: { amount: bj.amount, scale: bj.scale, seed: (bj.seed ?? 0) + guard } } : {}),
        ...(fe > 0 ? { feather: { width: fe, pattern: "blue" as const, seed: (st.seed ?? 0) + guard } } : {}) });
      bandCount++;
      y += th;
      if (last) break;
    }
  }

  // the footprint's covering circles: the oriented box of the columns that stand (least area over 5-degree turns), covered
  // by circles along its long side
  let best = { area: Infinity, ux: 0, uz: 0, mid: [0, 0], hl: 0, hw: 0 };
  for (let deg = 0; deg < 90; deg += 5) {
    const th = (deg * Math.PI) / 180, ux = Math.cos(th), uz = Math.sin(th);
    let p0 = Infinity, p1 = -Infinity, q0 = Infinity, q1 = -Infinity;
    for (const [x, z] of pts) {
      const p = x * ux + z * uz, q = -x * uz + z * ux;
      p0 = Math.min(p0, p); p1 = Math.max(p1, p); q0 = Math.min(q0, q); q1 = Math.max(q1, q);
    }
    const area = (p1 - p0) * (q1 - q0);
    if (area < best.area) best = { area, ux, uz, mid: [(p0 + p1) / 2, (q0 + q1) / 2], hl: (p1 - p0) / 2 + s / 2, hw: (q1 - q0) / 2 + s / 2 };
  }
  let long = best.hl, wide = best.hw, ax = best.ux, az = best.uz, mp = best.mid[0], mq = best.mid[1];
  if (wide > long) { [long, wide] = [wide, long]; [ax, az] = [-az, ax]; [mp, mq] = [mq, -mp]; }
  const count = Math.max(1, Math.min(16, Math.ceil(long / wide)));
  const half = long / count, rad = Math.hypot(wide, half);
  // along-axis position p, across q: back to x, z through the axis (ax, az) and its normal (-az, ax)
  const circles: Area[] = [];
  for (let i = 0; i < count; i++) {
    const p = mp - long + half * (2 * i + 1), q = mq;
    circles.push({ x: round3((p * ax - q * az) * ku), z: round3((p * az + q * ax) * ku), radius: round3(rad * ku) });
  }

  const min: [number, number] = [(t[0] + i0 * s) * ku, (t[2] + j0 * s) * ku], max: [number, number] = [(t[0] + i1 * s) * ku, (t[2] + j1 * s) * ku];
  const args: FieldArgs = { ...extra, units: a.units ?? "map", voxelSize: s, min, max, floor: a.floor, keepFloor, grid, materials: rules };
  const um = (v: number) => round3(v * ku);
  // the exposed step tops as built: each level rounds to whole voxels over the lattice floor (the field's own rounding)
  const stepWidth = (i: number) => (i === n - 1 ? null : wM);
  const ledges = [
    ...(ta ? [{ kind: "talus top" as const, h: talusH, width: null as number | null, walk: false }] : []),
    ...level.map((h, i) => ({ kind: i === n - 1 ? "plateau" as const : "step" as const, h, width: stepWidth(i), walk: i === n - 1 || walkable })),
  ].filter((l, i, all) => l.kind !== "step" || wM > 0 || i === all.length - 1).map((l) => ({ kind: l.kind, y: { map: round3(builtY(l.h)), world: round3(builtY(l.h) * WORLD_PER_MAP) },
    ...(l.width !== null ? { width: um(l.width) } : {}), walkable: l.walk }));
  const info = {
    topY: { map: round3(a.top * k), world: round3(a.top * k * WORLD_PER_MAP) },
    height: { map: round3(Hm), world: round3(Hm * WORLD_PER_MAP) },
    steps: n, riser: { map: round3(riser0), voxels: step }, risers: level.map((v, i) => round3((v - (i ? level[i - 1] : talusH)) * ku)),
    tread: { walkable, width: um(wM), jitter: um(jitM), ...(setback ? { setback: true } : {}) },
    // every exposed top a worm or a sand line meets, as built (whole voxels), lowest first: the talus' top, each step's
    // tread (none on a sheer face) and the plateau
    ledges,
    plateau: { columns: plateauCols, area: round3(plateauCols * s * s * ku * ku), deepest: um(plateauMax) },
    talus: ta ? { height: um(talusH), width: um(talusW), profile: ta.profile ?? "linear" } : null,
    strataBands: bandCount, ...(st.style === "beds" ? { strataStyle: "beds" } : {}),
    ...(ledgeNote ? { ledgeAt: ledgeNote } : {}),
    ...(a.avoidBand ? { avoidBand: { y: a.avoidBand.y, band: a.avoidBand.band, moved: avoided, note: avoided.length ? "steps that would be built inside the band: moved to the nearest voxel level outside it (to), or left out (to: null)" : "no step's top was inside the band",
      ...(Math.abs(builtY(Hm) - a.avoidBand.y * k) < a.avoidBand.band * k ? { warning: "the plateau's top is inside the band (top is as asked: raise or lower it)" } : {}) } } : {}),
    ...(extra.floorLattice ? { lattice: { y: "floor", note: "the voxel lattice starts at floor: every ledge is floor + whole voxels" } } : {}),
    ...(gulD ? { gullies: { depth: um(gulD), spacing: um(gulS * 2) } } : {}),
    ...(plateauCols < 9 ? { warning: `the plateau is only ${plateauCols} column${plateauCols === 1 ? "" : "s"}: a tower, not a mesa (fewer or narrower steps, or a larger outline, leave a top to stand on)` } : {}),
    footprint: { min, max, units: a.units ?? "map", columns: [nx, nz], circles: count },
  };
  return { args, info, circles };
}
