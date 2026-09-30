// One open project in the editor: the pinned base (for the patch), the working scene, voxels, the undo stack and the
// selection. The working scene is edited only through commands.
import {
  CommandStack, MAX_UNDO, toPatch, validatePatch, type Command, type Frame, type Patch, type Scene, type Surround,
} from "../../../sdk/erg";
import { copySurround, emptySurround, surroundOf } from "../terrain/surround";
import { Frames, translationOnlyFrames } from "./geometry";

export interface Loaded { scene: Scene; blobs: Map<number, ArrayBuffer>; }

export class EditorStore {
  readonly base: Scene;
  readonly scene: Scene;
  readonly frames: Frames;
  readonly translationOnly: Set<number>;
  readonly baseVoxels = new Map<number, Uint32Array>();    // base blob ref -> base voxels
  readonly voxels = new Map<number, Uint32Array>();        // base blob ref -> edited voxels
  readonly baseSurround: Surround | null;                  // the base's .hmp (null: none, painting starts from zeros)
  readonly surround: Surround;                             // the painted surround (in the patch with hmp paint)
  readonly stack: CommandStack;
  selection: number[] = [];
  version = 0;
  private saved: string;
  private listeners = new Set<(why: Change) => void>();

  constructor(readonly project: string, base: Loaded, current: Loaded, limit = MAX_UNDO) {
    this.base = base.scene;
    this.scene = structuredClone(current.scene);
    this.frames = new Frames(this.scene);
    this.translationOnly = translationOnlyFrames(this.scene);
    const cur = new Map(current.scene.frames.map((f) => [f.id, f]));
    for (const f of base.scene.frames) {
      if (f.voxels === null) continue;
      const b = base.blobs.get(f.voxels);
      if (!b) continue;
      const bv = new Uint32Array(b.slice(0));
      this.baseVoxels.set(f.voxels, bv);
      const cf = cur.get(f.id);
      const cb = cf && cf.voxels !== null ? current.blobs.get(cf.voxels) : undefined;
      this.voxels.set(f.voxels, cb && cb.byteLength === b.byteLength ? new Uint32Array(cb.slice(0)) : bv.slice());
    }
    this.baseSurround = surroundOf(base);
    const painted = current.scene.hmp.mode === "paint" ? surroundOf(current) : null;
    this.surround = copySurround(painted ?? this.baseSurround ?? emptySurround());
    this.stack = new CommandStack(this.scene, limit);
    this.saved = JSON.stringify(this.patch());
  }

  /** The surround can be painted: the base's .hmp arrived, or the base has none (painting starts from zeros). */
  get canPaintSurround() { return this.baseSurround !== null || this.base.base.sha256.hmp === null; }

  /** Voxels of a frame by its id (the base's ref). */
  voxelsOf(f: Pick<Frame, "id">): Uint32Array | undefined {
    const bf = this.base.frames.find((x) => x.id === f.id);
    return bf && bf.voxels !== null ? this.voxels.get(bf.voxels) : undefined;
  }

  patch(): Patch {
    return toPatch(this.base, this.scene, { base: this.baseVoxels, edited: this.voxels }, { base: this.baseSurround, edited: this.surround });
  }
  patchText(): string {
    if (this.textAt !== this.version) {
      this.text = JSON.stringify(this.patch());
      this.textAt = this.version;
    }
    return this.text;
  }
  private text = "";
  private textAt = -1;
  get dirty(): boolean { return this.patchText() !== this.saved; }
  markSaved(text = this.patchText()) {
    this.saved = text;
    this.emit("saved");
  }
  savedText() { return this.saved; }
  problems(): string[] {
    const p = this.patch();
    return validatePatch(p, JSON.stringify(p).length).errors;
  }

  exec(cmd: Command, mergeable = false) {
    this.stack.exec(cmd, mergeable);
    this.dropMissingSelection();
    this.emit("scene");
  }
  undo() {
    if (!this.stack.undo()) return false;
    this.dropMissingSelection();
    this.emit("scene");
    return true;
  }
  redo() {
    if (!this.stack.redo()) return false;
    this.dropMissingSelection();
    this.emit("scene");
    return true;
  }
  select(ids: number[]) {
    const known = new Set(this.scene.details.map((d) => d.id));
    this.selection = [...new Set(ids)].filter((id) => known.has(id));
    this.emit("selection");
  }
  detail(id: number) { return this.scene.details.find((d) => d.id === id); }
  selected() { return this.selection.map((id) => this.detail(id)!).filter(Boolean); }

  on(fn: (why: Change) => void): () => void {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }
  private dropMissingSelection() {
    if (!this.selection.length) return;
    const known = new Set(this.scene.details.map((d) => d.id));
    this.selection = this.selection.filter((id) => known.has(id));
  }
  private emit(why: Change) {
    this.version++;
    for (const fn of [...this.listeners]) fn(why);
  }
}

export type Change = "scene" | "selection" | "saved";
