// A rock bank's palette as the game would show it, before the bank is built: every rock drawn flat shaded from the
// south-east on the ground's colour, its sides in the palette's stone and its upward faces under the palette's dust, each
// colour through a look grade (lib/grade.ts) when one is given. Dustcrown's first boulders were built in a sandstone that
// played near white on the sand; the grade says so here, without a build and a match. Pure.
import { Raster, type Rgb } from "./inspect-png";
import { applyGrade, type Grade } from "./grade";
import type { BuiltRock } from "./rockmesh";

export interface RockSwatch { name: string; kind: string; base: Rgb; dust: Rgb; drawn: { base: string; dust: string }; contrast: { sun: number; shade: number } | null; }
const hx = (c: Rgb) => `#${c.map((v) => v.toString(16).padStart(2, "0")).join("")}`;
const lum = (c: Rgb) => 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];

/** The sheet (one cell a rock, `cell` pixels) and each rock's colours as drawn. `ground` (the colour the ground plays in,
 * already as the game shows it) is the background and the measure: `contrast` is how far the rock's lit top and its
 * shaded side stand from it in brightness, 0-255 (under about 12 the rock melts into the ground). */
export function rockSheet(rocks: BuiltRock[], o: { grade?: Grade | null; ground?: Rgb | null; cell?: number } = {}): { png: Uint8Array; width: number; height: number; swatches: RockSwatch[] } {
  const cell = o.cell ?? 160, cols = Math.min(4, Math.max(1, rocks.length)), rows = Math.ceil(rocks.length / cols);
  const bg: Rgb = o.ground ?? [150, 150, 150], g = (c: Rgb): Rgb => (o.grade ? applyGrade(o.grade, c) : c);
  const R = new Raster(cols * cell, rows * (cell + 14), bg);
  // the eye looks down 30 degrees from the south-east; the sun stands south-west, high
  const ca = Math.cos(Math.PI / 4), sa = Math.sin(Math.PI / 4), ce = Math.cos(Math.PI / 6), se = Math.sin(Math.PI / 6);
  const sun = [-0.45, 0.8, 0.4], sl = Math.hypot(...sun);
  const swatches: RockSwatch[] = [];
  rocks.forEach((rk, n) => {
    const ox = (n % cols) * cell, oy = Math.floor(n / cols) * (cell + 14);
    const base = g(rk.palette.base as Rgb), dustMix = rk.palette.dustShare ?? 1;
    const dust = g(rk.palette.dust.map((v, i) => Math.round(rk.palette.base[i] + (v - rk.palette.base[i]) * dustMix)) as Rgb);
    const t = rk.tris, nT = t.length / 9;
    // screen x to the right, y up, depth toward the eye
    const P = (i: number): [number, number, number] => {
      const x = t[i], y = t[i + 1], z = t[i + 2], rx = x * ca - z * sa, rz = x * sa + z * ca;
      return [rx, y * ce - rz * se, y * se + rz * ce];
    };
    let x0 = Infinity, x1 = -Infinity, y0 = Infinity, y1 = -Infinity;
    for (let i = 0; i < t.length; i += 3) { const p = P(i); x0 = Math.min(x0, p[0]); x1 = Math.max(x1, p[0]); y0 = Math.min(y0, p[1]); y1 = Math.max(y1, p[1]); }
    const sc = (cell - 16) / Math.max(x1 - x0, y1 - y0, 1e-6), mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
    const zb = new Float32Array(cell * cell).fill(-Infinity);
    for (let k = 0; k < nT; k++) {
      const a = P(k * 9), b = P(k * 9 + 3), c = P(k * 9 + 6);
      const ux = t[k * 9 + 3] - t[k * 9], uy = t[k * 9 + 4] - t[k * 9 + 1], uz = t[k * 9 + 5] - t[k * 9 + 2], vx = t[k * 9 + 6] - t[k * 9], vy = t[k * 9 + 7] - t[k * 9 + 1], vz = t[k * 9 + 8] - t[k * 9 + 2];
      let nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
      const nl = Math.hypot(nx, ny, nz) || 1;
      nx /= nl; ny /= nl; nz /= nl;
      // a hull's faces point outward; a face seen from behind is its other side
      const lit = Math.max(0, (nx * sun[0] + ny * sun[1] + nz * sun[2]) / sl), up = Math.max(0, ny);
      const w = up * up, col = base.map((v, i) => v + (dust[i] - v) * w), f = 0.45 + 0.55 * lit;
      const rgb = col.map((v) => Math.max(0, Math.min(255, Math.round(v * f)))) as Rgb;
      const S = [a, b, c].map((p) => [cell / 2 + (p[0] - mx) * sc, cell / 2 - (p[1] - my) * sc, p[2]]);
      const bx0 = Math.max(0, Math.floor(Math.min(S[0][0], S[1][0], S[2][0]))), bx1 = Math.min(cell - 1, Math.ceil(Math.max(S[0][0], S[1][0], S[2][0])));
      const by0 = Math.max(0, Math.floor(Math.min(S[0][1], S[1][1], S[2][1]))), by1 = Math.min(cell - 1, Math.ceil(Math.max(S[0][1], S[1][1], S[2][1])));
      const den = (S[1][1] - S[2][1]) * (S[0][0] - S[2][0]) + (S[2][0] - S[1][0]) * (S[0][1] - S[2][1]);
      if (Math.abs(den) < 1e-9) continue;
      for (let py = by0; py <= by1; py++) for (let px = bx0; px <= bx1; px++) {
        const w0 = ((S[1][1] - S[2][1]) * (px + 0.5 - S[2][0]) + (S[2][0] - S[1][0]) * (py + 0.5 - S[2][1])) / den;
        const w1 = ((S[2][1] - S[0][1]) * (px + 0.5 - S[2][0]) + (S[0][0] - S[2][0]) * (py + 0.5 - S[2][1])) / den, w2 = 1 - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        const z = w0 * S[0][2] + w1 * S[1][2] + w2 * S[2][2];
        if (z <= zb[py * cell + px]) continue;
        zb[py * cell + px] = z;
        R.set(ox + px, oy + py, rgb);
      }
    }
    R.text(ox + 4, oy + cell + 2, rk.name.slice(0, 22), lum(bg) > 128 ? [20, 20, 20] : [240, 240, 240]);
    const top = dust.map((v) => Math.round(v)) as Rgb, side = base.map((v) => Math.round(v * 0.45)) as Rgb;
    swatches.push({ name: rk.name, kind: rk.kind, base: rk.palette.base as Rgb, dust: rk.palette.dust as Rgb, drawn: { base: hx(base), dust: hx(dust) },
      contrast: o.ground ? { sun: Math.round(Math.abs(lum(top) - lum(bg))), shade: Math.round(Math.abs(lum(side) - lum(bg))) } : null });
  });
  return { png: R.png(), width: R.w, height: R.h, swatches };
}
