// The theme's 64 materials as colours: the preview atlas's when the server has one, flat distinct colours otherwise.
import { MATERIALS } from "./voxel";

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
