// erg-scene/1 and erg-patch/1: the scene the level service sends and the patch the editor saves (the C++ model is
// src/erg/scene.h and patch.h; the JSON Schemas are docs/erg-scene-1.schema.json and docs/erg-patch-1.schema.json).
// Positions, scales and voxel coordinates are .xan units; water is in world units (20 per .xan unit), sea level 0.

export const SCENE_FORMAT = "erg-scene/1";
export const PATCH_FORMAT = "erg-patch/1";
export const WORLD_PER_XAN = 20;
export const LIMITS = { frames: 4096, details: 65536, frameVoxels: 262144, ops: 20000, patchBytes: 4 << 20, runsPerFrame: 2000 };

export type Vec3 = [number, number, number];
export type Role = "scenery" | "spawn" | "object" | "camera" | "light" | "emitter" | "sound" | "collision" | "marker" | "other";
export const ROLES: readonly Role[] = ["scenery", "spawn", "object", "camera", "light", "emitter", "sound", "collision", "marker", "other"];
export const THEMES = ["ARABIAN", "WILDWEST", "CAMELOT", "PREHISTORIC", "BUILDING", "ARCTIC", "ENGLAND", "HORROR", "LUNAR", "PIRATE", "WAR"] as const;
export const TIMES_OF_DAY = ["DAY", "EVENING", "NIGHT"] as const;
export type SpawnMode = "random" | "knots";
export type HmpMode = "copy" | "none" | "flat";

export interface Sha256Set { xan: string; xom: string; hmp: string | null; }
export interface SceneBase { key: string; source: "game" | "pack"; file: string; sha256: Sha256Set; }
export interface PatchBase { key: string; source: "game" | "pack"; sha256: Sha256Set; }
export interface Registry { levelType: number; themeType: number; scripts: string[]; previewType: number; }
export interface Databank { theme: string; timeOfDay: string; materialFile: string; heightmapBase: string; heightmapSecond: string; }

export interface Frame {
  id: number; parent: number | null; name: string;
  pos: Vec3; rot: Vec3; scale: Vec3;           // rot: Euler radians
  size: Vec3;                                   // X, Y, Z voxels
  voxels: number | null; heightMap: number | null;   // blob refs
  folder: boolean;
}
export interface Detail {
  id: number; src: number | null; frame: number; name: string; resource: string;
  pos: Vec3; rot: Vec3; scale: Vec3; voxelPos: Vec3; role: Role;
}
export interface Blob { ref: number; kind: "voxels" | "heightMap"; frame: number; bytes: number; }

export interface Scene {
  format: typeof SCENE_FORMAT;
  stem: string; title: string;
  base: SceneBase;
  registry: Registry;
  databank: Databank;
  water: { level: number | null };
  spawns: { mode: SpawnMode };
  hmp: { mode: HmpMode };
  units: { worldPerXan: number };
  frames: Frame[];
  details: Detail[];
  blobs: Blob[];
}

export interface DetailFields { name?: string; resource?: string; pos?: Vec3; rot?: Vec3; scale?: Vec3; voxelPos?: Vec3; }
export type VoxelRun = [start: number, count: number, value: number];
export type Op =
  | ({ op: "set"; src: number } & DetailFields)
  | { op: "add"; frame: number; detail: DetailFields & { name: string; resource: string; pos: Vec3 } }
  | { op: "remove"; src: number }
  | { op: "voxels"; frame: number; runs: VoxelRun[] };

export interface Patch {
  format: typeof PATCH_FORMAT;
  stem: string; title: string;
  base: PatchBase;
  databank?: Partial<Databank>;
  water?: { level: number | null };
  spawns?: { mode: SpawnMode };
  hmp?: { mode: HmpMode };
  ops: Op[];
}

export interface Validation { ok: boolean; errors: string[]; }

type Obj = Record<string, unknown>;
const isObj = (v: unknown): v is Obj => typeof v === "object" && v !== null && !Array.isArray(v);
const isInt = (v: unknown, lo: number, hi: number) => typeof v === "number" && Number.isInteger(v) && v >= lo && v <= hi;
const isNum = (v: unknown, lim: number) => typeof v === "number" && Number.isFinite(v) && Math.abs(v) <= lim;
const isVec = (v: unknown, lim = 1e6): v is Vec3 => Array.isArray(v) && v.length === 3 && v.every((c) => isNum(c, lim));
export const printable = (s: unknown, min: number, max: number): s is string =>
  typeof s === "string" && s.length >= min && s.length <= max && /^[\x20-\x7e]*$/.test(s);
const isHex64 = (s: unknown) => typeof s === "string" && /^[0-9a-f]{64}$/.test(s);

class Checker {
  errors: string[] = [];
  fail(path: string, why: string): false {
    if (this.errors.length < 50) this.errors.push(`${path}: ${why}`);
    return false;
  }
  keys(o: Obj, allowed: readonly string[], path: string): boolean {
    for (const k of Object.keys(o)) if (!allowed.includes(k)) return this.fail(path, `unknown key '${k}'`);
    return true;
  }
}

export function validRunValue(v: number): boolean {
  return Number.isInteger(v) && v >= 0 && v <= 0xffffff && ((v & 3) === 0 || (v & 3) === 3);
}

/** Derived exactly as the server does (src/erg/scene.cpp DeriveRole). */
export function deriveRole(name: string, resource: string): Role {
  const n = name.toUpperCase(), r = resource.toUpperCase();
  if (/^WORM\d/.test(n) || r === "CHEESYGRINWORM") return "spawn";
  if (["MINE", "OILDRUM", "MINEFACTORY", "TELEPAD"].includes(n) || r.includes("MINE") || r.includes("OILDRUM") ||
      r.includes("OIL DRUM") || r === "TELEPAD" || r.includes("CRATE") || r === "TARGET") return "object";
  if (r === "CAMERA" || r === "LOOKAT POINT") return "camera";
  if (r === "LIGHT" || n.includes("PNTLGHT")) return "light";
  if (r.includes("PARTICLE EMITTER") || n.includes("EMITTER")) return "emitter";
  if (r === "SOUND EFFECT") return "sound";
  if (r.includes("COLLISION SPHERE") || n.includes("RAND SPAWN VOLUME")) return "collision";
  if (r === "AXIS" || r === "UNIT" || r === "WATER" || n.includes("STATUE SPAWN")) return "marker";
  if (n.includes("VISIBLE") || n.includes("PROP") || n.includes("STANDIN")) return "scenery";
  return "other";
}

function checkDatabank(c: Checker, db: Obj, partial: boolean) {
  if (!c.keys(db, ["theme", "timeOfDay", "materialFile", "heightmapBase", "heightmapSecond"], "databank")) return;
  for (const [k, v] of Object.entries(db)) if (typeof v !== "string") c.fail(`databank.${k}`, "must be a string");
  const theme = db.theme as string | undefined, tod = db.timeOfDay as string | undefined, mf = db.materialFile as string | undefined;
  if ((!partial || theme) && !THEMES.includes(theme as (typeof THEMES)[number])) c.fail("databank.theme", "not one of the eleven themes");
  if ((!partial || tod) && !TIMES_OF_DAY.includes(tod as (typeof TIMES_OF_DAY)[number])) c.fail("databank.timeOfDay", "must be DAY, EVENING or NIGHT");
  if (mf && (!printable(mf, 1, 120) || mf.includes("..") || mf.includes(":") || /^[\\/]/.test(mf) || !/\.txt$/i.test(mf)))
    c.fail("databank.materialFile", "must be a relative .txt path inside the install");
  for (const k of ["heightmapBase", "heightmapSecond"]) if (db[k] && !printable(db[k], 1, 64)) c.fail(`databank.${k}`, "must be 1-64 printable characters");
}

function checkBase(c: Checker, b: unknown, withFile: boolean) {
  if (!isObj(b)) return c.fail("base", "must be an object");
  if (!c.keys(b, withFile ? ["key", "source", "file", "sha256"] : ["key", "source", "sha256"], "base")) return;
  if (!printable(b.key, 1, 79)) c.fail("base.key", "must be 1-79 printable characters");
  if (b.source !== "game" && b.source !== "pack") c.fail("base.source", "must be \"game\" or \"pack\"");
  if (withFile && !printable(b.file, 1, 63)) c.fail("base.file", "must be 1-63 printable characters");
  const sha = b.sha256;
  if (!isObj(sha) || !c.keys(sha, ["xan", "xom", "hmp"], "base.sha256")) return c.fail("base.sha256", "must be an object");
  if (!isHex64(sha.xan) || !isHex64(sha.xom) || (sha.hmp != null && !isHex64(sha.hmp)))
    c.fail("base.sha256", "each hash must be 64 lower-case hex digits");
}

function checkMode(c: Checker, v: unknown, path: string, modes: readonly string[]) {
  if (!isObj(v) || !c.keys(v, ["mode"], path) || !modes.includes(v.mode as string)) c.fail(`${path}.mode`, `must be one of ${modes.join(", ")}`);
}

function checkWater(c: Checker, v: unknown) {
  if (!isObj(v) || !c.keys(v, ["level"], "water")) return c.fail("water", "must be an object");
  if (v.level !== null && v.level !== undefined && !isNum(v.level, 1000)) c.fail("water.level", "must be -1000..1000 world units");
}

export function validateScene(s: unknown): Validation {
  const c = new Checker();
  if (!isObj(s)) return { ok: false, errors: ["scene: must be an object"] };
  c.keys(s, ["format", "stem", "title", "base", "registry", "databank", "water", "spawns", "hmp", "units", "frames", "details", "blobs"], "scene");
  if (s.format !== SCENE_FORMAT) c.fail("scene.format", `must be "${SCENE_FORMAT}"`);
  if (!printable(s.stem, 1, 48) || (s.stem as string).includes(".")) c.fail("scene.stem", "must be 1-48 printable characters without '.'");
  if (!printable(s.title, 1, 40)) c.fail("scene.title", "must be 1-40 printable ASCII characters");
  checkBase(c, s.base, true);
  const reg = s.registry;
  if (!isObj(reg) || !c.keys(reg, ["levelType", "themeType", "scripts", "previewType"], "registry") || !isInt(reg.levelType, 0, 16) ||
      !isInt(reg.themeType, 0, 11) || (reg.previewType !== undefined && !isInt(reg.previewType, 0, 16)) || !Array.isArray(reg.scripts) ||
      reg.scripts.length < 1 || reg.scripts.length > 8 || !reg.scripts.every((n) => printable(n, 1, 63) && !n.includes(",")))
    c.fail("registry", "levelType, themeType, previewType and 1-8 script names are required");
  if (isObj(s.databank)) checkDatabank(c, s.databank, true);
  else c.fail("databank", "must be an object");
  checkWater(c, s.water);
  checkMode(c, s.spawns, "spawns", ["random", "knots"]);
  checkMode(c, s.hmp, "hmp", ["copy", "none", "flat"]);
  if (!isObj(s.units) || s.units.worldPerXan !== WORLD_PER_XAN) c.fail("units.worldPerXan", "must be 20");
  const frames = s.frames, details = s.details, blobs = s.blobs;
  if (!Array.isArray(frames) || frames.length > LIMITS.frames) return { ok: false, errors: [...c.errors, "frames: must be an array of at most 4096 frames"] };
  if (!Array.isArray(details) || details.length > LIMITS.details) return { ok: false, errors: [...c.errors, "details: must be an array of at most 65536 details"] };
  if (!Array.isArray(blobs) || blobs.length > 2 * LIMITS.frames) return { ok: false, errors: [...c.errors, "blobs: must be an array"] };

  const blobByRef = new Map<number, Obj>();
  blobs.forEach((b, i) => {
    const p = `blobs[${i}]`;
    if (!isObj(b) || !c.keys(b, ["ref", "kind", "frame", "bytes"], p)) return c.fail(p, "must be an object");
    if (!isInt(b.ref, 0, 1 << 24) || (b.kind !== "voxels" && b.kind !== "heightMap") || !isInt(b.frame, 1, 1 << 24) || !isInt(b.bytes, 0, 4 * LIMITS.frameVoxels))
      return c.fail(p, "ref, kind (voxels or heightMap), frame and bytes are required");
    if (blobByRef.has(b.ref as number)) return c.fail(p, `duplicate ref ${b.ref}`);
    blobByRef.set(b.ref as number, b);
  });
  const frameById = new Map<number, Obj>();
  let roots = 0;
  frames.forEach((f, i) => {
    const p = `frames[${i}]`;
    if (!isObj(f) || !c.keys(f, ["id", "parent", "name", "pos", "rot", "scale", "size", "voxels", "heightMap", "folder"], p)) return c.fail(p, "must be an object");
    if (!isInt(f.id, 1, 1 << 24)) return c.fail(`${p}.id`, "must be a positive integer");
    if (frameById.has(f.id as number)) return c.fail(p, `duplicate id ${f.id}`);
    frameById.set(f.id as number, f);
    if (f.parent === null) roots++;
    else if (!isInt(f.parent, 1, 1 << 24)) c.fail(`${p}.parent`, "must be a frame id or null");
    if (typeof f.name !== "string" || f.name.length > 127) c.fail(`${p}.name`, "must be a string of at most 127 characters");
    if (!isVec(f.pos) || !isVec(f.rot, 1e3) || !isVec(f.scale)) c.fail(p, "pos, rot and scale must be 3 finite numbers");
    const size = f.size;
    if (!Array.isArray(size) || size.length !== 3 || !size.every((v) => isInt(v, 0, 255)) || size[0] * size[1] * size[2] > LIMITS.frameVoxels)
      return c.fail(`${p}.size`, "must be 3 integers 0..255, at most 262144 voxels");
    const cells = size[0] * size[1] * size[2];
    if (f.voxels !== null) {
      const b = blobByRef.get(f.voxels as number);
      if (!b || b.kind !== "voxels" || b.frame !== f.id || b.bytes !== cells * 4) c.fail(p, "the voxels blob does not match the frame size");
    }
    if (f.heightMap !== null) {
      const b = blobByRef.get(f.heightMap as number);
      const corners = cells ? (size[0] + 1) * (size[2] + 1) : 0;
      if (!b || b.kind !== "heightMap" || b.frame !== f.id || b.bytes !== corners * 4) c.fail(p, "the heightMap blob does not match the frame size");
    }
    if (typeof f.folder !== "boolean") c.fail(`${p}.folder`, "must be a boolean");
  });
  if (frames.length && roots !== 1) c.fail("frames", "exactly one root frame (parent null) is required");
  for (const f of frameById.values()) {
    let cur: unknown = f.id;
    for (let hops = 0; cur !== null && cur !== undefined; hops++) {
      const fr = frameById.get(cur as number);
      if (!fr) { c.fail(`frame ${f.id}`, "parent is not a frame"); break; }
      if (hops > frames.length) { c.fail(`frame ${f.id}`, "the parent chain has a cycle"); break; }
      cur = fr.parent;
    }
  }
  for (const b of blobByRef.values()) if (!frameById.has(b.frame as number)) c.fail(`blob ${b.ref}`, "names a missing frame");
  const ids = new Set<number>(), srcs = new Set<number>();
  details.forEach((d, i) => {
    const p = `details[${i}]`;
    if (!isObj(d) || !c.keys(d, ["id", "src", "frame", "name", "resource", "pos", "rot", "scale", "voxelPos", "role"], p)) return c.fail(p, "must be an object");
    if (!isInt(d.id, 1, 1 << 24) || ids.has(d.id as number)) c.fail(`${p}.id`, "must be a unique positive integer");
    ids.add(d.id as number);
    if (d.src !== null) {
      if (!isInt(d.src, 1, 1 << 24) || srcs.has(d.src as number)) c.fail(`${p}.src`, "must be a unique object index or null");
      srcs.add(d.src as number);
    }
    if (!frameById.has(d.frame as number)) c.fail(`${p}.frame`, `frame ${String(d.frame)} is not in the scene`);
    if (typeof d.name !== "string" || typeof d.resource !== "string" || d.name.length > 127 || d.resource.length > 127)
      c.fail(p, "name and resource must be strings of at most 127 characters");
    if (!isVec(d.pos) || !isVec(d.rot, 1e3) || !isVec(d.scale) || !isVec(d.voxelPos)) c.fail(p, "pos, rot, scale and voxelPos must be 3 finite numbers");
    if (!ROLES.includes(d.role as Role)) c.fail(`${p}.role`, `unknown role '${String(d.role)}'`);
  });
  return { ok: c.errors.length === 0, errors: c.errors };
}

function checkFields(c: Checker, o: Obj, p: string, add: boolean) {
  if (!c.keys(o, ["name", "resource", "pos", "rot", "scale", "voxelPos"], p)) return;
  for (const k of ["name", "resource"]) {
    if (o[k] === undefined) { if (add) c.fail(`${p}.${k}`, "is required"); continue; }
    if (!printable(o[k], 1, 63)) c.fail(`${p}.${k}`, "must be 1-63 printable ASCII characters");
  }
  if (add && o.pos === undefined) c.fail(`${p}.pos`, "is required");
  for (const k of ["pos", "rot", "scale", "voxelPos"])
    if (o[k] !== undefined && !isVec(o[k], k === "rot" ? 1e3 : 1e6)) c.fail(`${p}.${k}`, "must be 3 finite numbers");
}

export function validatePatch(p: unknown, sizeBytes?: number): Validation {
  const c = new Checker();
  if (!isObj(p)) return { ok: false, errors: ["patch: must be an object"] };
  if (sizeBytes !== undefined && sizeBytes > LIMITS.patchBytes) c.fail("patch", "larger than 4 MB");
  c.keys(p, ["format", "stem", "title", "base", "databank", "water", "spawns", "hmp", "ops"], "patch");
  if (p.format !== PATCH_FORMAT) c.fail("patch.format", `must be "${PATCH_FORMAT}"`);
  if (typeof p.stem !== "string" || !/^[a-z0-9_]{1,48}$/.test(p.stem) || !p.stem.includes("_"))
    c.fail("patch.stem", "must be <prefix>_<slug> (a-z, 0-9, '_', at most 48)");
  if (!printable(p.title, 1, 40)) c.fail("patch.title", "must be 1-40 printable ASCII characters");
  checkBase(c, p.base, false);
  if (p.databank !== undefined) {
    if (!isObj(p.databank)) c.fail("databank", "must be an object");
    else {
      for (const [k, v] of Object.entries(p.databank)) if (typeof v !== "string" || !v) c.fail(`databank.${k}`, "must be a non-empty string");
      checkDatabank(c, p.databank, true);
    }
  }
  if (p.water !== undefined) checkWater(c, p.water);
  if (p.spawns !== undefined) checkMode(c, p.spawns, "spawns", ["random", "knots"]);
  if (p.hmp !== undefined) checkMode(c, p.hmp, "hmp", ["copy", "none", "flat"]);
  if (!Array.isArray(p.ops)) return { ok: false, errors: [...c.errors, "ops: must be an array"] };
  if (p.ops.length > LIMITS.ops) c.fail("ops", "at most 20000 ops");
  const runsPerFrame = new Map<number, number>();
  p.ops.forEach((o, i) => {
    const path = `ops[${i}]`;
    if (!isObj(o)) return c.fail(path, "must be an object");
    switch (o.op) {
      case "set": {
        if (!isInt(o.src, 1, 1 << 24)) c.fail(`${path}.src`, "must be an object index");
        const { op: _op, src: _src, ...fields } = o;
        checkFields(c, fields, path, false);
        if (!Object.keys(fields).length) c.fail(path, "a set changes at least one field");
        break;
      }
      case "add":
        if (!c.keys(o, ["op", "frame", "detail"], path)) break;
        if (!isInt(o.frame, 1, 1 << 24)) c.fail(`${path}.frame`, "must be a frame id");
        if (!isObj(o.detail)) c.fail(`${path}.detail`, "must be an object");
        else checkFields(c, o.detail, `${path}.detail`, true);
        break;
      case "remove":
        if (c.keys(o, ["op", "src"], path) && !isInt(o.src, 1, 1 << 24)) c.fail(`${path}.src`, "must be an object index");
        break;
      case "voxels": {
        if (!c.keys(o, ["op", "frame", "runs"], path)) break;
        if (!isInt(o.frame, 1, 1 << 24)) { c.fail(`${path}.frame`, "must be a frame id"); break; }
        if (!Array.isArray(o.runs) || !o.runs.length) { c.fail(`${path}.runs`, "must be a non-empty array"); break; }
        const total = (runsPerFrame.get(o.frame as number) ?? 0) + o.runs.length;
        runsPerFrame.set(o.frame as number, total);
        if (total > LIMITS.runsPerFrame) c.fail(`${path}.runs`, `more than 2000 runs for frame ${o.frame}`);
        o.runs.forEach((r: unknown, j: number) => {
          const rp = `${path}.runs[${j}]`;
          if (!Array.isArray(r) || r.length !== 3 || !r.every((v) => isInt(v, 0, 0xffffffff))) return c.fail(rp, "must be [start, count, value]");
          if (r[1] === 0 || r[0] + r[1] > LIMITS.frameVoxels) return c.fail(rp, "the run is empty or past 262144 voxels");
          if (!validRunValue(r[2])) c.fail(rp, "the value must keep bits 24-31 at 0 and the solid bits at 0 or 3");
        });
        break;
      }
      default:
        c.fail(`${path}.op`, `unknown op '${String(o.op)}'`);
    }
  });
  return { ok: c.errors.length === 0, errors: c.errors };
}

export function validate(doc: unknown): Validation {
  if (isObj(doc) && doc.format === PATCH_FORMAT) return validatePatch(doc);
  return validateScene(doc);
}

/** Row-major 3x4 affine matrix (.xan units). */
export type Mat3x4 = [number, number, number, number, number, number, number, number, number, number, number, number];

/** T(pos) * Rz(rot.z) * Ry(rot.y) * Rx(rot.x) * S(scale): the composition the engine's detail placement matches. */
export function frameLocal(f: Pick<Frame, "pos" | "rot" | "scale">): Mat3x4 {
  const [cx, sx, cy, sy, cz, sz] = [Math.cos(f.rot[0]), Math.sin(f.rot[0]), Math.cos(f.rot[1]), Math.sin(f.rot[1]), Math.cos(f.rot[2]), Math.sin(f.rot[2])];
  const r = [
    [cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx],
    [sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx],
    [-sy, cy * sx, cy * cx],
  ];
  const m = new Array(12).fill(0) as Mat3x4;
  for (let j = 0; j < 3; j++) {
    for (let k = 0; k < 3; k++) m[j * 4 + k] = r[j][k] * f.scale[k];
    m[j * 4 + 3] = f.pos[j];
  }
  return m;
}

export function multiply(a: Mat3x4, b: Mat3x4): Mat3x4 {
  const m = new Array(12).fill(0) as Mat3x4;
  for (let i = 0; i < 3; i++)
    for (let j = 0; j < 4; j++) {
      let v = j === 3 ? a[i * 4 + 3] : 0;
      for (let k = 0; k < 3; k++) v += a[i * 4 + k] * b[k * 4 + j];
      m[i * 4 + j] = v;
    }
  return m;
}

export function apply(m: Mat3x4, p: Vec3): Vec3 {
  return [0, 1, 2].map((i) => m[i * 4] * p[0] + m[i * 4 + 1] * p[1] + m[i * 4 + 2] * p[2] + m[i * 4 + 3]) as Vec3;
}

/** The frame's world matrix, composed up to the root (whose own transform is ignored); null for a missing frame. */
export function frameWorld(frames: Map<number, Frame>, id: number): Mat3x4 | null {
  let acc: Mat3x4 = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0];
  let cur: number | null = id;
  for (let hops = 0; cur !== null; hops++) {
    const f = frames.get(cur);
    if (!f || hops > frames.size) return null;
    if (f.parent === null) break;
    acc = multiply(frameLocal(f), acc);
    cur = f.parent;
  }
  return acc;
}
