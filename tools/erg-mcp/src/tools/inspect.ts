// Tool group "inspect": read-only looks at the open level. heightmap_ascii, render_topdown and render_side show the ground
// (text and PNG), query_ground answers one column, list_materials lists the material and object palettes. None of them
// edits the project.
import type { EditorStore, ServerPaletteEntry as PaletteEntry, Vec3 } from "../core";
import { extentXZ, markers, sampleField, spansAt, waterMap, type Field, type MarkerKind, type Region } from "../lib/inspect-field";
import { Raster, SKY, VOID, WATER, hex, materialTint, shade, type Rgb } from "../lib/inspect-png";
import { nodeAt, search, standing, surfaces, type ReachOptions } from "../lib/reach";
import { loadMaterials, projectMaterials, resolveFile, themeFile, tintFor, type MaterialSet, type PaletteMode } from "../lib/materials";
import { WORLD_PER_MAP, grids, isSolid, material, tidy, type Grid } from "../lib/voxel";
import { ToolError, image, type Content, type Tool } from "../registry";
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

export async function renderTopdown(s: Session, a: RegionArgs & { step?: number; scale?: number; shading?: "both" | "height" | "material"; markers?: boolean; palette?: PaletteMode }): Promise<{ content: Content[] }> {
  const store = s.store, gs = grids(store);
  const r = region(gs, a);
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

export async function queryGround(s: Session, a: { x: number; z: number; units?: "map" | "world"; below?: number; maxSpans?: number }) {
  const store = s.store;
  const px = inMap(a.x, a.units), pz = inMap(a.z, a.units);
  const sp = spansAt(grids(store), px, pz, a.below === undefined ? Infinity : inMap(a.below, a.units));
  const water = waterMap(store);
  const names = await materialNames(s);
  const out = (y: number) => ({ map: r2(y), world: r2(y * WORLD_PER_MAP) });
  const span = (q: (typeof sp)[number]) => ({ top: out(q.top), bottom: out(q.bottom), frame: q.frame, frameName: frameName(store, q.frame) ?? null, material: q.material, materialName: names?.[q.material] ?? null });
  const at = { map: [r2(px), r2(pz)], world: [r2(px * WORLD_PER_MAP), r2(pz * WORLD_PER_MAP)] };
  if (!sp.length) return { at, ground: null, note: a.below === undefined ? "no voxel frame has ground in this column" : "no ground under that height in this column" };
  const top = span(sp[0]);
  return {
    at, ground: top,
    // the top stretch's frame and material at the top level too, where the other tools name a frame (frame, frameName)
    frame: top.frame, frameName: top.frameName, material: top.material, materialName: top.materialName,
    standOn: out(sp[0].top),    // the y a thing placed here rests on
    water: water === null ? "the level's own height (not known here)" : { map: r2(water), world: r2(water * WORLD_PER_MAP), groundIsUnderWater: sp[0].top <= water },
    spans: { count: sp.length, listed: sp.slice(0, a.maxSpans ?? 6).map(span), note: "highest first: every stretch of solid voxels in the column over all frames (below asks for the ground under a height)" },
  };
}

// ------------------------------------------------------------------ check_reachability

interface ReachArgs extends RegionArgs {
  from?: number[][]; target?: { x: number; z: number; radius?: number }; step?: number; maxStep?: number; maxSlope?: number; maxDrop?: number;
  maxGap?: number; headroom?: number; water?: boolean;
}

export function checkReachability(s: Session, a: ReachArgs) {
  const store = s.store, gs = grids(store), u = a.units;
  const L = (v: number | undefined, d: number) => (v === undefined ? d : inMap(v, u));
  const r = region(gs, a);
  const step = L(a.step, 1);
  const cols = Math.ceil((r.x1 - r.x0) / step) * Math.ceil((r.z1 - r.z0) / step);
  if (cols > 1_000_000) throw new ToolError(`${cols} columns is too many (at most 1000000): use a larger step or a smaller region`);
  const water = a.water === false ? null : waterMap(store);
  const o: ReachOptions = { step, maxStep: L(a.maxStep, 0.5), maxSlope: a.maxSlope ?? 45, maxDrop: L(a.maxDrop, 8), maxGap: L(a.maxGap, 0), headroom: L(a.headroom, 1.5), water };
  const S = surfaces(gs, r, step, o.headroom);
  if (!S.nodes) throw new ToolError("no ground in the region");
  const out = (p: Vec3) => p.map((v) => r2(outUnit(v, u)));
  // the target: the highest ground (in the circle, when one is given) and every surface within 1.5 of that height there
  const tr = a.target ? L(a.target.radius, 3) : 3;
  let tx = 0, tz = 0, top = -Infinity;
  const inCircle = (n: number, x: number, z: number) => { const p = nodeAt(S, n); return Math.hypot(p[0] - x, p[2] - z) <= tr; };
  if (a.target) {
    tx = inMap(a.target.x, u); tz = inMap(a.target.z, u);
    for (let n = 0; n < S.nodes; n++) if (S.y[n] > top && inCircle(n, tx, tz)) top = S.y[n];
    if (top === -Infinity) throw new ToolError("no ground within the target's radius");
  } else {
    let best = 0;
    for (let n = 1; n < S.nodes; n++) if (S.y[n] > S.y[best]) best = n;
    [tx, top, tz] = nodeAt(S, best);
  }
  const goal = new Uint8Array(S.nodes);
  let goals = 0;
  for (let n = 0; n < S.nodes; n++) if (S.y[n] >= top - 1.5 && inCircle(n, tx, tz) && (water === null || S.y[n] > water)) { goal[n] = 1; goals++; }
  if (!goals) throw new ToolError("the target is under the water");
  // the starts: the given points, else the spawn knots
  const starts: { name: string; at: Vec3 }[] = a.from
    ? a.from.map((p, i) => {
      if (p.length !== 3 || !p.every(Number.isFinite)) throw new ToolError(`from[${i}]: [x, y, z]`);
      return { name: `from[${i}]`, at: [inMap(p[0], u), inMap(p[1], u), inMap(p[2], u)] as Vec3 };
    })
    : markers(store).filter((m) => m.kind === "spawn").map((m) => ({ name: m.name, at: m.pos }));
  if (!starts.length) throw new ToolError("the level has no spawn knots: pass from");
  const spawns = starts.map((st) => {
    const n0 = standing(S, st.at, o);
    if (n0 < 0) return { name: st.name, at: out(st.at), reachable: false, why: water !== null && standing(S, st.at, { ...o, water: null }) >= 0 ? "it stands at or under the water" : "no ground under it in the region" };
    const q = search(S, n0, o);
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
    if (hit >= 0) {
      const path: number[] = [];
      for (let n = hit; n >= 0; n = q.from[n]) path.push(n);
      path.reverse();
      const every = Math.max(1, Math.ceil(path.length / 12));
      return { ...base, walk: r2(outUnit(q.dist[hit], u)), waypoints: path.filter((_, i) => i % every === 0 || i === path.length - 1).map((n) => out(nodeAt(S, n))) };
    }
    // where it is stopped: the reached ground nearest the target with a climb too high next to it
    const stops: { n: number; d: number }[] = [];
    for (let n = 0; n < S.nodes; n++) if (q.reached[n] && q.blocked[n] < Infinity) { const p = nodeAt(S, n); stops.push({ n, d: Math.hypot(p[0] - tx, p[2] - tz) }); }
    stops.sort((x, y) => x.d - y.d);
    const blockedAt: object[] = [];
    for (const st2 of stops) {
      const p = nodeAt(S, st2.n);
      if (blockedAt.some((b) => Math.hypot((b as { x: number }).x - p[0], (b as { z: number }).z - p[2]) < 4 * step)) continue;
      blockedAt.push({ x: p[0], z: p[2], at: out(p), climbNeeded: r2(outUnit(q.blocked[st2.n], u)) });
      if (blockedAt.length >= 3) break;
    }
    return { ...base, closest: { at: out(nodeAt(S, near)), distance: r2(outUnit(nearD, u)) },
      blockedAt: blockedAt.map((b) => { const { x: _x, z: _z, ...rest } = b as { x: number; z: number }; return rest; }) };
  });
  const ok = spawns.filter((x) => x.reachable).length;
  return compact({
    summary: `${ok} of ${spawns.length} reach the target on foot`,
    target: { at: out([tx, top, tz]), radius: r2(outUnit(tr, u)), surfaces: goals, note: a.target ? "the highest ground within the radius" : "the highest ground of the region" },
    rules: { step: r2(outUnit(step, u)), maxStep: r2(outUnit(o.maxStep, u)), maxSlope: o.maxSlope, maxDrop: r2(outUnit(o.maxDrop, u)), maxGap: r2(outUnit(o.maxGap, u)),
      headroom: r2(outUnit(o.headroom, u)), water: water === null ? "not used (the level's own height is unknown, or water: false)" : r2(outUnit(water, u)), units: u ?? "map",
      note: "walking only (no rope, jetpack or girders): a climb is taken when it is at most maxStep, or no steeper than maxSlope over one voxel of the ground it climbs onto (the game draws a voxel step as a slope); drops up to maxDrop; jumps over gaps up to maxGap (0: none); ground at or under the water is not used" },
    graph: { columns: S.w * S.h, surfaces: S.nodes },
    spawns,
  });
}

// ------------------------------------------------------------------ list_materials

export async function listMaterials(s: Session, a: { materialFile?: string; theme?: string; search?: string; role?: string; limit?: number; only?: "materials" | "palette" }) {
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
        file: m.file, ...(asked?.note ? { note: asked.note } : {}), ...(!a.materialFile && !store ? { from: `the theme's own file (${file})` } : {}),
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
    description: "The ground at a column (x, z): the top of the highest solid voxel (map and world y, the frame and the material, under ground; frame, frameName, material and materialName are also at the top level, as other tools name them), the y to stand on, whether it is under water, and every solid stretch of the column over all frames (so bridges and overhangs show; `below` asks for the ground under a given height).",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["x", "z"],
      properties: {
        x: num("x of the column."), z: num("z of the column."), units,
        below: num("Only ground under this y (in the same units), e.g. the floor under a bridge."),
        maxSpans: { type: "integer", minimum: 1, maximum: 100, description: "Stretches to list (default 6)." },
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
        materialFile: { type: "string", description: "A material file from the themes listing (default: the open project's)." },
        theme: { type: "string", description: "Theme for the palette (default: the open project's)." },
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
        materialFile: { type: "string", description: "A material file from list_materials' listing, e.g. Maps\\dinermight.txt or ThemeCamelot\\ThemeCamelot.txt." },
        theme: { type: "string", description: "The theme whose folder the textures are looked up in first, and whose own file is shown when no materialFile is given and no project is open." },
      },
    },
    async handler(s, a: { materialFile?: string; theme?: string }) {
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
    description: "Can each spawn walk to the summit? Builds a worm-walk graph over the voxels (the tops of solid ground with headroom, a column every `step`, joined where a worm can step up maxStep or climb a slope no steeper than maxSlope over one voxel, drop maxDrop, or optionally jump a gap of maxGap) and searches it from every spawn knot (or the `from` points) to the target (default: the highest ground; or `target` x, z, radius). Answers per spawn whether it gets there, the walk and waypoints, or the closest point reached and where it is blocked (the climb that stops it). Ground at or under the water is not walked.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        ...regionProps,
        from: { type: "array", maxItems: 64, items: { type: "array", items: { type: "number" }, minItems: 3, maxItems: 3 }, description: "Start points [x, y, z] instead of the spawn knots." },
        target: { type: "object", additionalProperties: false, required: ["x", "z"], description: "The goal: the highest ground within radius (default 3 map units) of x, z.",
          properties: { x: { type: "number" }, z: { type: "number" }, radius: { type: "number", minimum: 0.1 } } },
        step: { type: "number", minimum: 0.25, maximum: 8, description: "Column spacing in the call's units (default 1 map unit)." },
        maxStep: { type: "number", minimum: 0, description: "A step up always taken (default 0.5 map units, 10 world)." },
        maxSlope: { type: "number", minimum: 0, maximum: 89, description: "Degrees: a riser no steeper than this over one voxel of the upper ground is walked (default 45)." },
        maxDrop: { type: "number", minimum: 0, description: "The longest drop taken (default 8 map units)." },
        maxGap: { type: "number", minimum: 0, description: "Jump gaps up to this wide to ground no higher than maxStep above (default 0: walking only)." },
        headroom: { type: "number", minimum: 0.5, description: "Air a worm needs above the ground (default 1.5 map units)." },
        water: { type: "boolean", description: "Treat ground at or under the project's water level as impassable (default true; the level's own water height is not known)." },
      },
    },
    handler: (s, a) => checkReachability(s, a),
  },
];

