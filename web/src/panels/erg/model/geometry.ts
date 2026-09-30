// Frame and detail geometry in .xan units: world positions, the inverse of a frame's matrix, snapping, and the frames
// under which the editor only moves details (their runtime placement does not follow the file's transform).
import { apply, frameLocal, frameWorld, multiply, type Detail, type Frame, type Mat3x4, type Scene, type Vec3 } from "../../../sdk/erg";

export const IDENTITY: Mat3x4 = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0];

export function invert(m: Mat3x4): Mat3x4 | null {
  const [a, b, c, tx, d, e, f, ty, g, h, i, tz] = m;
  const A = e * i - f * h, B = f * g - d * i, C = d * h - e * g;
  const det = a * A + b * B + c * C;
  if (!Number.isFinite(det) || Math.abs(det) < 1e-12) return null;
  const r = 1 / det;
  const inv = [
    A * r, (c * h - b * i) * r, (b * f - c * e) * r,
    B * r, (a * i - c * g) * r, (c * d - a * f) * r,
    C * r, (b * g - a * h) * r, (a * e - b * d) * r,
  ];
  return [
    inv[0], inv[1], inv[2], -(inv[0] * tx + inv[1] * ty + inv[2] * tz),
    inv[3], inv[4], inv[5], -(inv[3] * tx + inv[4] * ty + inv[5] * tz),
    inv[6], inv[7], inv[8], -(inv[6] * tx + inv[7] * ty + inv[8] * tz),
  ];
}

export const round = (v: number, places = 4) => {
  const k = 10 ** places;
  const r = Math.round(v * k) / k;
  return r === 0 ? 0 : r;
};
export const roundVec = (v: Vec3, places = 4): Vec3 => [round(v[0], places), round(v[1], places), round(v[2], places)];
export const snap = (v: number, step: number | null) => (step ? round(Math.round(v / step) * step) : round(v));
export const snapVec = (v: Vec3, step: number | null): Vec3 => [snap(v[0], step), snap(v[1], step), snap(v[2], step)];

export type FrameIndex = Map<number, Frame>;
export const frameIndex = (s: Scene): FrameIndex => new Map(s.frames.map((f) => [f.id, f]));

/** Cached world matrices and inverses of a scene's frames (frames do not change in the editor). */
export class Frames {
  readonly byId: FrameIndex;
  private world = new Map<number, Mat3x4 | null>();
  private inv = new Map<number, Mat3x4 | null>();
  constructor(scene: Scene) { this.byId = frameIndex(scene); }
  worldOf(id: number): Mat3x4 | null {
    if (!this.world.has(id)) this.world.set(id, frameWorld(this.byId, id));
    return this.world.get(id)!;
  }
  inverseOf(id: number): Mat3x4 | null {
    if (!this.inv.has(id)) {
      const w = this.worldOf(id);
      this.inv.set(id, w ? invert(w) : null);
    }
    return this.inv.get(id)!;
  }
  /** A detail's position in the level. */
  detailWorld(d: Pick<Detail, "frame" | "pos">): Vec3 {
    const w = this.worldOf(d.frame);
    return w ? apply(w, d.pos) : [d.pos[0], d.pos[1], d.pos[2]];
  }
  /** A detail's full matrix: frame * T(pos) * Rz Ry Rx(rot) * S(scale). */
  detailMatrix(d: Pick<Detail, "frame" | "pos" | "rot" | "scale">): Mat3x4 {
    return multiply(this.worldOf(d.frame) ?? IDENTITY, frameLocal(d));
  }
  /** A level position in the frame's own space. */
  toLocal(frame: number, p: Vec3): Vec3 {
    const inv = this.inverseOf(frame);
    return inv ? apply(inv, p) : [p[0], p[1], p[2]];
  }
}

// Frames whose children the engine does not place from the file transform (the swinging lamps' bowls in
// Deathmatch1); details under them are edited by translation only and their positions shown are approximate.
const ANIMATED: Record<string, number[]> = { deathmatch1: [325, 329, 333, 337, 341] };

export function translationOnlyFrames(scene: Scene): Set<number> {
  const file = scene.base.file.toLowerCase().replace(/^multi[._]/, "");
  const out = new Set<number>(ANIMATED[file] ?? []);
  const byId = frameIndex(scene);
  for (const f of scene.frames) {
    let cur: Frame | undefined = f;
    for (let hops = 0; cur && hops <= scene.frames.length; hops++) {
      if (/hanginglamp/i.test(cur.name) || out.has(cur.id)) {
        out.add(f.id);
        break;
      }
      cur = cur.parent === null ? undefined : byId.get(cur.parent);
    }
  }
  return out;
}

/** True when a point lies over the frame's voxel box footprint and not below it (the air above counts). */
export function overFrame(frames: Frames, f: Frame, p: Vec3, margin = 1): boolean {
  if (f.folder || !f.size[0] || !f.size[2] || f.parent === null) return false;
  const q = frames.toLocal(f.id, p);
  return q[0] >= -margin && q[0] <= f.size[0] + margin && q[2] >= -margin && q[2] <= f.size[2] + margin && q[1] >= -margin;
}
