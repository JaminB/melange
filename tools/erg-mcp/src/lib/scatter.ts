// Kitbash crags and seat props for a decoration pass (tools/kitbash.ts place_crags; bench/caldera/detail-rock.mjs). Pure:
// rotation matrices in the engine's detail composition (R = Rz * Ry * Rx, scale applied first: web/src/sdk/erg/scene.ts
// frameLocal), the tilt that turns a prop's up axis toward the ground's normal, a seeded crag of 1-4 copies of one mesh
// (spire, ledge, stack, tor, single) in the crag's own frame, and the composed pose of every piece seated so a fraction
// of the crag's height is under the lowest ground sample of its footprint. Degrees at the edges, radians inside.
export type V3 = [number, number, number];
export type M3 = [V3, V3, V3];
export interface Box { min: V3; max: V3; }

const RAD = Math.PI / 180;

export function rng(seed: number): () => number {
  let s = (seed >>> 0) || 1;
  return () => ((s = (s * 1664525 + 1013904223) >>> 0) / 4294967296);
}

/** Rz(z) * Ry(y) * Rx(x), angles in degrees (the engine's detail order). */
export function rotMat(deg: V3): M3 {
  const [x, y, z] = deg.map((v) => v * RAD);
  const [cx, sx, cy, sy, cz, sz] = [Math.cos(x), Math.sin(x), Math.cos(y), Math.sin(y), Math.cos(z), Math.sin(z)];
  return [
    [cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx],
    [sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx],
    [-sy, cy * sx, cy * cx],
  ];
}

/** The [x, y, z] degrees of a rotation matrix in rotMat's order (y kept in -90..90). */
export function eulerOf(r: M3): V3 {
  const sy = Math.max(-1, Math.min(1, -r[2][0]));
  const y = Math.asin(sy);
  let x: number, z: number;
  if (Math.abs(sy) < 0.99999) { x = Math.atan2(r[2][1], r[2][2]); z = Math.atan2(r[1][0], r[0][0]); }
  else { x = Math.atan2(-r[1][2], r[1][1]); z = 0; }
  return [x / RAD, y / RAD, z / RAD];
}

export function mul(a: M3, b: M3): M3 {
  return [0, 1, 2].map((i) => [0, 1, 2].map((j) => a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j])) as M3;
}
export function apply(r: M3, v: V3): V3 {
  return [0, 1, 2].map((i) => r[i][0] * v[0] + r[i][1] * v[1] + r[i][2] * v[2]) as V3;
}

/** Rotation by `deg` about the unit axis. */
export function axisAngle(axis: V3, deg: number): M3 {
  const [x, y, z] = axis, a = deg * RAD, c = Math.cos(a), s = Math.sin(a), t = 1 - c;
  return [
    [t * x * x + c, t * x * y - s * z, t * x * z + s * y],
    [t * x * y + s * z, t * y * y + c, t * y * z - s * x],
    [t * x * z - s * y, t * y * z + s * x, t * z * z + c],
  ];
}

/** The rotation that leans the up axis toward the ground normal by `align` of the slope (0 upright, 1 square to the
 * surface), capped at maxTilt degrees, then a jitter of up to `jitter` degrees in a random direction. And the tilt. */
export function tiltTo(normal: V3, align: number, maxTilt: number, jitter = 0, R: () => number = Math.random): { m: M3; tilt: number } {
  const n = normal, len = Math.hypot(...n) || 1, ny = n[1] / len;
  const slope = Math.acos(Math.max(-1, Math.min(1, ny))) / RAD;
  const h = Math.hypot(n[0], n[2]);
  let m: M3 = [[1, 0, 0], [0, 1, 0], [0, 0, 1]];
  const lean = Math.min(slope * align, maxTilt);
  // up x n = (nz, 0, -nx) (normalised): turning up about it by `lean` leans the top downhill along n's horizontal part
  if (h > 1e-6 && lean > 0) m = axisAngle([n[2] / h, 0, -n[0] / h], lean);
  if (jitter > 0) {
    const a = R() * Math.PI * 2;
    m = mul(axisAngle([Math.cos(a), 0, Math.sin(a)], (R() * 2 - 1) * jitter), m);
  }
  return { m, tilt: Math.acos(Math.max(-1, Math.min(1, m[1][1]))) / RAD };
}

/** A piece of a crag in the crag's frame: its centre offset (the mesh's box centre, x along the crag's downhill axis), its
 * own rotation (degrees) and its size (map units along the mesh's own x, y, z). */
export interface Piece { at: V3; rot: V3; size: V3; }
export type Archetype = "spire" | "ledge" | "stack" | "tor" | "single";
export const ARCHETYPES: Archetype[] = ["spire", "ledge", "stack", "tor", "single"];

/** A seeded crag `height` tall and about `width` across (map units), its base at y 0, made of 1-4 interpenetrating pieces.
 * stretch: the most a piece is drawn taller than wide (the mesh's normals light pale when stretched far: keep <= 2.2). */
export function kitbash(kind: Archetype, height: number, width: number, seed: number, stretch = 2): Piece[] {
  const R = rng(seed), J = (a: number) => (R() * 2 - 1) * a, yaw = () => R() * 360;
  const st = Math.max(1, Math.min(2.2, stretch));
  const pc = (cx: number, cz: number, y0: number, w: number, h: number, lean = 8, d = w): Piece =>
    ({ at: [cx, y0 + h / 2, cz], rot: [J(lean), yaw(), J(lean)], size: [w, h, d * (0.85 + R() * 0.3)] });
  const out: Piece[] = [];
  if (kind === "single") out.push(pc(0, 0, 0, width, Math.min(height, width * st), 6));
  else if (kind === "tor") {
    // a squat stack of two or three blocks, each smaller and stepped off the one below
    const n = 2 + (R() < 0.5 ? 1 : 0); let y = 0, w = width;
    for (let i = 0; i < n; i++) {
      const h = (height / n) * (1.25 + R() * 0.2);
      out.push(pc(J(w * 0.12), J(w * 0.12), y, w, Math.min(h, w * st), 10));
      y += h * 0.62; w *= 0.72 + R() * 0.1;
    }
  } else if (kind === "spire") {
    // a tall core, a narrower shard rising out of its top beside the axis, and 1-2 shoulders leaning on it
    const w = Math.max(width * 0.55, height / st);
    const h0 = Math.min(height * 0.72, w * st);
    out.push(pc(0, 0, 0, w, h0, 5));
    const w1 = w * (0.55 + R() * 0.15), h1 = Math.min(height * 0.55, w1 * st);
    out.push(pc(J(w * 0.15), J(w * 0.15), height - h1, w1, h1, 9));
    const ns = 1 + (R() < 0.6 ? 1 : 0);
    for (let i = 0; i < ns; i++) {
      const a = R() * Math.PI * 2, ws = w * (0.6 + R() * 0.2), hs = Math.min(height * (0.3 + R() * 0.15), ws * st);
      out.push(pc(Math.cos(a) * w * 0.45, Math.sin(a) * w * 0.45, 0, ws, hs, 14));
    }
  } else if (kind === "ledge") {
    // steps falling downhill (+x): each a flatter block, lower and further out, overlapping the one above
    const n = 3 + (R() < 0.5 ? 1 : 0);
    for (let i = 0; i < n; i++) {
      const f = 1 - i / n, w = width * (0.55 - i * 0.06) * (0.9 + R() * 0.2), h = Math.max(height * f, w * 0.55);
      out.push(pc(i * width * 0.24 - width * 0.3, J(width * 0.1), 0, w, Math.min(h, w * st), 6, w * 0.8));
    }
  } else {
    // stack (a sea stack): a tall narrow column of 2-3 blocks, each narrower, with a slight wander
    const n = 2 + (R() < 0.6 ? 1 : 0); let y = 0, w = width;
    for (let i = 0; i < n; i++) {
      const h = Math.min((height / n) * 1.3, w * st);
      out.push(pc(J(w * 0.08), J(w * 0.08), y, w, h, 6));
      y += h * 0.72; w *= 0.78 + R() * 0.08;
    }
  }
  return out;
}

/** The lowest and highest y of the box `b` scaled by s, rotated by r and centred at c (its box centre at c). */
function yRange(b: Box, s: V3, r: M3, c: V3): [number, number] {
  const mid: V3 = [0, 1, 2].map((i) => (b.min[i] + b.max[i]) / 2) as V3;
  let lo = Infinity, hi = -Infinity;
  for (let k = 0; k < 8; k++) {
    const p: V3 = [0, 1, 2].map((i) => ((k >> i) & 1 ? b.max[i] : b.min[i]) - mid[i]) as V3;
    const y = apply(r, [p[0] * s[0], p[1] * s[1], p[2] * s[2]])[1] + c[1];
    lo = Math.min(lo, y); hi = Math.max(hi, y);
  }
  return [lo, hi];
}

export interface Pose { pos: V3; rot: V3; scale: V3; }

/** Every piece's pose in the level: the crag at (x, z), turned by yaw (its +x downhill), tilted by `tilt`, and lowered so
 * `sink` of its height (its pieces' boxes, rotated) is below `groundLow` (the lowest ground sample of its footprint).
 * With `groundLow` an array (one lowest sample per piece: conform), every piece that stands on the crag's base is seated on
 * its own ground instead (`sink` of that piece's height under its own lowest sample), so a ledge steps down a slope with the
 * ground and nothing is buried on the uphill side; pieces raised off the base (a spire's shard, a tor's upper blocks) move
 * with the first piece. pos is the detail's origin (the mesh's box centre offset by the mesh's own centre), rot degrees,
 * scale per axis. */
export function composeCrag(pieces: Piece[], mesh: Box, x: number, z: number, yaw: number, tilt: M3, groundLow: number | number[], sink: number):
  { poses: Pose[]; bottom: number; top: number; height: number; radius: number } {
  const crag = mul(tilt, rotMat([0, yaw, 0]));
  const ms: V3 = [0, 1, 2].map((i) => Math.max(1e-6, mesh.max[i] - mesh.min[i])) as V3;
  const mid: V3 = [0, 1, 2].map((i) => (mesh.min[i] + mesh.max[i]) / 2) as V3;
  const parts = pieces.map((p) => {
    const r = mul(crag, rotMat(p.rot)), s: V3 = [p.size[0] / ms[0], p.size[1] / ms[1], p.size[2] / ms[2]];
    const c = apply(crag, p.at);
    return { r, s, c, y: yRange(mesh, s, r, c) };
  });
  const lo = Math.min(...parts.map((q) => q.y[0])), hi = Math.max(...parts.map((q) => q.y[1])), h = hi - lo;
  let dys: number[];
  if (Array.isArray(groundLow)) {
    const H = Math.max(...pieces.map((p) => p.at[1] + p.size[1] / 2)) || 1;
    const based = pieces.map((p) => p.at[1] - p.size[1] / 2 < 0.05 * H);
    const own = parts.map((q, i) => groundLow[i] - sink * (q.y[1] - q.y[0]) - q.y[0]);
    dys = parts.map((_, i) => (based[i] ? own[i] : own[0]));
  } else {
    const dy = groundLow - sink * h - lo;
    dys = parts.map(() => dy);
  }
  let radius = 0, bottom = Infinity, top = -Infinity;
  const poses = parts.map((q, i) => {
    const off = apply(q.r, [mid[0] * q.s[0], mid[1] * q.s[1], mid[2] * q.s[2]]);   // the origin sits -off from the box centre
    radius = Math.max(radius, Math.hypot(q.c[0], q.c[2]) + Math.max(q.s[0] * ms[0], q.s[2] * ms[2]) / 2);
    bottom = Math.min(bottom, q.y[0] + dys[i]); top = Math.max(top, q.y[1] + dys[i]);
    return { pos: [x + q.c[0] - off[0], q.c[1] + dys[i] - off[1], z + q.c[2] - off[2]] as V3, rot: eulerOf(q.r), scale: q.s };
  });
  return { poses, bottom, top, height: top - bottom, radius };
}

/** The crag's footprint for ground samples: its centre and 8 points round each piece's box (yaw only, no tilt), and the
 * radius it covers. */
export function footprint(pieces: Piece[], x: number, z: number, yaw: number): { pts: [number, number][]; radius: number } {
  const r = rotMat([0, yaw, 0]), pts: [number, number][] = [[x, z]];
  let radius = 0;
  for (const p of pieces) {
    const c = apply(r, p.at), half = Math.max(p.size[0], p.size[2]) / 2 * 0.85;
    radius = Math.max(radius, Math.hypot(c[0], c[2]) + half);
    for (let i = 0; i < 8; i++) { const a = i / 8 * Math.PI * 2; pts.push([x + c[0] + Math.cos(a) * half, z + c[2] + Math.sin(a) * half]); }
  }
  return { pts, radius };
}

/** What a Test showed another theme's piece does in a level of this theme (the offline preview is by resource name and
 * cannot tell): "draws", "never draws", or what it draws as. Keys: level theme, then resource (lower case). Added to as
 * pieces are checked in game (docs/erg-mcp.md, Detail layer). */
export const DRAW_CHECKS: Record<string, Record<string, string>> = {
  PREHISTORIC: {
    horror5: "draws as a pale blue-grey cobbled boulder (much paler than its preview albedo #36374a), lighter than the basalt terrain",
    pirate5: "draws as a yellow rock with red veins (round 3 probe; it and PIRATE10 stood 6 apart and looked alike)",
    pirate10: "draws as a yellow rock with red veins (round 3 probe; it and Pirate5 stood 6 apart and looked alike)",
    wildwest6: "not seen in the round 3 probe (scale 3.5 and 1.2-2.4 on the NW flank): check again",
    arabian6: "never draws (any scale or rotation)",
    camelot16: "draws as a purple plant pod",
    camelot19: "draws as a purple plant pod",
    // detail layer rounds 7-8 (scratchpad caldet/r7 and r8 flora check sheets); resource names that draw the same in every base
    wildwest1: "draws as a big agave-like spiky grass clump (GhostHill, DoomCanyon)",
    wildwest2: "draws as a leafy bush with a trunk (GhostHill, DoomCanyon)",
    wildwest15: "draws as a turtle shell: reads as a pickup",
    wildwest16: "draws as a stack of green and purple bulbs: reads as a pickup",
    wildwest18: "draws as a dinosaur skull",
    england7: "draws too small to see at gameplay distance",
  },
};

/** The catalog's verdict for a piece of `pieceTheme` in a level of `levelTheme`. */
export function drawCheck(levelTheme: string | null, pieceTheme: string, resource: string): string | undefined {
  if (!levelTheme) return undefined;
  const known = DRAW_CHECKS[levelTheme]?.[resource.toLowerCase()];
  if (known) return known;
  return levelTheme === pieceTheme ? "same theme" : "other theme: unchecked (it may not draw, or draw as another piece): check it once in a Test";
}
