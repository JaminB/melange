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

export function translate(shapes: Shape[], d: Vec3): Shape[] {
  return shapes.map((s) => ({
    op: s.op, mat: s.mat, min: [s.min[0] + d[0], s.min[1] + d[1], s.min[2] + d[2]], max: [s.max[0] + d[0], s.max[1] + d[1], s.max[2] + d[2]],
    hit: (x, y, z) => s.hit(x - d[0], y - d[1], z - d[2]), part: s.part,
  }));
}

/** Shapes made in voxels of `k` map units, scaled about their origin into map units: a design whose walls are 2 voxels
 * thick keeps 2 voxels at any k, and its crenellations their 2-on-1-off rhythm. */
export function scaled(shapes: Shape[], k: number): Shape[] {
  if (k === 1) return shapes;
  return shapes.map((s) => ({
    op: s.op, mat: s.mat, min: [s.min[0] * k, s.min[1] * k, s.min[2] * k], max: [s.max[0] * k, s.max[1] * k, s.max[2] * k],
    hit: (x, y, z) => s.hit(x / k, y / k, z / k), part: s.part,
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
      op: sh.op, mat: sh.mat, min, max, part: sh.part,
      hit: (x: number, y: number, z: number) => {
        const dx = x - at[0], dz = z - at[2];
        return sh.hit(c * dx - s * dz, y - at[1], s * dx + c * dz);
      },
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
 * centred on the side. Round-topped when the opening is at least 3 wide and 2 higher than half its width. */
export function doorway(hw: number, hd: number, wallT: number, side: Side, width: number, height: number, y0 = 0, shift = 0): Shape {
  const alongX = side === "n" || side === "s";
  const outer = (alongX ? hd : hw) + 1, inner = (alongX ? hd : hw) - wallT - 1;
  const sign = side === "s" || side === "e" ? 1 : -1;
  const d0 = sign > 0 ? inner : -outer, d1 = sign > 0 ? outer : -inner;
  const r = width / 2, arched = width >= 3 && height >= r + 2;
  const ux0 = -r + shift, ux1 = r + shift;
  const hit = (x: number, y: number, z: number) => {
    const u = (alongX ? x : z) - shift, d = alongX ? z : x;
    if (d < d0 || d >= d1 || u < -r || u >= r || y < y0 || y >= y0 + height) return false;
    if (!arched || y < y0 + height - r) return true;
    const dy = y - (y0 + height - r);
    return u * u + dy * dy <= r * r;
  };
  return {
    op: "carve", mat: 0, hit,
    min: alongX ? [ux0, y0, d0] : [d0, y0, ux0], max: alongX ? [ux1, y0 + height, d1] : [d1, y0 + height, ux1],
  };
}

// ------------------------------------------------------------------ walls

export type Parapet = "left" | "right" | "both" | "all";

/** One wall segment from (ax, ay, az) to (bx, by, bz), a rectangle in plan `t` thick (extended `ext` past both ends) and
 * `h` high above the sloping base line, `found` voxels of foundation below it, and optionally crenellations one row
 * above the top. Left of the direction a->b is +v where v = (-dz, dx). Parts: the body and the merlons. */
export function wallSegment(a: Vec3, b: Vec3, t: number, h: number, mat: number, o: { found?: number; cren?: boolean; parapet?: Parapet; ext?: number } = {}): Shape[] {
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
  return [body, merlons];
}

// ------------------------------------------------------------------ towers and keeps

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
  out.push(body("fill", r, -t.found, h));
  if (t.cren) {
    const n = Math.max(6, Math.round(2 * Math.PI * r));
    out.push({
      op: "fill", mat: t.material, min: [cx - r, h, cz - r], max: [cx + r, h + 1, cz + r],
      hit: (x, y, z) => {
        if (y < h || y >= h + 1 || !inRing(x, z, r) || inRing(x, z, r - 1)) return false;
        const dx = x - cx, dz = z - cz;
        if (round) return merlon(((Math.atan2(dz, dx) + Math.PI) / (2 * Math.PI)) * n);
        const p = Math.abs(dx) > Math.abs(dz) ? Math.floor(dz) + (dx > 0 ? 1000 : 2000) : Math.floor(dx) + (dz > 0 ? 3000 : 4000);
        return merlon(p);
      },
    });
  }
  if (t.hollow && r - t.wall > 0.5) out.push(body("carve", r - t.wall, 0, h + 2));
  if (t.door) out.push(...translate([doorway(r, r, t.wall, t.door, t.doorWidth, t.doorHeight)], [cx, 0, cz]));
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
}

/** A rectangular keep centred on the origin: walls `wall` thick, a floor-level interior, a flat roof one layer thick with
 * crenellations on its rim, a door and arrow slits. */
export function keep(k: KeepSpec): Shape[] {
  const hw = k.size[0] / 2, hd = k.size[1] / 2, h = k.height, w = k.wall;
  const out: Shape[] = [box("fill", k.material, [-hw, -k.found, -hd], [hw, h, hd])];
  if (k.cren) {
    out.push({
      op: "fill", mat: k.material, min: [-hw, h, -hd], max: [hw, h + 1, hd],
      hit: (x, y, z) => {
        if (y < h || y >= h + 1 || (Math.abs(x) < hw - 1 && Math.abs(z) < hd - 1)) return false;
        return merlon(Math.abs(x) >= hw - 1 && Math.abs(z) < hd - 1 ? z + 500 : x + 500);
      },
    });
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
  gate: Side | null;
  gateWidth: number;
  gateHeight: number;
  material: number;
  cren: boolean;
  found: number;
}

/** A curtain wall on a rectangle with a tower on each corner, an optional keep in the courtyard and a gate. */
export function castle(c: CastleSpec): Shape[] {
  const hw = c.size[0] / 2, hd = c.size[1] / 2, t = c.wallThickness, out: Shape[] = [];
  // parts, for packing new blocks one per part: 0 the foundation, 1-4 the walls, 5-8 the towers, 9 the keep
  if (c.found > 0) out.push({ ...box("fill", c.material, [-hw, -c.found, -hd], [hw, 0, hd]), part: 0 });
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
    out.push(...tagged(wallSegment(a, b, t, c.wallHeight, c.material, { cren: c.cren, parapet: inward ? "right" : "left", ext: t / 2 }), 1 + w));

  }
  let tp = 5;
  for (const sx of [-1, 1]) for (const sz of [-1, 1]) {
    out.push(...tagged(tower({ shape: c.towerShape, radius: c.towerRadius, height: c.towerHeight, material: c.material, hollow: false, wall: 2, door: null,
      doorWidth: 2, doorHeight: 3, cren: c.cren, found: c.found, cx: sx * (hw - t / 2), cz: sz * (hd - t / 2) }), tp++));
  }
  if (c.keepSize) {
    out.push(...tagged(keep({ size: c.keepSize, height: c.keepHeight, material: c.material, wall: Math.min(2, Math.floor(Math.min(...c.keepSize) / 3)), hollow: true,
      roof: true, door: c.gate, doorWidth: Math.min(3, c.gateWidth), doorHeight: Math.min(4, c.gateHeight), windows: true, cren: c.cren, found: c.found }), 9));
  }
  if (c.gate) out.push(doorway(hw, hd, t, c.gate, c.gateWidth, c.gateHeight));
  return out;
}

export interface Spot { kind: "courtyard" | "rampart" | "tower"; at: Vec3; }

/** Where worms can start in a castle, in its local coordinates (y = the level they stand on): the open courtyard between
 * the curtain wall and the keep (never inside a wall passage), the middle of each wall-walk, and each tower top. */
export function castleSpots(c: CastleSpec): Spot[] {
  const hw = c.size[0] / 2, hd = c.size[1] / 2, t = c.wallThickness, out: Spot[] = [];
  const ix = hw - t, iz = hd - t;                  // the courtyard's half sizes
  if (c.keepSize) {
    const kx = c.keepSize[0] / 2, kz = c.keepSize[1] / 2;
    // the middle of the open band between the keep and each wall, where it is at least 1.5 wide
    if (iz - kz >= 1.5) for (const sz of [-1, 1]) out.push({ kind: "courtyard", at: [0, 0, sz * (kz + iz) / 2] });
    if (ix - kx >= 1.5) for (const sx of [-1, 1]) out.push({ kind: "courtyard", at: [sx * (kx + ix) / 2, 0, 0] });
  } else {
    out.push({ kind: "courtyard", at: [0, 0, 0] });
    for (const sx of [-1, 1]) for (const sz of [-1, 1]) if (ix >= 4 && iz >= 4) out.push({ kind: "courtyard", at: [sx * ix / 2, 0, sz * iz / 2] });
  }
  // wall-walks: halfway along each side, on the inner half of the wall's top (behind the parapet)
  const walk = Math.max(0.5, t / 4);
  out.push({ kind: "rampart", at: [0, c.wallHeight, -hd + t / 2 + walk] }, { kind: "rampart", at: [0, c.wallHeight, hd - t / 2 - walk] },
    { kind: "rampart", at: [-hw + t / 2 + walk, c.wallHeight, 0] }, { kind: "rampart", at: [hw - t / 2 - walk, c.wallHeight, 0] });
  for (const sx of [-1, 1]) for (const sz of [-1, 1]) out.push({ kind: "tower", at: [sx * (hw - t / 2), c.towerHeight, sz * (hd - t / 2)] });
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
export function tunnel(points: Vec3[], radius: number, shape: "round" | "square", lining: { material: number; thickness: number } | null): Shape[] {
  const out: Shape[] = [];
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
            const t = Math.min(1, Math.max(0, ((x - a[0]) * dx + (y - a[1]) * dy + (z - a[2]) * dz) / len2));
            const ex = x - a[0] - t * dx, ey = y - a[1] - t * dy, ez = z - a[2] - t * dz;
            return ex * ex + ey * ey + ez * ez < r * r;
          },
        });
      } else {
        s.push({
          op, mat, min, max,
          hit: (x, y, z) => {
            const t = Math.min(1, Math.max(0, ((x - a[0]) * dx + (y - a[1]) * dy + (z - a[2]) * dz) / len2));
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
