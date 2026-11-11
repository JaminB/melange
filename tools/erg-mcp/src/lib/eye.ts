// A camera's eye in open air with a clear view: the game's camera override draws from any point, inside the rock too,
// so a pose chosen without the level shows the inside of a wall or hides its subject. In the Caldera v2 round-2 run the
// climb shots' side poses clipped into the cone's rock and the castles' walls (c1_t3_w0_start, c1_t1_w4_mid) and the
// builder's blast cameras stood inside the volcano. Read from the open project's voxels as every inspect tool reads them
// (spansAt: a draped skin at its sheared height). Pure: grids in; map units.
import { spansAt } from "./inspect-field";
import type { Grid } from "./voxel";

/** Whether the map point q is inside a solid voxel. */
export const inSolid = (gs: Grid[], q: number[]): boolean => spansAt(gs, q[0], q[2]).some((sp) => q[1] < sp.top && q[1] > sp.bottom);

/** Whether solid voxels lie on the line of sight from eye to p, sampled every half unit; the last `skip` map units at p
 * are not looked at (the ground a worm stands on, the wall a blast is in). */
export function blocked(gs: Grid[], eye: number[], p: number[], skip = 2): boolean {
  const d = Math.hypot(eye[0] - p[0], eye[1] - p[1], eye[2] - p[2]), n = Math.ceil(d / 0.5);
  for (let i = 0; i < n; i++) {
    const t = i / n;
    if (d * (1 - t) < skip) break;
    if (inSolid(gs, [eye[0] + (p[0] - eye[0]) * t, eye[1] + (p[1] - eye[1]) * t, eye[2] + (p[2] - eye[2]) * t])) return true;
  }
  return false;
}

/** The eye is in open air with `room` of air round it (the camera's near plane clips what is closer: a wall half a
 * unit away fills the picture). */
export function openAir(gs: Grid[], eye: number[], room = 0.75): boolean {
  if (inSolid(gs, eye)) return false;
  for (const [dx, dy, dz] of [[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]])
    if (inSolid(gs, [eye[0] + dx * room, eye[1] + dy * room, eye[2] + dz * room])) return false;
  return true;
}

export interface ClearEye { eye: number[]; clear: boolean; moved: string | null; tried: number; }

/** An eye looking at p from near `want` (map units) in open air with a clear line of sight: `want` itself when it is,
 * else the nearest change that is: raised (up to 75 degrees over p), then nearer or farther, then turned round p
 * (30 degrees at a time, both ways, to the far side), each raised again. `clear: false` (and `want` back) when none is. */
export function clearEye(gs: Grid[], want: number[], p: number[], o: { skip?: number; room?: number; minDistance?: number } = {}): ClearEye {
  const skip = o.skip ?? 2, room = o.room ?? 0.75;
  const ok = (e: number[]) => openAir(gs, e, room) && !blocked(gs, e, p, skip);
  if (ok(want)) return { eye: want, clear: true, moved: null, tried: 1 };
  const dx = want[0] - p[0], dy = want[1] - p[1], dz = want[2] - p[2];
  const d0 = Math.max(o.minDistance ?? 3, Math.hypot(dx, dy, dz)), az0 = Math.atan2(dx, dz), el0 = Math.atan2(dy, Math.hypot(dx, dz));
  const deg = Math.PI / 180;
  const elevs = [el0, ...[12, 24, 36, 50].map((e) => el0 + e * deg)].map((e) => Math.min(75 * deg, Math.max(-20 * deg, e))).filter((e, i, all) => all.indexOf(e) === i);
  const turns = [0, 30, -30, 60, -60, 90, -90, 135, -135, 180];
  let tried = 1;
  for (const tu of turns)
    for (const f of [1, 0.75, 1.3, 0.55])
      for (const el of elevs) {
        const az = az0 + tu * deg, d = d0 * f;
        const e = [p[0] + d * Math.cos(el) * Math.sin(az), p[1] + d * Math.sin(el), p[2] + d * Math.cos(el) * Math.cos(az)];
        tried++;
        if (ok(e)) {
          const why = [tu ? `turned ${tu} degrees round the point` : "", Math.abs(el - el0) > 1e-6 ? `raised ${Math.round((el - el0) / deg)} degrees` : "", f !== 1 ? `${f < 1 ? "nearer" : "farther"} (x ${f})` : ""].filter(Boolean);
          return { eye: e, clear: true, moved: why.join(", ") || "moved", tried };
        }
      }
  return { eye: want, clear: false, moved: null, tried };
}
