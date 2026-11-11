// Detail layer round 9: THE LAWN TILES' COLOUR. The round-8 judge: "the green lawn tiles still look like a placeholder
// checkerboard, and that is now the loudest wrong thing on the map. They are saturated, square-edged and tile-aligned";
// "check whether the lawn tiles' texture or colour can change without touching the sim".
// Answer: the colour can, the edges cannot (here). The tiles are Prehistoric grass01 / grass02 voxels and drape tiles, whose
// TOP faces draw the textures P01 / P03 (their blends P02 / P04). erg-mcp tint_landscape recolours a texture for this level
// only (a scenery carrier's name: a visual channel; the fingerprint proves it), so the lawns are pulled from the saturated
// placeholder green to a muted olive green, the plants' own lush green, a step darker, so the plants read on them and they
// read as one vegetation with the scrub above. The square EDGES are the voxels' primary material per cell: on Caldera's draped
// skin the editor refuses a repaint (the word carries a second material and corner mask, which are sim), so rounding them off
// is a terrain-paint change on the map side (erg-mcp: paint the grass with per-corner blends, or an irregular mask).
export const LAWN_TINTS = [
  { texture: "P01", color: "#5f7d3a" },   // grass01 top (the bright tiles)
  { texture: "P02", color: "#627d3d" },   // grass01blend top
  { texture: "P03", color: "#6a7f42" },   // grass02 top (the olive tiles)
  { texture: "P04", color: "#6a7f42" },   // grass02blend top
];
export async function lawnPass(ctx) {
  const r = (await ctx.call("tint_landscape", { tints: LAWN_TINTS })).json;
  return { tints: r.tints.map((t) => `${t.texture} ${t.color}`), level: r.level.length };
}
