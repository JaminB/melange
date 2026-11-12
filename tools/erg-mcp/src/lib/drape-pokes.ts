// Undraped blocks that poke out of a draped skin, for save and describe_level: a plug (a small solid block put under a
// skin where mines or worms fell through it) is meant to lie under the skin's surface, so whatever goes through the skin
// lands on it. One whose top stands over the skin's sheared surface is a lip in the trail instead: a worm trips on it and
// the skin's own slope is broken (Caldera v2 round 3: 4 x 3 x 4 leak plugs with their tops a quarter unit under the
// skin read as planned, but a plug on a steep flank, where the skin's surface falls across the plug's 4 units, stands out
// of the slope on its downhill side). A structure that stands on the skin (a castle, a pad, a tower) rises far over it
// and is not a poke: only a block whose top is nowhere more than `maxOver` over the skin is read as a plug. A low block
// meant to stand on the skin (boulders bedded into dunes, a kerb) is tagged by its name: the structure tools' `onSkin`
// names their new blocks ergonskin_N (ON_SKIN, also any frame whose name holds ONSKIN), and those are left out and
// counted, so the advisory stays for real plugs. Map units.
import { columnSpans } from "./inspect-field";
import type { Grid } from "./voxel";

/** A block named as a deliberate on-skin structure (the structure tools' `onSkin`). */
export const ON_SKIN = /ONSKIN/i;

/** How many added blocks are tagged on-skin (left out of drapePokes). */
export const onSkinBlocks = (gs: Grid[]) => gs.filter((g) => g.id < 0 && !g.frame.corners && ON_SKIN.test(g.frame.name)).length;

export interface Poke {
  block: number;                  // the block's frame id (negative: added in this project)
  name: string;
  skin: number;                   // the draped block it pokes out of
  columns: number;                // columns of the block whose top is over the skin's surface
  of: number;                     // columns of the block under the skin's footprint
  maxOver: number;                // the furthest the block's top stands over the skin
  at: [number, number];           // where
  blockTop: number;
  skinTop: number;
}

/** The undraped added blocks whose top pokes out of a draped block's surface by up to `maxOver` map units (a block
 * higher than that anywhere is a structure on the skin, not a plug). Each block once, at its worst column. */
export function drapePokes(gs: Grid[], maxOver = 3, lip = 0.05): Poke[] {
  const skins = gs.filter((g) => g.frame.corners), out: Poke[] = [];
  for (const g of gs) {
    if (g.frame.corners || g.id >= 0 || ON_SKIN.test(g.frame.name)) continue;
    const m = g.toMap, sc = m[0];
    if (!(sc > 0) || m[1] !== 0 || m[2] !== 0) continue;   // upright, unrotated: the blocks this project adds
    for (const d of skins) {
      if (d.bounds.max[0] <= g.bounds.min[0] || d.bounds.min[0] >= g.bounds.max[0] || d.bounds.max[2] <= g.bounds.min[2] || d.bounds.min[2] >= g.bounds.max[2]) continue;
      let worst: Poke | null = null, columns = 0, of = 0, structure = false;
      for (let cz = 0; cz < g.size[2] && !structure; cz++)
        for (let cx = 0; cx < g.size[0]; cx++) {
          const px = m[3] + (cx + 0.5) * sc, pz = m[11] + (cz + 0.5) * sc;
          const bt = columnSpans(g, px, pz, true)[0], st = columnSpans(d, px, pz, true)[0];
          if (!bt || !st) continue;
          of++;
          const over = bt.top - st.top;
          if (over > maxOver + 1e-6) { structure = true; break; }
          if (over <= lip) continue;
          columns++;
          if (!worst || over > worst.maxOver) worst = { block: g.id, name: g.frame.name, skin: d.id, columns: 0, of: 0, maxOver: over, at: [px, pz], blockTop: bt.top, skinTop: st.top };
        }
      if (worst && !structure) out.push({ ...worst, columns, of });
    }
  }
  return out.sort((a, b) => b.maxOver - a.maxOver);
}

/** The pokes as warning lines, rounded. */
export function pokeLines(pokes: Poke[]): string[] {
  const r2 = (v: number) => Math.round(v * 100) / 100;
  return pokes.map((p) => `${p.name} (frame ${p.block}) pokes out of the draped skin (frame ${p.skin}) by up to ${r2(p.maxOver)} map units at (${r2(p.at[0])}, ${r2(p.at[1])}): its top ${r2(p.blockTop)} over the skin's ${r2(p.skinTop)} in ${p.columns} of its ${p.of} columns; a plug belongs under the skin (lower it, or make it fit the slope)`);
}
