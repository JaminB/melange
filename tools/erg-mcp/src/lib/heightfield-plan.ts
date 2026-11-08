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
  AddFrame, Group, LIMITS, SculptStroke, blockFrames, cellBudget, filled, frameWorld, isSolid, sceneFrame, validNewFrameScale, voxelRuns,
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
  noise?: { threshold?: number; scale?: number; seed?: number; octaves?: number };   // only where noise is above threshold
}

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
  runLimit: number;                         // runs a new block may hold: the limit less runsHeadroom
  lo: Int32Array; hi: Int32Array;           // per column [lo, hi): hi <= lo is no column
  height: Float32Array;                     // per column, voxels above the floor (after crater, cap)
  slope: Float32Array;                      // per column, degrees
  materialAt(col: number, y: number): number;
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
  const floor = Math.round((a.floor * k - t[1]) / s);
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
    !exclude.some((ar) => [-1, 1].every((dx) => [-1, 1].every((dz) => inArea(ar, mx + (dx * s) / 2, mz + (dz * s) / 2))));
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
      const hv = Math.max(0, h * kv);
      if (hv > MAX_COLUMN) throw new ToolError(`the height reaches ${num(h)} (${Math.round(hv)} voxels) at x ${num(mx / k)}, z ${num(mz / k)}: at most ${MAX_COLUMN} voxels; pass maxHeight`);
      const c = b * nx + a2, top = edges === "round" ? Math.round(hv) : Math.floor(hv + 1 - threshold(i0 + a2, j0 + b));
      height[c] = hv;
      if (top >= 1 && kept(mx, mz)) {
        lo[c] = floor - depth;
        hi[c] = floor + top;
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
  const ruleMask = rules.map((r, i) => {
    if (r.mask === undefined) return null;
    try {
      return compile(r.mask, MASK_VARIABLES) as unknown as (v: Record<string, number>) => number;
    } catch (e) {
      if (e instanceof ExprError) throw new ToolError(`materials[${i}].mask: ${e.message}`);
      throw e;
    }
  });
  const mvars: Record<string, number> = { ...vars, h: 0, y: 0, depth: 0, slope: 0 };
  const centreOf = (col: number): [number, number] => { const ai = col % nx, bj = (col - ai) / nx; return [(i0 + ai + 0.5) * s + t[0], (j0 + bj + 0.5) * s + t[2]]; };
  // The top of the neighbouring column (its hi; the floor where it has none), for exposed: a voxel is exposed when
  // the column's top is it, or a neighbour's top is at or under it (air beside it), the field's edge included.
  const topOf = (ai: number, bj: number) => {
    if (ai < 0 || bj < 0 || ai >= nx || bj >= nz) return -Infinity;
    const c = bj * nx + ai;
    return hi[c] > lo[c] ? hi[c] : -Infinity;
  };
  const materialAt = (col: number, y: number) => {
    if (!rules.length) return material;
    const above = (y + 0.5 - floor) / kv, fromTop = hi[col] - 1 - y, sl = slope[col], depth = (hi[col] - y - 0.5) / kv;
    let exposed: boolean | undefined;
    for (const r of rules) {
      let shift = 0;
      if (r.jitter && (r.minHeight !== undefined || r.maxHeight !== undefined)) {
        const ai = col % nx, bj = (col - ai) / nx, sc = r.jitter.scale ?? 8;
        // the column's centre in region units, so the bands wander the same way whatever the voxel size
        shift = r.jitter.amount * noise2(((i0 + ai + 0.5) * s + t[0]) / k / sc, ((j0 + bj + 0.5) * s + t[2]) / k / sc, r.jitter.seed ?? 0);
      }
      if (r.minHeight !== undefined && above < r.minHeight + shift) continue;
      if (r.maxHeight !== undefined && above >= r.maxHeight + shift) continue;
      if (r.minSlope !== undefined && sl < r.minSlope) continue;
      if (r.maxSlope !== undefined && sl > r.maxSlope) continue;
      if (r.surface !== undefined && fromTop >= r.surface) continue;
      if (r.minDepth !== undefined && depth < r.minDepth) continue;
      if (r.maxDepth !== undefined && depth >= r.maxDepth) continue;
      if (r.exposed !== undefined) {
        if (exposed === undefined) {
          const ai = col % nx, bj = (col - ai) / nx;
          exposed = fromTop === 0 || [[1, 0], [-1, 0], [0, 1], [0, -1]].some(([dx, dz]) => topOf(ai + dx, bj + dz) <= y);
        }
        if (exposed !== r.exposed) continue;
      }
      const ri = rules.indexOf(r);
      if (ruleRegion[ri] || ruleMask[ri] || r.noise) {
        const [mx, mz] = centreOf(col);
        if (ruleRegion[ri] && !ruleRegion[ri]!.some((ar) => inArea(ar, mx, mz))) continue;
        if (r.noise) {
          const sc = r.noise.scale ?? 8, oc = r.noise.octaves ?? 1, nx2 = mx / k / sc, nz2 = mz / k / sc;
          const v = oc > 1 ? fbm(nx2, nz2, r.noise.seed ?? 0, oc) : noise2(nx2, nz2, r.noise.seed ?? 0);
          if (!(v > (r.noise.threshold ?? 0))) continue;
        }
        if (ruleMask[ri]) {
          mvars.x = (mx - cx) / k; mvars.z = (mz - cz) / k;
          mvars.u = ((mx - t[0]) / s - i0) / nx; mvars.v = ((mz - t[2]) / s - j0) / nz;
          mvars.r = Math.hypot(mvars.x, mvars.z); mvars.theta = Math.atan2(mvars.z, mvars.x);
          mvars.h = (hi[col] - floor) / kv; mvars.y = above; mvars.depth = depth; mvars.slope = sl;
          if (!(ruleMask[ri]!(mvars) > 0)) continue;
        }
      }
      return r.material;
    }
    return material;
  };
  const runLimit = Math.max(100, LIMITS.runsPerFrame - Math.max(0, Math.round(a.runsHeadroom ?? 0)));
  return { k, s, t, sceneId, i0, j0, nx, nz, floor, runLimit, lo, hi, height, slope, materialAt };
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
          changes.set(i, [w, filled(f.materialAt(c[0], sy))]);
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
      for (let y = lo; y < hi; y++) if (!c?.[y - f.lo[col]]) w[(z * X + x) * Y + (y - p.y0)] = filled(f.materialAt(col, y));
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
      floor: { map: num(f.floor * f.s + f.t[1]), world: toW(f.floor * f.s + f.t[1]) }, peakVoxels: columns ? top - f.floor : 0 },
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
  const f = p.field;
  const cmds: Command[] = [];
  const changes = new Map<number, Map<number, [number, number]>>();
  for (const x of p.canvas.fills) if (x.changes.size) changes.set(x.grid.ref, new Map(x.changes));
  if (changes.size) cmds.push(new SculptStroke("Heightfield", --strokes, store.voxels, changes, () => {}));
  const names = new Set(store.scene.frames.map((fr) => fr.name.toLowerCase()));
  const ids = new Set(blockFrames(store.scene).map((fr) => fr.id));
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
