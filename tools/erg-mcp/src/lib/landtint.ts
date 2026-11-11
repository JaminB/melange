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

export interface LandTint { digest: string; color: string; texture: string | null }

/** A texture given as a Prehistoric tile name ("P05") or a 16-hex digest -> its digest (lower case). */
export function textureDigest(t: string): { digest: string; texture: string | null } {
  const s = t.trim();
  const tile = PREHISTORIC_TILES[s.toUpperCase()];
  if (tile) return { digest: tile, texture: s.toUpperCase() };
  if (/^[0-9a-fA-F]{16}$/.test(s)) {
    const d = s.toLowerCase();
    const name = Object.entries(PREHISTORIC_TILES).find(([, v]) => v === d)?.[0] ?? null;
    return { digest: d, texture: name };
  }
  throw new Error(`texture: a Prehistoric tile name (P01-P64) or a 16-hex texture digest, not ${JSON.stringify(t)}`);
}

/** The tiles a material record draws: "top" its first, "side" its steep faces, "all" both (deduplicated). */
export function materialTiles(material: number | string, faces: "top" | "side" | "all" = "side"): { index: number; name: string; tiles: string[] } {
  const index = typeof material === "number" ? material : PREHISTORIC_MATERIALS.findIndex((r) => r[4].toLowerCase() === String(material).toLowerCase());
  const r = PREHISTORIC_MATERIALS[index];
  if (!r) throw new Error(`material: a Prehistoric material index (0-${PREHISTORIC_MATERIALS.length - 1}) or name, not ${JSON.stringify(material)}`);
  const pick = faces === "top" ? [r[0]] : faces === "side" ? [r[1], r[2]] : [r[0], r[1], r[2]];
  return { index, name: r[4], tiles: [...new Set(pick.filter((t) => t !== "NULL"))] };
}

/** The landscape tints a name carries (every tag). */
export function landTintsOf(name: string): LandTint[] {
  const out: LandTint[] = [];
  for (const m of name.matchAll(LTINT_RE)) {
    const digest = m[1].toLowerCase();
    out.push({ digest, color: `#${m[2].toLowerCase()}`, texture: Object.entries(PREHISTORIC_TILES).find(([, v]) => v === digest)?.[0] ?? null });
  }
  return out;
}

/** The carrier's name: the source piece's name, "_landtint" (so remove_scenery {search: "_landtint"} finds it) and the tag. */
export function carrierName(sourceName: string, digest: string, color: string): string {
  return `${sourceName.replace(LTINT_RE, "")}_landtint_ltint${digest.toLowerCase()}${color.replace(/^#/, "").toLowerCase()}`;
}
