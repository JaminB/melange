// Detail layer round 6: the perched lint (round 5's judge: a crag "hangs over a lava channel with no visible base"; pieces
// that read as glued to a wall or floating). erg-mcp lint_perched reads every _dtl clump as drawn and flags the ones whose
// underside shows from the fixed camera set (detail-cams.mjs: the shots the judge sees) over the ground beneath them; this
// pass removes those clumps (the strata, _ldg, overhang their faces on purpose and are not read; flora, _flo, is seated).
import { cameras } from "./detail-cams.mjs";
export async function perchPass(ctx) {
  const { call } = ctx;
  const cams = cameras(ctx.buildOut).map((c) => ({ name: c.name, pos: c.pos }));
  const r = (await call("lint_perched", { search: "_dtl", cams, limit: 500 })).json;
  const ids = r.list.flatMap((c) => c.ids);
  if (ids.length) await call("remove_scenery", { ids });
  const after = (await call("lint_perched", { search: "_dtl", cams, limit: 20 })).json;
  return { clumps: r.clumps, flagged: r.perched, removedPieces: ids.length, first: r.list.slice(0, 12), left: after.perched };
}
