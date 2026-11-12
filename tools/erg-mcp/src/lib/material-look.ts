// game_material_shots' helpers: where each material shows on the open level, and how a shot of it compares with the
// material file's swatch colour. preview_materials' tile is the texture's mean colour, which is not the in-game look:
// the game lights and tiles the texture, and the Sunstone graphics plugin's landscape shader adds material detail per
// texture class (its texture tags: a stone_rock texture gets grime, and moss where its pits face the sky, by its wear
// parameter), so GreySlateFloor's warm grey tile came out pale off-white with green blotches in the game (Caldera v2
// round 4). A shot of each material on the level itself, with the plugins the Test runs with, is the look the players
// see. Pure: grids in, patches out; map units.
import { spansAt, extentXZ } from "./inspect-field";
import type { Rgb } from "./inspect-png";
import type { Grid } from "./voxel";

export interface Patch { material: number; at: [number, number, number]; columns: number; samples: number; }

/** For each material (or `only` those), the centre of its best exposed patch on the level: the ground's top is sampled
 * every `step` (default 2 map units) over the frames' extent (ground under `water` left out), and each sample scores
 * the samples of the same material within `radius` (default 4) and the flatness round it (a sample whose neighbours
 * differ by more than 1.5 map units in height scores lower), so a shot looks at a flat stretch of the material rather
 * than one voxel in a cliff. `samples` is the material's whole count, `columns` the score of the pick. */
export function materialPatches(gs: Grid[], o: { step?: number; radius?: number; only?: number[]; water?: number | null } = {}): Patch[] {
  const ext = extentXZ(gs);
  if (!ext) return [];
  const step = o.step ?? 2, radius = o.radius ?? 4, want = o.only ? new Set(o.only) : null;
  const W = Math.max(1, Math.ceil((ext.x1 - ext.x0) / step)), H = Math.max(1, Math.ceil((ext.z1 - ext.z0) / step));
  const mat = new Int16Array(W * H).fill(-1), top = new Float32Array(W * H);
  for (let j = 0; j < H; j++)
    for (let i = 0; i < W; i++) {
      const sp = spansAt(gs, ext.x0 + (i + 0.5) * step, ext.z0 + (j + 0.5) * step)[0];
      if (!sp || (o.water !== undefined && o.water !== null && sp.top <= o.water)) continue;
      if (want && !want.has(sp.material)) continue;
      mat[j * W + i] = sp.material;
      top[j * W + i] = sp.top;
    }
  const R = Math.max(1, Math.round(radius / step));
  const best = new Map<number, Patch>(), counts = new Map<number, number>();
  for (let j = 0; j < H; j++)
    for (let i = 0; i < W; i++) {
      const m = mat[j * W + i];
      if (m < 0) continue;
      counts.set(m, (counts.get(m) ?? 0) + 1);
      let same = 0, flat = 0, n = 0;
      for (let dj = -R; dj <= R; dj++)
        for (let di = -R; di <= R; di++) {
          const a = i + di, b = j + dj;
          if (a < 0 || b < 0 || a >= W || b >= H || di * di + dj * dj > R * R) continue;
          n++;
          if (mat[b * W + a] !== m) continue;
          same++;
          if (Math.abs(top[b * W + a] - top[j * W + i]) <= 1.5) flat++;
        }
      const score = same + flat;
      const cur = best.get(m);
      if (!cur || score > cur.columns) best.set(m, { material: m, at: [ext.x0 + (i + 0.5) * step, top[j * W + i], ext.z0 + (j + 0.5) * step], columns: score, samples: 0 });
    }
  return [...best.values()].map((p) => ({ ...p, at: p.at.map((v) => Math.round(v * 100) / 100) as [number, number, number], samples: counts.get(p.material) ?? 0 })).sort((a, b) => a.material - b.material);
}

/** The mean colour of the middle of a picture: the box `part` (default 0.3) of its width and height round the centre. */
export function centreColour(rgb: Uint8Array, w: number, h: number, part = 0.3): Rgb {
  const x0 = Math.floor((w * (1 - part)) / 2), x1 = Math.ceil((w * (1 + part)) / 2), y0 = Math.floor((h * (1 - part)) / 2), y1 = Math.ceil((h * (1 + part)) / 2);
  let r = 0, g = 0, b = 0, n = 0;
  for (let y = y0; y < y1; y++)
    for (let x = x0; x < x1; x++) { const i = (y * w + x) * 3; r += rgb[i]; g += rgb[i + 1]; b += rgb[i + 2]; n++; }
  return n ? [Math.round(r / n), Math.round(g / n), Math.round(b / n)] : [0, 0, 0];
}

/** How a shot's colour differs from the swatch: per channel, in lightness (0-255), and in words (lighter or darker,
 * the hue it leans to), so "warm grey on the sheet, pale off-white with a green cast in the game" is a number. */
export function lookDelta(swatch: Rgb, inGame: Rgb): { delta: Rgb; lightness: number; cast: string; words: string } {
  const lum = (c: Rgb) => c[0] * 0.3 + c[1] * 0.59 + c[2] * 0.11;
  const delta = inGame.map((v, i) => v - swatch[i]) as Rgb, dl = Math.round(lum(inGame) - lum(swatch));
  // the cast: the channel that gains most over the others, once the lightness change is taken out
  const even = delta.map((d) => d - dl) as Rgb, names = ["red", "green", "blue"];
  const i = even.indexOf(Math.max(...even)), cast = even[i] > 12 ? names[i] : "none";
  const light = Math.abs(dl) < 12 ? "about as light as the swatch" : dl > 0 ? `${dl > 60 ? "much " : ""}lighter (+${dl}) than the swatch` : `${dl < -60 ? "much " : ""}darker (${dl}) than the swatch`;
  return { delta, lightness: dl, cast, words: `${light}${cast !== "none" ? `, with a ${cast} cast` : ""}` };
}

export type PatchKind = "steep" | "shade";
export interface FacePatch extends Patch { kind: PatchKind; facing: "n" | "e" | "s" | "w"; slope?: number; }

/** More patches of each material than the flat sunlit one, for a grade that knows shade: `steep` are stretches of the
 * material on ground steeper than `minSlope` (default 30 degrees), one per way the slope faces (n, e, s, w: the side it
 * falls toward), and `shade` are flat stretches at the foot of ground at least `wall` (default 3 map units) higher
 * within `reach` (default 4) on that side. Which of them the sun reaches depends on the level's light, so the shots
 * are told apart by their brightness afterwards (the darkest is the shade). Up to four of each kind a material. */
export function facePatches(gs: Grid[], o: { step?: number; only?: number[]; water?: number | null; kinds?: PatchKind[]; minSlope?: number; wall?: number; reach?: number } = {}): FacePatch[] {
  const ext = extentXZ(gs);
  if (!ext) return [];
  const step = o.step ?? 2, want = o.only ? new Set(o.only) : null, kinds = new Set(o.kinds ?? ["steep", "shade"]);
  const W = Math.max(1, Math.ceil((ext.x1 - ext.x0) / step)), H = Math.max(1, Math.ceil((ext.z1 - ext.z0) / step));
  const mat = new Int16Array(W * H).fill(-1), top = new Float32Array(W * H).fill(NaN);
  for (let j = 0; j < H; j++)
    for (let i = 0; i < W; i++) {
      const sp = spansAt(gs, ext.x0 + (i + 0.5) * step, ext.z0 + (j + 0.5) * step)[0];
      if (!sp || (o.water !== undefined && o.water !== null && sp.top <= o.water)) continue;
      mat[j * W + i] = sp.material;
      top[j * W + i] = sp.top;
    }
  const at = (i: number, j: number) => (i < 0 || j < 0 || i >= W || j >= H ? NaN : top[j * W + i]);
  const tan = Math.tan(((o.minSlope ?? 30) * Math.PI) / 180), wall = o.wall ?? 3, reach = Math.max(1, Math.round((o.reach ?? 4) / step));
  const DIRS = [["n", 0, -1], ["e", 1, 0], ["s", 0, 1], ["w", -1, 0]] as const;
  const best = new Map<string, FacePatch>(), counts = new Map<number, number>();
  for (let j = 0; j < H; j++)
    for (let i = 0; i < W; i++) {
      const m = mat[j * W + i];
      if (m < 0 || (want && !want.has(m))) continue;
      counts.set(m, (counts.get(m) ?? 0) + 1);
      const h = top[j * W + i], hx0 = at(i - 1, j), hx1 = at(i + 1, j), hz0 = at(i, j - 1), hz1 = at(i, j + 1);
      if ([hx0, hx1, hz0, hz1].some(Number.isNaN)) continue;
      const gx = (hx1 - hx0) / (2 * step), gz = (hz1 - hz0) / (2 * step), slope = Math.hypot(gx, gz);
      const same = [[1, 0], [-1, 0], [0, 1], [0, -1]].filter(([di, dj]) => mat[(j + dj) * W + i + di] === m).length;
      const put = (kind: PatchKind, facing: FacePatch["facing"], score: number, sl?: number) => {
        const key = `${m}|${kind}|${facing}`, cur = best.get(key);
        if (!cur || score > cur.columns) best.set(key, { material: m, kind, facing, at: [ext.x0 + (i + 0.5) * step, h, ext.z0 + (j + 0.5) * step], columns: score, samples: 0, ...(sl !== undefined ? { slope: Math.round((Math.atan(sl) * 180) / Math.PI) } : {}) });
      };
      if (kinds.has("steep") && slope >= tan && slope < 4) {
        // the side the ground falls toward (downhill is against the gradient)
        const facing = Math.abs(gx) > Math.abs(gz) ? (gx > 0 ? "w" : "e") : gz > 0 ? "n" : "s";
        put("steep", facing, same * 10 - Math.abs(slope - 1.2 * tan), slope);
      } else if (kinds.has("shade") && slope < 0.35) {
        for (const [facing, di, dj] of DIRS) {
          let rise = 0;
          for (let r = 1; r <= reach; r++) { const t = at(i + di * r, j + dj * r); if (!Number.isNaN(t)) rise = Math.max(rise, t - h); }
          if (rise >= wall) put("shade", facing, same * 10 + Math.min(rise, 12));
        }
      }
    }
  return [...best.values()].map((p) => ({ ...p, at: p.at.map((v) => Math.round(v * 100) / 100) as [number, number, number], samples: counts.get(p.material) ?? 0 }))
    .sort((a, b) => a.material - b.material || a.kind.localeCompare(b.kind) || a.facing.localeCompare(b.facing));
}

/** A colour's brightness, 0-255 (Rec. 709 on the sRGB bytes: enough to rank shots of one material). */
export const luma = (c: Rgb) => 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
