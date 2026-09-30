// The terrain tool as the viewport drives it: rays in .xan units (the view divides world positions by 20) from pointer
// hover, down, drag and up. A drag is one undo step; a step runs only when the brush moves to another voxel.
import type { Vec3 } from "../../../sdk/erg";
import { MAX_BRUSH, type Anchor, type Brush } from "./brush";
import type { Box } from "./voxel";
import type { Sculptor, StepResult } from "./sculpt";

export interface Preview { box: Box; frames: number[]; frame: number; }

export class TerrainTool {
  brush: Brush = { mode: "carve", shape: "sphere", size: [5, 5, 5], material: 0 };
  last?: StepResult;
  private dragging = false;
  private lastKey = "";
  private listeners = new Set<() => void>();

  constructor(readonly sculptor: Sculptor) {}

  setBrush(b: Partial<Brush>) {
    const next = { ...this.brush, ...b };
    next.size = next.size.map((v) => Math.min(MAX_BRUSH, Math.max(1, Math.round(v)))) as Vec3;
    if (next.material !== "column") next.material = Math.min(63, Math.max(0, Math.round(next.material)));
    if (next.mode === "paint" && next.material === "column") next.material = 0;
    this.brush = next;
    this.emit();
  }

  /** Grows or shrinks every axis by one voxel. */
  resize(delta: number) { this.setBrush({ size: this.brush.size.map((v) => v + delta) as Vec3 }); }

  hover(origin: Vec3, dir: Vec3): Preview | null {
    const a = this.anchorFor(origin, dir);
    return a ? { ...this.sculptor.preview(a, this.brush), frame: a.grid.frame.id } : null;
  }

  down(origin: Vec3, dir: Vec3): StepResult | null {
    this.dragging = true;
    this.lastKey = "";
    this.sculptor.begin();
    return this.drag(origin, dir);
  }

  drag(origin: Vec3, dir: Vec3): StepResult | null {
    if (!this.dragging) return null;
    const a = this.anchorFor(origin, dir);
    if (!a) return null;
    const key = `${a.grid.ref}:${a.center.join()}`;
    if (key === this.lastKey) return null;
    this.lastKey = key;
    this.last = this.sculptor.step(a, this.brush);
    this.emit();
    return this.last;
  }

  up() {
    this.dragging = false;
    this.sculptor.end();
  }

  /** "[" and "]" resize the brush; returns true when the key was used. */
  key(e: { key: string; ctrlKey?: boolean; metaKey?: boolean; altKey?: boolean }): boolean {
    if (e.ctrlKey || e.metaKey || e.altKey) return false;
    if (e.key === "[") this.resize(-1);
    else if (e.key === "]") this.resize(1);
    else return false;
    return true;
  }

  onChange(fn: () => void): () => void {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  private anchorFor(origin: Vec3, dir: Vec3): Anchor | null {
    const hit = this.sculptor.pick(origin, dir);
    return hit ? this.sculptor.anchor(hit, this.brush) : null;
  }

  private emit() { for (const fn of [...this.listeners]) fn(); }
}
