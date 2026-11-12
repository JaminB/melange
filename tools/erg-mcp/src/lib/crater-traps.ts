// Crater traps (check_crater_traps): a blast in a voxel skin leaves a hole whose walls are whole voxels high, and where
// a voxel is as tall as a worm's jump the hole is a trap: the worm that stood there, or the next one to walk in, cannot
// get out on foot. Dustcrown's sand skin has 2.5-unit voxels and a worm's jump rises 2.48 (game_riser_test), so a
// one-voxel crater on level sand held a worm. Here trial craters are cut into copies of the level's grids (nothing is
// edited), the walk graph is rebuilt round each one with a jump as the step a worm takes, and a crater is a trap when no
// walk from its floor gets out of it. Pure: grids in, verdicts out. Map units.
//
// What the game does (a Test of Dustcrown, round 6, the land read back with its own sweep on a 0.5 lattice): a blast
// takes exactly the voxels whose centres lie in its sphere, whole, sheared with their columns: a radius of 1.75 (a
// bazooka's 35 world units) at the middle of a 2.5-unit column took that one column, 2.5 deep, and at a column's corner
// took nothing; a radius of 3.5 took 2 x 3 columns, one layer. The walls are a voxel high and vertical: nothing slopes.
// In the one-column hole a worm did not get out in seven tries (a forward jump rose 2.55, to the rim's height and no
// further across; a backflip and a standing jump rose 3.5 and came straight down). In the 5 x 7.5 hole it got out twice
// in six: a forward jump from 3 units back, and a backflip. So a hole a jump does not clear is a TRAP when its floor is
// too short for a run-up (`room`), and TIGHT when it is long enough and the wall is under a backflip's rise.
import { cornerOffset, isSolid, type Vec3 } from "../core";
import { nodeAt, search, surfaces, type ReachOptions, type Region, type Surfaces } from "./reach";
import type { Grid } from "./voxel";

/** A worm's jump as game_riser_test measured it (map units): the highest step a jump clears. */
export const JUMP_RISE = 2.48;
/** What a backflip and a standing jump rise (map units; a Test of Dustcrown, round 6: 3.5 from a crater's floor). */
export const BACKFLIP_RISE = 3.5;
/** The floor a worm needs for a run-up or a backflip's arc (map units): out of a 5 x 7.5 hole from 3 back, never out of a 2.5 x 2.5 one. */
export const RUN_UP = 3;

export interface CraterShape {
  radius: number;          // the blast's radius
  depth?: number;          // how far under the ground its centre lies (default 0: on the surface; negative: over it)
  /** How a cell is carved: "centre" (default: a cell goes when its centre is inside the sphere, the rule the offline
   * craters of check_exposed_core use) or "touch" (a cell goes when the sphere reaches into it at all: the widest hole). */
  rule?: "centre" | "touch";
}

/** Copies of the grids a sphere at `p` reaches, with the cells inside it emptied; the others as they are (the same
 * objects). Frames whose name holds PERM are never carved, as in the game. `carved` counts the cells that went. */
export function carveCrater(gs: Grid[], p: Vec3, c: CraterShape): { grids: Grid[]; carved: number; layers: number; voxel: number } {
  const r = c.radius, cy = p[1] - (c.depth ?? 0);
  let carved = 0, layers = 0, voxel = 0;
  const out = gs.map((g) => {
    const b = g.bounds, lift = g.frame.corners ? [Math.min(...g.frame.corners), Math.max(...g.frame.corners)] : [0, 0];
    const sy = Math.hypot(g.toMap[1], g.toMap[5], g.toMap[9]);
    if (/PERM/i.test(g.frame.name) || b.max[0] < p[0] - r || b.min[0] > p[0] + r || b.max[2] < p[2] - r || b.min[2] > p[2] + r ||
      b.max[1] + lift[1] * sy < cy - r || b.min[1] + lift[0] * sy > cy + r) return g;
    const [X, Y, Z] = g.size, m = g.toMap, f = g.fromMap;
    const lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
    for (let k = 0; k < 8; k++) {
      const q = [p[0] + (k & 1 ? r : -r), cy + (k & 2 ? r : -r), p[2] + (k & 4 ? r : -r)];
      for (let a = 0; a < 3; a++) { const v = f[a * 4] * q[0] + f[a * 4 + 1] * q[1] + f[a * 4 + 2] * q[2] + f[a * 4 + 3]; lo[a] = Math.min(lo[a], v); hi[a] = Math.max(hi[a], v); }
    }
    lo[1] -= lift[1]; hi[1] -= lift[0];
    const x0 = Math.max(0, Math.floor(lo[0]) - 1), x1 = Math.min(X - 1, Math.ceil(hi[0]) + 1), y0 = Math.max(0, Math.floor(lo[1]) - 1), y1 = Math.min(Y - 1, Math.ceil(hi[1]) + 1);
    const z0 = Math.max(0, Math.floor(lo[2]) - 1), z1 = Math.min(Z - 1, Math.ceil(hi[2]) + 1);
    if (x0 > x1 || y0 > y1 || z0 > z1) return g;
    // half a cell's diagonal: "touch" carves a cell whose centre is within the radius plus this
    const reach = c.rule === "touch" ? r + 0.5 * Math.hypot(m[0], m[5], m[10]) * 0.8 : r;
    let words: Uint32Array | null = null;
    const ys = new Set<number>();
    for (let z = z0; z <= z1; z++) for (let x = x0; x <= x1; x++) {
      const off = g.frame.corners ? cornerOffset(g.frame, x + 0.5, z + 0.5) : 0;
      for (let y = y0; y <= y1; y++) {
        const i = (z * X + x) * Y + y;
        if (!isSolid(g.words[i])) continue;
        const gy = y + 0.5 + off;
        const wx = m[0] * (x + 0.5) + m[1] * gy + m[2] * (z + 0.5) + m[3], wy = m[4] * (x + 0.5) + m[5] * gy + m[6] * (z + 0.5) + m[7], wz = m[8] * (x + 0.5) + m[9] * gy + m[10] * (z + 0.5) + m[11];
        if ((wx - p[0]) ** 2 + (wy - cy) ** 2 + (wz - p[2]) ** 2 > reach * reach) continue;
        words ??= new Uint32Array(g.words);
        words[i] = 0;
        carved++; ys.add(y);
      }
    }
    if (!words) return g;
    if (ys.size > layers) { layers = ys.size; voxel = sy; }
    return { ...g, words };
  });
  return { grids: out, carved, layers, voxel };
}

export interface CraterVerdict {
  at: Vec3;                    // where the blast was centred on the ground
  carved: number;              // cells the crater took
  layers: number;              // the most voxel layers it took out of one frame
  voxel: number;               // that frame's voxel height
  floor: number | null;        // the crater floor's lowest ground (null: it holed the ground through)
  depth: number | null;        // the ground before, less the floor
  trap: boolean;               // no walk from the floor gets out, with a jump as the step, and no room or rise for a backflip
  /** No walk out with a jump as the step, but the floor is `room` long and the wall under a backflip's rise: a worm
   * gets out with a run-up or a backflip (seen in the game), not at every try. */
  tight: boolean;
  floorSpan: number;           // the longest stretch of the hole's floor, in x or z
  /** Closed with no ground beside it to climb to: the blast left the spot cut off by the sea (a shore), not walled in. */
  cutOff: boolean;
  holed: boolean;              // nothing to stand on under the blast: a hole through the skin
  /** The least climb at the crater's edge: from ground the floor's walk reaches up to ground it does not (the wall a
   * worm must clear). Null when the walk gets out, or nothing stands beside it. */
  wall: number | null;
  wallAt: Vec3 | null;
  reachedColumns: number;      // how much ground the floor's walk covers
}

export interface TrapRules { jump: number; maxSlope: number; step: number; headroom: number; water: number | null; margin: number; backflip?: number; room?: number; }

/** One trial crater at `p` (a point on the ground): cut, and walked out of. The walk graph is built over the crater and
 * `margin` round it; the crater is a trap when the walk from its floor, taking steps up to `jump`, reaches no ground
 * further than the crater's radius plus half the margin from its centre. A spot that was no better before the blast (a
 * pit already, an islet) is not the crater's doing: `wasOpen` says whether the same walk got out before. */
export function craterVerdict(gs: Grid[], p: Vec3, c: CraterShape, rules: TrapRules): CraterVerdict & { wasOpen: boolean } {
  const R = c.radius + rules.margin, r: Region = { x0: p[0] - R, z0: p[2] - R, x1: p[0] + R, z1: p[2] + R };
  const near = gs.filter((g) => !(g.bounds.max[0] <= r.x0 || g.bounds.min[0] >= r.x1 || g.bounds.max[2] <= r.z0 || g.bounds.min[2] >= r.z1));
  const o: ReachOptions = { step: rules.step, maxStep: rules.jump, maxSlope: rules.maxSlope, maxDrop: Infinity, maxGap: 0, headroom: rules.headroom, water: rules.water,
    // a rise over the jump is judged over the move itself, whatever the frame: a voxel-high wall is a barrier, never a stair
    slopeOver: "segment" };
  const out = c.radius + rules.margin / 2;
  // the walk from the ground at the centre: does it get `out` away, and what wall stops it
  const walk = (S: Surfaces) => {
    const i = Math.floor((p[0] - S.r.x0) / S.step), j = Math.floor((p[2] - S.r.z0) / S.step), col = j * S.w + i;
    // the highest ground of the centre's column at or under the blast point (the floor), dry
    let n0 = -1;
    for (let n = S.start[col]; n < S.start[col + 1]; n++) if (S.y[n] <= p[1] + 0.75 && (o.water === null || S.y[n] > o.water)) { n0 = n; break; }
    if (n0 < 0) return null;
    const q = search(S, n0, o);
    let escaped = false, reached = 0, wall: number | null = null, wallAt: Vec3 | null = null, floor = Infinity;
    let fx0 = Infinity, fx1 = -Infinity, fz0 = Infinity, fz1 = -Infinity;
    for (let n = 0; n < S.nodes; n++) {
      if (!q.reached[n]) continue;
      reached++;
      const at = nodeAt(S, n), d = Math.hypot(at[0] - p[0], at[2] - p[2]);
      fx0 = Math.min(fx0, at[0]); fx1 = Math.max(fx1, at[0]); fz0 = Math.min(fz0, at[2]); fz1 = Math.max(fz1, at[2]);
      if (d <= c.radius) floor = Math.min(floor, S.y[n]);
      if (d >= out) escaped = true;
      const ci = S.cell[n] % S.w, cj = Math.floor(S.cell[n] / S.w);
      for (const [dx, dz] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
        const ni = ci + dx, nj = cj + dz;
        if (ni < 0 || nj < 0 || ni >= S.w || nj >= S.h) continue;
        const nc = nj * S.w + ni;
        for (let b = S.start[nc]; b < S.start[nc + 1]; b++) {
          if (q.reached[b] || (o.water !== null && S.y[b] <= o.water)) continue;
          const rise = S.y[b] - S.y[n];
          if (rise > 0 && (wall === null || rise < wall)) { wall = rise; wallAt = nodeAt(S, b); }
        }
      }
    }
    return { escaped, reached, wall, wallAt, floor: Number.isFinite(floor) ? floor : S.y[n0], span: reached ? Math.max(fx1 - fx0, fz1 - fz0) + S.step : 0 };
  };
  const before = walk(surfaces(near, r, rules.step, rules.headroom));
  const cut = carveCrater(near, p, c);
  const after = cut.carved ? walk(surfaces(cut.grids, r, rules.step, rules.headroom)) : before;
  const holed = cut.carved > 0 && after === null;
  const closed = !!after && !after.escaped && !!before?.escaped;
  // closed to a jump: a backflip or a run-up still gets out of a hole that is long enough and not too deep
  const cutOff = closed && after!.wall === null;
  const tight = closed && !cutOff && after!.span >= (rules.room ?? RUN_UP) - 1e-6 && after!.wall! <= (rules.backflip ?? BACKFLIP_RISE) + 1e-6;
  return {
    at: p, carved: cut.carved, layers: cut.layers, voxel: cut.voxel,
    floor: after ? after.floor : null, depth: after ? p[1] - after.floor : null,
    trap: closed && !tight && !cutOff, tight, cutOff, floorSpan: closed ? after!.span : 0, holed,
    wall: after && !after.escaped ? after.wall : null, wallAt: after && !after.escaped ? after.wallAt : null,
    reachedColumns: after?.reached ?? 0, wasOpen: !!before?.escaped,
  };
}

/** What would keep a crater's walls under a jump: the voxel height a skin needs, from the walls the traps showed. A
 * crater that takes `layers` voxel layers at its rim leaves a wall of layers x voxel; under `jump` less `clear` (the
 * room a worm needs over the lip, default 0.25) it is a step a jump takes. */
export function trapAdvice(traps: CraterVerdict[], jump: number, clear = 0.25): { voxelSize: number; layers: number; why: string } | null {
  if (!traps.length) return null;
  const voxel = Math.max(...traps.map((t) => t.voxel)), layers = Math.max(1, ...traps.map((t) => (t.voxel > 0 && t.wall ? Math.round(t.wall / t.voxel) : 1)));
  // down to a quarter unit, the sizes a skin is built in
  const size = Math.floor(((jump - clear) / layers) * 4) / 4;
  return { voxelSize: Math.max(0.5, size), layers,
    why: `the walls are ${layers} voxel layer${layers === 1 ? "" : "s"} of ${Math.round(voxel * 100) / 100} (a wall of ${Math.round(layers * voxel * 100) / 100}), and a jump rises ${jump}: a skin of voxels no taller than ${Math.max(0.5, size)} leaves walls a jump clears with ${clear} to spare` };
}

/** The middle (x, z) of the voxel column of `g` under a map point, for an upright grid; null when the point is off it. */
export function columnCentre(g: Grid, x: number, z: number): [number, number] | null {
  const b = g.bounds, m = g.toMap;
  if (x < b.min[0] || x >= b.max[0] || z < b.min[2] || z >= b.max[2] || !(m[0] > 0) || !(m[10] > 0)) return null;
  const cx = Math.min(g.size[0] - 1, Math.max(0, Math.floor((x - m[3]) / m[0]))), cz = Math.min(g.size[2] - 1, Math.max(0, Math.floor((z - m[11]) / m[10])));
  return [m[3] + (cx + 0.5) * m[0], m[11] + (cz + 0.5) * m[10]];
}
