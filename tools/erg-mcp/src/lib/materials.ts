// Material files for the inspect tools: which file a name means, its record names and its real colours (the server's
// level.materials with colors: the average of each record's surface texture), cached per connection. The renders tint
// voxels with these colours; servers without them (older ones, the mock) fall back to distinct index hues.
import type { Session } from "../session";
import { hex, materialTint, type Rgb } from "./inspect-png";
import { landTintsOf, tintTheme, tintedMaterialColours } from "./landtint";

export interface MaterialSet {
  file: string;
  names: string[];
  colors: (Rgb | null)[] | null;           // per record; null without the server's colours
  resolved: boolean[] | null;              // the record's texture was found (else the colour is the server's stand-in)
}

const cache = new WeakMap<object, Map<string, MaterialSet>>();

/** A theme's own material file, as level.themes lists it: CAMELOT -> ThemeCamelot\ThemeCamelot.txt. */
export function themeFile(theme: string): string {
  const t = theme.charAt(0).toUpperCase() + theme.slice(1).toLowerCase();
  return `Theme${t}\\Theme${t}.txt`;
}

const squash = (f: string) => f.toLowerCase().replace(/[^a-z0-9.]/g, "");

/** The install's name for a material file the caller gave: slashes become backslashes, case is matched, and a name that
 * lost its separators (Mapsdinermight.txt, a JSON "\d" read as "d") is matched to the listed file it squashes to. */
export async function resolveFile(s: Session, given: string): Promise<{ file: string; note?: string }> {
  const norm = given.trim().replace(/\//g, "\\");
  let files: string[] = [];
  try {
    files = (await (await s.levels()).themes()).materialFiles ?? [];
  } catch {
    return { file: norm };
  }
  const exact = files.find((f) => f.toLowerCase() === norm.toLowerCase());
  if (exact) return { file: exact, ...(exact !== given ? { note: `read as ${exact}` } : {}) };
  const loose = files.filter((f) => squash(f) === squash(norm));
  if (loose.length === 1) return { file: loose[0], note: `'${given}' is not a listed material file; read as ${loose[0]} (in JSON a backslash is written \\\\)` };
  return { file: norm };
}

/** Names and colours of a material file. `base`/`source` resolve a pack level's own Maps\<stem>.txt. */
export async function loadMaterials(s: Session, file: string, theme: string | undefined, base?: { key: string; source: "game" | "pack" }): Promise<MaterialSet> {
  const client = await s.client();
  let per = cache.get(client);
  if (!per) cache.set(client, (per = new Map()));
  const key = `${file}|${theme ?? ""}|${base?.key ?? ""}`;
  const hit = per.get(key);
  if (hit) return hit;
  const params: Record<string, unknown> = { file, colors: true, ...(theme ? { theme } : {}), ...(base ? { base: base.key, source: base.source } : {}) };
  const r = await client.call<{ file: string; names: string[]; colors?: unknown; resolved?: unknown }>("level.materials", params, 60000);
  const colors = Array.isArray(r.colors)
    ? r.colors.map((c) => (typeof c === "string" && /^#[0-9a-f]{6}$/i.test(c) ? [1, 3, 5].map((i) => parseInt(c.slice(i, i + 2), 16)) as Rgb : null))
    : null;
  const set: MaterialSet = { file: r.file ?? file, names: Array.isArray(r.names) ? r.names : [], colors,
    resolved: Array.isArray(r.resolved) ? r.resolved.map((v) => v === true) : null };
  per.set(key, set);
  return set;
}

/** The open project's materials (names and colours), or null when the server cannot say. */
export async function projectMaterials(s: Session): Promise<MaterialSet | null> {
  try {
    const { scene } = s.requireProject().store;
    if (!scene.databank.materialFile) return null;
    return await loadMaterials(s, scene.databank.materialFile, scene.databank.theme, { key: scene.base.key, source: scene.base.source });
  } catch {
    return null;
  }
}

export type PaletteMode = "real" | "index";

/** The tint of material m for the renders: the material's real colour when the server gave one and `mode` is real,
 * else a distinct hue per index. */
export function tintFor(set: MaterialSet | null, mode: PaletteMode = "real"): (m: number) => Rgb {
  if (mode === "index" || !set?.colors) return materialTint;
  const c = set.colors;
  return (m) => c[m] ?? materialTint(m);
}

/** tintFor with the level's landscape tints laid over it (the renders' `tints: true`): a material whose top tile a
 * tint_landscape carrier recolours draws in that colour. Null when the level has no tint the theme's records name. */
export function landTintPreview(store: { scene: { details: { name: string; src: unknown }[]; databank: { theme?: string | null } } }, set: MaterialSet | null, base: (m: number) => Rgb): { tint: (m: number) => Rgb; materials: { material: number; name: string; color: string }[] } | null {
  const theme = tintTheme(store.scene.databank.theme) ?? "PREHISTORIC";
  const tints = store.scene.details.filter((d) => d.src === null).flatMap((d) => landTintsOf(d.name, theme));
  if (!set || !tints.length) return null;
  const over = tintedMaterialColours(set.names, tints, theme);
  if (!over.size) return null;
  return { tint: (m) => over.get(m) ?? base(m), materials: [...over].map(([m, c]) => ({ material: m, name: set.names[m], color: hex(c) })) };
}

export const colourOf = (tint: (m: number) => Rgb, m: number) => hex(tint(m));
