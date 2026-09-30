// Coalesces the frames touched by strokes, undo and redo into one remesh per animation frame, touched frames only.
export type Schedule = (fn: () => void) => void;

const nextFrame: Schedule = (fn) => {
  const raf = (globalThis as { requestAnimationFrame?: (cb: () => void) => number }).requestAnimationFrame;
  if (raf) raf(fn);
  else setTimeout(fn, 16);
};

export class RemeshQueue {
  private dirty = new Set<number>();
  private pending = false;
  constructor(private run: (frameIds: number[]) => void, private schedule: Schedule = nextFrame) {}

  add(frameIds: Iterable<number>) {
    for (const id of frameIds) this.dirty.add(id);
    if (this.pending || !this.dirty.size) return;
    this.pending = true;
    this.schedule(() => this.flush());
  }

  flush() {
    this.pending = false;
    if (!this.dirty.size) return;
    const ids = [...this.dirty].sort((a, b) => a - b);
    this.dirty.clear();
    this.run(ids);
  }

  get size() { return this.dirty.size; }
}
