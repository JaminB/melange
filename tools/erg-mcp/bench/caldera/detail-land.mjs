// Detail layer round 5: the cone's pale sliver faces. The steep faces of a voxel or drape tile draw with its material's
// SIDE texture, and in the Prehistoric theme the side tile of path02 (the trails' bed) and grass01 is P05, a pale grey
// (#9b9893), and that of rock02, grass02 and GreySlateFloor is P07 (#848387): on the black cone they read as pale
// blue-grey slivers ("broken glass"). erg-mcp tint_landscape recolours those two TEXTURES in this level only (Melange
// MirageTints -> Sunstone's landscape shaders, mg_landTint), keeping their detail: P05 toward a warm dark basalt a step
// lighter than VolcanoRock (P57, #443f34), so a trail's edge reads as rock, not a hole; P07 a little lighter and greyer
// (the grey-rock streaks of the flank stay a variation of the basalt, not a different stone). Castles (stone, flint,
// chippedstone: P13, P18, P30), the trails' top (P10), sand (P11) and the lava are other textures and are untouched.
// The carriers are scenery copies: the fingerprint's sim hash cannot change (detail.mjs checks it after the pass).
export const LAND_TINTS = [
  { texture: "P05", color: "#3b3531" },
  { texture: "P07", color: "#46403b" },
];
export async function landPass(ctx) {
  const r = (await ctx.call("tint_landscape", { tints: LAND_TINTS })).json;
  return { tints: r.tints.map((t) => `${t.texture} ${t.color}`), level: r.level.length };
}
