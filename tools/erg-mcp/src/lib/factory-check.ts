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
// the drop hits nothing, so for it those two are warnings and the water is checked against the knot. Map units throughout.
import type { EditorStore } from "../core";
import { markers, spansAt, waterMap } from "./inspect-field";
import { grids } from "./voxel";

/** The factory sweep's reach under the knot, map units (1000 world). */
export const FACTORY_DROP = 50;
/** Water this far over the point it settled on sets it off (10 world units). */
export const FACTORY_WATER = 0.5;

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
  if (sp.top > y + 1e-6) out.warnings.push(`the mine factory's knot is inside solid ground (its top is ${len(sp.top - y)} higher): put it on the surface`);
  if (draped && !keep) out.problems.push("the mine factory's knot is over a draped skin (heightfield_drape's corner offsets): the game's drop never landed on one in the Tests (it settles at a stale point, here under the sea, and explodes at load): set keepKnot (the factory then stays at its knot when its drop misses; place it with snap: \"surface\" so the knot is on the skin), or put it on an undraped block (a castle, keep or add_block pad), and Test with game_mines {alive: true}");
  if (draped && keep) out.warnings.push("the mine factory's knot is over a draped skin, where the game's drop has missed: with keepKnot it stays at its knot, so keep the knot on the skin's surface (snap: \"surface\")");
  if (Number.isFinite(top) && top > landY + FACTORY_WATER)
    out.problems.push(`the water (${len(top)}${waterTop !== undefined && waterTop !== null && waterTop >= (water ?? -Infinity) ? ", the script's highest" : ""}) gets more than ${len(FACTORY_WATER)} over the ground the factory lands on (${len(landY)}): it explodes${water !== null && water > landY + FACTORY_WATER ? " at load" : " when the water gets there"}`);
  if (!draped) out.warnings.push(`the factory's drop is the game's own sweep, and on Caldera it also missed undraped blocks inside the crater near the level's centre${keep ? " (keepKnot keeps it at its knot then)" : ""}: Test and call game_mines {alive: true} (it answers whether the factory exists and where it settled)`);
  return out;
}

/** The check as answer lines ("error: " for problems), or none without a factory. */
export function factoryLines(c: FactoryCheck | null): string[] {
  if (!c) return [];
  return [...c.problems.map((p) => `error: ${p}`), ...c.warnings.map((w) => `warning: ${w}`)];
}
