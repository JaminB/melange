// A small RGB raster and PNG encoder for the inspect tools (zlib from node:zlib; no dependencies), plus the colours and
// the 3x5 digit font the renders use.
import { deflateSync } from "node:zlib";

export type Rgb = [number, number, number];

const CRC = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

const crc32 = (b: Uint8Array) => {
  let c = 0xffffffff;
  for (let i = 0; i < b.length; i++) c = CRC[(c ^ b[i]) & 255] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
};

const be32 = (n: number) => [(n >>> 24) & 255, (n >>> 16) & 255, (n >>> 8) & 255, n & 255];

function chunk(type: string, data: Uint8Array): Uint8Array {
  const out = new Uint8Array(12 + data.length);
  out.set(be32(data.length), 0);
  for (let i = 0; i < 4; i++) out[4 + i] = type.charCodeAt(i);
  out.set(data, 8);
  out.set(be32(crc32(out.subarray(4, 8 + data.length))), 8 + data.length);
  return out;
}

/** An 8-bit truecolour PNG of w x h pixels from RGB bytes (3 per pixel, rows top to bottom). */
export function encodePng(w: number, h: number, rgb: Uint8Array): Uint8Array {
  if (rgb.length !== w * h * 3) throw new Error(`png: ${rgb.length} bytes for ${w}x${h}`);
  const raw = new Uint8Array((w * 3 + 1) * h);   // each row starts with filter type 0 (none)
  for (let y = 0; y < h; y++) raw.set(rgb.subarray(y * w * 3, (y + 1) * w * 3), y * (w * 3 + 1) + 1);
  const head = new Uint8Array([...be32(w), ...be32(h), 8, 2, 0, 0, 0]);   // 8 bits, RGB, deflate, no filter, no interlace
  const parts = [new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", head), chunk("IDAT", deflateSync(raw, { level: 6 })), chunk("IEND", new Uint8Array(0))];
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) { out.set(p, at); at += p.length; }
  return out;
}

const GLYPHS: Record<string, string> = {
  "0": "111101101101111", "1": "010110010010111", "2": "111001111100111", "3": "111001111001111", "4": "101101111001001",
  "5": "111100111001111", "6": "111100111101111", "7": "111001001001001", "8": "111101111101111", "9": "111101111001111",
  "-": "000000111000000", ".": "000000000000010",
};

export class Raster {
  readonly data: Uint8Array;
  constructor(readonly w: number, readonly h: number, fill: Rgb = [0, 0, 0]) {
    this.data = new Uint8Array(w * h * 3);
    this.rect(0, 0, w, h, fill);
  }
  set(x: number, y: number, c: Rgb) {
    if (x < 0 || y < 0 || x >= this.w || y >= this.h) return;
    const i = (y * this.w + x) * 3;
    this.data[i] = c[0]; this.data[i + 1] = c[1]; this.data[i + 2] = c[2];
  }
  get(x: number, y: number): Rgb {
    const i = (y * this.w + x) * 3;
    return [this.data[i], this.data[i + 1], this.data[i + 2]];
  }
  rect(x: number, y: number, w: number, h: number, c: Rgb) {
    const x0 = Math.max(0, x), x1 = Math.min(this.w, x + w), y1 = Math.min(this.h, y + h);
    for (let yy = Math.max(0, y); yy < y1; yy++)
      for (let xx = x0; xx < x1; xx++) this.set(xx, yy, c);
  }
  /** Blends colour c over the rectangle (a = 0..1). */
  tint(x: number, y: number, w: number, h: number, c: Rgb, a: number) {
    for (let yy = Math.max(0, y); yy < Math.min(this.h, y + h); yy++)
      for (let xx = Math.max(0, x); xx < Math.min(this.w, x + w); xx++) {
        const o = this.get(xx, yy);
        this.set(xx, yy, [o[0] + (c[0] - o[0]) * a, o[1] + (c[1] - o[1]) * a, o[2] + (c[2] - o[2]) * a].map(Math.round) as Rgb);
      }
  }
  /** A filled square marker with a dark outline, centred on (cx, cy). */
  marker(cx: number, cy: number, r: number, c: Rgb) {
    this.rect(cx - r - 1, cy - r - 1, 2 * r + 3, 2 * r + 3, [0, 0, 0]);
    this.rect(cx - r, cy - r, 2 * r + 1, 2 * r + 1, c);
  }
  /** A line from (x0, y0) to (x1, y1), `width` pixels wide; with `dash`, drawn `dash` pixels on and `dash` off. */
  line(x0: number, y0: number, x1: number, y1: number, c: Rgb, width = 1, dash = 0) {
    const n = Math.max(1, Math.ceil(Math.max(Math.abs(x1 - x0), Math.abs(y1 - y0)))), o = Math.floor((width - 1) / 2);
    for (let k = 0; k <= n; k++) {
      if (dash && Math.floor(k / dash) % 2) continue;
      this.rect(Math.round(x0 + ((x1 - x0) * k) / n) - o, Math.round(y0 + ((y1 - y0) * k) / n) - o, width, width, c);
    }
  }
  /** An X of half-size r with a dark outline, centred on (cx, cy). */
  cross(cx: number, cy: number, r: number, c: Rgb) {
    for (const [w, col] of [[3, [0, 0, 0] as Rgb], [1, c]] as const) {
      this.line(cx - r, cy - r, cx + r, cy + r, col, w);
      this.line(cx - r, cy + r, cx + r, cy - r, col, w);
    }
  }
  /** Digits, '-' and '.' in a 3x5 font at integer scale s; returns the width drawn. */
  text(x: number, y: number, s: string, c: Rgb, scale = 1): number {
    let cx = x;
    for (const ch of s) {
      const g = GLYPHS[ch];
      if (g) for (let i = 0; i < 15; i++) if (g[i] === "1") this.rect(cx + (i % 3) * scale, y + Math.floor(i / 3) * scale, scale, scale, c);
      cx += 4 * scale;
    }
    return cx - x;
  }
  png(): Uint8Array { return encodePng(this.w, this.h, this.data); }
}

export const hex = (c: Rgb) => `#${c.map((v) => v.toString(16).padStart(2, "0")).join("")}`;

/** A stable tint per material index (golden-angle hues), so the same material is the same colour in every render. */
export function materialTint(index: number): Rgb {
  const h = ((index * 137.508) % 360) / 60, s = 0.5, l = 0.55;
  const c = (1 - Math.abs(2 * l - 1)) * s, x = c * (1 - Math.abs((h % 2) - 1)), m = l - c / 2;
  const [r, g, b] = h < 1 ? [c, x, 0] : h < 2 ? [x, c, 0] : h < 3 ? [0, c, x] : h < 4 ? [0, x, c] : h < 5 ? [x, 0, c] : [c, 0, x];
  return [r + m, g + m, b + m].map((v) => Math.round(v * 255)) as Rgb;
}

export const shade = (c: Rgb, f: number): Rgb => c.map((v) => Math.max(0, Math.min(255, Math.round(v * f)))) as Rgb;

export const WATER: Rgb = [38, 92, 168];
export const VOID: Rgb = [24, 24, 28];
export const SKY: Rgb = [196, 214, 232];
