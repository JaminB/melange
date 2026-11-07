// Heightfield terrain: a height per column over a rectangle of the level, filled from a floor up, then packed into the
// voxels the patch can carry. Planning is pure (nothing changes until apply()).
//
// The lattice. Added blocks sit under the Scene frame with their corners on its whole units (blocks.ts), so the plan
// works in Scene-frame space: cell (x, y, z) fills [x, x+1) x [y, y+1) x [z, z+1) there, and the Scene frame must be
// translation-only in the level (it is on the game's maps). Map = scene + t.
//
// The budget. A patch adds at most 64 blocks of at most 32 voxels a side (2,097,152 cells in all) with at most 2000
// runs each, so the plan:
//   1. fills what it can in voxel frames that overlap the field (no SLIPPY/PERM/TEAMBASE in the name): a voxels op on a
//      frame costs no block, and the server takes any valid word for any cell of a base frame, empty or not. By default
//      only frames on the lattice (translation-only, unit scale, whole-unit offset: earlier blocks; no base frame of the
//      game's maps is) take part; "any" also fills rotated and scaled frames (not scaled under 0.9) cell by cell, which
//      only approximates the field at its surface. Cells already solid there count as done. A frame whose runs would
//      pass 2000 is left out.
//   2. tiles the rest in 32x32 columns (trying four offsets each way, keeping the fewest blocks), stacks 32-high slices
//      in each tile from its lowest needed cell, and trims every slice to the box of the cells it needs, so a cone's
//      upper slices are small. A slice whose runs pass 2000 (material bands add a run per band per column) is halved
//      along its longer side until it fits.
//   3. when the blocks do not fit the budget (64 minus those already added, or maxBlocks), refuses, or with partial
//      keeps the blocks holding the most voxels, a slice only when the slice under it in its tile is kept (nothing floats),
//      and lists what was dropped.
import {
  AddFrame, Group, LIMITS, SculptStroke, blockFrames, filled, frameWorld, isSolid, sceneFrame, voxelRuns, type EditorStore,
  type Frame, type Vec3, type Command,
} from "../core";
import { ToolError } from "../registry";
import { PATCH_BUDGET, WORLD_PER_MAP, grids, runBytes, tidy, type Grid } from "./voxel";
import { compile, ExprError, type Vars } from "./heightfield-expr";

export type Units = "map" | "world";

export interface MaterialRule {
  material: number;
  minHeight?: number; maxHeight?: number;   // the voxel's height above the floor, in the region's units
  minSlope?: number; maxSlope?: number;     // the column's slope, degrees
  surface?: number;                         // only the top n voxels of a column
}

export interface Crater { radius: number; depth: number; x?: number; z?: number; rim?: number; }

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
}

export const SIDE = LIMITS.newFrameSide;
const MAX_COLUMNS = 512 * 512, MAX_SPAN = 1024, MAX_COLUMN = 1024;
const OP_BYTES = 160;   // an addFrame op and its voxels op's envelope, JSON

/** The sampled field: one column per lattice cell of the rectangle. */
export interface Field {
  k: number;                                // map units per region unit
  t: Vec3;                                  // Scene frame -> map translation
  sceneId: number | null;                   // null: no Scene frame, no blocks
  i0: number; j0: number; nx: number; nz: number;
  floor: number;                            // floor cell (scene y)
  lo: Int32Array; hi: Int32Array;           // per column [lo, hi): hi <= lo is no column
  height: Float32Array;                     // per column, voxels above the floor (after crater, cap)
  slope: Float32Array;                      // per column, degrees
  materialAt(col: number, y: number): number;
}

const num = (v: number) => tidy([v, 0, 0])[0];
const mapOf = (f: Field, s: Vec3): Vec3 => [s[0] + f.t[0], s[1] + f.t[1], s[2] + f.t[2]];

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
  const { id: sceneId, t } = sceneLattice(store);
  if (!!a.expr === !!a.grid) throw new ToolError("give the height as either expr or grid");
  const i0 = Math.round(Math.min(a.min[0], a.max[0]) * k - t[0]), i1 = Math.round(Math.max(a.min[0], a.max[0]) * k - t[0]);
  const j0 = Math.round(Math.min(a.min[1], a.max[1]) * k - t[2]), j1 = Math.round(Math.max(a.min[1], a.max[1]) * k - t[2]);
  const nx = i1 - i0, nz = j1 - j0;
  if (nx < 1 || nz < 1) throw new ToolError("the rectangle is less than one voxel wide (1 voxel = 1 map unit = 20 world units)");
  if (nx > MAX_SPAN || nz > MAX_SPAN || nx * nz > MAX_COLUMNS)
    throw new ToolError(`the rectangle is ${nx}x${nz} voxels: at most ${MAX_SPAN} a side and ${MAX_COLUMNS} columns (64 blocks cover at most 256x256 at 32 high)`);
  const floor = Math.round(a.floor * k - t[1]);
  const depth = Math.max(0, Math.round((a.baseDepth ?? 0) * k));
  const material = a.material ?? 0;
  checkMaterial(material, "material");
  (a.materials ?? []).forEach((r, i) => checkMaterial(r.material, `materials[${i}].material`));

  // Region coordinates (units): x, z from the centre.
  const cx = (i0 + i1) / 2 + t[0], cz = (j0 + j1) / 2 + t[2];
  const W = nx / k, D = nz / k;
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
  const n = nx * nz;
  const lo = new Int32Array(n), hi = new Int32Array(n), height = new Float32Array(n), slope = new Float32Array(n);
  const vars: Vars = { x: 0, z: 0, u: 0, v: 0, r: 0, theta: 0, W, D, R: Math.min(W, D) / 2, pi: Math.PI, e: Math.E };
  for (let b = 0; b < nz; b++)
    for (let a2 = 0; a2 < nx; a2++) {
      const mx = i0 + a2 + 0.5 + t[0], mz = j0 + b + 0.5 + t[2];
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
      const hv = Math.max(0, h * k);
      if (hv > MAX_COLUMN) throw new ToolError(`the height reaches ${num(h)} (${Math.round(hv)} voxels) at x ${num(mx / k)}, z ${num(mz / k)}: at most ${MAX_COLUMN} voxels; pass maxHeight`);
      const c = b * nx + a2, top = Math.round(hv);
      height[c] = hv;
      if (top >= 1) {
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
  const materialAt = (col: number, y: number) => {
    if (!rules.length) return material;
    const above = (y + 0.5 - floor) / k, fromTop = hi[col] - 1 - y, s = slope[col];
    for (const r of rules) {
      if (r.minHeight !== undefined && above < r.minHeight) continue;
      if (r.maxHeight !== undefined && above >= r.maxHeight) continue;
      if (r.minSlope !== undefined && s < r.minSlope) continue;
      if (r.maxSlope !== undefined && s > r.maxSlope) continue;
      if (r.surface !== undefined && fromTop >= r.surface) continue;
      return r.material;
    }
    return material;
  };
  return { k, t, sceneId, i0, j0, nx, nz, floor, lo, hi, height, slope, materialAt };
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
 * lattice (translation-only, unit scale, whole-unit offset) the two are the same cells. Other frames ("any") are props
 * placed at any angle and scale, so their cells only approximate the field's (up to half a scaled voxel at the surface). */
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
  for (const g of grids(store)) {
    const m = g.toMap, inv = g.fromMap;
    const x0 = Math.max(f.i0, Math.floor(g.bounds.min[0] - f.t[0])), x1 = Math.min(f.i0 + f.nx, Math.ceil(g.bounds.max[0] - f.t[0]));
    const z0 = Math.max(f.j0, Math.floor(g.bounds.min[2] - f.t[2])), z1 = Math.min(f.j0 + f.nz, Math.ceil(g.bounds.max[2] - f.t[2]));
    const y0 = Math.max(yMin, Math.floor(g.bounds.min[1] - f.t[1])), y1 = Math.min(yMax, Math.ceil(g.bounds.max[1] - f.t[1]));
    if (x0 >= x1 || z0 >= z1 || y0 >= y1) continue;
    const unit = [m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]].every((v, i) => Math.abs(v - IDENTITY[i]) <= 1e-6);
    const whole = [m[3] - f.t[0], m[7] - f.t[1], m[11] - f.t[2]].every((v) => Math.abs(v - Math.round(v)) <= 1e-4);
    const aligned = unit && whole;
    if (!aligned && reuse !== "any") {
      out.skipped.push({ id: g.id, name: g.frame.name, why: unit ? "not on whole units of the Scene frame" : "rotated or scaled" });
      continue;
    }
    if (!aligned && [0, 1, 2].some((j) => Math.hypot(m[j], m[4 + j], m[8 + j]) < 0.9)) {
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
          const sy = Math.floor(py), c = cellOf(Math.floor(px), sy, Math.floor(pz));
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
          const px = sx + 0.5 + f.t[0], py = sy + 0.5 + f.t[1], pz = sz + 0.5 + f.t[2];
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

export interface Piece {
  x0: number; x1: number; z0: number; z1: number;   // columns of the field, [x0, x1)
  y0: number; y1: number;                           // scene y, [y0, y1)
  voxels: number;
  runs: number;
  bytes: number;
  stack: number; level: number;                     // its tile and slice: a slice needs the one under it
}

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

function words(f: Field, cv: Canvas, p: Piece): Uint32Array {
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

/** Fills in runs and bytes; halves a piece along its longer side while its runs pass the limit. */
function finish(f: Field, cv: Canvas, p: Omit<Piece, "runs" | "bytes">, out: Piece[]) {
  const piece = p as Piece;
  const w = words(f, cv, piece);   // not kept: apply() builds them again for the blocks it adds
  const runs = voxelRuns(new Uint32Array(w.length), w);
  if (runs.length <= LIMITS.runsPerFrame) {
    piece.runs = runs.length;
    piece.bytes = runBytes(runs) + OP_BYTES;
    out.push(piece);
    return;
  }
  const X = p.x1 - p.x0, Y = p.y1 - p.y0, Z = p.z1 - p.z0;
  const halves: [number, number, number, number, number, number][] = [];
  if (X >= Z && X > 1) { const m = p.x0 + (X >> 1); halves.push([p.x0, m, p.z0, p.z1, p.y0, p.y1], [m, p.x1, p.z0, p.z1, p.y0, p.y1]); }
  else if (Z > 1) { const m = p.z0 + (Z >> 1); halves.push([p.x0, p.x1, p.z0, m, p.y0, p.y1], [p.x0, p.x1, m, p.z1, p.y0, p.y1]); }
  else { const m = p.y0 + (Y >> 1); halves.push([p.x0, p.x1, p.z0, p.z1, p.y0, m], [p.x0, p.x1, p.z0, p.z1, m, p.y1]); }
  for (const h of halves) {
    const b = trim(f, cv, h[0], h[1], h[2], h[3], h[4], h[5]);
    if (b) finish(f, cv, { ...b, stack: p.stack, level: p.level }, out);
  }
}

/** The blocks for what the canvas leaves: the tiling offset with the fewest pieces, then the least volume. */
export function pack(f: Field, cv: Canvas): Piece[] {
  type Box = { x0: number; x1: number; z0: number; z1: number; y0: number; y1: number; voxels: number; stack: number; level: number };
  let best: Box[] | null = null, bestVolume = Infinity;
  const offsets = [0, 8, 16, 24];
  for (const ox of f.nx > SIDE ? offsets : [0])
    for (const oz of f.nz > SIDE ? offsets : [0]) {
      const boxes: Box[] = [];
      let volume = 0, stack = 0;
      for (let tz = -oz; tz < f.nz; tz += SIDE)
        for (let tx = -ox; tx < f.nx && !(best && boxes.length > best.length); tx += SIDE, stack++) {
          const x0 = Math.max(0, tx), x1 = Math.min(f.nx, tx + SIDE), z0 = Math.max(0, tz), z1 = Math.min(f.nz, tz + SIDE);
          if (x0 >= x1 || z0 >= z1) continue;
          const all = trim(f, cv, x0, x1, z0, z1, -Infinity, Infinity);
          if (!all) continue;
          for (let y = all.y0, level = 0; y < all.y1; y += SIDE, level++) {
            const b = trim(f, cv, all.x0, all.x1, all.z0, all.z1, y, Math.min(all.y1, y + SIDE));
            if (!b) continue;
            boxes.push({ ...b, stack, level });
            volume += (b.x1 - b.x0) * (b.y1 - b.y0) * (b.z1 - b.z0);
          }
        }
      if (!best || boxes.length < best.length || (boxes.length === best.length && volume < bestVolume)) {
        best = boxes;
        bestVolume = volume;
      }
    }
  const out: Piece[] = [];
  for (const b of best ?? []) finish(f, cv, b, out);
  return out;
}

/** Keeps at most `slots` pieces within `bytes`: the most voxels first, a slice only above a kept one. */
export function choose(pieces: Piece[], slots: number, bytes: number): { kept: Piece[]; dropped: Piece[] } {
  if (pieces.length <= slots && pieces.reduce((n, p) => n + p.bytes, 0) <= bytes) return { kept: pieces, dropped: [] };
  const kept = new Set<Piece>();
  const below = (p: Piece) => pieces.filter((q) => q.stack === p.stack && q.level === p.level - 1);
  let used = 0;
  for (;;) {
    let pick: Piece | null = null;
    for (const p of pieces)
      if (!kept.has(p) && used + p.bytes <= bytes && below(p).every((q) => kept.has(q)) && (!pick || p.voxels > pick.voxels)) pick = p;
    if (!pick || kept.size >= slots) break;
    kept.add(pick);
    used += pick.bytes;
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
  return { size: [p.x1 - p.x0, p.y1 - p.y0, p.z1 - p.z0], voxels: p.voxels, runs: p.runs,
    map: { min: tidy(min), max: tidy(max) }, world: { min: tidy(min.map((v) => v * WORLD_PER_MAP) as Vec3), max: tidy(max.map((v) => v * WORLD_PER_MAP) as Vec3) } };
}

export function plan(store: EditorStore, a: FieldArgs): Plan {
  const f = sampleField(store, a);
  const cv = canvas(store, f, a.reuseFrames ?? "aligned");
  const pieces = pack(f, cv);

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
  const { kept, dropped } = choose(pieces, Math.min(slots, Math.floor(opsRoom / 2)), byteRoom);

  const blockVoxels = pieces.reduce((n, p) => n + p.voxels, 0), keptVoxels = kept.reduce((n, p) => n + p.voxels, 0);
  const fillVoxels = cv.fills.reduce((n, x) => n + x.covers, 0), solidVoxels = cv.fills.reduce((n, x) => n + x.solid, 0);
  const frameCells = cv.fills.reduce((n, x) => n + x.changes.size, 0);
  const keptBytes = kept.reduce((n, p) => n + p.bytes, 0);
  const runVoxelsAfter = runVoxels + keptVoxels + frameCells;
  let why: string | undefined;
  if (dropped.length) {
    const lostTop = Math.max(...dropped.map((p) => p.y1));
    const blocks = (n: number) => `${n} block${n === 1 ? "" : "s"}`;
    why = f.sceneId === null
      ? `this level has no Scene frame, so no block can be added, and the frames that overlap the field leave ${blockVoxels} of its ${voxels} voxels ` +
        `(${blocks(pieces.length)} worth)` + ((a.reuseFrames ?? "aligned") === "aligned" ? `: reuseFrames: "any" also fills the rotated and scaled frames` : "") +
        ", or pass partial: true to fill only what the frames can take."
      : `the field needs ${blocks(pieces.length)} and ${slots} may be added (${usedBlocks} of ${LIMITS.newFrames} already used` +
        `${a.maxBlocks !== undefined ? `, maxBlocks ${a.maxBlocks}` : ""})` +
        (pieces.reduce((n, p) => n + p.bytes, 0) > byteRoom ? ", and their runs pass the patch's size budget" : "") +
        `. Without ${blocks(dropped.length)} ${blockVoxels - keptVoxels} voxels are missing` +
        (lostTop >= top ? `, up to the peak (map y ${num(lostTop + f.t[1])})` : "") +
        ". Shrink the rectangle, lower the heights or baseDepth, use fewer material bands (each band crossing a column costs a run, and a block over 2000 runs is split), or pass partial: true to place the blocks holding the most voxels.";
  }
  if (runVoxelsAfter > LIMITS.runVoxels) why = `the patch's runs would cover ${runVoxelsAfter} voxels (at most ${LIMITS.runVoxels})`;
  const fits = !why || (!!a.partial && runVoxelsAfter <= LIMITS.runVoxels);

  const toW = (v: number) => num(v * WORLD_PER_MAP);
  const rect = { min: mapOf(f, [f.i0, f.floor, f.j0]), max: mapOf(f, [f.i0 + f.nx, top === -Infinity ? f.floor : top, f.j0 + f.nz]) };
  const report = {
    fits, ...(why ? { why } : {}),
    region: { columns: f.nx * f.nz, size: [f.nx, f.nz], solidColumns: columns, map: { min: tidy(rect.min), max: tidy(rect.max) },
      world: { min: tidy(rect.min.map((v) => v * WORLD_PER_MAP) as Vec3), max: tidy(rect.max.map((v) => v * WORLD_PER_MAP) as Vec3) },
      floor: { map: f.floor + f.t[1], world: toW(f.floor + f.t[1]) }, peakVoxels: columns ? top - f.floor : 0 },
    voxels: { field: voxels, alreadySolid: solidVoxels, inFrames: fillVoxels, inBlocks: keptVoxels, dropped: blockVoxels - keptVoxels },
    frames: cv.fills.map((x) => ({ id: x.grid.id, name: x.grid.frame.name, ...(x.aligned ? {} : { approximate: true }), cellsFilled: x.changes.size, covers: x.covers, alreadySolid: x.solid, runs: x.runs })),
    ...(cv.skipped.length ? { framesSkipped: { count: cv.skipped.length,
      why: cv.skipped.reduce((o, x) => ({ ...o, [x.why]: (o[x.why] ?? 0) + 1 }), {} as Record<string, number>), list: cv.skipped.slice(0, 20) } } : {}),
    blocks: { needed: pieces.length, placed: kept.length, runs: kept.reduce((n, p) => n + p.runs, 0), list: kept.map((p) => pieceInfo(f, p)) },
    ...(dropped.length ? { dropped: { count: dropped.length, voxels: blockVoxels - keptVoxels, list: dropped.slice(0, 100).map((p) => pieceInfo(f, p)) } } : {}),
    budget: {
      blocks: { before: usedBlocks, after: usedBlocks + (fits ? kept.length : 0), max: LIMITS.newFrames, left: LIMITS.newFrames - usedBlocks - (fits ? kept.length : 0) },
      patchBytes: { before: patchBytes, after: patchBytes + (fits ? fillBytes + keptBytes : 0), budget: PATCH_BUDGET, note: "estimated" },
      runVoxels: { after: runVoxelsAfter, max: LIMITS.runVoxels },
    },
  };
  return { field: f, canvas: cv, pieces, kept, dropped, fits, slots, columns, voxels, why, report };
}

// ------------------------------------------------------------------ apply

let strokes = -1_000_000;

/** Applies a plan that fits as one undo step: the frame fills, then the blocks. */
export function apply(store: EditorStore, p: Plan): { blocks: { id: number; name: string }[] } {
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
    const size: Vec3 = [pc.x1 - pc.x0, pc.y1 - pc.y0, pc.z1 - pc.z0];
    const frame: Frame = { id: tmp, new: true, parent: f.sceneId!, name, pos: [f.i0 + pc.x0 + size[0] / 2, pc.y0 + size[1] / 2, f.j0 + pc.z0 + size[2] / 2],
      rot: [0, 0, 0], scale: [1, 1, 1], size, voxels: store.freshRef(), heightMap: null, folder: false };
    cmds.push(new AddFrame(store.voxels, frame, words(f, p.canvas, pc)));
    made.push({ id: tmp, name });
  }
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
