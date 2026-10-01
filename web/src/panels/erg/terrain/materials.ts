// Terrain colours: flat per-theme shades for the view until the theme's material atlas is set, and the 64-material
// brush palette (the preview atlas's colours when the server has one, flat distinct colours otherwise).
import { MATERIALS } from "./voxel";

const THEME_HUE: Record<string, [number, number, number]> = {
  ARABIAN: [38, 0.45, 0.62], WILDWEST: [28, 0.45, 0.52], CAMELOT: [100, 0.3, 0.45], PREHISTORIC: [80, 0.35, 0.42],
  BUILDING: [30, 0.08, 0.55], ARCTIC: [200, 0.2, 0.82], ENGLAND: [110, 0.35, 0.42], HORROR: [280, 0.15, 0.38],
  LUNAR: [220, 0.05, 0.6], PIRATE: [45, 0.35, 0.58], WAR: [35, 0.25, 0.4],
};

const atlas = new Map<string, Uint8Array>();

function hsl(h: number, s: number, l: number): [number, number, number] {
  const k = (n: number) => (n + h / 30) % 12;
  const a = s * Math.min(l, 1 - l);
  const f = (n: number) => l - a * Math.max(-1, Math.min(k(n) - 3, Math.min(9 - k(n), 1)));
  return [Math.round(f(0) * 255), Math.round(f(8) * 255), Math.round(f(4) * 255)];
}

export function themePalette(theme: string): Uint8Array {
  const real = atlas.get(theme);
  if (real) return real;
  const [h, s, l] = THEME_HUE[theme] ?? THEME_HUE.BUILDING;
  const out = new Uint8Array(64 * 3);
  for (let i = 0; i < 64; i++) {
    const r = ((i * 2654435761) >>> 0) / 4294967296;
    const rgb = hsl((h + (r - 0.5) * 40 + 360) % 360, Math.min(1, s * (0.7 + r * 0.6)), Math.max(0.15, Math.min(0.9, l + (((i * 7) % 11) - 5) * 0.03)));
    out.set(rgb, i * 3);
  }
  return out;
}

export function setAtlas(theme: string, rgb: Uint8Array | null) {
  if (rgb && rgb.length >= 64 * 3) atlas.set(theme, rgb.slice(0, 64 * 3));
  else atlas.delete(theme);
}

const SKY: Record<string, [string, string]> = {
  DAY: ["#9cc9ec", "#dbeaf5"], EVENING: ["#e79a63", "#f3d1a8"], NIGHT: ["#1b2640", "#3a4865"],
};

/** Sky and fog colours for a time of day. */
export function skyColors(timeOfDay: string): [string, string] {
  return SKY[timeOfDay] ?? SKY.DAY;
}

export function flatPalette(): string[] {
  return Array.from({ length: MATERIALS }, (_, m) => `hsl(${Math.round((m * 137.508) % 360)}, ${45 + (m % 3) * 12}%, ${38 + (m % 4) * 8}%)`);
}

/** Up to 64 CSS colours from the atlas index; missing or malformed entries keep the flat colour. */
export function paletteFrom(colors: unknown): string[] {
  const out = flatPalette();
  if (!Array.isArray(colors)) return out;
  colors.slice(0, MATERIALS).forEach((c, i) => {
    if (typeof c === "string" && /^#[0-9a-f]{6}$/i.test(c)) out[i] = c;
  });
  return out;
}

/** The 64 material colours of a theme atlas PNG (8 x 8 cells of 16 px, material i in cell i), or null. */
export async function fetchAtlas(url: string): Promise<Uint8Array | null> {
  const res = await fetch(url, { credentials: "same-origin" });
  if (!res.ok) return null;
  const img = await createImageBitmap(await res.blob());
  const c = document.createElement("canvas");
  c.width = img.width;
  c.height = img.height;
  const g = c.getContext("2d");
  if (!g || img.width < 128 || img.height < 128) return null;
  g.drawImage(img, 0, 0);
  const px = g.getImageData(0, 0, 128, 128).data;
  const out = new Uint8Array(64 * 3);
  for (let i = 0; i < 64; i++) {
    const o = ((Math.floor(i / 8) * 16 + 8) * 128 + (i % 8) * 16 + 8) * 4;
    out.set([px[o], px[o + 1], px[o + 2]], i * 3);
  }
  return out;
}

export function hexColors(rgb: Uint8Array): string[] {
  const out: string[] = [];
  for (let i = 0; i < 64; i++) out.push(`#${[0, 1, 2].map((k) => rgb[i * 3 + k].toString(16).padStart(2, "0")).join("")}`);
  return out;
}

/** The record names from a level.materials reply (at most 64 strings), or undefined for anything else. */
export function materialNames(reply: unknown): string[] | undefined {
  const names = (reply as { names?: unknown } | null)?.names;
  return Array.isArray(names) && names.every((n) => typeof n === "string") ? (names as string[]).slice(0, MATERIALS) : undefined;
}
