// Detail layer round 5: the lint pass, after every placing pass (erg-mcp lint_dressing on the "_dtl" copies). A small dark
// clump (under 1.5 across, darker than a mid grey) on walkable dry ground reads as a mine (round 4's judge: "any isolated
// dark piece under ~1.5 units on sand or a walkable surface must go"): every such clump is removed. Hovering and buried
// clumps are counted (every placing pass seats its pieces into the ground, so there should be none), and the density per
// band of ground height is kept in the report, so sparseness is a number.
const WATER = 1;
export async function lintPass(ctx) {
  const { call } = ctx;
  const lint = (await call("lint_dressing", { search: "_dtl", water: WATER + 0.3, limit: 500 })).json;
  const sus = lint.mineSuspects.list.flatMap((c) => c.ids);
  if (sus.length) await call("remove_scenery", { ids: sus });
  const after = (await call("lint_dressing", { search: "_dtl", water: WATER + 0.3, limit: 50 })).json;
  return { removedMineSuspects: lint.mineSuspects.count, removedPieces: sus.length, pieces: after.pieces, clumps: after.clumps, mineSuspectsLeft: after.mineSuspects.count,
    hovering: after.contact.hovering, buried: after.contact.buried, contactList: after.contact.list.slice(0, 20), density: after.density };
}
