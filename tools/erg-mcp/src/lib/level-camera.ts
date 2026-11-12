// The level camera (set_level_camera): one pose a level's builder chose to show the whole level from, kept per project
// for this erg-mcp session like the level look and the overview framing. The harness's overview (game_camera and
// game_shots {preset: "overview"}), render_view {view: "level"}, set_level_thumbnail's default picture and render_view
// {gameMatch}'s haze check use it. The default overview stands 0.95 of the level's span back, which on a level with a
// haze is far enough for the haze to wash the island out (Dustcrown round 5); a level camera frames the island itself.
// World units (map x 20), as the game's cameras are.
import type { Vec3 } from "../core";
import { spansAt } from "./inspect-field";
import { fitDistance, orbitCamera } from "./render-3d";
import type { Grid } from "./voxel";

export interface LevelCamera { pos: Vec3; target: Vec3; fov: number; how: string; }
const cameras = new Map<string, LevelCamera>();
export const levelCameraOf = (projectId: string): LevelCamera | null => cameras.get(projectId) ?? null;
export const setLevelCamera = (projectId: string, c: LevelCamera | null) => { if (c) cameras.set(projectId, c); else cameras.delete(projectId); };

/** The points a level camera frames: the land over the water on a lattice (its top, and the waterline under it), so the
 * sea bed, a shell under the skin and a wiped base's empty frames do not pull the camera back. `fit` solid takes every
 * column's top instead. Map units; empty when nothing qualifies. */
export function landPoints(gs: Grid[], water: number | null, fit: "island" | "solid", step = 2): Vec3[] {
  let x0 = Infinity, z0 = Infinity, x1 = -Infinity, z1 = -Infinity;
  for (const g of gs) { x0 = Math.min(x0, g.bounds.min[0]); z0 = Math.min(z0, g.bounds.min[2]); x1 = Math.max(x1, g.bounds.max[0]); z1 = Math.max(z1, g.bounds.max[2]); }
  const out: Vec3[] = [], floor = fit === "island" ? water ?? -Infinity : -Infinity;
  for (let z = z0 + step / 2; z < z1; z += step) for (let x = x0 + step / 2; x < x1; x += step) {
    const top = spansAt(gs, x, z)[0];
    if (!top || top.top <= floor) continue;
    out.push([x, top.top, z]);
    if (Number.isFinite(floor)) out.push([x, floor, z]);
  }
  return out;
}

/** A pose from a side: `azimuth` (the bearing from the target to the eye, clockwise from north) and `elevation` over
 * `target` (default the middle of the points' box), as far back as shows every point in a view of `fov` and `aspect`,
 * times `margin`; or at `distance`. Map units. */
export function fittedPose(points: Vec3[], o: { azimuth: number; elevation: number; target?: Vec3; distance?: number; fov?: number; aspect?: number; margin?: number }): { pos: Vec3; target: Vec3; fov: number; distance: number } {
  const lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
  for (const p of points) for (let k = 0; k < 3; k++) { lo[k] = Math.min(lo[k], p[k]); hi[k] = Math.max(hi[k], p[k]); }
  const target = o.target ?? ([(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2] as Vec3), fov = o.fov ?? 60;
  const distance = o.distance ?? fitDistance(points, target, o.azimuth, o.elevation, fov, o.aspect ?? 16 / 9, o.margin ?? 1.08);
  const c = orbitCamera(target, o.azimuth, o.elevation, distance, fov);
  return { pos: c.pos, target: c.target, fov, distance };
}
