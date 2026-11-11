// A base level's backdrop: the scenery details round and under the playing area, found from each piece's bounds in the
// level, and the lowest water that covers the base's ground planes. Only what the level's details hold: the distant set
// of Multi.DinerMight (the yellow table plane, the green barrel, the pink buildings, the mushroom) is none of its 137
// details nor of its 558 voxel frames (checked 2026-11-08), so it is not found here and no patch op reaches it. A piece's bounds are its preview mesh's (the
// server's /erg/assets/<key>.glb, the Erg panel's 3D view's own) through the detail's matrix; a piece whose mesh is not
// served is placed by its position alone. Map units throughout.
import { Frames, WORLD_PER_XAN, apply as applyMat, type Detail, type Scene, type Vec3 } from "../core";
import type { Session } from "../session";
import { grids, isSolid, type Grid } from "./voxel";

export interface Bounds { min: Vec3; max: Vec3; }

type M4 = number[];   // column-major 4 x 4, as glTF writes it
const mul4 = (a: M4, b: M4): M4 => { const o = new Array<number>(16).fill(0); for (let c = 0; c < 4; c++) for (let r = 0; r < 4; r++) for (let k = 0; k < 4; k++) o[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k]; return o; };
function trs(n: { matrix?: number[]; translation?: number[]; rotation?: number[]; scale?: number[] }): M4 {
  if (n.matrix?.length === 16) return n.matrix;
  const [x, y, z, w] = n.rotation ?? [0, 0, 0, 1], [sx, sy, sz] = n.scale ?? [1, 1, 1], [tx, ty, tz] = n.translation ?? [0, 0, 0];
  return [(1 - 2 * (y * y + z * z)) * sx, 2 * (x * y + z * w) * sx, 2 * (x * z - y * w) * sx, 0,
    2 * (x * y - z * w) * sy, (1 - 2 * (x * x + z * z)) * sy, 2 * (y * z + x * w) * sy, 0,
    2 * (x * z + y * w) * sz, 2 * (y * z - x * w) * sz, (1 - 2 * (x * x + y * y)) * sz, 0, tx, ty, tz, 1];
}

/** A GLB's bounds (its JSON chunk: each mesh primitive's POSITION accessor min/max through its node's transform, the
 * node hierarchy of the default scene included, so quantized positions come out dequantized), or null. */
export function glbBounds(bytes: Uint8Array): Bounds | null {
  if (bytes.length < 20) return null;
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (v.getUint32(0, true) !== 0x46546c67) return null;            // "glTF"
  const len = v.getUint32(12, true), type = v.getUint32(16, true);
  if (type !== 0x4e4f534a || 20 + len > bytes.length) return null;  // "JSON"
  type Node = { mesh?: number; children?: number[]; matrix?: number[]; translation?: number[]; rotation?: number[]; scale?: number[] };
  let doc: { scene?: number; scenes?: { nodes?: number[] }[]; nodes?: Node[]; meshes?: { primitives?: { attributes?: Record<string, number> }[] }[]; accessors?: { min?: number[]; max?: number[] }[] };
  try {
    doc = JSON.parse(new TextDecoder().decode(bytes.subarray(20, 20 + len)));
  } catch {
    return null;
  }
  const min: Vec3 = [Infinity, Infinity, Infinity], max: Vec3 = [-Infinity, -Infinity, -Infinity];
  const addMesh = (mi: number, m: M4) => {
    for (const p of doc.meshes?.[mi]?.primitives ?? []) {
      const a = p.attributes?.POSITION === undefined ? undefined : doc.accessors?.[p.attributes.POSITION];
      if (!a?.min || !a.max || a.min.length < 3) continue;
      for (const x of [a.min[0], a.max[0]]) for (const y of [a.min[1], a.max[1]]) for (const z of [a.min[2], a.max[2]]) {
        const q = [m[0] * x + m[4] * y + m[8] * z + m[12], m[1] * x + m[5] * y + m[9] * z + m[13], m[2] * x + m[6] * y + m[10] * z + m[14]];
        for (let i = 0; i < 3; i++) { min[i] = Math.min(min[i], q[i]); max[i] = Math.max(max[i], q[i]); }
      }
    }
  };
  const walk = (ni: number, parent: M4, depth: number) => {
    const n = doc.nodes?.[ni];
    if (!n || depth > 32) return;
    const m = mul4(parent, trs(n));
    if (n.mesh !== undefined) addMesh(n.mesh, m);
    for (const c of n.children ?? []) walk(c, m, depth + 1);
  };
  const I: M4 = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
  const roots = doc.scenes?.[doc.scene ?? 0]?.nodes;
  if (roots?.length) for (const r of roots) walk(r, I, 0);
  else (doc.meshes ?? []).forEach((_, mi) => addMesh(mi, I));
  return Number.isFinite(min[0]) ? { min, max } : null;
}

// mesh bounds per preview key, per server URL (a key's mesh does not change while the server runs)
const meshCache = new Map<string, Promise<Bounds | null>>();

/** The preview mesh's bounds of a resource in `scene` (map units, the mesh's own frame), or null where the server has none. */
export async function meshBounds(s: Session, scene: Scene, resource: string): Promise<Bounds | null> {
  const key = scene.previews?.[resource] ?? scene.previews?.[resource.toLowerCase()];
  if (!key) return null;
  const conn = await s.ensure();
  const base = new URL(conn.url), token = base.searchParams.get("k");
  const url = `http://${base.host}/erg/assets/${encodeURIComponent(key)}.glb${token ? `?k=${encodeURIComponent(token)}` : ""}`;
  const ck = `${base.host}|${key}`;
  let p = meshCache.get(ck);
  if (!p) {
    p = fetch(url).then(async (r) => (r.ok ? glbBounds(new Uint8Array(await r.arrayBuffer())) : null), () => null)
      .then((b) => b && { min: b.min.map((x) => x / WORLD_PER_XAN) as Vec3, max: b.max.map((x) => x / WORLD_PER_XAN) as Vec3 });
    meshCache.set(ck, p);
  }
  return p;
}

/** A detail's bounds in the level: its mesh's box through the detail's matrix (the eight corners), or its position. */
export function placedBounds(frames: Frames, d: Detail, mesh: Bounds | null): Bounds & { mesh: boolean } {
  if (!mesh) { const p = frames.detailWorld(d); return { min: [...p] as Vec3, max: [...p] as Vec3, mesh: false }; }
  const m = frames.detailMatrix(d), min: Vec3 = [Infinity, Infinity, Infinity], max: Vec3 = [-Infinity, -Infinity, -Infinity];
  for (const x of [mesh.min[0], mesh.max[0]]) for (const y of [mesh.min[1], mesh.max[1]]) for (const z of [mesh.min[2], mesh.max[2]]) {
    const q = applyMat(m, [x, y, z]);
    for (let i = 0; i < 3; i++) { min[i] = Math.min(min[i], q[i]); max[i] = Math.max(max[i], q[i]); }
  }
  return { min, max, mesh: true };
}

/** The xz box of the voxel frames that hold solid ground (the playing area), or null. */
export function solidExtent(gs: Grid[]): { x0: number; z0: number; x1: number; z1: number } | null {
  let x0 = Infinity, z0 = Infinity, x1 = -Infinity, z1 = -Infinity;
  for (const g of gs) {
    if (!g.words.some((w) => isSolid(w))) continue;
    x0 = Math.min(x0, g.bounds.min[0]); z0 = Math.min(z0, g.bounds.min[2]); x1 = Math.max(x1, g.bounds.max[0]); z1 = Math.max(z1, g.bounds.max[2]);
  }
  return Number.isFinite(x0) ? { x0, z0, x1, z1 } : null;
}

export type BackdropKind = "plane" | "distant" | "large";
export interface BackdropPiece { detail: Detail; bounds: Bounds & { mesh: boolean }; kind: BackdropKind; }

/** The scenery of `scene` that is backdrop: a flat piece at least 40 map units both ways (a ground or table plane), a piece
 * wholly outside the playing area (grown by 4), or one wider than half of it and a quarter of it the other way (a set
 * piece looming over it; not a long thin strip). */
export async function backdropOf(s: Session, scene: Scene, gs: Grid[] = []): Promise<{ pieces: BackdropPiece[]; extent: ReturnType<typeof solidExtent>; meshes: number; without: number }> {
  const frames = new Frames(scene);
  // the playing area: the voxel frames that hold ground, or without the voxels (another base) its spawns and objects grown by 20
  let ext = solidExtent(gs);
  if (!gs.length) {
    const ps = scene.details.filter((d) => d.role === "spawn" || d.role === "object").map((d) => frames.detailWorld(d));
    if (ps.length) ext = { x0: Math.min(...ps.map((p) => p[0])) - 20, z0: Math.min(...ps.map((p) => p[2])) - 20, x1: Math.max(...ps.map((p) => p[0])) + 20, z1: Math.max(...ps.map((p) => p[2])) + 20 };
  }
  const out: BackdropPiece[] = [];
  let meshes = 0, without = 0;
  for (const d of scene.details) {
    if (d.role !== "scenery") continue;
    const mb = await meshBounds(s, scene, d.resource);
    if (mb) meshes++; else without++;
    const b = placedBounds(frames, d, mb);
    const dx = b.max[0] - b.min[0], dy = b.max[1] - b.min[1], dz = b.max[2] - b.min[2], wide = Math.max(dx, dz);
    let kind: BackdropKind | null = null;
    if (b.mesh && Math.min(dx, dz) >= 40 && dy <= Math.max(2, wide * 0.08)) kind = "plane";
    else if (ext && (b.max[0] < ext.x0 - 4 || b.min[0] > ext.x1 + 4 || b.max[2] < ext.z0 - 4 || b.min[2] > ext.z1 + 4)) kind = "distant";
    else if (ext && b.mesh && wide >= 0.5 * Math.max(ext.x1 - ext.x0, ext.z1 - ext.z0) && Math.min(dx, dz) >= 0.25 * Math.max(ext.x1 - ext.x0, ext.z1 - ext.z0)) kind = "large";
    if (kind) out.push({ detail: d, bounds: b, kind });
  }
  return { pieces: out, extent: ext, meshes, without };
}

/** The lowest water (map y) that covers the base's ground planes that reach out past the playing area (the sea would
 * show them otherwise), or null when it has none. */
export function coverLevel(pieces: BackdropPiece[], ext: ReturnType<typeof solidExtent>): { level: number; piece: BackdropPiece } | null {
  let best: { level: number; piece: BackdropPiece } | null = null;
  for (const p of pieces) {
    if (p.kind !== "plane") continue;
    const b = p.bounds, out = !ext || b.min[0] < ext.x0 || b.max[0] > ext.x1 || b.min[2] < ext.z0 || b.max[2] > ext.z1;
    if (out && (!best || b.max[1] > best.level)) best = { level: b.max[1], piece: p };
  }
  return best;
}

/** The water check for set_water and save: a warning when the project's water is under the cover level. */
export async function waterBackdropWarning(s: Session): Promise<{ warning?: string; coverLevel?: { map: number; world: number; piece: string } } | null> {
  const store = s.store;
  if (store.scene.water.level === null) return null;
  const { pieces, extent } = await backdropOf(s, store.scene, grids(store));
  const c = coverLevel(pieces, extent);
  if (!c) return null;
  const r = (v: number) => Math.round(v * 100) / 100, w = store.scene.water.level / WORLD_PER_XAN;
  const cl = { map: r(c.level), world: r(c.level * WORLD_PER_XAN), piece: `${c.piece.detail.name} (${c.piece.detail.resource}, #${c.piece.detail.id})` };
  return w >= c.level ? { coverLevel: cl } : { coverLevel: cl,
    warning: `the water (${r(w * WORLD_PER_XAN)} world) is under the base's ground plane ${cl.piece} (its top ${cl.world} world): the sea shows that mesh instead of water; raise the water over ${cl.world} world or remove it (remove_backdrop)` };
}
