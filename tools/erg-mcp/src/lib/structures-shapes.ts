// Structure geometry for the "structures" tool group: pure functions, no editor store. A structure is an ordered list of
// shapes. Each shape is a box with a hit test over a point in map units (the centre of a voxel cell), and an operation:
// fill (solid, with a material), carve (empty) or paint (change the material of a voxel that is solid). A cell takes the
// shapes in order, so a later carve opens a door in an earlier wall.
//
// Builders work in local coordinates: y up, the structure's origin on the floor (y = 0 is the level of the first solid
// layer), and lengths in voxels. transform() then yaws a structure about the origin and moves it into the level.
// Sides are compass names in map space: n = -z, s = +z, e = +x, w = -x.
import type { Vec3 } from "../core";

export type Op = "fill" | "carve" | "paint";
export type Side = "n" | "e" | "s" | "w";
export const SIDES: readonly Side[] = ["n", "e", "s", "w"];

export interface Shape {
  op: Op;
  mat: number;                               // 0-63; ignored by carve
  min: Vec3;                                 // bounding box (map units): hit() is false outside it
  max: Vec3;
  hit(x: number, y: number, z: number): boolean;
  part?: number;                             // the part (a wall, a tower) its voxels belong to when blocks are packed by part
  matAt?(x: number, y: number, z: number): number;   // fill and paint: the material per cell, instead of mat
  keep?: boolean;                            // fill: leave solid voxels as they are (only empty cells are filled)
}

export interface Plan {
  label: string;                             // the undo step's name
  shapes: Shape[];
  /** Facts about the structure for the tool's answer (resolved sizes, where the ramp ends, ...). */
  info: Record<string, unknown>;
  notes: string[];
}

/** The box that holds every shape. */
export function planBounds(shapes: Shape[]): { min: Vec3; max: Vec3 } {
  const min: Vec3 = [Infinity, Infinity, Infinity], max: Vec3 = [-Infinity, -Infinity, -Infinity];
  for (const s of shapes) for (let a = 0; a < 3; a++) {
    min[a] = Math.min(min[a], s.min[a]);
    max[a] = Math.max(max[a], s.max[a]);
  }
  return { min, max };
}

// ------------------------------------------------------------------ primitives

export const box = (op: Op, mat: number, min: Vec3, max: Vec3): Shape => ({
  op, mat, min, max, hit: (x, y, z) => x >= min[0] && x < max[0] && y >= min[1] && y < max[1] && z >= min[2] && z < max[2],
});

/** A vertical cylinder of radius r about (cx, cz) from y0 to y1. */
export const cylinder = (op: Op, mat: number, cx: number, cz: number, r: number, y0: number, y1: number): Shape => ({
  op, mat, min: [cx - r, y0, cz - r], max: [cx + r, y1, cz + r],
  hit: (x, y, z) => y >= y0 && y < y1 && (x - cx) * (x - cx) + (z - cz) * (z - cz) < r * r,
});

/** Two cells on, one off along a running coordinate: the crenellations. */
export const merlon = (u: number) => ((Math.floor(u) % 3) + 3) % 3 < 2;

// ------------------------------------------------------------------ material bands and trim

/** A material by height above the structure's floor (or its base line, for a wall): [from, to) in voxels. */
export interface MatBand { from: number; to: number; material: number; }

/** Trim: the topmost layer of the body (`top`, `topDepth` thick, default 1), the merlon row (`merlonCap`) and one stripe at
 * height `course.y` (`course.h` thick, default 1). */
export interface Trim { top?: number; topDepth?: number; merlonCap?: number; course?: { y: number; material: number; h?: number }; }

/** Material bands and trim for a structure's fills. Unset (or empty), the shapes are left exactly as they are. */
export interface MatStyle { bands?: MatBand[]; trim?: Trim; }

export const hasStyle = (st?: MatStyle): st is MatStyle =>
  !!st && (!!st.bands?.length || (!!st.trim && (st.trim.top !== undefined || st.trim.merlonCap !== undefined || st.trim.course !== undefined)));

/** The material of a fill cell at height `d` above the structure's floor. `top` is the height of the body's top (its top
 * layer is the voxel row under it), `cap` marks the merlon row. Precedence: course, then top (merlonCap on the merlons),
 * then the first band holding d, then the shape's own material. */
export function styleMat(mat: number, st: MatStyle, d: number, top: number | undefined, cap = false): number {
  const t = st.trim;
  if (t?.course && d >= t.course.y && d < t.course.y + (t.course.h ?? 1)) return t.course.material;
  if (cap) { if (t?.merlonCap !== undefined) return t.merlonCap; }
  else if (t?.top !== undefined && top !== undefined && d >= top - (t.topDepth ?? 1) && d < top) return t.top;
  if (st.bands) for (const b of st.bands) if (d >= b.from && d < b.to) return b.material;
  return mat;
}

/** The shape with its material styled by height above y = 0 (fills only; a shape without style comes back as it is). */
export function styled(s: Shape, st: MatStyle | undefined, top?: number, cap = false): Shape {
  if (!hasStyle(st) || s.op === "carve") return s;
  const base = s.matAt;
  return { ...s, matAt: (x, y, z) => styleMat(base ? base(x, y, z) : s.mat, st, y, top, cap) };
}

export function translate(shapes: Shape[], d: Vec3): Shape[] {
  return shapes.map((s) => ({
    op: s.op, mat: s.mat, min: [s.min[0] + d[0], s.min[1] + d[1], s.min[2] + d[2]], max: [s.max[0] + d[0], s.max[1] + d[1], s.max[2] + d[2]],
    hit: (x, y, z) => s.hit(x - d[0], y - d[1], z - d[2]), part: s.part, ...(s.keep ? { keep: true } : {}),
    ...(s.matAt ? { matAt: (x: number, y: number, z: number) => s.matAt!(x - d[0], y - d[1], z - d[2]) } : {}),
  }));
}

/** Shapes made in voxels of `k` map units, scaled about their origin into map units: a design whose walls are 2 voxels
 * thick keeps 2 voxels at any k, and its crenellations their 2-on-1-off rhythm. */
export function scaled(shapes: Shape[], k: number): Shape[] {
  if (k === 1) return shapes;
  return shapes.map((s) => ({
    op: s.op, mat: s.mat, min: [s.min[0] * k, s.min[1] * k, s.min[2] * k], max: [s.max[0] * k, s.max[1] * k, s.max[2] * k],
    hit: (x, y, z) => s.hit(x / k, y / k, z / k), part: s.part, ...(s.keep ? { keep: true } : {}),
    ...(s.matAt ? { matAt: (x: number, y: number, z: number) => s.matAt!(x / k, y / k, z / k) } : {}),
  }));
}

/** The shapes, each tagged as part `part` (for packing new blocks by part). */
export const tagged = (shapes: Shape[], part: number): Shape[] => shapes.map((s) => ({ ...s, part }));

/** Yaws local shapes by `yawDeg` about their origin (counter-clockwise seen from above, +x east, -z north), then moves the
 * origin to `at`. */
export function place(shapes: Shape[], at: Vec3, yawDeg = 0): Shape[] {
  if (!yawDeg) return translate(shapes, at);
  const a = (yawDeg * Math.PI) / 180, c = Math.cos(a), s = Math.sin(a);
  return shapes.map((sh) => {
    const min: Vec3 = [Infinity, sh.min[1] + at[1], Infinity], max: Vec3 = [-Infinity, sh.max[1] + at[1], -Infinity];
    for (const x of [sh.min[0], sh.max[0]]) for (const z of [sh.min[2], sh.max[2]]) {
      const wx = c * x + s * z + at[0], wz = -s * x + c * z + at[2];
      min[0] = Math.min(min[0], wx); max[0] = Math.max(max[0], wx);
      min[2] = Math.min(min[2], wz); max[2] = Math.max(max[2], wz);
    }
    return {
      op: sh.op, mat: sh.mat, min, max, part: sh.part, ...(sh.keep ? { keep: true } : {}),
      hit: (x: number, y: number, z: number) => {
        const dx = x - at[0], dz = z - at[2];
        return sh.hit(c * dx - s * dz, y - at[1], s * dx + c * dz);
      },
      ...(sh.matAt ? { matAt: (x: number, y: number, z: number) => {
        const dx = x - at[0], dz = z - at[2];
        return sh.matAt!(c * dx - s * dz, y - at[1], s * dx + c * dz);
      } } : {}),
    };
  });
}

/** A local point yawed and moved as place() moves shapes. */
export function placePoint(p: Vec3, at: Vec3, yawDeg = 0): Vec3 {
  const a = (yawDeg * Math.PI) / 180, c = Math.cos(a), s = Math.sin(a);
  return [c * p[0] + s * p[2] + at[0], p[1] + at[1], -s * p[0] + c * p[2] + at[2]];
}

// ------------------------------------------------------------------ openings

/** A door or gate through the wall of a rectangle (half sizes hw along x, hd along z, walls `wallT` thick) on `side`,
 * centred on the side (`shift` along it). Round-topped when the opening is at least 3 wide and 2 higher than half its
 * width. `below` carves its floor that far under y0 at the outer face, rising to y0 at the inner face: a ramp down to
 * ground outside that is lower than the floor, instead of a sill as high as the difference. `bevel` cants the head
 * instead: the two top corners are cut at 45 degrees, `bevel` in from each (at most half the width and the height). */
export function doorway(hw: number, hd: number, wallT: number, side: Side, width: number, height: number, y0 = 0, shift = 0, below = 0, square = false, bevel = 0): Shape {
  const alongX = side === "n" || side === "s";
  const outer = (alongX ? hd : hw) + 1, inner = (alongX ? hd : hw) - wallT - 1;
  const sign = side === "s" || side === "e" ? 1 : -1;
  const d0 = sign > 0 ? inner : -outer, d1 = sign > 0 ? outer : -inner;
  const bv = Math.max(0, Math.min(bevel, width / 2, height));
  const r = width / 2, arched = !bv && !square && width >= 3 && height >= r + 2;
  const ux0 = -r + shift, ux1 = r + shift;
  // the floor of the opening at depth d across the wall: y0 at the inner face, y0 - below at the outer face
  const floorAt = (d: number) => (below > 0 ? y0 - below * Math.min(1, Math.max(0, (sign * d - inner) / (outer - inner))) : y0);
  const hit = (x: number, y: number, z: number) => {
    const u = (alongX ? x : z) - shift, d = alongX ? z : x;
    if (d < d0 || d >= d1 || u < -r || u >= r || y < floorAt(d) || y >= y0 + height) return false;
    if (bv) return r - Math.abs(u) + (y0 + height - y) >= bv;
    if (!arched || y < y0 + height - r) return true;
    const dy = y - (y0 + height - r);
    return u * u + dy * dy <= r * r;
  };
  const yMin = y0 - Math.max(0, below);
  return {
    op: "carve", mat: 0, hit,
    min: alongX ? [ux0, yMin, d0] : [d0, yMin, ux0], max: alongX ? [ux1, y0 + height, d1] : [d1, y0 + height, ux1],
  };
}

// ------------------------------------------------------------------ walls

export type Parapet = "left" | "right" | "both" | "all";

/** One wall segment from (ax, ay, az) to (bx, by, bz), a rectangle in plan `t` thick (extended `ext` past both ends) and
 * `h` high above the sloping base line, `found` voxels of foundation below it, and optionally crenellations one row
 * above the top. Left of the direction a->b is +v where v = (-dz, dx). Parts: the body and the merlons. */
export function wallSegment(a: Vec3, b: Vec3, t: number, h: number, mat: number, o: { found?: number; cren?: boolean; parapet?: Parapet; ext?: number; style?: MatStyle } = {}): Shape[] {
  const dx = b[0] - a[0], dz = b[2] - a[2], len = Math.hypot(dx, dz);
  if (len < 1e-9) return [];
  const ux = dx / len, uz = dz / len, found = o.found ?? 0, ext = o.ext ?? t / 2, hw = t / 2;
  const uv = (x: number, z: number): [number, number] => [(x - a[0]) * ux + (z - a[2]) * uz, -(x - a[0]) * uz + (z - a[2]) * ux];
  const base = (u: number) => a[1] + (b[1] - a[1]) * Math.min(1, Math.max(0, u / len));
  const min: Vec3 = [Infinity, Math.min(a[1], b[1]) - found, Infinity], max: Vec3 = [-Infinity, Math.max(a[1], b[1]) + h + 1, -Infinity];
  for (const u of [-ext, len + ext]) for (const v of [-hw, hw]) {
    const x = a[0] + u * ux - v * uz, z = a[2] + u * uz + v * ux;
    min[0] = Math.min(min[0], x); max[0] = Math.max(max[0], x);
    min[2] = Math.min(min[2], z); max[2] = Math.max(max[2], z);
  }
  // The hit takes v = -hw (and u = -ext) and that edge can be the box's max side: a cell centred on it is inside the
  // wall, so the box reaches just past it (a 1-thick wall on a whole-unit line facing +x or +z was otherwise empty).
  for (const k of [0, 2]) { min[k] -= 1e-6; max[k] += 1e-6; }
  const body: Shape = {
    op: "fill", mat, min, max,
    hit: (x, y, z) => {
      const [u, v] = uv(x, z);
      if (u < -ext || u >= len + ext || v < -hw || v >= hw) return false;
      const yb = base(u);
      return y >= yb - found && y < yb + h;
    },
  };
  const st = hasStyle(o.style) ? o.style : undefined;
  // styled by height above the (sloping) base line
  if (st) body.matAt = (x, y, z) => styleMat(mat, st, y - base(uv(x, z)[0]), h);
  if (!o.cren) return [body];
  const parapet = t < 3 ? "all" : o.parapet ?? "both";
  const merlons: Shape = {
    op: "fill", mat, min, max,
    hit: (x, y, z) => {
      const [u, v] = uv(x, z);
      if (u < -ext || u >= len + ext || v < -hw || v >= hw || !merlon(u)) return false;
      const yt = base(u) + h;
      if (y < yt || y >= yt + 1) return false;
      return parapet === "all" || (parapet === "left" ? v >= hw - 1 : parapet === "right" ? v < -hw + 1 : Math.abs(v) >= hw - 1);
    },
  };
  if (st) merlons.matAt = (x, y, z) => styleMat(mat, st, y - base(uv(x, z)[0]), h, true);
  return [body, merlons];
}

// ------------------------------------------------------------------ towers and keeps

/** A roof on a tower's top: a solid of revolution standing on the top floor (y = the tower's height), inside the merlon
 * ring when the tower is crenellated. dome: a sphere cap (height <= radius) or half an ellipsoid; onion: a neck, a bulge a
 * tenth wider than the base a third of the way up, then a taper to a point; cone: straight to a point. `tip` paints the
 * topmost `tipHeight` (a finial). */
export interface RoofSpec { style: "dome" | "onion" | "cone"; radius: number; height: number; material: number; tip?: number; tipHeight?: number; }

/** The least radius of a cone's or an onion's top (design voxels): a voxel centre half a voxel off the axis is inside. */
export const TIP_RADIUS = 0.75;

/** An onion dome's width (1 = the base circle's radius) at t, 0 at its foot to 1 at its tip. */
export function onionProfile(t: number): number {
  if (t <= 0.3) return 0.85 + 0.25 * Math.sin((t / 0.3) * (Math.PI / 2));
  return 1.1 * Math.pow(Math.cos(((t - 0.3) / 0.7) * (Math.PI / 2)), 0.8);
}

/** The roof's shape with its base centre at (cx, y0, cz). */
export function roof(r: RoofSpec, cx: number, y0: number, cz: number): Shape {
  const R = r.radius, h = r.height, w = r.style === "onion" ? R * 1.1 : R;
  let inside: (d2: number, y: number) => boolean;
  // a pointed roof thinner than a voxel near its tip would end short of its height: it keeps a stub of TIP_RADIUS
  if (r.style === "cone") inside = (d2, y) => { const q = Math.max(TIP_RADIUS, R * (1 - (y - y0) / h)); return d2 < q * q; };
  else if (r.style === "onion") inside = (d2, y) => { const q = Math.max(TIP_RADIUS, R * onionProfile((y - y0) / h)); return d2 < q * q; };
  else if (h <= R) { const S = (R * R + h * h) / (2 * h), yc = y0 + h - S; inside = (d2, y) => d2 + (y - yc) ** 2 < S * S; }
  else inside = (d2, y) => d2 / (R * R) + ((y - y0) / h) ** 2 < 1;
  const s: Shape = { op: "fill", mat: r.material, min: [cx - w, y0, cz - w], max: [cx + w, y0 + h, cz + w],
    hit: (x, y, z) => y >= y0 && y < y0 + h && inside((x - cx) ** 2 + (z - cz) ** 2, y) };
  if (r.tip !== undefined) { const from = y0 + h - (r.tipHeight ?? 1), tip = r.tip; s.matAt = (_x, y) => (y >= from - 1e-9 ? tip : r.material); }
  return s;
}

export interface TowerSpec {
  shape: "round" | "square";
  radius: number;               // round: radius; square: half the side
  height: number;
  material: number;
  hollow: boolean;
  wall: number;                 // wall thickness when hollow
  door: Side | null;
  doorWidth: number;
  doorHeight: number;
  cren: boolean;
  found: number;
  cx?: number; cz?: number;     // centre in local coordinates (default 0)
  style?: MatStyle;
  roof?: RoofSpec;              // on the top floor (a solid tower's top, a hollow tower's rim is roofed over)
}

/** A tower on its floor level: solid or hollow (open top, floor at y = 0), with a door and crenellations round the rim. */
export function tower(t: TowerSpec): Shape[] {
  const cx = t.cx ?? 0, cz = t.cz ?? 0, r = t.radius, h = t.height, out: Shape[] = [];
  const round = t.shape === "round";
  const inRing = (x: number, z: number, rr: number) => round ? (x - cx) * (x - cx) + (z - cz) * (z - cz) < rr * rr
    : Math.abs(x - cx) < rr && Math.abs(z - cz) < rr;
  const body = (op: Op, rr: number, y0: number, y1: number): Shape => ({
    op, mat: t.material, min: [cx - rr, y0, cz - rr], max: [cx + rr, y1, cz + rr], hit: (x, y, z) => y >= y0 && y < y1 && inRing(x, z, rr),
  });
  out.push(styled(body("fill", r, -t.found, h), t.style, h));
  if (t.cren) {
    const n = Math.max(6, Math.round(2 * Math.PI * r));
    out.push(styled({
      op: "fill", mat: t.material, min: [cx - r, h, cz - r], max: [cx + r, h + 1, cz + r],
      hit: (x, y, z) => {
        if (y < h || y >= h + 1 || !inRing(x, z, r) || inRing(x, z, r - 1)) return false;
        const dx = x - cx, dz = z - cz;
        if (round) return merlon(((Math.atan2(dz, dx) + Math.PI) / (2 * Math.PI)) * n);
        const p = Math.abs(dx) > Math.abs(dz) ? Math.floor(dz) + (dx > 0 ? 1000 : 2000) : Math.floor(dx) + (dz > 0 ? 3000 : 4000);
        return merlon(p);
      },
    }, t.style, h, true));
  }
  if (t.hollow && r - t.wall > 0.5) out.push(body("carve", r - t.wall, 0, h + 2));
  if (t.door) out.push(...translate([doorway(r, r, t.wall, t.door, t.doorWidth, t.doorHeight)], [cx, 0, cz]));
  // after the hollow's carve (which reaches 2 over the rim), so the roof closes the tower
  if (t.roof) out.push(roof(t.roof, cx, h, cz));
  return out;
}

export interface KeepSpec {
  size: [number, number];       // outer x, z
  height: number;
  material: number;
  wall: number;
  hollow: boolean;
  roof: boolean;
  door: Side | null;
  doorWidth: number;
  doorHeight: number;
  windows: boolean;
  cren: boolean;
  found: number;
  style?: MatStyle;
}

/** A rectangular keep centred on the origin: walls `wall` thick, a floor-level interior, a flat roof one layer thick with
 * crenellations on its rim, a door and arrow slits. */
export function keep(k: KeepSpec): Shape[] {
  const hw = k.size[0] / 2, hd = k.size[1] / 2, h = k.height, w = k.wall;
  const out: Shape[] = [styled(box("fill", k.material, [-hw, -k.found, -hd], [hw, h, hd]), k.style, h)];
  if (k.cren) {
    out.push(styled({
      op: "fill", mat: k.material, min: [-hw, h, -hd], max: [hw, h + 1, hd],
      hit: (x, y, z) => {
        if (y < h || y >= h + 1 || (Math.abs(x) < hw - 1 && Math.abs(z) < hd - 1)) return false;
        return merlon(Math.abs(x) >= hw - 1 && Math.abs(z) < hd - 1 ? z + 500 : x + 500);
      },
    }, k.style, h, true));
  }
  if (k.hollow) {
    const top = k.roof ? h - 1 : h + 2;
    if (hw - w > 0.5 && hd - w > 0.5 && top > 1) out.push(box("carve", 0, [-hw + w, 0, -hd + w], [hw - w, top, hd - w]));
  }
  if (k.windows && k.hollow) {
    const wy = Math.min(3, Math.max(1, Math.floor(h / 2) - 1));
    if (h - 2 > wy + 1) {
      for (let u = -hw + 3.5; u < hw - 2; u += 4) {
        if (k.door === "n" && Math.abs(u) < k.doorWidth / 2 + 1) continue;
        if (k.door === "s" && Math.abs(u) < k.doorWidth / 2 + 1) continue;
        for (const sz of [-1, 1]) out.push(box("carve", 0, [u - 0.5, wy, sz > 0 ? hd - w - 1 : -hd - 1], [u + 0.5, wy + 2, sz > 0 ? hd + 1 : -hd + w + 1]));
      }
      for (let u = -hd + 3.5; u < hd - 2; u += 4) {
        if (k.door === "e" && Math.abs(u) < k.doorWidth / 2 + 1) continue;
        if (k.door === "w" && Math.abs(u) < k.doorWidth / 2 + 1) continue;
        for (const sx of [-1, 1]) out.push(box("carve", 0, [sx > 0 ? hw - w - 1 : -hw - 1, wy, u - 0.5], [sx > 0 ? hw + 1 : -hw + w + 1, wy + 2, u + 0.5]));
      }
    }
  }
  if (k.door) out.push(doorway(hw, hd, w, k.door, k.doorWidth, k.doorHeight));
  return out;
}

// ------------------------------------------------------------------ castle

export interface CastleSpec {
  size: [number, number];           // outer x, z of the curtain wall
  wallHeight: number;
  wallThickness: number;
  towerShape: "round" | "square";
  towerRadius: number;
  towerHeight: number;
  keepSize: [number, number] | null;
  keepHeight: number;
  /** The keep's centre in the courtyard [x, z] (default the castle's centre) and the side its door faces (default the
   * gate's): build_castle's gateLane moves the keep aside so a lane runs from the gate past it. */
  keepAt?: [number, number];
  keepDoor?: Side | null;
  gate: Side | null;
  gateWidth: number;
  gateHeight: number;
  /** More gates (each its own side, width, height, shift along the side and below); when given, `gate` is only the
   * keep's door side. */
  gates?: GateSpec[];
  material: number;
  cren: boolean;
  found: number;
  style?: MatStyle;                 // bands and trim by height above the floor, on the walls, towers, keep and foundation
  /** A roof per corner tower, in TOWER_CORNERS' order (nw, sw, ne, se); null leaves that tower's top open. */
  towerRoofs?: (RoofSpec | null)[];
}

/** The corner towers in the order castle() builds them (parts 5-8): x west to east, z north (-z) to south. */
export const TOWER_CORNERS = ["nw", "sw", "ne", "se"] as const;
export type TowerCorner = typeof TOWER_CORNERS[number];

/** Each corner tower's centre at its top floor, in the castle's local coordinates, with its roof (if any). */
export function castleTowers(c: CastleSpec): { corner: TowerCorner; at: Vec3; roof: RoofSpec | null }[] {
  const hw = c.size[0] / 2, hd = c.size[1] / 2, t = c.wallThickness, out: { corner: TowerCorner; at: Vec3; roof: RoofSpec | null }[] = [];
  let i = 0;
  for (const sx of [-1, 1]) for (const sz of [-1, 1]) { out.push({ corner: TOWER_CORNERS[i], at: [sx * (hw - t / 2), c.towerHeight, sz * (hd - t / 2)], roof: c.towerRoofs?.[i] ?? null }); i++; }
  return out;
}

export interface GateSpec { side: Side; width: number; height: number; shift: number; below: number; bevel?: number; }

/** A curtain wall on a rectangle with a tower on each corner, an optional keep in the courtyard and a gate (or gates). */
export function castle(c: CastleSpec): Shape[] {
  const hw = c.size[0] / 2, hd = c.size[1] / 2, t = c.wallThickness, out: Shape[] = [];
  // parts, for packing new blocks one per part: 0 the foundation, 1-4 the walls, 5-8 the towers, 9 the keep
  if (c.found > 0) out.push({ ...styled(box("fill", c.material, [-hw, -c.found, -hd], [hw, 0, hd]), c.style), part: 0 });
  // wall centre lines, each crenellated on its outer side (the side away from the origin)
  const ring: [Vec3, Vec3][] = [
    [[-hw + t / 2, 0, -hd + t / 2], [hw - t / 2, 0, -hd + t / 2]],
    [[hw - t / 2, 0, -hd + t / 2], [hw - t / 2, 0, hd - t / 2]],
    [[hw - t / 2, 0, hd - t / 2], [-hw + t / 2, 0, hd - t / 2]],
    [[-hw + t / 2, 0, hd - t / 2], [-hw + t / 2, 0, -hd + t / 2]],
  ];
  for (const [w, [a, b]] of ring.entries()) {
    const dx = b[0] - a[0], dz = b[2] - a[2], len = Math.hypot(dx, dz);
    const vx = -dz / len, vz = dx / len;                       // left of a->b
    const inward = ((a[0] + b[0]) / 2) * vx + ((a[2] + b[2]) / 2) * vz < 0;   // the origin lies on the +v side
    out.push(...tagged(wallSegment(a, b, t, c.wallHeight, c.material, { cren: c.cren, parapet: inward ? "right" : "left", ext: t / 2, style: c.style }), 1 + w));

  }
  let tp = 5;
  for (const sx of [-1, 1]) for (const sz of [-1, 1]) {
    out.push(...tagged(tower({ shape: c.towerShape, radius: c.towerRadius, height: c.towerHeight, material: c.material, hollow: false, wall: 2, door: null,
      doorWidth: 2, doorHeight: 3, cren: c.cren, found: c.found, style: c.style, cx: sx * (hw - t / 2), cz: sz * (hd - t / 2),
      ...(c.towerRoofs?.[tp - 5] ? { roof: c.towerRoofs[tp - 5]! } : {}) }), tp++));
  }
  if (c.keepSize) {
    const k = keep({ size: c.keepSize, height: c.keepHeight, material: c.material, wall: Math.min(2, Math.floor(Math.min(...c.keepSize) / 3)), hollow: true,
      roof: true, door: c.keepDoor === undefined ? c.gate : c.keepDoor, doorWidth: Math.min(3, c.gateWidth), doorHeight: Math.min(4, c.gateHeight), windows: true, cren: c.cren, found: c.found, style: c.style });
    out.push(...tagged(c.keepAt && (c.keepAt[0] || c.keepAt[1]) ? translate(k, [c.keepAt[0], 0, c.keepAt[1]]) : k, 9));
  }
  // a gate that leaves no lintel (within a voxel of the wall's top) would leave the merlons over it floating: it is cut
  // open to the top of the merlon row instead, square
  const open = (h: number) => h >= c.wallHeight - 1 - 1e-9;
  const gh = (h: number) => (open(h) ? c.wallHeight + 2 : h);
  if (c.gates) for (const g of c.gates) out.push(doorway(hw, hd, t, g.side, g.width, gh(g.height), 0, g.shift, g.below, open(g.height), open(g.height) ? 0 : g.bevel ?? 0));
  else if (c.gate) out.push(doorway(hw, hd, t, c.gate, c.gateWidth, gh(c.gateHeight), 0, 0, 0, open(c.gateHeight)));
  return out;
}

export interface Spot { kind: "courtyard" | "rampart" | "tower"; at: Vec3; }

/** Where worms can start in a castle, in its local coordinates (y = the level they stand on): the open courtyard between
 * the curtain wall and the keep (never inside a wall passage), the middle of each wall-walk, and each tower top. */
export function castleSpots(c: CastleSpec): Spot[] {
  const hw = c.size[0] / 2, hd = c.size[1] / 2, t = c.wallThickness, out: Spot[] = [];
  const ix = hw - t, iz = hd - t;                  // the courtyard's half sizes
  if (c.keepSize) {
    const kx = c.keepSize[0] / 2, kz = c.keepSize[1] / 2, [ox, oz] = c.keepAt ?? [0, 0];
    // the middle of the open band between the keep and each wall, where it is at least 1.5 wide (a keep moved aside for
    // a gate lane leaves a wide band on one side and a narrow one, or none, on the other)
    for (const sz of [-1, 1]) if (iz - sz * oz - kz >= 1.5) out.push({ kind: "courtyard", at: [ox, 0, (sz * iz + oz + sz * kz) / 2] });
    for (const sx of [-1, 1]) if (ix - sx * ox - kx >= 1.5) out.push({ kind: "courtyard", at: [(sx * ix + ox + sx * kx) / 2, 0, oz] });
  } else {
    out.push({ kind: "courtyard", at: [0, 0, 0] });
    for (const sx of [-1, 1]) for (const sz of [-1, 1]) if (ix >= 4 && iz >= 4) out.push({ kind: "courtyard", at: [sx * ix / 2, 0, sz * iz / 2] });
  }
  // wall-walks: halfway along each side, on the inner half of the wall's top (behind the parapet)
  const walk = Math.max(0.5, t / 4);
  out.push({ kind: "rampart", at: [0, c.wallHeight, -hd + t / 2 + walk] }, { kind: "rampart", at: [0, c.wallHeight, hd - t / 2 - walk] },
    { kind: "rampart", at: [-hw + t / 2 + walk, c.wallHeight, 0] }, { kind: "rampart", at: [hw - t / 2 - walk, c.wallHeight, 0] });
  // a roofed tower's top is no place to stand
  for (const tw of castleTowers(c)) if (!tw.roof) out.push({ kind: "tower", at: tw.at });
  return out;
}


// ------------------------------------------------------------------ slopes: ramps, stairs, bridges

/** A lane starting at (ax, az) going along the unit vector (dx, dz) for `len`, `w` wide centred on that line, whose top
 * runs from yA to yB: a smooth slope, or `steps` treads. Solid down to floorY, or a slab `thick` thick. */
export interface Lane {
  a: [number, number]; d: [number, number]; len: number; w: number; yA: number; yB: number; steps?: number;
  floorY: number | null; thick: number; mat: number;
}

export const laneTop = (l: Lane, u: number) => {
  const f = Math.min(1, Math.max(0, u / l.len));
  if (!l.steps) return l.yA + (l.yB - l.yA) * f;
  const n = l.steps, i = Math.min(n - 1, Math.floor(f * n));
  return l.yB >= l.yA ? l.yA + ((l.yB - l.yA) * (i + 1)) / n : l.yA + ((l.yB - l.yA) * i) / n;
};

export function lane(l: Lane): Shape {
  const [ax, az] = l.a, [dx, dz] = l.d, hw = l.w / 2;
  const min: Vec3 = [Infinity, Math.min(l.yA, l.yB, l.floorY ?? Infinity) - l.thick, Infinity], max: Vec3 = [-Infinity, Math.max(l.yA, l.yB) + 1, -Infinity];
  for (const u of [0, l.len]) for (const v of [-hw, hw]) {
    const x = ax + u * dx - v * dz, z = az + u * dz + v * dx;
    min[0] = Math.min(min[0], x); max[0] = Math.max(max[0], x);
    min[2] = Math.min(min[2], z); max[2] = Math.max(max[2], z);
  }
  return {
    op: "fill", mat: l.mat, min, max,
    hit: (x, y, z) => {
      const u = (x - ax) * dx + (z - az) * dz, v = -(x - ax) * dz + (z - az) * dx;
      if (u < 0 || u >= l.len || v < -hw || v >= hw) return false;
      const top = laneTop(l, u);
      return y < top && y >= (l.floorY !== null ? Math.min(l.floorY, top - 1) : top - l.thick);
    },
  };
}

/** The air over a lane: `headroom` above its top across its width (build_ramp mode cut carves it out of the terrain the
 * ramp is cut into). */
export function laneCut(l: Lane, headroom: number): Shape {
  const body = lane(l), [ax, az] = l.a, [dx, dz] = l.d, hw = l.w / 2;
  return {
    op: "carve", mat: 0, min: [body.min[0], Math.min(l.yA, l.yB), body.min[2]], max: [body.max[0], Math.max(l.yA, l.yB) + headroom, body.max[2]],
    hit: (x, y, z) => {
      const u = (x - ax) * dx + (z - az) * dz, v = -(x - ax) * dz + (z - az) * dx;
      if (u < 0 || u >= l.len || v < -hw || v >= hw) return false;
      const top = laneTop(l, u);
      return y >= top && y < top + headroom;
    },
  };
}

// ------------------------------------------------------------------ craters, tunnels, arches

export interface CraterSpec {
  radius: number; depth: number; rimWidth: number; rimHeight: number; material: number; bowlMaterial: number | null;
  support: number; clearance: number;
}

/** A paraboloid bowl carved below the surface level y = 0 and a rim of raised material round it. Local origin: the
 * centre of the crater on the surface. */
export function crater(c: CraterSpec): Shape[] {
  const R = c.radius, D = c.depth, out: Shape[] = [];
  const floor = (d: number) => -D * (1 - (d * d) / (R * R));
  out.push({
    op: "carve", mat: 0, min: [-R, -D, -R], max: [R, c.clearance, R],
    hit: (x, y, z) => { const d = Math.hypot(x, z); return d < R && y >= floor(d) && y < Math.max(c.clearance, 0); },
  });
  if (c.bowlMaterial !== null) {
    out.push({
      op: "paint", mat: c.bowlMaterial, min: [-R, -D - 2, -R], max: [R, 0, R],
      hit: (x, y, z) => { const d = Math.hypot(x, z); return d < R && y < floor(d) && y >= floor(d) - 2; },
    });
  }
  if (c.rimHeight > 0 && c.rimWidth > 0) {
    const W = c.rimWidth;
    out.push({
      op: "fill", mat: c.material, min: [-R - W, -c.support, -R - W], max: [R + W, c.rimHeight, R + W],
      hit: (x, y, z) => {
        const d = Math.hypot(x, z);
        return d >= R && d < R + W && y >= -c.support && y < c.rimHeight * (1 - (d - R) / W);
      },
    });
  }
  return out;
}

/** The tunnel as shapes: a lining shell of `lining` thickness (fill) round every segment, then the bore (carve). */
export function tunnel(points: Vec3[], radius: number, shape: "round" | "square", lining: { material: number; thickness: number } | null, caps: "round" | "flat" = "round"): Shape[] {
  const out: Shape[] = [];
  // caps flat: the bore stops at the plane through its first and its last point, square to the segment there (a mouth
  // flush with a wall's face); round (default) runs a radius past them
  const seg = points.map((p, i) => i + 1 < points.length && Math.hypot(points[i + 1][0] - p[0], points[i + 1][1] - p[1], points[i + 1][2] - p[2]) > 1e-6);
  const first = seg.indexOf(true), last = seg.lastIndexOf(true);
  const past = (i: number, t: number) => caps === "flat" && ((i === first && t < 0) || (i === last && t > 1));
  const parts = (r: number, op: Op, mat: number): Shape[] => {
    const s: Shape[] = [];
    for (let i = 0; i + 1 < points.length; i++) {
      const a = points[i], b = points[i + 1];
      const dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2], len2 = dx * dx + dy * dy + dz * dz;
      if (len2 < 1e-12) continue;
      const hl = Math.hypot(dx, dz);
      const ux = hl > 1e-9 ? dx / hl : 1, uz = hl > 1e-9 ? dz / hl : 0;   // plan direction (vertical segment: any)
      const min: Vec3 = [Math.min(a[0], b[0]) - r, Math.min(a[1], b[1]) - r, Math.min(a[2], b[2]) - r];
      const max: Vec3 = [Math.max(a[0], b[0]) + r, Math.max(a[1], b[1]) + r, Math.max(a[2], b[2]) + r];
      if (shape === "round") {
        s.push({
          op, mat, min, max,
          hit: (x, y, z) => {
            const t0 = ((x - a[0]) * dx + (y - a[1]) * dy + (z - a[2]) * dz) / len2;
            if (past(i, t0)) return false;
            const t = Math.min(1, Math.max(0, t0));
            const ex = x - a[0] - t * dx, ey = y - a[1] - t * dy, ez = z - a[2] - t * dz;
            return ex * ex + ey * ey + ez * ez < r * r;
          },
        });
      } else {
        s.push({
          op, mat, min, max,
          hit: (x, y, z) => {
            const t0 = ((x - a[0]) * dx + (y - a[1]) * dy + (z - a[2]) * dz) / len2;
            if (past(i, t0)) return false;
            const t = Math.min(1, Math.max(0, t0));
            const px = x - a[0] - t * dx, pz = z - a[2] - t * dz;
            const v = -px * uz + pz * ux;                         // sideways in plan, off the line
            const dyLine = y - (a[1] + t * dy);
            return Math.abs(v) < r && Math.abs(dyLine) < r && Math.abs(px * ux + pz * uz) < r;
          },
        });
      }
    }
    return s;
  };
  if (lining && lining.thickness > 0) out.push(...parts(radius + lining.thickness, "fill", lining.material));
  out.push(...parts(radius, "carve", 0));
  return out;
}

export interface FlatTunnelSpec {
  points: Vec3[];               // on the floor: y is the level worms walk on, straight (sloping) between the points
  radius: number;               // half the width
  shape: "round" | "square";    // round: the top `radius` of the bore is a half circle
  headroom: number;             // floor to roof
  floor: number;                // thickness of the floor slab filled under it where it is empty (0: none)
  material: number;             // the floor slab's
  lining: { material: number; thickness: number } | null;
  caps?: "round" | "flat";     // flat: the bore stops at the plane through its first and last point (default round)
}

/** A tunnel with a flat, walkable floor (dig_tunnel floor "flat"): per segment, a cell's distance from the segment in plan
 * (so bends are rounded) and its height over the floor line there. Shapes: the lining shell, the floor slab (only empty
 * cells: a floor across a gap, not a repaint of the rock), then the bore. */
export function flatTunnel(t: FlatTunnelSpec): Shape[] {
  const r = t.radius, H = t.headroom, out: Shape[] = [];
  const segs = t.points.map((p, i) => i + 1 < t.points.length && Math.hypot(t.points[i + 1][0] - p[0], t.points[i + 1][2] - p[2]) > 1e-6);
  const firstSeg = segs.indexOf(true), lastSeg = segs.lastIndexOf(true);
  const parts = (op: Op, mat: number, grow: number, below: number, slab: boolean): Shape[] => {
    const s: Shape[] = [];
    for (let i = 0; i + 1 < t.points.length; i++) {
      const a = t.points[i], b = t.points[i + 1];
      const dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2], hl2 = dx * dx + dz * dz;
      if (hl2 < 1e-12) continue;
      const R = r + grow, top = slab ? 0 : H + grow;
      const min: Vec3 = [Math.min(a[0], b[0]) - R - 1e-6, Math.min(a[1], b[1]) - below, Math.min(a[2], b[2]) - R - 1e-6];
      const max: Vec3 = [Math.max(a[0], b[0]) + R + 1e-6, Math.max(a[1], b[1]) + top, Math.max(a[2], b[2]) + R + 1e-6];
      s.push({
        op, mat, min, max, ...(slab ? { keep: true } : {}),
        hit: (x, y, z) => {
          const f0 = ((x - a[0]) * dx + (z - a[2]) * dz) / hl2;
          if (t.caps === "flat" && ((i === firstSeg && f0 < 0) || (i === lastSeg && f0 > 1))) return false;
          const f = Math.min(1, Math.max(0, f0));
          const px = x - a[0] - f * dx, pz = z - a[2] - f * dz, d2 = px * px + pz * pz;
          if (d2 >= R * R) return false;
          const h = y - (a[1] + f * dy);
          if (h < -below || h >= top) return false;
          if (slab || t.shape === "square" || h < H - r) return true;
          const e = h - (H - r);
          return d2 + e * e < R * R;
        },
      });
    }
    return s;
  };
  if (t.lining && t.lining.thickness > 0) out.push(...parts("fill", t.lining.material, t.lining.thickness, t.lining.thickness, false));
  if (t.floor > 0) out.push(...parts("fill", t.material, 0, t.floor, true));
  out.push(...parts("carve", 0, 0, 0, false));
  return out;
}

export interface ArchSpec {
  span: number; rise: number; pier: number; depth: number; crown: number; material: number; found: number;
}

/** A freestanding arch (a gateway) in the x-y plane: two piers, a round-topped opening of width `span` that goes
 * straight up `rise` and then curves, and a crown above. */
export function arch(a: ArchSpec): Shape[] {
  const hw = a.span / 2 + a.pier, r = a.span / 2, d = a.depth / 2, top = a.rise + r + a.crown;
  return [
    box("fill", a.material, [-hw, -a.found, -d], [hw, top, d]),
    {
      op: "carve", mat: 0, min: [-r, 0, -d - 1], max: [r, a.rise + r, d + 1],
      hit: (x, y, z) => {
        if (z < -d - 1 || z >= d + 1 || x < -r || x >= r || y < 0) return false;
        if (y < a.rise) return true;
        const dy = y - a.rise;
        return x * x + dy * dy < r * r;
      },
    },
  ];
}

export interface BridgeSpec {
  a: Vec3; b: Vec3; width: number; deck: number; rails: boolean; material: number;
  pierSpacing: number; pierWidth: number; downTo: number | null; arched: boolean;
}

/** A deck from a to b (its top surface is the line between the two points), with rails one row above its edges, and
 * piers every `pierSpacing` voxels down to `downTo`; `arched` carves a round arch through each bay between piers. */
export function bridge(g: BridgeSpec): { shapes: Shape[]; length: number } {
  const dx = g.b[0] - g.a[0], dz = g.b[2] - g.a[2], len = Math.hypot(dx, dz);
  const d: [number, number] = [dx / len, dz / len];
  const top = (u: number) => g.a[1] + (g.b[1] - g.a[1]) * Math.min(1, Math.max(0, u / len));
  const deck = lane({ a: [g.a[0], g.a[2]], d, len, w: g.width, yA: g.a[1], yB: g.b[1], floorY: null, thick: g.deck, mat: g.material });
  const out: Shape[] = [];
  const hw = g.width / 2;
  const uv = (x: number, z: number): [number, number] => [(x - g.a[0]) * d[0] + (z - g.a[2]) * d[1], -(x - g.a[0]) * d[1] + (z - g.a[2]) * d[0]];
  const solidTo = g.downTo !== null && g.pierSpacing > 0 ? g.downTo : null;
  const piers: number[] = [];
  if (solidTo !== null) for (let u = g.pierSpacing; u < len - g.pierSpacing / 2; u += g.pierSpacing) piers.push(u);
  const edges = [0, ...piers, len];
  if (solidTo !== null) {
    if (g.arched) {
      // solid body between downTo and the deck, then arches carved through it
      out.push({ ...deck, min: [deck.min[0], solidTo, deck.min[2]], hit: (x, y, z) => { const [u, v] = uv(x, z); return u >= 0 && u < len && v >= -hw && v < hw && y >= solidTo && y < top(u); } });
    } else {
      for (const u0 of piers) out.push({ ...deck, min: [deck.min[0], solidTo, deck.min[2]],
        hit: (x, y, z) => { const [u, v] = uv(x, z); return Math.abs(u - u0) < g.pierWidth / 2 && v >= -hw && v < hw && y >= solidTo && y < top(u); } });
    }
  }
  out.push(deck);
  if (solidTo !== null && g.arched) {
    for (let i = 0; i + 1 < edges.length; i++) {
      const u0 = edges[i] + (i === 0 ? g.pierWidth / 2 : g.pierWidth / 2), u1 = edges[i + 1] - g.pierWidth / 2;
      if (u1 - u0 < 2) continue;
      const uc = (u0 + u1) / 2, r = (u1 - u0) / 2, crown = top(uc) - g.deck, ry = Math.min(r, crown - solidTo), ys = crown - ry;
      if (ry < 1) continue;
      out.push({
        op: "carve", mat: 0, min: [deck.min[0], solidTo, deck.min[2]], max: deck.max,
        hit: (x, y, z) => {
          const [u, v] = uv(x, z);
          if (v < -hw - 1 || v >= hw + 1 || y < solidTo || Math.abs(u - uc) >= r) return false;
          if (y < ys) return true;
          const e = (u - uc) / r, f = (y - ys) / ry;
          return e * e + f * f < 1;
        },
      });
    }
  }
  if (g.rails) {
    out.push({
      ...deck, min: [deck.min[0], deck.min[1], deck.min[2]],
      hit: (x, y, z) => {
        const [u, v] = uv(x, z);
        if (u < 0 || u >= len || Math.abs(v) >= hw || Math.abs(v) < hw - 1) return false;
        const t = top(u);
        return y >= t && y < t + 1;
      },
    });
  }
  return { shapes: out, length: len };
}
