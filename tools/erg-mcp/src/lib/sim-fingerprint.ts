// The open level's simulation fingerprint: a stable hash of everything that can change what the game's simulation does
// (voxel occupancy and collision-shaping bits, frames and their drape corners, trims, the details the sim reads: spawn
// and object knots, cameras and look-at points, collision volumes, anything not proven visual), the objects and their
// settings, water, the surround, the databank, the level script), with a hash per part. The proven-visual channels
// (docs/erg-mcp.md, "Detail layer"): scenery, light, emitter and sound details whose names carry no engine tag but
// VISIBLE, and the primary material of solid voxels, are hashed apart (`visual`), so a dressing pass can show it changed
// only those. Pure: no network, the script text is passed in.
import { createHash } from "node:crypto";
import { deriveRole, type Detail, type EditorStore, type Frame } from "../core";
import { grids } from "./voxel";
import { canonical, normScript, storedObject } from "./project-hash";

/** Voxel word bits the sim reads: 0-1 solid, 8-23 second-material flags, material and the corner mask (the mask shapes
 * collision: docs/erg.md). Bits 2-7, the primary material, are the visual `paint` part. */
export const SIM_WORD_MASK = 0x00ffff03;
export const PAINT_MASK = 0xfc;
/** Detail roles whose details draw or sound only (each proven, docs/erg-mcp.md "Detail layer"). */
export const VISUAL_ROLES = new Set(["scenery", "light", "emitter", "sound"]);
/** Engine tags in a detail name that may give it behaviour (MORTAL: blown away by explosions); such a detail is sim. */
const SIM_TAG = /(MORTAL|COLLIDE|COLLISION|PERM|SLIPPY|TEAMBASE|SPAWN|STATUE|WORM|CRATE|MINE|TRIG|TELEPAD|ERGBLAST)/i;

export type DetailClass = "sim" | "visual";

/** Whether a detail is visual only: a visual role (derived from its name and resource as the server does) and no tag. */
export function classifyDetail(d: Pick<Detail, "name" | "resource">): { cls: DetailClass; role: string; why: string } {
  const role = deriveRole(d.name, d.resource);
  if (!VISUAL_ROLES.has(role)) return { cls: "sim", role, why: `role ${role}` };
  const tag = d.name.match(SIM_TAG);
  if (tag) return { cls: "sim", role, why: `name carries ${tag[1].toUpperCase()}` };
  return { cls: "visual", role, why: `role ${role}` };
}

const r4 = (v: number) => Math.round(v * 1e4) / 1e4;
const v3 = (v: readonly number[] | undefined) => (v ? [r4(v[0]), r4(v[1]), r4(v[2])] : null);
const sha = (s: string) => createHash("sha256").update(s).digest("hex");
/** Bytes into a hash (node accepts any ArrayBufferView; src/node.d.ts types strings only). */
const bytes = (v: ArrayBufferView) => new Uint8Array(v.buffer, v.byteOffset, v.byteLength) as unknown as string;

function frameOut(f: Frame) {
  return { id: f.id, parent: f.parent, name: f.name, pos: v3(f.pos), rot: v3(f.rot), scale: v3(f.scale), size: f.size, folder: f.folder,
    new: !!f.new, corners: f.corners && f.corners.some((c) => c !== 0) ? f.corners.map(r4) : null };
}
const detailOut = (d: Detail) => ({ frame: d.frame, name: d.name, resource: d.resource, pos: v3(d.pos), rot: v3(d.rot), scale: v3(d.scale), voxelPos: v3(d.voxelPos) });
/** Details sorted by their content (ids shift when details are added; the content is what the game loads). */
const sortedDetails = (list: Detail[]) => list.map((d) => canonical(detailOut(d))).sort();

export interface Fingerprint {
  sim: string;                                   // 64 hex: every sim part
  visual: string;                                // 64 hex: the visual parts
  parts: Record<string, string>;                 // part -> 16 hex
  visualParts: Record<string, string>;
  counts: Record<string, number>;
  frameHashes?: Record<string, string>;          // frame id -> 12 hex of its sim voxels (detail: true)
  paintHashes?: Record<string, string>;
  simDetails?: { name: string; resource: string; role: string; why: string }[];
}

/** The fingerprint of the open level (its store) and its script text. */
export function simFingerprint(store: EditorStore, script: string, o: { detail?: boolean; strict?: boolean } = {}): Fingerprint {
  const sc = store.scene;
  const parts: Record<string, string> = {}, visualParts: Record<string, string> = {}, counts: Record<string, number> = {};
  const put = (k: string, v: unknown) => { parts[k] = sha(canonical(v)).slice(0, 16); };
  const putV = (k: string, v: unknown) => { visualParts[k] = sha(canonical(v)).slice(0, 16); };

  put("base", { key: sc.base.key, source: sc.base.source, sha256: sc.base.sha256 });
  put("databank", sc.databank);
  put("kind", sc.kind ?? null);
  put("water", sc.water);
  put("spawnMode", sc.spawns);
  // the surround (heightmap ground) collides: its mode, and the painted heights and blend when it is painted
  const sur = sc.hmp.mode === "paint" ? store.surround : null;
  put("surround", { mode: sc.hmp.mode, heights: sur ? createHash("sha256").update(bytes(sur.heights)).update(bytes(sur.blend)).digest("hex") : null });
  put("frames", sc.frames.map(frameOut).sort((a, b) => a.id - b.id));
  put("trims", (sc.trims ?? []).map((t) => ({ frame: t.frame, min: t.min, size: t.size })).sort((a, b) => a.frame - b.frame));
  put("objects", (sc.objects ?? []).map((x) => storedObject(x as unknown as Record<string, unknown>)).map((x) => canonical(x)).sort());
  put("script", normScript(script));

  // voxels: occupancy and the collision-shaping bits per frame; the primary material of solid cells apart
  const vox = createHash("sha256"), paint = createHash("sha256");
  const frameHashes: Record<string, string> = {}, paintHashes: Record<string, string> = {};
  let solid = 0, frames = 0;
  for (const g of grids(store).sort((a, b) => a.id - b.id)) {
    const w = g.words, simW = new Uint32Array(w.length), paintW = new Uint8Array(w.length);
    for (let i = 0; i < w.length; i++) {
      simW[i] = w[i] & SIM_WORD_MASK;
      if ((w[i] & 3) === 3) { solid++; paintW[i] = (w[i] & PAINT_MASK) >>> 2; }
    }
    const head = `${g.id}:${g.size.join("x")};`;
    const fh = createHash("sha256").update(head).update(bytes(simW)).digest("hex");
    const ph = createHash("sha256").update(head).update(bytes(paintW)).digest("hex");
    vox.update(fh);
    paint.update(ph);
    frameHashes[g.id] = fh.slice(0, 12);
    paintHashes[g.id] = ph.slice(0, 12);
    frames++;
  }
  parts.voxels = vox.digest("hex").slice(0, 16);
  counts.voxelFrames = frames;
  counts.solidVoxels = solid;

  // details: by class; spawn and object knots are named apart in the breakdown
  const simD: Detail[] = [], visD: Record<string, Detail[]> = { scenery: [], light: [], emitter: [], sound: [] };
  const simWhy: { name: string; resource: string; role: string; why: string }[] = [];
  for (const d of sc.details) {
    const c = classifyDetail(d);
    if (c.cls === "visual" && !(o.strict && c.role !== "scenery" && c.role !== "light")) visD[c.role].push(d);
    else { simD.push(d); simWhy.push({ name: d.name, resource: d.resource, role: c.role, why: c.why }); }
  }
  const byRole = (r: string) => simD.filter((d) => deriveRole(d.name, d.resource) === r);
  put("spawnKnots", sortedDetails(byRole("spawn")));
  put("objectKnots", sortedDetails(byRole("object")));
  put("cameras", sortedDetails(byRole("camera")));
  put("otherSimDetails", sortedDetails(simD.filter((d) => !["spawn", "object", "camera"].includes(deriveRole(d.name, d.resource)))));
  counts.simDetails = simD.length;
  for (const [k, list] of Object.entries(visD)) { putV(k, sortedDetails(list)); counts[k] = list.length; }
  if (o.strict) parts.paint = paint.digest("hex").slice(0, 16);
  else visualParts.paint = paint.digest("hex").slice(0, 16);

  const fold = (p: Record<string, string>) => sha(Object.keys(p).sort().map((k) => `${k}=${p[k]}`).join("\n"));
  const out: Fingerprint = { sim: fold(parts), visual: fold(visualParts), parts, visualParts, counts };
  if (o.detail) Object.assign(out, { frameHashes, paintHashes, simDetails: simWhy });
  return out;
}

/** The parts that differ between two fingerprints (and, with frameHashes on both, the frames whose voxels differ). */
export function diffFingerprints(a: Partial<Fingerprint>, b: Fingerprint) {
  const diff = (x: Record<string, string> = {}, y: Record<string, string> = {}) =>
    [...new Set([...Object.keys(x), ...Object.keys(y)])].filter((k) => x[k] !== y[k]).sort();
  const out: Record<string, unknown> = { simSame: a.sim === b.sim, simParts: diff(a.parts, b.parts), visualParts: diff(a.visualParts, b.visualParts) };
  if (a.frameHashes && b.frameHashes) out.voxelFrames = diff(a.frameHashes, b.frameHashes).slice(0, 40).map(Number);
  return out;
}
