// What a level's own looks (set_water_look: the sea material, the water plane's opacity, the level haze) do to a
// picture, for the offline renders and for check_sea_opacity. Dustcrown's sand sea and its dust were judged only in the
// game until round 5: render_view drew a blue translucent plane and no haze, so a look that hid the island from the
// overview's distance, or a sea that let its bed show, was found in a Test's screenshot. Pure: numbers in, numbers out.
//
// The haze is modelled on Sunstone 2.4's (postfx/hdr/lin.frag), the mod that draws a level haze: over land
//   haze = landHaze * (1 - exp(-max(dist - start, 0) / distance)) * amount
// and over the sea up to maxHaze, with the mod's own range where the level gives 0. The game without such a mod draws
// no haze at all, and the model says so (`drawnBy`).
import type { Rgb } from "./inspect-png";
import type { Grid } from "./voxel";
import { spansAt } from "./inspect-field";
import { readHaze, readSea, readWaterLook, type HazeLook, type SeaLook, type WaterLook } from "./water-look";

/** Sunstone 2.4's haze settings as shipped (effect.ini): world units. */
export const HAZE_MODEL = { fogStart: 400, fogDistance: 3000, landHaze: 0.6, maxHaze: 0.7, source: "Sunstone 2.4 (postfx/hdr/lin.frag, effect.ini defaults)" };

/** A level's looks as its script's set_water_look block says them. */
export interface LevelLooks { look: WaterLook | null; sea: SeaLook | null; haze: HazeLook | null; }
export function levelLooks(script: string): LevelLooks {
  return { look: readWaterLook(script), sea: readSea(script), haze: readHaze(script) };
}

/** How much of a point `dist` world units away the haze replaces: over land and over the sea (0-1). */
export function hazeAt(dist: number, h: HazeLook): { land: number; sea: number } {
  const start = h.start > 0 ? h.start : HAZE_MODEL.fogStart, range = h.distance > 0 ? h.distance : HAZE_MODEL.fogDistance;
  const od = Math.max(dist - start, 0) / range, k = (1 - Math.exp(-od)) * Math.max(0, Math.min(1, h.amount));
  return { land: Math.min(1, HAZE_MODEL.landHaze * k), sea: Math.min(1, HAZE_MODEL.maxHaze * k) };
}

/** The distance (world units) at which the land haze reaches `share` (null: it never does, the haze tops out below). */
export function hazeReach(share: number, h: HazeLook): number | null {
  const top = HAZE_MODEL.landHaze * Math.max(0, Math.min(1, h.amount));
  if (share >= top - 1e-9) return null;
  const start = h.start > 0 ? h.start : HAZE_MODEL.fogStart, range = h.distance > 0 ? h.distance : HAZE_MODEL.fogDistance;
  return start - range * Math.log(1 - share / top);
}

const mix = (a: Rgb, b: number[], k: number): Rgb => [0, 1, 2].map((c) => Math.round(a[c] + (b[c] - a[c]) * k)) as Rgb;
const far = (a: number[], b: number[]) => Math.max(Math.abs(a[0] - b[0]), Math.abs(a[1] - b[1]), Math.abs(a[2] - b[2]));

/** The haze verdict for a camera `dist` world units from what it looks at: the share of the land's colour the haze
 * takes there, a land colour as it then reads, and a warning when the island is washed out (`hidden`, default a third
 * of its colour gone: round 5's first overview read as a pale wash at 0.4). */
export function hazeVerdict(dist: number, h: HazeLook, o: { land?: Rgb; hidden?: number; what?: string } = {}) {
  const k = hazeAt(dist, h), limit = o.hidden ?? 0.33, clear = hazeReach(limit, h);
  const r1 = (v: number) => Math.round(v * 100) / 100;
  return {
    distance: Math.round(dist), land: r1(k.land), sea: r1(k.sea),
    ...(o.land ? { landReads: mix(o.land, h.color, k.land) } : {}),
    hidden: k.land >= limit,
    ...(k.land >= limit ? { warning: `the haze takes ${Math.round(k.land * 100)} % of the land's colour from ${o.what ?? "this camera"} (${Math.round(dist)} world units away): the island reads as a wash of the haze colour. ` +
      (clear === null ? "" : `It stays under ${Math.round(limit * 100)} % within ${Math.round(clear)} world units; `) +
      `bring the camera closer (set_level_camera, set_overview {fit: "island"}), or thin the haze (a lower amount, a later start, a longer distance)` } : {}),
  };
}

/** Which of the two draws the sea: a water shader mod that honours the sea material, or the game's own water shader. */
export type SeaDrawnBy = "mod" | "game";

/** The sea's opacity as it is drawn: the sea material's where a shader mod draws it, else the water plane's NearOpacity
 * (null when the look sets none: the theme's own, about 0.6 to 0.9). */
export function seaOpacity(l: LevelLooks, by: SeaDrawnBy): number | null {
  if (by === "mod" && l.sea) return l.sea.opacity;
  const o = l.look?.NearOpacity;
  return typeof o === "number" ? o : null;
}

export interface Shallow { x: number; z: number; depth: number; material: number; }

/** The ground under the water within `depth` of its surface (map units), on a lattice of `step`: the shallows a
 * translucent sea shows. Each is the column's highest ground at or under the water. */
export function shallows(gs: Grid[], water: number, depth: number, step: number, region?: { x0: number; z0: number; x1: number; z1: number }): { cells: Shallow[]; wet: number; dry: number } {
  let x0 = Infinity, z0 = Infinity, x1 = -Infinity, z1 = -Infinity;
  for (const g of gs) { x0 = Math.min(x0, g.bounds.min[0]); z0 = Math.min(z0, g.bounds.min[2]); x1 = Math.max(x1, g.bounds.max[0]); z1 = Math.max(z1, g.bounds.max[2]); }
  if (region) { x0 = Math.max(x0, region.x0); z0 = Math.max(z0, region.z0); x1 = Math.min(x1, region.x1); z1 = Math.min(z1, region.z1); }
  const cells: Shallow[] = [];
  let wet = 0, dry = 0;
  for (let z = z0 + step / 2; z < z1; z += step) for (let x = x0 + step / 2; x < x1; x += step) {
    const top = spansAt(gs, x, z)[0];
    if (!top) continue;
    if (top.top > water) { dry++; continue; }
    wet++;
    if (water - top.top <= depth + 1e-9) cells.push({ x, z, depth: water - top.top, material: top.material });
  }
  return { cells, wet, dry };
}

/** check_sea_opacity's verdict: does the sea let the land under it show? `tint` gives a material's colour as drawn
 * (null: no colours, the opacity alone is judged). A shallow shows when the sea is not opaque there: what reaches the
 * eye is the bed's colour times (1 - opacity), and it is flagged when that moves the sea's colour by `threshold` or
 * more (0-255, default 10). With no sea colour known (the game's own blue water) every shallow under a sea that is not
 * opaque is flagged. */
export function seaShowsThrough(cells: Shallow[], opacity: number | null, sea: number[] | null, tint: ((m: number) => Rgb) | null, threshold = 10):
  { opaque: boolean; opacity: number | null; visible: Shallow[]; worst: { at: [number, number]; depth: number; bed: Rgb | null; shift: number } | null } {
  const op = opacity === null ? null : Math.max(0, Math.min(1, opacity));
  if (op !== null && op >= 0.995) return { opaque: true, opacity: op, visible: [], worst: null };
  const through = 1 - (op ?? 0.75);
  const visible: Shallow[] = [];
  let worst: { at: [number, number]; depth: number; bed: Rgb | null; shift: number } | null = null;
  for (const c of cells) {
    const bed = tint ? tint(c.material) : null;
    const shift = bed && sea ? far(bed, sea) * through : 255 * through;
    if (shift < threshold) continue;
    visible.push(c);
    if (!worst || shift > worst.shift) worst = { at: [c.x, c.z], depth: c.depth, bed, shift: Math.round(shift) };
  }
  return { opaque: false, opacity: op, visible, worst };
}
