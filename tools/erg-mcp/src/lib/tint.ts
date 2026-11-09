// Scenery tints: a scenery copy's name may end in a tint tag, "_tint<rrggbb>" (VISIBLE_Horror5_dtl_tint3a302b). Melange's
// MirageTints module reads the level's tinted copies at match start and hands the colour to GLSL replacements that
// declare `uniform vec4 mg_tint` (Sunstone's model shaders), which recolour the piece toward it, keeping the texture's
// light and dark detail (a texel of luminance 0.5 becomes the colour). Without such a shader (Sunstone off) the piece
// draws in its own texture. A name is a visual channel of sim_fingerprint (scenery copies have no collision), so a tint
// never changes the sim. Pure: tools/tint.ts, tools/dress.ts, tools/kitbash.ts.
export const TINT_RE = /_tint([0-9a-fA-F]{6})(?=$|_)/g;

/** "#3a302b" or "3a302b" -> "3a302b" (lower case); throws a readable message for anything else. */
export function tintHex(color: unknown, path = "tint"): string {
  if (typeof color !== "string" || !/^#?[0-9a-fA-F]{6}$/.test(color.trim())) throw new Error(`${path}: a colour as "#rrggbb", not ${JSON.stringify(color)}`);
  return color.trim().replace(/^#/, "").toLowerCase();
}

/** The name with its tint tags removed and, when color is given, "_tint<rrggbb>" appended. */
export function withTint(name: string, color: string | null): string {
  const bare = name.replace(TINT_RE, "");
  return color === null ? bare : `${bare}_tint${tintHex(color)}`;
}

/** The tint a name carries ("#rrggbb", the last tag), or null. */
export function tintOf(name: string): string | null {
  let last: string | null = null;
  for (const m of name.matchAll(TINT_RE)) last = `#${m[1].toLowerCase()}`;
  return last;
}

/** Mix two "#rrggbb" colours: t = 0 gives a, 1 gives b. */
export function mixHex(a: string, b: string, t: number): string {
  const pa = tintHex(a), pb = tintHex(b), k = Math.max(0, Math.min(1, t));
  let out = "#";
  for (let i = 0; i < 3; i++) {
    const va = parseInt(pa.slice(2 * i, 2 * i + 2), 16), vb = parseInt(pb.slice(2 * i, 2 * i + 2), 16);
    out += Math.round(va + (vb - va) * k).toString(16).padStart(2, "0");
  }
  return out;
}
