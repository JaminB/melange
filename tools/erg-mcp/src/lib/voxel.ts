// Voxel helpers for the tool groups: frame grids, coordinates, reading and writing voxel words as undoable edits, the
// run encoding the patch uses, and the server's limits.
//
// Units. Map units are the level file's (.xan) units: frame and detail positions, and 1 voxel = 1 map unit. World units
// are map units x 20 (WORLD_PER_XAN): what the Erg panel shows, and the unit of the water level.
// Grid coordinates are a frame's voxel cells: voxel (x, y, z) fills [x, x+1) x [y, y+1) x [z, z+1) of grid space, y is
// up, and its word is at index (z*X + x)*Y + y, so one (x, z) column is a contiguous run of Y words. voxelWorld places
// the grid in the level, centred on the frame's position.
// Words (src/erg/voxels.h): bits 0-1 solid (3) or empty (0), 2-7 material, 8-9 second-material flags, 10-15 second
// material, 16-23 corner mask, 24-31 clear. isSolid/material/filled/carved/painted/withSecond come from terrain/voxel.ts.
import {
  LIMITS, SculptStroke, WORLD_PER_XAN, apply, blockProblem, cellBudget, gridFrames, index, invert, validEdit, validRunValue, voxelRuns, type Box, type EditorStore,
  type Frame, type Mat3x4, type Patch, type Vec3, type VoxelRun,
} from "../core";
import { ToolError } from "../registry";

export {
  carved, filled, isSolid, keptFrom, maskOf, material, painted, secondOf, validRunValue, withSecond, MATERIALS, SOLID,
} from "../core";

// ------------------------------------------------------------------ units

export const WORLD_PER_MAP = WORLD_PER_XAN;
export const toWorld = (v: Vec3): Vec3 => [v[0] * WORLD_PER_MAP, v[1] * WORLD_PER_MAP, v[2] * WORLD_PER_MAP];
export const toMap = (v: Vec3): Vec3 => [v[0] / WORLD_PER_MAP, v[1] / WORLD_PER_MAP, v[2] / WORLD_PER_MAP];
export const boxToWorld = (b: Box): Box => ({ min: toWorld(b.min), max: toWorld(b.max) });
/** Rounds away float noise for output (4 places; -0 becomes 0). */
export const tidy = (v: Vec3, places = 4): Vec3 => v.map((c) => Math.round(c * 10 ** places) / 10 ** places || 0) as Vec3;

// ------------------------------------------------------------------ grids

/** One editable voxel frame: its live words and where its grid sits in the level. */
export interface Grid {
  id: number;                  // frame id (negative: a block added in this project)
  frame: Frame;
  ref: number;                 // key in store.voxels / store.baseVoxels
  size: Vec3;                  // X, Y, Z voxels
  words: Uint32Array;          // the edited words (live: change them only through VoxelEdit or a Command)
  base: Uint32Array;           // what toPatch diffs against: the base's words, or zeros for an added block
  toMap: Mat3x4;               // grid -> map units
  fromMap: Mat3x4;             // map units -> grid
  bounds: Box;                 // the grid's box in map units
}

/** Every frame with voxels of the right size and an invertible placement, in scene order. */
export function grids(store: EditorStore): Grid[] {
  return gridFrames(store.scene, store.voxels, (id) => store.voxelRef(id)).map((g) => ({
    id: g.frame.id, frame: g.frame, ref: g.ref, size: [g.frame.size[0], g.frame.size[1], g.frame.size[2]], words: store.voxels.get(g.ref)!,
    base: store.baseVoxels.get(g.ref) ?? new Uint32Array(store.voxels.get(g.ref)!.length), toMap: g.toWorld, fromMap: g.toLocal, bounds: g.bounds,
  }));
}

/** The grid of one frame, or a ToolError saying why it has none. */
export function grid(store: EditorStore, frameId: number): Grid {
  const g = grids(store).find((x) => x.id === frameId);
  if (g) return g;
  const f = store.scene.frames.find((x) => x.id === frameId);
  throw new ToolError(f ? `frame ${frameId} (${f.name}) has no voxels` : `no frame ${frameId}`);
}

export const inside = (size: Vec3, x: number, y: number, z: number) =>
  Number.isInteger(x) && Number.isInteger(y) && Number.isInteger(z) && x >= 0 && y >= 0 && z >= 0 && x < size[0] && y < size[1] && z < size[2];

/** The word index of a cell: (z*X + x)*Y + y. */
export const voxelIndex = (size: Vec3, x: number, y: number, z: number) => index({ size }, x, y, z);

function at(g: Grid, x: number, y: number, z: number): number {
  if (!inside(g.size, x, y, z)) throw new ToolError(`(${x}, ${y}, ${z}) is outside frame ${g.id}'s ${g.size.join("x")} voxels`);
  return voxelIndex(g.size, x, y, z);
}

/** The current word of a cell. */
export const getVoxel = (g: Grid, x: number, y: number, z: number) => g.words[at(g, x, y, z)];

// ------------------------------------------------------------------ coordinates

/** Grid coordinates (fractional) -> map units. A cell's centre is (x + 0.5, y + 0.5, z + 0.5). */
export const gridToMap = (g: Grid, p: Vec3): Vec3 => apply(g.toMap, p);
/** Map units -> grid coordinates (fractional). */
export const mapToGrid = (g: Grid, p: Vec3): Vec3 => apply(g.fromMap, p);
export const gridToWorld = (g: Grid, p: Vec3): Vec3 => toWorld(gridToMap(g, p));
export const worldToGrid = (g: Grid, p: Vec3): Vec3 => mapToGrid(g, toMap(p));
export const cellCentre = (g: Grid, x: number, y: number, z: number): Vec3 => gridToMap(g, [x + 0.5, y + 0.5, z + 0.5]);

/** The cell holding a map point, or null outside the grid. */
export function cellAt(g: Grid, p: Vec3): Vec3 | null {
  const q = mapToGrid(g, p).map(Math.floor) as Vec3;
  return inside(g.size, q[0], q[1], q[2]) ? q : null;
}

/** Every grid whose box holds a map point, with the cell. */
export function gridsAt(store: EditorStore, p: Vec3): { grid: Grid; cell: Vec3 }[] {
  const out: { grid: Grid; cell: Vec3 }[] = [];
  for (const g of grids(store)) {
    const cell = cellAt(g, p);
    if (cell) out.push({ grid: g, cell });
  }
  return out;
}

/** A frame's world matrix (map units) for any frame, voxels or not. */
export const frameMatrix = (store: EditorStore, frameId: number): Mat3x4 | null => store.frames.worldOf(frameId);
export { invert };

// ------------------------------------------------------------------ runs

/** The patch's voxels encoding: [start, count, value] over changed word indices (index order, so runs follow columns),
 * neighbours with the same new value merged. This is diff.ts voxelRuns, which toPatch uses. */
export const columnRuns = (before: Uint32Array, after: Uint32Array): VoxelRun[] => voxelRuns(before, after);
/** Runs of a whole grid against an empty one (an added block's voxels op). */
export const encodeRuns = (words: Uint32Array): VoxelRun[] => voxelRuns(new Uint32Array(words.length), words);
/** JSON bytes runs take in the patch ("[start,count,value],"). */
export const runBytes = (runs: VoxelRun[]) => runs.reduce((n, r) => n + String(r[0]).length + String(r[1]).length + String(r[2]).length + 5, 0);
/** Runs a grid needs in the patch now. */
export const gridRuns = (g: Grid): VoxelRun[] => voxelRuns(g.base, g.words);

// ------------------------------------------------------------------ limits

/** The headroom the panel keeps under the 4 MB patch limit for detail ops and the envelope (sculpt.ts). */
export const PATCH_BUDGET = LIMITS.patchBytes - (512 << 10);

export interface Usage {
  frames: number; details: number;
  newFrames: { used: number; max: number };
  newFrameSide: number;
  newFrameScale: { min: number; max: number };
  voxelCells: { used: number; max: number; free: number; note: string };
  objects: { used: number; max: number };
  ops: { used: number; max: number };
  patchBytes: { used: number; max: number; budget: number };
  runsPerFrame: { worst: number; frame: number | null; max: number };
}

/** The open project's patch against the server's limits (LIMITS in sdk/erg/scene.ts, src/erg/patch.h). */
export function usage(store: EditorStore, patch: Patch = store.patch()): Usage {
  let worst = 0, worstFrame: number | null = null;
  for (const op of patch.ops) if (op.op === "voxels" && op.runs.length > worst) { worst = op.runs.length; worstFrame = op.frame; }
  return {
    frames: store.scene.frames.length, details: store.scene.details.length,
    newFrames: { used: store.scene.frames.filter((f) => f.new).length, max: LIMITS.newFrames },
    newFrameSide: LIMITS.newFrameSide,
    newFrameScale: LIMITS.newFrameScale,
    voxelCells: { ...cellBudget(store.scene), note: "X*Y*Z of every voxel frame, base and added, solid or empty: the game crashes loading more" },
    objects: { used: store.scene.objects?.length ?? 0, max: LIMITS.objects },
    ops: { used: patch.ops.length, max: LIMITS.ops },
    patchBytes: { used: JSON.stringify(patch).length, max: LIMITS.patchBytes, budget: PATCH_BUDGET },
    runsPerFrame: { worst, frame: worstFrame, max: LIMITS.runsPerFrame },
  };
}

/** Why a new block of this size and scale cannot be added (count, side, scale or the game's cell budget), or null. */
export function newFrameProblem(store: EditorStore, size: Vec3, scale = 1): string | null {
  const why = blockProblem(store.scene, size, scale);
  return why && why[0].toLowerCase() + why.slice(1);
}

/** Everything over a limit, then what the patch validator refuses (store.problems()). */
export function limitProblems(store: EditorStore): string[] {
  const u = usage(store);
  const out: string[] = [];
  if (u.newFrames.used > u.newFrames.max) out.push(`${u.newFrames.used} added blocks (at most ${u.newFrames.max})`);
  if (u.objects.used > u.objects.max) out.push(`${u.objects.used} objects (at most ${u.objects.max})`);
  if (u.ops.used > u.ops.max) out.push(`${u.ops.used} ops (at most ${u.ops.max})`);
  if (u.patchBytes.used > u.patchBytes.max) out.push(`the patch is ${u.patchBytes.used} bytes (at most ${u.patchBytes.max})`);
  if (u.runsPerFrame.worst > u.runsPerFrame.max) out.push(`frame ${u.runsPerFrame.frame} needs ${u.runsPerFrame.worst} runs (at most ${u.runsPerFrame.max})`);
  return [...out, ...store.problems()];
}

// ------------------------------------------------------------------ editing

let strokes = 0;

/** A batch of voxel writes over any frames, committed as one undo step (the panel's SculptStroke command). Writes are
 * checked as the panel's sculptor checks a brush step: valid edits of the base's words (validEdit), at most 2000 runs per
 * frame, the patch within its budget. Reads through get() see the batch's own pending writes. */
export class VoxelEdit {
  private changes = new Map<number, Map<number, [number, number]>>();   // ref -> index -> [before, after]
  private byId = new Map<number, Grid>();

  constructor(private store: EditorStore) {}

  private gridOf(frameId: number): Grid {
    let g = this.byId.get(frameId);
    if (!g) this.byId.set(frameId, (g = grid(this.store, frameId)));
    return g;
  }

  get(frameId: number, x: number, y: number, z: number): number {
    const g = this.gridOf(frameId), i = at(g, x, y, z);
    return this.changes.get(g.ref)?.get(i)?.[1] ?? g.words[i];
  }

  /** `second`: the word carries a second material and corner mask of its own (bits 8-23), as the panel's 2nd-material
   * brush writes; the level service accepts those (its blend rule), so only the word's shape is checked. */
  set(frameId: number, x: number, y: number, z: number, word: number, second = false) {
    if (!validRunValue(word)) throw new ToolError(`0x${word.toString(16)} is not a voxel word (bits 0-1 must be 0 or 3, bits 24-31 clear)`);
    const g = this.gridOf(frameId), i = at(g, x, y, z);
    if (!second && !validEdit(g.base[i], word))
      throw new ToolError(`0x${word.toString(16)} is not a valid edit of the base word 0x${g.base[i].toString(16)} (bits 8-23 must be zero or the base's)`);
    let c = this.changes.get(g.ref);
    if (!c) this.changes.set(g.ref, (c = new Map()));
    const before = c.get(i)?.[0] ?? g.words[i];
    if (before === word) c.delete(i);
    else c.set(i, [before, word]);
  }

  /** Voxels whose word changes. */
  get size() { let n = 0; for (const c of this.changes.values()) n += c.size; return n; }
  /** The frames touched, by id. */
  get frames(): number[] { return [...this.byId.values()].filter((g) => this.changes.get(g.ref)?.size).map((g) => g.id); }

  /** Why the batch cannot be applied, or null. */
  check(): string | null {
    let total = 0;
    const touched = new Set<number>();
    for (const [ref, c] of this.changes) {
      if (!c.size) continue;
      touched.add(ref);
      const g = [...this.byId.values()].find((x) => x.ref === ref)!;
      const words = g.words.slice();
      for (const [i, [, after]] of c) words[i] = after;
      const runs = voxelRuns(g.base, words);
      if (runs.length > LIMITS.runsPerFrame)
        return `frame ${g.id} would need ${runs.length} runs (the limit is ${LIMITS.runsPerFrame}): the edits are too scattered`;
      total += runBytes(runs);
    }
    for (const g of grids(this.store)) if (!touched.has(g.ref)) total += runBytes(gridRuns(g));
    if (total > PATCH_BUDGET) return "the terrain edits would make the patch larger than the server accepts";
    return null;
  }

  /** Applies the batch as one undoable command; a ToolError when check() refuses. Returns what changed. */
  commit(label = "Edit voxels"): { changed: number; frames: number[] } {
    const changed = this.size, frames = this.frames;
    if (!changed) return { changed, frames: [] };
    const why = this.check();
    if (why) throw new ToolError(why);
    const live = new Map([...this.changes].filter(([, c]) => c.size));
    this.store.exec(new SculptStroke(label, --strokes, this.store.voxels, live, () => {}));
    this.changes = new Map();
    return { changed, frames };
  }
}

/** One voxel written as its own undo step (a VoxelEdit of one); false when the word was already there. */
export function setVoxel(store: EditorStore, frameId: number, x: number, y: number, z: number, word: number, label = "Set voxel"): boolean {
  const e = new VoxelEdit(store);
  e.set(frameId, x, y, z, word);
  return e.commit(label).changed > 0;
}
