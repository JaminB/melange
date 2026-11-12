// A look grade for the offline renders: the colour a sunlit top of a material shows in the game against the colour the
// tools draw it in (the texture's mean, or a landscape tint's colour). The game lights the texture and, with the
// Sunstone graphics plugin, expands highlights, exposes, grades through a LUT and saturates, so a tint that looks ochre
// in render_view played as a saturated orange (Dustcrown round 3: the trough's #d8ad6a). A grade is fitted from pairs
// measured in a match (game_material_shots {saveGrade}): a saturation, a gain and an offset per channel in linear light,
// pulled toward leaving a colour as it is, and render_view / render_topdown {grade} apply it. Pure.
import type { Rgb } from "./inspect-png";

export interface Grade {
  /** rows for red, green and blue: out = a * r + b * g + c * b + d, on linear-light values 0..1 */
  m: number[][];
  pairs: number;                 // the pairs it was fitted from
  residual: number;              // the fit's RMS error on its pairs, sRGB 0-255
  saturation?: number;           // the fit's saturation and per-channel gains (m is these written out)
  gain?: number[];
  source?: string;               // where it was measured (level, time of day, mods)
  /** The materials it was measured on: the colour each was drawn in then and the colour the game showed. A render of a
   * material still drawn in that colour takes the game's colour as measured, not the fit's. */
  measured?: { material: number; from: Rgb; to: Rgb; tinted?: boolean }[];
  /** The fit over the tinted materials alone, when two or more were measured: a landscape tint's colour is drawn by
   * another path than a texture (the tint times the texel's luminance), and grades differently. */
  tint?: { m: number[][]; pairs: number; residual: number };
  /** The fit from each material's colour as drawn to the colour its darkest steep or shaded patch showed
   * (game_material_shots {faces}), when two or more were measured; `shadeMeasured` the pairs themselves. A tint that
   * reads ochre in the sun can play salmon on a lee face or in a wall's shade: this is what says so. */
  shade?: { m: number[][]; pairs: number; residual: number };
  shadeMeasured?: { material: number; from: Rgb; to: Rgb; tinted?: boolean; kind?: string }[];
}

/** The colour a material shows in SHADE (a lee face, a wall's shadow) by a grade: as measured there for this material
 * in this colour, else by the grade's shade fit; null when the grade was measured in the sun only. */
export function gradeShade(g: Grade, material: number, drawn: Rgb): Rgb | null {
  const hit = g.shadeMeasured?.find((x) => x.material === material && Math.max(Math.abs(x.from[0] - drawn[0]), Math.abs(x.from[1] - drawn[1]), Math.abs(x.from[2] - drawn[2])) <= 6);
  if (hit) return hit.to;
  return g.shade ? applyGrade({ ...g, m: g.shade.m }, drawn) : null;
}

/** Hue (degrees), saturation and value (0-1) of a colour. */
export function hueSat(c: Rgb): { hue: number; sat: number; value: number } {
  const r = c[0] / 255, g = c[1] / 255, b = c[2] / 255, hi = Math.max(r, g, b), lo = Math.min(r, g, b), d = hi - lo;
  let hue = 0;
  if (d > 1e-6) hue = hi === r ? ((g - b) / d + 6) % 6 : hi === g ? (b - r) / d + 2 : (r - g) / d + 4;
  return { hue: hue * 60, sat: hi > 0 ? d / hi : 0, value: hi };
}

/** What a colour's shade does to it, in words, when it leaves the family it was asked in: redder (toward salmon and
 * orange: the hue turns 8 degrees or more toward red and it is still saturated) or more saturated; null when it holds. */
export function shadeDrift(sun: Rgb, shade: Rgb): string | null {
  const a = hueSat(sun), b = hueSat(shade);
  let dh = b.hue - a.hue;
  if (dh > 180) dh -= 360; else if (dh < -180) dh += 360;
  const redder = a.hue <= 70 && dh <= -8 && b.sat >= 0.25;
  const richer = b.sat - a.sat >= 0.15;
  if (!redder && !richer) return null;
  return [redder ? `the hue turns ${Math.round(-dh)} degrees toward red (salmon or orange)` : null, richer ? `the saturation rises from ${Math.round(a.sat * 100)} to ${Math.round(b.sat * 100)} %` : null].filter(Boolean).join(" and ");
}

/** The colour a material draws in the game by a grade: as measured when the grade was measured on this material in this
 * colour (within 6 of it per channel), else by the tinted fit for a tinted material, else by the fit. */
export function gradeMaterial(g: Grade, material: number, drawn: Rgb, tinted = false): Rgb {
  const hit = g.measured?.find((x) => x.material === material && Math.max(Math.abs(x.from[0] - drawn[0]), Math.abs(x.from[1] - drawn[1]), Math.abs(x.from[2] - drawn[2])) <= 6);
  if (hit) return hit.to;
  return applyGrade(tinted && g.tint ? { ...g, m: g.tint.m } : g, drawn);
}

const toLin = (v: number) => { const c = v / 255; return c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4; };
const toSrgb = (v: number) => { const c = Math.max(0, Math.min(1, v)); return Math.round(255 * (c <= 0.0031308 ? c * 12.92 : 1.055 * c ** (1 / 2.4) - 0.055)); };

export const IDENTITY: Grade = { m: [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0]], pairs: 0, residual: 0 };

/** The colour as the game shows it. */
export function applyGrade(g: Grade, c: Rgb): Rgb {
  const l = [toLin(c[0]), toLin(c[1]), toLin(c[2])];
  return g.m.map((r) => toSrgb(r[0] * l[0] + r[1] * l[1] + r[2] * l[2] + r[3])) as Rgb;
}

const LUMA = [0.2126, 0.7152, 0.0722];

/** The grade that best maps each pair's `from` (the tools' colour) to its `to` (the game's): a saturation about the
 * colour's own luminance, then a gain and an offset per channel, all in linear light (seven numbers, written out as the
 * 3 x 4 map), least squares over the pairs by their `weight` (default 1) with a pull toward leaving a colour as it is
 * (`ridge`, default 0.02). A full affine map has twelve numbers and, fitted to a level's dozen sand-toned materials,
 * maps every other colour wildly; this one cannot. At least two pairs. */
export function fitGrade(pairs: { from: Rgb; to: Rgb; weight?: number }[], o: { ridge?: number; source?: string } = {}): Grade {
  if (pairs.length < 2) throw new Error("a grade needs at least two colour pairs");
  const X = pairs.map((p) => [toLin(p.from[0]), toLin(p.from[1]), toLin(p.from[2])]), Y = pairs.map((p) => [toLin(p.to[0]), toLin(p.to[1]), toLin(p.to[2])]);
  const wt = pairs.map((p) => Math.max(0, p.weight ?? 1)), wsum = wt.reduce((n, v) => n + v, 0) || 1;
  const ridge = (o.ridge ?? 0.02) * wsum;
  let best: { cost: number; s: number; g: number[]; off: number[] } | null = null;
  for (let s = 0.4; s <= 3.001; s += 0.05) {
    let cost = ridge * (s - 1) * (s - 1);
    const g: number[] = [], off: number[] = [];
    for (let c = 0; c < 3; c++) {
      // y = g x' + off with x' the saturated colour; the normal equations with the pull toward g 1, off 0
      let sxx = ridge, sx = 0, s1 = ridge, sxy = ridge, sy = 0;
      const xs = X.map((x) => { const L = LUMA[0] * x[0] + LUMA[1] * x[1] + LUMA[2] * x[2]; return L + s * (x[c] - L); });
      xs.forEach((x, n) => { sxx += wt[n] * x * x; sx += wt[n] * x; s1 += wt[n]; sxy += wt[n] * x * Y[n][c]; sy += wt[n] * Y[n][c]; });
      const det = sxx * s1 - sx * sx, gc = det > 1e-12 ? (sxy * s1 - sx * sy) / det : 1, oc = det > 1e-12 ? (sxx * sy - sx * sxy) / det : 0;
      g.push(gc); off.push(oc);
      xs.forEach((x, n) => { cost += wt[n] * (gc * x + oc - Y[n][c]) ** 2; });
      cost += ridge * ((gc - 1) ** 2 + oc * oc);
    }
    if (!best || cost < best.cost) best = { cost, s, g, off };
  }
  const { s, g, off } = best!;
  const r5 = (v: number) => Math.round(v * 1e5) / 1e5 + 0;   // + 0: no negative zero in the file
  const m = [0, 1, 2].map((c) => [...[0, 1, 2].map((k) => r5(g[c] * ((k === c ? s : 0) + (1 - s) * LUMA[k]))), r5(off[c])]);
  const gr: Grade = { m, pairs: pairs.length, residual: 0, saturation: Math.round(s * 100) / 100, gain: g.map((v) => Math.round(v * 1000) / 1000), ...(o.source ? { source: o.source } : {}) };
  let se = 0;
  pairs.forEach((p, n) => { const q = applyGrade(gr, p.from); for (let c = 0; c < 3; c++) se += wt[n] * (q[c] - p.to[c]) ** 2; });
  gr.residual = Math.round(Math.sqrt(se / (wsum * 3)) * 10) / 10;
  return gr;
}

/** A grade read from JSON text (game_material_shots' saveGrade file), or an Error saying what is wrong with it. */
export function parseGrade(text: string): Grade {
  let j: unknown;
  try { j = JSON.parse(text); } catch { throw new Error("the grade file is not JSON"); }
  const g = (j as { grade?: unknown })?.grade ?? j, m = (g as Grade)?.m;
  if (!Array.isArray(m) || m.length !== 3 || !m.every((r) => Array.isArray(r) && r.length === 4 && r.every((v) => typeof v === "number" && Number.isFinite(v))))
    throw new Error("the grade file has no m: three rows of four numbers");
  const o = g as Grade;
  const rgb = (v: unknown): v is Rgb => Array.isArray(v) && v.length === 3 && v.every((x) => typeof x === "number" && x >= 0 && x <= 255);
  const measured = Array.isArray(o.measured) ? o.measured.filter((x) => x && Number.isInteger(x.material) && rgb(x.from) && rgb(x.to)) : undefined;
  const tm = o.tint?.m, tint = Array.isArray(tm) && tm.length === 3 && tm.every((r) => Array.isArray(r) && r.length === 4 && r.every((v) => typeof v === "number" && Number.isFinite(v))) ? o.tint : undefined;
  const sm = o.shade?.m, shade = Array.isArray(sm) && sm.length === 3 && sm.every((r) => Array.isArray(r) && r.length === 4 && r.every((v) => typeof v === "number" && Number.isFinite(v))) ? o.shade : undefined;
  const shadeMeasured = Array.isArray(o.shadeMeasured) ? o.shadeMeasured.filter((x) => x && Number.isInteger(x.material) && rgb(x.from) && rgb(x.to)) : undefined;
  return { m, pairs: Number(o.pairs) || 0, residual: Number(o.residual) || 0, ...(typeof o.source === "string" ? { source: o.source } : {}), ...(measured?.length ? { measured } : {}), ...(tint ? { tint } : {}),
    ...(shade ? { shade } : {}), ...(shadeMeasured?.length ? { shadeMeasured } : {}) };
}

/** Grades measured in the game and kept here, by name. */
export const GRADES: Record<string, Grade> = {};
