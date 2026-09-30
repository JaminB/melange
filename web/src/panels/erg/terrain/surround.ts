// Painting the surround (.hmp): 100x100 heights 0..1, row-major. Strokes are undoable commands (a drag merges its steps);
// heights are kept on a 1/256 grid so the patch runs stay short and read back exactly.
import { LIMITS, type Command, type Scene, type Surround } from "../../../sdk/erg";

export const SURROUND_SIDE = LIMITS.hmpSide;
export const MAX_SURROUND_RADIUS = 25;
export type SurroundMode = "raise" | "lower" | "flatten" | "smooth";
export interface SurroundBrush { mode: SurroundMode; radius: number; strength: number; level: number; }

const HEIGHT_BYTES = LIMITS.hmpCells * 4;

export const quantizeHeight = (v: number) => Math.min(1, Math.max(0, Math.round(v * 256) / 256));

export function emptySurround(): Surround {
  return { heights: new Float32Array(LIMITS.hmpCells), blend: new Uint8Array(LIMITS.hmpCells) };
}

export function copySurround(s: Surround): Surround {
  return { heights: s.heights.slice(), blend: s.blend.slice() };
}

/** The surround a loaded scene carries (hmp.ref), or null. */
export function surroundOf(l: { scene: Scene; blobs: Map<number, ArrayBuffer> }): Surround | null {
  const ref = l.scene.hmp.ref;
  const b = ref !== undefined ? l.blobs.get(ref) : undefined;
  if (!b || b.byteLength !== LIMITS.hmpBytes) return null;
  return { heights: new Float32Array(b.slice(0, HEIGHT_BYTES)), blend: new Uint8Array(b.slice(HEIGHT_BYTES)) };
}

export function surroundBytes(s: Surround): ArrayBuffer {
  const out = new Uint8Array(LIMITS.hmpBytes);
  out.set(new Uint8Array(s.heights.buffer, s.heights.byteOffset, HEIGHT_BYTES), 0);
  out.set(s.blend, HEIGHT_BYTES);
  return out.buffer;
}

type Changes = Map<number, [number, number]>;

/** The cells one dab changes around (cx, cy): [before, after] heights. */
export function dabChanges(h: Float32Array, cx: number, cy: number, b: SurroundBrush): Changes {
  const out: Changes = new Map();
  const r = Math.max(1, Math.min(MAX_SURROUND_RADIUS, b.radius)), k = Math.min(1, Math.max(0, b.strength));
  for (let y = Math.max(0, Math.ceil(cy - r)); y <= Math.min(SURROUND_SIDE - 1, Math.floor(cy + r)); y++)
    for (let x = Math.max(0, Math.ceil(cx - r)); x <= Math.min(SURROUND_SIDE - 1, Math.floor(cx + r)); x++) {
      const d = Math.hypot(x - cx, y - cy) / r;
      if (d > 1) continue;
      const w = k * (1 - d * d), i = y * SURROUND_SIDE + x, was = h[i];
      let next = was;
      if (b.mode === "raise") next = was + 0.1 * w;
      else if (b.mode === "lower") next = was - 0.1 * w;
      else if (b.mode === "flatten") next = was + (b.level - was) * w;
      else {
        let sum = 0, n = 0;
        for (let dy = -1; dy <= 1; dy++)
          for (let dx = -1; dx <= 1; dx++) {
            const xx = x + dx, yy = y + dy;
            if (xx < 0 || yy < 0 || xx >= SURROUND_SIDE || yy >= SURROUND_SIDE) continue;
            sum += h[yy * SURROUND_SIDE + xx];
            n++;
          }
        next = was + (sum / n - was) * w;
      }
      const q = quantizeHeight(next);
      if (Math.fround(q) !== was) out.set(i, [was, Math.fround(q)]);
    }
  return out;
}

export class SurroundStroke implements Command {
  constructor(readonly label: string, readonly stroke: number, private s: Surround, private changes: Changes) {}
  get size() { return this.changes.size; }
  do() { for (const [i, [, after]] of this.changes) this.s.heights[i] = after; }
  undo() { for (const [i, [before]] of this.changes) this.s.heights[i] = before; }
  merge(next: Command): boolean {
    if (!(next instanceof SurroundStroke) || next.stroke !== this.stroke) return false;
    for (const [i, [before, after]] of next.changes) {
      const had = this.changes.get(i);
      if (!had) this.changes.set(i, [before, after]);
      else if (had[0] === after) this.changes.delete(i);
      else had[1] = after;
    }
    return true;
  }
}

export interface SurroundHost { surround: Surround; exec(cmd: Command, mergeable: boolean): void; }

export class SurroundPainter {
  brush: SurroundBrush = { mode: "raise", radius: 4, strength: 0.5, level: 0 };
  private stroke = 0;
  private open = false;

  constructor(private host: SurroundHost) {}

  setBrush(b: Partial<SurroundBrush>) {
    const n = { ...this.brush, ...b };
    n.radius = Math.min(MAX_SURROUND_RADIUS, Math.max(1, Math.round(n.radius)));
    n.strength = Math.min(1, Math.max(0.05, n.strength));
    n.level = quantizeHeight(n.level);
    this.brush = n;
  }

  begin() { this.stroke++; this.open = true; }
  end() { this.open = false; }

  /** One dab at a cell position (fractional x, y in 0..100); returns the number of cells changed. */
  step(cx: number, cy: number): number {
    if (!this.open) this.begin();
    const c = dabChanges(this.host.surround.heights, cx, cy, this.brush);
    if (!c.size) return 0;
    const label = `${this.brush.mode[0].toUpperCase()}${this.brush.mode.slice(1)} surround`;
    this.host.exec(new SurroundStroke(label, this.stroke, this.host.surround, c), true);
    return c.size;
  }

  /** The height under a cell, for picking the flatten level. */
  heightAt(x: number, y: number): number {
    const xi = Math.min(SURROUND_SIDE - 1, Math.max(0, Math.floor(x))), yi = Math.min(SURROUND_SIDE - 1, Math.max(0, Math.floor(y)));
    return this.host.surround.heights[yi * SURROUND_SIDE + xi];
  }
}
