// Sculpting against the editor's scene: strokes become one undoable command each (a drag merges its steps), touched
// frames are remeshed, and a step that would push the patch past the server's limits is refused before it is applied. The
// block mode adds a new terrain block instead (see blocks.ts).
import { LIMITS, RemoveFrame, voxelRuns, type Command, type CommandStack, type Frame, type Scene, type Vec3 } from "../../../sdk/erg";
import { blockBox, blockCentre, blockFrames, blockKey, planBlock } from "./blocks";
import { anchorAt, strokeChanges, worldBox, type Anchor, type Brush } from "./brush";
import { pick, type Hit } from "./pick";
import { gridFrames, overlaps, type Box, type GridFrame } from "./voxel";

export interface TerrainHost {
  scene: Scene;
  voxels: Map<number, Uint32Array>;   // base blob ref -> the edited words (the scene the view draws)
  base: Map<number, Uint32Array>;     // base blob ref -> the words as loaded (what toPatch diffs against)
  refOf(id: number): number | null;   // a frame's key in voxels and base
  stack: CommandStack;
  remesh(frameIds: number[]): void;   // the mesher rebuilds these frames only
  freshRef?(): number;                // a blob ref for an added block
}

type Changes = Map<number, Map<number, [number, number]>>;

/** One brush stroke over any number of frames. Steps of the same stroke merge into it. */
export class SculptStroke implements Command {
  constructor(readonly label: string, readonly stroke: number, private voxels: Map<number, Uint32Array>,
    private changes: Changes, private notify: (refs: number[]) => void) {}
  get size() { let n = 0; for (const c of this.changes.values()) n += c.size; return n; }
  refs() { return [...this.changes.keys()]; }
  do() { this.write(1); }
  undo() { this.write(0); }
  merge(next: Command): boolean {
    if (!(next instanceof SculptStroke) || next.stroke !== this.stroke) return false;
    for (const [ref, add] of next.changes) {
      const mine = this.changes.get(ref) ?? new Map<number, [number, number]>();
      for (const [i, [before, after]] of add) {
        const had = mine.get(i);
        if (!had) mine.set(i, [before, after]);
        else if (had[0] === after) mine.delete(i);
        else had[1] = after;
      }
      if (mine.size) this.changes.set(ref, mine);
      else this.changes.delete(ref);
    }
    return true;
  }
  private write(which: 0 | 1) {
    for (const [ref, c] of this.changes) {
      const v = this.voxels.get(ref);
      if (!v) throw new Error(`no voxel blob ${ref}`);
      for (const [i, pair] of c) v[i] = pair[which];
    }
    this.notify(this.refs());
  }
}

export interface StepResult { changed: number; frames: number[]; refused?: string; ms: number; }

// JSON bytes of a run, "[start,count,value],"
const runBytes = (r: [number, number, number]) => String(r[0]).length + String(r[1]).length + String(r[2]).length + 5;
const PATCH_BUDGET = LIMITS.patchBytes - (512 << 10);   // headroom for the detail ops and the envelope

const now = () => (typeof performance !== "undefined" ? performance.now() : Date.now());

export class Sculptor {
  private grids: GridFrame[] = [];
  private frameOfRef = new Map<number, number>();
  private patchCost = new Map<number, number>();   // ref -> JSON bytes of its runs
  private stroke = 0;
  private open = false;
  private blocks = "";

  constructor(private host: TerrainHost) { this.refresh(); }

  /** Rebuilds the frame caches (after a load or a reload of the scene). */
  refresh() {
    this.blocks = blockKey(this.host.scene);
    this.grids = gridFrames(this.host.scene, this.host.voxels, (id) => this.host.refOf(id));
    this.frameOfRef = new Map(this.grids.map((g) => [g.ref, g.frame.id]));
    this.patchCost.clear();
    for (const g of this.grids) this.patchCost.set(g.ref, this.cost(g.ref, this.host.voxels.get(g.ref)!));
  }

  /** Blocks come and go with their commands and with undo, so the caches follow the scene. */
  private sync() { if (blockKey(this.host.scene) !== this.blocks) this.refresh(); }

  get frames(): readonly GridFrame[] { this.sync(); return this.grids; }

  pick(origin: Vec3, dir: Vec3): Hit | null { this.sync(); return pick(this.grids, this.host.voxels, origin, dir); }

  anchor(hit: Hit, b: Brush): Anchor { return anchorAt(hit, b.mode); }

  /** The added blocks, in the order they were added. */
  get blockList(): Frame[] { return blockFrames(this.host.scene); }

  /** Adds a block of the brush's size and material resting where the ray met the terrain. */
  placeBlock(a: Anchor, b: Brush): StepResult {
    const t0 = now();
    if (!a.point || !this.host.freshRef) return { changed: 0, frames: [], ms: now() - t0, refused: "Blocks cannot be added here" };
    const centre = blockCentre(this.host.scene, a.point, b.size);
    if (!centre) return { changed: 0, frames: [], ms: now() - t0, refused: "This level has no Scene frame to add blocks under" };
    const cmd = planBlock(this.host.scene, this.host.voxels, this.host.freshRef(), centre, b.size, b.material === "column" ? 0 : b.material);
    if (typeof cmd === "string") return { changed: 0, frames: [], ms: now() - t0, refused: cmd };
    this.host.stack.exec(cmd);
    this.refresh();
    return { changed: b.size[0] * b.size[1] * b.size[2], frames: [cmd.frame.id], ms: now() - t0 };
  }

  removeBlock(id: number) {
    this.host.stack.exec(new RemoveFrame(this.host.voxels, id));
    this.refresh();
  }

  /** The brush outline (world box, .xan units) and the frames it reaches, for the cursor: every overlapping frame for
   * carve, the anchor frame alone for fill and paint, none for a block. */
  preview(a: Anchor, b: Brush): { box: Box; frames: number[] } {
    if (b.mode === "block") {
      const centre = a.point && blockCentre(this.host.scene, a.point, b.size);
      const box = centre && blockBox(this.host.scene, centre, b.size);
      return { box: box ?? worldBox(a, b), frames: [] };
    }
    const box = worldBox(a, b);
    const targets = b.mode === "carve" ? this.grids : [a.grid];
    return { box, frames: targets.filter((g) => overlaps(box, g.bounds)).map((g) => g.frame.id) };
  }

  /** Starts a stroke: the steps until end() are one undo step. */
  begin() { this.stroke++; this.open = true; }
  end() { this.open = false; }

  /** Applies the brush once at the anchor; refused when a frame would pass 2000 runs or the patch 3.5 MB. */
  step(a: Anchor, b: Brush): StepResult {
    if (b.mode === "block") return this.placeBlock(a, b);
    this.sync();
    const t0 = now();
    if (!this.open) this.begin();
    const changes = strokeChanges(this.grids, this.host.voxels, a, b);
    const frames = [...changes.keys()].map((r) => this.frameOfRef.get(r)!);
    let changed = 0;
    for (const c of changes.values()) changed += c.size;
    if (!changed) return { changed: 0, frames: [], ms: now() - t0 };

    let total = 0;
    for (const v of this.patchCost.values()) total += v;
    for (const [ref, c] of changes) {
      const words = this.host.voxels.get(ref)!.slice();
      for (const [i, [, after]] of c) words[i] = after;
      const runs = voxelRuns(this.baseOf(ref, words.length), words);
      if (runs.length > LIMITS.runsPerFrame)
        return { changed: 0, frames: [], ms: now() - t0,
          refused: `frame ${this.frameOfRef.get(ref)} would need ${runs.length} runs (the limit is ${LIMITS.runsPerFrame}): its edits are too scattered` };
      total += runs.reduce((n, r) => n + runBytes(r), 0) - (this.patchCost.get(ref) ?? 0);
    }
    if (total > PATCH_BUDGET)
      return { changed: 0, frames: [], ms: now() - t0, refused: "the terrain edits would make the patch larger than the server accepts" };

    const cmd = new SculptStroke(`${b.mode[0].toUpperCase()}${b.mode.slice(1)} terrain`, this.stroke, this.host.voxels, changes,
      (refs) => this.onWrite(refs));
    this.host.stack.exec(cmd, true);
    return { changed, frames, ms: now() - t0 };
  }

  /** The JSON cost of every frame's runs so far (bytes); the panel shows it against the limit. */
  get patchBytes() { let n = 0; for (const v of this.patchCost.values()) n += v; return n; }

  private onWrite(refs: number[]) {
    for (const ref of refs) this.patchCost.set(ref, this.cost(ref, this.host.voxels.get(ref)!));
    this.host.remesh(refs.map((r) => this.frameOfRef.get(r)!).filter((id) => id !== undefined));
  }

  /** The words toPatch diffs against: the base's, or an added block's empty grid. */
  private baseOf(ref: number, n: number) { return this.host.base.get(ref) ?? new Uint32Array(n); }

  private cost(ref: number, words: Uint32Array) {
    return voxelRuns(this.baseOf(ref, words.length), words).reduce((n, r) => n + runBytes(r), 0);
  }
}
