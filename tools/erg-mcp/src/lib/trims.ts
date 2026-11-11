// Trimming the base map's voxel frames (Scene.trims, the patch's trimFrame op): the game counts X*Y*Z of every voxel
// frame in 16 bits (65 535 cells), solid or empty, so a wiped base still spends its cells until its frames are cut down.
// A trim keeps the box [min, min + size) of a frame's grid in the built level; the scene keeps the whole grid, so
// every other tool reads and edits the frame as before. Every solid voxel must stay inside the box (the server refuses
// the save otherwise), and no detail on the frame may be tied to a voxel (voxelPos). An all-empty frame is trimmed to
// one cell and stays where it is; a box holding solid voxels moves the frame by the box's offset from the grid's centre
// and its details and child frames back by as much (src/erg/build.cpp), so nothing moves in the level.
import { Group, SetTrims, isSolid, type Box, type EditorStore, type FrameTrim, type Vec3 } from "../core";
import { ToolError } from "../registry";
import { grids, VoxelEdit, type Grid } from "./voxel";

export interface FramePick {
  frames?: number[];             // frame ids
  search?: string;               // name contains (any case)
  box?: Box;                     // map units: frames whose voxel box overlaps it
  all?: boolean;                 // every base frame with voxels
}

/** The base frames (never added blocks) a pick names, by the grids' scene order. */
export function pickBaseFrames(store: EditorStore, p: FramePick): Grid[] {
  if (!p.frames && !p.search && !p.box && !p.all) throw new ToolError("give frames, search or box, or all: true for every base frame");
  let gs = grids(store).filter((g) => !g.frame.new);
  if (p.frames) {
    const ids = new Set(p.frames);
    for (const id of ids) {
      const g = grids(store).find((x) => x.id === id);
      if (!g) throw new ToolError(`frame ${id} has no voxels`);
      if (g.frame.new) throw new ToolError(`frame ${id} is a block added in this project: remove_block removes it`);
    }
    gs = gs.filter((g) => ids.has(g.id));
  }
  const needle = p.search?.toLowerCase();
  if (needle) gs = gs.filter((g) => g.frame.name.toLowerCase().includes(needle));
  if (p.box) {
    const b = p.box;
    gs = gs.filter((g) => [0, 1, 2].every((k) => g.bounds.max[k] > b.min[k] && g.bounds.min[k] < b.max[k]));
  }
  return gs;
}

/** The smallest box holding every solid voxel of the words, or null for none. */
export function solidBox(size: Vec3, words: ArrayLike<number>): { min: Vec3; size: Vec3; solid: number } | null {
  const lo: Vec3 = [Infinity, Infinity, Infinity], hi: Vec3 = [-1, -1, -1];
  let solid = 0;
  for (let i = 0; i < words.length; i++) {
    if (!isSolid(words[i])) continue;
    solid++;
    const y = i % size[1], r = (i - y) / size[1], x = r % size[0], z = (r - x) / size[0];
    const c = [x, y, z];
    for (let k = 0; k < 3; k++) { lo[k] = Math.min(lo[k], c[k]); hi[k] = Math.max(hi[k], c[k]); }
  }
  return solid ? { min: lo, size: [hi[0] - lo[0] + 1, hi[1] - lo[1] + 1, hi[2] - lo[2] + 1], solid } : null;
}

const cells = (s: Vec3) => s[0] * s[1] * s[2];
const tied = (store: EditorStore, frame: number) => store.scene.details.find((d) => d.frame === frame && d.voxelPos.some((v) => v !== 0));

export interface TrimRow { id: number; name: string; size: Vec3; keep: FrameTrim; solid: number; cellsBefore: number; cellsAfter: number; moves: boolean; }
export interface Skipped { id: number; name: string; why: string; }

/** What a trim of each picked frame would keep: the box of its solid voxels (`emptyOnly`: only frames with none, to one
 * cell). Frames already that small, or holding a detail tied to a voxel, are skipped with why. */
export function planTrims(store: EditorStore, gs: Grid[], emptyOnly: boolean, carveAll = false): { rows: TrimRow[]; skipped: Skipped[] } {
  const rows: TrimRow[] = [], skipped: Skipped[] = [];
  const trims = new Map((store.scene.trims ?? []).map((t) => [t.frame, t]));
  for (const g of gs) {
    const d = tied(store, g.id);
    if (d) { skipped.push({ id: g.id, name: g.frame.name, why: `detail ${d.id} '${d.name}' is tied to one of its voxels (voxelPos); remove it first` }); continue; }
    const box = carveAll ? null : solidBox(g.size, g.words);
    if (box && emptyOnly) { skipped.push({ id: g.id, name: g.frame.name, why: `${box.solid} solid voxels (remove_base_frames carves them, or trim_base_frames {keep: "solid"} keeps them)` }); continue; }
    const keep: FrameTrim = { frame: g.id, min: box ? box.min : [0, 0, 0], size: box ? box.size : [1, 1, 1] };
    const was = trims.get(g.id);
    const before = cells(was?.size ?? g.size), after = cells(keep.size);
    // remove_base_frames (carveAll) still empties a frame of one cell that holds something
    const holds = carveAll && g.words.some((v) => v !== 0);
    if (!holds && after >= before && (!was || (was.min.every((v, k) => v === keep.min[k]) && was.size.every((v, k) => v === keep.size[k])))) {
      skipped.push({ id: g.id, name: g.frame.name, why: box ? "its solid voxels fill its grid" : "already one cell" });
      continue;
    }
    const moves = !!box && [0, 1, 2].some((k) => 2 * keep.min[k] + keep.size[k] !== g.size[k]);
    rows.push({ id: g.id, name: g.frame.name, size: g.size, keep, solid: box?.solid ?? 0, cellsBefore: before, cellsAfter: after, moves });
  }
  return { rows, skipped };
}

/** Applies the trims (and, with `carve`, the carving of every solid voxel of those frames first) as one undo step. */
export function applyTrims(store: EditorStore, rows: TrimRow[], carve: VoxelEdit | null, label: string) {
  const set = new SetTrims(new Map(rows.map((r) => [r.id, r.keep])));
  store.exec(carve && carve.size ? new Group(label, [carve.command(label), set]) : set);
}

/** Empties the frames' grids, writing a 0 word in every cell that holds anything (a solid voxel, or an empty one with
 * material bits): one patch run a frame, into one batch. */
export function carveFrames(store: EditorStore, gs: Grid[]): VoxelEdit {
  const e = new VoxelEdit(store);
  for (const g of gs)
    for (let i = 0; i < g.words.length; i++) {
      if (!g.words[i]) continue;
      const y = i % g.size[1], r = (i - y) / g.size[1], x = r % g.size[0], z = (r - x) / g.size[0];
      e.set(g.id, x, y, z, 0);
    }
  return e;
}

/** Drops the trims of the frames (or every trim); one undo step. */
export function clearTrims(store: EditorStore, frames?: number[]): number[] {
  const ids = (store.scene.trims ?? []).map((t) => t.frame).filter((id) => !frames || frames.includes(id));
  if (ids.length) store.exec(new SetTrims(new Map(ids.map((id) => [id, null]))));
  return ids;
}
