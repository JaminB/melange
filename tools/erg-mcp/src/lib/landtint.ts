// Landscape tints (detail layer round 5): a level may recolour one of its landscape TEXTURES (the voxel/drape tiles the
// engine draws, not a place) toward a colour, keeping the texture's light and dark detail. The level carries it as a
// scenery copy whose name holds "_ltint", the 16 hex digits of the texture's digest (Melange's texture-tag scheme
// "gl-level0-sha256-64": SHA-256 of the engine's level-0 upload, the first 8 bytes) and six hex digits of colour
// (`VISIBLE_Horror5_landtint_ltintbfb9ab5911ad8e293d3632`). Melange's MirageTints reads the level's tags at match start
// and feeds `uniform vec4 mg_landTint` to Sunstone's landscape shaders for draws of that texture; with Sunstone off the
// landscape draws as before. The carrier is a scenery copy (no collision) and its name a visual channel of
// sim_fingerprint, so a landscape tint never changes the sim. Pure: tools/landtint.ts.

/** "_ltint" + 16 hex (digest) + 6 hex (colour), then the end or '_'. */
export const LTINT_RE = /_ltint([0-9a-fA-F]{16})([0-9a-fA-F]{6})(?=$|_)/g;

/** The Prehistoric theme's landscape tiles (ThemePrehistoric\Pnn.tga in Data\Bundles\Bundl16.xom), by name, with the
 * digest Melange computes for each (sunstone tools/tag_digest.py --xom, the same scheme as src/render/mirage/textags_logic.h). */
export const PREHISTORIC_TILES: Record<string, string> = {
  P01: "37afb0bc167da00e", P02: "d1d5c39867867017", P03: "75ba9bc143a67116", P04: "297c46e53bec309d", P05: "bfb9ab5911ad8e29", P06: "b61d727d4060caf9",
  P07: "049e2021928ca5e7", P08: "eaff948fcffa8f3f", P09: "af29cf6ed399a54e", P10: "a2598e76239b3982", P11: "a80ddc1725633b3e", P12: "bded03ef0836c56a",
  P13: "4cb56d8fdbb4228f", P14: "a53fee5e1ae20eff", P15: "2a2060a4f7a69859", P16: "73a1b0771614bbf5", P17: "39c537808fcce92f", P18: "09d213518626dc14",
  P19: "d383019d1b6df5ca", P20: "3ef814db0fa76189", P21: "26489319de6fe8b4", P22: "dc8d8d73d802fad5", P23: "b163985752a50941", P24: "d8a40f439caeed74",
  P25: "3c4f61628dba1dda", P26: "524e34186a704f1d", P27: "e80c916480ae3594", P28: "ef0219e3b37f5a22", P29: "af9504410839308d", P30: "58199c119c2f6087",
  P31: "c347e6542df38116", P32: "b931bce9c5aeafad", P33: "3c692dba562960c5", P34: "f2623becff114ac6", P35: "4dc6f6853e710b04", P36: "fd2ba3a65420cecc",
  P37: "ef61622199306abd", P38: "d323ae3b68a6d109", P39: "ecc16c9a3acae66f", P40: "6be5e29e3f91665b", P41: "5f32646d718193b4", P42: "f7b4f48a9b18780f",
  P43: "0152ea84b4d37cf5", P44: "a27bd74d8f1fe87b", P45: "cd97f3c712e00a43", P46: "59325d5a3a4017c2", P47: "28e3ece3c3429090", P48: "7e375b855fb6c991",
  P49: "c514ba1b5141fb88", P50: "b25e6b3d0f15549a", P51: "091a94c438c84fa5", P52: "f55bea972e412f89", P53: "81019953de3e1df0", P54: "c58dd46317320130",
  P55: "5e9a44a0802667b4", P56: "9fff4145377d2f42", P57: "c86a54e111a7cdb5", P58: "73a4a23b9e360d0e", P59: "57e81cc44c28f552", P60: "0924f9f6f3088b60",
  P61: "0b7780f99b2cb3a4", P62: "54ac91fa3acdd1e0", P63: "c88b6e58dec650a5", P64: "47ddc562e6cab01e",
};

/** ThemePrehistoric.txt's records by material index: [top, side, side, extra, name, preview] texture names (the first is
 * the face that looks up, the second and third the steep faces; Caldera's pale "slivers" are path02's and grass01's
 * side tile P05 and rock02's/grass02's/GreySlateFloor's P07). */
export const PREHISTORIC_MATERIALS: [string, string, string, string, string][] = [
  ["P01", "P05", "P05", "P14", "grass01"], ["P02", "P06", "P06", "P14", "grass01blend"], ["P03", "P07", "P07", "P15", "grass02"], ["P04", "P08", "P08", "P15", "grass02blend"],
  ["P05", "P05", "P05", "NULL", "rock01"], ["P61", "P61", "P61", "NULL", "rock02NEWBLEND"], ["P07", "P07", "P07", "NULL", "rock02"], ["P08", "P08", "P08", "NULL", "rock02blend"],
  ["P09", "P07", "P07", "NULL", "path01"], ["P10", "P05", "P05", "NULL", "path02"], ["P11", "P11", "P11", "NULL", "sand01"], ["P12", "P11", "P11", "NULL", "sand01blend"],
  ["P13", "P13", "P13", "NULL", "stone"], ["P21", "P16", "P21", "NULL", "treetrunk"], ["P21", "P47", "P21", "NULL", "treetrunkBlend"], ["P17", "P17", "P17", "NULL", "treeleaves"],
  ["P18", "P18", "P18", "NULL", "flint"], ["P19", "P19", "P19", "NULL", "strawHouse"], ["P22", "P22", "P22", "NULL", "lightWood"], ["P23", "P07", "P07", "NULL", "GreySlateFloor"],
  ["P50", "P07", "P07", "NULL", "GreySlateFloorBLEND"], ["P39", "P24", "P39", "NULL", "roundwindow01"], ["P39", "P38", "P39", "NULL", "roundwindow02"], ["P28", "P28", "P28", "NULL", "bone"],
  ["P48", "P48", "P48", "NULL", "boneBLEND"], ["P29", "P29", "P29", "NULL", "dinoegg01"], ["P31", "P31", "P31", "NULL", "dinoegg02"], ["P30", "P30", "P30", "NULL", "chippedstone"],
  ["P32", "P32", "P32", "NULL", "Dinoskin01"], ["P33", "P33", "P33", "NULL", "leaf01"], ["P36", "P36", "P36", "NULL", "redpetal"], ["P34", "P34", "P34", "NULL", "mosquitobody"],
  ["P35", "P35", "P35", "NULL", "mosquitowings"], ["P37", "P37", "P37", "NULL", "chippedstoneAxels"], ["P39", "P39", "P39", "NULL", "darkmossywood"], ["P26", "P26", "P26", "NULL", "mosswood01"],
  ["P27", "P27", "P27", "NULL", "mosswood02"], ["P39", "P25", "P39", "NULL", "door01"], ["P40", "P40", "P40", "NULL", "animalSKIN"], ["P41", "P41", "P41", "NULL", "steppedstalk"],
  ["P44", "P44", "P44", "NULL", "smoothstalk"], ["P42", "P42", "P42", "NULL", "dustystone"], ["P21", "P43", "P21", "NULL", "cycadtrunk"], ["P45", "P45", "P45", "NULL", "feathers"],
  ["P46", "P46", "P46", "NULL", "vineROPE"], ["P49", "P49", "P49", "NULL", "eye"], ["P51", "P51", "P51", "NULL", "cycad-blend"], ["P53", "P53", "P53", "NULL", "Mosquito-hair"],
  ["P52", "P52", "P52", "NULL", "Dinosaurskin2"], ["P57", "P57", "P57", "NULL", "VolcanoRock"], ["P58", "P58", "P58", "NULL", "LiquidHotMagma"], ["P54", "P54", "P54", "NULL", "HotRocks"],
  ["P60", "P60", "P60", "NULL", "MetalRivet"], ["P59", "P59", "P59", "NULL", "Metal"],
];

/** The Arabian theme's landscape tiles (ThemeArabian\Ann.tga in Data\Bundles\Bundl13.xom; tools/tag_digest.py --xom).
 * Some are one image under two names: A03 is A11 (grass02's top is the sand's), A14 = A15 = A60 = A61, and A62-A64 are
 * Prehistoric's P62-P64, so a tint of one draws on the others too (tileTwins). */
export const ARABIAN_TILES: Record<string, string> = {
  A01: "55d188b195b8fb00", A02: "335290ea7d90a1ad", A03: "cf7a74ae6a495453", A04: "35271f0d5eaa25c3", A05: "198ef08b85f29dbd", A06: "45b286f2546ae116",
  A07: "c5f49c6b37b89b64", A08: "f6d002160c7246b7", A09: "c7db86dac1be436a", A10: "2479eddfbf9ec34e", A11: "cf7a74ae6a495453", A12: "9f8e2cc964d2c148",
  A13: "15403853c992d13f", A14: "b7270dad0d8fefd9", A15: "b7270dad0d8fefd9", A16: "54f121563df677f4", A17: "a185febeb62fc487", A18: "a86dbaf1eb56fe6f",
  A19: "73dcbd4bbab6aa58", A20: "9a6013f6f3884ee5", A21: "6ae149ba1cd0b2fb", A22: "f7ce3ae95289552e", A23: "39799a074fee4c8c", A24: "86c46bf422218138",
  A25: "ad34227110aaa74d", A26: "8de26622f4a709d7", A27: "20a0e64a45901dab", A28: "b6415c45aa76b4d5", A29: "522c0bcb20510d3b", A30: "c4f4174f9c64646c",
  A31: "646c35be32f39798", A32: "08c014f3444b75d1", A33: "4646389dc31bd1f4", A34: "6a493df98f7928ce", A35: "090c3bdd367d3b28", A36: "d072d6b2abae83eb",
  A37: "09aeddcd652f9a2e", A38: "fa0a605be035b8ea", A39: "4347f46919b17fe6", A40: "cd77d4b784a8878b", A41: "6dc95550bfb9af6d", A42: "8c04ba908acb4401",
  A43: "4d0d93c4c2da47aa", A44: "59fc63db85b1d53d", A45: "f209ab4a0d50886c", A46: "8ca599532078db06", A47: "2cc8ad336059b001", A48: "63661ba7ea669b39",
  A49: "190c77b4edc965e1", A50: "dd103b0aa65a98bf", A51: "3e30567f01ec586c", A52: "f41620d8851ef6d1", A53: "404c955228493459", A54: "50a39ee6810b8974",
  A55: "6213e62d56c61e77", A56: "66f781a3218fb812", A57: "f7b6f204047188fc", A58: "ab43e0b579b2fbd8", A59: "81a187b165e596c2", A60: "b7270dad0d8fefd9",
  A61: "b7270dad0d8fefd9", A62: "54ac91fa3acdd1e0", A63: "c88b6e58dec650a5", A64: "47ddc562e6cab01e",
};

/** ThemeArabian.txt's records by material index, as PREHISTORIC_MATERIALS (sand01 and Sand draw A11, grass02's top too). */
export const ARABIAN_MATERIALS: [string, string, string, string, string][] = [
  ["A01", "A05", "A05", "NULL", "grass01"], ["A02", "A06", "A06", "NULL", "grass01blend"], ["A03", "A07", "A07", "NULL", "grass02"], ["A04", "A08", "A08", "NULL", "grass02blend"],
  ["A05", "A05", "A05", "NULL", "rock01"], ["A06", "A06", "A06", "NULL", "rock01blend"], ["A07", "A07", "A07", "NULL", "rock02"], ["A08", "A08", "A08", "NULL", "rock02blend"],
  ["A09", "A09", "A09", "NULL", "path01"], ["A10", "A10", "A10", "NULL", "path02"], ["A11", "A11", "A11", "NULL", "sand01"], ["A12", "A12", "A12", "NULL", "sand01blend"],
  ["A13", "A13", "A13", "NULL", "stone"], ["A14", "A14", "A14", "NULL", "Leaves"], ["A15", "A15", "A15", "NULL", "Bricks"], ["A16", "A16", "A16", "NULL", "sandy-plaster"],
  ["A17", "A17", "A17", "NULL", "sandy-plasterBLEND"], ["A18", "A18", "A18", "NULL", "cracked-pot"], ["A19", "A19", "A19", "NULL", "DomeTile"], ["A20", "A20", "A20", "NULL", "BlueStripeAwning"],
  ["A21", "A21", "A21", "NULL", "Metal"], ["A22", "A22", "A22", "NULL", "Plaster"], ["A23", "A23", "A23", "NULL", "PlasterBlend"], ["A12", "A12", "A12", "NULL", "SandBlend"],
  ["A11", "A11", "A11", "NULL", "Sand"], ["A26", "A26", "A26", "NULL", "Gold"], ["A27", "A27", "A27", "NULL", "GoldEdging"], ["A28", "A28", "A28", "NULL", "YellowRedStripeAwning"],
  ["A29", "A29", "A29", "NULL", "Reddiamond-carpet"], ["A30", "A30", "A30", "NULL", "Trellice"], ["A31", "A31", "A31", "NULL", "StarMosaic"], ["A32", "A32", "A32", "NULL", "RedCarpet"],
  ["A33", "A33", "A33", "NULL", "PlainTiles"], ["A34", "A34", "A34", "NULL", "Door1"], ["A35", "A35", "A35", "NULL", "Window1"], ["A36", "A36", "A36", "NULL", "PlasterPattern"],
  ["A37", "A37", "A37", "NULL", "PlainTileBlend"], ["A38", "A38", "A38", "NULL", "Columnbase"], ["A39", "A39", "A39", "NULL", "BlueMosaic1"], ["A40", "A40", "A40", "NULL", "BlueMosaic2"],
  ["A41", "A41", "A41", "NULL", "camel-fur"], ["A42", "A42", "A42", "NULL", "blue-crisscross"], ["A43", "A43", "A43", "NULL", "bigleaf"], ["A56", "A44", "A56", "NULL", "bamboo"],
  ["A45", "A45", "A45", "NULL", "purple-trim"], ["A46", "A46", "A46", "NULL", "superfancytrim"], ["A47", "A47", "A47", "NULL", "purple-plaster"], ["A48", "A48", "A48", "NULL", "sandy-window"],
  ["A24", "A24", "A24", "NULL", "LightWood-planks"], ["A25", "A25", "A25", "NULL", "bark"], ["A49", "A49", "A49", "NULL", "carpet3"], ["A50", "A50", "A50", "NULL", "Cloud"],
  ["A51", "A51", "A53", "NULL", "canon"], ["A52", "A52", "A52", "NULL", "ROPE"], ["A54", "A54", "A54", "NULL", "woodgoldpanel"], ["A55", "A55", "A55", "NULL", "plasterpattern"],
  ["A57", "A57", "A57", "NULL", "whitecanvas"], ["A58", "A58", "A58", "NULL", "darkyellowplaster"], ["A56", "A56", "A56", "NULL", "darkgreyplaster"], ["A59", "A59", "A59", "NULL", "PrisonSign"],
  ["A60", "A60", "A60", "NULL", "girder"], ["A63", "A62", "A63", "NULL", "girder"], ["A60", "A60", "A60", "NULL", "girder"],
];

/** The themes whose tiles and material records are known, by the tile names' letter. */
export const TINT_THEMES = {
  PREHISTORIC: { letter: "P", label: "Prehistoric", tiles: PREHISTORIC_TILES, materials: PREHISTORIC_MATERIALS },
  ARABIAN: { letter: "A", label: "Arabian", tiles: ARABIAN_TILES, materials: ARABIAN_MATERIALS },
} as const;
export type TintTheme = keyof typeof TINT_THEMES;

/** A level's databank theme ("ARABIAN", "Prehistoric") -> a theme with known tiles, or null. */
export function tintTheme(theme: string | null | undefined): TintTheme | null {
  const t = String(theme ?? "").trim().toUpperCase();
  return t in TINT_THEMES ? (t as TintTheme) : null;
}

/** A digest's tile name: the theme's own first, then the other known themes'. */
function tileOf(digest: string, theme: TintTheme): string | null {
  for (const th of [theme, ...(Object.keys(TINT_THEMES) as TintTheme[]).filter((x) => x !== theme)]) {
    const name = Object.entries(TINT_THEMES[th].tiles).find(([, v]) => v === digest)?.[0];
    if (name) return name;
  }
  return null;
}

/** Every tile of a theme that is this digest's image (a tint of one draws on all of them). */
export function tileTwins(digest: string, theme: TintTheme = "PREHISTORIC"): string[] {
  return Object.entries(TINT_THEMES[theme].tiles).filter(([, v]) => v === digest).map(([k]) => k);
}

export interface LandTint { digest: string; color: string; texture: string | null }

/** A texture given as a tile name (Prehistoric "P05", Arabian "A11") or a 16-hex digest -> its digest (lower case).
 * A digest's name is looked up in `theme` first (default Prehistoric). */
export function textureDigest(t: string, theme: TintTheme = "PREHISTORIC"): { digest: string; texture: string | null } {
  const s = t.trim(), up = s.toUpperCase();
  for (const th of Object.values(TINT_THEMES)) {
    const tiles: Record<string, string> = th.tiles;
    if (up.startsWith(th.letter) && tiles[up]) return { digest: tiles[up], texture: up };
  }
  if (/^[0-9a-fA-F]{16}$/.test(s)) {
    const d = s.toLowerCase();
    return { digest: d, texture: tileOf(d, theme) };
  }
  throw new Error(`texture: a Prehistoric tile name (P01-P64), an Arabian one (A01-A64) or a 16-hex texture digest, not ${JSON.stringify(t)}`);
}

/** The tiles a material record of `theme` (default Prehistoric) draws: "top" its first, "side" its steep faces, "all"
 * both (deduplicated). */
export function materialTiles(material: number | string, faces: "top" | "side" | "all" = "side", theme: TintTheme = "PREHISTORIC"): { index: number; name: string; tiles: string[] } {
  const recs = TINT_THEMES[theme].materials;
  const index = typeof material === "number" ? material : recs.findIndex((r) => r[4].toLowerCase() === String(material).toLowerCase());
  const r = recs[index];
  if (!r) throw new Error(`material: a ${TINT_THEMES[theme].label} material index (0-${recs.length - 1}) or name, not ${JSON.stringify(material)}`);
  const pick = faces === "top" ? [r[0]] : faces === "side" ? [r[1], r[2]] : [r[0], r[1], r[2]];
  return { index, name: r[4], tiles: [...new Set(pick.filter((t) => t !== "NULL"))] };
}

/** The landscape tints a name carries (every tag); `theme` names the textures first (default Prehistoric). */
export function landTintsOf(name: string, theme: TintTheme = "PREHISTORIC"): LandTint[] {
  const out: LandTint[] = [];
  for (const m of name.matchAll(LTINT_RE)) {
    const digest = m[1].toLowerCase();
    out.push({ digest, color: `#${m[2].toLowerCase()}`, texture: tileOf(digest, theme) });
  }
  return out;
}

/** The carrier's name: the source piece's name, "_landtint" (so remove_scenery {search: "_landtint"} finds it) and the tag. */
export function carrierName(sourceName: string, digest: string, color: string): string {
  return `${sourceName.replace(LTINT_RE, "")}_landtint_ltint${digest.toLowerCase()}${color.replace(/^#/, "").toLowerCase()}`;
}

/** "#rrggbb" read as an sRGB colour -> the linear-light value the tag carries. Sunstone's landscape shader multiplies
 * the tag's colour in linear light and the game's output is then gamma-encoded, so a tag written with the hex a picker
 * shows draws far lighter than it (Dustcrown round 2b: #c47f3c a flat orange, #f6eedb white). */
export function srgbToLinearHex(color: string): string {
  const h = color.replace(/^#/, "");
  let out = "#";
  for (let i = 0; i < 3; i++) {
    const v = parseInt(h.slice(2 * i, 2 * i + 2), 16) / 255;
    const lin = v <= 0.04045 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4;
    out += Math.round(lin * 255).toString(16).padStart(2, "0");
  }
  return out;
}

/** The inverse: a tag's linear colour -> the sRGB colour it draws as (for the renders' preview). */
export function linearToSrgb(color: string): [number, number, number] {
  const h = color.replace(/^#/, "");
  return [0, 1, 2].map((i) => {
    const v = parseInt(h.slice(2 * i, 2 * i + 2), 16) / 255;
    return Math.round((v <= 0.0031308 ? v * 12.92 : 1.055 * v ** (1 / 2.4) - 0.055) * 255);
  }) as [number, number, number];
}

/** The renders' tint preview: for each material of a material file (its record names, in index order) whose TOP tile a
 * land tint recolours, the colour the tint draws it as (the texture's mean becomes the tag's colour, shown in sRGB).
 * Records are matched by name against `theme`'s, so a file in another order still previews the right materials. */
export function tintedMaterialColours(names: string[], tints: LandTint[], theme: TintTheme): Map<number, [number, number, number]> {
  const out = new Map<number, [number, number, number]>();
  if (!tints.length) return out;
  const recs = TINT_THEMES[theme].materials, tiles: Record<string, string> = TINT_THEMES[theme].tiles;
  names.forEach((n, m) => {
    const r = recs.find((x) => x[4].toLowerCase() === String(n).toLowerCase());
    const t = r && tints.find((x) => x.digest === tiles[r[0]]);
    if (t) out.set(m, linearToSrgb(t.color));
  });
  return out;
}
