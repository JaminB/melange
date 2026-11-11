// Read-only views of the level for the inspect tools: solid spans of a (x, z) column across every voxel frame, a height
// field over a region, and the level's markers (spawns, objects, mine factory). Map units throughout (1 voxel = 1 map unit).
import { cornerOffset, type EditorStore, type Vec3 } from "../core";
import { inside, isSolid, material, type Grid } from "./voxel";

/** A solid stretch of one frame's column: from `bottom` up to `top` (map y), the material of its top voxel. */
export interface Span { top: number; bottom: number; frame: number; material: number; }

const EPS = 1e-6;
/** True when the grid is moved and scaled along the axes but not rotated (the game's moved frames, and every added block
 * whatever its scale), so a map column is exactly one grid column. */
const upright = (g: Grid) => {
  const m = g.toMap;
  return m[0] > EPS && m[5] > EPS && m[10] > EPS &&
    Math.abs(m[1]) < EPS && Math.abs(m[2]) < EPS && Math.abs(m[4]) < EPS && Math.abs(m[6]) < EPS && Math.abs(m[8]) < EPS && Math.abs(m[9]) < EPS;
};

/** Solid spans of one grid at the map column (px, pz), highest first; only the highest when `topOnly`. A grid that is not
 * rotated (moved, or scaled along its axes, as every added block) is read cell by cell (exact tops); a rotated one is
 * sampled every half voxel (tops good to 0.25). */
export function columnSpans(g: Grid, px: number, pz: number, topOnly = false): Span[] {
  const b = g.bounds;
  if (px < b.min[0] || px >= b.max[0] || pz < b.min[2] || pz >= b.max[2]) return [];
  const out: Span[] = [];
  const [X, Y] = g.size;
  if (upright(g)) {
    const m = g.toMap;
    // the column is inside the bounds, so its cell is too: a point a hair under a block's far edge (x -40 - 1e-12 on a
    // skin tile that ends at -40) rounded up past the last cell while the tile beyond starts only at -40, and the column
    // read as no ground (check_trail's and check_holes' false holes on skin-tile edges, Caldera v2 round 2)
    const cx = Math.min(X - 1, Math.max(0, Math.floor((px - m[3]) / m[0] + 1e-9)));
    const cz = Math.min(g.size[2] - 1, Math.max(0, Math.floor((pz - m[11]) / m[10] + 1e-9)));
    if (!inside(g.size, cx, 0, cz)) return out;
    const at = (cz * X + cx) * Y;
    // An added block's corner offsets move the whole column up (draped blocks: heightfield_drape).
    const lift = g.frame.corners ? cornerOffset(g.frame, (px - m[3]) / m[0], (pz - m[11]) / m[10]) * m[5] : 0;
    for (let y = Y - 1; y >= 0; y--) {
      if (!isSolid(g.words[at + y])) continue;
      const top = m[7] + (y + 1) * m[5] + lift;
      let lo = y;
      while (lo > 0 && isSolid(g.words[at + lo - 1])) lo--;
      out.push({ top, bottom: m[7] + lo * m[5] + lift, frame: g.id, material: material(g.words[at + y]) });

      if (topOnly) break;
      y = lo;
    }
    return out;
  }
  let open: Span | null = null;
  for (let y = Math.ceil(b.max[1] * 2) / 2 - 0.25; y > b.min[1]; y -= 0.5) {
    const q = g.fromMap;
    const gx = Math.floor(q[0] * px + q[1] * y + q[2] * pz + q[3]), gy = Math.floor(q[4] * px + q[5] * y + q[6] * pz + q[7]), gz = Math.floor(q[8] * px + q[9] * y + q[10] * pz + q[11]);
    const w = inside(g.size, gx, gy, gz) ? g.words[(gz * X + gx) * Y + gy] : 0;
    if (isSolid(w)) {
      if (!open) open = { top: y + 0.25, bottom: y - 0.25, frame: g.id, material: material(w) };
      else open.bottom = y - 0.25;
    } else if (open) {
      out.push(open);
      open = null;
      if (topOnly) return out;
    }
  }
  if (open) out.push(open);
  return out;
}

/** Spans of a column over the given grids, highest top first. `below`: only ground whose top is at or under this map y
 * (spans straddling it are clipped to it). */
export function spansAt(gs: Grid[], px: number, pz: number, below = Infinity): Span[] {
  const out: Span[] = [];
  for (const g of gs)
    for (const s of columnSpans(g, px, pz)) {
      if (s.bottom >= below) continue;
      out.push(s.top > below ? { ...s, top: below } : s);
    }
  return out.sort((a, b) => b.top - a.top);
}

export interface Region { x0: number; z0: number; x1: number; z1: number; }   // map units, x0 < x1, z0 < z1

/** The xz box of every voxel frame, or null with none. */
export function extentXZ(gs: Grid[]): Region | null {
  if (!gs.length) return null;
  const r: Region = { x0: Infinity, z0: Infinity, x1: -Infinity, z1: -Infinity };
  for (const g of gs) {
    r.x0 = Math.min(r.x0, g.bounds.min[0]); r.x1 = Math.max(r.x1, g.bounds.max[0]);
    r.z0 = Math.min(r.z0, g.bounds.min[2]); r.z1 = Math.max(r.z1, g.bounds.max[2]);
  }
  return r;
}

/** Ground over a region: one sample per `step` x `step` map units, taken at the cell's centre. */
export interface Field {
  x0: number; z0: number; step: number; w: number; h: number;
  top: Float32Array;       // highest solid top (map y) per cell; -Infinity where nothing is solid
  frame: Int32Array;       // frame id of that top
  mat: Int16Array;         // its material index; -1 where nothing is solid
  min: number; max: number;   // lowest and highest top over the solid cells (NaN with none)
  solid: number;           // cells with ground
}

export function sampleField(gs: Grid[], r: Region, step: number): Field {
  const w = Math.max(1, Math.ceil((r.x1 - r.x0) / step)), h = Math.max(1, Math.ceil((r.z1 - r.z0) / step));
  const f: Field = { x0: r.x0, z0: r.z0, step, w, h, top: new Float32Array(w * h).fill(-Infinity), frame: new Int32Array(w * h), mat: new Int16Array(w * h).fill(-1),
    min: NaN, max: NaN, solid: 0 };
  for (const g of gs) {
    const b = g.bounds;
    if (b.max[0] <= r.x0 || b.min[0] >= r.x1 || b.max[2] <= r.z0 || b.min[2] >= r.z1) continue;
    const i0 = Math.max(0, Math.floor((b.min[0] - r.x0) / step)), i1 = Math.min(w - 1, Math.floor((b.max[0] - r.x0) / step));
    const j0 = Math.max(0, Math.floor((b.min[2] - r.z0) / step)), j1 = Math.min(h - 1, Math.floor((b.max[2] - r.z0) / step));
    for (let j = j0; j <= j1; j++)
      for (let i = i0; i <= i1; i++) {
        const s = columnSpans(g, r.x0 + (i + 0.5) * step, r.z0 + (j + 0.5) * step, true)[0];
        if (s && s.top > f.top[j * w + i]) {
          f.top[j * w + i] = s.top;
          f.frame[j * w + i] = s.frame;
          f.mat[j * w + i] = s.material;
        }
      }
  }
  let lo = Infinity, hi = -Infinity;
  for (const t of f.top) if (t > -Infinity) { f.solid++; if (t < lo) lo = t; if (t > hi) hi = t; }
  if (f.solid) { f.min = lo; f.max = hi; }
  return f;
}

export type MarkerKind = "spawn" | "crate" | "telepad" | "trigger" | "minefactory" | "object";
export interface Marker { kind: MarkerKind; name: string; detail: number; pos: Vec3; }   // map units

/** Spawns, the level's objects (crates, telepads, triggers, the mine factory) and other placed objects, as the Erg panel
 * positions them. */
export function markers(store: EditorStore): Marker[] {
  const scene = store.scene, out: Marker[] = [];
  const at = (d: (typeof scene.details)[number]) => store.frames.detailWorld(d);
  const knots = new Map(scene.details.filter((d) => d.src === null).map((d) => [d.name, d]));
  const objectKnots = new Set<string>();
  for (const o of scene.objects ?? []) {
    objectKnots.add(o.knot);
    const d = knots.get(o.knot);
    if (d) out.push({ kind: o.type, name: o.knot, detail: d.id, pos: at(d) });
  }
  for (const d of scene.details) {
    if (d.role === "spawn") out.push({ kind: "spawn", name: d.name, detail: d.id, pos: at(d) });
    else if (d.role === "object" && !(d.src === null && objectKnots.has(d.name))) out.push({ kind: "object", name: d.name, detail: d.id, pos: at(d) });
  }
  return out;
}

/** The level's water as map y, or null when the level keeps its own (not known to the editor). */
export const waterMap = (store: EditorStore): number | null => (store.scene.water.level === null ? null : store.scene.water.level / 20);

