// The mine factory's placement against what the game does with it, for place_object, list_objects and save. From the
// game's code (MineFactoryLogicEntity.cpp) and Tests on Caldera (2026-11-08): when the match places it, the factory drops
// from its knot onto the land below along a sweep of at most 1000 world units (50 map). When that sweep hits nothing it
// keeps the point the match's last sweep left (an assert, bCollidesWithLand, is all the game does), on Caldera far off
// on the south shore, 7.7 map units under the sea; on its first update it explodes when the water is more than 10 world
// units (half a map unit) over the point it settled on, and an explosion within its radius sets it off 100 ticks later.
// Either way it posts MineFactory.Deleted and is gone for the match: no mine is ever fired.
//
// Seen in the Tests: the sweep never landed on a draped skin (corner offsets; 6 tries at 4 places), and it landed on an
// undraped block (a castle's, or a 4 x 1 x 4 pad on the skin) at 5 places 30-66 map units from the level's centre, but
// not on pads or a 6 x 4 x 6 block inside the crater (within about 20 of the centre, 5 tries), for reasons not found.
// Hence: an error for a draped skin or no land in reach, a warning for water over the landing, and always a Test with
// game_mines {alive: true} to confirm. A factory with keepKnot (Melange 0.13+, levels/factory.cpp) stays at its knot when
// the drop hits nothing, so for it those two are warnings and the water is checked against the knot. In the Caldera
// iteration-7 Test (2026-11-08) a keepKnot factory whose knot sat on the crater floor's top stood exactly on its knot and
// fired, so a keepKnot knot on a voxel top (within ON_SURFACE) gets no advisory: whatever the sweep does, it stays there.
// Map units throughout.
import type { EditorStore } from "../core";
import { findHoles } from "./holes";
import { checkSupport } from "./support";
import { markers, spansAt, waterMap } from "./inspect-field";
import { grids } from "./voxel";

/** The factory sweep's reach under the knot, map units (1000 world). */
export const FACTORY_DROP = 50;
/** Water this far over the point it settled on sets it off (10 world units). */
export const FACTORY_WATER = 0.5;
/** The radius round the knot checked for see-through holes (check_holes), map units. */
export const HOLE_RADIUS = 6;
/** The radius round the knot checked for a shell over a void (check_support), map units. */
export const SUPPORT_RADIUS = 1.5;
/** A keepKnot knot this close over the top of the ground under it sits on it (place_object snap "surface" puts it there). */
export const ON_SURFACE = 0.5;

export interface FactoryCheck {
  knot: [number, number, number];
  lands: { y: number; frame: number; draped: boolean } | null;   // the ground the drop reaches (null: none in reach)
  problems: string[];      // the game is expected to lose the factory at load
  warnings: string[];      // it may be lost, or later
}

/** The level's mine factory checked as the game places it, or null without one. `waterTop`: the highest the water gets
 * (map; the script's schedule cap), when known. `len` writes a map length with its units. */
export function checkFactory(store: EditorStore, len: (map: number) => string, waterTop?: number | null): FactoryCheck | null {
  const f = markers(store).find((m) => m.kind === "minefactory");
  if (!f) return null;
  const spec = store.scene.objects?.find((o) => o.type === "minefactory");
  const keep = !!(spec && spec.type === "minefactory" && spec.factory?.keepKnot);
  const gs = grids(store), [x, y, z] = f.pos;
  const knot: [number, number, number] = [x, y, z];
  const out: FactoryCheck = { knot, lands: null, problems: [], warnings: [] };
  const water = waterMap(store);
  const top = Math.max(water ?? -Infinity, waterTop ?? -Infinity);
  // the drop starts at the knot: the first solid top at or under it (a knot inside the ground lands on the top of its stretch)
  const sp = spansAt(gs, x, z).find((s) => s.bottom <= y + 1e-6);
  if (!sp || y - Math.min(sp.top, y) > FACTORY_DROP) {
    if (!keep) {
      out.problems.push(`no voxel ground within ${len(FACTORY_DROP)} under the mine factory's knot (the game's drop reaches 1000 world units): it settles at a stale point of the match and is lost at load (keepKnot keeps it at its knot)`);
      return out;
    }
    out.warnings.push(`no voxel ground within ${len(FACTORY_DROP)} under the mine factory's knot: with keepKnot it stays at its knot, in the air`);
    if (Number.isFinite(top) && top > y + FACTORY_WATER) out.problems.push(`the water (${len(top)}) gets more than ${len(FACTORY_WATER)} over the factory's knot (${len(y)}): it explodes`);
    return out;
  }
  const landY = Math.min(sp.top, y), g = gs.find((q) => q.id === sp.frame), draped = !!g?.frame.corners;
  out.lands = { y: landY, frame: sp.frame, draped };
  // keepKnot with the knot on the ground's top: the factory stands on its knot whether or not the sweep hits
  const onSurface = keep && y - landY <= ON_SURFACE + 1e-6;
  if (sp.top > y + 1e-6) out.warnings.push(`the mine factory's knot is inside solid ground (its top is ${len(sp.top - y)} higher): put it on the surface`);
  if (draped && !keep) out.problems.push("the mine factory's knot is over a draped skin (heightfield_drape's corner offsets): the game's drop never landed on one in the Tests (it settles at a stale point, here under the sea, and explodes at load): set keepKnot (the factory then stays at its knot when its drop misses; place it with snap: \"surface\" so the knot is on the skin), or put it on an undraped block (a castle, keep or add_block pad), and Test with game_mines {alive: true}");
  if (draped && keep && !onSurface) out.warnings.push("the mine factory's knot is over a draped skin, where the game's drop has missed: with keepKnot it stays at its knot, so keep the knot on the skin's surface (snap: \"surface\")");
  if (Number.isFinite(top) && top > landY + FACTORY_WATER)
    out.problems.push(`the water (${len(top)}${waterTop !== undefined && waterTop !== null && waterTop >= (water ?? -Infinity) ? ", the script's highest" : ""}) gets more than ${len(FACTORY_WATER)} over the ground the factory lands on (${len(landY)}): it explodes${water !== null && water > landY + FACTORY_WATER ? " at load" : " when the water gets there"}`);
  // see-through holes round the knot (check_holes): the floor the mines land on and the camera sees
  const holes = findHoles(gs, { x0: x - HOLE_RADIUS, z0: z - HOLE_RADIUS, x1: x + HOLE_RADIUS, z1: z + HOLE_RADIUS }, 0.5, water ?? 0,
    (px, pz) => Math.hypot(px - x, pz - z) <= HOLE_RADIUS);
  if (holes.holes) out.warnings.push(`${holes.holes} of ${holes.columns} rays within ${len(HOLE_RADIUS)} of the mine factory's knot hit no ground over the water (see-through holes${holes.clusters.length ? `, the largest round (${holes.clusters[0].centre.map((v) => Math.round(v * 10) / 10).join(", ")})` : ""}): mines fall through them; check_holes {around} lists them`);
  // a shell over a void under the knot (check_support): check_holes passes it, but a shot opens it under the factory.
  // Only with a known water level, the base the ground must reach.
  if (water !== null) {
    const sup = checkSupport(gs, x, z, SUPPORT_RADIUS, 0.5, water);
    if (sup.hollow) out.warnings.push(`${sup.hollow} of ${sup.columns} columns within ${len(SUPPORT_RADIUS)} of the mine factory's knot are a shell over a void (the thinnest ${len(sup.thinnest!.thickness)} thick, air ${len(sup.deepest!.voidBelow)} deep under it), not solid down to the water: fill under it; check_support {at} lists them`);
  }
  // launchers (world units in the spec): each erupts its share of a fire from 1 map unit over it, so one under the water
  // (or that the script's water reaches) throws its mines from the sea
  const launchers = spec && spec.type === "minefactory" ? spec.factory?.launchers ?? [] : [];
  launchers.forEach((l, i) => {
    const ly = l[1] / 20;
    if (Number.isFinite(top) && top > ly) out.warnings.push(`launcher ${i} (${len(l[0] / 20)}, ${len(ly)}, ${len(l[2] / 20)}) is under the water (${len(top)}${waterTop !== undefined && waterTop !== null && waterTop >= (water ?? -Infinity) ? ", the script's highest" : ""}): its mines start in the sea`);
    if (!spansAt(gs, l[0] / 20, l[2] / 20).length) out.warnings.push(`launcher ${i} has no ground under it: it throws from the air (fine, but put it on a caldera side to read as a vent)`);
  });
  // the advisory goes for a keepKnot factory standing on its knot (onSurface); without keepKnot, or with the knot over the
  // ground, it says what clears it
  if (!draped && !onSurface)
    out.warnings.push(`the factory's drop is the game's own sweep, and on Caldera it also missed undraped blocks inside the crater near the level's centre${keep
      ? ` (keepKnot keeps it at its knot then; the knot is ${len(y - landY)} over the ground: put it on the surface with place_object {pos, snap: "surface"} or move {drop: true} and this advisory goes)`
      : " (set_mine_factory {keepKnot: true} with the knot on the ground's top makes it stand on its knot, and this advisory goes)"}: Test and call game_mines {alive: true} (it answers whether the factory exists and where it settled)`);
  return out;
}

/** The check as answer lines ("error: " for problems), or none without a factory. */
export function factoryLines(c: FactoryCheck | null): string[] {
  if (!c) return [];
  return [...c.problems.map((p) => `error: ${p}`), ...c.warnings.map((w) => `warning: ${w}`)];
}
