// Greedy voxel mesher: the faces between solid and empty voxels of each frame, merged into rectangles per material and
// direction, then moved into the level by voxelWorld. Self-contained so the Web Worker bundle stays small.
// Voxel (x, y, z) is the unit cube [x, x+1] x [y, y+1] x [z, z+1] in grid space; word index (z*X + x)*Y + y.

export interface MeshFrame {
  id: number;
  size: [number, number, number];
  world: number[];            // row-major 3x4 (.xan units)
  voxels: Uint32Array;
}

export interface MeshData {
  positions: Float32Array;    // 4 vertices per quad
  normals: Float32Array;
  colors: Uint8Array;         // RGB per vertex
  index: Uint32Array;         // 6 per quad
  quadFrame: Uint32Array;     // frame id of each quad (triangle t belongs to quad t >> 1)
  quads: number;
  truncated: boolean;         // the quad budget ran out; later frames are left out
}

/** Quads a whole level may mesh into (about 130 bytes each once in GPU-ready buffers). */
export const MAX_LEVEL_QUADS = 1 << 21;

export const isSolid = (v: number) => (v & 3) === 3;
export const materialOf = (v: number) => (v >>> 2) & 63;

/** Quads of one frame in frame space: [axis, dir, plane, u0, v0, u1, v1, material] per quad; at most maxQuads. */
export function frameQuads(size: readonly number[], voxels: Uint32Array, maxQuads = Infinity): Int16Array {
  const [X, Y, Z] = size;
  let out = new Int16Array(0), used = 0;
  const push = (...q: number[]) => {
    if (used + 8 > out.length) {
      const grown = new Int16Array(Math.max(64, out.length * 2));
      grown.set(out);
      out = grown;
    }
    out.set(q, used);
    used += 8;
  };
  if (!X || !Y || !Z || voxels.length < X * Y * Z) return out;
  const dims = [X, Y, Z];
  const at = (p: number[]) => voxels[(p[2] * X + p[0]) * Y + p[1]];
  const p = [0, 0, 0];
  for (let d = 0; d < 3; d++) {
    const u = (d + 1) % 3, v = (d + 2) % 3;
    const nu = dims[u], nv = dims[v];
    const mask = new Int32Array(nu * nv);
    for (let s = 0; s <= dims[d]; s++) {
      for (let j = 0; j < nv; j++)
        for (let i = 0; i < nu; i++) {
          p[u] = i;
          p[v] = j;
          let a = 0, b = 0;
          if (s > 0) { p[d] = s - 1; a = at(p); }
          if (s < dims[d]) { p[d] = s; b = at(p); }
          const sa = s > 0 && isSolid(a), sb = s < dims[d] && isSolid(b);
          mask[j * nu + i] = sa && !sb ? materialOf(a) + 1 : !sa && sb ? -(materialOf(b) + 1) : 0;
        }
      for (let j = 0; j < nv; j++)
        for (let i = 0; i < nu; ) {
          const m = mask[j * nu + i];
          if (!m) { i++; continue; }
          let w = 1;
          while (i + w < nu && mask[j * nu + i + w] === m) w++;
          let h = 1;
          grow: for (; j + h < nv; h++)
            for (let k = 0; k < w; k++) if (mask[(j + h) * nu + i + k] !== m) break grow;
          for (let jj = 0; jj < h; jj++) mask.fill(0, (j + jj) * nu + i, (j + jj) * nu + i + w);
          if (used / 8 >= maxQuads) return out.subarray(0, used);
          push(d, m > 0 ? 1 : -1, s, i, j, i + w, j + h, Math.abs(m) - 1);
          i += w;
        }
    }
  }
  return out.subarray(0, used);
}

/** Meshes several frames into one buffer set, at most maxQuads quads. `palette` is 64 RGB triples. */
export function meshFrames(frames: MeshFrame[], palette: Uint8Array, maxQuads = MAX_LEVEL_QUADS): MeshData {
  const all: { f: MeshFrame; q: Int16Array }[] = [];
  let quads = 0, truncated = false;
  for (const f of frames) {
    const q = frameQuads(f.size, f.voxels, maxQuads - quads + 1);
    if (quads + q.length / 8 > maxQuads) {
      truncated = true;
      break;
    }
    quads += q.length / 8;
    all.push({ f, q });
  }
  const positions = new Float32Array(quads * 12), normals = new Float32Array(quads * 12);
  const colors = new Uint8Array(quads * 12), index = new Uint32Array(quads * 6), quadFrame = new Uint32Array(quads);
  let n = 0;
  const c = [0, 0, 0], corner = [0, 0, 0];
  for (const { f, q } of all) {
    const m = f.world;
    // Normals go through the cofactor matrix (the inverse transpose up to a positive factor when det > 0).
    const cof = [
      m[5] * m[10] - m[6] * m[9], m[6] * m[8] - m[4] * m[10], m[4] * m[9] - m[5] * m[8],
      m[2] * m[9] - m[1] * m[10], m[0] * m[10] - m[2] * m[8], m[1] * m[8] - m[0] * m[9],
      m[1] * m[6] - m[2] * m[5], m[2] * m[4] - m[0] * m[6], m[0] * m[5] - m[1] * m[4],
    ];
    const det = m[0] * cof[0] + m[1] * cof[1] + m[2] * cof[2];
    const sign = det < 0 ? -1 : 1;
    for (let k = 0; k < q.length; k += 8) {
      const d = q[k], dir = q[k + 1], s = q[k + 2], u0 = q[k + 3], v0 = q[k + 4], u1 = q[k + 5], v1 = q[k + 6], mat = q[k + 7];
      const u = (d + 1) % 3, v = (d + 2) % 3;
      const nl = [0, 0, 0];
      nl[d] = dir;
      let nx = (cof[0] * nl[0] + cof[1] * nl[1] + cof[2] * nl[2]) * sign;
      let ny = (cof[3] * nl[0] + cof[4] * nl[1] + cof[5] * nl[2]) * sign;
      let nz = (cof[6] * nl[0] + cof[7] * nl[1] + cof[8] * nl[2]) * sign;
      const len = Math.hypot(nx, ny, nz) || 1;
      nx /= len; ny /= len; nz /= len;
      const corners = dir > 0 ? [[u0, v0], [u1, v0], [u1, v1], [u0, v1]] : [[u0, v0], [u0, v1], [u1, v1], [u1, v0]];
      c[0] = palette[mat * 3]; c[1] = palette[mat * 3 + 1]; c[2] = palette[mat * 3 + 2];
      for (let i = 0; i < 4; i++) {
        corner[d] = s;
        corner[u] = corners[i][0];
        corner[v] = corners[i][1];
        const o = (n * 4 + i) * 3;
        positions[o] = m[0] * corner[0] + m[1] * corner[1] + m[2] * corner[2] + m[3];
        positions[o + 1] = m[4] * corner[0] + m[5] * corner[1] + m[6] * corner[2] + m[7];
        positions[o + 2] = m[8] * corner[0] + m[9] * corner[1] + m[10] * corner[2] + m[11];
        normals[o] = nx; normals[o + 1] = ny; normals[o + 2] = nz;
        colors[o] = c[0]; colors[o + 1] = c[1]; colors[o + 2] = c[2];
      }
      // A mirroring frame (det < 0) flips the winding back.
      const base = n * 4;
      const tri = sign > 0 ? [0, 1, 2, 0, 2, 3] : [0, 2, 1, 0, 3, 2];
      for (let i = 0; i < 6; i++) index[n * 6 + i] = base + tri[i];
      quadFrame[n] = f.id;
      n++;
    }
  }
  return { positions, normals, colors, index, quadFrame, quads, truncated };
}

export function emptyMesh(): MeshData {
  return meshFrames([], new Uint8Array(192));
}
