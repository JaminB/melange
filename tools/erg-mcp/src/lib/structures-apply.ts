// Writes a structure plan (structures-shapes.ts) into the open level as one undo step: voxels of the frames it overlaps
// are edited with the panel's SculptStroke command, and what falls outside every frame goes into new terrain blocks
// (AddFrame, the panel's own block command) under the Scene frame. Edits go through the same patch the panel saves.
import {
  AddFrame, Group, LIMITS, SculptStroke, cellBudget, type Command, carved, cornerOffset, filled, isSolid, material, painted, sceneFrame, validNewFrameScale, voxelRuns,
  type EditorStore, type Frame, type Vec3,
} from "../core";
import { ToolError } from "../registry";
import { PATCH_BUDGET, boxToWorld, grids, limitProblems, tidy, usage, type Grid } from "./voxel";
import { planBounds, type Plan, type Shape } from "./structures-shapes";

export type Place = "auto" | "existing" | "new";
/** How new blocks are cut: tiles (default) of at most 31 cells a side, each trimmed to what it holds; parts: a block
 * per part of the structure (a wall, a tower, the keep), merged where that costs few cells, so the air between parts (a
 * courtyard) is in no block; or fit: the tiles, then the box whose halving saves the most cells halved again and again
 * while blocks are left (maxBlocks) and a halving saves at least a twentieth of its box: a rotated castle's diagonal walls
 * in boxes that follow them instead of a few large boxes of air. */
export type Packing = "tiles" | "parts" | "fit";

/** drape: "skip" (default) leaves draped blocks (corner offsets) alone; "sheared" edits them where the game draws their
 * cells (each column moved up by its bilinear corner offset), so a ramp or tunnel cut runs through a skin. */
export interface ApplyOptions { place: Place; dryRun: boolean; blockScale?: number; packing?: Packing; drape?: "skip" | "sheared"; maxBlocks?: number; maxCells?: number;
  /** Commands of the same undo step, applied first (sculpt_road's re-graded skin blocks). */
  extra?: Command[]; }

/** The most cells a structure may sample (existing frames plus the block lattice). */
export const MAX_CELLS = 6_000_000;

let lastPart = -1;   // resolve(): the part of the last fill that hit the cell

function resolve(shapes: Shape[], x: number, y: number, z: number, word: number, fills: boolean): number {
  lastPart = -1;
  for (let k = 0; k < shapes.length; k++) {
    const s = shapes[k];
    if (x < s.min[0] || x >= s.max[0] || y < s.min[1] || y >= s.max[1] || z < s.min[2] || z >= s.max[2] || !s.hit(x, y, z)) continue;
    const mat = s.matAt ? s.matAt(x, y, z) : s.mat;
    if (s.op === "fill") {
      if (fills && !(isSolid(word) && (s.keep || material(word) === mat))) word = filled(mat);
      lastPart = s.part ?? k;
    } else if (s.op === "carve") word = carved(word);
    else if (isSolid(word)) word = painted(word, mat);
  }
  return word;
}

/** The solid material that most voxels near `p` (within `r` map units) have: the look of the level's own terrain. */
export function dominantMaterial(store: EditorStore, p: Vec3, r = 40): number | null {
  const count = new Map<number, number>();
  for (const g of grids(store)) {
    if (g.bounds.min.some((v, a) => v > p[a] + r) || g.bounds.max.some((v, a) => v < p[a] - r)) continue;
    for (const w of g.words) if (isSolid(w)) count.set(material(w), (count.get(material(w)) ?? 0) + 1);
  }
  let best: number | null = null, n = 0;
  for (const [m, c] of count) if (c > n) { best = m; n = c; }
  return best;
}

interface Block { frame: Frame; words: Uint32Array; voxels: number; }

export interface StructureResult {
  label: string;
  dryRun: boolean;
  voxels: { added: number; removed: number; repainted: number; changed: number };
  frames: { id: number; name: string; changed: number }[];
  newBlocks: { id: number; name: string; size: Vec3; centre: { map: Vec3; world: Vec3 }; voxels: number; cells: number; scale: number }[];
  /** The game's voxel cells (X x Y x Z of each frame, empty or not) this structure's new blocks take, and what the
   * level's frames hold and have left after it (65 535 at most). */
  cells: { newBlocks: number; level: number; free: number; max: number };
  outsideFrames: number;
  bounds: { map: { min: Vec3; max: Vec3 }; world: { min: Vec3; max: Vec3 } };
  usage: { newBlocks: string; patchBytes: string; worstRuns: string; voxelCells: string };
  notes: string[];
}

function cellRange(g: Grid, min: Vec3, max: Vec3, lift: [number, number] = [0, 0]): [Vec3, Vec3] | null {
  const lo: Vec3 = [Infinity, Infinity, Infinity], hi: Vec3 = [-Infinity, -Infinity, -Infinity];
  for (let k = 0; k < 8; k++) {
    const p: Vec3 = [k & 1 ? max[0] : min[0], k & 2 ? max[1] : min[1], k & 4 ? max[2] : min[2]];
    const q = [0, 1, 2].map((r) => g.fromMap[r * 4] * p[0] + g.fromMap[r * 4 + 1] * p[1] + g.fromMap[r * 4 + 2] * p[2] + g.fromMap[r * 4 + 3]);
    for (let a = 0; a < 3; a++) { lo[a] = Math.min(lo[a], q[a]); hi[a] = Math.max(hi[a], q[a]); }
  }
  // a draped block's cells sit up to its corner offsets above their grid place
  lo[1] -= lift[1];
  hi[1] -= lift[0];
  const a = lo.map((v) => Math.max(0, Math.floor(v))) as Vec3, b = hi.map((v, i) => Math.min(g.size[i] - 1, Math.ceil(v))) as Vec3;
  return a.every((v, i) => v <= b[i]) ? [a, b] : null;
}

const overlap = (g: Grid, min: Vec3, max: Vec3) => g.bounds.min.every((v, a) => v < max[a]) && g.bounds.max.every((v, a) => v > min[a]);

/** The range of a draped block's corner offsets in its voxels ([0, 0] for any other frame). */
const liftRange = (g: Grid): [number, number] => (g.frame.corners ? [Math.min(...g.frame.corners), Math.max(...g.frame.corners)] : [0, 0]);
/** Whether a grid's box, its draped columns moved up by their offsets, overlaps [min, max). */
const overlapSheared = (g: Grid, min: Vec3, max: Vec3) => {
  const [l0, l1] = liftRange(g), sy = Math.hypot(g.toMap[1], g.toMap[5], g.toMap[9]);
  return g.bounds.min.every((v, a) => (a === 1 ? v + l0 * sy : v) < max[a]) && g.bounds.max.every((v, a) => (a === 1 ? v + l1 * sy : v) > min[a]);
};

/** Splits `lo..hi` (inclusive lattice cells) into blocks whose words fit the runs limit. `at` is a cell's material + 1, or
 * 0 for none. */
function makeBlocks(at: (i: number, j: number, k: number) => number, lo: Vec3, hi: Vec3, ids: { next(): number; name(): string }, store: EditorStore, sceneId: number, out: Block[]) {
  const tl: Vec3 = [Infinity, Infinity, Infinity], th: Vec3 = [-Infinity, -Infinity, -Infinity];
  for (let k = lo[2]; k <= hi[2]; k++) for (let i = lo[0]; i <= hi[0]; i++) for (let j = lo[1]; j <= hi[1]; j++) {
    if (!at(i, j, k)) continue;
    const c = [i, j, k];
    for (let a = 0; a < 3; a++) { tl[a] = Math.min(tl[a], c[a]); th[a] = Math.max(th[a], c[a]); }
  }
  if (tl[0] === Infinity) return;
  const size: Vec3 = [th[0] - tl[0] + 1, th[1] - tl[1] + 1, th[2] - tl[2] + 1];
  const words = new Uint32Array(size[0] * size[1] * size[2]);
  let n = 0;
  for (let k = 0; k < size[2]; k++) for (let i = 0; i < size[0]; i++) for (let j = 0; j < size[1]; j++) {
    const m = at(tl[0] + i, tl[1] + j, tl[2] + k);
    if (m) { words[(k * size[0] + i) * size[1] + j] = filled(m - 1); n++; }
  }
  if (voxelRuns(new Uint32Array(words.length), words).length > LIMITS.runsPerFrame && n > 1) {
    const axis = size[0] >= size[1] && size[0] >= size[2] ? 0 : size[1] >= size[2] ? 1 : 2;
    if (size[axis] > 1) {
      const cut = tl[axis] + Math.floor(size[axis] / 2);
      const h1: Vec3 = [th[0], th[1], th[2]], l2: Vec3 = [tl[0], tl[1], tl[2]];
      h1[axis] = cut - 1;
      l2[axis] = cut;
      makeBlocks(at, tl, h1, ids, store, sceneId, out);
      makeBlocks(at, l2, th, ids, store, sceneId, out);
      return;
    }
  }
  const frame: Frame = {
    id: ids.next(), new: true, parent: sceneId, name: ids.name(),
    pos: [0, 0, 0], rot: [0, 0, 0], scale: [1, 1, 1], size, voxels: store.freshRef(), heightMap: null, folder: false,
  };
  // the corner is stored in `pos` for now (lattice units); the caller adds the lattice origin and half the size
  frame.pos = [tl[0] + size[0] / 2, tl[1] + size[1] / 2, tl[2] + size[2] / 2];
  out.push({ frame, words, voxels: n });
}

/** A block's box and the part tiles in it: a cell goes in the block when it is in one of them, of that tile's part. */
interface PartBox { lo: Vec3; hi: Vec3; members: { lo: Vec3; hi: Vec3; part: number }[]; }
const boxCells = (b: { lo: Vec3; hi: Vec3 }) => (b.hi[0] - b.lo[0] + 1) * (b.hi[1] - b.lo[1] + 1) * (b.hi[2] - b.lo[2] + 1);

/** packing "parts": the box of each part's cells (tiled at 31 a side and trimmed), then boxes merged two at a time while
 * the merged box fits 31 a side and costs at most an eighth more cells than the two apart (fewer blocks for little). */
function partBoxes(cells: Uint8Array, parts: Int32Array, dim: Vec3): PartBox[] {
  const side = LIMITS.newFrameSide;
  const ext = new Map<number, { lo: Vec3; hi: Vec3 }>();
  for (let k = 0; k < dim[2]; k++) for (let i = 0; i < dim[0]; i++) for (let j = 0; j < dim[1]; j++) {
    const n = (k * dim[0] + i) * dim[1] + j;
    if (!cells[n]) continue;
    const e = ext.get(parts[n]);
    if (!e) ext.set(parts[n], { lo: [i, j, k], hi: [i, j, k] });
    else {
      const c = [i, j, k];
      for (let a = 0; a < 3; a++) { e.lo[a] = Math.min(e.lo[a], c[a]); e.hi[a] = Math.max(e.hi[a], c[a]); }
    }
  }
  let boxes: PartBox[] = [];
  for (const [p, e] of ext) {
    for (let z = e.lo[2]; z <= e.hi[2]; z += side) for (let x = e.lo[0]; x <= e.hi[0]; x += side) for (let y = e.lo[1]; y <= e.hi[1]; y += side) {
      const a: Vec3 = [x, y, z], b: Vec3 = [Math.min(e.hi[0], x + side - 1), Math.min(e.hi[1], y + side - 1), Math.min(e.hi[2], z + side - 1)];
      // trimmed to the part's cells in the tile
      const tl: Vec3 = [Infinity, Infinity, Infinity], th: Vec3 = [-Infinity, -Infinity, -Infinity];
      for (let k = a[2]; k <= b[2]; k++) for (let i = a[0]; i <= b[0]; i++) for (let j = a[1]; j <= b[1]; j++) {
        const n = (k * dim[0] + i) * dim[1] + j;
        if (!cells[n] || parts[n] !== p) continue;
        const c = [i, j, k];
        for (let q = 0; q < 3; q++) { tl[q] = Math.min(tl[q], c[q]); th[q] = Math.max(th[q], c[q]); }
      }
      if (tl[0] !== Infinity) boxes.push({ lo: tl, hi: th, members: [{ lo: tl, hi: th, part: p }] });
    }
  }
  for (;;) {
    let best: [number, number, PartBox] | null = null, bestCost = Infinity;
    for (let u = 0; u < boxes.length; u++) for (let v = u + 1; v < boxes.length; v++) {
      const m: PartBox = { lo: [0, 1, 2].map((a) => Math.min(boxes[u].lo[a], boxes[v].lo[a])) as Vec3,
        hi: [0, 1, 2].map((a) => Math.max(boxes[u].hi[a], boxes[v].hi[a])) as Vec3, members: [...boxes[u].members, ...boxes[v].members] };
      if ([0, 1, 2].some((a) => m.hi[a] - m.lo[a] + 1 > side)) continue;
      const apart = boxCells(boxes[u]) + boxCells(boxes[v]), cost = boxCells(m) - apart;
      if (cost <= apart / 8 && cost < bestCost) { best = [u, v, m]; bestCost = cost; }
    }
    if (!best) break;
    boxes = [...boxes.filter((_, i) => i !== best![0] && i !== best![1]), best[2]];
  }
  return boxes;
}

/** packing "fit": the 31-a-side tiles trimmed to their cells, then, while there are fewer than `cap` boxes, the box and
 * cut whose two trimmed halves save the most cells is split, as long as that saves at least a twentieth of the box (and
 * 8 cells). A halving is searched along each axis at every cut, from each slice's own box of cells (prefix and suffix
 * unions), so it costs the box's volume per box, not per cut. Blocks the runs limit splits later are not counted. */
export function fitBoxes(cells: Uint8Array, dim: Vec3, cap: number): { lo: Vec3; hi: Vec3 }[] {
  const side = LIMITS.newFrameSide, idx = (i: number, j: number, k: number) => (k * dim[0] + i) * dim[1] + j;
  const trim = (a: Vec3, b: Vec3): { lo: Vec3; hi: Vec3 } | null => {
    const tl: Vec3 = [Infinity, Infinity, Infinity], th: Vec3 = [-Infinity, -Infinity, -Infinity];
    for (let k = a[2]; k <= b[2]; k++) for (let i = a[0]; i <= b[0]; i++) for (let j = a[1]; j <= b[1]; j++) {
      if (!cells[idx(i, j, k)]) continue;
      const c = [i, j, k];
      for (let q = 0; q < 3; q++) { tl[q] = Math.min(tl[q], c[q]); th[q] = Math.max(th[q], c[q]); }
    }
    return tl[0] === Infinity ? null : { lo: tl, hi: th };
  };
  type Box = { lo: Vec3; hi: Vec3; cut?: { save: number; a: { lo: Vec3; hi: Vec3 }; b: { lo: Vec3; hi: Vec3 } } | null };
  // the best halving of a box: per axis, each slice's box of cells, then every cut's two unions
  const bestCut = (bx: Box) => {
    let best: Box["cut"] = null;
    const vol = boxCells(bx);
    for (let ax = 0; ax < 3; ax++) {
      const n = bx.hi[ax] - bx.lo[ax] + 1;
      if (n < 2) continue;
      const sl: ({ lo: Vec3; hi: Vec3 } | null)[] = [];
      for (let s = 0; s < n; s++) {
        const a: Vec3 = [...bx.lo] as Vec3, b: Vec3 = [...bx.hi] as Vec3;
        a[ax] = b[ax] = bx.lo[ax] + s;
        sl.push(trim(a, b));
      }
      const join = (p: { lo: Vec3; hi: Vec3 } | null, q: { lo: Vec3; hi: Vec3 } | null) => (!p ? q : !q ? p
        : { lo: [0, 1, 2].map((a) => Math.min(p.lo[a], q.lo[a])) as Vec3, hi: [0, 1, 2].map((a) => Math.max(p.hi[a], q.hi[a])) as Vec3 });
      const pre: ({ lo: Vec3; hi: Vec3 } | null)[] = [], suf: ({ lo: Vec3; hi: Vec3 } | null)[] = new Array(n).fill(null);
      for (let s = 0; s < n; s++) pre.push(join(s ? pre[s - 1] : null, sl[s]));
      for (let s = n - 1; s >= 0; s--) suf[s] = join(s < n - 1 ? suf[s + 1] : null, sl[s]);
      for (let c = 1; c < n; c++) {
        const a = pre[c - 1], b = suf[c];
        if (!a || !b) continue;
        const save = vol - boxCells(a) - boxCells(b);
        if (!best || save > best.save) best = { save, a, b };
      }
    }
    return best;
  };
  let boxes: Box[] = [];
  for (let cz = 0; cz < dim[2]; cz += side) for (let cx = 0; cx < dim[0]; cx += side) for (let cy = 0; cy < dim[1]; cy += side) {
    const t = trim([cx, cy, cz], [Math.min(dim[0], cx + side) - 1, Math.min(dim[1], cy + side) - 1, Math.min(dim[2], cz + side) - 1]);
    if (t) boxes.push(t);
  }
  while (boxes.length < cap) {
    let pick = -1;
    for (let u = 0; u < boxes.length; u++) {
      if (boxes[u].cut === undefined) boxes[u].cut = bestCut(boxes[u]);
      const c = boxes[u].cut;
      if (c && c.save >= Math.max(8, boxCells(boxes[u]) / 20) && (pick < 0 || c.save > boxes[pick].cut!.save)) pick = u;
    }
    if (pick < 0) break;
    const c = boxes[pick].cut!;
    boxes = [...boxes.slice(0, pick), ...boxes.slice(pick + 1), { lo: c.a.lo, hi: c.a.hi }, { lo: c.b.lo, hi: c.b.hi }];
  }
  return boxes.map((b) => ({ lo: b.lo, hi: b.hi }));
}

/** Writes the plan into the open project (or only counts it, with dryRun). */

export function applyPlan(store: EditorStore, plan: Plan, opt: ApplyOptions): StructureResult {
  const { min, max } = planBounds(plan.shapes);
  if (!plan.shapes.length || !Number.isFinite(min[0])) throw new ToolError("the structure is empty");
  const vol = (max[0] - min[0]) * (max[1] - min[1]) * (max[2] - min[2]);
  if (vol > MAX_CELLS) throw new ToolError(`the structure's box is ${Math.round(vol)} cells (at most ${MAX_CELLS}): split it into parts`);
  const notes = [...plan.notes];
  // A draped block (corner offsets: heightfield_drape) is not where its grid says, so structures leave it alone, unless
  // drape "sheared" asks to edit its cells where the game draws them.
  const sheared = opt.drape === "sheared";
  const near = grids(store).filter((g) => (g.frame.corners ? sheared && overlapSheared(g, min, max) : overlap(g, min, max)));
  const fillsExisting = opt.place !== "new";
  const stroke = new Map<number, Map<number, [number, number]>>();
  const perFrame = new Map<number, { id: number; name: string; changed: number }>();
  let added = 0, removed = 0, repainted = 0;

  // 1. frames that exist
  for (const g of near) {
    const range = cellRange(g, min, max, liftRange(g));
    if (!range) continue;
    const [lo, hi] = range;
    let c = stroke.get(g.ref);
    for (let z = lo[2]; z <= hi[2]; z++) for (let x = lo[0]; x <= hi[0]; x++) for (let y = lo[1]; y <= hi[1]; y++) {
      const cy = y + 0.5 + (g.frame.corners ? cornerOffset(g.frame, x + 0.5, z + 0.5) : 0);
      const p = [0, 1, 2].map((r) => g.toMap[r * 4] * (x + 0.5) + g.toMap[r * 4 + 1] * cy + g.toMap[r * 4 + 2] * (z + 0.5) + g.toMap[r * 4 + 3]);
      const i = (z * g.size[0] + x) * g.size[1] + y;
      const before = g.words[i];
      const after = resolve(plan.shapes, p[0], p[1], p[2], before, fillsExisting);
      if (after === before) continue;
      if (!c) stroke.set(g.ref, (c = new Map()));
      c.set(i, [before, after]);
      if (!isSolid(before) && isSolid(after)) added++;
      else if (isSolid(before) && !isSolid(after)) removed++;
      else if (isSolid(after)) repainted++;
      const f = perFrame.get(g.id) ?? { id: g.id, name: g.frame.name, changed: 0 };
      f.changed++;
      perFrame.set(g.id, f);
    }
  }

  // 2. what is left over goes into new blocks on the Scene frame's lattice of blockScale map units (whole units by default)
  const blocks: Block[] = [];
  let outside = 0;
  const scene = sceneFrame(store.scene);
  const bs = opt.blockScale ?? 1;
  if (!validNewFrameScale(bs)) throw new ToolError(`blockScale is ${LIMITS.newFrameScale.min}-${LIMITS.newFrameScale.max} map units per voxel`);
  if (opt.place !== "existing") {
    const w = scene ? store.frames.worldOf(scene.id) : null;
    const flat = w && [1, 0, 0, 0, 1, 0, 0, 0, 1].every((v, i) => Math.abs(w[Math.floor(i / 3) * 4 + (i % 3)] - v) < 1e-6);
    if (!scene || !w || !flat) {
      notes.push(scene ? "the Scene frame is rotated or scaled, so new blocks cannot be aligned: only existing frames were edited"
        : "this level has no Scene frame to add blocks under: only existing frames were edited");
    } else {
      const t: Vec3 = [w[3], w[7], w[11]];
      const lo = min.map((v, a) => Math.floor((v - t[a]) / bs + 1e-9)) as Vec3, hi = max.map((v, a) => Math.ceil((v - t[a]) / bs - 1e-9)) as Vec3;
      const dim: Vec3 = [hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]];
      if (dim[0] * dim[1] * dim[2] > MAX_CELLS) throw new ToolError(`the structure's box is ${dim[0] * dim[1] * dim[2]} block cells (at most ${MAX_CELLS}): split it into parts`);
      const cells = new Uint8Array(dim[0] * dim[1] * dim[2]);
      const parts = opt.packing === "parts" ? new Int32Array(cells.length) : null;
      for (let k = 0; k < dim[2]; k++) for (let i = 0; i < dim[0]; i++) for (let j = 0; j < dim[1]; j++) {
        const px = (lo[0] + i + 0.5) * bs + t[0], py = (lo[1] + j + 0.5) * bs + t[1], pz = (lo[2] + k + 0.5) * bs + t[2];
        if (px < min[0] || px >= max[0] || py < min[1] || py >= max[1] || pz < min[2] || pz >= max[2]) continue;
        const r = resolve(plan.shapes, px, py, pz, 0, true);
        if (!isSolid(r)) continue;
        if (parts) parts[(k * dim[0] + i) * dim[1] + j] = lastPart;
        let covered = false;
        for (const g of near) {
          const q = [0, 1, 2].map((a) => g.fromMap[a * 4] * px + g.fromMap[a * 4 + 1] * py + g.fromMap[a * 4 + 2] * pz + g.fromMap[a * 4 + 3]);
          if (g.frame.corners) q[1] -= cornerOffset(g.frame, q[0], q[2]);
          const x = Math.floor(q[0]), y = Math.floor(q[1]), z = Math.floor(q[2]);
          if (x < 0 || y < 0 || z < 0 || x >= g.size[0] || y >= g.size[1] || z >= g.size[2]) continue;
          // auto: the frame's own pass handled this cell; new: a solid voxel of the frame is already there
          if (opt.place === "auto" || isSolid(g.words[(z * g.size[0] + x) * g.size[1] + y])) { covered = true; break; }
        }
        if (!covered) { cells[(k * dim[0] + i) * dim[1] + j] = material(r) + 1; outside++; }
      }
      const used = new Set(store.scene.frames.map((f) => f.id)), names = new Set(store.scene.frames.map((f) => f.name.toLowerCase()));
      const ids = {
        next() { let id = -1; while (used.has(id)) id--; used.add(id); return id; },
        name() { let n = 0; while (names.has(`ergblock_${n}`)) n++; names.add(`ergblock_${n}`); return `ergblock_${n}`; },
      };
      const side = LIMITS.newFrameSide;
      const idx = (i: number, j: number, k: number) => (k * dim[0] + i) * dim[1] + j;
      if (opt.packing === "fit") {
        const at = (i: number, j: number, k: number) => cells[idx(i, j, k)];
        const freeBlocks = LIMITS.newFrames - store.scene.frames.filter((f) => f.new).length;
        for (const b of fitBoxes(cells, dim, Math.max(1, Math.min(freeBlocks, opt.maxBlocks ?? freeBlocks)))) makeBlocks(at, b.lo, b.hi, ids, store, scene.id, blocks);
      } else if (!parts) {
        const at = (i: number, j: number, k: number) => cells[idx(i, j, k)];
        for (let cz = 0; cz < dim[2]; cz += side) for (let cx = 0; cx < dim[0]; cx += side) for (let cy = 0; cy < dim[1]; cy += side) {
          makeBlocks(at, [cx, cy, cz], [Math.min(dim[0], cx + side) - 1, Math.min(dim[1], cy + side) - 1, Math.min(dim[2], cz + side) - 1], ids, store, scene.id, blocks);
        }
      } else {
        for (const g of partBoxes(cells, parts, dim)) {
          const at = (i: number, j: number, k: number) => {
            const n = idx(i, j, k);
            return cells[n] && g.members.some((m) => m.part === parts[n] && i >= m.lo[0] && i <= m.hi[0] && j >= m.lo[1] && j <= m.hi[1] && k >= m.lo[2] && k <= m.hi[2]) ? cells[n] : 0;
          };

          makeBlocks(at, g.lo, g.hi, ids, store, scene.id, blocks);
        }
      }
      for (const b of blocks) {
        b.frame.pos = [(b.frame.pos[0] + lo[0]) * bs, (b.frame.pos[1] + lo[1]) * bs, (b.frame.pos[2] + lo[2]) * bs];
        b.frame.scale = [bs, bs, bs];
      }

    }
  }
  const free = LIMITS.newFrames - store.scene.frames.filter((f) => f.new).length;
  if (blocks.length > free) throw new ToolError(`the structure needs ${blocks.length} new blocks but the level has room for ${free} (at most ${LIMITS.newFrames}); use place "existing" or a smaller structure`);
  if (opt.maxBlocks !== undefined && blocks.length > opt.maxBlocks)
    throw new ToolError(`the structure needs ${blocks.length} new blocks, over maxBlocks ${opt.maxBlocks}${opt.packing === "fit" ? "" : " (packing \"fit\" splits within maxBlocks)"}`);
  const cells = cellBudget(store.scene), blockCells = blocks.reduce((n, b) => n + b.frame.size[0] * b.frame.size[1] * b.frame.size[2], 0);
  if (opt.maxCells !== undefined && blockCells > opt.maxCells)
    throw new ToolError(`the structure's new blocks hold ${blockCells} of the game's voxel cells, over maxCells ${opt.maxCells}: a larger blockScale or scale, packing "parts" or "fit", or a smaller structure (costOnly compares them)`);
  if (blockCells > cells.free)
    throw new ToolError(`the structure's new blocks hold ${blockCells} voxel cells but the level has ${cells.free} of the game's ${cells.max} left ` +
      `(its voxel frames hold ${cells.used}); build it inside existing frames (place "existing"), make it smaller, or remove blocks`);
  for (const b of blocks) added += b.voxels;

  // 3. the runs limit of the edited frames
  for (const [ref, c] of stroke) {
    const g = near.find((x) => x.ref === ref)!;
    const words = g.words.slice();
    for (const [i, [, a]] of c) words[i] = a;
    const runs = voxelRuns(g.base, words).length;
    if (runs > LIMITS.runsPerFrame) throw new ToolError(`frame ${g.id} (${g.frame.name}) would need ${runs} runs (the limit is ${LIMITS.runsPerFrame}): the edit is too scattered` +
      (g.frame.new ? "; in an added block every change of material down a column is a run, so paint fewer material bands (or thinner ones) there, or build the terrain in smaller blocks (heightfield maxCells)" : ""));
  }

  const result = (u: ReturnType<typeof usage>): StructureResult => ({
    label: plan.label, dryRun: opt.dryRun,
    voxels: { added, removed, repainted, changed: added + removed + repainted },
    frames: [...perFrame.values()].sort((a, b) => b.changed - a.changed).slice(0, 60),
    newBlocks: blocks.map((b) => {
      const c = scene ? store.frames.worldOf(scene.id) : null;
      const m: Vec3 = c ? [c[3] + b.frame.pos[0], c[7] + b.frame.pos[1], c[11] + b.frame.pos[2]] : b.frame.pos;
      return { id: b.frame.id, name: b.frame.name, size: b.frame.size, centre: { map: tidy(m), world: tidy(m.map((v) => v * 20) as Vec3) }, voxels: b.voxels,
        cells: b.frame.size[0] * b.frame.size[1] * b.frame.size[2], scale: b.frame.scale?.[0] ?? 1 };
    }),
    cells: { newBlocks: blockCells, level: u.voxelCells.used, free: u.voxelCells.max - u.voxelCells.used, max: u.voxelCells.max },
    outsideFrames: opt.place === "existing" ? 0 : outside,
    bounds: { map: { min: tidy(min), max: tidy(max) }, world: boxToWorld({ min, max }) },
    usage: {
      newBlocks: `${u.newFrames.used}/${u.newFrames.max}`,
      patchBytes: `${u.patchBytes.used}/${u.patchBytes.max} (panel budget ${u.patchBytes.budget})`,
      worstRuns: `${u.runsPerFrame.worst}/${u.runsPerFrame.max}`,
      voxelCells: `${u.voxelCells.used}/${u.voxelCells.max}`,
    },
    notes: [
      ...notes,
      ...(opt.place === "existing" ? ["place existing: parts of the structure that lie outside every voxel frame were not built"] : []),
      ...(!added && !removed && !repainted ? ["no voxel changed: the structure lies outside every voxel frame or the voxels already match"] : []),
    ],
  });

  if (opt.dryRun) {
    const u = usage(store);
    u.newFrames = { used: u.newFrames.used + blocks.length, max: u.newFrames.max };
    u.voxelCells = { ...u.voxelCells, used: u.voxelCells.used + blockCells, free: u.voxelCells.free - blockCells };
    return result(u);
  }
  if (!stroke.size && !blocks.length && !opt.extra?.length) throw new ToolError(`nothing was built: ${result(usage(store)).notes.join('; ')}`);

  // 4. apply as one undo step; take it back if it breaks a limit
  const before = limitProblems(store).length;
  const cmds: Command[] = [
    ...(opt.extra ?? []),
    ...blocks.map((b) => new AddFrame(store.voxels, b.frame, b.words)),
    ...(stroke.size ? [new SculptStroke(plan.label, --strokeId, store.voxels, stroke, () => {})] : []),
  ];
  store.exec(cmds.length === 1 ? cmds[0] : new Group(plan.label, cmds));
  const after = limitProblems(store);
  const u = usage(store);
  if (after.length > before || u.patchBytes.used > PATCH_BUDGET) {
    store.undo();
    throw new ToolError(`the structure would break a patch limit and was undone: ${after.slice(before).join("; ") || `the patch is ${u.patchBytes.used} bytes (budget ${PATCH_BUDGET})`}`);
  }
  return result(u);
}

let strokeId = -1_000_000;
