// Tool group "inspect": read-only looks at the open level. heightmap_ascii, render_topdown and render_side show the ground
// (text and PNG), query_ground answers one column, list_materials lists the material and object palettes. None of them
// edits the project.
import type { EditorStore, ServerPaletteEntry as PaletteEntry } from "../core";
import { extentXZ, markers, sampleField, spansAt, waterMap, type Field, type MarkerKind, type Region } from "../lib/inspect-field";
import { Raster, SKY, VOID, WATER, hex, materialTint, shade, type Rgb } from "../lib/inspect-png";
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
  try {
    const { scene } = s.requireProject().store;
    const m = await (await s.levels()).materials(scene.databank.materialFile, scene.base.key, scene.base.source);
    return Array.isArray(m?.names) ? m.names : null;
  } catch {
    return null;
  }
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
export function drawTopdown(f: Field, scale: number, opts: { water: number | null; shading: "both" | "height" | "material" }) {
  const img = new Raster(f.w * scale + MARGIN, f.h * scale + MARGIN, [12, 12, 14]);
  const span = f.solid ? Math.max(1e-6, f.max - f.min) : 1;
  const topAt = (i: number, j: number) => (i < 0 || j < 0 || i >= f.w || j >= f.h ? -Infinity : f.top[j * f.w + i]);
  for (let j = 0; j < f.h; j++)
    for (let i = 0; i < f.w; i++) {
      const t = f.top[j * f.w + i];
      let c: Rgb;
      if (t === -Infinity) c = opts.water !== null ? (WATER.map((v) => Math.round(v * 0.55)) as Rgb) : VOID;
      else {
        const base = opts.shading === "height" ? ([150, 150, 150] as Rgb) : materialTint(f.mat[j * f.w + i]);
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

export async function renderTopdown(s: Session, a: RegionArgs & { step?: number; scale?: number; shading?: "both" | "height" | "material"; markers?: boolean }): Promise<{ content: Content[] }> {
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
  const { img, px, pz } = drawTopdown(f, scale, { water, shading });
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
  const names = await materialNames(s);
  const u = a.units ?? "map";
  const info = {
    image: { width: img.w, height: img.h, pixelsPerCell: scale, cellSize: `${r2(outUnit(step, a.units))} ${u} units`, margin: MARGIN },
    region: regionInfo(r, a.units),
    reading: `pixel (px, py) is x ${r2(outUnit(f.x0, a.units))} + (px - ${MARGIN}) / ${scale} * ${r2(outUnit(step, a.units))}, and z likewise with py; x grows right, z grows down. Ticks are labelled in ${u} units.`,
    ground: f.solid ? { cells: f.solid, of: f.w * f.h, topY: { min: r2(outUnit(f.min, a.units)), max: r2(outUnit(f.max, a.units)) } } : "none in this region",
    shading: shading === "height" ? "grey relief: brighter = higher" : shading === "material" ? "flat material tints" : "material tint, brighter = higher, with relief",
    water: water === null ? "the level's own height (not known here, so not drawn); columns with no ground are dark grey" : { y: r2(outUnit(water, a.units)), drawn: "ground at or under it is tinted blue; columns with no ground are dark blue" },
    materials: [...usedMat].sort((x, y) => y[1] - x[1]).map(([m, n]) => ({ index: m, name: names?.[m] ?? null, colour: hex(materialTint(m)), cells: n })),
    markers: { legend: Object.fromEntries(Object.entries(MARK_RGB).map(([k, c]) => [k, hex(c)])), placed },
  };
  return { content: [image(img.png()), { type: "text", text: compact(info) }] };
}

// ------------------------------------------------------------------ render_side

export async function renderSide(s: Session, a: { x0: number; z0: number; x1: number; z1: number; units?: "map" | "world"; y0?: number; y1?: number; scale?: number }) {
  const store = s.store, gs = grids(store);
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
    for (const q of cols[k]) {
      const top = Math.min(q.top, hi), bot = Math.max(q.bottom, lo);
      if (top <= bot) continue;
      const c = materialTint(q.material);
      usedMat.add(q.material);
      img.rect(left + k * xPer, yPix(top), xPer, Math.max(1, yPix(bot) - yPix(top)), c);
      img.rect(left + k * xPer, yPix(top), xPer, 1, shade(c, 1.3));
    }
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
  const names = await materialNames(s);
  const u = a.units ?? "map";
  const info = {
    image: { width: img.w, height: img.h, pixelsPerVoxelY: yScale, pixelsPerSample: xPer, leftMargin: left, topMargin: MARGIN },
    line: { from: [r2(outUnit(ax, a.units)), r2(outUnit(az, a.units))], to: [r2(outUnit(bx, a.units)), r2(outUnit(bz, a.units))], length: r2(outUnit(len, a.units)), units: u },
    reading: `left to right is the distance along the line from the first point (top ruler); the left ruler is y (${u} units). Light blue is sky, blue is water${water === null ? " (the level's own height is not known here, so none is drawn)" : ` (y ${r2(outUnit(water, a.units))})`}.`,
    y: [r2(outUnit(lo, a.units)), r2(outUnit(hi, a.units))],
    topProfile: { note: "[distance along the line, highest ground y] (null: none), about 80 samples", points: profile },
    materials: [...usedMat].map((m) => ({ index: m, name: names?.[m] ?? null, colour: hex(materialTint(m)) })),
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
  return {
    at, ground: span(sp[0]),
    standOn: out(sp[0].top),    // the y a thing placed here rests on
    water: water === null ? "the level's own height (not known here)" : { map: r2(water), world: r2(water * WORLD_PER_MAP), groundIsUnderWater: sp[0].top <= water },
    spans: { count: sp.length, listed: sp.slice(0, a.maxSpans ?? 6).map(span), note: "highest first: every stretch of solid voxels in the column over all frames (below asks for the ground under a height)" },
  };
}

// ------------------------------------------------------------------ list_materials

export async function listMaterials(s: Session, a: { materialFile?: string; theme?: string; search?: string; role?: string; limit?: number; only?: "materials" | "palette" }) {
  const levels = await s.levels();
  const store = s.project?.store;
  const file = a.materialFile ?? store?.scene.databank.materialFile;
  const theme = a.theme ?? store?.scene.databank.theme;
  if (!file && !theme) {
    const t = await levels.themes();
    return { note: "no project is open: pass theme and/or materialFile, or open_project", themes: t.themes, materialFiles: t.materialFiles, timesOfDay: t.timesOfDay };
  }
  const q = a.search?.toLowerCase();
  let materials: unknown = null;
  if (file && a.only !== "palette") {
    const own = !!store && !a.materialFile;    // the open project's own file: its base can resolve it, and its voxels can be counted
    const base = store?.scene.base;
    const m = await levels.materials(file, own ? base?.key : undefined, own ? base?.source : undefined).catch((e: unknown) => ({ file, names: null as string[] | null, error: e instanceof Error ? e.message : String(e) }));
    const counts = new Map<number, number>();
    if (own) for (const g of grids(store!)) for (const w of g.words) if (isSolid(w)) counts.set(material(w), (counts.get(material(w)) ?? 0) + 1);
    if (!m.names) materials = { file, error: `the server could not read the material file: ${(m as { error?: string }).error}` };
    else materials = {
      file: m.file,
      list: m.names.map((name, index) => ({ index, name, ...(own ? { solidVoxels: counts.get(index) ?? 0 } : {}), colour: hex(materialTint(index)) }))
        .filter((e) => !q || e.name.toLowerCase().includes(q)),
      note: `colour is the tint render_topdown and render_side use${own ? "; solidVoxels counts the open project" : ""}`,
    };
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
      },
    },
    handler: (s, a) => renderSide(s, a),
  },
  {
    name: "query_ground",
    description: "The ground at a column (x, z): the top of the highest solid voxel (map and world y, the frame and the material), the y to stand on, whether it is under water, and every solid stretch of the column over all frames (so bridges and overhangs show; `below` asks for the ground under a given height).",
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
];
