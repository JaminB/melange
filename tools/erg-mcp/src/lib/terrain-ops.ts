// The terrain group's engine: brush strokes, paths, blocks, clearing, raw fills and the surround, built on the panel's own
// brush (terrain/brush.ts strokeChanges), block planner (blocks.ts) and surround painter (surround.ts dabChanges), so a
// tool edits exactly as a mouse stroke does. Everything here edits through VoxelEdit or store.exec: one undo step each.
import {
  RemoveFrame, SetLevel, SurroundStroke, MAX_BRUSH, SURROUND_SIDE, apply, blockCentre, cellBudget, dabChanges, gridFrames, planBlock, quantizeHeight,
  strokeChanges, validEdit, carved, filled, isSolid, type Anchor, type Brush, type BrushMaterial, type BrushMode, type BrushShape, type EditorStore,
  type GridFrame, type SurroundBrush, type SurroundMode, type Vec3,
} from "../core";
import { ToolError } from "../registry";
import { VoxelEdit, toMap, tidy, toWorld, usage, voxelIndex, grids } from "./voxel";

export type Units = "world" | "map" | "grid";

/** A point or size in the given units to map units (grid: needs the frame's grid). */
export function pointToMap(p: Vec3, units: Units, g?: GridFrame): Vec3 {
  if (units === "map") return p;
  if (units === "world") return toMap(p);
  if (!g) throw new ToolError("units 'grid' needs a frame");
  return apply(g.toWorld, p);
}

export const frameGrids = (store: EditorStore): GridFrame[] => gridFrames(store.scene, store.voxels, (id) => store.voxelRef(id));

const nameOf = (store: EditorStore, id: number) => store.scene.frames.find((f) => f.id === id)?.name ?? String(id);

function gridById(gs: GridFrame[], id: number): GridFrame {
  const g = gs.find((x) => x.frame.id === id);
  if (!g) throw new ToolError(`frame ${id} has no voxels (describe_level lists the voxel frames)`);
  return g;
}

const cellOf = (g: GridFrame, mapPt: Vec3): Vec3 => apply(g.toLocal, mapPt);
const within = (g: GridFrame, p: Vec3, margin: number) =>
  [0, 1, 2].every((a) => p[a] >= -margin && p[a] < g.frame.size[a] + margin);

/** The frame a stroke starts on: `frameId`, or the voxel frame holding the point (a solid voxel under it first, then the
 * smallest frame); a fill may start in the empty voxel just outside a frame, so the search widens by one voxel. */
export function pickAnchor(store: EditorStore, gs: GridFrame[], mapPt: Vec3, frameId: number | undefined, widen: boolean): GridFrame {
  if (frameId !== undefined) return gridById(gs, frameId);
  const hits = (margin: number) => gs.filter((g) => within(g, cellOf(g, mapPt), margin));
  let c = hits(0);
  if (!c.length && widen) c = hits(1);
  if (!c.length) throw new ToolError("no voxel frame holds that point: pass `frame` (describe_level gives each frame's bounds)");
  const solidAt = (g: GridFrame) => {
    const q = cellOf(g, mapPt).map(Math.floor) as Vec3;
    return within(g, q, 0) && isSolid(store.voxels.get(g.ref)![voxelIndex(g.frame.size as Vec3, q[0], q[1], q[2])]);
  };
  const vol = (g: GridFrame) => g.frame.size[0] * g.frame.size[1] * g.frame.size[2];
  return c.sort((a, b) => Number(solidAt(b)) - Number(solidAt(a)) || vol(a) - vol(b) || a.frame.id - b.frame.id)[0];
}

// ------------------------------------------------------------------ strokes

export interface StrokeSpec {
  mode: Extract<BrushMode, "carve" | "fill" | "paint">;
  shape: BrushShape;
  size: Vec3;                     // voxels of the anchor frame
  material?: BrushMaterial;
}

export function brushOf(s: StrokeSpec): Brush {
  if (!s.size.every((v) => Number.isInteger(v) && v >= 1 && v <= MAX_BRUSH)) throw new ToolError(`each brush size is a whole number of voxels, 1-${MAX_BRUSH}`);
  let m: BrushMaterial = s.material ?? 0;
  if (s.mode !== "carve") {
    if (s.material === undefined) throw new ToolError(`${s.mode} needs a material (0-63)`);
    if (m === "column" && s.mode !== "fill") throw new ToolError("material 'column' is for fill only");
    if (typeof m === "number" && !(Number.isInteger(m) && m >= 0 && m <= 63)) throw new ToolError("material is a whole number 0-63");
  } else m = 0;
  return { mode: s.mode, shape: s.shape, size: s.size, material: m };
}

export interface StrokeResult {
  mode: string; shape: string; anchorFrame: { id: number; name: string }; dabs: number;
  changed: number; frames: { id: number; name: string; changed: number }[]; label?: string; dryRun?: boolean;
}

const snapC = (c: Vec3): Vec3 => c.map((v) => Math.floor(v) + 0.5) as Vec3;

/** Dab positions along a polyline in map units, `spacing` apart, the first point and the end included. */
export function pathPoints(points: Vec3[], spacing: number, maxDabs = 4000): Vec3[] {
  const out: Vec3[] = [points[0]];
  let travelled = 0, nextAt = spacing;
  for (let i = 1; i < points.length; i++) {
    const a = points[i - 1], b = points[i];
    const len = Math.hypot(b[0] - a[0], b[1] - a[1], b[2] - a[2]);
    for (; len && nextAt <= travelled + len + 1e-9; nextAt += spacing) {
      const t = (nextAt - travelled) / len;
      out.push([a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t]);
      if (out.length > maxDabs) throw new ToolError(`the path needs more than ${maxDabs} dabs: raise spacing or shorten it`);
    }
    travelled += len;
  }
  const last = points[points.length - 1], end = out[out.length - 1];
  if (Math.hypot(last[0] - end[0], last[1] - end[1], last[2] - end[2]) > 1e-9) out.push(last);
  return out;
}

/** Index in a frame's words -> grid cell. */
function cellIndex(size: Vec3, i: number): Vec3 {
  const y = i % size[1], r = (i - y) / size[1], x = r % size[0];
  return [x, y, (r - x) / size[0]];
}

/** Applies `centres` (map units) with the brush, each dab seeing the one before, anchored on `anchor` (fill and paint touch
 * only that frame, carve reaches every frame the brush overlaps). One undo step. */
export function stroke(store: EditorStore, spec: StrokeSpec, centres: Vec3[], anchor: GridFrame, o: { snap: boolean; dryRun?: boolean; label?: string }): StrokeResult {
  const brush = brushOf(spec);
  const gs = frameGrids(store);
  const live = new Map(store.voxels);               // working copy, cloned per blob on first write
  const cloned = new Set<number>();
  const touched = new Map<number, Set<number>>();   // ref -> word indices
  let last = "", dabs = 0;
  for (const m of centres) {
    let c = apply(anchor.toLocal, m);
    if (o.snap) c = snapC(c);
    const key = c.join();
    if (key === last) continue;                     // the panel steps only when the brush moves to another voxel
    last = key;
    dabs++;
    const a: Anchor = { grid: anchor, center: c };
    for (const [ref, changes] of strokeChanges(gs, live, a, brush)) {
      if (!cloned.has(ref)) { live.set(ref, live.get(ref)!.slice()); cloned.add(ref); }
      const w = live.get(ref)!, t = touched.get(ref) ?? touched.set(ref, new Set()).get(ref)!;
      for (const [i, [, after]] of changes) { w[i] = after; t.add(i); }
    }
  }
  const edit = new VoxelEdit(store);
  const per = new Map<number, number>();
  const byRef = new Map(gs.map((g) => [g.ref, g]));
  const baseOf = new Map(grids(store).map((g) => [g.ref, g.base]));
  for (const [ref, idx] of touched) {
    const g = byRef.get(ref)!, orig = store.voxels.get(ref)!, w = live.get(ref)!, base = baseOf.get(ref)!;
    for (const i of idx) {
      if (w[i] === orig[i]) continue;
      if (!validEdit(base[i], w[i])) throw new ToolError(`internal: 0x${w[i].toString(16)} is not a valid edit of the base word 0x${base[i].toString(16)}`);
      const [x, y, z] = cellIndex(g.frame.size as Vec3, i);
      edit.set(g.frame.id, x, y, z, w[i]);
      per.set(g.frame.id, (per.get(g.frame.id) ?? 0) + 1);
    }
  }
  const label = o.label ?? `${brush.mode[0].toUpperCase()}${brush.mode.slice(1)} terrain`;
  const why = edit.check();
  if (why) throw new ToolError(why);
  const changed = edit.size;
  if (!o.dryRun) edit.commit(label);
  return {
    mode: brush.mode, shape: brush.shape, anchorFrame: { id: anchor.frame.id, name: anchor.frame.name }, dabs, changed,
    frames: [...per].sort((a, b) => b[1] - a[1]).slice(0, 60).map(([id, n]) => ({ id, name: nameOf(store, id), changed: n })),
    ...(o.dryRun ? { dryRun: true } : { label }),
  };
}

// ------------------------------------------------------------------ blocks

export interface BlockSpec { size: Vec3; material: number; at?: Vec3; center?: Vec3; units: Units; scale?: number; dryRun?: boolean; }

export function addBlock(store: EditorStore, s: BlockSpec) {
  if (!!s.at === !!s.center) throw new ToolError("give exactly one of `at` (the point the block rests on: x and z are its centre, y its bottom) or `center`");
  if (!(Number.isInteger(s.material) && s.material >= 0 && s.material <= 63)) throw new ToolError("material is a whole number 0-63");
  const to = (p: Vec3) => (s.units === "world" ? toMap(p) : p);
  if (s.units === "grid") throw new ToolError("blocks are placed in map or world units");
  const scale = s.scale ?? 1;
  const bottom: Vec3 = s.at ? to(s.at) : (() => { const c = to(s.center!); return [c[0], c[1] - (s.size[1] * scale) / 2, c[2]] as Vec3; })();
  const centre = blockCentre(store.scene, bottom, s.size, scale);
  if (!centre) throw new ToolError("This level has no Scene frame to add blocks under");
  const cmd = planBlock(store.scene, store.voxels, store.freshRef(), centre, s.size, s.material, scale);
  if (typeof cmd === "string") throw new ToolError(cmd);
  if (s.dryRun) {
    // the cost before it is committed: the block's whole box counts against the game's cells
    const before = cellBudget(store.scene), cost = s.size[0] * s.size[1] * s.size[2];
    const w = store.frames.worldOf(cmd.frame.parent!), t: Vec3 = w ? [w[3], w[7], w[11]] : [0, 0, 0];
    const half = s.size.map((v) => (v * scale) / 2), c = cmd.frame.pos.map((v, i) => v + t[i]);
    return {
      dryRun: true, size: s.size, scale, material: s.material, voxels: cost,
      cells: { cost, before: before.used, after: before.used + cost, max: before.max, left: before.free - cost },
      bounds: { map: { min: tidy([c[0] - half[0], c[1] - half[1], c[2] - half[2]]), max: tidy([c[0] + half[0], c[1] + half[1], c[2] + half[2]]) } },
      covers: { mapUnits: tidy(s.size.map((v) => v * scale) as Vec3), cellsPerMapUnit3: Number((1 / scale ** 3).toFixed(4)) },
    };
  }
  store.exec(cmd);
  const g = frameGrids(store).find((x) => x.frame.id === cmd.frame.id)!;
  return {
    id: cmd.frame.id, name: cmd.frame.name, size: s.size, ...(scale !== 1 ? { scale } : {}), material: s.material, voxels: s.size[0] * s.size[1] * s.size[2],
    cells: cellBudget(store.scene),
    bounds: { map: { min: tidy(g.bounds.min), max: tidy(g.bounds.max) }, world: { min: tidy(toWorld(g.bounds.min)), max: tidy(toWorld(g.bounds.max)) } },
    note: "the block's corner is snapped to whole Scene-frame units; sculpt or fill_box_raw can shape it",
  };
}

export function removeBlock(store: EditorStore, id: number) {
  const f = store.scene.frames.find((x) => x.id === id);
  if (!f) throw new ToolError(`no frame ${id}`);
  if (!f.new) throw new ToolError(`frame ${id} (${f.name}) is part of the base map; only blocks added in this project can be removed (clear_base_terrain carves base terrain away)`);
  const kids = store.scene.frames.filter((x) => x.parent === id).length + store.scene.details.filter((d) => d.frame === id).length;
  if (kids) throw new ToolError(`block ${id} has ${kids} child frames or details; move or delete them first`);
  store.exec(new RemoveFrame(store.voxels, id));
  return { removed: id, name: f.name, blocksLeft: store.scene.frames.filter((x) => x.new).length };
}

// ------------------------------------------------------------------ boxes

export interface Box3 { min: Vec3; max: Vec3; }

/** Cells of `g` whose centre lies in the box (min inclusive, max exclusive), box given in `units`. */
export function* cellsIn(g: GridFrame, box: Box3 | undefined, units: Units): Generator<number> {
  const size = g.frame.size as Vec3;
  let lo: Vec3 = [0, 0, 0], hi: Vec3 = [size[0] - 1, size[1] - 1, size[2] - 1];
  let test: ((c: Vec3) => boolean) | null = null;
  if (box) {
    if (units === "grid") {
      lo = [0, 1, 2].map((a) => Math.max(0, Math.ceil(box.min[a] - 0.5))) as Vec3;
      hi = [0, 1, 2].map((a) => Math.min(size[a] - 1, Math.ceil(box.max[a] - 0.5) - 1)) as Vec3;
    } else {
      const mn = pointToMap(box.min, units), mx = pointToMap(box.max, units);
      test = (c) => { const m = apply(g.toWorld, c); return [0, 1, 2].every((a) => m[a] >= mn[a] && m[a] < mx[a]); };
    }
  }
  for (let z = lo[2]; z <= hi[2]; z++)
    for (let x = lo[0]; x <= hi[0]; x++)
      for (let y = lo[1]; y <= hi[1]; y++) if (!test || test([x + 0.5, y + 0.5, z + 0.5])) yield voxelIndex(size, x, y, z);
}

export interface FillSpec { frame: number; box?: Box3; units: Units; state: "solid" | "empty"; material?: number; word?: number; only?: "empty" | "solid"; wipe?: boolean; dryRun?: boolean; }

/** Writes a word into every cell of a box, with the server's rule for the word (validEdit against the base's). */
export function fillBox(store: EditorStore, s: FillSpec) {
  const g = grids(store).find((x) => x.id === s.frame);
  if (!g) throw new ToolError(`frame ${s.frame} has no voxels`);
  const gf = gridById(frameGrids(store), s.frame);
  let word: number | null = s.word ?? null;
  if (word === null) {
    if (s.state === "solid") {
      if (s.material === undefined) throw new ToolError("a solid fill needs a material (0-63) or a raw `word`");
      word = filled(s.material);
    } else word = s.wipe ? 0 : null;     // null: carve each cell, keeping its material bits
  }
  if (s.material !== undefined && !(Number.isInteger(s.material) && s.material >= 0 && s.material <= 63)) throw new ToolError("material is a whole number 0-63");
  const e = new VoxelEdit(store);
  let seen = 0;
  for (const i of cellsIn(gf, s.box, s.units)) {
    seen++;
    const cur = g.words[i], solid = isSolid(cur);
    if (s.only === "empty" && solid) continue;
    if (s.only === "solid" && !solid) continue;
    const next = word ?? carved(cur);
    if (next === cur) continue;
    if (!validEdit(g.base[i], next)) throw new ToolError(`0x${next.toString(16)} is not a valid edit of the base word 0x${g.base[i].toString(16)} (bits 8-23 must be zero or the base's, bits 24-31 clear, solid bits 0 or 3)`);
    const [x, y, z] = cellIndex(g.size, i);
    e.set(g.id, x, y, z, next);
  }
  const why = e.check();
  if (why) throw new ToolError(why);
  const changed = e.size;
  if (!s.dryRun) e.commit(s.state === "solid" || s.word !== undefined ? "Fill voxels" : "Clear voxels");
  return { frame: g.id, name: g.frame.name, cellsInBox: seen, changed, ...(s.dryRun ? { dryRun: true } : {}) };
}

// ------------------------------------------------------------------ clearing the base

export interface ClearSpec { frames?: number[]; search?: string; box?: Box3; units: Units; includeBlocks?: boolean; wipe?: boolean; dryRun?: boolean; }

/** Carves every solid voxel of the chosen frames (the base map's by default, never added blocks unless asked) that lies in
 * the box, if one is given. Carving keeps each voxel's material bits as the brush does; `wipe` writes empty words (0), which
 * make shorter runs in the patch. */
export function clearBase(store: EditorStore, s: ClearSpec) {
  const gs = frameGrids(store);
  const needle = s.search?.toLowerCase();
  let chosen = gs.filter((g) => (s.includeBlocks || !g.frame.new));
  if (s.frames) {
    const ids = new Set(s.frames);
    for (const id of ids) if (!gs.some((g) => g.frame.id === id)) throw new ToolError(`frame ${id} has no voxels`);
    chosen = gs.filter((g) => ids.has(g.frame.id));
  }
  if (needle) chosen = chosen.filter((g) => g.frame.name.toLowerCase().includes(needle));
  const e = new VoxelEdit(store);
  const per: { id: number; name: string; cleared: number }[] = [];
  const byId = new Map(grids(store).map((g) => [g.id, g]));
  for (const gf of chosen) {
    const g = byId.get(gf.frame.id)!;
    let n = 0;
    for (const i of cellsIn(gf, s.box, s.units)) {
      const cur = g.words[i];
      if (!isSolid(cur)) continue;
      const next = s.wipe ? 0 : carved(cur);
      if (!validEdit(g.base[i], next)) throw new ToolError(`internal: 0x${next.toString(16)} is not a valid edit of the base word 0x${g.base[i].toString(16)}`);
      const [x, y, z] = cellIndex(g.size, i);
      e.set(g.id, x, y, z, next);
      n++;
    }
    if (n) per.push({ id: g.id, name: g.frame.name, cleared: n });
  }
  const why = e.check();
  if (why) throw new ToolError(why);
  const changed = e.size;
  if (!s.dryRun) e.commit("Clear terrain");
  return { framesConsidered: chosen.length, framesCleared: per.length, voxelsCleared: changed, frames: per.sort((a, b) => b.cleared - a.cleared).slice(0, 60), ...(s.dryRun ? { dryRun: true } : {}) };
}

// ------------------------------------------------------------------ the surround

export const SURROUND_CELLS = SURROUND_SIDE * SURROUND_SIDE;

let strokes = 1_000_000;

/** Switches the level to a painted surround (the panel's "Paint the surround" button); false when it already was. */
export function enableSurroundPaint(store: EditorStore): boolean {
  if (store.scene.hmp.mode === "paint") return false;
  if (!store.canPaintSurround) throw new ToolError("the base level's surround did not load; reopen the project to paint it");
  store.exec(new SetLevel({ hmp: "paint" }, "Paint the surround"));
  return true;
}

export interface SurroundDabs { mode: SurroundMode; points: [number, number][]; radius: number; strength: number; level: number; passes: number; spacing: number; }

/** The panel's dabs (SurroundPainter.step) along `points` (cell units, 0-100), as one undo step. */
export function surroundDabs(store: EditorStore, s: SurroundDabs) {
  const brush: SurroundBrush = {
    mode: s.mode, radius: Math.min(25, Math.max(1, Math.round(s.radius))), strength: Math.min(1, Math.max(0.05, s.strength)), level: quantizeHeight(s.level),
  };
  const centres = pathPoints(s.points.map(([x, y]) => [x, y, 0] as Vec3), s.spacing);
  const stroke = ++strokes, label = `${brush.mode[0].toUpperCase()}${brush.mode.slice(1)} surround`;
  let cells = 0;
  const touched = new Set<number>();
  const before = store.surround.heights.slice();
  for (let p = 0; p < s.passes; p++)
    for (const [cx, cy] of centres) {
      const c = dabChanges(store.surround.heights, cx, cy, brush);
      if (!c.size) continue;
      for (const i of c.keys()) touched.add(i);
      store.exec(new SurroundStroke(label, stroke, store.surround, c), true);
    }
  cells = touched.size;
  let changed = 0;
  for (const i of touched) if (before[i] !== store.surround.heights[i]) changed++;
  return { dabs: centres.length * s.passes, cellsTouched: cells, cellsChanged: changed, label, brush };
}

const FORMULA_NAMES = ["abs", "acos", "asin", "atan", "atan2", "ceil", "cos", "exp", "floor", "log", "max", "min", "pow", "round", "sign", "sin", "sqrt", "tan", "hypot", "PI", "E"];
const VARIABLES = ["x", "y", "u", "v", "h"];

/** Compiles a formula of x, y (cell 0-99), u, v (0-1 across the grid) and h (the current height). Only arithmetic, comparison,
 * ?: and the Math functions listed are allowed; it is parsed here, never handed to eval or Function. */
export function compileFormula(src: string): (x: number, y: number, u: number, v: number, h: number) => number {
  if (src.length > 500) throw new ToolError("formula: at most 500 characters");
  if (!/^[0-9A-Za-z_\s+\-*/%(),.?:<>=!&|]*$/.test(src)) throw new ToolError("formula: only digits, names, + - * / % ( ) , . ? : < > = ! & | are allowed");
  if (/[A-Za-z_)\]]\s*\./.test(src)) throw new ToolError("formula: member access is not allowed; write sin(x), not Math.sin(x)");
  const fn = parseFormula(src);
  return (x, y, u, v, h) => fn([x, y, u, v, h]);
}

type Formula = (vars: number[]) => number;
const FORMULA_CONSTANTS: Record<string, number> = { PI: Math.PI, E: Math.E };
const FORMULA_OPS = ["===", "!==", "**", "==", "!=", "<=", ">=", "&&", "||", "+", "-", "*", "/", "%", "<", ">", "!", "?", ":", "(", ")", ","];

/** Parses a formula into a closure tree (never eval) with JavaScript's precedence and number semantics: ?:, || &&,
 * == != === !==, < <= > >=, + -, * / %, unary - + !, ** (right associative), calls of the Math functions above. */
function parseFormula(src: string): Formula {
  const toks: { t: string; at: number }[] = [];
  for (let i = 0; i < src.length;) {
    if (/\s/.test(src[i])) { i++; continue; }
    const m = /^(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?|^[A-Za-z_][A-Za-z0-9_]*/.exec(src.slice(i));
    const t = m ? m[0] : FORMULA_OPS.find((o) => src.startsWith(o, i));
    if (!t) throw new ToolError(`formula: unexpected '${src[i]}' at character ${i + 1}`);
    toks.push({ t, at: i });
    i += t.length;
  }
  let p = 0, depth = 0;
  const peek = () => toks[p]?.t;
  const fail = (why: string): never => { throw new ToolError(`formula: ${why} at character ${(toks[p]?.at ?? src.length) + 1}`); };
  const take = (t: string) => { if (peek() !== t) fail(`expected '${t}'`); p++; };
  const truthy = (v: number) => v !== 0 && !Number.isNaN(v);
  const nest = <T>(f: () => T): T => { if (++depth > 64) fail("it nests deeper than 64"); const r = f(); depth--; return r; };

  const binary = (next: () => Formula, ops: Record<string, (a: number, b: number) => number>) => (): Formula => {
    let left = next();
    while (peek() !== undefined && Object.hasOwn(ops, peek()!)) {
      const f = ops[toks[p++].t], l = left, r = next();
      left = (v) => f(l(v), r(v));
    }
    return left;
  };
  const cond = (): Formula => nest(() => {
    const c = or();
    if (peek() !== "?") return c;
    p++;
    const a = cond();
    take(":");
    const b = cond();
    return (v) => (truthy(c(v)) ? a(v) : b(v));
  });
  const or = (): Formula => {
    let left = and();
    while (peek() === "||") { p++; const l = left, r = and(); left = (v) => { const a = l(v); return truthy(a) ? a : r(v); }; }
    return left;
  };
  const and = (): Formula => {
    let left = eq();
    while (peek() === "&&") { p++; const l = left, r = eq(); left = (v) => { const a = l(v); return truthy(a) ? r(v) : a; }; }
    return left;
  };
  const unary = (): Formula => nest(() => {
    const t = peek();
    if (t === "-") { p++; const a = unary(); return (v) => -a(v); }
    if (t === "+") { p++; return unary(); }
    if (t === "!") { p++; const a = unary(); return (v) => (truthy(a(v)) ? 0 : 1); }
    const base = atom();
    if (peek() !== "**") return base;
    p++;
    const ex = unary();
    return (v) => base(v) ** ex(v);
  });
  const mul = binary(unary, { "*": (a, b) => a * b, "/": (a, b) => a / b, "%": (a, b) => a % b });
  const add = binary(mul, { "+": (a, b) => a + b, "-": (a, b) => a - b });
  const rel = binary(add, { "<": (a, b) => +(a < b), "<=": (a, b) => +(a <= b), ">": (a, b) => +(a > b), ">=": (a, b) => +(a >= b) });
  const eq = binary(rel, { "==": (a, b) => +(a === b), "===": (a, b) => +(a === b), "!=": (a, b) => +(a !== b), "!==": (a, b) => +(a !== b) });
  function atom(): Formula {
    const t = peek();
    if (t === undefined) return fail("the formula ends early");
    if (t === "(") { p++; const inner = cond(); take(")"); return inner; }
    if (/^[\d.]/.test(t)) { p++; const n = Number(t); return () => n; }
    if (!/^[A-Za-z_]/.test(t)) return fail(`unexpected '${t}'`);
    p++;
    const vi = VARIABLES.indexOf(t);
    if (vi >= 0) return (v) => v[vi];
    if (Object.hasOwn(FORMULA_CONSTANTS, t)) { const n = FORMULA_CONSTANTS[t]; return () => n; }
    if (!FORMULA_NAMES.includes(t)) return fail(`unknown name '${t}' (variables ${VARIABLES.join(", ")}; functions ${FORMULA_NAMES.join(", ")})`);
    if (peek() !== "(") return fail(`${t} is a function: write ${t}(...)`);
    const f = (Math as unknown as Record<string, (...a: number[]) => number>)[t];
    p++;
    const args: Formula[] = [];
    if (peek() !== ")") {
      args.push(cond());
      while (peek() === ",") { p++; args.push(cond()); }
    }
    take(")");
    return (v) => f(...args.map((a) => a(v)));
  }

  const fn = cond();
  if (p < toks.length) fail(`unexpected '${toks[p].t}'`);
  return fn;
}

export interface SetGrid { heights?: number[] | number[][]; formula?: string; region?: { x0: number; y0: number; x1: number; y1: number }; mix: number; }

/** Sets heights from a 100x100 array (rows of y, then x, or flat y*100+x) or a formula, over a region (cells x0..x1, y0..y1
 * inclusive), mixed `mix` of the way from the current height. Heights are quantised to 1/256 as the painter does. */
export function setSurroundGrid(store: EditorStore, s: SetGrid) {
  if (!!s.heights === !!s.formula) throw new ToolError("give exactly one of `heights` or `formula`");
  const N = SURROUND_SIDE;
  let flat: Float64Array | null = null;
  if (s.heights) {
    const h = s.heights;
    const rows = Array.isArray(h[0]);
    if (rows) {
      if (h.length !== N || (h as number[][]).some((r) => !Array.isArray(r) || r.length !== N)) throw new ToolError(`heights: ${N} rows of ${N} numbers`);
      flat = Float64Array.from((h as number[][]).flat());
    } else {
      if (h.length !== SURROUND_CELLS) throw new ToolError(`heights: ${SURROUND_CELLS} numbers (or ${N} rows of ${N})`);
      flat = Float64Array.from(h as number[]);
    }
    if (flat.some((v) => !Number.isFinite(v))) throw new ToolError("heights: every value must be a finite number");
  }
  const f = s.formula ? compileFormula(s.formula) : null;
  const r = s.region ?? { x0: 0, y0: 0, x1: N - 1, y1: N - 1 };
  if (r.x0 > r.x1 || r.y0 > r.y1) throw new ToolError("region: x0 <= x1 and y0 <= y1");
  const H = store.surround.heights;
  const changes = new Map<number, [number, number]>();
  for (let y = Math.max(0, r.y0); y <= Math.min(N - 1, r.y1); y++)
    for (let x = Math.max(0, r.x0); x <= Math.min(N - 1, r.x1); x++) {
      const i = y * N + x, was = H[i];
      const target = flat ? flat[i] : f!(x, y, x / (N - 1), y / (N - 1), was);
      if (!Number.isFinite(target)) throw new ToolError(`formula gave ${target} at x=${x}, y=${y}`);
      const q = Math.fround(quantizeHeight(was + (Math.min(1, Math.max(0, target)) - was) * s.mix));
      if (q !== was) changes.set(i, [was, q]);
    }
  if (changes.size) store.exec(new SurroundStroke("Set surround", ++strokes, store.surround, changes));
  return { cellsChanged: changes.size };
}

export interface SurroundSummary { min: number; max: number; mean: number; grid: number[][]; step: number; }

/** Heights summarised: min, max, mean and the mean of each step x step block (rows are y). */
export function surroundSummary(store: EditorStore, step = 10): SurroundSummary {
  const H = store.surround.heights, N = SURROUND_SIDE;
  let min = Infinity, max = -Infinity, sum = 0;
  for (let i = 0; i < SURROUND_CELLS; i++) { min = Math.min(min, H[i]); max = Math.max(max, H[i]); sum += H[i]; }
  const grid: number[][] = [];
  for (let by = 0; by < N; by += step) {
    const row: number[] = [];
    for (let bx = 0; bx < N; bx += step) {
      let s = 0, n = 0;
      for (let y = by; y < Math.min(N, by + step); y++) for (let x = bx; x < Math.min(N, bx + step); x++) { s += H[y * N + x]; n++; }
      row.push(Math.round((s / n) * 1000) / 1000);
    }
    grid.push(row);
  }
  return { min, max, mean: Math.round((sum / SURROUND_CELLS) * 1e4) / 1e4, grid, step };
}

/** What patch space is left, for tool answers. */
export const patchLeft = (store: EditorStore) => {
  const u = usage(store);
  return { patchBytes: u.patchBytes.used, budget: u.patchBytes.budget, newFrames: u.newFrames.used, worstRuns: u.runsPerFrame.worst };
};
