// Gameplay-distance size of props (detail layer round 9; the round-8 judge's tool gap "no gameplay-distance preview that
// shows which props drop below about 3 px, to cull sub-pixel flora"). Pure: a camera (position, target, vertical field of
// view, image size) and a prop's bounding box in, the prop's on-screen size in pixels out (the projected diameter of the
// box's bounding sphere, and of its height), and whether it is in the frame at all. tools/sightsize.ts reads the placed
// meshes and calls this per camera.

export type V3 = [number, number, number];
export interface Cam { pos: V3; target: V3; fov?: number; width?: number; height?: number }
export interface Seen { inView: boolean; dist: number; px: number; pxHeight: number; screen: [number, number] | null }

const sub = (a: V3, b: V3): V3 => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const dot = (a: V3, b: V3) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const cross = (a: V3, b: V3): V3 => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const norm = (a: V3): V3 => { const l = Math.hypot(...a) || 1; return [a[0] / l, a[1] / l, a[2] / l]; };

/** The camera's basis: forward, right and up (world up is +y). */
export function basis(c: Cam): { f: V3; r: V3; u: V3 } {
  const f = norm(sub(c.target, c.pos));
  let r = cross(f, [0, 1, 0]);
  if (Math.hypot(...r) < 1e-6) r = [1, 0, 0];
  r = norm(r);
  return { f, r, u: cross(r, f) };
}

/** Where a box of `min`..`max` lands in the camera's image: its centre's pixel, its bounding sphere's diameter and its
 * height in pixels (at its centre's depth), and whether any of it is in the frame (the sphere against the frustum). */
export function seenBy(c: Cam, min: V3, max: V3): Seen {
  const W = c.width ?? 1280, H = c.height ?? 720, fov = ((c.fov ?? 55) * Math.PI) / 180;
  const ctr: V3 = [(min[0] + max[0]) / 2, (min[1] + max[1]) / 2, (min[2] + max[2]) / 2];
  const rad = Math.hypot(max[0] - min[0], max[1] - min[1], max[2] - min[2]) / 2;
  const { f, r, u } = basis(c), d = sub(ctr, c.pos), z = dot(d, f), x = dot(d, r), y = dot(d, u);
  const dist = Math.hypot(...d);
  const ty = Math.tan(fov / 2), tx = ty * (W / H);
  // pixels per unit at depth z
  const ppu = z > 1e-6 ? H / 2 / (z * ty) : 0;
  const px = 2 * rad * ppu, pxHeight = (max[1] - min[1]) * ppu;
  // in view: the sphere's centre within the frustum widened by its radius (and in front of the camera)
  const slackX = rad * Math.sqrt(1 + tx * tx), slackY = rad * Math.sqrt(1 + ty * ty);
  const inView = z + rad > 0.05 && Math.abs(x) <= z * tx + slackX && Math.abs(y) <= z * ty + slackY;
  const screen: [number, number] | null = z > 1e-6 ? [W / 2 + (x / (z * tx)) * (W / 2), H / 2 - (y / (z * ty)) * (H / 2)] : null;
  return { inView, dist, px, pxHeight, screen };
}

export interface PropBox { id: number; min: V3; max: V3; tag?: string }
export interface PixelReport {
  cams: { name: string; inView: number; under: number; median: number | null; underIds: number[] }[];
  /** per prop: its largest size over the cameras that see it (null: no camera sees it) */
  best: { id: number; px: number | null; tag?: string }[];
  /** props under minPx in every camera that sees them, or seen by none */
  subPixel: number[];
}

/** Each prop's size in each camera; `minPx` (default 3) is the cull line. */
export function pixelReport(props: PropBox[], cams: (Cam & { name: string })[], minPx = 3): PixelReport {
  const best = new Map<number, number | null>(props.map((p) => [p.id, null]));
  const out: PixelReport["cams"] = cams.map((c) => {
    const sizes: number[] = [], under: number[] = [];
    for (const p of props) {
      const s = seenBy(c, p.min, p.max);
      if (!s.inView) continue;
      sizes.push(s.px);
      if (s.px < minPx) under.push(p.id);
      const b = best.get(p.id);
      if (b === null || b === undefined || s.px > b) best.set(p.id, s.px);
    }
    sizes.sort((a, b) => a - b);
    return { name: c.name, inView: sizes.length, under: under.length, median: sizes.length ? Math.round(sizes[sizes.length >> 1] * 10) / 10 : null, underIds: under };
  });
  const bestList = props.map((p) => { const b = best.get(p.id) ?? null; return { id: p.id, px: b === null ? null : Math.round(b * 10) / 10, ...(p.tag ? { tag: p.tag } : {}) }; });
  return { cams: out, best: bestList, subPixel: bestList.filter((b) => b.px === null || b.px < minPx).map((b) => b.id) };
}
