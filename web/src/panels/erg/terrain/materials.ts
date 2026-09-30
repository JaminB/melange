// Flat terrain colours per theme until the theme's material atlas is available: 64 shades around the theme's hue,
// deterministic per material index. setAtlas() replaces them with real per-material colours.

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
