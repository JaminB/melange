// The engine's landscape box and a height line over it (Sandtide's sand line): pure, for describe_level's landBounds and
// check_height_line. The box is the union of the boxes of the voxel frames that hold ground (map y), the end each frame
// sets is named. A frame with no solid voxel is left out: the game builds its box from the land there is, so a base frame
// emptied by remove_base_frames (DinerMight's crane, 278 map units up) does not move the line (Dustcrown: the game logged
// land Y -400 to 1110 while every frame's box gave -1678 to 5559).
import { isSolid, type Grid } from "./voxel";

export interface LandBounds {
  minY: number; maxY: number;              // map units
  minFrame: number; maxFrame: number;      // the frame whose box sets each end
  minFrameName: string; maxFrameName: string;
  emptySkipped: number;                    // voxel frames with no solid voxel, left out of the box
}

/** The land box's vertical extent from the voxel frames that hold a solid voxel; null when there are none. Sandtide reads
 * the same box from wum.sim.land() (Land.MinBounds / MaxBounds). A grid given without its words counts as holding ground. */
export function landBounds(gs: (Pick<Grid, "id" | "frame" | "bounds"> & { words?: Uint32Array })[]): LandBounds | null {
  type G = Pick<Grid, "id" | "frame" | "bounds">;
  let lo: G | null = null, hi: G | null = null, emptySkipped = 0;
  for (const g of gs) {
    if (g.words && !g.words.some(isSolid)) { emptySkipped++; continue; }
    if (!lo || g.bounds.min[1] < lo.bounds.min[1]) lo = g;
    if (!hi || g.bounds.max[1] > hi.bounds.max[1]) hi = g;
  }
  if (!lo || !hi) return null;
  return { minY: lo.bounds.min[1], maxY: hi.bounds.max[1], minFrame: lo.id, maxFrame: hi.id, minFrameName: lo.frame.name, maxFrameName: hi.frame.name, emptySkipped };
}

export interface HeightLine {
  line: number;       // map y
  base: number;       // map y the fraction counts from
  baseKind: "water" | "minY" | "fixed";
  fraction: number | null;
}

/** The line: base + fraction * (maxY - base), base the water at start when the land reaches below it (minY < water < maxY,
 * Sandtide's rule), else minY; or the fixed y. */
export function heightLine(b: LandBounds, water: number | null, o: { fraction?: number; y?: number }): HeightLine {
  if (o.y !== undefined) return { line: o.y, base: o.y, baseKind: "fixed", fraction: null };
  const f = o.fraction ?? 0.25;
  const wet = water !== null && water > b.minY && water < b.maxY;
  const base = wet ? water! : b.minY;
  return { line: base + f * (b.maxY - base), base, baseKind: wet ? "water" : "minY", fraction: f };
}

export type Side = "below" | "near" | "above";
export const sideOf = (y: number, line: number, band: number): Side => (Math.abs(y - line) <= band ? "near" : y < line ? "below" : "above");

/** Warnings about the land, the water and the line (all map units). */
export function lineWarnings(b: LandBounds, water: number | null, h: HeightLine, surfaceMin: number | null, surfaceMax: number | null): string[] {
  const w: string[] = [];
  if (water !== null && water <= b.minY) w.push(`the water (${water}) is at or under the land box's bottom (${b.minY}): all land is above it, so the base is minY and the sea does not bound the line`);
  if (water !== null && water >= b.maxY) w.push(`the water (${water}) is at or over the land box's top (${b.maxY}): all land is under it`);
  if (h.line < b.minY) w.push(`the line (${h.line}) is under the land box's bottom (${b.minY}): nothing is below it`);
  if (h.line > b.maxY) w.push(`the line (${h.line}) is over the land box's top (${b.maxY}): everything is below it`);
  if (surfaceMin !== null && surfaceMin > h.line) w.push("every standable surface sampled is above the line: no worm would be cursed");
  if (surfaceMax !== null && surfaceMax < h.line) w.push("every standable surface sampled is below the line: every worm would be cursed");
  return w;
}
