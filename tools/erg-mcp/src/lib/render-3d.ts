// A perspective view of the level's voxels for render_view: every exposed face of every voxel frame (rotated and scaled
// ones through their grid -> map matrix) rasterised as a quad into a z-buffer, lit by a sun and an ambient term per
// face, then the water plane (translucent), distance fog, silhouette edges, and the markers as small billboards. The sky
// is a gradient by time of day. Map units throughout (1 voxel = 1 map unit, y up, x east, z south). Pure: no I/O.
import type { Vec3 } from "../core";
import type { Marker, MarkerKind, Region } from "./inspect-field";
import type { Rgb } from "./inspect-png";
import { FACES, MIN_AREA, gridCorner, splitQuad, triangleNormal } from "./drape-mesh";
import { isSolid, maskOf, material, secondOf, type Grid } from "./voxel";

// ------------------------------------------------------------------ per-corner blends

// The top face's corners (FACES[2]: (x0 z0), (x0 z1), (x1 z1), (x1 z0)) and the bit each has in a word's corner mask
// (terrain/blend.ts CORNER_BIT, top row: [[3, 2], [1, 0]] indexed [z][x]). The game blends the second material in over
// the corners whose bit is set; render_view approximates it per triangle: the top face is split in two and each triangle
// is tinted the mix of the two materials by how many of its three corners carry the second one (0, 1/3, 2/3, 1), so a
// blended trail edge reads as a gradient of two or three steps instead of a flat tile of the first material.
const TOP_CORNER_BITS = [3, 1, 0, 2];
const TOP_TRIS = [[0, 1, 2], [0, 2, 3]];

/** The tints of the top face's two triangles for a word with a second material, or null when it has none. */
export function topBlendTints(wd: number, tint: (m: number) => Rgb): [Rgb, Rgb] | null {
  const m2 = secondOf(wd);
  if (m2 < 0) return null;
  const mask = maskOf(wd);
  if (!mask) return null;
  const a = tint(material(wd)), b = tint(m2);
  return TOP_TRIS.map((t) => {
    const n = t.filter((v) => (mask >>> TOP_CORNER_BITS[v]) & 1).length, f = n / 3;
    return [0, 1, 2].map((i) => Math.round(a[i] * (1 - f) + b[i] * f)) as Rgb;
  }) as [Rgb, Rgb];
}

// ------------------------------------------------------------------ cameras

/** A perspective camera: the eye, the point looked at, the vertical field of view in degrees. */
export interface Camera { pos: Vec3; target: Vec3; fov: number; }

const RAD = Math.PI / 180;
const sub = (a: Vec3, b: Vec3): Vec3 => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const dot = (a: Vec3, b: Vec3) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const cross = (a: Vec3, b: Vec3): Vec3 => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const norm = (a: Vec3): Vec3 => { const l = Math.hypot(a[0], a[1], a[2]) || 1; return [a[0] / l, a[1] / l, a[2] / l]; };

/** The unit vector from a point toward a bearing (degrees clockwise from north, -z) at an elevation (degrees up). */
export function bearingDir(azimuth: number, elevation: number): Vec3 {
  const a = azimuth * RAD, e = elevation * RAD;
  return [Math.cos(e) * Math.sin(a), Math.sin(e), -Math.cos(e) * Math.cos(a)];
}

/** A camera looking at `target` from `distance` along a bearing and elevation. */
export const orbitCamera = (target: Vec3, azimuth: number, elevation: number, distance: number, fov: number): Camera => {
  const d = bearingDir(azimuth, elevation);
  return { pos: [target[0] + d[0] * distance, target[1] + d[1] * distance, target[2] + d[2] * distance], target, fov };
};

/** The bearing (degrees clockwise from north), elevation and distance of a camera's eye seen from its target. */
export function cameraAngles(c: Camera): { azimuth: number; elevation: number; distance: number } {
  const v = sub(c.pos, c.target), distance = Math.hypot(v[0], v[1], v[2]);
  const azimuth = ((Math.atan2(v[0], -v[2]) / RAD) + 360) % 360;
  return { azimuth, elevation: distance ? Math.asin(Math.max(-1, Math.min(1, v[1] / distance))) / RAD : 0, distance };
}

/** The camera's basis: forward, right and up (right = forward x world up, so x east is to the right looking north). */
function basis(c: Camera): { f: Vec3; r: Vec3; u: Vec3 } {
  const f = norm(sub(c.target, c.pos));
  let r = cross(f, [0, 1, 0]);
  if (Math.hypot(r[0], r[1], r[2]) < 1e-6) r = [1, 0, 0];   // looking straight up or down: east stays right
  r = norm(r);
  return { f, r, u: cross(r, f) };
}

/** The 8 corners of a box. */
export const boxCorners = (min: Vec3, max: Vec3): Vec3[] =>
  [0, 1, 2, 3, 4, 5, 6, 7].map((k) => [k & 1 ? max[0] : min[0], k & 2 ? max[1] : min[1], k & 4 ? max[2] : min[2]] as Vec3);

/** The distance along a bearing at which every point fits the view (aspect = width / height), with a margin. */
export function fitDistance(points: Vec3[], target: Vec3, azimuth: number, elevation: number, fov: number, aspect: number, margin = 1.08): number {
  const { f, r, u } = basis(orbitCamera(target, azimuth, elevation, 1, fov));
  const tv = Math.tan((fov * RAD) / 2), th = tv * aspect;
  let D = 1;
  for (const p of points) {
    const q = sub(p, target), a = dot(q, r), b = dot(q, u), d = dot(q, f);
    // the point at depth D + d must have |a| <= (D + d) tan(h/2), |b| <= (D + d) tan(v/2)
    D = Math.max(D, (Math.abs(a) * margin) / th - d, (Math.abs(b) * margin) / tv - d, 1 - d);
  }
  return D;
}

// ------------------------------------------------------------------ lighting by time of day

export interface Look {
  zenith: Rgb; horizon: Rgb; below: Rgb;    // the sky at the top, at the horizon, and under it (no ground drawn there)
  sun: Vec3;                                  // toward the sun (or the moon), unit
  sunColour: [number, number, number];        // multiplies the diffuse light
  ambient: number; diffuse: number;
  water: Rgb;                                 // the water's own colour
}

export const LOOKS: Record<"DAY" | "EVENING" | "NIGHT", Look> = {
  DAY: { zenith: [86, 142, 212], horizon: [208, 226, 240], below: [150, 162, 172], sun: bearingDir(150, 52), sunColour: [1.0, 0.97, 0.9], ambient: 0.42, diffuse: 0.7, water: [36, 98, 158] },
  EVENING: { zenith: [62, 70, 132], horizon: [240, 158, 102], below: [96, 78, 82], sun: bearingDir(250, 16), sunColour: [1.0, 0.78, 0.55], ambient: 0.4, diffuse: 0.78, water: [54, 66, 112] },
  NIGHT: { zenith: [8, 12, 30], horizon: [38, 50, 86], below: [20, 24, 36], sun: bearingDir(200, 38), sunColour: [0.62, 0.72, 1.0], ambient: 0.4, diffuse: 0.5, water: [16, 32, 62] },
};

export const lookFor = (tod: string | undefined): Look => LOOKS[(tod ?? "DAY").toUpperCase() as keyof typeof LOOKS] ?? LOOKS.DAY;

// ------------------------------------------------------------------ voxel geometry

// The faces and their corners (a draped block's sheared) are drape-mesh.ts's, shared with its mesh checks.
export { gridCorner };

/** A grid's face normals in map units: the inverse transpose of its matrix (fromMap's rows, transposed), normalised. */
function faceNormals(g: Grid): Vec3[] {
  const q = g.fromMap;
  return FACES.map(({ d }) => norm([q[0] * d[0] + q[4] * d[1] + q[8] * d[2], q[1] * d[0] + q[5] * d[1] + q[9] * d[2], q[2] * d[0] + q[6] * d[1] + q[10] * d[2]]));
}

// ------------------------------------------------------------------ the renderer

export interface ViewInput {
  grids: Grid[];
  tint: (m: number) => Rgb;
  tintSide?: (m: number) => Rgb;   // the colour of a material's sides (its side texture's), for every face but a top; default tint
  normaliseTop?: boolean;          // scale the light so a level top in the sun draws exactly its tint (a graded tint is the game's colour of such a top)
  look: Look;
  water: number | null;        // map y of the water plane, or null for none
  markers?: Marker[];
  fog?: number;                // map units at which fog hides about 63 % (0 or none: no fog)
  clip?: Region | null;        // draw only voxels whose centre is in this xz box
  edges?: boolean;             // darken silhouette edges (default true)
  meshes?: { tris: ArrayLike<number>; rgb: Rgb }[];   // extra triangles (scenery meshes, map units, 9 numbers each), drawn two-sided in a flat colour
}

export interface PlacedMarker { kind: MarkerKind; name: string; pos: Vec3; px: [number, number]; visible: boolean; }

export interface View {
  w: number; h: number; rgb: Uint8Array;
  faces: number;               // faces rasterised (front-facing, in front of the eye)
  framesDrawn: number;         // voxel frames with at least one face rasterised
  framesTotal: number;
  markers: PlacedMarker[];     // markers in the picture, with their pixel
  project: (p: Vec3) => [number, number, number] | null;   // map point -> pixel x, y and depth, or null behind the eye
}

export const MARKER_RGB: Record<MarkerKind, Rgb> = {    // as render_topdown's
  spawn: [60, 220, 60], crate: [250, 220, 40], telepad: [40, 220, 230], trigger: [230, 60, 220], minefactory: [250, 120, 20], object: [250, 250, 250],
};

const NEAR = 0.05;

/** A cheap per-cell hash in [-1, 1], to vary each voxel's shade a little so a flat field still shows its cells. */
const jitter = (id: number, x: number, y: number, z: number) => {
  let h = (x * 73856093) ^ (y * 19349663) ^ (z * 83492791) ^ (id * 2654435761);
  h = Math.imul(h ^ (h >>> 13), 1274126177);
  return ((h ^ (h >>> 16)) & 1023) / 511.5 - 1;
};

/** Renders the voxels from `cam` into w x h pixels (ss: supersampling factor per side, box-filtered down). */
export function renderView(input: ViewInput, cam: Camera, w: number, h: number, ss = 1): View {
  const W = w * ss, H = h * ss;
  const { f, r, u } = basis(cam);
  const E = cam.pos;
  const fpx = H / 2 / Math.tan((cam.fov * RAD) / 2);
  const cx = W / 2, cy = H / 2;
  const inv = new Float32Array(W * H);          // 1 / view depth of the nearest face (0: nothing)
  const col = new Uint32Array(W * H);           // its colour, 0xRRGGBB
  const look = input.look, L = look.sun;

  // ---- rasterising: a triangle in screen space with 1/z at each vertex (1/z is linear in screen space)
  const tri = (ax: number, ay: number, az: number, bx: number, by: number, bz: number, qx: number, qy: number, qz: number, c: number) => {
    const area = (bx - ax) * (qy - ay) - (by - ay) * (qx - ax);
    if (Math.abs(area) < 1e-12) return;
    const x0 = Math.max(0, Math.ceil(Math.min(ax, bx, qx) - 0.5)), x1 = Math.min(W - 1, Math.floor(Math.max(ax, bx, qx) - 0.5));
    const y0 = Math.max(0, Math.ceil(Math.min(ay, by, qy) - 0.5)), y1 = Math.min(H - 1, Math.floor(Math.max(ay, by, qy) - 0.5));
    if (x0 > x1 || y0 > y1) return;
    const k = 1 / area, eps = -1e-7;
    // barycentric weights of a and b as linear functions of the pixel centre
    const a0x = -(qy - by) * k, b0x = -(ay - qy) * k;
    for (let py = y0; py <= y1; py++) {
      const yc = py + 0.5, xs = x0 + 0.5;
      let wa = ((bx - xs) * (qy - yc) - (by - yc) * (qx - xs)) * k;
      let wb = ((qx - xs) * (ay - yc) - (qy - yc) * (ax - xs)) * k;
      let i = py * W + x0;
      for (let px = x0; px <= x1; px++, i++, wa += a0x, wb += b0x) {
        const wq = 1 - wa - wb;
        if (wa < eps || wb < eps || wq < eps) continue;
        const iz = wa * az + wb * bz + wq * qz;
        if (iz > inv[i]) { inv[i] = iz; col[i] = c; }
      }
    }
  };

  // a face's corners in view space, clipped against the near plane, fanned into triangles
  const vx = new Float64Array(8), vy = new Float64Array(8), vz = new Float64Array(8);
  const sx = new Float64Array(8), sy = new Float64Array(8), sz = new Float64Array(8);
  const qx = new Float64Array(4), qy = new Float64Array(4), qz = new Float64Array(4);
  const quad = (pts: Float64Array, c: number, corners = 4): boolean => {
    let n = 0, front = 0;
    for (let k = 0; k < corners; k++) {
      const dx = pts[k * 3] - E[0], dy = pts[k * 3 + 1] - E[1], dz = pts[k * 3 + 2] - E[2];
      qx[k] = dx * r[0] + dy * r[1] + dz * r[2];
      qy[k] = dx * u[0] + dy * u[1] + dz * u[2];
      qz[k] = dx * f[0] + dy * f[1] + dz * f[2];
      if (qz[k] >= NEAR) front++;
    }
    if (!front) return false;
    if (front === corners) {
      for (let k = 0; k < corners; k++) { vx[k] = qx[k]; vy[k] = qy[k]; vz[k] = qz[k]; }
      n = corners;
    } else {
      for (let k = 0; k < corners; k++) {   // Sutherland-Hodgman against z = NEAR
        const j = (k + 1) % corners, ink = qz[k] >= NEAR, inj = qz[j] >= NEAR;
        if (ink) { vx[n] = qx[k]; vy[n] = qy[k]; vz[n] = qz[k]; n++; }
        if (ink !== inj) {
          const t = (NEAR - qz[k]) / (qz[j] - qz[k]);
          vx[n] = qx[k] + (qx[j] - qx[k]) * t; vy[n] = qy[k] + (qy[j] - qy[k]) * t; vz[n] = NEAR; n++;
        }
      }
    }
    let minx = Infinity, maxx = -Infinity, miny = Infinity, maxy = -Infinity;
    for (let k = 0; k < n; k++) {
      sz[k] = 1 / vz[k];
      sx[k] = cx + vx[k] * fpx * sz[k];
      sy[k] = cy - vy[k] * fpx * sz[k];
      if (sx[k] < minx) minx = sx[k]; if (sx[k] > maxx) maxx = sx[k];
      if (sy[k] < miny) miny = sy[k]; if (sy[k] > maxy) maxy = sy[k];
    }
    if (maxx < 0 || minx > W || maxy < 0 || miny > H) return false;
    for (let k = 1; k + 1 < n; k++) tri(sx[0], sy[0], sz[0], sx[k], sy[k], sz[k], sx[k + 1], sy[k + 1], sz[k + 1], c);
    return true;
  };

  // ---- the faces: a face of a solid cell is exposed when its neighbour in the same grid is empty or outside it
  let faces = 0, framesDrawn = 0;
  const pts = new Float64Array(12), centre = new Float64Array(3), tri3 = new Float64Array(9);
  const litRaw = (nm: Vec3) => {
    const diff = Math.max(0, dot(nm, L)) * look.diffuse, amb = look.ambient * (0.78 + 0.22 * nm[1]);
    return [amb + diff * look.sunColour[0], amb + diff * look.sunColour[1], amb + diff * look.sunColour[2]];
  };
  const topL = input.normaliseTop ? litRaw([0, 1, 0]).map((v) => Math.max(v, 1e-3)) : [1, 1, 1];
  const lit = (nm: Vec3) => { const l = litRaw(nm); return [l[0] / topL[0], l[1] / topL[1], l[2] / topL[2]]; };
  const sideTint = input.tintSide;
  const clip = input.clip ?? null;
  for (const g of input.grids) {
    const [X, Y, Z] = g.size, words = g.words;
    const normals = faceNormals(g);
    // per face direction: is it facing a camera anywhere (an affine grid's faces share a normal), and its light
    const light = normals.map((nm) => lit(nm));
    let drew = false;
    for (let z = 0; z < Z; z++)
      for (let x = 0; x < X; x++) {
        const at = (z * X + x) * Y;
        for (let y = 0; y < Y; y++) {
          const wd = words[at + y];
          if (!isSolid(wd)) continue;
          if (clip) {
            gridCorner(g, x + 0.5, y + 0.5, z + 0.5, centre, 0);
            if (centre[0] < clip.x0 || centre[0] >= clip.x1 || centre[2] < clip.z0 || centre[2] >= clip.z1) continue;
          }
          let tintC: Rgb | null = null, jit = 0, sideC: Rgb | null = null;
          for (let k = 0; k < 6; k++) {
            const d = FACES[k].d, nx = x + d[0], ny = y + d[1], nz = z + d[2];
            if (nx >= 0 && ny >= 0 && nz >= 0 && nx < X && ny < Y && nz < Z && isSolid(words[(nz * X + nx) * Y + ny])) continue;
            const c = FACES[k].c;
            for (let v = 0; v < 4; v++) gridCorner(g, x + c[v][0], y + c[v][1], z + c[v][2], pts, v * 3);
            if (g.frame?.corners) {
              // a draped block's face is not flat: its two triangles are culled and lit by their own normals
              if (!tintC) { tintC = input.tint(material(wd)); jit = 1 + 0.035 * jitter(g.id, x, y, z); }
              const idx = splitQuad(pts);
              for (let t = 0; t < 6; t += 3) {
                const { n: tn, area } = triangleNormal(pts, idx[t], idx[t + 1], idx[t + 2]);
                if (area < MIN_AREA) continue;
                const a0 = idx[t] * 3;
                if ((pts[a0] - E[0]) * tn[0] + (pts[a0 + 1] - E[1]) * tn[1] + (pts[a0 + 2] - E[2]) * tn[2] >= 0) continue;
                for (let v = 0; v < 3; v++) for (let q = 0; q < 3; q++) tri3[v * 3 + q] = pts[idx[t + v] * 3 + q];
                const li = lit(tn);
                // a skin's sheared top is still its top; its other faces are sides
                const fc = sideTint && k !== 2 ? (sideC ??= sideTint(material(wd))) : tintC;
                const rr = Math.min(255, Math.round(fc[0] * li[0] * jit)), gg = Math.min(255, Math.round(fc[1] * li[1] * jit)), bb = Math.min(255, Math.round(fc[2] * li[2] * jit));
                if (quad(tri3, (rr << 16) | (gg << 8) | bb, 3)) { faces++; drew = true; }
              }
              continue;
            }
            // back faces: the eye is behind the face's plane
            const nm = normals[k];
            if ((pts[0] - E[0]) * nm[0] + (pts[1] - E[1]) * nm[1] + (pts[2] - E[2]) * nm[2] >= 0) continue;
            if (!tintC) { tintC = input.tint(material(wd)); jit = 1 + 0.035 * jitter(g.id, x, y, z); }
            const li = light[k];
            // a top with a second material on some corners: two triangles, each the mix of its corners
            const bl = k === 2 ? topBlendTints(wd, input.tint) : null;
            if (bl) {
              for (let t = 0; t < 2; t++) {
                for (let v = 0; v < 3; v++) for (let q = 0; q < 3; q++) tri3[v * 3 + q] = pts[TOP_TRIS[t][v] * 3 + q];
                const c = bl[t], rr = Math.min(255, Math.round(c[0] * li[0] * jit)), gg = Math.min(255, Math.round(c[1] * li[1] * jit)), bb = Math.min(255, Math.round(c[2] * li[2] * jit));
                if (quad(tri3, (rr << 16) | (gg << 8) | bb, 3)) { faces++; drew = true; }
              }
              continue;
            }
            const fc = sideTint && k !== 2 ? (sideC ??= sideTint(material(wd))) : tintC;
            const rr = Math.min(255, Math.round(fc[0] * li[0] * jit)), gg = Math.min(255, Math.round(fc[1] * li[1] * jit)), bb = Math.min(255, Math.round(fc[2] * li[2] * jit));
            if (quad(pts, (rr << 16) | (gg << 8) | bb)) { faces++; drew = true; }
          }
        }
      }
    if (drew) framesDrawn++;
  }
  // ---- extra meshes (render_view {scenery: true}): two-sided, lit by the normal that faces the eye
  for (const m of input.meshes ?? []) {
    const t = m.tris;
    for (let i = 0; i + 8 < t.length; i += 9) {
      for (let q = 0; q < 9; q++) tri3[q] = t[i + q];
      const ux = tri3[3] - tri3[0], uy = tri3[4] - tri3[1], uz = tri3[5] - tri3[2], wx = tri3[6] - tri3[0], wy = tri3[7] - tri3[1], wz = tri3[8] - tri3[2];
      let nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
      const nl = Math.hypot(nx, ny, nz);
      if (nl < 1e-12) continue;
      nx /= nl; ny /= nl; nz /= nl;
      if ((tri3[0] - E[0]) * nx + (tri3[1] - E[1]) * ny + (tri3[2] - E[2]) * nz > 0) { nx = -nx; ny = -ny; nz = -nz; }
      const li = lit([nx, ny, nz]);
      const rr = Math.min(255, Math.round(m.rgb[0] * li[0])), gg = Math.min(255, Math.round(m.rgb[1] * li[1])), bb = Math.min(255, Math.round(m.rgb[2] * li[2]));
      if (quad(tri3, (rr << 16) | (gg << 8) | bb, 3)) faces++;
    }
  }

  // ---- shading per pixel: sky, water, fog, edges
  const out = new Uint8Array(W * H * 3);
  const fog = input.fog && input.fog > 0 ? input.fog : 0;
  const edges = input.edges !== false;
  const water = input.water;
  const under = water !== null && E[1] < water;
  const [hR, hG, hB] = look.horizon, [zR, zG, zB] = look.zenith, [bR, bG, bB] = look.below, [wR, wG, wB] = look.water;
  const farOf = (j: number, z0: number) => inv[j] <= 0 || 1 / inv[j] > z0 * 1.04 + 0.4 * ss;
  for (let py = 0; py < H; py++)
    for (let px = 0; px < W; px++) {
      const i = py * W + px;
      // the ray through the pixel centre: d = f + r a - u b (depth 1 along f), |d| = len
      const a = (px + 0.5 - cx) / fpx, b = (py + 0.5 - cy) / fpx;
      const dy = f[1] + r[1] * a - u[1] * b;
      const len = Math.hypot(f[0] + r[0] * a - u[0] * b, dy, f[2] + r[2] * a - u[2] * b), ry = dy / len;
      let cR: number, cG: number, cB: number;
      let dist: number;                          // distance along the ray to what the pixel shows (Infinity: sky)
      if (inv[i] > 0) {
        const c = col[i];
        cR = c >>> 16; cG = (c >>> 8) & 255; cB = c & 255;
        dist = len / inv[i];
        // silhouette edges: a neighbour much further away (or sky) darkens the near side
        if (edges) {
          const z0 = 1 / inv[i];
          if ((px > 0 && farOf(i - 1, z0)) || (px < W - 1 && farOf(i + 1, z0)) || (py > 0 && farOf(i - W, z0)) || (py < H - 1 && farOf(i + W, z0))) {
            cR *= 0.72; cG *= 0.72; cB *= 0.72;
          }
        }
      } else {
        // the sky: horizon to zenith above, to the haze colour below
        const up = ry >= 0, t = up ? Math.pow(Math.min(1, ry), 0.55) : Math.min(1, -ry * 3);
        cR = hR + ((up ? zR : bR) - hR) * t;
        cG = hG + ((up ? zG : bG) - hG) * t;
        cB = hB + ((up ? zB : bB) - hB) * t;
        dist = Infinity;
      }
      // the water plane, translucent: more opaque with the depth of water the ray crosses, lighter at grazing angles
      if (water !== null && !under && Math.abs(ry) > 1e-9) {
        const t = (water - E[1]) / ry;           // distance along the unit ray
        if (t > 0 && t < dist) {
          const fres = 0.45 * Math.pow(1 - Math.min(1, Math.abs(ry)), 4);
          const sR = wR + (hR - wR) * fres, sG = wG + (hG - wG) * fres, sB = wB + (hB - wB) * fres;
          let alpha: number;
          if (dist === Infinity) { alpha = 0.7; cR = wR * 0.55; cG = wG * 0.55; cB = wB * 0.55; }   // open water: its deep colour under
          else alpha = Math.min(0.88, 0.38 + 0.12 * (dist - t) * Math.abs(ry));
          cR += (sR - cR) * alpha; cG += (sG - cG) * alpha; cB += (sB - cB) * alpha;
          dist = t;
        }
      } else if (under && dist !== Infinity) {
        cR += (wR - cR) * 0.5; cG += (wG - cG) * 0.5; cB += (wB - cB) * 0.5;
      }
      if (fog && dist !== Infinity) {
        const k = 1 - Math.exp(-dist / fog);
        cR += (hR - cR) * k; cG += (hG - cG) * k; cB += (hB - cB) * k;
      }
      out[i * 3] = Math.round(cR); out[i * 3 + 1] = Math.round(cG); out[i * 3 + 2] = Math.round(cB);
    }

  // ---- down to the asked size
  let rgb = out;
  if (ss > 1) {
    rgb = new Uint8Array(w * h * 3);
    const n = ss * ss;
    for (let y = 0; y < h; y++)
      for (let x = 0; x < w; x++)
        for (let k = 0; k < 3; k++) {
          let s = 0;
          for (let j = 0; j < ss; j++) for (let i2 = 0; i2 < ss; i2++) s += out[((y * ss + j) * W + x * ss + i2) * 3 + k];
          rgb[(y * w + x) * 3 + k] = Math.round(s / n);
        }
  }

  // ---- markers: a billboard 1 map unit across, its centre 1.4 above the spot, on a thin post; hollow when hidden
  const project = (p: Vec3): [number, number, number] | null => {
    const q = sub(p, E), z = dot(q, f);
    if (z < NEAR) return null;
    return [(cx + (dot(q, r) * fpx) / z) / ss, (cy - (dot(q, u) * fpx) / z) / ss, z];
  };
  const placed: PlacedMarker[] = [];
  const put = (x: number, y: number, c: Rgb) => {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    const i = (y * w + x) * 3;
    rgb[i] = c[0]; rgb[i + 1] = c[1]; rgb[i + 2] = c[2];
  };
  const depthAt = (x: number, y: number) => {   // the view depth of the ground at an output pixel (Infinity: none)
    const X = Math.min(W - 1, Math.max(0, Math.floor(x * ss))), Yp = Math.min(H - 1, Math.max(0, Math.floor(y * ss)));
    const v = inv[Yp * W + X];
    return v > 0 ? 1 / v : Infinity;
  };
  for (const m of input.markers ?? []) {
    const top: Vec3 = [m.pos[0], m.pos[1] + 1.4, m.pos[2]];
    const pc = project(top), pb = project(m.pos);
    if (!pc || !pb || pc[0] < 0 || pc[1] < 0 || pc[0] >= w || pc[1] >= h) continue;
    const half = Math.max(3, Math.min(14, Math.round((0.5 * fpx) / pc[2] / ss)));
    const visible = pc[2] <= depthAt(pc[0], pc[1]) + 0.6 || pb[2] <= depthAt(pb[0], pb[1]) + 0.6;
    const X0 = Math.round(pc[0]), Y0 = Math.round(pc[1]), c = MARKER_RGB[m.kind], dark: Rgb = [0, 0, 0];
    for (let y = Math.round(pc[1]) + half; y <= Math.round(pb[1]); y++) put(X0, y, dark);
    for (let y = -half - 1; y <= half + 1; y++)
      for (let x = -half - 1; x <= half + 1; x++) {
        const edge = Math.abs(x) === half + 1 || Math.abs(y) === half + 1, rim = Math.abs(x) >= half - 1 || Math.abs(y) >= half - 1;
        if (edge) put(X0 + x, Y0 + y, dark);
        else if (visible || rim) put(X0 + x, Y0 + y, c);
      }
    placed.push({ kind: m.kind, name: m.name, pos: m.pos, px: [Math.round(pc[0]), Math.round(pc[1])], visible });
  }

  return { w, h, rgb, faces, framesDrawn, framesTotal: input.grids.length, markers: placed, project };
}
