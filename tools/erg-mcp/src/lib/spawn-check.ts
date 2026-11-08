// Camera and water sanity of the spawn knots, for list_objects and place_spawn_knots: a few column reads around each
// spawn (about 80 within the default radius), so it is cheap enough to run on every answer. Map units throughout.
import type { EditorStore, Vec3 } from "../core";
import { markers, spansAt, waterMap } from "./inspect-field";
import { grids, type Grid } from "./voxel";

/** How far above the ground a worm stands on its turn camera sits: an assumption (the turn camera sits behind and above
 * the worm, about 120 world units up), not read from the game. */
export const CAMERA_RISE = 6;
/** How far from the spawn, horizontally, ground at the camera's height is looked for. */
export const WALL_RADIUS = 2.5;

export interface SpawnCheck {
  name: string; pos: Vec3;
  standOn: number | null;                                  // the top of the ground it stands on (null: none under it)
  wall: { distance: number; top: number; x: number; z: number } | null;   // the nearest column solid at the camera's height
  underWater: boolean;                                     // standOn is at or under the water
}

/** The nearest column within `radius` of (x, z) that is solid at height y (sampled every half map unit), or null. */
export function solidNear(gs: Grid[], x: number, z: number, y: number, radius: number): SpawnCheck["wall"] {
  let best: SpawnCheck["wall"] = null;
  const n = Math.ceil(radius / 0.5);
  for (let j = -n; j <= n; j++)
    for (let i = -n; i <= n; i++) {
      const d = Math.hypot(i, j) * 0.5;
      if (d > radius + 1e-9 || (best && d >= best.distance)) continue;
      for (const s of spansAt(gs, x + i * 0.5, z + j * 0.5))
        if (s.bottom <= y && s.top > y) { best = { distance: d, top: s.top, x: x + i * 0.5, z: z + j * 0.5 }; break; }
    }
  return best;
}

export function checkSpawns(store: EditorStore, radius = WALL_RADIUS, rise = CAMERA_RISE): SpawnCheck[] {
  const gs = grids(store), water = waterMap(store);
  return markers(store).filter((m) => m.kind === "spawn").map((m) => {
    // what it stands on: the highest ground not above 1.5 over the knot (check_reachability's rule)
    const g = spansAt(gs, m.pos[0], m.pos[2], m.pos[1] + 1.5)[0];
    const standOn = g ? g.top : null;
    return { name: m.name, pos: m.pos, standOn, wall: standOn === null ? null : solidNear(gs, m.pos[0], m.pos[2], standOn + rise, radius),
      underWater: standOn !== null && water !== null && standOn <= water };
  });
}

/** The checks as warning lines; `len` writes a map length in the caller's units (with the unit). */
export function spawnWarnings(store: EditorStore, len: (map: number) => string, radius = WALL_RADIUS, rise = CAMERA_RISE): string[] {
  const out: string[] = [];
  const water = waterMap(store);
  for (const c of checkSpawns(store, radius, rise)) {
    if (c.wall) out.push(`warning: ${c.name}'s turn camera may start inside a wall: ground ${len(c.wall.distance)} away rises to ${len(c.wall.top - c.standOn!)} above the ground it stands on (the camera sits about ${len(rise)} above the worm; checked within ${len(radius)})`);
    if (c.underWater) out.push(`warning: ${c.name} stands on ground under the water (ground y ${len(c.standOn!)}, water y ${len(water!)})`);
  }
  return out;
}
