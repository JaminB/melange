// Detail layer round 9: ONE ROCK, BANDED BY HEIGHT. The round-8 judge: "three rock languages fight each other" and "use one
// rock tint per elevation band so all the rock reads as one material". Every rock copy of the layer (the crags and scree
// _dtl, the cliff piles _cpl, the flank slabs _slb) takes its band's tint from erg-mcp tint_scenery {ramp} (a stepped ramp:
// each band one colour, the step between bands one value notch): wet-dark at the waterline, the basalt charcoal on the lower
// and middle cone, a hair warmer on the rim (the heat). No per-piece palette, no lighter scree grey: one stone.
export const ROCK_BANDS = [
  [-20, "#2e2f32"], [3.9, "#2e2f32"],          // waterline: wet
  [4, "#36373a"], [15.9, "#36373a"],           // lower cone
  [16, "#3a3b3e"], [31.9, "#3a3b3e"],          // middle cone
  [32, "#3c3b3c"], [80, "#3c3b3c"],            // rim and crater: a hair warmer
];
export async function retintPass(ctx) {
  const out = {};
  // round 10: the talus fans, strata ledges and column clusters (the caldera.* meshes, detail-basalt.mjs) take the bands too
  for (const q of ["_dtl", "_cpl", "_slb", "_tal", "_ldg", "_col"]) {
    const r = (await ctx.call("tint_scenery", { search: q, ramp: ROCK_BANDS, units: "map" }, { soft: true }));
    out[q] = r.err ? r.text.slice(0, 120) : (r.json?.changed ?? r.json?.count ?? r.json?.tinted ?? "ok");
  }
  return out;
}
