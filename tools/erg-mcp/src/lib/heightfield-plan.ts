// Heightfield terrain: a height per column over a rectangle of the level, filled from a floor up, then packed into the
// voxels the patch can carry. Planning is pure (nothing changes until apply()).
//
// The lattice. Added blocks sit under the Scene frame (blocks.ts), so the plan works in Scene-frame space: with a voxel
// size s (map units per voxel, the blocks' scale, 1-4), cell (x, y, z) fills [x, x+1) x [y, y+1) x [z, z+1) times s
// there, and the Scene frame must be translation-only in the level (it is on the game's maps). Map = s * cell + t.
//
// The budget. A patch adds at most 64 blocks of at most 31 voxels a side (the game asserts on 32) with at most 2000
// runs each, and the game counts the cells of every voxel frame of the level (X*Y*Z, base and added, solid or empty) in
// 16 bits: at most 65535 (LIMITS.voxelCells), so a block's whole box counts, not its solid voxels. The plan:
//   1. fills what it can in voxel frames that overlap the field (no SLIPPY/PERM/TEAMBASE in the name): a voxels op on a
//      frame costs no block, and the server takes any valid word for any cell of a base frame, empty or not. By default
//      only frames on the lattice (translation-only, unit scale, whole-unit offset: earlier blocks; no base frame of the
//      game's maps is) take part; "any" also fills rotated and scaled frames (not scaled under 0.9) cell by cell, which
//      only approximates the field at its surface. Cells already solid there count as done. A frame whose runs would
//      pass 2000 is left out.
//   2. tiles the rest in 31x31 columns (trying four offsets each way, keeping the fewest blocks), stacks 31-high slices
//      in each tile from its lowest needed cell, and trims every slice to the box of the cells it needs, so a cone's
//      upper slices are small. A slice whose runs pass 2000 (material bands add a run per band per column) is halved
//      along its longer side until it fits.
//   3. when the blocks do not fit the budget (64 minus those already added, or maxBlocks, and the cells left), refuses, or with partial
//      keeps the blocks holding the most voxels, a slice only when the slice under it in its tile is kept (nothing floats),
//      and lists what was dropped.
import {
  AddFrame, CORNER_BIT, Group, LIMITS, SculptStroke, blockFrames, cellBudget, filled, frameWorld, isSolid, sceneFrame, sharedCorners, validNewFrameScale, voxelRuns, withSecond,
  type EditorStore,
  type Frame, type Vec3, type Command,
} from "../core";
import { ToolError } from "../registry";
import { PATCH_BUDGET, WORLD_PER_MAP, grids, runBytes, tidy, type Grid } from "./voxel";
import { compile, fbm, noise2, ExprError, type Vars } from "./heightfield-expr";

export type Units = "map" | "world";

export interface MaterialRule {
  material: number;
  minHeight?: number; maxHeight?: number;   // the voxel's height above the floor, in the region's units
  minSlope?: number; maxSlope?: number;     // the column's slope, degrees
  surface?: number;                         // only the top n voxels of a column
  minDepth?: number; maxDepth?: number;     // the voxel's depth under its column's top, in the region's units
  exposed?: boolean;                        // true: only voxels with air above or beside them; false: only buried ones
  jitter?: { amount: number; scale?: number; seed?: number };   // moves the height bounds by up to amount (noise)
  region?: Area[];                          // only columns whose centre is in one of these
  mask?: string;                            // an expression of the column and voxel: only where it is above 0
  masks?: string[];                         // more such expressions: only where at least one of them is above 0
  noise?: { threshold?: number; scale?: number; seed?: number; octaves?: number };   // only where noise is above threshold
  feather?: { width: number; pattern?: Feather; cell?: number; seed?: number };   // the mask and height bounds move by a per-voxel threshold in +-width/2
  paths?: PathMask[];                       // only within a path's width of one of these polylines (roads, switchbacks)
  exclude?: Area[];                         // never in these areas (the crater floor for a grass rule)
  excludeMask?: string;                     // never where this expression is above 0
  edge?: { depth?: number };                // the rule's own edge per corner: a voxel outside it with corners inside takes it as second material
}

/** How a feathered rule's threshold varies: dither (a 4 x 4 ordered pattern over x, z and y), noise (a hash), blue (a
 * 16 x 16 blue-noise tile: no checkerboard, no clumps) or perlin (smooth noise: the edge wanders in lobes). dither, noise
 * and blue take `cell` (units; default one voxel), the size of the squares the pattern is made of; perlin takes it as
 * its feature size (default 4 voxels). */
export type Feather = "dither" | "noise" | "blue" | "perlin";

/** A road or path for a material rule: the columns within width / 2 of the polyline `points` ([x, z], level coordinates
 * in the call's units), narrowing linearly to taper x width at the end; or a switchback from `from` to `to` folded into
 * `legs` straight legs `spread` wide (each leg crosses the line from -spread/2 to +spread/2, turn to turn). */
export interface PathMask {
  points?: [number, number][];
  width: number;
  taper?: number;
  switchback?: { from: [number, number]; to: [number, number]; legs: number; spread: number };
}

/** The polyline a path mask follows, in its own units: its points, or a switchback's turns. */
export function pathPoints(p: PathMask): [number, number][] {
  if (p.points) return p.points;
  const sw = p.switchback!;
  const dx = sw.to[0] - sw.from[0], dz = sw.to[1] - sw.from[1], len = Math.hypot(dx, dz) || 1;
  const nx = -dz / len, nz = dx / len, out: [number, number][] = [sw.from];
  for (let i = 0; i < sw.legs - 1; i++) {
    const t = (i + 1) / sw.legs, side = i % 2 === 0 ? 1 : -1;
    out.push([sw.from[0] + dx * t + nx * side * sw.spread / 2, sw.from[1] + dz * t + nz * side * sw.spread / 2]);
  }
  out.push(sw.to);
  return out;
}

/** A 16 x 16 blue-noise tile of thresholds in (-0.5, 0.5): the order in which a best-candidate fill (each next cell the
 * one farthest, on the torus, from those already taken; ties by position) takes the cells. */
const BLUE = (() => {
  const N = 16, taken: number[] = [], rank = new Float32Array(N * N).fill(-1);
  const d2 = (a: number, b: number) => {
    const ax = a % N, az = (a - ax) / N, bx = b % N, bz = (b - bx) / N;
    const dx = Math.min(Math.abs(ax - bx), N - Math.abs(ax - bx)), dz = Math.min(Math.abs(az - bz), N - Math.abs(az - bz));
    return dx * dx + dz * dz;
  };
  const near = new Float32Array(N * N).fill(Infinity);
  for (let k = 0; k < N * N; k++) {
    let best = -1;
    for (let c = 0; c < N * N; c++) if (rank[c] < 0 && (best < 0 || near[c] > near[best])) best = c;
    rank[best] = k;
    taken.push(best);
    for (let c = 0; c < N * N; c++) near[c] = Math.min(near[c], d2(c, best));
  }
  return Array.from(rank, (r) => (r + 0.5) / (N * N) - 0.5);
})();

/** Second-material corner blending at material boundaries (the game's per-corner blend, as the panel's 2nd material):
 * the top `depth` voxels of each column take the most common other material among their neighbours as their second
 * material, on the corners they share with it. */
export interface Blend { depth?: number; }

/** The variables a material rule's mask sees beyond the field's own: the column's height above the floor (h), the voxel's
 * height above the floor (y) and its depth under the column's top (depth), all in the region's units, and the column's
 * slope in degrees. */
export const MASK_VARIABLES = ["h", "y", "depth", "slope"] as const;

/** How a column's height becomes whole voxels: round (nearest; concentric terraces on a cone), or with a threshold that
 * varies per column, so a terrace edge breaks up instead of running as a ring: dither (an ordered 4 x 4 pattern) or
 * noise (a hash per column). Both keep the field's volume on average. */
export type Edges = "round" | "dither" | "noise";

export interface Crater { radius: number; depth: number; x?: number; z?: number; rim?: number; }

/** A part of the level in x, z (the region's units): a circle or a rectangle. */
export interface Area { x?: number; z?: number; radius?: number; min?: [number, number]; max?: [number, number]; }

export interface FieldArgs {
  units?: Units;
  min: [number, number];                    // x, z (level coordinates in units)
  max: [number, number];
  floor: number;                            // y the columns stand on
  keepFloor?: boolean;                      // heights from floor as given, not from the lattice height it snaps to
  expr?: string;
  grid?: number[][];                        // rows along z (row 0 at min z), columns along x
  maxHeight?: number;
  baseDepth?: number;
  crater?: Crater;
  material?: number;
  materials?: MaterialRule[];
  reuseFrames?: Reuse;
  maxBlocks?: number;
  partial?: boolean;
  voxelSize?: number;                       // map units per voxel (the new blocks' scale), 1-4; default 1
  maxCells?: number;                        // the new blocks hold at most this many of the game's cells
  centre?: [number, number];                // x, z origin of the expression's x, z, r, theta (default the rectangle's centre)
  clip?: Area[];                            // keep only columns whose centre is in one of these
  exclude?: Area[];                         // leave out columns lying wholly inside one of these
  runsHeadroom?: number;                    // runs to leave free in each new block for later edits (default 0)
  edges?: Edges;                            // default round
  packer?: Packer;                          // default tiles
  costAt?: number[];                        // also plan at these voxel sizes and report their cost
  fitBudget?: boolean;                      // raise voxelSize (in steps of 0.25) until the field fits maxBlocks and maxCells whole
  blend?: boolean | Blend;                  // blend material boundaries per corner (default off)
  relax?: number;                           // passes of smoothing over the heights (default 0)
  maxGradient?: number;                     // degrees: no column steeper than this against its neighbours (default none)
  // internal (never from a tool's arguments): keep a column by its centre (map units) and the voxel size, after clip
  // and exclude; heightfield_drape's refine cuts the fine zone out of the coarse skin and clips the fine one to it
  keepColumn?: (mx: number, mz: number, s: number) => boolean;
}

/** Where the field's heights stand: the floor asked and the lattice height it snapped to (map units), the shift, and
 * the y a height h has its ground at; a warning when the snap moves every height. */
export function floorReport(f: Field) {
  const used = f.floor * f.s + f.t[1], shift = used - f.floorAsked, n = (v: number) => Math.round(v * 1000) / 1000;
  const base = f.keepFloor ? f.floorAsked : used;
  return { asked: n(f.floorAsked), lattice: n(used), shift: n(shift), keepFloor: f.keepFloor,
    surface: `a height h has its ground at y = ${n(base)} + h (map units${f.s !== 1 ? `; a stepped column rounds to whole voxels of ${f.s}` : ""})`,
    ...(Math.abs(shift) > 1e-6 && !f.keepFloor ? { warning: `the floor ${n(f.floorAsked)} is off the ${f.s}-unit lattice and snapped to ${n(used)}: every height stands ${n(Math.abs(shift))} map units ${shift < 0 ? "lower" : "higher"} than floor + h (keepFloor: true counts from ${n(f.floorAsked)}, or give a floor on the lattice)` } : {}) };
}

/** Smooths and slope-limits a field's heights (voxels above the floor, one column per voxel) in place, over the columns
 * `live` marks: `relax` passes of a 3 x 3 mean (each live neighbour counted), then `maxGradient` (degrees) by moving
 * each too-steep pair of neighbours toward each other by half the excess (the volume stays), until none is left or 500
 * passes. Answers the passes the limiter took and the steepest slope left, degrees. */
export function limitSlopes(h: Float32Array, nx: number, nz: number, live: Uint8Array, relax = 0, maxGradient?: number): { passes: number; steepest: number } {
  const tmp = new Float32Array(h.length);
  for (let p = 0; p < relax; p++) {
    for (let b = 0; b < nz; b++)
      for (let a = 0; a < nx; a++) {
        const c = b * nx + a;
        if (!live[c]) { tmp[c] = h[c]; continue; }
        let sum = 0, n = 0;
        for (let dz = -1; dz <= 1; dz++) for (let dx = -1; dx <= 1; dx++) {
          const a2 = a + dx, b2 = b + dz;
          if (a2 < 0 || b2 < 0 || a2 >= nx || b2 >= nz || !live[b2 * nx + a2]) continue;
          sum += h[b2 * nx + a2]; n++;
        }
        tmp[c] = sum / n;
      }
    h.set(tmp);
  }
  let passes = 0, steepest = 0;
  const pairs: [number, number, number][] = [[1, 0, 1], [0, 1, 1], [1, 1, Math.SQRT2], [1, -1, Math.SQRT2]];
  if (maxGradient !== undefined) {
    const d = Math.tan((Math.min(89, Math.max(1, maxGradient)) * Math.PI) / 180);
    for (; passes < 500; passes++) {
      let moved = false;
      for (let b = 0; b < nz; b++)
        for (let a = 0; a < nx; a++) {
          const c = b * nx + a;
          if (!live[c]) continue;
          for (const [dx, dz, run] of pairs) {
            const a2 = a + dx, b2 = b + dz;
            if (a2 < 0 || b2 < 0 || a2 >= nx || b2 >= nz) continue;
            const c2 = b2 * nx + a2;
            if (!live[c2]) continue;
            const diff = h[c] - h[c2], excess = Math.abs(diff) - d * run;
            if (excess <= 1e-4) continue;
            const m = (excess / 2) * Math.sign(diff);
            h[c] -= m; h[c2] += m;
            moved = true;
          }
        }
      if (!moved) break;
    }
  }
  for (let b = 0; b < nz; b++)
    for (let a = 0; a + 1 < nx; a++) if (live[b * nx + a] && live[b * nx + a + 1]) steepest = Math.max(steepest, Math.abs(h[b * nx + a] - h[b * nx + a + 1]));
  for (let b = 0; b + 1 < nz; b++)
    for (let a = 0; a < nx; a++) if (live[b * nx + a] && live[(b + 1) * nx + a]) steepest = Math.max(steepest, Math.abs(h[b * nx + a] - h[(b + 1) * nx + a]));
  return { passes, steepest: (Math.atan(steepest) * 180) / Math.PI };
}

export const SIDE = LIMITS.newFrameSide;
const MAX_COLUMNS = 512 * 512, MAX_SPAN = 1024, MAX_COLUMN = 1024;
const OP_BYTES = 160;   // an addFrame op and its voxels op's envelope, JSON
// In the game a one-voxel surface layer of another material over 6954 columns (a 250 x 250 island at voxel size 2.5)
// made the landscape combiner run out of vertices (CombineLandscapeAction asserts, holes in the drawn terrain); the same
// island in height bands, or in one material, did not. Past this many columns a surface rule gets a note.
const SURFACE_NOTE_COLUMNS = 2500;

/** The sampled field: one column per lattice cell of the rectangle. */
export interface Field {
  k: number;                                // map units per region unit
  s: number;                                // map units per voxel (cell)
  t: Vec3;                                  // Scene frame -> map translation
  sceneId: number | null;                   // null: no Scene frame, no blocks
  i0: number; j0: number; nx: number; nz: number;
  floor: number;                            // floor cell (scene y)
  floorAsked: number;                       // the floor asked for, map y (the heights count from floorAsked with keepFloor)
  keepFloor: boolean;
  runLimit: number;                         // runs a new block may hold: the limit less runsHeadroom
  lo: Int32Array; hi: Int32Array;           // per column [lo, hi): hi <= lo is no column
  height: Float32Array;                     // per column, voxels above the floor (after crater, cap)
  slope: Float32Array;                      // per column, degrees
  limited?: { passes: number; steepest: number };   // relax / maxGradient ran: the limiter's passes and the steepest slope left
  materialAt(col: number, y: number): number;
  /** The word a field voxel is written as: filled(materialAt), with a second material at a boundary when blend is on.
   * `layered` compares a neighbour's voxel at the same depth under its own top (a drape's skin) instead of the same y. */
  wordAt(col: number, y: number, layered?: boolean): number;
}

const num = (v: number) => tidy([v, 0, 0])[0];
// The 4 x 4 ordered-dither matrix (Bayer), 0-15.
const BAYER = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5];
const mapOf = (f: Field, c: Vec3): Vec3 => [c[0] * f.s + f.t[0], c[1] * f.s + f.t[1], c[2] * f.s + f.t[2]];

function checkMaterial(m: number, path: string) {
  if (!Number.isInteger(m) || m < 0 || m > 63) throw new ToolError(`${path}: a material is 0-63`);
}

/** The Scene frame and its translation, or a ToolError. A level without one (the W3D maps) gets the level's own lattice
 * and no blocks: only lattice-aligned frames can be filled there. */
export function sceneLattice(store: EditorStore): { id: number | null; t: Vec3 } {
  const sf = sceneFrame(store.scene);
  if (!sf) return { id: null, t: [0, 0, 0] };
  const w = frameWorld(new Map(store.scene.frames.map((f) => [f.id, f])), sf.id);
  if (!w) throw new ToolError("the Scene frame has no placement");
  const lin = [w[0], w[1], w[2], w[4], w[5], w[6], w[8], w[9], w[10]], id = [1, 0, 0, 0, 1, 0, 0, 0, 1];
  if (lin.some((v, i) => Math.abs(v - id[i]) > 1e-6)) throw new ToolError("the Scene frame is rotated or scaled in this level: heightfields need it translation-only");
  return { id: sf.id, t: [w[3], w[7], w[11]] };
}

/** Samples the height function over the rectangle. */
export function sampleField(store: EditorStore, a: FieldArgs): Field {
  const k = a.units === "world" ? 1 / WORLD_PER_MAP : 1;
  const s = a.voxelSize ?? 1;
  if (!validNewFrameScale(s)) throw new ToolError(`voxelSize is ${LIMITS.newFrameScale.min}-${LIMITS.newFrameScale.max} map units per voxel`);
  const kv = k / s;                         // voxels per region unit
  const { id: sceneId, t } = sceneLattice(store);
  if (!!a.expr === !!a.grid) throw new ToolError("give the height as either expr or grid");
  const i0 = Math.round((Math.min(a.min[0], a.max[0]) * k - t[0]) / s), i1 = Math.round((Math.max(a.min[0], a.max[0]) * k - t[0]) / s);
  const j0 = Math.round((Math.min(a.min[1], a.max[1]) * k - t[2]) / s), j1 = Math.round((Math.max(a.min[1], a.max[1]) * k - t[2]) / s);
  const nx = i1 - i0, nz = j1 - j0;
  if (nx < 1 || nz < 1) throw new ToolError(`the rectangle is less than one voxel wide (1 voxel = ${s} map units = ${s * WORLD_PER_MAP} world units)`);
  if (nx > MAX_SPAN || nz > MAX_SPAN || nx * nz > MAX_COLUMNS)
    throw new ToolError(`the rectangle is ${nx}x${nz} voxels: at most ${MAX_SPAN} a side and ${MAX_COLUMNS} columns (the game's ${LIMITS.voxelCells} cells hold about 256x256 voxels one high; a larger voxelSize covers more ground)`);
  // the floor on the lattice; keepFloor counts the heights from the floor as asked (the remainder added to every height)
  const floorExact = (a.floor * k - t[1]) / s, floor = Math.round(floorExact), lift = a.keepFloor ? floorExact - floor : 0;
  const depth = Math.max(0, Math.round((a.baseDepth ?? 0) * kv));
  const material = a.material ?? 0;
  checkMaterial(material, "material");
  (a.materials ?? []).forEach((r, i) => checkMaterial(r.material, `materials[${i}].material`));

  // Region coordinates (units): x, z from the centre (the rectangle's, snapped to the lattice, unless `centre` names one:
  // fields of different voxel sizes then share their x, z, r and theta).
  if (a.centre && !a.centre.every(Number.isFinite)) throw new ToolError("centre: two finite numbers");
  const cx = a.centre ? a.centre[0] * k : ((i0 + i1) / 2) * s + t[0], cz = a.centre ? a.centre[1] * k : ((j0 + j1) / 2) * s + t[2];
  const areas = (list: Area[] | undefined, what: string) => (list ?? []).map((ar, i) => {
    const circle = ar.radius !== undefined;
    if (circle ? ar.x === undefined || ar.z === undefined || !(ar.radius! > 0) || ar.min || ar.max : !ar.min || !ar.max || ar.x !== undefined || ar.z !== undefined)
      throw new ToolError(`${what}[${i}]: give either x, z and radius (a circle) or min and max (a rectangle)`);
    return circle
      ? { circle: true, x: ar.x! * k, z: ar.z! * k, r: ar.radius! * k, x0: 0, z0: 0, x1: 0, z1: 0 }
      : { circle: false, x: 0, z: 0, r: 0, x0: Math.min(ar.min![0], ar.max![0]) * k, x1: Math.max(ar.min![0], ar.max![0]) * k,
        z0: Math.min(ar.min![1], ar.max![1]) * k, z1: Math.max(ar.min![1], ar.max![1]) * k };
  });
  const clip = areas(a.clip, "clip"), exclude = areas(a.exclude, "exclude");
  type A = (typeof clip)[number];
  const inArea = (ar: A, x: number, z: number) => ar.circle ? (x - ar.x) ** 2 + (z - ar.z) ** 2 <= ar.r * ar.r : x >= ar.x0 && x <= ar.x1 && z >= ar.z0 && z <= ar.z1;
  // clip keeps a column whose centre is inside; exclude drops one whose four corners are: a fine field clipped to an area
  // and a coarse one excluding the same area overlap at its edge instead of leaving a gap.
  const kept = (mx: number, mz: number) => (!clip.length || clip.some((ar) => inArea(ar, mx, mz))) &&
    !exclude.some((ar) => [-1, 1].every((dx) => [-1, 1].every((dz) => inArea(ar, mx + (dx * s) / 2, mz + (dz * s) / 2)))) &&
    (!a.keepColumn || a.keepColumn(mx, mz, s));
  const W = (nx * s) / k, D = (nz * s) / k;
  let fn: (v: Vars) => number;
  if (a.expr !== undefined) {
    try {
      fn = compile(a.expr);
    } catch (e) {
      if (e instanceof ExprError) throw new ToolError(`expr: ${e.message}`);
      throw e;
    }
  } else {
    const g = a.grid!;
    const rows = g.length, cols = g[0]?.length ?? 0;
    if (!rows || !cols || g.some((r) => r.length !== cols)) throw new ToolError("grid: every row needs the same number of heights");
    if (g.some((r) => r.some((v) => typeof v !== "number" || !Number.isFinite(v)))) throw new ToolError("grid: heights are numbers");
    fn = (v) => {
      const gx = v.u * (cols - 1), gz = v.v * (rows - 1);
      const x0 = Math.min(cols - 1, Math.floor(gx)), z0 = Math.min(rows - 1, Math.floor(gz));
      const x1 = Math.min(cols - 1, x0 + 1), z1 = Math.min(rows - 1, z0 + 1), fx = gx - x0, fz = gz - z0;
      const a0 = g[z0][x0] + (g[z0][x1] - g[z0][x0]) * fx, a1 = g[z1][x0] + (g[z1][x1] - g[z1][x0]) * fx;
      return a0 + (a1 - a0) * fz;
    };
  }
  const cr = a.crater;
  const crx = cr?.x ?? cx / k, crz = cr?.z ?? cz / k;
  const edges = a.edges ?? "round";
  // The threshold a column's fraction of a voxel must pass to count, by lattice cell (so fields that share a lattice
  // agree): 0.5 rounds; dither and noise spread it over (0, 1).
  const threshold = (ci: number, cj: number) => {
    if (edges === "dither") return (BAYER[(((cj % 4) + 4) % 4) * 4 + (((ci % 4) + 4) % 4)] + 0.5) / 16;
    if (edges === "noise") {
      let h = (Math.imul(ci, 0x27d4eb2d) ^ Math.imul(cj, 0x165667b1)) >>> 0;
      h = Math.imul(h ^ (h >>> 15), 0x85ebca6b) >>> 0;
      h = Math.imul(h ^ (h >>> 13), 0xc2b2ae35) >>> 0;
      return (((h ^ (h >>> 16)) >>> 0) + 0.5) / 4294967296;
    }
    return 0.5;
  };
  const n = nx * nz;
  const lo = new Int32Array(n), hi = new Int32Array(n), height = new Float32Array(n), slope = new Float32Array(n);
  const vars: Vars = { x: 0, z: 0, u: 0, v: 0, r: 0, theta: 0, W, D, R: Math.min(W, D) / 2, pi: Math.PI, e: Math.E };
  for (let b = 0; b < nz; b++)
    for (let a2 = 0; a2 < nx; a2++) {
      const mx = (i0 + a2 + 0.5) * s + t[0], mz = (j0 + b + 0.5) * s + t[2];
      vars.x = (mx - cx) / k;
      vars.z = (mz - cz) / k;
      vars.u = (a2 + 0.5) / nx;
      vars.v = (b + 0.5) / nz;
      vars.r = Math.hypot(vars.x, vars.z);
      vars.theta = Math.atan2(vars.z, vars.x);
      let h = fn(vars);
      if (cr) {
        const r = Math.hypot(mx / k - crx, mz / k - crz);
        if (r < cr.radius) h -= cr.depth * (1 - (r / cr.radius) ** 2);
        if (cr.rim) h += cr.rim * Math.exp(-(((r - cr.radius) / (cr.radius * 0.3)) ** 2));
      }
      if (a.maxHeight !== undefined) h = Math.min(h, a.maxHeight);
      const hv = Math.max(0, h * kv + (h * kv > 0 ? lift : 0));
      if (hv > MAX_COLUMN) throw new ToolError(`the height reaches ${num(h)} (${Math.round(hv)} voxels) at x ${num(mx / k)}, z ${num(mz / k)}: at most ${MAX_COLUMN} voxels; pass maxHeight`);
      const c = b * nx + a2, top = edges === "round" ? Math.round(hv) : Math.floor(hv + 1 - threshold(i0 + a2, j0 + b));
      height[c] = hv;
      if (top >= 1 && kept(mx, mz)) {
        lo[c] = floor - depth;
        hi[c] = floor + top;
      }
    }
  // relax and maxGradient: the heights smoothed and slope-limited over the kept columns, then made whole voxels again
  let limited: { passes: number; steepest: number } | undefined;
  if ((a.relax ?? 0) > 0 || a.maxGradient !== undefined) {
    if (a.relax !== undefined && !(Number.isInteger(a.relax) && a.relax >= 0 && a.relax <= 50)) throw new ToolError("relax is 0-50 passes");
    if (a.maxGradient !== undefined && !(a.maxGradient >= 1 && a.maxGradient <= 89)) throw new ToolError("maxGradient is 1-89 degrees");
    const keptCol = new Uint8Array(n);
    for (let b = 0; b < nz; b++)
      for (let a2 = 0; a2 < nx; a2++) keptCol[b * nx + a2] = height[b * nx + a2] > 0 && kept((i0 + a2 + 0.5) * s + t[0], (j0 + b + 0.5) * s + t[2]) ? 1 : 0;
    limited = limitSlopes(height, nx, nz, keptCol, a.relax ?? 0, a.maxGradient);
    for (let c = 0; c < n; c++) {
      if (!keptCol[c]) continue;
      const a2 = c % nx, b = (c - a2) / nx, hv = Math.max(0, height[c]);
      height[c] = hv;
      const top = edges === "round" ? Math.round(hv) : Math.floor(hv + 1 - threshold(i0 + a2, j0 + b));
      if (top >= 1) { lo[c] = floor - depth; hi[c] = floor + top; } else { lo[c] = 0; hi[c] = 0; }
    }
  }
  for (let b = 0; b < nz; b++)
    for (let a2 = 0; a2 < nx; a2++) {
      const c = b * nx + a2;
      const hx = (height[b * nx + Math.min(nx - 1, a2 + 1)] - height[b * nx + Math.max(0, a2 - 1)]) / Math.max(1, Math.min(nx - 1, a2 + 1) - Math.max(0, a2 - 1));
      const hz = (height[Math.min(nz - 1, b + 1) * nx + a2] - height[Math.max(0, b - 1) * nx + a2]) / Math.max(1, Math.min(nz - 1, b + 1) - Math.max(0, b - 1));
      slope[c] = (Math.atan(Math.hypot(hx, hz)) * 180) / Math.PI;
    }
  const rules = a.materials ?? [];
  rules.forEach((r, i) => {
    if (r.jitter && !(Number.isFinite(r.jitter.amount) && (r.jitter.scale === undefined || r.jitter.scale > 0)))
      throw new ToolError(`materials[${i}].jitter: amount is a number and scale above 0`);
    if (r.noise && !((r.noise.scale === undefined || r.noise.scale > 0) && (r.noise.octaves === undefined || (Number.isInteger(r.noise.octaves) && r.noise.octaves >= 1 && r.noise.octaves <= 8))))
      throw new ToolError(`materials[${i}].noise: scale above 0, octaves 1-8`);
  });
  // a rule's region, mask and noise, prepared once: the areas in map units, the mask compiled
  const ruleRegion = rules.map((r, i) => (r.region ? areas(r.region, `materials[${i}].region`) : null));
  const ruleExclude = rules.map((r, i) => (r.exclude ? areas(r.exclude, `materials[${i}].exclude`) : null));
  const maskFn = (src: string, path: string) => {
    try {
      return compile(src, MASK_VARIABLES) as unknown as (v: Record<string, number>) => number;
    } catch (e) {
      if (e instanceof ExprError) throw new ToolError(`${path}: ${e.message}`);
      throw e;
    }
  };
  const ruleMask = rules.map((r, i) => (r.mask === undefined ? null : maskFn(r.mask, `materials[${i}].mask`)));
  const ruleExcludeMask = rules.map((r, i) => (r.excludeMask === undefined ? null : maskFn(r.excludeMask, `materials[${i}].excludeMask`)));
  rules.forEach((r, i) => {
    if (r.edge && (typeof r.edge !== "object" || (r.edge.depth !== undefined && !(Number.isInteger(r.edge.depth) && r.edge.depth >= 1 && r.edge.depth <= 8))))
      throw new ToolError(`materials[${i}].edge: {depth}, depth 1-8 voxels`);
    if (r.edge && !(r.region || r.mask !== undefined || r.masks || r.noise || r.paths))
      throw new ToolError(`materials[${i}].edge: the rule needs an area to have an edge (region, mask, masks, noise or paths)`);
  });
  const ruleMasks = rules.map((r, i) => {
    if (r.masks === undefined) return null;
    if (!r.masks.length) throw new ToolError(`materials[${i}].masks: give at least one expression`);
    return r.masks.map((m, j) => maskFn(m, `materials[${i}].masks[${j}]`));
  });
  rules.forEach((r, i) => {
    if (r.feather && !(Number.isFinite(r.feather.width) && r.feather.width >= 0)) throw new ToolError(`materials[${i}].feather: width is a number of 0 or more`);
    if (r.feather?.cell !== undefined && !(r.feather.cell > 0)) throw new ToolError(`materials[${i}].feather: cell is above 0`);
  });
  // A feathered rule's threshold at a voxel, in (-0.5, 0.5): dither, an ordered pattern over x, z and y; noise, a hash;
  // blue, a blue-noise tile over x and z; perlin, smooth noise. The pattern is laid in squares of `cell` voxels.
  const featherAt = (f: { pattern?: Feather; cell?: number; seed?: number }, ci: number, cj: number, y: number) => {
    const pattern = f.pattern;
    if (pattern === "perlin") {
      const fs = f.cell === undefined ? 4 : Math.max(0.25, (f.cell * k) / s);
      return Math.max(-0.5, Math.min(0.5, 0.6 * noise2((ci + 0.5) / fs, (cj + 0.5) / fs, f.seed ?? 0)));
    }
    const cv = f.cell === undefined ? 1 : Math.max(1, (f.cell * k) / s);
    const qi = cv === 1 ? ci : Math.floor(ci / cv), qj = cv === 1 ? cj : Math.floor(cj / cv), qy = cv === 1 ? y : Math.floor(y / cv);
    if (pattern === "noise") {
      let h = (Math.imul(qi, 0x27d4eb2d) ^ Math.imul(qj, 0x165667b1) ^ Math.imul(qy, 0x9e3779b9) ^ Math.imul(f.seed ?? 0, 0x7feb352d)) >>> 0;
      h = Math.imul(h ^ (h >>> 15), 0x85ebca6b) >>> 0;
      h = Math.imul(h ^ (h >>> 13), 0xc2b2ae35) >>> 0;
      return (((h ^ (h >>> 16)) >>> 0) + 0.5) / 4294967296 - 0.5;
    }
    if (pattern === "blue") return BLUE[((((qj + 7 * (f.seed ?? 0)) % 16) + 16) % 16) * 16 + ((((qi + 11 * (f.seed ?? 0)) % 16) + 16) % 16)];
    return (BAYER[((((qj + qy) % 4) + 4) % 4) * 4 + (((qi % 4) + 4) % 4)] + 0.5) / 16 - 0.5;
  };
  // A rule's paths: per column, the most any path's half width exceeds the distance to it (above 0: on the path), in
  // the call's units, computed when first asked for.
  const rulePaths = rules.map((r, i) => {
    if (!r.paths) return null;
    if (!r.paths.length) throw new ToolError(`materials[${i}].paths: give at least one path`);
    const segs = r.paths.map((p, j) => {
      const where = `materials[${i}].paths[${j}]`;
      if (!(p.width > 0)) throw new ToolError(`${where}: width is above 0`);
      if (!p.points === !p.switchback) throw new ToolError(`${where}: give points or switchback`);
      if (p.switchback && !(Number.isInteger(p.switchback.legs) && p.switchback.legs >= 1 && p.switchback.legs <= 64 && p.switchback.spread >= 0)) throw new ToolError(`${where}.switchback: legs 1-64, spread 0 or more`);
      const pts = pathPoints(p);
      if (pts.length < 2 || pts.length > 512 || !pts.every((q) => Array.isArray(q) && q.length === 2 && q.every(Number.isFinite))) throw new ToolError(`${where}: 2-512 points [x, z]`);
      let total = 0;
      const cum = [0];
      for (let q = 1; q < pts.length; q++) cum.push((total += Math.hypot(pts[q][0] - pts[q - 1][0], pts[q][1] - pts[q - 1][1])));
      return { pts, cum, total: total || 1, w: p.width, taper: p.taper ?? 1 };
    });
    return { segs, at: new Float32Array(nx * nz).fill(NaN) };
  });
  const pathAt = (ri: number, col: number, mx: number, mz: number) => {
    const rp = rulePaths[ri]!;
    let v = rp.at[col];
    if (!Number.isNaN(v)) return v;
    v = pathAtPoint(ri, mx, mz);
    rp.at[col] = v;
    return v;
  };
  // the same at any point (map x, z), uncached: a column's corners for the rule's edge
  function pathAtPoint(ri: number, mx: number, mz: number) {
    const rp = rulePaths[ri]!;
    let v = -Infinity;
    const x = mx / k, z = mz / k;
    for (const sg of rp.segs) for (let q = 1; q < sg.pts.length; q++) {
      const [ax, az] = sg.pts[q - 1], [bx, bz] = sg.pts[q], dx = bx - ax, dz = bz - az, l2 = dx * dx + dz * dz;
      const tt = l2 > 0 ? Math.max(0, Math.min(1, ((x - ax) * dx + (z - az) * dz) / l2)) : 0;
      const d = Math.hypot(x - ax - tt * dx, z - az - tt * dz), along = (sg.cum[q - 1] + tt * Math.sqrt(l2)) / sg.total;
      v = Math.max(v, (sg.w * (1 + (sg.taper - 1) * along)) / 2 - d);
    }
    return v;
  }
  const mvars: Record<string, number> = { ...vars, h: 0, y: 0, depth: 0, slope: 0 };
  const centreOf = (col: number): [number, number] => { const ai = col % nx, bj = (col - ai) / nx; return [(i0 + ai + 0.5) * s + t[0], (j0 + bj + 0.5) * s + t[2]]; };
  // The top of the neighbouring column (its hi; the floor where it has none), for exposed: a voxel is exposed when
  // the column's top is it, or a neighbour's top is at or under it (air beside it), the field's edge included.
  const topOf = (ai: number, bj: number) => {
    if (ai < 0 || bj < 0 || ai >= nx || bj >= nz) return -Infinity;
    const c = bj * nx + ai;
    return hi[c] > lo[c] ? hi[c] : -Infinity;
  };
  // Whether rule ri holds for the voxel at height y of column col; `at` (map x, z) moves the rule's area tests (region,
  // paths, noise, masks, exclusions) to another point of the column (a corner, for the rule's edge), the height, slope
  // and depth bounds staying the voxel's. `exposed` is the voxel's, worked out once by the caller when a rule asks.
  const holds = (r: MaterialRule, ri: number, col: number, y: number, exp: { v?: boolean }, at?: [number, number]) => {
    const above = (y + 0.5 - floor) / kv, fromTop = hi[col] - 1 - y, sl = slope[col], depth = (hi[col] - y - 0.5) / kv;
    let shift = 0;
    if (r.jitter && (r.minHeight !== undefined || r.maxHeight !== undefined)) {
      const ai = col % nx, bj = (col - ai) / nx, sc = r.jitter.scale ?? 8;
      // the column's centre in region units, so the bands wander the same way whatever the voxel size
      shift = r.jitter.amount * noise2(((i0 + ai + 0.5) * s + t[0]) / k / sc, ((j0 + bj + 0.5) * s + t[2]) / k / sc, r.jitter.seed ?? 0);
    }
    // feather: the bounds and masks move by a threshold that varies per voxel, so an edge becomes a dithered band
    let fe = 0;
    if (r.feather && r.feather.width > 0) {
      const ai = col % nx, bj = (col - ai) / nx;
      fe = featherAt(r.feather, i0 + ai, j0 + bj, y) * r.feather.width;
      shift += fe;
    }
    if (r.minHeight !== undefined && above < r.minHeight + shift) return false;
    if (r.maxHeight !== undefined && above >= r.maxHeight + shift) return false;
    if (r.minSlope !== undefined && sl < r.minSlope) return false;
    if (r.maxSlope !== undefined && sl > r.maxSlope) return false;
    if (r.surface !== undefined && fromTop >= r.surface) return false;
    if (r.minDepth !== undefined && depth < r.minDepth) return false;
    if (r.maxDepth !== undefined && depth >= r.maxDepth) return false;
    if (r.exposed !== undefined) {
      if (exp.v === undefined) {
        const ai = col % nx, bj = (col - ai) / nx;
        exp.v = fromTop === 0 || [[1, 0], [-1, 0], [0, 1], [0, -1]].some(([dx, dz]) => topOf(ai + dx, bj + dz) <= y);
      }
      if (exp.v !== r.exposed) return false;
    }
    if (ruleRegion[ri] || ruleMask[ri] || ruleMasks[ri] || r.noise || rulePaths[ri] || ruleExclude[ri] || ruleExcludeMask[ri]) {
      const [mx, mz] = at ?? centreOf(col);
      if (ruleRegion[ri] && !ruleRegion[ri]!.some((ar) => inArea(ar, mx, mz))) return false;
      if (ruleExclude[ri] && ruleExclude[ri]!.some((ar) => inArea(ar, mx, mz))) return false;
      // a corner's distance to the path is its own, not the column's cached one
      if (rulePaths[ri] && !((at ? pathAtPoint(ri, mx, mz) : pathAt(ri, col, mx, mz)) > fe)) return false;
      if (r.noise) {
        const sc = r.noise.scale ?? 8, oc = r.noise.octaves ?? 1, nx2 = mx / k / sc, nz2 = mz / k / sc;
        const v = oc > 1 ? fbm(nx2, nz2, r.noise.seed ?? 0, oc) : noise2(nx2, nz2, r.noise.seed ?? 0);
        if (!(v > (r.noise.threshold ?? 0))) return false;
      }
      if (ruleMask[ri] || ruleMasks[ri] || ruleExcludeMask[ri]) {
        mvars.x = (mx - cx) / k; mvars.z = (mz - cz) / k;
        mvars.u = ((mx - t[0]) / s - i0) / nx; mvars.v = ((mz - t[2]) / s - j0) / nz;
        mvars.r = Math.hypot(mvars.x, mvars.z); mvars.theta = Math.atan2(mvars.z, mvars.x);
        mvars.h = (hi[col] - floor) / kv; mvars.y = above; mvars.depth = depth; mvars.slope = sl;
        if (ruleMask[ri] && !(ruleMask[ri]!(mvars) > fe)) return false;
        if (ruleMasks[ri] && !ruleMasks[ri]!.some((fn) => fn(mvars) > fe)) return false;
        if (ruleExcludeMask[ri] && ruleExcludeMask[ri]!(mvars) > 0) return false;
      }
    }
    return true;
  };
  const materialAt = (col: number, y: number) => {
    if (!rules.length) return material;
    const exp: { v?: boolean } = {};
    for (let ri = 0; ri < rules.length; ri++) if (holds(rules[ri], ri, col, y, exp)) return rules[ri].material;
    return material;
  };
  const runLimit = Math.max(100, LIMITS.runsPerFrame - Math.max(0, Math.round(a.runsHeadroom ?? 0)));
  // Corner blending: a voxel among the top `depth` of its column whose neighbours (the 26 round it, in the field) include
  // another material takes the most common such material as its second, on the corners it shares with those voxels.
  if (a.blend !== undefined && typeof a.blend !== "boolean" && (typeof a.blend !== "object" || a.blend === null || Array.isArray(a.blend)))
    throw new ToolError("blend is true, false or {depth}");
  const blendDepth = !a.blend ? 0 : a.blend === true ? 1 : Math.round(a.blend.depth ?? 1);
  if (a.blend && !(blendDepth >= 1 && blendDepth <= 8)) throw new ToolError("blend.depth is 1-8 voxels");
  const cache = new Map<number, number>();
  const matCached = (col: number, y: number) => {
    const key = col * 8192 + (y - floor + 4096);
    let m = cache.get(key);
    if (m === undefined) { m = materialAt(col, y); if (cache.size < 2_000_000) cache.set(key, m); }
    return m;
  };
  // A rule's own edge (edge: {depth}): a voxel among the top `depth` of its column that a later rule (or the default)
  // paints takes the edged rule's material as its second material on the corners where the rule holds (its area tested
  // at each of the column's four corners in x, z, the height and slope bounds the voxel's), so the boundary is drawn
  // where the mask or path really runs, between voxel centres, instead of as a staircase of whole voxels.
  const edgeRules = rules.map((r, ri) => (r.edge ? ri : -1)).filter((ri) => ri >= 0);
  const edgeDepth = edgeRules.reduce((m, ri) => Math.max(m, rules[ri].edge!.depth ?? 1), 0);
  const edgeWord = (col: number, y: number): number | null => {
    const fromTop = hi[col] - 1 - y, exp: { v?: boolean } = {};
    let own = 0;
    while (own < rules.length && !holds(rules[own], own, col, y, exp)) own++;
    const mat = own < rules.length ? rules[own].material : material;
    const ai = col % nx, bj = (col - ai) / nx;
    for (const ri of edgeRules) {
      if (ri >= own) break;
      const r = rules[ri];
      if (r.material === mat || fromTop >= (r.edge!.depth ?? 1)) continue;
      let bits = 0;
      for (let cz = 0; cz < 2; cz++)
        for (let cx = 0; cx < 2; cx++)
          if (holds(r, ri, col, y, exp, [(i0 + ai + cx) * s + t[0], (j0 + bj + cz) * s + t[2]]))
            bits |= (1 << CORNER_BIT[1][cz][cx]) | (1 << CORNER_BIT[0][cz][cx]);
      if (bits) return withSecond(filled(mat), r.material, bits);
    }
    return null;
  };
  const field: Field = { k, s, t, sceneId, i0, j0, nx, nz, floor, floorAsked: a.floor * k, keepFloor: !!a.keepFloor, runLimit, lo, hi, height, slope, materialAt, ...(limited ? { limited } : {}),
    wordAt(col: number, y: number, layered = false) {
      if (edgeDepth && hi[col] - 1 - y < edgeDepth) {
        const e = edgeWord(col, y);
        if (e !== null) return e;
      }
      if (!blendDepth || hi[col] - 1 - y >= blendDepth) return filled(materialAt(col, y));
      const own = matCached(col, y), ai = col % nx, bj = (col - ai) / nx;
      // the material of the field voxel at offset (dx, dy, dz), or -1 where there is none
      const at = (dx: number, dy: number, dz: number) => {
        const a2 = ai + dx, b2 = bj + dz;
        if (a2 < 0 || b2 < 0 || a2 >= nx || b2 >= nz) return -1;
        const c2 = b2 * nx + a2, y2 = (layered ? y - hi[col] + hi[c2] : y) + dy;
        return hi[c2] > lo[c2] && y2 >= lo[c2] && y2 < hi[c2] ? matCached(c2, y2) : -1;
      };
      const count = new Map<number, number>();
      for (let dy = -1; dy <= 1; dy++) for (let dz = -1; dz <= 1; dz++) for (let dx = -1; dx <= 1; dx++) {
        if (!dx && !dy && !dz) continue;
        const m = at(dx, dy, dz);
        if (m >= 0 && m !== own) count.set(m, (count.get(m) ?? 0) + 1);
      }
      if (!count.size) return filled(own);
      let second = -1, n = 0;
      for (const [m, c] of count) if (c > n || (c === n && m < second)) { second = m; n = c; }
      const mask = sharedCorners(0, 0, 0, (x2, y2, z2) => at(x2, y2, z2) === second);
      if (!mask) return filled(own);
      return withSecond(filled(own), second, mask);
    } };
  return field;
}

// ------------------------------------------------------------------ frames as canvas

export type Reuse = "aligned" | "any" | "none";

export interface FrameFill {
  grid: Grid;
  aligned: boolean;                         // its cells are the lattice's
  changes: Map<number, [number, number]>;  // word index -> [before, after]
  covers: number;                           // field cells this fill makes solid
  solid: number;                            // field cells already solid here
  runs: number;                             // the frame's runs after the fill
  bytes: number;                            // JSON bytes of those runs minus the frame's runs now
}

export interface Canvas {
  fills: FrameFill[];
  skipped: { id: number; name: string; why: string }[];
  covered: Map<number, Uint8Array>;         // column -> per cell of [lo, hi): done by a frame
}

const TAGGED = /SLIPPY|PERM|TEAMBASE/i;
const IDENTITY = [1, 0, 0, 0, 1, 0, 0, 0, 1];

/** Voxel frames that overlap the field, and what each can take. A frame's cell is filled when its centre is in the
 * field; a field cell is done when its centre is in a frame cell that is solid (before or after). For a frame on the
 * lattice (translation-only, scaled by the voxel size, offset by whole voxels) the two are the same cells. Other frames
 * ("any") are props placed at any angle and scale, so their cells only approximate the field's (up to half a scaled voxel
 * at the surface). */
export function canvas(store: EditorStore, f: Field, reuse: Reuse = "aligned"): Canvas {
  const out: Canvas = { fills: [], skipped: [], covered: new Map() };
  if (reuse === "none") return out;
  let yMin = Infinity, yMax = -Infinity;
  for (let c = 0; c < f.lo.length; c++) if (f.hi[c] > f.lo[c]) { yMin = Math.min(yMin, f.lo[c]); yMax = Math.max(yMax, f.hi[c]); }
  if (yMin > yMax) return out;
  // A field cell (scene) -> its column and offset in [lo, hi), or null.
  const cellOf = (sx: number, sy: number, sz: number): [number, number] | null => {
    const a = sx - f.i0, b = sz - f.j0;
    if (a < 0 || b < 0 || a >= f.nx || b >= f.nz) return null;
    const col = b * f.nx + a;
    return sy >= f.lo[col] && sy < f.hi[col] ? [col, sy - f.lo[col]] : null;
  };
  const s = f.s;
  for (const g of grids(store)) {
    if (g.frame.corners) continue;   // a draped block's cells are sheared off its grid: not a canvas
    const m = g.toMap, inv = g.fromMap;
    const x0 = Math.max(f.i0, Math.floor((g.bounds.min[0] - f.t[0]) / s)), x1 = Math.min(f.i0 + f.nx, Math.ceil((g.bounds.max[0] - f.t[0]) / s));
    const z0 = Math.max(f.j0, Math.floor((g.bounds.min[2] - f.t[2]) / s)), z1 = Math.min(f.j0 + f.nz, Math.ceil((g.bounds.max[2] - f.t[2]) / s));
    const y0 = Math.max(yMin, Math.floor((g.bounds.min[1] - f.t[1]) / s)), y1 = Math.min(yMax, Math.ceil((g.bounds.max[1] - f.t[1]) / s));
    if (x0 >= x1 || z0 >= z1 || y0 >= y1) continue;
    const unit = [m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]].every((v, i) => Math.abs(v - s * IDENTITY[i]) <= 1e-6);
    const whole = [m[3] - f.t[0], m[7] - f.t[1], m[11] - f.t[2]].every((v) => Math.abs(v / s - Math.round(v / s)) <= 1e-4);
    const aligned = unit && whole;
    if (!aligned && reuse !== "any") {
      out.skipped.push({ id: g.id, name: g.frame.name,
        why: unit ? "not on whole voxels of the Scene frame's lattice" : s === 1 ? "rotated or scaled" : "rotated, or scaled other than voxelSize" });
      continue;
    }
    if (!aligned && [0, 1, 2].some((j) => Math.hypot(m[j], m[4 + j], m[8 + j]) < 0.9 * s)) {
      out.skipped.push({ id: g.id, name: g.frame.name, why: "scaled down: its voxels are smaller than the lattice's" });
      continue;
    }
    if (TAGGED.test(g.frame.name)) {
      out.skipped.push({ id: g.id, name: g.frame.name, why: "its name tags it slippery, indestructible or a team base" });
      continue;
    }
    const [X, Y, Z] = g.size;
    // Empty frame cells whose centre is in a field cell still to do.
    const changes = new Map<number, [number, number]>();
    for (let z = 0; z < Z; z++)
      for (let x = 0; x < X; x++)
        for (let y = 0; y < Y; y++) {
          const i = (z * X + x) * Y + y, w = g.words[i];
          if (isSolid(w)) continue;
          const px = m[0] * (x + 0.5) + m[1] * (y + 0.5) + m[2] * (z + 0.5) + m[3] - f.t[0];
          const py = m[4] * (x + 0.5) + m[5] * (y + 0.5) + m[6] * (z + 0.5) + m[7] - f.t[1];
          const pz = m[8] * (x + 0.5) + m[9] * (y + 0.5) + m[10] * (z + 0.5) + m[11] - f.t[2];
          const sy = Math.floor(py / s), c = cellOf(Math.floor(px / s), sy, Math.floor(pz / s));
          if (!c || out.covered.get(c[0])?.[c[1]]) continue;
          changes.set(i, [w, f.wordAt(c[0], sy)]);
        }
    // Field cells whose centre is in a frame cell solid before or after.
    const marks: [number, number][] = [];
    let covers = 0, solid = 0;
    for (let sz = z0; sz < z1; sz++)
      for (let sx = x0; sx < x1; sx++)
        for (let sy = y0; sy < y1; sy++) {
          const c = cellOf(sx, sy, sz);
          if (!c || out.covered.get(c[0])?.[c[1]]) continue;
          const px = (sx + 0.5) * s + f.t[0], py = (sy + 0.5) * s + f.t[1], pz = (sz + 0.5) * s + f.t[2];
          const gx = Math.floor(inv[0] * px + inv[1] * py + inv[2] * pz + inv[3]);
          const gy = Math.floor(inv[4] * px + inv[5] * py + inv[6] * pz + inv[7]);
          const gz = Math.floor(inv[8] * px + inv[9] * py + inv[10] * pz + inv[11]);
          if (gx < 0 || gy < 0 || gz < 0 || gx >= X || gy >= Y || gz >= Z) continue;
          const i = (gz * X + gx) * Y + gy;
          if (isSolid(g.words[i])) solid++;
          else if (changes.has(i)) covers++;
          else continue;
          marks.push(c);
        }
    if (!marks.length && !changes.size) continue;
    let runs = 0, bytes = 0;
    if (changes.size) {
      const after = g.words.slice();
      for (const [i, [, w]] of changes) after[i] = w;
      const r = voxelRuns(g.base, after);
      runs = r.length;
      if (runs > LIMITS.runsPerFrame) {
        out.skipped.push({ id: g.id, name: g.frame.name, why: `filling it would need ${runs} runs (at most ${LIMITS.runsPerFrame})` });
        continue;
      }
      bytes = runBytes(r) - runBytes(voxelRuns(g.base, g.words));
    }
    for (const [col, at] of marks) {
      let c = out.covered.get(col);
      if (!c) out.covered.set(col, (c = new Uint8Array(f.hi[col] - f.lo[col])));
      c[at] = 1;
    }
    out.fills.push({ grid: g, aligned, changes, covers, solid, runs, bytes });
  }
  return out;
}

// ------------------------------------------------------------------ packing

/** A box of field cells for one block: columns [x0, x1) x [z0, z1) and scene y [y0, y1), trimmed to the cells it needs.
 * deps are the boxes it stands on (a slice on the slice under it in its tile): it is kept only when they are. */
export interface Box {
  x0: number; x1: number; z0: number; z1: number;   // columns of the field, [x0, x1)
  y0: number; y1: number;                           // scene y, [y0, y1)
  voxels: number;
  deps: Box[];
}
export interface Piece extends Box {
  runs: number;
  bytes: number;
  deps: Piece[];
}

/** The game's cells a box costs: all of it. */
const cellsOf = (p: Omit<Box, "deps">) => (p.x1 - p.x0) * (p.y1 - p.y0) * (p.z1 - p.z0);

/** The cells a column needs from blocks in [y0, y1): first, end, count. */
function need(f: Field, cv: Canvas, col: number, y0: number, y1: number): [number, number, number] {
  const lo = Math.max(f.lo[col], y0), hi = Math.min(f.hi[col], y1);
  if (hi <= lo) return [0, 0, 0];
  const c = cv.covered.get(col);
  if (!c) return [lo, hi, hi - lo];
  let first = -1, end = 0, n = 0;
  for (let y = lo; y < hi; y++) {
    if (c[y - f.lo[col]]) continue;
    if (first < 0) first = y;
    end = y + 1;
    n++;
  }
  return n ? [first, end, n] : [0, 0, 0];
}

/** The box a part of a tile's slice needs, trimmed; null when it needs nothing. */
function trim(f: Field, cv: Canvas, x0: number, x1: number, z0: number, z1: number, y0: number, y1: number) {
  let bx0 = Infinity, bx1 = -Infinity, bz0 = Infinity, bz1 = -Infinity, by0 = Infinity, by1 = -Infinity, voxels = 0;
  for (let z = z0; z < z1; z++)
    for (let x = x0; x < x1; x++) {
      const [a, b, n] = need(f, cv, z * f.nx + x, y0, y1);
      if (!n) continue;
      voxels += n;
      if (x < bx0) bx0 = x;
      if (x + 1 > bx1) bx1 = x + 1;
      if (z < bz0) bz0 = z;
      if (z + 1 > bz1) bz1 = z + 1;
      if (a < by0) by0 = a;
      if (b > by1) by1 = b;
    }
  return voxels ? { x0: bx0, x1: bx1, z0: bz0, z1: bz1, y0: by0, y1: by1, voxels } : null;
}

function words(f: Field, cv: Canvas, p: Omit<Box, "deps">): Uint32Array {
  const X = p.x1 - p.x0, Y = p.y1 - p.y0, Z = p.z1 - p.z0;
  const w = new Uint32Array(X * Y * Z);
  for (let z = 0; z < Z; z++)
    for (let x = 0; x < X; x++) {
      const col = (p.z0 + z) * f.nx + p.x0 + x;
      const lo = Math.max(f.lo[col], p.y0), hi = Math.min(f.hi[col], p.y1);
      const c = cv.covered.get(col);
      for (let y = lo; y < hi; y++) if (!c?.[y - f.lo[col]]) w[(z * X + x) * Y + (y - p.y0)] = f.wordAt(col, y);
    }
  return w;
}

/** A box as the pieces it becomes: runs and bytes filled in, halved along its longer side while its runs pass the
 * limit. The pieces keep the box's deps (still boxes; pack() maps them to pieces). */
function finish(f: Field, cv: Canvas, box: Box, out: Piece[]) {
  const piece = { ...box } as Piece;
  const w = words(f, cv, piece);   // not kept: apply() builds them again for the blocks it adds
  const runs = voxelRuns(new Uint32Array(w.length), w);
  if (runs.length <= f.runLimit) {
    piece.runs = runs.length;
    piece.bytes = runBytes(runs) + OP_BYTES;
    out.push(piece);
    return;
  }
  const X = box.x1 - box.x0, Y = box.y1 - box.y0, Z = box.z1 - box.z0;
  const halves: [number, number, number, number, number, number][] = [];
  if (X >= Z && X > 1) { const m = box.x0 + (X >> 1); halves.push([box.x0, m, box.z0, box.z1, box.y0, box.y1], [m, box.x1, box.z0, box.z1, box.y0, box.y1]); }
  else if (Z > 1) { const m = box.z0 + (Z >> 1); halves.push([box.x0, box.x1, box.z0, m, box.y0, box.y1], [box.x0, box.x1, m, box.z1, box.y0, box.y1]); }
  else { const m = box.y0 + (Y >> 1); halves.push([box.x0, box.x1, box.z0, box.z1, box.y0, m], [box.x0, box.x1, box.z0, box.z1, m, box.y1]); }
  for (const h of halves) {
    const b = trim(f, cv, h[0], h[1], h[2], h[3], h[4], h[5]);
    if (b) finish(f, cv, { ...b, deps: box.deps }, out);
  }
}

/** A box cut in two across `axis` (0 x, 1 y, 2 z), each half trimmed; null when it is one cell thick there. */
function split(f: Field, cv: Canvas, b: Box, axis: 0 | 1 | 2): [Omit<Box, "deps">, Omit<Box, "deps">] | null {
  const lo = [b.x0, b.y0, b.z0][axis], hi = [b.x1, b.y1, b.z1][axis];
  if (hi - lo < 2) return null;
  const m = lo + ((hi - lo) >> 1);
  const r = (from: number, to: number) => {
    const x = axis === 0 ? [from, to] : [b.x0, b.x1], y = axis === 1 ? [from, to] : [b.y0, b.y1], z = axis === 2 ? [from, to] : [b.z0, b.z1];
    return trim(f, cv, x[0], x[1], z[0], z[1], y[0], y[1]);
  };
  const a = r(lo, m), c = r(m, hi);
  return a && c ? [a, c] : null;
}

/** How the field is cut into blocks before the halving: tiles (a fixed grid of 31 x 31 columns, the best of four offsets
 * each way), greedy (each tile starts at the first column still to place, scanning rows, so a ring or a sparse field is
 * covered by tiles that sit on its columns rather than on a grid), or best (both, keeping the fewer blocks, then cells). */
export type Packer = "tiles" | "greedy" | "best";

/** A tile's columns stacked in 31-high slices, each trimmed to the cells it needs, each on the one under it. */
function slices(f: Field, cv: Canvas, x0: number, x1: number, z0: number, z1: number, boxes: Box[]): number {
  const all = trim(f, cv, x0, x1, z0, z1, -Infinity, Infinity);
  if (!all) return 0;
  let under: Box | null = null, volume = 0;
  for (let y = all.y0; y < all.y1; y += SIDE) {
    const b = trim(f, cv, all.x0, all.x1, all.z0, all.z1, y, Math.min(all.y1, y + SIDE));
    if (!b) continue;
    const box: Box = { ...b, deps: under ? [under] : [] };
    boxes.push(box);
    under = box;
    volume += cellsOf(b);
  }
  return volume;
}

/** Tiles on a grid: the offset with the fewest boxes, then the least volume. */
function gridTiles(f: Field, cv: Canvas): { boxes: Box[]; volume: number } {
  let best: Box[] | null = null, bestVolume = Infinity;
  const offsets = [0, 8, 16, 24];
  for (const ox of f.nx > SIDE ? offsets : [0])
    for (const oz of f.nz > SIDE ? offsets : [0]) {
      const boxes: Box[] = [];
      let volume = 0;
      for (let tz = -oz; tz < f.nz; tz += SIDE)
        for (let tx = -ox; tx < f.nx && !(best && boxes.length > best.length); tx += SIDE) {
          const x0 = Math.max(0, tx), x1 = Math.min(f.nx, tx + SIDE), z0 = Math.max(0, tz), z1 = Math.min(f.nz, tz + SIDE);
          if (x0 >= x1 || z0 >= z1) continue;
          volume += slices(f, cv, x0, x1, z0, z1, boxes);
        }
      if (!best || boxes.length < best.length || (boxes.length === best.length && volume < bestVolume)) {
        best = boxes;
        bestVolume = volume;
      }
    }
  return { boxes: best ?? [], volume: bestVolume };
}

/** Tiles where the field is: scanning rows, a tile of 31 x 31 starts at the row of the first column still to place and,
 * across, as far left as 30 columns before it where its rows have columns to place, never over a tile placed before
 * (so tiles do not overlap, and every column is in one). */
function greedyTiles(f: Field, cv: Canvas): { boxes: Box[]; volume: number } {
  const todo = new Uint8Array(f.nx * f.nz);
  for (let c = 0; c < todo.length; c++) if (need(f, cv, c, -Infinity, Infinity)[2]) todo[c] = 1;
  const tiles: [number, number, number, number][] = [];   // x0, x1, z0, z1
  const boxes: Box[] = [];
  let volume = 0;
  for (let z = 0; z < f.nz; z++)
    for (let x = 0; x < f.nx; x++) {
      if (!todo[z * f.nx + x]) continue;
      const z0 = z, z1 = Math.min(f.nz, z + SIDE);
      let lo = 0, hi = f.nx;
      for (const [tx0, tx1, tz0, tz1] of tiles) {
        if (tz1 <= z0 || tz0 >= z1) continue;
        if (tx1 <= x) lo = Math.max(lo, tx1);
        else if (tx0 > x) hi = Math.min(hi, tx0);
      }
      let x0 = x;
      for (let xx = Math.max(lo, x - SIDE + 1); xx < x && x0 === x; xx++)
        for (let zz = z0; zz < z1; zz++) if (todo[zz * f.nx + xx]) { x0 = xx; break; }
      const x1 = Math.min(hi, x0 + SIDE);
      tiles.push([x0, x1, z0, z1]);
      for (let zz = z0; zz < z1; zz++) for (let xx = x0; xx < x1; xx++) todo[zz * f.nx + xx] = 0;
      volume += slices(f, cv, x0, x1, z0, z1, boxes);
    }
  return { boxes, volume };
}

/** The blocks for what the canvas leaves. Tiles of 31 x 31 columns (`packer`), stacked in 31-high slices trimmed to
 * their cells. While the boxes hold more than `cellRoom` cells and fewer than `slots` blocks are used, the box whose
 * halving saves the most cells is halved (in x, z or y, each half trimmed): a cone's flanks then cost their slope, not
 * their tile's whole height. */
export function pack(f: Field, cv: Canvas, slots = Infinity, cellRoom = Infinity, packer: Packer = "tiles"): Piece[] {
  let boxes: Box[];
  if (packer === "tiles") boxes = gridTiles(f, cv).boxes;
  else if (packer === "greedy") boxes = greedyTiles(f, cv).boxes;
  else {
    const g = gridTiles(f, cv), q = greedyTiles(f, cv);
    boxes = q.boxes.length < g.boxes.length || (q.boxes.length === g.boxes.length && q.volume < g.volume) ? q.boxes : g.boxes;
  }
  let total = boxes.reduce((n, b) => n + cellsOf(b), 0);
  const cuts = new Map<Box, { save: number; axis: 0 | 1 | 2; halves: [Omit<Box, "deps">, Omit<Box, "deps">] } | null>();
  while (total > cellRoom && boxes.length < slots) {
    let pick: Box | null = null;
    for (const b of boxes) {
      if (!cuts.has(b)) {
        let c: { save: number; axis: 0 | 1 | 2; halves: [Omit<Box, "deps">, Omit<Box, "deps">] } | null = null;
        for (const axis of [0, 2, 1] as const) {
          const h = split(f, cv, b, axis);
          const save = h ? cellsOf(b) - cellsOf(h[0]) - cellsOf(h[1]) : 0;
          if (h && save > (c?.save ?? 0)) c = { save, axis, halves: h };
        }
        cuts.set(b, c);
      }
      const c = cuts.get(b);
      if (c && (!pick || c.save > cuts.get(pick)!.save)) pick = b;
    }
    if (!pick) break;
    const { save, axis, halves } = cuts.get(pick)!;
    // Halves side by side stand on what the box stood on; in y the upper half stands on the lower. What stood on the box
    // now stands on both halves (in y, on the upper one).
    const lower: Box = { ...halves[0], deps: pick.deps };
    const upper: Box = { ...halves[1], deps: axis === 1 ? [lower] : pick.deps };
    const on = axis === 1 ? [upper] : [lower, upper];
    boxes = boxes.flatMap((b) => (b === pick ? [lower, upper] : [b]));
    for (const b of boxes) if (b.deps.includes(pick)) b.deps = b.deps.flatMap((d) => (d === pick ? on : [d]));
    total -= save;
  }
  const out: Piece[] = [], of = new Map<Box, Piece[]>();
  for (const b of boxes) {
    const n = out.length;
    finish(f, cv, b, out);
    of.set(b, out.slice(n));
  }
  for (const p of out) p.deps = (p.deps as Box[]).flatMap((d) => of.get(d) ?? []);
  return out;
}

/** Keeps at most `slots` pieces within `bytes` and `cells`: the most voxels first, a piece only when what it stands on is. */
export function choose(pieces: Piece[], slots: number, bytes: number, cells = Infinity): { kept: Piece[]; dropped: Piece[] } {
  if (pieces.length <= slots && pieces.reduce((n, p) => n + p.bytes, 0) <= bytes && pieces.reduce((n, p) => n + cellsOf(p), 0) <= cells)
    return { kept: pieces, dropped: [] };
  const kept = new Set<Piece>();
  let used = 0, usedCells = 0;
  for (;;) {
    let pick: Piece | null = null;
    for (const p of pieces)
      if (!kept.has(p) && used + p.bytes <= bytes && usedCells + cellsOf(p) <= cells && p.deps.every((q) => kept.has(q)) &&
        (!pick || p.voxels > pick.voxels)) pick = p;
    if (!pick || kept.size >= slots) break;
    kept.add(pick);
    used += pick.bytes;
    usedCells += cellsOf(pick);
  }
  return { kept: pieces.filter((p) => kept.has(p)), dropped: pieces.filter((p) => !kept.has(p)) };
}

// ------------------------------------------------------------------ the plan

export interface Plan {
  field: Field;
  canvas: Canvas;
  pieces: Piece[];
  kept: Piece[];
  dropped: Piece[];
  fits: boolean;
  slots: number;                            // blocks this call may add
  columns: number;
  voxels: number;                           // cells the field asks for
  why?: string;
  report: Record<string, unknown>;
}

function pieceInfo(f: Field, p: Piece) {
  const min: Vec3 = mapOf(f, [f.i0 + p.x0, p.y0, f.j0 + p.z0]), max: Vec3 = mapOf(f, [f.i0 + p.x1, p.y1, f.j0 + p.z1]);
  return { size: [p.x1 - p.x0, p.y1 - p.y0, p.z1 - p.z0], voxels: p.voxels, cells: cellsOf(p), runs: p.runs,
    map: { min: tidy(min), max: tidy(max) }, world: { min: tidy(min.map((v) => v * WORLD_PER_MAP) as Vec3), max: tidy(max.map((v) => v * WORLD_PER_MAP) as Vec3) } };
}

let suggesting = false;   // plan() re-plans at other voxel sizes (a suggestion, costAt), never from inside such a plan

/** `shape` reshapes the sampled field before it is packed (heightfield_drape's core: the columns under its skin). */
export function plan(store: EditorStore, a: FieldArgs, shape?: (f: Field) => void): Plan {
  const f = sampleField(store, a);
  shape?.(f);
  const cv = canvas(store, f, a.reuseFrames ?? "aligned");

  let columns = 0, voxels = 0, top = -Infinity;
  for (let c = 0; c < f.lo.length; c++) if (f.hi[c] > f.lo[c]) { columns++; voxels += f.hi[c] - f.lo[c]; top = Math.max(top, f.hi[c]); }

  const patch = store.patch();
  const patchBytes = JSON.stringify(patch).length;
  let runVoxels = 0;
  for (const op of patch.ops) if (op.op === "voxels") for (const r of op.runs) runVoxels += r[1];
  const usedBlocks = blockFrames(store.scene).length;
  const free = LIMITS.newFrames - usedBlocks, frameRoom = LIMITS.frames - store.scene.frames.length;
  const slots = f.sceneId === null ? 0 : Math.max(0, Math.min(free, frameRoom, a.maxBlocks ?? Infinity));
  const opsRoom = LIMITS.ops - patch.ops.length - cv.fills.filter((x) => x.changes.size && !x.grid.frame.new && x.grid.words.every((w, i) => w === x.grid.base[i])).length;
  const fillBytes = cv.fills.reduce((n, x) => n + x.bytes, 0);
  const byteRoom = PATCH_BUDGET - patchBytes - fillBytes;
  const cells = cellBudget(store.scene);
  const blockRoom = Math.min(slots, Math.floor(opsRoom / 2)), cellRoom = Math.max(0, Math.min(cells.free, a.maxCells ?? Infinity));
  const pieces = pack(f, cv, blockRoom, cellRoom, a.packer ?? "tiles");
  const { kept, dropped } = choose(pieces, blockRoom, byteRoom, cellRoom);
  const neededCells = pieces.reduce((n, p) => n + cellsOf(p), 0), keptCells = kept.reduce((n, p) => n + cellsOf(p), 0);

  const blockVoxels = pieces.reduce((n, p) => n + p.voxels, 0), keptVoxels = kept.reduce((n, p) => n + p.voxels, 0);
  const fillVoxels = cv.fills.reduce((n, x) => n + x.covers, 0), solidVoxels = cv.fills.reduce((n, x) => n + x.solid, 0);
  const frameCells = cv.fills.reduce((n, x) => n + x.changes.size, 0);
  const keptBytes = kept.reduce((n, p) => n + p.bytes, 0);
  const runVoxelsAfter = runVoxels + keptVoxels + frameCells;
  let why: string | undefined;
  const blocks = (n: number) => `${n} block${n === 1 ? "" : "s"}`;
  // What binds, the cell budget first: while it binds the packer spends blocks halving boxes to save cells, so a block
  // count past the limit is then its consequence rather than the cause.
  const limitReasons = () => {
    const cellsBind = neededCells > cellRoom;
    const out = [
      ...(cellsBind ? [`cell budget exceeded (${neededCells} needed, ${cellRoom} allowed): the blocks' boxes hold ${neededCells} voxel cells where the ` +
        `level has ${cells.free} of the game's ${cells.max} left (its frames hold ${cells.used}${a.maxCells !== undefined ? `, maxCells ${a.maxCells}` : ""}); ` +
        `${neededCells} is this packing's after halving boxes with up to ${blockRoom} blocks, not the least the field can cost (more blocks, a coarser voxelSize or fitBudget change it)`] : []),
      ...(pieces.length > blockRoom || !cellsBind ? [`the field needs ${blocks(pieces.length)} and ${slots} may be added (${usedBlocks} of ${LIMITS.newFrames} ` +
        `already used${a.maxBlocks !== undefined ? `, maxBlocks ${a.maxBlocks}` : ""})`] : []),
      ...(pieces.reduce((n, p) => n + p.bytes, 0) > byteRoom ? ["their runs pass the patch's size budget"] : []),
    ];
    return out.join("; ");
  };
  if (dropped.length) {
    const lostTop = Math.max(...dropped.map((p) => p.y1));
    why = f.sceneId === null
      ? `this level has no Scene frame, so no block can be added, and the frames that overlap the field leave ${blockVoxels} of its ${voxels} voxels ` +
        `(${blocks(pieces.length)} worth)` + ((a.reuseFrames ?? "aligned") === "aligned" ? `: reuseFrames: "any" also fills the rotated and scaled frames` : "") +
        ", or pass partial: true to fill only what the frames can take."
      : limitReasons() +
        `. Without ${blocks(dropped.length)} ${blockVoxels - keptVoxels} voxels are missing` +
        (lostTop >= top ? `, up to the peak (map y ${num(lostTop * f.s + f.t[1])})` : "") +
        ". Shrink the rectangle, lower the heights or baseDepth, use a larger voxelSize (a cell costs the same and covers more), use fewer material bands (each band crossing a column costs a run, and a block over 2000 runs is split), or pass partial: true to place the blocks holding the most voxels.";
  }
  if (runVoxelsAfter > LIMITS.runVoxels) why = `the patch's runs would cover ${runVoxelsAfter} voxels (at most ${LIMITS.runVoxels})`;
  const fits = !why || (!!a.partial && runVoxelsAfter <= LIMITS.runVoxels);
  // When the cells bind: the smallest voxelSize (in steps of 0.25) at which the same field fits whole.
  let suggest: { voxelSize: number; cells: number; blocks: number } | null | undefined;
  if (dropped.length && neededCells > cellRoom && f.sceneId !== null && !suggesting) {
    suggesting = true;
    suggest = null;
    try {
      for (let v = Math.floor(f.s * 4 + 1e-9) / 4 + 0.25; v <= LIMITS.newFrameScale.max + 1e-9; v += 0.25) {
        const q = plan(store, { ...a, voxelSize: v, partial: false });
        if (!q.dropped.length && !q.why) {
          suggest = { voxelSize: v, cells: q.kept.reduce((n, p) => n + cellsOf(p), 0), blocks: q.kept.length };
          break;
        }
      }
    } catch {
      // a voxel size the field cannot take (a span or a height cap): no suggestion from there on
    } finally {
      suggesting = false;
    }
    if (why) why += suggest ? ` At voxelSize ${suggest.voxelSize} the same field fits whole: ${blocks(suggest.blocks)} of ${suggest.cells} cells.`
      : ` No voxelSize up to ${LIMITS.newFrameScale.max} fits it whole: shrink the rectangle, lower the heights, or raise maxCells.`;
  }

  // costAt: the same field planned at other voxel sizes, so the cost of finer voxels on a part is known before it is built.
  let costs: object[] | undefined;
  if (a.costAt?.length && !suggesting) {
    suggesting = true;
    try {
      costs = [...new Set(a.costAt)].slice(0, 8).map((v) => {
        try {
          const q = plan(store, { ...a, voxelSize: v, partial: false, costAt: undefined });
          return { voxelSize: v, fits: q.fits && !q.dropped.length, blocks: q.pieces.length, cells: q.pieces.reduce((n, pc) => n + cellsOf(pc), 0),
            voxels: q.voxels, ...(q.why ? { why: q.why.split(". ")[0] } : {}) };
        } catch (e) {
          return { voxelSize: v, error: e instanceof Error ? e.message : String(e) };
        }
      });
    } finally {
      suggesting = false;
    }
  }
  const toW = (v: number) => num(v * WORLD_PER_MAP);
  const rect = { min: mapOf(f, [f.i0, f.floor, f.j0]), max: mapOf(f, [f.i0 + f.nx, top === -Infinity ? f.floor : top, f.j0 + f.nz]) };
  const notes: string[] = [];
  if ((a.materials ?? []).some((r) => r.surface !== undefined) && columns > SURFACE_NOTE_COLUMNS)
    notes.push(`a surface rule paints a skin of another material over ${columns} columns: in a test a one-voxel skin over 6954 columns made the game's ` +
      "landscape combiner run out of vertices (CombineLandscapeAction asserts, holes in the drawn terrain) where height bands over the same island did not; " +
      "on a large field prefer minHeight/maxHeight or slope bands, and Test the level");
  // Base frames the field overlaps but aligned reuse left out: filling them costs no block and no cells.
  const reuse = a.reuseFrames ?? "aligned";
  if (reuse === "aligned" && pieces.length && cv.skipped.some((x) => /rotated|scaled|lattice/.test(x.why)) && !suggesting) {
    const any = canvas(store, f, "any");
    const free = any.fills.filter((x) => !x.aligned).reduce((n, x) => n + x.covers, 0);
    if (free > 0) notes.push(`${any.fills.filter((x) => !x.aligned).length} base frames the field overlaps could take ${free} of its voxels at no block or cell cost with ` +
      `reuseFrames: "any" (filled cell by cell, so their surface follows the field to within one of their own voxels)`);
  }
  if ((a.packer ?? "tiles") === "tiles" && pieces.length > 4 && !suggesting) {
    const g = pack(f, cv, blockRoom, cellRoom, "greedy");
    if (g.length < pieces.length) notes.push(`packer: "greedy" cuts this field into ${g.length} blocks (${g.reduce((n, pc) => n + cellsOf(pc), 0)} cells) instead of ${pieces.length}`);
  }
  const report = {
    fits, ...(why ? { why } : {}), ...(notes.length ? { notes } : {}),
    ...(suggest !== undefined ? { suggestedVoxelSize: suggest ?? "none up to 4 fits the field whole" } : {}),
    ...(costs ? { costAt: costs } : {}),
    region: { voxelSize: f.s, edges: a.edges ?? "round", packer: a.packer ?? "tiles", columns: f.nx * f.nz, size: [f.nx, f.nz], solidColumns: columns, map: { min: tidy(rect.min), max: tidy(rect.max) },
      world: { min: tidy(rect.min.map((v) => v * WORLD_PER_MAP) as Vec3), max: tidy(rect.max.map((v) => v * WORLD_PER_MAP) as Vec3) },
      floor: { map: num(f.floor * f.s + f.t[1]), world: toW(f.floor * f.s + f.t[1]), ...floorReport(f) }, peakVoxels: columns ? top - f.floor : 0,
      ...(f.limited ? { slopeLimit: { relax: a.relax ?? 0, ...(a.maxGradient !== undefined ? { maxGradient: a.maxGradient } : {}), passes: f.limited.passes, steepestDegrees: Math.round(f.limited.steepest * 10) / 10 } } : {}) },
    voxels: { field: voxels, alreadySolid: solidVoxels, inFrames: fillVoxels, inBlocks: keptVoxels, dropped: blockVoxels - keptVoxels },
    frames: cv.fills.map((x) => ({ id: x.grid.id, name: x.grid.frame.name, ...(x.aligned ? {} : { approximate: true }), cellsFilled: x.changes.size, covers: x.covers, alreadySolid: x.solid, runs: x.runs })),
    ...(cv.skipped.length ? { framesSkipped: { count: cv.skipped.length,
      why: cv.skipped.reduce((o, x) => ({ ...o, [x.why]: (o[x.why] ?? 0) + 1 }), {} as Record<string, number>), list: cv.skipped.slice(0, 20) } } : {}),
    blocks: { needed: pieces.length, placed: kept.length, scale: f.s, cells: keptCells, runs: kept.reduce((n, p) => n + p.runs, 0), list: kept.map((p) => pieceInfo(f, p)) },
    ...(dropped.length ? { dropped: { count: dropped.length, voxels: blockVoxels - keptVoxels, list: dropped.slice(0, 100).map((p) => pieceInfo(f, p)) } } : {}),
    budget: {
      blocks: { before: usedBlocks, after: usedBlocks + (fits ? kept.length : 0), max: LIMITS.newFrames, left: LIMITS.newFrames - usedBlocks - (fits ? kept.length : 0) },
      cells: { before: cells.used, needed: neededCells, after: cells.used + (fits ? keptCells : 0), max: cells.max, left: cells.free - (fits ? keptCells : 0),
        note: "the game counts X*Y*Z of every voxel frame (each block's whole box) and crashes loading more than max" },
      patchBytes: { before: patchBytes, after: patchBytes + (fits ? fillBytes + keptBytes : 0), budget: PATCH_BUDGET, note: "estimated" },
      runVoxels: { after: runVoxelsAfter, max: LIMITS.runVoxels },
    },
  };
  return { field: f, canvas: cv, pieces, kept, dropped, fits, slots, columns, voxels, why, report };
}

/** plan(), and with fitBudget the same field at the smallest voxelSize (from the one asked, in steps of 0.25, up to 4)
 * whose plan fits whole within maxBlocks and maxCells: blocks and cells are then chosen by the voxel size instead of by
 * halving boxes until the 64 blocks are spent. The answer's `fitted` says which size was taken and what each one tried
 * cost. Without fitBudget, or when the size asked fits, it is plan() as it is. */
export function planFit(store: EditorStore, a: FieldArgs, shape?: (f: Field) => void): Plan {
  const p = plan(store, a, shape);
  if (!a.fitBudget) return p;
  const asked = a.voxelSize ?? 1;
  if (p.fits && !p.dropped.length) {
    p.report = { fitted: { asked, voxelSize: asked, note: "the size asked fits within the budget" }, ...p.report };
    return p;
  }
  const tried: object[] = [{ voxelSize: asked, blocks: p.pieces.length, cells: p.pieces.reduce((n, pc) => n + cellsOf(pc), 0), fits: false }];
  for (let v = Math.floor(asked * 4 + 1e-9) / 4 + 0.25; v <= LIMITS.newFrameScale.max + 1e-9; v += 0.25) {
    let q: Plan;
    try {
      q = plan(store, { ...a, voxelSize: v, partial: false, costAt: undefined }, shape);
    } catch (e) {
      tried.push({ voxelSize: v, error: e instanceof Error ? e.message : String(e) });
      continue;
    }
    const whole = q.fits && !q.dropped.length;
    tried.push({ voxelSize: v, blocks: q.pieces.length, cells: q.pieces.reduce((n, pc) => n + cellsOf(pc), 0), fits: whole });
    if (whole) {
      q.report = { fitted: { asked, voxelSize: v, tried }, ...q.report };
      return q;
    }
  }
  p.report = { fitted: { asked, voxelSize: null, tried, note: `no voxelSize up to ${LIMITS.newFrameScale.max} fits the field whole within the budget: shrink it, raise maxBlocks or maxCells, or pass partial` }, ...p.report };
  return p;
}

// ------------------------------------------------------------------ apply

let strokes = -1_000_000;

/** Applies a plan that fits as one undo step: the frame fills, then the blocks. */
/** `extra` adds commands to the same undo step, given the block ids and names taken so far (heightfield_drape's skin). */
export function apply(store: EditorStore, p: Plan, extra?: (ids: Set<number>, names: Set<string>) => Command[]): { blocks: { id: number; name: string }[] } {
  if (!p.fits) throw new ToolError(p.why ?? "the heightfield does not fit");
  const names = new Set(store.scene.frames.map((fr) => fr.name.toLowerCase()));
  const ids = new Set(blockFrames(store.scene).map((fr) => fr.id));
  const { cmds, blocks: made } = planCommands(store, p, { ids, names });
  if (extra) cmds.push(...extra(ids, names));
  if (!cmds.length) throw new ToolError(p.why ? `nothing could be placed: ${p.why}` : "the field is already solid: nothing to place");
  const before = new Set(store.problems());
  store.exec(new Group("Heightfield", cmds));
  const fresh = store.problems().filter((x) => !before.has(x));
  if (fresh.length) {
    store.undo();
    throw Object.assign(new ToolError("the heightfield would make the patch invalid, so it was not applied"), { details: fresh });
  }
  return { blocks: made };
}

/** A plan's commands (the frame fills, then its blocks), not yet run: `taken` holds the block ids and names used so far
 * and gets the new ones, so several plans go into one undo step (heightfield_drape's refine: a coarse skin and a fine
 * one along the trails). */
export function planCommands(store: EditorStore, p: Plan, taken: { ids: Set<number>; names: Set<string> }): { cmds: Command[]; blocks: { id: number; name: string }[] } {
  const f = p.field;
  const cmds: Command[] = [];
  const changes = new Map<number, Map<number, [number, number]>>();
  for (const x of p.canvas.fills) if (x.changes.size) changes.set(x.grid.ref, new Map(x.changes));
  if (changes.size) cmds.push(new SculptStroke("Heightfield", --strokes, store.voxels, changes, () => {}));
  const { ids, names } = taken;
  const made: { id: number; name: string }[] = [];
  let n = 0, tmp = -1;
  for (const pc of p.kept) {
    while (names.has(`ergblock_${n}`)) n++;
    while (ids.has(tmp)) tmp--;
    if (tmp < -LIMITS.newFrames) throw new ToolError(`a level holds at most ${LIMITS.newFrames} added blocks`);
    const name = `ergblock_${n}`;
    names.add(name);
    ids.add(tmp);
    const size: Vec3 = [pc.x1 - pc.x0, pc.y1 - pc.y0, pc.z1 - pc.z0], s = f.s;
    const frame: Frame = { id: tmp, new: true, parent: f.sceneId!, name,
      pos: [(f.i0 + pc.x0 + size[0] / 2) * s, (pc.y0 + size[1] / 2) * s, (f.j0 + pc.z0 + size[2] / 2) * s],
      rot: [0, 0, 0], scale: [s, s, s], size, voxels: store.freshRef(), heightMap: null, folder: false };
    cmds.push(new AddFrame(store.voxels, frame, words(f, p.canvas, pc)));
    made.push({ id: tmp, name });
  }
  return { cmds, blocks: made };
}
