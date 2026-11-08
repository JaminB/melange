// Tool group "inspect": read-only looks at the open level. heightmap_ascii, render_topdown and render_side show the ground
// (text and PNG), query_ground answers one column, list_materials lists the material and object palettes. None of them
// edits the project.
import type { EditorStore, ServerPaletteEntry as PaletteEntry, Vec3 } from "../core";
import { extentXZ, markers, sampleField, spansAt, waterMap, type Field, type MarkerKind, type Region } from "../lib/inspect-field";
import { Raster, SKY, VOID, WATER, hex, materialTint, shade, type Rgb } from "../lib/inspect-png";
import { findHoles } from "../lib/holes";
import { UTILITY_REACH, WORM_WIDTH, classify, nodeAt, ownTurn, pathStats, pathTo, search, standableAt, standing, surfaces, turnAt, type FloodRules, type ReachOptions, type Surfaces } from "../lib/reach";
import { rotation, slotOf } from "../lib/turns";
import { waterByTurn, type WaterSchedule } from "../lib/flood";
import { scriptWater, type ScriptWater } from "../lib/script-water";
import { pieces } from "../lib/islets";
import { loadMaterials, projectMaterials, resolveFile, themeFile, tintFor, type MaterialSet, type PaletteMode } from "../lib/materials";
import { WORLD_PER_MAP, grids, isSolid, material, tidy, type Grid } from "../lib/voxel";
import { ExprError, compile } from "../lib/heightfield-expr";
import { ToolError, image, type Content, type Schema, type Tool } from "../registry";
import type { Session } from "../session";

// ------------------------------------------------------------------ arguments

const units = { type: "string", enum: ["map", "world"], description: "Unit of the coordinates you give and of the lengths in the answer: map (a voxel; the default) or world (map x 20, as the Erg panel shows)." } as const;
const num = (d: string) => ({ type: "number", description: d }) as const;
const regionProps = {
  x0: num("Region west edge (default: the level's extent)."), z0: num("Region north edge."),
  x1: num("Region east edge."), z1: num("Region south edge."), units,
} as const;

type Units = "map" | "world" | undefined;
interface RegionArgs { x0?: number; z0?: number; x1?: number; z1?: number; units?: "map" | "world"; }

/** Map units from the caller's. */
const inMap = (v: number, u: Units) => (u === "world" ? v / WORLD_PER_MAP : v);
/** The caller's units from map units. */
const outUnit = (v: number, u: Units) => (u === "world" ? v * WORLD_PER_MAP : v);
const r2 = (v: number) => Math.round(v * 100) / 100;

/** The region asked for; an edge left out is the level's extent on that side. */
function region(gs: Grid[], a: RegionArgs): Region {
  const ext = extentXZ(gs);
  if (!ext) throw new ToolError("the level has no voxel frames");
  const r: Region = {
    x0: a.x0 === undefined ? ext.x0 : inMap(a.x0, a.units), z0: a.z0 === undefined ? ext.z0 : inMap(a.z0, a.units),
    x1: a.x1 === undefined ? ext.x1 : inMap(a.x1, a.units), z1: a.z1 === undefined ? ext.z1 : inMap(a.z1, a.units),
  };
  if (r.x0 > r.x1) [r.x0, r.x1] = [r.x1, r.x0];
  if (r.z0 > r.z1) [r.z0, r.z1] = [r.z1, r.z0];
  if (r.x1 - r.x0 < 1e-6 || r.z1 - r.z0 < 1e-6) throw new ToolError("the region is empty");
  return r;
}

/** Material record names of the open project's material file, or null when the server cannot say. */
async function materialNames(s: Session): Promise<string[] | null> {
  return (await projectMaterials(s))?.names ?? null;
}

const paletteSchema = { type: "string", enum: ["real", "index"], description: "Material colours: real (default; each material's texture colour, from the server) or index (a distinct hue per material index, to tell neighbouring materials apart)." } as const;

/** How the renders tint materials, and what to say about it. */
async function renderTint(s: Session, mode: PaletteMode | undefined) {
  const set = await projectMaterials(s);
  const tint = tintFor(set, mode);
  const real = mode !== "index" && !!set?.colors;
  return { set, tint, note: real ? "real: each material's texture colour (the server's average of its surface texture)" : mode === "index" ? "index: a distinct hue per material index" : "index: a distinct hue per material index (the server gave no material colours)" };
}

const regionInfo = (r: Region, u: Units) => ({
  x: [r2(outUnit(r.x0, u)), r2(outUnit(r.x1, u))], z: [r2(outUnit(r.z0, u)), r2(outUnit(r.z1, u))], units: u ?? "map",
});

const MARK_CHAR: Record<MarkerKind, string> = { spawn: "S", crate: "C", telepad: "T", trigger: "R", minefactory: "F", object: "O" };
const MARK_RGB: Record<MarkerKind, Rgb> = {
  spawn: [60, 220, 60], crate: [250, 220, 40], telepad: [40, 220, 230], trigger: [230, 60, 220], minefactory: [250, 120, 20], object: [250, 250, 250],
};

/** Indented JSON with each array or object of plain values on one line (positions, ranges and list entries stay readable). */
const compact = (v: unknown) => JSON.stringify(v, null, 1).replace(/[[{][^[\]{}]*[\]}]/g, (m) => m.replace(/\s*\n\s*/g, " "));

const frameName = (store: EditorStore, id: number) => store.scene.frames.find((f) => f.id === id)?.name;

// ------------------------------------------------------------------ heightmap_ascii

const BANDS = "0123456789abcdefghijklmnopqrstuvwxyz";

export function heightmapAscii(s: Session, a: RegionArgs & { step?: number; markers?: boolean }): string {
  const store = s.store, gs = grids(store);
  const r = region(gs, a);
  const MAXC = 200;
  const step = a.step === undefined ? Math.max(1, Math.ceil(Math.max(r.x1 - r.x0, r.z1 - r.z0) / 120)) : inMap(a.step, a.units);
  const cols = Math.ceil((r.x1 - r.x0) / step), rows = Math.ceil((r.z1 - r.z0) / step);
  if (cols > MAXC || rows > MAXC) throw new ToolError(`${cols}x${rows} cells is too many (at most ${MAXC} each way): use a smaller region or a larger step`);
  const f = sampleField(gs, r, step);
  const water = waterMap(store);
  // bands of equal height, in whole or half voxels so the legend reads simply
  const band = f.solid ? Math.max(1, Math.ceil((f.max - f.min) / (BANDS.length - 1) / 0.5) * 0.5) : 1;
  const bandOf = (t: number) => Math.min(BANDS.length - 1, Math.floor((t - f.min) / band));
  const cells: string[][] = [];
  for (let j = 0; j < f.h; j++) {
    const row: string[] = [];
    for (let i = 0; i < f.w; i++) {
      const t = f.top[j * f.w + i];
      row.push(t === -Infinity ? "." : BANDS[bandOf(t)]);
    }
    cells.push(row);
  }
  const drawn: { ch: string; kind: MarkerKind; name: string; pos: number[] }[] = [];
  for (const m of a.markers === false ? [] : markers(store)) {
    const i = Math.floor((m.pos[0] - f.x0) / step), j = Math.floor((m.pos[2] - f.z0) / step);
    if (i < 0 || j < 0 || i >= f.w || j >= f.h) continue;
    cells[j][i] = MARK_CHAR[m.kind];
    drawn.push({ ch: MARK_CHAR[m.kind], kind: m.kind, name: m.name, pos: tidy(m.pos, 2) });
  }
  const labels = cells.map((_, j) => String(r2(outUnit(f.z0 + (j + 0.5) * step, a.units))));
  const pad = Math.max(...labels.map((l) => l.length));
  const tick = 10;
  let head = " ".repeat(pad + 1), ruler = " ".repeat(pad + 1);
  for (let i = 0; i < f.w; i += tick) {
    head += String(r2(outUnit(f.x0 + (i + 0.5) * step, a.units))).padEnd(tick).slice(0, tick);
    ruler += "|" + " ".repeat(tick - 1);
  }
  const lines = [head.trimEnd(), ruler.slice(0, pad + 1 + f.w).trimEnd(), ...cells.map((row, j) => `${labels[j].padStart(pad)} ${row.join("")}`)];
  const used = new Set<number>();
  for (const t of f.top) if (t > -Infinity) used.add(bandOf(t));
  const u = a.units ?? "map";
  const legend = [...used].sort((x, y) => x - y).map((b) => {
    const lo = f.min + b * band, hi = lo + band;
    return `${BANDS[b]}  top y ${r2(outUnit(lo, a.units))} to ${r2(outUnit(hi, a.units))}${water !== null && hi <= water + 1e-9 ? " (under water)" : ""}`;
  });
  const top = [
    `Top-down map of ${f.solid} of ${f.w * f.h} cells with ground. x grows right, z grows down; one character is ${r2(outUnit(step, a.units))} ${u} units.`,
    `Region x ${r2(outUnit(r.x0, a.units))}..${r2(outUnit(r.x1, a.units))}, z ${r2(outUnit(r.z0, a.units))}..${r2(outUnit(r.z1, a.units))} (${u} units). Axis numbers are the centre of the cell.`,
    water === null ? "Water: the level's own height (not known here)." : `Water at y ${r2(outUnit(water, a.units))} ${u} units.`,
  ];
  if (!f.solid) return [...top, "", "No ground in this region."].join("\n");
  const key = [
    ".  no ground in the column", ...legend,
    ...[...new Set(drawn.map((d) => `${d.ch}  ${d.kind === "object" ? "other placed object" : d.kind}`))],
  ];
  const where = drawn.map((d) => `${d.ch} ${d.name} at x ${r2(outUnit(d.pos[0], a.units))}, y ${r2(outUnit(d.pos[1], a.units))}, z ${r2(outUnit(d.pos[2], a.units))}`);
  return [...top, "", ...lines, "", "Key (height bands are equal steps of the top y):", ...key,
    ...(where.length ? ["", "Markers:", ...where.slice(0, 60), ...(where.length > 60 ? [`... ${where.length - 60} more`] : [])] : [])].join("\n");
}

// ------------------------------------------------------------------ render_topdown

const MARGIN = 14;

/** A height-shaded, material-tinted top view of the field, with the pixel of a map x and z. */
export function drawTopdown(f: Field, scale: number, opts: { water: number | null; shading: "both" | "height" | "material"; tint?: (m: number) => Rgb }) {
  const tint = opts.tint ?? materialTint;
  const img = new Raster(f.w * scale + MARGIN, f.h * scale + MARGIN, [12, 12, 14]);
  const span = f.solid ? Math.max(1e-6, f.max - f.min) : 1;
  const topAt = (i: number, j: number) => (i < 0 || j < 0 || i >= f.w || j >= f.h ? -Infinity : f.top[j * f.w + i]);
  for (let j = 0; j < f.h; j++)
    for (let i = 0; i < f.w; i++) {
      const t = f.top[j * f.w + i];
      let c: Rgb;
      if (t === -Infinity) c = opts.water !== null ? (WATER.map((v) => Math.round(v * 0.55)) as Rgb) : VOID;
      else {
        const base = opts.shading === "height" ? ([150, 150, 150] as Rgb) : tint(f.mat[j * f.w + i]);
        let light = 1;
        if (opts.shading !== "material") {
          // a ramp with height, and relief from the slope toward the south-east
          const e = topAt(i + 1, j), w = topAt(i - 1, j), n = topAt(i, j - 1), so = topAt(i, j + 1);
          const dx = (e > -Infinity ? e : t) - (w > -Infinity ? w : t), dz = (so > -Infinity ? so : t) - (n > -Infinity ? n : t);
          light = Math.max(0.55, Math.min(1.35, (0.55 + 0.45 * ((t - f.min) / span)) * (1 - (0.12 * (dx + dz)) / (2 * f.step))));
        }
        c = shade(base, light);
        if (opts.water !== null && t <= opts.water) c = c.map((v, k) => Math.round(v * 0.45 + WATER[k] * 0.55)) as Rgb;
      }
      img.rect(MARGIN + i * scale, MARGIN + j * scale, scale, scale, c);
    }
  return { img, px: (x: number) => MARGIN + Math.round(((x - f.x0) / f.step) * scale), pz: (z: number) => MARGIN + Math.round(((z - f.z0) / f.step) * scale) };
}

type TopdownArgs = RegionArgs & { step?: number; scale?: number; shading?: "both" | "height" | "material"; palette?: PaletteMode };

/** render_topdown's picture of region r without its markers: the ground, the water and the axis ticks. */
async function topdownView(s: Session, gs: Grid[], r: Region, a: TopdownArgs) {
  const store = s.store;
  const longest = Math.max(r.x1 - r.x0, r.z1 - r.z0);
  const step = a.step === undefined ? (longest > 400 ? Math.ceil(longest / 400) : 1) : inMap(a.step, a.units);
  const cols = Math.ceil((r.x1 - r.x0) / step), rows = Math.ceil((r.z1 - r.z0) / step);
  if (cols > 1500 || rows > 1500) throw new ToolError(`${cols}x${rows} cells is too many (at most 1500 each way): use a smaller region or a larger step`);
  const scale = a.scale ?? Math.max(1, Math.min(8, Math.floor(900 / Math.max(cols, rows))));
  if ((cols * scale + MARGIN) * (rows * scale + MARGIN) > 4_500_000) throw new ToolError("the image would be too large: lower scale or use a smaller region");
  const f = sampleField(gs, r, step);
  const water = waterMap(store);
  const shading = a.shading ?? "both";
  const pal = await renderTint(s, a.palette);
  const { img, px, pz } = drawTopdown(f, scale, { water, shading, tint: pal.tint });
  // axis ticks at a whole number of units, at least 36 pixels apart
  const tickStep = [1, 2, 5, 10, 20, 50, 100, 200, 500].find((t) => (t / step) * scale >= 36) ?? 1000;
  const LAB: Rgb = [230, 230, 230];
  for (let x = Math.ceil(r.x0 / tickStep) * tickStep; x < r.x1; x += tickStep) {
    img.rect(px(x), MARGIN - 3, 1, 3, LAB);
    img.text(px(x) + 2, 1, String(r2(outUnit(x, a.units))), LAB);
  }
  for (let z = Math.ceil(r.z0 / tickStep) * tickStep; z < r.z1; z += tickStep) {
    img.rect(MARGIN - 3, pz(z), 3, 1, LAB);
    img.text(1, pz(z) + 2, String(r2(outUnit(z, a.units))).slice(-3), LAB);
  }
  return { f, step, scale, water, shading, pal, img, px, pz };
}

export async function renderTopdown(s: Session, a: TopdownArgs & { markers?: boolean }): Promise<{ content: Content[] }> {
  const store = s.store, gs = grids(store);
  const r = region(gs, a);
  const { f, step, scale, water, shading, pal, img, px, pz } = await topdownView(s, gs, r, a);
  const placed: object[] = [];
  const mr = Math.max(2, Math.min(5, Math.round(scale / 2) + 1));
  for (const m of a.markers === false ? [] : markers(store)) {
    if (m.pos[0] < r.x0 || m.pos[0] >= r.x1 || m.pos[2] < r.z0 || m.pos[2] >= r.z1) continue;
    img.marker(px(m.pos[0]), pz(m.pos[2]), mr, MARK_RGB[m.kind]);
    placed.push({ kind: m.kind, name: m.name, at: m.pos.map((v) => r2(outUnit(v, a.units))) });
  }
  const usedMat = new Map<number, number>();
  for (let k = 0; k < f.mat.length; k++) if (f.mat[k] >= 0) usedMat.set(f.mat[k], (usedMat.get(f.mat[k]) ?? 0) + 1);
  const names = pal.set?.names ?? null;
  const u = a.units ?? "map";
  const info = {
    image: { width: img.w, height: img.h, pixelsPerCell: scale, cellSize: `${r2(outUnit(step, a.units))} ${u} units`, margin: MARGIN },
    palette: pal.note,
    region: regionInfo(r, a.units),
    reading: `pixel (px, py) is x ${r2(outUnit(f.x0, a.units))} + (px - ${MARGIN}) / ${scale} * ${r2(outUnit(step, a.units))}, and z likewise with py; x grows right, z grows down. Ticks are labelled in ${u} units.`,
    ground: f.solid ? { cells: f.solid, of: f.w * f.h, topY: { min: r2(outUnit(f.min, a.units)), max: r2(outUnit(f.max, a.units)) } } : "none in this region",
    shading: shading === "height" ? "grey relief: brighter = higher" : shading === "material" ? "flat material tints" : "material tint, brighter = higher, with relief",
    water: water === null ? "the level's own height (not known here, so not drawn); columns with no ground are dark grey" : { y: r2(outUnit(water, a.units)), drawn: "ground at or under it is tinted blue; columns with no ground are dark blue" },
    materials: [...usedMat].sort((x, y) => y[1] - x[1]).map(([m, n]) => ({ index: m, name: names?.[m] ?? null, colour: hex(pal.tint(m)), cells: n })),
    markers: { legend: Object.fromEntries(Object.entries(MARK_RGB).map(([k, c]) => [k, hex(c)])), placed },
  };
  return { content: [image(img.png()), { type: "text", text: compact(info) }] };
}

// ------------------------------------------------------------------ render_side

export async function renderSide(s: Session, a: { x0: number; z0: number; x1: number; z1: number; units?: "map" | "world"; y0?: number; y1?: number; scale?: number; palette?: PaletteMode }) {
  const store = s.store, gs = grids(store);
  const pal = await renderTint(s, a.palette);
  const ax = inMap(a.x0, a.units), az = inMap(a.z0, a.units), bx = inMap(a.x1, a.units), bz = inMap(a.z1, a.units);
  const len = Math.hypot(bx - ax, bz - az);
  if (len < 1e-6) throw new ToolError("the line has no length");
  if (len > 2000) throw new ToolError("the line is longer than 2000 map units");
  const n = Math.max(2, Math.ceil(len * 2));    // two samples per voxel along the line
  const cols: ReturnType<typeof spansAt>[] = [];
  let lo = Infinity, hi = -Infinity;
  for (let k = 0; k < n; k++) {
    const t = (k + 0.5) / n;
    const sp = spansAt(gs, ax + (bx - ax) * t, az + (bz - az) * t);
    cols.push(sp);
    for (const q of sp) { lo = Math.min(lo, q.bottom); hi = Math.max(hi, q.top); }
  }
  const water = waterMap(store);
  if (a.y0 !== undefined) lo = inMap(a.y0, a.units);
  else if (lo === Infinity) lo = 0;
  if (a.y1 !== undefined) hi = inMap(a.y1, a.units);
  else if (hi === -Infinity) hi = lo + 10;
  if (hi <= lo) hi = lo + 1;
  const yScale = a.scale ?? Math.max(1, Math.min(12, Math.floor(700 / (hi - lo))));
  const xPer = Math.max(1, Math.min(yScale, Math.floor(1400 / n)));   // pixels per sample column
  const left = 28;
  const img = new Raster(left + n * xPer, Math.ceil((hi - lo) * yScale) + MARGIN, SKY);
  const yPix = (y: number) => MARGIN + Math.round((hi - y) * yScale);
  if (water !== null && water > lo) img.rect(left, yPix(Math.min(water, hi)), n * xPer, yPix(lo) - yPix(Math.min(water, hi)), WATER);
  const profile: (number | null)[][] = [];
  const usedMat = new Set<number>();
  const every = Math.max(1, Math.round(n / 80));
  for (let k = 0; k < n; k++) {
    // Spans of stacked or overlapping frames (a block on the block under it) are one stretch of ground: only the top
    // of a stretch, where air is above it, gets the light edge, so block boundaries leave no seam.
    const lit: { y: number; c: Rgb }[] = [];
    let reach = Infinity;
    for (const q of cols[k]) {
      const top = Math.min(q.top, hi), bot = Math.max(q.bottom, lo);
      if (top <= bot) continue;
      const c = pal.tint(q.material);
      usedMat.add(q.material);
      img.rect(left + k * xPer, yPix(top), xPer, Math.max(1, yPix(bot) - yPix(top)), c);
      // spans come highest top first: one whose top is under everything drawn so far starts a new stretch
      if (top < reach - 1e-6) lit.push({ y: top, c });
      reach = Math.min(reach, bot);

    }
    for (const e of lit) img.rect(left + k * xPer, yPix(e.y), xPer, 1, shade(e.c, 1.3));

    if (k % every === 0) profile.push([r2(outUnit(((k + 0.5) / n) * len, a.units)), cols[k].length ? r2(outUnit(cols[k][0].top, a.units)) : null]);
  }
  // markers within 1.5 voxels of the line
  const dx = (bx - ax) / len, dz = (bz - az) / len, near: object[] = [];
  for (const m of markers(store)) {
    const along = (m.pos[0] - ax) * dx + (m.pos[2] - az) * dz, off = Math.abs((m.pos[0] - ax) * dz - (m.pos[2] - az) * dx);
    if (along < 0 || along > len || off > 1.5 || m.pos[1] < lo || m.pos[1] > hi) continue;
    img.marker(left + Math.round((along / len) * n * xPer), yPix(m.pos[1]), 3, MARK_RGB[m.kind]);
    near.push({ kind: m.kind, name: m.name, along: r2(outUnit(along, a.units)), y: r2(outUnit(m.pos[1], a.units)) });
  }
  const LAB: Rgb = [20, 20, 20];
  const tickY = [1, 2, 5, 10, 20, 50, 100, 200].find((t) => t * yScale >= 24) ?? 500;
  for (let y = Math.ceil(lo / tickY) * tickY; y <= hi; y += tickY) {
    img.rect(left - 3, yPix(y), 3, 1, LAB);
    img.text(1, yPix(y) - 2, String(r2(outUnit(y, a.units))).slice(-6), LAB);
  }
  const tickX = [1, 2, 5, 10, 20, 50, 100, 200, 500].find((t) => t * 2 * xPer >= 36) ?? 1000;
  for (let d = 0; d < len; d += tickX) {
    const p = left + Math.round((d / len) * n * xPer);
    img.rect(p, MARGIN - 3, 1, 3, LAB);
    img.text(p + 2, 1, String(r2(outUnit(d, a.units))), LAB);
  }
  const names = pal.set?.names ?? null;
  const u = a.units ?? "map";
  const info = {
    image: { width: img.w, height: img.h, pixelsPerVoxelY: yScale, pixelsPerSample: xPer, leftMargin: left, topMargin: MARGIN },
    palette: pal.note,
    line: { from: [r2(outUnit(ax, a.units)), r2(outUnit(az, a.units))], to: [r2(outUnit(bx, a.units)), r2(outUnit(bz, a.units))], length: r2(outUnit(len, a.units)), units: u },
    reading: `left to right is the distance along the line from the first point (top ruler); the left ruler is y (${u} units). Light blue is sky, blue is water${water === null ? " (the level's own height is not known here, so none is drawn)" : ` (y ${r2(outUnit(water, a.units))})`}.`,
    y: [r2(outUnit(lo, a.units)), r2(outUnit(hi, a.units))],
    topProfile: { note: "[distance along the line, highest ground y] (null: none), about 80 samples", points: profile },
    materials: [...usedMat].map((m) => ({ index: m, name: names?.[m] ?? null, colour: hex(pal.tint(m)) })),
    markersNearLine: near,
  };
  return { content: [image(img.png()), { type: "text", text: compact(info) }] };
}

// ------------------------------------------------------------------ query_ground

/** The air of a column between its merged solid stretches (spans highest first): each opening's floor (the top of the
 * stretch under it), its ceiling (the bottom of the one over it; null: open sky) and height, map units, highest first. */
export function openings(sp: { top: number; bottom: number }[]): { floor: number; ceiling: number | null; height: number }[] {
  const merged: { top: number; bottom: number }[] = [];
  for (const q of [...sp].sort((a, b) => b.top - a.top)) {
    const last = merged[merged.length - 1];
    if (last && q.top >= last.bottom - 1e-6) last.bottom = Math.min(last.bottom, q.bottom);
    else merged.push({ top: q.top, bottom: q.bottom });
  }
  return merged.map((m, i) => {
    const ceiling = i === 0 ? null : merged[i - 1].bottom;
    return { floor: m.top, ceiling, height: ceiling === null ? Infinity : ceiling - m.top };
  });
}

export async function queryGround(s: Session, a: { x: number; z: number; units?: "map" | "world"; below?: number; maxSpans?: number; y?: number; headroom?: number; mode?: "top" | "atOrBelow" }) {
  const store = s.store;
  const px = inMap(a.x, a.units), pz = inMap(a.z, a.units);
  const all = spansAt(grids(store), px, pz);
  if (a.mode === "atOrBelow" && a.y === undefined) throw new ToolError("mode atOrBelow answers the floor under a height: give y");
  if (a.mode === "atOrBelow" && a.below !== undefined) throw new ToolError("mode atOrBelow and below both pick the ground under a height: give one");
  let sp = a.below === undefined ? all : spansAt(grids(store), px, pz, inMap(a.below, a.units));
  const head = a.headroom === undefined ? 1.5 : inMap(a.headroom, a.units);
  const gaps = openings(all);
  const gapOut = (g: (typeof gaps)[number]) => ({ floor: { map: r2(g.floor), world: r2(g.floor * WORLD_PER_MAP) }, ceiling: g.ceiling === null ? null : { map: r2(g.ceiling), world: r2(g.ceiling * WORLD_PER_MAP) },
    height: g.ceiling === null ? null : { map: r2(g.height), world: r2(g.height * WORLD_PER_MAP) }, passable: g.height >= head });
  // at a height: the opening whose floor is the ground under y (within half a map unit over it, a step), and whether a worm fits
  let atY: object | undefined;
  if (a.y !== undefined) {
    const y = inMap(a.y, a.units), g = gaps.find((q) => q.floor <= y + 0.5 && (q.ceiling === null || q.ceiling > y));
    atY = g ? { y: { map: r2(y), world: r2(y * WORLD_PER_MAP) }, ...gapOut(g), note: g.height >= head ? "a worm fits here: an opening at least headroom high over this floor" : "too low for a worm: the opening is under headroom" }
      : { y: { map: r2(y), world: r2(y * WORLD_PER_MAP) }, passable: false, note: "solid at this height (inside a wall or the ground)" };
  }
  // atOrBelow: the stretches from the one whose top is the floor of the opening at y down, so ground and standOn are that floor
  let modeNote: string | undefined;
  if (a.mode === "atOrBelow") {
    const y = inMap(a.y!, a.units), g = gaps.find((q) => q.floor <= y + 0.5 && (q.ceiling === null || q.ceiling > y));
    sp = g ? all.filter((q) => q.top <= g.floor + 1e-6) : [];
    modeNote = g ? `atOrBelow: ground and standOn are the floor under y (${g.ceiling === null ? "open sky above" : `air up to ${r2(outUnit(g.ceiling, a.units))}`}), not the column's highest top` : "atOrBelow: y is inside solid ground (a wall or the ground), so there is no floor there";
  }
  const extra = {
    ...(modeNote ? { mode: modeNote } : {}),
    openings: { listed: gaps.slice(0, a.maxSpans ?? 6).map(gapOut), headroom: { map: r2(head), world: r2(head * WORLD_PER_MAP) },
      note: "highest first: the air over each merged solid stretch up to the stretch above it (ceiling null: open sky); passable when at least headroom high, so a gate under a lintel shows as a passable opening below the lintel's top" },
    ...(atY ? { passableAt: atY } : {}),
  };
  const water = waterMap(store);
  const names = await materialNames(s);
  const out = (y: number) => ({ map: r2(y), world: r2(y * WORLD_PER_MAP) });
  const span = (q: (typeof sp)[number]) => ({ top: out(q.top), bottom: out(q.bottom), frame: q.frame, frameName: frameName(store, q.frame) ?? null, material: q.material, materialName: names?.[q.material] ?? null });
  const at = { map: [r2(px), r2(pz)], world: [r2(px * WORLD_PER_MAP), r2(pz * WORLD_PER_MAP)] };
  if (!sp.length) return { at, frame: null, frameName: null, material: null, ground: null, note: a.mode === "atOrBelow" ? "no floor at or under y in this column" : a.below === undefined ? "no voxel frame has ground in this column" : "no ground under that height in this column", ...extra };
  const top = span(sp[0]);
  return {
    at, ground: top, ...extra,
    // the top stretch's frame and material at the top level too, where the other tools name a frame (frame, frameName)
    frame: top.frame, frameName: top.frameName, material: top.material, materialName: top.materialName,
    standOn: out(sp[0].top),    // the y a thing placed here rests on
    water: water === null ? "the level's own height (not known here)" : { map: r2(water), world: r2(water * WORLD_PER_MAP), groundIsUnderWater: sp[0].top <= water },
    spans: { count: sp.length, listed: sp.slice(0, a.maxSpans ?? 6).map(span), note: "highest first: every stretch of solid voxels in the column over all frames (below asks for the ground under a height)" },
  };
}

// ------------------------------------------------------------------ check_reachability

interface FloodArgs {
  schedule?: WaterSchedule | "script"; levels?: { turn: number; level: number }[]; rounds?: { round: number; level: number }[]; start?: number; every?: number; firstTurn?: number; maxTurns?: number;
  teams?: number; wormsPerTeam?: number; margin?: number; minMoves?: number; turnOrder?: number[]; minOwnTurns?: number; minDryMargin?: number;
  capMargin?: number;
}
/** The verdict's default margin over the water, map units: a worm is knocked about and the computer team's worms walk,
 * so ground just over the water is not dry (in the Caldera iteration-8 Test a worm drowned from ground 1.7 map units up). */
const DRY_MARGIN = 1.5;
/** capReport's default: spawns whose ground is within this of the water's highest level (map units) are listed. */
const CAP_MARGIN = 3;
interface TargetArg { x: number; z: number; radius?: number; name?: string; }
interface ReachArgs extends RegionArgs {
  from?: number[][]; target?: { x: number; z: number; radius?: number } | "nearest"; targets?: TargetArg[]; targetMode?: "nearest" | "each"; step?: number; maxStep?: number; maxSlope?: number; maxDrop?: number;
  maxGap?: number; headroom?: number; water?: boolean; render?: boolean; palette?: PaletteMode; slopeOver?: "tread" | "riser" | "segment" | "skin";
  walkSpeed?: number; turnTime?: number; flood?: FloodArgs; islets?: boolean;
  via?: ViaArgs; materialSlopes?: { materials: number[]; maxSlope: number }[];
  bodyWidth?: number; bodyHeight?: number; route?: boolean;
}

/** A route constraint: the ground "on the route" is where the top voxel's material is one of `materials` (when given)
 * and `mask` is above 0 (when given); `only` walks on it alone, `avoid` never on it; within `near` of a start or the
 * target the ground is always walkable (a spawn in a courtyard, the summit). */
interface ViaArgs { materials?: number[]; mask?: string; mode?: "only" | "avoid"; near?: number; }

/** The per-node route constraint and material slope caps, or nothing when neither is asked for. Map units in. */
function routeRules(S: Surfaces, a: ReachArgs, u: Units, starts: Vec3[], target: { x: number; z: number; r: number }, more: { x: number; z: number; r: number }[] = []):
  { allow?: Uint8Array; slopeCap?: Float32Array; info?: object } {
  const out: { allow?: Uint8Array; slopeCap?: Float32Array; info?: Record<string, unknown> } = {};
  if (a.materialSlopes?.length) {
    const cap = new Float32Array(S.nodes).fill(90), byMat = new Map<number, number>();
    a.materialSlopes.forEach((m, i) => {
      if (!m.materials.length) throw new ToolError(`materialSlopes[${i}]: give at least one material`);
      for (const k of m.materials) byMat.set(k, Math.min(byMat.get(k) ?? 90, m.maxSlope));
    });
    for (let n = 0; n < S.nodes; n++) cap[n] = byMat.get(S.material[n]) ?? 90;
    out.slopeCap = cap;
    out.info = { materialSlopes: Object.fromEntries([...byMat].map(([k, v]) => [k, v])),
      materialSlopesNote: "any climb onto ground whose top voxel is one of these materials steeper than its maxSlope is refused, small steps too (a scree or ash flank a worm slides down)" };
  }
  const v = a.via;
  if (v) {
    if (!v.materials?.length && !v.mask) throw new ToolError("via: give materials, a mask, or both");
    let fn: ((x: Record<string, number>) => number) | null = null;
    if (v.mask) {
      try {
        fn = compile(v.mask, ["h", "material"]) as unknown as (x: Record<string, number>) => number;
      } catch (e) {
        if (e instanceof ExprError) throw new ToolError(`via.mask: ${e.message}`);
        throw e;
      }
    }
    const mats = v.materials?.length ? new Set(v.materials) : null, mode = v.mode ?? "only", near = v.near === undefined ? 3 : inMap(v.near, u);
    const vars: Record<string, number> = { x: 0, z: 0, u: 0, v: 0, r: 0, theta: 0, W: 0, D: 0, R: 0, pi: Math.PI, e: Math.E, h: 0, material: 0 };
    const allow = new Uint8Array(S.nodes);
    let on = 0;
    for (let n = 0; n < S.nodes; n++) {
      const p = nodeAt(S, n);
      let inRoute = !mats || mats.has(S.material[n]);
      if (inRoute && fn) {
        vars.x = outUnit(p[0], u); vars.z = outUnit(p[2], u); vars.h = outUnit(p[1], u); vars.material = S.material[n];
        vars.r = Math.hypot(vars.x, vars.z); vars.theta = Math.atan2(vars.z, vars.x);
        inRoute = fn(vars) > 0;
      }
      if (inRoute) on++;
      const free = starts.some((q) => Math.hypot(q[0] - p[0], q[2] - p[2]) <= near) || [target, ...more].some((t) => Math.hypot(t.x - p[0], t.z - p[2]) <= t.r + near);
      allow[n] = free || (mode === "only" ? inRoute : !inRoute) ? 1 : 0;
    }
    out.allow = allow;
    out.info = { ...(out.info ?? {}), via: { mode, ...(v.materials ? { materials: v.materials } : {}), ...(v.mask ? { mask: v.mask } : {}), near: r2(outUnit(near, u)), routeSurfaces: on, of: S.nodes,
      note: mode === "only" ? "the walk may step only onto ground on the route (and anywhere within near of a start or the target): reachable proves a route along it"
        : "the walk never steps onto ground on the route (except within near of a start or the target): not reachable proves the route is the only way" } };
  }
  return out;
}

/** A worm's walking speed on level ground, map units a second (measured in a Test on 2026-11-08: worms walked 55-58
 * world units a second over level stretches, 2.75-2.9 map), and the turn time the turn counts assume (the Quick Game's
 * 45 s): see WALK_NOTE. */
export const WALK_SPEED = 2.75, TURN_TIME = 45;
const WALK_NOTE = "turns = the walk over walkSpeed x turnTime per own turn, a worm walking the whole turn without stopping (the least turns; " +
  "jumps, aiming and the retreat time are not counted)";

const ROUTE_RGB: Rgb[] = [[255, 70, 70], [70, 150, 255], [255, 210, 40], [200, 90, 255], [60, 230, 120], [255, 140, 30], [40, 230, 230], [255, 110, 200]];

/** One start's routes for the picture: the walk (to the target, or to the closest point reached), the route with the
 * utility it needs, where that is used, and where the walk is stopped. Map units. */
interface Route { name: string; start: Vec3; walk: number[]; utility: number[]; usedAt: Vec3 | null; blocked: Vec3[]; }

async function drawRoutes(s: Session, gs: Grid[], r: Region, S: Surfaces, routes: Route[], target: Vec3, u: Units, palette?: PaletteMode) {
  const v = await topdownView(s, gs, r, { units: u, palette });
  const { img, px, pz } = v, w = Math.max(1, Math.min(3, Math.round(v.scale / 3)));
  const poly = (path: number[], c: Rgb, dash: number) => {
    for (let k = 1; k < path.length; k++) {
      const a = nodeAt(S, path[k - 1]), b = nodeAt(S, path[k]);
      img.line(px(a[0]), pz(a[2]), px(b[0]), pz(b[2]), c, w, dash);
    }
  };
  const ring = (x: number, z: number, rad: number, c: Rgb) => {
    for (let k = 0; k < 48; k++) {
      const t0 = (k / 48) * Math.PI * 2, t1 = ((k + 1) / 48) * Math.PI * 2;
      img.line(px(x) + Math.round(rad * Math.cos(t0)), pz(z) + Math.round(rad * Math.sin(t0)), px(x) + Math.round(rad * Math.cos(t1)), pz(z) + Math.round(rad * Math.sin(t1)), c, 2);
    }
  };
  const legend: object[] = [];
  routes.forEach((rt, i) => {
    const c = ROUTE_RGB[i % ROUTE_RGB.length];
    poly(rt.utility, c, 4);
    poly(rt.walk, c, 0);
    for (const b of rt.blocked) img.cross(px(b[0]), pz(b[2]), 4, c);
    if (rt.usedAt) ring(rt.usedAt[0], rt.usedAt[2], 5, c);
    img.marker(px(rt.start[0]), pz(rt.start[2]), 3, c);
    legend.push({ name: rt.name, colour: hex(c) });
  });
  ring(target[0], target[2], 8, [255, 255, 255]);
  return {
    image: img.png(),
    info: { width: img.w, height: img.h, pixelsPerCell: v.scale, cellSize: `${r2(outUnit(v.step, u))} ${u ?? "map"} units`, margin: MARGIN,
      reading: `pixel (px, py) is x ${r2(outUnit(v.f.x0, u))} + (px - ${MARGIN}) / ${v.scale} * ${r2(outUnit(v.step, u))}, and z likewise with py; x grows right, z grows down`,
      drawn: "per start (its colour): a square at the start, a solid line for the walk (to the target, or to the closest point reached), a dashed line for the route with the utility it needs, a ring where that utility is first used, an X where the walk is blocked; the white ring is the target",
      palette: v.pal.note, routes: legend },
  };
}

/** The flood a check runs with: the water by game turn and when a worm walks. */
async function floodRules(s: Session, f: FloodArgs, u: Units, perTurn: number): Promise<{ rules: FloodRules; info: object; horizon: number; top: number }> {
  const store = s.store, own = waterMap(store);
  const start = f.start !== undefined ? inMap(f.start, u) : own;
  if (start === null) throw new ToolError("flood: the project keeps the level's own water height, which is not known here: pass flood.start (the water before the schedule)");
  if ([f.schedule, f.levels, f.rounds].filter((x) => x !== undefined).length !== 1) throw new ToolError("flood: give one of schedule (as wum.sim.waterSchedule's options, or \"script\" to read the project's script), levels ([{turn, level}]) or rounds ([{round, level}])");
  // rounds: wum.sim.round()'s rounds (sim.turnStarted's round), which go up once every team has had a turn: round r starts
  // at game turn (r - 1) x teams + 1 while every team is alive
  const teamsN = f.teams ?? 2;
  const byRound = f.rounds?.map((e) => ({ turn: (e.round - 1) * teamsN + 1, level: e.level }));
  // levels and start are the call's units (map by default), unlike a schedule's (world by default): a value over every
  // ground of the level by far is world units given as map, which drowns every spawn at turn 1 (Caldera, iteration 6)
  if (u !== "world") {
    let ground = -Infinity;
    for (const g of grids(store)) ground = Math.max(ground, g.bounds.max[1]);
    const over = [...(f.start !== undefined ? [f.start] : []), ...(f.levels ?? byRound ?? []).map((e) => e.level)].filter((v) => Number.isFinite(ground) && v > ground + 40);
    if (over.length)
      throw new ToolError(`flood: ${over.length > 1 ? "levels" : "a level"} of ${over.slice(0, 3).join(", ")} map units is far over the level's highest ground (${r2(ground)} map): levels and start are in the call's units, map unless units: "world" (a schedule's are world unless its units say map); pass units: "world" or divide by 20`);
  }
  let schedule: WaterSchedule | undefined, from = "the call", sim: ScriptWater | null = null;
  if (f.schedule === "script") {
    sim = await scriptWater(s, start, f.maxTurns ?? 200, teamsN, true).catch((e: unknown) => {
      if (e instanceof ToolError) throw e;
      throw new ToolError(`flood: could not read or run the project's script: ${e instanceof Error ? e.message : String(e)}`);
    });
    if (!sim) throw new ToolError("flood: the project's script sets no water (no wum.sim.waterSchedule, no Water.Level write): pass schedule or levels");
    from = sim.from;
    schedule = sim.schedule;
  } else if (f.schedule) {
    if (typeof f.schedule !== "object" || Array.isArray(f.schedule)) throw new ToolError("flood.schedule: an object of wum.sim.waterSchedule's options, or \"script\"");
    for (const [k, v] of Object.entries(f.schedule)) {
      if (!["startTurn", "every", "step", "cap", "from", "units"].includes(k)) throw new ToolError(`flood.schedule.${k}: the options are startTurn, every, step, cap, from and units`);
      if (k === "units" ? v !== "map" && v !== "world" : typeof v !== "number" || !Number.isFinite(v)) throw new ToolError(`flood.schedule.${k}: ${k === "units" ? "map or world" : "a number"}`);
    }
    if (f.schedule.step === undefined || f.schedule.cap === undefined) throw new ToolError("flood.schedule: step and cap are required (as for wum.sim.waterSchedule)");
    schedule = f.schedule;
  }
  const levels = (f.levels ?? byRound)?.map((e) => ({ turn: e.turn, level: inMap(e.level, u) }));
  const waterAt = sim ? sim.waterAt : waterByTurn(start, { schedule, levels });
  const scriptInfo = sim ? { ...(sim.schedules ? { scriptSchedules: sim.schedules } : {}), ...(sim.errors ? { scriptErrors: sim.errors } : {}), ...(sim.warnings.length ? { scriptWarnings: sim.warnings } : {}) } : {};
  const every = f.every ?? 2, firstTurn = f.firstTurn ?? 1, horizon = f.maxTurns ?? 200;
  const rules: FloodRules = { waterAt, perTurn, firstTurn, every };
  const sample = [1, 2, 3, 5, 10, 20, 50].filter((t) => t <= horizon).map((t) => ({ turn: t, water: r2(outUnit(waterAt(t), u)), map: r2(waterAt(t)), world: r2(waterAt(t) * WORLD_PER_MAP) }));
  let top = start;
  for (let t = 1; t <= horizon; t++) top = Math.max(top, waterAt(t));
  return { rules, horizon, top, info: {
    from, start: r2(outUnit(start, u)), ...(schedule ? { schedule, scheduleUnits: `${schedule.units === "map" ? "map" : "world"} (wum.sim.waterSchedule's own); the answer's other lengths are ${u ?? "map"} units, and water levels are given in both` }
      : sim ? {} : { levels: f.levels ?? byRound, levelsUnits: `${u ?? "map"} (the call's units)`,
        ...(byRound ? { rounds: f.rounds, roundTurns: `round r from game turn (r - 1) x ${teamsN} + 1 (teams ${teamsN}: wum.sim.round goes up once every team has had a turn; a team wiped out makes the rounds shorter)` } : {}) }), ...scriptInfo,
    every, firstTurn, maxTurns: horizon, perOwnTurn: r2(outUnit(perTurn, u)), waterByTurn: sample, highestWater: { map: r2(top), world: r2(top * WORLD_PER_MAP) },
    turnIndex: "game turns count from 1: game turn n is the match's n-th turn start, wum.sim.turnStarted's turn n, game_state's turn n and a waterSchedule's startTurn n (its step lands at that turn's start)",
    note: `floodsAtTurn and floods.turn are game turns (each team's turn counts one). escapes walks each start's worm perOwnTurn in each of its turns, the first in game turn ${firstTurn}, then every ${every} turns (its team picking it every time: the best case). ` +
      "moves (spawn knots) counts its own moves instead, in the engine's rotation (the teams alternate from team 0, each moves its living worms in turn, a drowned worm is skipped so its team's others move sooner; or turnOrder as given): ownTurns gives the water at each of its first own turns, and escapesOnOwnMoves walks it on those. " +
      "A place counts only while it is dry in the turn the worm gets there",
  } };
}

/** A spawn knot's worm in the rotation (lib/turns.ts): its team and index, and the game turns it moves on. */
interface WormMoves { team: number; worm: number; turns: number[]; every: number; teams: number; }

/** One start under the flood: when its ground goes under, and whether a walk gets it to the target dry in time. */
function floodFor(S: Surfaces, n0: number, goal: Uint8Array, o: ReachOptions, f: { rules: FloodRules; horizon: number }, out: (p: Vec3) => number[], u: Units,
  mv: WormMoves | null, need: { ownTurns: number; margin: number }) {
  let floods: number | null = null;
  for (let t = 1; t <= f.horizon; t++) if (S.y[n0] <= f.rules.waterAt(t)) { floods = t; break; }
  const both = (v: number) => ({ map: r2(v), world: r2(v * WORLD_PER_MAP) });
  const extra = {
    ground: both(S.y[n0]),
    ...(floods !== null ? { floods: { turn: floods, water: both(f.rules.waterAt(floods)), ground: both(S.y[n0]) } } : {}),
    ...(mv ? { moves: ownMoves(S, n0, goal, o, f.rules, mv, floods, need) } : {}),
  };
  return { ...floodWalk(S, n0, goal, o, f, out, u, floods), ...extra };
}

/** The worm's own moves before its ground floods, the water at the start of each of its first own turns (at least 2,
 * `need.ownTurns`) and whether its ground stays `need.margin` over it there, and whether a walk on its own moves alone
 * gets it to the target dry. */
function ownMoves(S: Surfaces, n0: number, goal: Uint8Array, o: ReachOptions, f: FloodRules, mv: WormMoves, floods: number | null, need: { ownTurns: number; margin: number }) {
  const rules: FloodRules = { ...f, firstTurn: mv.turns[0] ?? 1, every: mv.every, turns: mv.turns };
  const own = floods === null ? null : mv.turns.filter((t) => t < floods).length;
  const teamMoves = floods === null ? null : Math.max(0, Math.ceil((floods - (1 + mv.team)) / mv.teams));
  const both = (v: number) => ({ map: r2(v), world: r2(v * WORLD_PER_MAP) });
  const rows = mv.turns.length ? Array.from({ length: Math.max(2, need.ownTurns) }, (_, k) => {
    const t = ownTurn(rules, k), w = f.waterAt(t), clear = S.y[n0] - w;
    return { own: k + 1, gameTurn: t, water: both(w), clearance: both(clear), ok: clear > 0 && clear >= need.margin - 1e-9 };
  }) : [];
  let escapes = false, arrives: number | undefined;
  if (mv.turns.length && S.y[n0] > rules.waterAt(mv.turns[0])) {
    const q = search(S, n0, { ...o, flood: rules });
    let hit = -1;
    for (let n = 0; n < S.nodes; n++) if (q.reached[n] && goal[n] && (hit < 0 || q.dist[n] < q.dist[hit])) hit = n;
    if (hit >= 0) { escapes = true; arrives = turnAt(rules, q.dist[hit]); }
  }
  return { team: mv.team, worm: mv.worm, firstMove: mv.turns[0] ?? null, secondMove: mv.turns[1] ?? null, every: mv.every, turns: mv.turns.slice(0, 6),
    ...(own !== null ? { ownMovesBeforeFlood: own, teamMovesBeforeFlood: teamMoves } : {}),
    ownTurns: rows.length ? rows : "it gets no turn in the turns checked (maxTurns)",
    escapesOnOwnMoves: escapes, ...(arrives !== undefined ? { arrivesTurn: arrives } : {}) };
}

function floodWalk(S: Surfaces, n0: number, goal: Uint8Array, o: ReachOptions, f: { rules: FloodRules; horizon: number }, out: (p: Vec3) => number[], u: Units, floods: number | null) {
  const startDry = S.y[n0] > f.rules.waterAt(f.rules.firstTurn);
  if (!startDry) return { floodsAtTurn: floods, escapes: false, why: `its ground is under the water by its first turn (${f.rules.firstTurn})` };
  const q = search(S, n0, { ...o, flood: f.rules });
  let hit = -1;
  for (let n = 0; n < S.nodes; n++) if (q.reached[n] && goal[n] && (hit < 0 || q.dist[n] < q.dist[hit])) hit = n;
  // its own turns that start before the water reaches its ground
  const ownLeft = floods === null ? null : Math.max(0, Math.ceil((floods - f.rules.firstTurn) / f.rules.every));
  if (hit >= 0) {
    const own = Math.max(1, Math.ceil(q.dist[hit] / f.rules.perTurn - 1e-9));
    return { floodsAtTurn: floods, escapes: true, ownTurns: own, arrivesTurn: turnAt(f.rules, q.dist[hit]), ...(ownLeft !== null ? { ownTurnsBeforeItFloods: ownLeft } : {}),
      walk: r2(outUnit(q.dist[hit], u)) };
  }
  // the dry ground reached that is highest: where the worm can wait longest
  let best = n0;
  for (let n = 0; n < S.nodes; n++) if (q.reached[n] && S.y[n] > S.y[best]) best = n;
  let safeUntil: number | null = null;
  for (let t = 1; t <= f.horizon; t++) if (S.y[best] <= f.rules.waterAt(t)) { safeUntil = t; break; }
  return { floodsAtTurn: floods, escapes: false, ...(ownLeft !== null ? { ownTurnsBeforeItFloods: ownLeft } : {}),
    why: "no walk reaches the target before the water covers the way (or the walk alone does not get there)",
    highestDry: { at: out(nodeAt(S, best)), floodsAtTurn: safeUntil } };
}

/** The pieces of terrain that are not the main landmass: debris under `small` map units cubed, offshore chunks, specks. */
function isletSummary(gs: Grid[], r: Region, step: number, water: number | null, u: Units, small = 50, limit = 20) {
  const ps = pieces(gs, r, step);
  if (!ps.length) return { pieces: 0 };
  const main = ps[0], rest = ps.slice(1);
  const box = (p: { min: number[]; max: number[] }) => ({ min: p.min.map((v) => r2(outUnit(v, u))), max: p.max.map((v) => r2(outUnit(v, u))) });
  return {
    pieces: ps.length, main: { volume: r2(main.volume), columns: main.columns, ...box(main) },
    detached: rest.length, small: rest.filter((p) => p.volume < small).length, smallBelow: small,
    list: [...rest].sort((a, b) => a.volume - b.volume).slice(0, limit).map((p) => ({
      volume: r2(p.volume), columns: p.columns, ...box(p), top: r2(outUnit(p.top, u)), ...(water !== null && p.top <= water ? { underWater: true } : {}),
      ...(p.volume < small ? { debris: true } : {}), frames: p.frames.slice(0, 4) })),
    note: "volume is map units cubed (voxels at scale 1); a piece is solid ground joined side by side (not by a corner only) to nothing of the main landmass; the smallest are listed first: carve them (fill_box_raw state empty, or clear_base_terrain with a box) or join them",
  };
}

interface OwnTurnRow { own: number; gameTurn: number; water: { map: number; world: number }; clearance: { map: number; world: number }; ok: boolean; }
interface FloodSpawn { name: string; why?: string; flood?: { floodsAtTurn: number | null; ground?: { map: number; world: number };
  moves?: { ownMovesBeforeFlood?: number; firstMove: number | null; ownTurns?: OwnTurnRow[] | string } } }

/** The flood's hard warnings: the water's top reaching a spawn's ground (within `margin`), and a spawn that floods before
 * its worm's 2nd move (it cannot leave in time), or before its `minMoves`-th (default 3). */
function floodChecks(spawns: FloodSpawn[], top: number, f: FloodArgs, u: Units): string[] {
  const out: string[] = [];
  const margin = f.margin === undefined ? 1 : inMap(f.margin, u), minMoves = f.minMoves ?? 3;
  const both = (v: number) => `${r2(v)} map (${r2(v * WORLD_PER_MAP)} world)`;
  for (const sp of spawns) {
    const g = sp.flood?.ground?.map;
    if (g === undefined) continue;
    if (top >= g - margin) out.push(`${sp.name}: the water's top ${both(top)} reaches ${top >= g ? "over" : "within " + both(margin) + " of"} the ground it stands on (${both(g)}): it drowns there; keep the cap under ${both(g - margin)} or move the spawn up`);
    const mv = sp.flood?.moves, own = mv?.ownMovesBeforeFlood;
    if (own === undefined) continue;
    if (own < 2) out.push(`${sp.name}: its ground floods in game turn ${sp.flood!.floodsAtTurn}, before its worm's 2nd move (its first is game turn ${mv!.firstMove}): ${own} move${own === 1 ? "" : "s"} to leave`);
    else if (own < minMoves) out.push(`${sp.name}: its ground floods in game turn ${sp.flood!.floodsAtTurn}, before its worm's move ${minMoves}: only ${own} moves to leave`);
  }
  return out;
}

/** The spawns against the water's highest level: each spawn's ground over it (map and world), nearest first, and the
 * ones within capMargin (default CAP_MARGIN map units) of it, which knock-back or a walk puts in the water. */
function capReport(spawns: FloodSpawn[], top: number, f: FloodArgs, u: Units) {
  const m = f.capMargin === undefined ? CAP_MARGIN : inMap(f.capMargin, u);
  const rows = spawns.filter((sp) => sp.flood?.ground?.map !== undefined).map((sp) => {
    const over = sp.flood!.ground!.map - top;
    return { name: sp.name, overCap: { map: r2(over), world: r2(over * WORLD_PER_MAP) }, within: over <= m + 1e-9 };
  }).sort((a, b) => a.overCap.map - b.overCap.map);
  const within = rows.filter((r) => r.within).map((r) => r.name);
  return { cap: { map: r2(top), world: r2(top * WORLD_PER_MAP) }, capMargin: r2(outUnit(m, u)), within, spawns: rows,
    note: "each spawn's ground over the water's highest level (the cap); within: at most capMargin over it, where knock-back, a slide or the computer team's walking puts a worm in the water before the verdict's turns are over" };
}

/** The flood's verdict: every spawn knot's ground must stay more than minDryMargin over the water (over it, with 0) at
 * the start of each of its worm's first minOwnTurns own turns (default 2: in its 2nd it can still move away), in the
 * rotation modelled. A spawn that fails is named with its own turn, the game turn and the water. */
function floodVerdict(spawns: FloodSpawn[], f: FloodArgs, u: Units, played: (name: string) => boolean) {
  const need = f.minOwnTurns ?? 2, margin = f.minDryMargin === undefined ? DRY_MARGIN : inMap(f.minDryMargin, u);
  const failures: string[] = [];
  let checked = 0;
  for (const sp of spawns) {
    if (!sp.flood) {
      // a start with no ground (or under the water) fails, unless it is a knot no worm of the match stands on
      if (sp.why && played(sp.name)) { checked++; failures.push(`${sp.name}: ${sp.why}`); }
      continue;
    }
    const rows = sp.flood.moves?.ownTurns;
    if (rows === undefined) continue;
    checked++;
    if (typeof rows === "string") { failures.push(`${sp.name}: ${rows}`); continue; }
    const bad = rows.slice(0, need).find((r) => !r.ok);
    if (bad) failures.push(`${sp.name}: ${bad.clearance.map <= 0 ? `${r2(outUnit(-bad.clearance.map, u))} under the water` : `only ${r2(outUnit(bad.clearance.map, u))} over the water (minDryMargin ${r2(outUnit(margin, u))})`} at its own turn ${bad.own} (game turn ${bad.gameTurn}: water ${bad.water.map} map, ${bad.water.world} world)`);
  }
  return { pass: failures.length === 0, checked, minOwnTurns: need, minDryMargin: r2(outUnit(margin, u)), ...(failures.length ? { failures } : {}),
    rule: `each spawn knot's ground stays more than minDryMargin over the water at the start of each of its worm's first ${need} own turns, in the engine's rotation (or turnOrder); a spawn with no ground under it fails` };
}

export async function checkReachability(s: Session, a: ReachArgs) {
  const store = s.store, gs = grids(store), u = a.units;
  const L = (v: number | undefined, d: number) => (v === undefined ? d : inMap(v, u));
  const r = region(gs, a);
  const step = L(a.step, 1);
  const cols = Math.ceil((r.x1 - r.x0) / step) * Math.ceil((r.z1 - r.z0) / step);
  if (cols > 1_000_000) throw new ToolError(`${cols} columns is too many (at most 1000000): use a larger step or a smaller region`);
  const water = a.water === false ? null : waterMap(store);
  const o: ReachOptions = { step, maxStep: L(a.maxStep, 0.5), maxSlope: a.maxSlope ?? 45, maxDrop: L(a.maxDrop, 8), maxGap: L(a.maxGap, 0), headroom: L(a.headroom, 1.5), water,
    slopeOver: a.slopeOver ?? "tread", ...(a.bodyWidth ? { bodyWidth: L(a.bodyWidth, 0) } : {}), ...(a.bodyHeight ? { bodyHeight: L(a.bodyHeight, 1.5) } : {}) };
  const perTurn = L(a.walkSpeed, WALK_SPEED) * (a.turnTime ?? TURN_TIME);
  const flood = a.flood ? await floodRules(s, a.flood, u, perTurn) : null;
  const S = surfaces(gs, r, step, o.headroom);
  if (!S.nodes) throw new ToolError("no ground in the region");
  const out = (p: Vec3) => p.map((v) => r2(outUnit(v, u)));
  // the target: the highest ground (in the circle, when one is given) and every surface within 1.5 of that height there
  // several targets (targets, or target "nearest" with them): the goal is any of them, each start answering the one its
  // walk reaches first; the first is the target of the picture and of the flood's summary
  if (a.target !== undefined && a.target !== "nearest" && (typeof a.target !== "object" || !Number.isFinite(a.target.x) || !Number.isFinite(a.target.z))) throw new ToolError("target: {x, z, radius?} or \"nearest\"");
  if (a.target === "nearest" && !a.targets?.length) throw new ToolError("target \"nearest\" picks the nearest of `targets`: give targets");
  const single = a.target && a.target !== "nearest" ? a.target : undefined;
  if (single && a.targets?.length) throw new ToolError("give target or targets, not both (target \"nearest\" goes with targets)");
  const list: TargetArg[] | null = a.targets?.length ? a.targets : null;
  const inCircleR = (n: number, x: number, z: number, rad: number) => { const p = nodeAt(S, n); return Math.hypot(p[0] - x, p[2] - z) <= rad; };
  const goalOf = (t: { x: number; z: number; r: number } | null, what: string) => {
    let x = 0, z = 0, hi = -Infinity;
    if (t) {
      x = t.x; z = t.z;
      for (let n = 0; n < S.nodes; n++) if (S.y[n] > hi && inCircleR(n, x, z, t.r)) hi = S.y[n];
      if (hi === -Infinity) throw new ToolError(`no ground within ${what}'s radius`);
    } else {
      let best = 0;
      for (let n = 1; n < S.nodes; n++) if (S.y[n] > S.y[best]) best = n;
      [x, hi, z] = nodeAt(S, best);
    }
    const g = new Uint8Array(S.nodes);
    let count = 0;
    for (let n = 0; n < S.nodes; n++) if (S.y[n] >= hi - 1.5 && inCircleR(n, x, z, t ? t.r : 3) && (water === null || S.y[n] > water)) { g[n] = 1; count++; }
    if (!count) throw new ToolError(`${what} is under the water`);
    return { x, z, top: hi, r: t ? t.r : 3, g, count };
  };
  const tgs = list ? list.map((t, i) => ({ name: t.name ?? `targets[${i}]`, ...goalOf({ x: inMap(t.x, u), z: inMap(t.z, u), r: L(t.radius, 3) }, t.name ?? `targets[${i}]`) }))
    : [{ name: "target", ...goalOf(single ? { x: inMap(single.x, u), z: inMap(single.z, u), r: L(single.radius, 3) } : null, "the target") }];
  const tr = tgs[0].r, tx = tgs[0].x, tz = tgs[0].z, top = tgs[0].top;
  const goal = new Uint8Array(S.nodes), goalOwner = new Int16Array(S.nodes).fill(-1);
  let goals = 0;
  tgs.forEach((t, i) => { for (let n = 0; n < S.nodes; n++) if (t.g[n] && !goal[n]) { goal[n] = 1; goalOwner[n] = i; goals++; } });
  // the starts: the given points, else the spawn knots
  const starts: { name: string; at: Vec3 }[] = a.from
    ? a.from.map((p, i) => {
      if (p.length !== 3 || !p.every(Number.isFinite)) throw new ToolError(`from[${i}]: [x, y, z]`);
      return { name: `from[${i}]`, at: [inMap(p[0], u), inMap(p[1], u), inMap(p[2], u)] as Vec3 };
    })
    : markers(store).filter((m) => m.kind === "spawn").map((m) => ({ name: m.name, at: m.pos }));
  if (!starts.length) throw new ToolError("the level has no spawn knots: pass from");
  const rr = routeRules(S, a, u, starts.map((st) => st.at), { x: tx, z: tz, r: tr }, tgs.slice(1));
  const teamsN = a.flood?.teams ?? 2, perTeam = a.flood?.wormsPerTeam ?? Math.max(1, Math.ceil(starts.length / teamsN));
  if (rr.allow) o.allow = rr.allow;
  if (rr.slopeCap) o.slopeCap = rr.slopeCap;
  const statsOut = (path: number[]) => {
    const st = pathStats(S, path, o);
    return { length: r2(outUnit(st.length, u)), maxSlope: Math.round(st.maxSlope * 10) / 10, maxStairSlope: Math.round(st.maxStairSlope! * 10) / 10,
      maxStepUp: r2(outUnit(st.maxStepUp, u)), maxDrop: r2(outUnit(st.maxDrop, u)), jumps: st.jumps,
      // the narrowest gap walked through (spanAcross: the free width across the move at body height), null when the
      // route is open all the way (gaps of OPEN_SPAN or more)
      narrowest: st.narrowest ? { width: r2(outUnit(st.narrowest.width, u)), at: out(nodeAt(S, st.narrowest.node)) } : null };
  };
  // the whole route, column by column (game_walk's `path`): at most 2000 points
  const routeOut = (path: number[]) => (a.route ? { route: path.slice(0, 2000).map((n) => out(nodeAt(S, n))) } : {});
  // the least own turns a walk of `d` (horizontal, map units) takes, and the game turn it ends in from the first own turn
  const turnsFor = (d: number) => {
    const own = Math.max(1, Math.ceil(d / perTurn - 1e-9));
    return { ownTurns: own, ...(flood ? { arrivesTurn: turnAt(flood.rules, d) } : {}) };
  };
  const thin = (path: number[]) => { const every = Math.max(1, Math.ceil(path.length / 12)); return path.filter((_, i) => i % every === 0 || i === path.length - 1).map((n) => out(nodeAt(S, n))); };
  const routes: Route[] = [];
  // the worm rotation (lib/turns.ts) for the spawn knots: a worm whose ground is under the water at a turn start is dead
  // from then on, so its team's others move sooner
  const stand = starts.map((st) => standing(S, st.at, o, gs));
  const knotOf = (name: string) => { const m = /^WORM(\d)$/.exec(name); return m ? Number(m[1]) : null; };
  let rot: ReturnType<typeof rotation> | null = null;
  if (flood && !a.from) {
    const order = a.flood?.turnOrder;
    if (order?.some((w) => !Number.isInteger(w) || w < 0 || w > 7)) throw new ToolError("turnOrder: worm numbers 0-7 (WORMn), in the order the game moved them from game turn 1");
    const ground = new Map<number, number>();
    starts.forEach((st, i) => { const w = knotOf(st.name); if (w !== null && stand[i] >= 0) ground.set(w, S.y[stand[i]]); });
    const drownsAt = (w: number) => {
      const g = ground.get(w);
      if (g === undefined) return 1;
      for (let t = 1; t <= flood.horizon; t++) if (g <= flood.rules.waterAt(t)) return t;
      return null;
    };
    // the match has teams x wormsPerTeam worms: knots past them are not played
    rot = rotation({ worms: starts.map((st) => knotOf(st.name)).filter((w): w is number => w !== null && w < teamsN * perTeam).sort((x, y) => x - y), teams: teamsN, perTeam,
      horizon: flood.horizon, order, drownsAt });
  }
  const movesOf = (name: string): WormMoves | null => {
    const w = knotOf(name);
    // a knot whose worm is not in the match (past teams x wormsPerTeam, or not in turnOrder) has no moves
    if (w === null || !rot || !rot.own.has(w) || (a.flood?.turnOrder?.length && !a.flood.turnOrder.includes(w))) return null;
    const sl = slotOf(w, perTeam, teamsN);
    return { team: sl.team, worm: sl.index, turns: rot.own.get(w) ?? [], every: a.flood?.turnOrder?.length ?? teamsN * perTeam, teams: teamsN };
  };
  const need = { ownTurns: a.flood?.minOwnTurns ?? 2, margin: a.flood?.minDryMargin === undefined ? DRY_MARGIN : inMap(a.flood.minDryMargin, u) };
  const spawns = starts.map((st, si) => {
    const n0 = stand[si];
    if (n0 < 0) {
      if (water !== null && standing(S, st.at, { ...o, water: null }, gs) >= 0) return { name: st.name, at: out(st.at), reachable: false, why: "it stands at or under the water" };
      const sa = standableAt(gs, st.at, 1.5, o.headroom);
      return { name: st.name, at: out(st.at), reachable: false, why: sa.ok ? "its ground is not in the walk graph (outside the region, or a ledge narrower than a step)" : `${sa.why} (within 1.5 map units of its y ${r2(outUnit(st.at[1], u))}, as build_castle's spawnSpots check)` };
    }
    const q = search(S, n0, o);
    const floodOut = flood ? floodFor(S, n0, goal, o, flood, out, u, movesOf(st.name), need) : undefined;
    let hit = -1, near = -1, nearD = Infinity, high = n0;
    for (let n = 0; n < S.nodes; n++) {
      if (!q.reached[n]) continue;
      if (goal[n] && (hit < 0 || q.dist[n] < q.dist[hit])) hit = n;
      const p = nodeAt(S, n), d = Math.hypot(p[0] - tx, p[2] - tz);
      if (d < nearD) { nearD = d; near = n; }
      if (S.y[n] > S.y[high]) high = n;
    }
    let reached = 0;
    for (let n = 0; n < S.nodes; n++) reached += q.reached[n];
    const base = { name: st.name, at: out(st.at), standsOn: r2(outUnit(S.y[n0], u)), reachable: hit >= 0, reachedColumns: reached, highestReached: out(nodeAt(S, high)) };
    // with several targets: the one reached first, and each target's own walk (mode each)
    const which = list && hit >= 0 ? { reachedTarget: tgs[goalOwner[hit]].name } : {};
    const each = list && a.targetMode === "each" ? { perTarget: tgs.map((t) => {
      let h = -1;
      for (let n = 0; n < S.nodes; n++) if (q.reached[n] && t.g[n] && (h < 0 || q.dist[n] < q.dist[h])) h = n;
      return { target: t.name, reachable: h >= 0, ...(h >= 0 ? { walk: r2(outUnit(q.dist[h], u)), turns: turnsFor(q.dist[h]) } : {}) };
    }) } : {};
    if (hit >= 0) {
      const path = pathTo(q, hit);
      routes.push({ name: st.name, start: st.at, walk: path, utility: [], usedAt: null, blocked: [] });
      return { ...base, ...which, walk: r2(outUnit(q.dist[hit], u)), turns: turnsFor(q.dist[hit]), waypoints: thin(path), path: statsOut(path), ...routeOut(path), ...each, ...(floodOut ? { flood: floodOut } : {}) };
    }
    // where it is stopped: the reached ground nearest the target with a climb too high next to it
    const stops: { n: number; d: number }[] = [];
    for (let n = 0; n < S.nodes; n++) if (q.reached[n] && q.blocked[n] < Infinity) { const p = nodeAt(S, n); stops.push({ n, d: Math.hypot(p[0] - tx, p[2] - tz) }); }
    stops.sort((x, y) => x.d - y.d);
    const blockedAt: object[] = [], blockedMap: Vec3[] = [];
    for (const st2 of stops) {
      const p = nodeAt(S, st2.n);
      if (blockedAt.some((b) => Math.hypot((b as { x: number }).x - p[0], (b as { z: number }).z - p[2]) < 4 * step)) continue;
      blockedAt.push({ x: p[0], z: p[2], at: out(p), climbNeeded: r2(outUnit(q.blocked[st2.n], u)) });
      blockedMap.push(p);
      if (blockedAt.length >= 3) break;
    }
    // the cheapest utility that gets there: the walk widened to what each adds (a heuristic, rules.utilities)
    const c = classify(S, n0, goal, o);
    let needs: object;
    if (c.utility) {
      const [fa, fb] = c.at ?? [n0, n0], pa = nodeAt(S, fa), pb = nodeAt(S, fb);
      needs = { utility: c.utility, usedAt: { from: out(pa), to: out(pb), rise: r2(outUnit(pb[1] - pa[1], u)), across: r2(outUnit(Math.hypot(pb[0] - pa[0], pb[2] - pa[2]), u)) },
        path: statsOut(c.path), waypoints: thin(c.path), ...routeOut(c.path) };
    } else needs = { utility: null, note: "not within a jetpack's reach either (rules.utilities)" };
    routes.push({ name: st.name, start: st.at, walk: pathTo(q, near), utility: c.path, usedAt: c.at ? nodeAt(S, c.at[0]) : null, blocked: blockedMap });
    return { ...base, closest: { at: out(nodeAt(S, near)), distance: r2(outUnit(nearD, u)) },
      blockedAt: blockedAt.map((b) => { const { x: _x, z: _z, ...rest } = b as { x: number; z: number }; return rest; }), needs, ...each, ...(floodOut ? { flood: floodOut } : {}) };
  });
  const floodWarnings = flood ? floodChecks(spawns as FloodSpawn[], flood.top, a.flood!, u) : [];
  const cap = flood ? capReport(spawns as FloodSpawn[], flood.top, a.flood!, u) : null;
  const ok = spawns.filter((x) => x.reachable).length;
  const escaped = flood ? spawns.filter((x) => (x as { flood?: { escapes: boolean } }).flood?.escapes).length : 0;
  const onOwn = flood && rot ? spawns.filter((x) => (x as { flood?: { moves?: { escapesOnOwnMoves?: boolean } } }).flood?.moves?.escapesOnOwnMoves).length : null;
  const played = (name: string) => { const w = knotOf(name); return w === null || (a.flood?.turnOrder?.length ? a.flood.turnOrder.includes(w) : w < teamsN * perTeam); };
  const verdict = flood ? floodVerdict(spawns as FloodSpawn[], a.flood!, u, played) : null;
  // the spawns that do not get there, by name, with why (no ground under it, under the water, or blocked)
  const notOk = spawns.filter((x) => !x.reachable).map((x) => `${x.name}${(x as { why?: string }).why ? ` (${(x as { why?: string }).why})` : ""}`);
  // routes through a gap narrower than a worm (WORM_WIDTH, an assumption) when the body is not enforced
  const tight = o.bodyWidth ? [] : spawns.flatMap((x) => {
    const nw = (x as { path?: { narrowest?: { width: number } | null } }).path?.narrowest;
    return nw && inMap(nw.width, u) < WORM_WIDTH - 1e-6 ? [`${x.name} ${nw.width}`] : [];
  });
  const answer = {
    summary: `${ok} of ${spawns.length} reach the target on foot` + (notOk.length ? ` (not: ${notOk.slice(0, 8).join(", ")})` : "") +
      (flood ? `; ${escaped} of ${spawns.length} reach it dry with the flood (best case: a turn every ${flood.rules.every})` + (onOwn !== null ? `, ${onOwn} on their own moves in the rotation` : "") : "") +
      (verdict ? `; flood ${verdict.pass ? "PASS" : "FAIL"}${verdict.failures ? `: ${verdict.failures.join("; ")}` : ` (${verdict.checked} spawns dry through their own turn ${verdict.minOwnTurns})`}` : "") +
      (cap && cap.within.length ? `; within ${cap.capMargin} ${u ?? "map"} of the water's cap: ${cap.within.join(", ")}` : "") +
      (tight.length ? `; through gaps narrower than a worm (${r2(outUnit(WORM_WIDTH, u))}): ${tight.join(", ")} (bodyWidth enforces it)` : ""),
    ...(verdict ? { floodVerdict: verdict } : {}),
    target: { at: out([tx, top, tz]), radius: r2(outUnit(tr, u)), surfaces: goals, note: single || list ? "the highest ground within the radius" : "the highest ground of the region" },
    ...(list ? { targets: tgs.map((t) => ({ name: t.name, at: out([t.x, t.top, t.z]), radius: r2(outUnit(t.r, u)), surfaces: t.count })),
      targetMode: a.targetMode ?? "nearest", targetsNote: "the goal is any of the targets: each start answers reachedTarget, the one its walk gets to first (target is the first, for the picture and the flood); targetMode each adds perTarget, every target's own walk" } : {}),
    rules: { step: r2(outUnit(step, u)), maxStep: r2(outUnit(o.maxStep, u)), maxSlope: o.maxSlope, maxDrop: r2(outUnit(o.maxDrop, u)), maxGap: r2(outUnit(o.maxGap, u)),
      headroom: r2(outUnit(o.headroom, u)), water: water === null ? "not used (the level's own height is unknown, or water: false)" : r2(outUnit(water, u)), units: u ?? "map",
      slopeOver: o.slopeOver, walkSpeed: r2(outUnit(perTurn / (a.turnTime ?? TURN_TIME), u)), turnTime: a.turnTime ?? TURN_TIME,
      body: { width: o.bodyWidth ? r2(outUnit(o.bodyWidth, u)) : null, height: r2(outUnit(o.bodyHeight ?? o.headroom, u)),
        note: `every route's narrowest gap is the free width across its moves at the worm's body height (between walls over its ground + maxStep and under + the height); with width set, a gap narrower is not walked; without, routes through gaps under ${r2(outUnit(WORM_WIDTH, u))} (a worm's width, an assumption) are named in the summary` },
      note: "walking only (no rope, jetpack or girders): a climb is taken when it is at most maxStep, or when it is a riser of at most one voxel of the ground it climbs onto and no steeper than maxSlope " +
        (o.slopeOver === "riser" ? "over one voxel of that ground" : o.slopeOver === "segment" ? "over the move itself (the rise over its horizontal length: every climb at its own slope, a skin's cliff face a barrier)"
          : o.slopeOver === "skin" ? "over the move itself where either end is a draped skin's (a skin's 62-78 degree cliff face is a barrier, not a stair), else over the tread above it"
          : "over the tread above it (the flat run to the next riser, up to 8 voxels: a staircase is judged at its average slope, the game draws voxel steps as a slope)") +
        "; drops up to maxDrop; jumps over gaps up to maxGap (0: none); ground at or under the water is not used",
      turns: WALK_NOTE, ...(rr.info ?? {}) },
    ...(flood ? { flood: { ...flood.info, teams: teamsN, wormsPerTeam: perTeam,
      ...(rot ? { turnOrder: { source: a.flood?.turnOrder?.length ? "turnOrder (as given, repeated)" : "the engine's rotation: the teams alternate from team 0, each moves its living worms in turn (WORM0 game turn 1, WORM4 turn 2, WORM1 turn 3 for two teams of four); a worm drowned at a turn start is skipped",
        firstTurns: rot.byTurn.slice(0, 16).map((w, i) => ({ turn: i + 1, worm: w < 0 ? null : `WORM${w}` })) } } : {}),
      ...(cap ? { capReport: cap } : {}),
      ...(floodWarnings.length ? { warnings: floodWarnings } : {}) } } : {}),
    graph: { columns: S.w * S.h, surfaces: S.nodes },
    spawns,
  };
  const U = UTILITY_REACH, lu = (v: number) => r2(outUnit(v, u));
  Object.assign(answer.rules, { utilities: {
    note: "a heuristic: for a start that does not walk there, the search is run again with the rules widened to each utility in turn, cheapest first; needs names the first that gets there, usedAt its first move that walking refuses",
    jump: `gaps up to ${lu(U.jump)} to ground no higher than maxStep`, girder: `gaps up to ${lu(U.girder)}, steps and far sides up to ${lu(U.girder / 2)} higher`,
    rope: `a climb up to ${lu(U.rope)} up the wall beside the worm (the wall is the anchor), and girder gaps`, jetpack: `up to ${lu(U.jetpackUp)} higher and ${lu(U.jetpackAcross)} across, any drop`,
    path: "length along the ground, the steepest walked segment (degrees), the highest step up and the deepest drop on the route, and its jumps" } });
  if (a.islets) Object.assign(answer, { islets: isletSummary(gs, r, step, water, u) });
  if (!a.render) return compact(answer);
  const pic = await drawRoutes(s, gs, r, S, routes, [tx, top, tz], u, a.palette);
  return { content: [image(pic.image), { type: "text", text: compact({ ...answer, image: pic.info }) }] };
}

// ------------------------------------------------------------------ list_materials

/** materialFile "theme" (any case): the theme's own file, Theme<Name>\Theme<Name>.txt, of `theme`, else of `base`'s theme,
 * else of the open project's theme; without any of them an error that names the path to give. Other values are kept. */
export async function themeKeyword(s: Session, a: { materialFile?: string; theme?: string; base?: string }): Promise<{ materialFile?: string; theme?: string; note?: string }> {
  let theme = a.theme;
  if (a.base) {
    const b = (await (await s.levels()).list()).bases.find((x) => x.key.toLowerCase() === a.base!.toLowerCase());
    if (!b) throw new ToolError(`base: no base level '${a.base}' (list_bases lists them)`);
    theme = theme ?? b.theme;
  }
  if (a.materialFile === undefined || a.materialFile.trim().toLowerCase() !== "theme") return { materialFile: a.materialFile, theme };
  const t = theme ?? s.project?.store.scene.databank.theme;
  if (!t) throw new ToolError(`materialFile "theme" is the theme's own material file, Theme<Name>\\Theme<Name>.txt (for PREHISTORIC: ${themeFile("PREHISTORIC")}): pass theme, or base (a base level, whose theme is used), or give that path as materialFile, or open a project`);
  return { materialFile: themeFile(t), theme: t, note: `materialFile "theme": the ${t.toUpperCase()} theme's own file, ${themeFile(t)}${a.base && !a.theme ? ` (${a.base}'s theme)` : ""}` };
}

export async function listMaterials(s: Session, a0: { materialFile?: string; theme?: string; base?: string; search?: string; role?: string; limit?: number; only?: "materials" | "palette" }) {
  const kw = await themeKeyword(s, a0);
  const a = { ...a0, materialFile: kw.materialFile, theme: kw.theme };
  const levels = await s.levels();
  const store = s.project?.store;
  const theme = a.theme ?? store?.scene.databank.theme;
  // the file: the one asked for (its separators repaired), the open project's, else the theme's own
  const asked = a.materialFile !== undefined ? await resolveFile(s, a.materialFile) : null;
  const file = asked?.file ?? (store?.scene.databank.materialFile || (a.theme ? themeFile(a.theme) : undefined));
  if (!file && !theme) {
    const t = await levels.themes();
    return { note: "no project is open: pass theme and/or materialFile, or open_project", themes: t.themes, materialFiles: t.materialFiles, timesOfDay: t.timesOfDay };
  }
  const q = a.search?.toLowerCase();
  let materials: unknown = null;
  if (file && a.only !== "palette") {
    const own = !!store && !a.materialFile && file === store.scene.databank.materialFile;   // the open project's own file: its base can resolve it, and its voxels can be counted
    const base = store?.scene.base;
    const m = await loadMaterials(s, file, theme, own && base ? { key: base.key, source: base.source } : undefined)
      .catch((e: unknown) => ({ file, names: null as string[] | null, colors: null, resolved: null, error: e instanceof Error ? e.message : String(e) }));
    const counts = new Map<number, number>();
    if (own) for (const g of grids(store!)) for (const w of g.words) if (isSolid(w)) counts.set(material(w), (counts.get(material(w)) ?? 0) + 1);
    if (!m.names) {
      const files = await levels.themes().then((t) => t.materialFiles, () => [] as string[]);
      materials = { file, error: `the server could not read the material file: ${(m as { error?: string }).error}`, materialFiles: files };
    } else {
      const tint = tintFor(m as MaterialSet);
      materials = {
        file: m.file, ...(asked?.note || kw.note ? { note: [kw.note, asked?.note].filter(Boolean).join("; ") } : {}), ...(!a.materialFile && !store ? { from: `the theme's own file (${file})` } : {}),
        list: m.names.map((name, index) => ({ index, name, ...(own ? { solidVoxels: counts.get(index) ?? 0 } : {}), colour: hex(tint(index)),
          ...(m.resolved ? { textured: m.resolved[index] ?? false } : {}) }))
          .filter((e) => !q || e.name.toLowerCase().includes(q)),
        colours: m.colors ? "each material's texture colour (the average of its surface texture; textured: false is a stand-in colour); the renders use them"
          : "distinct hues per index (the server gave no texture colours); the renders use them",
        ...(own ? { solidVoxels: "counts the open project" } : {}),
      };
    }
  }

  let palette: unknown = null;
  if (theme && a.only !== "materials") {
    // the server answers { theme, entries }; the e2e mock answers the bare array
    const reply = await (await s.client()).call<{ entries?: PaletteEntry[] } | PaletteEntry[]>("level.palette", { theme }, 30000);
    const all = Array.isArray(reply) ? reply : reply.entries ?? [];
    const kept = all.filter((e) => (!q || `${e.name} ${e.resource}`.toLowerCase().includes(q)) && (!a.role || e.role === a.role));
    const limit = a.limit ?? 200;
    palette = {
      theme, total: all.length, matching: kept.length, entries: kept.slice(0, limit).map(({ name, resource, role }) => ({ name, resource, role })),
      ...(kept.length > limit ? { note: `first ${limit}: narrow with search or role, or raise limit` } : {}),
    };
  }
  return compact({ materials, palette });
}

// ------------------------------------------------------------------ tools

const floodSchema = {
  type: "object" as const, additionalProperties: false,
  description: "A rising sea: each start also gets the turn its ground goes under and whether a dry walk reaches the target in time (see check_flood).",
  properties: {
    schedule: { description: "wum.sim.waterSchedule's options {startTurn, every, step, cap, from, units (world default, or map)}, or \"script\": the project's script, run turn by turn by Melange.exe's server (every waterSchedule call, phases set from turnStarted handlers, every/after timers, Water.Level writes); on the game's own server its first waterSchedule call." },
    levels: { type: "array", maxItems: 1000, items: { type: "object", additionalProperties: false, required: ["turn", "level"], properties: { turn: { type: "integer", minimum: 1 }, level: { type: "number" } } },
      description: "Or the water by turn: [{turn, level}], each level from that turn on, in the call's units: MAP unless units: \"world\" (unlike a schedule, world by default). A level far over every ground in map units is refused as world units." },
    rounds: { type: "array", maxItems: 1000, items: { type: "object", additionalProperties: false, required: ["round", "level"], properties: { round: { type: "integer", minimum: 1 }, level: { type: "number" } } },
      description: "Or the water by round: [{round, level}], each from that round on (wum.sim.round, sim.turnStarted's round: it goes up once every team has had a turn, so round r starts at game turn (r - 1) x teams + 1), in the call's units as levels. A script that sets the water by round from a turnStarted handler is also run as it is by schedule \"script\" on Melange.exe's server." },
    start: { type: "number", description: "The water before the flood, in the call's units (map unless units: \"world\"; default the project's water level)." },
    every: { type: "integer", minimum: 1, maximum: 64, description: "Game turns between a worm's own turns (default 2: two teams)." },
    firstTurn: { type: "integer", minimum: 1, maximum: 1000, description: "The game turn of the worm's first own turn (default 1)." },
    maxTurns: { type: "integer", minimum: 1, maximum: 10000, description: "Turns looked ahead for floodsAtTurn (default 200)." },
    teams: { type: "integer", minimum: 1, maximum: 8, description: "Teams in the match, for each spawn's own moves (default 2, the Quick Game)." },
    wormsPerTeam: { type: "integer", minimum: 1, maximum: 8, description: "Worms per team (default the spawn knots / teams): WORMn is worm n % wormsPerTeam of team floor(n / wormsPerTeam)." },
    margin: { type: "number", minimum: 0, description: "Warn when the water's top comes within this of a spawn's ground (the call's units; default 1 map unit, 20 world)." },
    minMoves: { type: "integer", minimum: 2, maximum: 20, description: "Warn when a spawn floods before its worm's move this many (default 3; before the 2nd move is always warned)." },
    turnOrder: { type: "array", minItems: 1, maxItems: 64, items: { type: "integer", minimum: 0, maximum: 7 },
      description: "The worms in the order the game moved them, from game turn 1 (WORMn numbers, repeated as a cycle), instead of the engine's rotation modelled (teams alternating from team 0, each moving its living worms in turn): an order game_state's turnLog observed in a Test." },
    minOwnTurns: { type: "integer", minimum: 1, maximum: 20, description: "The verdict (floodVerdict, and PASS or FAIL in the summary): each spawn knot's ground must stay more than minDryMargin over the water at the start of each of its worm's first minOwnTurns own turns (default 2)." },
    minDryMargin: { type: "number", minimum: 0, description: "How far over the water a spawn's ground must stay through its first minOwnTurns own turns (the call's units; default 1.5 map units, 30 world: knock-back and the computer team's walking move worms, and a Test worm drowned from ground 1.7 map units up; 0: over it at all)." },
    capMargin: { type: "number", minimum: 0, description: "flood.capReport lists every spawn's ground over the water's highest level and names those within this of it (the call's units; default 3 map units, 60 world)." },
  } as Record<string, Schema>,
};

export const tools: Tool[] = [
  {
    name: "heightmap_ascii",
    description: "Top-down ASCII map of the open level: for each cell the height of the highest solid voxel, as a character from 0-9a-z (equal height bands, listed in the legend); '.' is no ground. Spawns (S), crates (C), telepads (T), triggers (R), the mine factory (F) and other placed objects (O) are overlaid. Rows are labelled with z, a ruler gives x. At most 200x200 cells: pick a region or a step. Cheap on tokens; use render_topdown for a picture.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: { ...regionProps, step: { type: "number", minimum: 0.01, maximum: 4000, description: "Size of a character in the same units as the coordinates (default: the level fits in about 120 columns)." }, markers: { type: "boolean", description: "Overlay spawns and objects (default true)." } },
    },
    handler: (s, a) => heightmapAscii(s, a),
  },
  {
    name: "render_topdown",
    description: "PNG top view of the open level (image content): height-shaded and tinted by material, water shown (ground under it blue), spawns, crates, telepads, triggers, the mine factory and other objects marked, axis ticks in the margin. The text part gives the region, how to read pixels as coordinates, the material colours and the marker list.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        ...regionProps,
        step: { type: "number", minimum: 0.01, maximum: 4000, description: "Size of a sample in the same units as the coordinates (default 1 voxel, or more for a level wider than 400)." },
        scale: { type: "integer", minimum: 1, maximum: 32, description: "Pixels per sample (default fits about 900 pixels)." },
        shading: { type: "string", enum: ["both", "height", "material"], description: "both (default): material tint with relief; height: grey relief only; material: flat tints." },
        markers: { type: "boolean", description: "Draw spawns and objects (default true)." },
        palette: paletteSchema,
      },
    },
    handler: (s, a) => renderTopdown(s, a),
  },
  {
    name: "render_side",
    description: "PNG cross-section of the open level along a line from (x0, z0) to (x1, z1): the solid voxels (tinted by material, overhangs and caves included), water, objects within 1.5 voxels of the line, a y ruler and a distance ruler. The text part adds the top profile (highest ground along the line) and the material colours.",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["x0", "z0", "x1", "z1"],
      properties: {
        x0: num("Start x."), z0: num("Start z."), x1: num("End x."), z1: num("End z."), units,
        y0: num("Lowest y shown (default: the lowest ground on the line)."), y1: num("Highest y shown (default: the highest)."),
        scale: { type: "integer", minimum: 1, maximum: 32, description: "Pixels per voxel in y (default fits about 700 pixels)." },
        palette: paletteSchema,
      },
    },
    handler: (s, a) => renderSide(s, a),
  },
  {
    name: "query_ground",
    description: "The ground at a column (x, z): the top of the highest solid voxel (map and world y, the frame and the material, under ground; frame, frameName, material and materialName are also at the top level, as other tools name them), the y to stand on, whether it is under water, every solid stretch of the column over all frames (so bridges and overhangs show; `below` asks for the ground under a given height), and the openings between them (floor, ceiling, passable at headroom). `y` answers passableAt: whether a worm fits in the opening at that height, so an open gate under a lintel is told from a wall.",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["x", "z"],
      properties: {
        x: num("x of the column."), z: num("z of the column."), units,
        below: num("Only ground under this y (in the same units), e.g. the floor under a bridge."),
        maxSpans: { type: "integer", minimum: 1, maximum: 100, description: "Stretches and openings to list (default 6)." },
        y: num("Also answer passableAt: the opening at this height (its floor the ground under y, within half a map unit over it), and whether a worm fits (at least headroom of air): an open gate under a lintel is passable, a wall is not."),
        headroom: { type: "number", minimum: 0.1, description: "The air a worm needs over a floor, for openings' passable (the call's units; default 1.5 map units, as check_reachability)." },
        mode: { type: "string", enum: ["top", "atOrBelow"], description: "top (default): ground, standOn, frame and material are the column's highest ground. atOrBelow (needs y): they are the floor a worm at y stands on, the top of the ground at or under y (within half a map unit over it) with the air over it, so a gate's floor under its lintel is the answer, not the lintel's top; null when y is inside solid." },
      },
    },
    handler: async (s, a) => compact(await queryGround(s, a)),
  },
  {
    name: "list_materials",
    description: "The level's material file (the names of its up to 64 materials in index order, with how many solid voxels of each the open project has and the colour the renders use) and the theme's object palette (the names, resources and roles that can be placed). With no project open, pass theme and/or materialFile; with none at all, lists the themes and material files.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        materialFile: { type: "string", description: "A material file from the themes listing (default: the open project's), or \"theme\": the theme's own file (Theme<Name>\\Theme<Name>.txt) of `theme`, of `base`'s theme or of the open project's." },
        theme: { type: "string", description: "Theme for the palette (default: the open project's)." },
        base: { type: "string", description: "A base level key (list_bases): with no project open, its theme is the theme used (for the palette and materialFile \"theme\")." },
        search: { type: "string", description: "Keep entries whose name (or resource) contains this (any case)." },
        role: { type: "string", enum: ["scenery", "spawn", "object", "camera", "light", "emitter", "sound", "collision", "marker", "other"], description: "Palette entries of this role." },
        limit: { type: "integer", minimum: 1, maximum: 2000, description: "Palette entries to list (default 200)." },
        only: { type: "string", enum: ["materials", "palette"], description: "Leave out the other half." },
      },
    },
    handler: (s, a) => listMaterials(s, a),
  },
  {
    name: "preview_materials",
    description: "A swatch sheet of a material file (image content): one tile per material in its real colour (the average of its texture, from the server), labelled with its index, and the list of names, colours and whether the texture was found. Default: the open project's material file; or a materialFile, or a theme's own file (theme). Use it to choose materials for heightfield bands and structures.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        materialFile: { type: "string", description: "A material file from list_materials' listing, e.g. Maps\\dinermight.txt or ThemeCamelot\\ThemeCamelot.txt, or \"theme\" (the theme's own file, as list_materials)." },
        base: { type: "string", description: "A base level key: with no project open, its theme is the theme used." },
        theme: { type: "string", description: "The theme whose folder the textures are looked up in first, and whose own file is shown when no materialFile is given and no project is open." },
      },
    },
    async handler(s, a0: { materialFile?: string; theme?: string; base?: string }) {
      const kw = await themeKeyword(s, a0);
      const a = { ...a0, materialFile: kw.materialFile, theme: kw.theme };
      const store = s.project?.store;
      const theme = a.theme ?? store?.scene.databank.theme;
      const asked = a.materialFile !== undefined ? await resolveFile(s, a.materialFile) : null;
      const file = asked?.file ?? (store?.scene.databank.materialFile || (theme ? themeFile(theme) : undefined));
      if (!file) throw new ToolError("pass materialFile or theme, or open a project");
      const own = !!store && file === store.scene.databank.materialFile;
      const m = await loadMaterials(s, file, theme, own ? { key: store!.scene.base.key, source: store!.scene.base.source } : undefined);
      if (!m.names.length) throw new ToolError(`${file} has no material records`);
      const tint = tintFor(m), TW = 72, TH = 40, COLS = 8, rows = Math.ceil(m.names.length / COLS);
      const img = new Raster(COLS * TW + 2, rows * TH + 2, [30, 30, 34]);
      m.names.forEach((_, i) => {
        const x = 2 + (i % COLS) * TW, y = 2 + Math.floor(i / COLS) * TH, c = tint(i);
        img.rect(x, y, TW - 2, TH - 2, c);
        const light = c[0] * 0.3 + c[1] * 0.59 + c[2] * 0.11 > 140;
        img.text(x + 3, y + 3, String(i), light ? [0, 0, 0] : [255, 255, 255]);
        if (m.resolved && !m.resolved[i]) img.text(x + 3, y + TH - 12, "?", light ? [0, 0, 0] : [255, 255, 255]);
      });
      const info = {
        file: m.file, theme: theme ?? null, ...(asked?.note ? { note: asked.note } : {}),
        reading: `tile i is at column i % ${COLS}, row floor(i / ${COLS}); a '?' marks a material whose texture the server did not find (a stand-in colour)`,
        colours: m.colors ? "each material's texture colour" : "the server gave no texture colours: distinct hues per index",
        materials: m.names.map((name, index) => ({ index, name, colour: hex(tint(index)), ...(m.resolved ? { textured: m.resolved[index] ?? false } : {}) })),
      };
      return { content: [image(img.png()), { type: "text", text: compact(info) }] };
    },
  },

  {
    name: "check_reachability",
    description: "Can each spawn walk to the summit? Builds a worm-walk graph over the voxels (the tops of solid ground with headroom, a column every `step`, joined where a worm can step up maxStep or climb a one-voxel riser whose stair, the rise over the tread above it, is no steeper than maxSlope, drop maxDrop, or optionally jump a gap of maxGap) and searches it from every spawn knot (or the `from` points) to the target (default: the highest ground; or `target` x, z, radius). Answers per spawn whether it gets there, the walk, the least turns it takes (walkSpeed x turnTime a turn), waypoints and path (length along the ground, steepest slope, the steepest stair as judged, highest step, deepest drop), or the closest point reached, where it is blocked (the climb that stops it) and needs: the cheapest utility (jump, girder, rope, jetpack; a heuristic) that gets there and where it is first used. Ground at or under the water is not walked. `flood` checks a rising sea (as check_flood); `islets` lists detached terrain (as check_islets). `via` constrains the walk to (or away from) a route such as the roads, and `materialSlopes` caps the climb onto given materials, to prove the roads are the way up. render: true adds a PNG of the routes.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        ...regionProps,
        from: { type: "array", maxItems: 64, items: { type: "array", items: { type: "number" }, minItems: 3, maxItems: 3 }, description: "Start points [x, y, z] instead of the spawn knots." },
        target: { anyOf: [{ type: "object", additionalProperties: false, required: ["x", "z"], properties: { x: { type: "number" }, z: { type: "number" }, radius: { type: "number", minimum: 0.1 } } },
          { type: "string", enum: ["nearest"] }], description: "The goal: the highest ground within radius (default 3 map units) of x, z; or \"nearest\" with targets: the nearest of them." },
        targets: { type: "array", minItems: 1, maxItems: 32, description: "Several goals instead of target: the walk may end at any (each start answers reachedTarget, the one it gets to first); each the highest ground within radius (default 3 map units) of x, z, with an optional name. target \"nearest\" says the same.",
          items: { type: "object", additionalProperties: false, required: ["x", "z"], properties: { x: { type: "number" }, z: { type: "number" }, radius: { type: "number", minimum: 0.1 }, name: { type: "string" } } } },
        targetMode: { type: "string", enum: ["nearest", "each"], description: "With targets: nearest (default) answers the target each start reaches first; each also answers every target's own walk (perTarget)." },
        step: { type: "number", minimum: 0.25, maximum: 8, description: "Column spacing in the call's units (default 1 map unit)." },
        maxStep: { type: "number", minimum: 0, description: "A step up always taken (default 0.5 map units, 10 world)." },
        maxSlope: { type: "number", minimum: 0, maximum: 89, description: "Degrees: a riser of at most one voxel is walked when its stair is no steeper than this (default 45): the rise over the tread above it (slopeOver tread), or over one voxel of the upper ground (riser)." },
        slopeOver: { type: "string", enum: ["tread", "riser", "segment", "skin"], description: "What a riser's slope is judged over: tread (default; the flat run above it to the next riser, up to 8 voxels, so a voxel staircase is walked at its average slope), riser (one voxel of the upper ground: the rule before, under which every voxel staircase is 45 degrees or more), segment (every climb over its own move: the rise over the horizontal step, so a smooth skin's steep face is a barrier) or skin (segment where either end is a draped skin's, tread elsewhere: terraces as stairs, skin cliffs of 62-78 degrees as barriers)." },
        walkSpeed: { type: "number", minimum: 0.01, description: `A worm's walking speed in the call's units a second, for the turn counts (default ${WALK_SPEED} map units, ${WALK_SPEED * 20} world).` },
        turnTime: { type: "number", minimum: 1, maximum: 600, description: `Seconds of a turn spent walking, for the turn counts (default ${TURN_TIME}, the Quick Game's turn).` },
        flood: floodSchema,
        islets: { type: "boolean", description: "Also report the terrain not joined to the main landmass: offshore chunks, specks and floating debris (as check_islets)." },
        maxDrop: { type: "number", minimum: 0, description: "The longest drop taken (default 8 map units)." },
        maxGap: { type: "number", minimum: 0, description: "Jump gaps up to this wide to ground no higher than maxStep above (default 0: walking only)." },
        headroom: { type: "number", minimum: 0.5, description: "Air a worm needs above the ground (default 1.5 map units)." },
        bodyWidth: { type: "number", minimum: 0, description: "The worm's width in the call's units: a move into a gap narrower than this (the free width across it at body height: a gate, a keep's door, the slot between tower pillars) is not walked. Default 0: not enforced, but every route reports its narrowest gap and the summary names routes through gaps under 0.6 map units (a worm's body, about 11 world units)." },
        bodyHeight: { type: "number", minimum: 0.5, description: "The worm's height for the gap widths: walls are solid between its ground + maxStep and this over it (default headroom)." },
        route: { type: "boolean", description: "Also answer each start's whole route column by column (`route`, at most 2000 points; for game_walk's path)." },
        water: { type: "boolean", description: "Treat ground at or under the project's water level as impassable (default true; the level's own water height is not known)." },
        render: { type: "boolean", description: "Also answer a top-down PNG (render_topdown's) with each start's route drawn in its colour: the walk, the route with the utility it needs (dashed), where that is used and where the walk is blocked (default false)." },
        palette: paletteSchema,
        via: { type: "object", additionalProperties: false, description: "A route constraint, to prove the roads are the climb: the ground on the route is where its top voxel's material is one of `materials` and/or `mask` is above 0 (the heightfield expression language of x, z, r, theta in the call's units, h the ground's height and material, its index). mode only (default): walk on it alone (reachable: there is a way up along the roads); avoid: never on it (not reachable: the roads are the only way). Ground within `near` (default 3 map units) of a start or the target is always walkable.",
          properties: { materials: { type: "array", items: { type: "integer", minimum: 0, maximum: 63 }, maxItems: 64 }, mask: { type: "string" }, mode: { type: "string", enum: ["only", "avoid"] }, near: { type: "number", minimum: 0 } } },
        materialSlopes: { type: "array", maxItems: 32, description: "The steepest climb onto ground of these materials (the top voxel's), degrees: any rise onto it steeper is refused, small steps too, so ash or scree flanks are not walked up while the paved road is. [{materials: [24, 36], maxSlope: 20}].",
          items: { type: "object", additionalProperties: false, required: ["materials", "maxSlope"], properties: { materials: { type: "array", items: { type: "integer", minimum: 0, maximum: 63 }, minItems: 1, maxItems: 64 }, maxSlope: { type: "number", minimum: 0, maximum: 89 } } } },
      },
    },
    handler: (s, a) => checkReachability(s, a),
  },
  {
    name: "check_flood",
    description: "A rising sea against the spawns: for each spawn knot (or `from` point), the turn the water reaches the ground it stands on (floodsAtTurn), and whether a dry walk gets it to the summit (or `target`) in time: a place counts only while it is dry in the turn the worm gets there, the worm walking walkSpeed x turnTime in each of its own turns (the first in game turn firstTurn, then every `every` turns). The flood is `schedule` (wum.sim.waterSchedule's options: startTurn, every, step, cap, from, units; or \"script\" to read the project's script), or `levels` ([{turn, level}] in the call's units). Each spawn knot also gets `floods` (the game turn, the water and its ground in map and world units both) and `moves`: its worm's own moves (teams x wormsPerTeam game turns apart), how many it gets before its ground floods and whether a walk on those alone escapes, in the engine's worm rotation (teams alternating from team 0, each moving its living worms in turn, drowned worms skipped; or turnOrder) with `ownTurns`: the water and the ground's clearance at the start of each of its first own turns; `floodVerdict` (PASS or FAIL in the summary, naming each failing spawn, its own turn and the game turn) holds every spawn's ground more than minDryMargin over the water through its first minOwnTurns own turns (default 2, margin 1.5 map units), and `flood.capReport` gives every spawn's ground over the water's highest level, naming those within capMargin (default 3 map units). Game turns count from 1, as wum.sim.turnStarted, game_state's turn and waterSchedule's startTurn do. `flood.warnings` names a spawn whose ground the water's top reaches (within margin) and one that floods before its worm's 2nd (or minMoves-th) move. It is check_reachability with the flood given; the other arguments are the same.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        ...regionProps,
        schedule: floodSchema.properties.schedule, levels: floodSchema.properties.levels, rounds: floodSchema.properties.rounds, start: floodSchema.properties.start, every: floodSchema.properties.every,
        firstTurn: floodSchema.properties.firstTurn, maxTurns: floodSchema.properties.maxTurns, teams: floodSchema.properties.teams, wormsPerTeam: floodSchema.properties.wormsPerTeam,
        margin: floodSchema.properties.margin, minMoves: floodSchema.properties.minMoves, turnOrder: floodSchema.properties.turnOrder,
        minOwnTurns: floodSchema.properties.minOwnTurns, minDryMargin: floodSchema.properties.minDryMargin, capMargin: floodSchema.properties.capMargin,
        from: { type: "array", maxItems: 64, items: { type: "array", items: { type: "number" }, minItems: 3, maxItems: 3 }, description: "Start points [x, y, z] instead of the spawn knots." },
        target: { anyOf: [{ type: "object", additionalProperties: false, required: ["x", "z"], properties: { x: { type: "number" }, z: { type: "number" }, radius: { type: "number", minimum: 0.1 } } },
          { type: "string", enum: ["nearest"] }], description: "The goal: the highest ground within radius (default 3 map units) of x, z; or \"nearest\" with targets: the nearest of them." },
        targets: { type: "array", minItems: 1, maxItems: 32, description: "Several goals instead of target: the walk may end at any (each start answers reachedTarget, the one it gets to first); each the highest ground within radius (default 3 map units) of x, z, with an optional name. target \"nearest\" says the same.",
          items: { type: "object", additionalProperties: false, required: ["x", "z"], properties: { x: { type: "number" }, z: { type: "number" }, radius: { type: "number", minimum: 0.1 }, name: { type: "string" } } } },
        targetMode: { type: "string", enum: ["nearest", "each"], description: "With targets: nearest (default) answers the target each start reaches first; each also answers every target's own walk (perTarget)." },
        step: { type: "number", minimum: 0.25, maximum: 8, description: "Column spacing in the call's units (default 1 map unit)." },
        maxStep: { type: "number", minimum: 0 }, maxSlope: { type: "number", minimum: 0, maximum: 89 }, maxDrop: { type: "number", minimum: 0 }, maxGap: { type: "number", minimum: 0 },
        slopeOver: { type: "string", enum: ["tread", "riser", "segment", "skin"] }, walkSpeed: { type: "number", minimum: 0.01 }, turnTime: { type: "number", minimum: 1, maximum: 600 },
        render: { type: "boolean" }, palette: paletteSchema,
        bodyWidth: { type: "number", minimum: 0, description: "The worm's width in the call's units: a move into a gap narrower than this (the free width across it at body height: a gate, a keep's door, the slot between tower pillars) is not walked. Default 0: not enforced, but every route reports its narrowest gap and the summary names routes through gaps under 0.6 map units (a worm's body, about 11 world units)." },
        bodyHeight: { type: "number", minimum: 0.5, description: "The worm's height for the gap widths: walls are solid between its ground + maxStep and this over it (default headroom)." },
        route: { type: "boolean", description: "Also answer each start's whole route column by column (`route`, at most 2000 points; for game_walk's path)." },
        via: { type: "object", additionalProperties: false, description: "A route constraint, to prove the roads are the climb: the ground on the route is where its top voxel's material is one of `materials` and/or `mask` is above 0 (the heightfield expression language of x, z, r, theta in the call's units, h the ground's height and material, its index). mode only (default): walk on it alone (reachable: there is a way up along the roads); avoid: never on it (not reachable: the roads are the only way). Ground within `near` (default 3 map units) of a start or the target is always walkable.",
          properties: { materials: { type: "array", items: { type: "integer", minimum: 0, maximum: 63 }, maxItems: 64 }, mask: { type: "string" }, mode: { type: "string", enum: ["only", "avoid"] }, near: { type: "number", minimum: 0 } } },
        materialSlopes: { type: "array", maxItems: 32, description: "The steepest climb onto ground of these materials (the top voxel's), degrees: any rise onto it steeper is refused, small steps too, so ash or scree flanks are not walked up while the paved road is. [{materials: [24, 36], maxSlope: 20}].",
          items: { type: "object", additionalProperties: false, required: ["materials", "maxSlope"], properties: { materials: { type: "array", items: { type: "integer", minimum: 0, maximum: 63 }, minItems: 1, maxItems: 64 }, maxSlope: { type: "number", minimum: 0, maximum: 89 } } } },
      },
    },
    handler(s, a: ReachArgs & FloodArgs) {
      const { schedule, levels, rounds, start, every, firstTurn, maxTurns, teams, wormsPerTeam, margin, minMoves, turnOrder, minOwnTurns, minDryMargin, capMargin, ...rest } = a;
      if (!schedule && !levels && !rounds) throw new ToolError("give schedule (wum.sim.waterSchedule's options, or \"script\"), levels or rounds");
      return checkReachability(s, { ...rest, flood: { schedule, levels, rounds, start, every, firstTurn, maxTurns, teams, wormsPerTeam, margin, minMoves, turnOrder, minOwnTurns, minDryMargin, capMargin } });
    },
  },
  {
    name: "check_islets",
    description: "Terrain not joined to the main landmass: the solid voxels of every frame merged per column (a column every `step`), joined where side-by-side columns' stretches overlap in height, and split into pieces. The largest is the main landmass; every other piece is listed smallest first (its volume in map units cubed, columns, bounds, top, the frames it is in, whether it is under the water), and those under `minVolume` are flagged debris: the coastal speckle and the offshore chunks a top view shows. Read-only.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        ...regionProps,
        step: { type: "number", minimum: 0.25, maximum: 8, description: "Column spacing in the call's units (default 1 map unit)." },
        minVolume: { type: "number", minimum: 0, description: "Pieces smaller than this (map units cubed) are flagged debris (default 50)." },
        limit: { type: "integer", minimum: 1, maximum: 500, description: "Pieces to list (default 20)." },
      },
    },
    handler(s, a: RegionArgs & { step?: number; minVolume?: number; limit?: number }) {
      const gs = grids(s.store), r = region(gs, a);
      const step = a.step === undefined ? 1 : inMap(a.step, a.units);
      const cols = Math.ceil((r.x1 - r.x0) / step) * Math.ceil((r.z1 - r.z0) / step);
      if (cols > 1_000_000) throw new ToolError(`${cols} columns is too many (at most 1000000): use a larger step or a smaller region`);
      return compact({ region: regionInfo(r, a.units), step: r2(outUnit(step, a.units)), ...isletSummary(gs, r, step, waterMap(s.store), a.units, a.minVolume ?? 50, a.limit ?? 20) });
    },
  },
  {
    name: "check_holes",
    description: "See-through holes in the ground of a region: a ray straight down every `step` (default 0.5 map units) over the region (x0, z0, x1, z1, or `around` {x, z, radius}: a circle such as the crater floor under the mine factory); a column whose ray hits no solid voxel over `above` (default the project's water level, else 0) is a hole, where the camera sees through the island (a plug cut out of a draped skin, a gap between a skin and an undraped block, a crater floor carved through). Draped skins are read at their sheared height and rotated frames sampled, as the other inspect tools read them. Answers the hole columns and their share, the holes grouped (largest first: cells, area, centre, box, and `through`: low ground under `above`, or the void with no voxel under it) and the seams: side-by-side columns whose top stretches do not overlap in height (ground, but a crack seen from the side: a thin skin stepping more than its thickness), with `ok` true when there is neither. Read-only.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        ...regionProps,
        around: { type: "object", additionalProperties: false, required: ["x", "z", "radius"], properties: { x: { type: "number" }, z: { type: "number" }, radius: { type: "number", minimum: 0.1 } },
          description: "A circle instead of (or inside) the region, in the call's units." },
        step: { type: "number", minimum: 0.1, maximum: 8, description: "Ray spacing in the call's units (default 0.5 map units)." },
        above: { type: "number", description: "A ray must hit ground over this height (the call's units; default the project's water level, else 0)." },
        seams: { type: "boolean", description: "Also look for seams (default true)." },
        limit: { type: "integer", minimum: 1, maximum: 200, description: "Hole groups to list (default 20)." },
      },
    },
    handler(s, a: RegionArgs & { around?: { x: number; z: number; radius: number }; step?: number; above?: number; seams?: boolean; limit?: number }) {
      const gs = grids(s.store), u = a.units;
      const c = a.around && { x: inMap(a.around.x, u), z: inMap(a.around.z, u), r: inMap(a.around.radius, u) };
      const r = c && a.x0 === undefined && a.x1 === undefined && a.z0 === undefined && a.z1 === undefined
        ? { x0: c.x - c.r, z0: c.z - c.r, x1: c.x + c.r, z1: c.z + c.r } : region(gs, a);
      const step = a.step === undefined ? 0.5 : inMap(a.step, u);
      const cols = Math.ceil((r.x1 - r.x0) / step) * Math.ceil((r.z1 - r.z0) / step);
      if (cols > 1_000_000) throw new ToolError(`${cols} columns is too many (at most 1000000): use a larger step or a smaller region`);
      const water = waterMap(s.store), above = a.above !== undefined ? inMap(a.above, u) : water ?? 0;
      const rep = findHoles(gs, r, step, above, c ? (x, z) => Math.hypot(x - c.x, z - c.z) <= c.r : undefined);
      const o = (v: number) => r2(outUnit(v, u)), xz = (p: [number, number]) => [o(p[0]), o(p[1])];
      const seams = a.seams !== false;
      return compact({
        ok: rep.holes === 0 && (!seams || rep.seamCount === 0),
        summary: `${rep.holes} of ${rep.columns} rays (${Math.round(rep.fraction * 1000) / 10} %) hit no ground over ${o(above)}` +
          (rep.clusters.length ? `, in ${rep.clusters.length} hole${rep.clusters.length > 1 ? "s" : ""} (largest ${o(rep.clusters[0].area * (u === "world" ? WORLD_PER_MAP : 1))} ${u === "world" ? "world" : "map"} units square at ${xz(rep.clusters[0].centre).join(", ")})` : "") +
          (seams ? `; ${rep.seamCount} seam${rep.seamCount === 1 ? "" : "s"}` : ""),
        region: regionInfo(r, u), ...(c ? { around: a.around } : {}), step: o(step), above: o(above),
        aboveNote: a.above !== undefined ? "as given" : water !== null ? "the project's water level" : "0 (the project keeps the level's own water height)",
        holes: { columns: rep.holes, of: rep.columns, groups: rep.clusters.slice(0, a.limit ?? 20).map((h) => ({ cells: h.cells, area: r2(h.area * (u === "world" ? WORLD_PER_MAP * WORLD_PER_MAP : 1)),
          centre: xz(h.centre), min: xz(h.min), max: xz(h.max), through: h.through })), groupCount: rep.clusters.length },
        ...(seams ? { seams: { count: rep.seamCount, examples: rep.seams.slice(0, 20).map((m) => ({ at: xz(m.at), gap: o(m.gap), tops: m.tops.map(o) })) } } : {}),
      });
    },
  },
];

