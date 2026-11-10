// Detail layer round 8: the SUPPORT check (the round-7 judge: "remove or re-seat every prop on faces steeper than about 55
// degrees that has no supporting ledge (the trail_NE mid-face cluster is the clearest case). Add a 'support' test: sample
// ground under the prop's footprint corners, and require at least 60% contact or an under-hang talus piece"). erg-mcp
// lint_support reads the rock layers (_dtl, _cpl) as drawn, clumped; a clump on ground over 55 degrees with under 60% of its
// footprint touching the ground and no other rock clump under its downhill edge is removed.
export async function supportPass(ctx) {
  const { call } = ctx;
  const r = (await call("lint_support", { search: ["_dtl", "_cpl"], minSlope: 55, minContact: 0.6, limit: 2000 })).json;
  const ids = r.unsupported.flatMap((c) => c.ids);
  if (ids.length) await call("remove_scenery", { ids });
  const after = (await call("lint_support", { search: ["_dtl", "_cpl"], minSlope: 55, minContact: 0.6, limit: 20 })).json;
  ctx.say?.("support", JSON.stringify({ clumps: r.clumps, counts: r.counts, removedPieces: ids.length, left: after.counts.unsupported }));
  return { clumps: r.clumps, counts: r.counts, removedPieces: ids.length, first: r.unsupported.slice(0, 12), after: after.counts };
}
