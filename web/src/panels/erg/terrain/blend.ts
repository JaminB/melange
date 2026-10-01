// Second-material paint (M6.2 T2): solid voxels inside the brush get flags 3, the chosen second material and corner mask
// 0xff; solid voxels next to them get only the corners they share with a painted voxel, so the transition sits at the
// painted region's border. Removing restores the loaded level's bits 8-23 inside the brush. The corner mask is also
// collision: each set bit fills a corner of the frame's collision grid.
import { maskOf, secondOf, withSecond } from "./voxel";

/** The mask bit of each voxel corner, CORNER_BIT[cy][cz][cx] (c = 0 at the low side, 1 at the high side). Bits 0-3 are
 * the top face's corners and 4-7 the bottom's (M6.2 T2, run pF); the order within a face is not confirmed in game yet.
 * This table is the only place that order lives: flip it here if the in-game check disagrees. */
export const CORNER_BIT: readonly (readonly (readonly number[])[])[] = [
  [[7, 6], [5, 4]],   // bottom: (x0 z0) (x1 z0), (x0 z1) (x1 z1)
  [[3, 2], [1, 0]],   // top
];

export const FULL_MASK = 0xff;

/** The corners of voxel (x, y, z) that it shares with any voxel `painted` accepts, as mask bits. */
export function sharedCorners(x: number, y: number, z: number, painted: (x: number, y: number, z: number) => boolean): number {
  let mask = 0;
  for (let cy = 0; cy < 2; cy++)
    for (let cz = 0; cz < 2; cz++)
      for (let cx = 0; cx < 2; cx++) {
        // The eight voxels around the corner's vertex, other than this one.
        let touch = false;
        for (let dy = cy - 1; dy <= cy && !touch; dy++)
          for (let dz = cz - 1; dz <= cz && !touch; dz++)
            for (let dx = cx - 1; dx <= cx && !touch; dx++)
              if ((dx || dy || dz) && painted(x + dx, y + dy, z + dz)) touch = true;
        if (touch) mask |= 1 << CORNER_BIT[cy][cz][cx];
      }
  return mask;
}

/** A border voxel's word: the shared corners added to its mask when it already shows this second material, else the
 * second material with only those corners. */
export function bordered(v: number, second: number, corners: number): number {
  return withSecond(v, second, secondOf(v) === second ? maskOf(v) | corners : corners);
}
