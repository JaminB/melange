// Dune readability (describe_dunes): whether a field of dunes reads as crested dunes, measured instead of eyeballed.
// Dustcrown's dunes were judged "soft hills" for three rounds from screenshots; what makes a dune read is a sharp
// crest (the slope breaks at the top instead of rolling over), a slip face steeper than the windward side, and a crest
// that stands off its trough in colour. All three are numbers of a height profile and of the materials along it. Pure:
// a profile in, the crests out. Map units, degrees.

export interface ProfileSample { d: number; y: number; material: number; }   // distance along the line, ground height, top material

export interface Crest {
  d: number; y: number;              // where along the line, and its height
  prominence: number;                // over the higher of the troughs beside it
  /** The slope either side of the crest, degrees, over `near` of run: `windward` the gentler side, `lee` the steeper
   * (the slip face). */
  windward: number; lee: number; leeSide: "before" | "after";
  /** The mean slope from each trough up to the crest, degrees. */
  rise: { before: number; after: number };
  /** How far the surface turns at the crest: windward + lee, the angle between the two faces' tangents less 180. A
   * rolling hill turns a few degrees within `near` of its top; a dune's crest 40 or more. */
  slopeBreak: number;
  /** How long the top is flat: the run through the crest where the ground slopes under 5 degrees. A dune's crest is a
   * line (0); a rolling hill's top a few units; a mesa's as wide as the mesa. */
  topWidth: number;
  troughBefore: { d: number; y: number } | null; troughAfter: { d: number; y: number } | null;
  wavelength: number | null;         // trough to trough
  materials: { crest: number; trough: number | null };
}

/** Degrees of a rise over a run. */
const deg = (rise: number, run: number) => (Math.atan2(Math.abs(rise), Math.max(1e-9, run)) * 180) / Math.PI;

/** The crests of a height profile sampled evenly: local maxima at least `minProminence` over the higher trough beside
 * them, with the slopes within `near` of each. The profile is smoothed over `smooth` first (default one sample: the
 * heights as they are), which keeps a voxel step from reading as a crest. */
export function findCrests(p: ProfileSample[], o: { minProminence?: number; near?: number; smooth?: number } = {}): Crest[] {
  const n = p.length;
  if (n < 5) return [];
  const step = (p[n - 1].d - p[0].d) / (n - 1), minProm = o.minProminence ?? 1.5, near = Math.max(step, o.near ?? 2);
  const half = Math.max(0, Math.round(((o.smooth ?? 0) / step - 1) / 2));
  const y = p.map((_, i) => { let s = 0, c = 0; for (let j = Math.max(0, i - half); j <= Math.min(n - 1, i + half); j++) { s += p[j].y; c++; } return s / c; });
  // candidate maxima (the middle of a flat top), then prominence by walking down each side to the lowest point before
  // ground higher than the crest
  const out: Crest[] = [];
  for (let i = 1; i < n - 1; i++) {
    if (!(y[i] > y[i - 1] + 1e-9)) continue;
    let j = i;
    while (j < n - 1 && Math.abs(y[j + 1] - y[i]) <= 1e-9) j++;
    if (j >= n - 1 || !(y[j + 1] < y[i] - 1e-9)) { i = j; continue; }
    const top = Math.round((i + j) / 2);
    let lo = top, a = top;
    for (let q = top - 1; q >= 0 && y[q] <= y[top] + 1e-9; q--) if (y[q] < y[lo]) lo = q;
    for (let q = top + 1; q < n && y[q] <= y[top] + 1e-9; q++) if (y[q] < y[a]) a = q;
    const prom = y[top] - Math.max(y[lo], y[a]);
    i = j;
    if (prom < minProm - 1e-9) continue;
    const k = Math.max(1, Math.round(near / step));
    // each face's slope beside the crest: the steepest stretch of `near` that starts within `near` of the top (a skin
    // rounds a crest that falls between its corners, and the face is itself again a column on)
    const side = (dir: -1 | 1, end: number) => {
      let best = 0;
      for (let o = 0; o <= k; o++) {
        const p0 = top + dir * o, p1 = top + dir * (o + k);
        if (dir < 0 ? p1 < end : p1 > end) break;
        best = Math.max(best, deg(Math.max(0, y[p0] - y[p1]), k * step));
      }
      // a face shorter than `near`: what there is of it
      return best || deg(y[top] - y[end], Math.abs(end - top) * step);
    };
    const sb = side(-1, lo), sa = side(1, a);
    // the flat of the top: where the ground slopes under 5 degrees over a map unit (or a sample, when that is longer)
    const k1 = Math.max(1, Math.round(1 / step)), flat = Math.tan((5 * Math.PI) / 180) * k1 * step;
    let w0 = top, w1 = top;
    while (w0 - k1 >= 0 && Math.abs(y[w0] - y[w0 - k1]) < flat) w0--;
    while (w1 + k1 <= n - 1 && Math.abs(y[w1] - y[w1 + k1]) < flat) w1++;
    const leeAfter = sa >= sb;
    out.push({
      d: p[top].d, y: y[top], prominence: prom,
      windward: Math.min(sa, sb), lee: Math.max(sa, sb), leeSide: leeAfter ? "after" : "before",
      rise: { before: deg(y[top] - y[lo], (top - lo) * step), after: deg(y[top] - y[a], (a - top) * step) },
      slopeBreak: sa + sb, topWidth: (w1 - w0) * step,
      troughBefore: lo < top ? { d: p[lo].d, y: y[lo] } : null, troughAfter: a > top ? { d: p[a].d, y: y[a] } : null,
      wavelength: lo < top && a > top ? p[a].d - p[lo].d : null,
      materials: { crest: p[top].material, trough: (y[lo] <= y[a] ? p[lo] : p[a]).material },
    });
  }
  return out;
}

export interface DuneRules { minBreak: number; minLee: number; minAsymmetry: number; maxTopWidth: number; minContrast: number; }
/** What a crested dune measures at least (the angle of repose of dry sand is 30 to 34 degrees; a slip face in coarse
 * voxels reads from about 26). */
export const DUNE_RULES: DuneRules = { minBreak: 38, minLee: 26, minAsymmetry: 8, maxTopWidth: 3, minContrast: 18 };

/** A crest judged: which of the readings it misses, in words. `contrast` is its crest-to-trough difference in luma
 * (0-255; null: no colours known). */
export function crestFaults(c: Crest, contrast: number | null, r: DuneRules = DUNE_RULES): string[] {
  const out: string[] = [];
  if (c.slopeBreak < r.minBreak) out.push(`the crest rolls over (the slope turns ${Math.round(c.slopeBreak)} degrees at the top, under ${r.minBreak})`);
  if (c.lee < r.minLee) out.push(`no slip face (the steeper side is ${Math.round(c.lee)} degrees, under ${r.minLee})`);
  if (c.lee - c.windward < r.minAsymmetry) out.push(`symmetric (the two sides differ by ${Math.round(c.lee - c.windward)} degrees, under ${r.minAsymmetry}: a mound, not a dune)`);
  if (c.topWidth > r.maxTopWidth) out.push(`a flat top ${Math.round(c.topWidth * 10) / 10} wide (over ${r.maxTopWidth})`);
  if (contrast !== null && contrast < r.minContrast) out.push(`the crest and its trough are ${Math.round(contrast)} apart in brightness (under ${r.minContrast}: they read as one tone)`);
  return out;
}

/** How a crest shows to a worm standing in the trough before it: the angle it stands over the worm's eye (degrees; at
 * or under 0 it is below the eye's horizon and no skyline), with the eye `eye` over the trough. */
export function wormView(c: Crest, eye = 1): { from: "before" | "after"; elevation: number; skyline: boolean } | null {
  const t = c.troughBefore ?? c.troughAfter;
  if (!t) return null;
  const el = (Math.atan2(c.y - (t.y + eye), Math.max(1e-9, Math.abs(c.d - t.d))) * 180) / Math.PI;
  return { from: c.troughBefore ? "before" : "after", elevation: el, skyline: el > 0 };
}

const median = (v: number[]) => { if (!v.length) return 0; const s = [...v].sort((a, b) => a - b), m = s.length >> 1; return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2; };
export { median };
