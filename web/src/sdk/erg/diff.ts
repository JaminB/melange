// toPatch: the edit between the loaded base and the edited scene, as an erg-patch/1 (details by src, voxels as runs).
// applyPatch is its inverse on the model, as the server applies it.
import {
  PATCH_FORMAT, deriveRole, type Databank, type Detail, type DetailFields, type Op, type Patch, type Scene, type Vec3, type VoxelRun,
} from "./scene";

const FIELD_KEYS = ["name", "resource", "pos", "rot", "scale", "voxelPos"] as const;
const DATABANK_KEYS: (keyof Databank)[] = ["theme", "timeOfDay", "materialFile", "heightmapBase", "heightmapSecond"];

const sameVec = (a: Vec3, b: Vec3) => a[0] === b[0] && a[1] === b[1] && a[2] === b[2];
const copyVec = (v: Vec3): Vec3 => [v[0], v[1], v[2]];

export interface VoxelData {
  base: Map<number, Uint32Array>;     // blob ref -> the base's voxels
  edited: Map<number, Uint32Array>;   // blob ref -> the edited voxels
}

/** Runs of changed words, [start, count, value], merging neighbours with the same new value. */
export function voxelRuns(before: Uint32Array, after: Uint32Array): VoxelRun[] {
  const runs: VoxelRun[] = [];
  const n = Math.min(before.length, after.length);
  for (let i = 0; i < n; i++) {
    if (before[i] === after[i]) continue;
    const last = runs[runs.length - 1];
    if (last && last[0] + last[1] === i && last[2] === after[i]) last[1]++;
    else runs.push([i, 1, after[i]]);
  }
  return runs;
}

export function toPatch(base: Scene, edited: Scene, voxels?: VoxelData): Patch {
  const patch: Patch = {
    format: PATCH_FORMAT,
    stem: edited.stem,
    title: edited.title,
    base: { key: base.base.key, source: base.base.source, sha256: { ...base.base.sha256 } },
    databank: {},
    water: { level: edited.water.level },
    spawns: { mode: edited.spawns.mode },
    hmp: { mode: edited.hmp.mode },
    ops: [],
  };
  for (const k of DATABANK_KEYS) if (edited.databank[k] !== base.databank[k] && edited.databank[k]) patch.databank![k] = edited.databank[k];

  const editedBySrc = new Map<number, Detail>();
  for (const d of edited.details) if (d.src !== null) editedBySrc.set(d.src, d);
  const ops: Op[] = [];
  for (const b of base.details) {
    if (b.src === null) continue;
    const e = editedBySrc.get(b.src);
    if (!e) {
      ops.push({ op: "remove", src: b.src });
      continue;
    }
    const changed: DetailFields = {};
    if (e.name !== b.name) changed.name = e.name;
    if (e.resource !== b.resource) changed.resource = e.resource;
    for (const k of ["pos", "rot", "scale", "voxelPos"] as const) if (!sameVec(e[k], b[k])) changed[k] = copyVec(e[k]);
    if (Object.keys(changed).length) ops.push({ op: "set", src: b.src, ...changed });
  }
  for (const e of edited.details) {
    if (e.src !== null) continue;
    const detail: DetailFields & { name: string; resource: string; pos: Vec3 } = { name: e.name, resource: e.resource, pos: copyVec(e.pos) };
    if (!sameVec(e.rot, [0, 0, 0])) detail.rot = copyVec(e.rot);
    if (!sameVec(e.scale, [1, 1, 1])) detail.scale = copyVec(e.scale);
    if (!sameVec(e.voxelPos, [0, 0, 0])) detail.voxelPos = copyVec(e.voxelPos);
    ops.push({ op: "add", frame: e.frame, detail });
  }
  if (voxels) {
    for (const f of base.frames) {
      if (f.voxels === null) continue;
      const b = voxels.base.get(f.voxels), e = voxels.edited.get(f.voxels);
      if (!b || !e) continue;
      const runs = voxelRuns(b, e);
      if (runs.length) ops.push({ op: "voxels", frame: f.id, runs });
    }
  }
  patch.ops = ops;
  return patch;
}

/** True when the patch changes nothing against its base scene. */
export function isEmptyPatch(p: Patch, base: Scene): boolean {
  return p.ops.length === 0 && Object.keys(p.databank ?? {}).length === 0 && (p.water?.level ?? null) === base.water.level &&
    (p.spawns?.mode ?? "random") === base.spawns.mode && (p.hmp?.mode ?? "copy") === base.hmp.mode && p.title === base.title &&
    p.stem === base.stem;
}

/** Applies a patch to a copy of the scene (voxels ops change the given blobs in place). Throws on a bad reference. */
export function applyPatch(scene: Scene, p: Patch, voxels?: Map<number, Uint32Array>): Scene {
  const s: Scene = structuredClone(scene);
  s.stem = p.stem;
  s.title = p.title;
  for (const k of DATABANK_KEYS) if (p.databank?.[k]) s.databank[k] = p.databank[k]!;
  s.water = { level: p.water?.level ?? null };
  s.spawns = { mode: p.spawns?.mode ?? "random" };
  s.hmp = { mode: p.hmp?.mode ?? "copy" };
  let nextId = s.details.reduce((m, d) => Math.max(m, d.id), 0) + 1;
  p.ops.forEach((op, i) => {
    switch (op.op) {
      case "set": {
        const d = s.details.find((x) => x.src === op.src);
        if (!d) throw new Error(`ops[${i}].src: ${op.src} is not a detail`);
        for (const k of FIELD_KEYS) {
          const v = op[k];
          if (v !== undefined) (d as unknown as Record<string, unknown>)[k] = Array.isArray(v) ? copyVec(v as Vec3) : v;
        }
        d.role = deriveRole(d.name, d.resource);
        break;
      }
      case "remove": {
        const at = s.details.findIndex((x) => x.src === op.src);
        if (at < 0) throw new Error(`ops[${i}].src: ${op.src} is not a detail`);
        s.details.splice(at, 1);
        break;
      }
      case "add": {
        if (!s.frames.some((f) => f.id === op.frame)) throw new Error(`ops[${i}].frame: ${op.frame} is not a frame`);
        const d = op.detail;
        s.details.push({ id: nextId++, src: null, frame: op.frame, name: d.name, resource: d.resource, pos: copyVec(d.pos),
          rot: d.rot ? copyVec(d.rot) : [0, 0, 0], scale: d.scale ? copyVec(d.scale) : [1, 1, 1],
          voxelPos: d.voxelPos ? copyVec(d.voxelPos) : [0, 0, 0], role: deriveRole(d.name, d.resource) });
        break;
      }
      case "voxels": {
        const f = s.frames.find((x) => x.id === op.frame);
        const v = f && f.voxels !== null ? voxels?.get(f.voxels) : undefined;
        if (!f) throw new Error(`ops[${i}].frame: ${op.frame} is not a frame`);
        if (!v) break;
        for (const [start, count, value] of op.runs) {
          if (start + count > v.length) throw new Error(`ops[${i}]: a run is past the frame's voxels`);
          v.fill(value, start, start + count);
        }
        break;
      }
    }
  });
  return s;
}
