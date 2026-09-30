// erg-scene/1|2 and erg-patch/1|2: the scene the level service sends and the patch the editor saves (the C++ model is
// src/erg/scene.h and patch.h; the JSON Schemas are docs/erg-{scene,patch}-{1,2}.schema.json). A document is /2 only when
// it uses a /2 feature (objects, a Survivor twin, a level script, new frames, a painted surround).
// Positions, scales and voxel coordinates are .xan units; water is in world units (20 per .xan unit), sea level 0.

export const SCENE_FORMAT = "erg-scene/1";
export const PATCH_FORMAT = "erg-patch/1";
export const SCENE_FORMAT_2 = "erg-scene/2";
export const PATCH_FORMAT_2 = "erg-patch/2";
export type SceneFormat = typeof SCENE_FORMAT | typeof SCENE_FORMAT_2;
export type PatchFormat = typeof PATCH_FORMAT | typeof PATCH_FORMAT_2;
export const WORLD_PER_XAN = 20;
export const KNOT_RESOURCE = "CheesyGrinWorm";   // the non-visual marker every knot detail uses (spawns and objects)
export const LIMITS = {
  frames: 4096, details: 65536, frameVoxels: 262144, ops: 20000, patchBytes: 4 << 20, runsPerFrame: 2000, runVoxels: 1 << 23,
  objects: 256, newFrames: 64, newFrameSide: 32, telepadGroups: 8, hmpSide: 100, hmpCells: 10000, hmpBytes: 50000,
};

export type Vec3 = [number, number, number];
export type Role = "scenery" | "spawn" | "object" | "camera" | "light" | "emitter" | "sound" | "collision" | "marker" | "other";
export const ROLES: readonly Role[] = ["scenery", "spawn", "object", "camera", "light", "emitter", "sound", "collision", "marker", "other"];
export const THEMES = ["ARABIAN", "WILDWEST", "CAMELOT", "PREHISTORIC", "BUILDING", "ARCTIC", "ENGLAND", "HORROR", "LUNAR", "PIRATE", "WAR"] as const;
export const TIMES_OF_DAY = ["DAY", "EVENING", "NIGHT"] as const;
export type SpawnMode = "random" | "knots";
export type HmpMode = "copy" | "none" | "flat" | "paint";

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
  new?: boolean;                                // an added block: id -1..-64, under the Scene frame
}
export interface Detail {
  id: number; src: number | null; frame: number; name: string; resource: string;
  pos: Vec3; rot: Vec3; scale: Vec3; voxelPos: Vec3; role: Role;
}
export interface Blob { ref: number; kind: "voxels" | "heightMap" | "hmp"; frame: number; bytes: number; }

export type CrateKind = "weapon" | "health" | "utility";
export type CrateSpec =
  | { kind: "weapon" | "utility"; contents: string; count?: number; hitpoints?: number; parachute?: boolean }
  | { kind: "health"; amount: number; hitpoints?: number; parachute?: boolean };
export interface TriggerSpec { index: number; radius?: number; teamCollect?: number; teamDestroy?: number; hitpoints?: number; wormCollect?: boolean; }
export type ObjectSpec =
  | { knot: string; type: "crate"; crate: CrateSpec }
  | { knot: string; type: "telepad"; group: number }
  | { knot: string; type: "trigger"; trigger: TriggerSpec }
  | { knot: string; type: "minefactory" };
export type ObjectType = ObjectSpec["type"];
export interface ScriptMeta { present: boolean; sha256: string | null; }
export interface NewFrame { tmp: number; parent: number; name: string; pos: Vec3; size: Vec3; }
/** [start, count, value] over the 100x100 surround cells, row-major: heights 0..1, blend 0..255. */
export type HmpRun = [start: number, count: number, value: number];
export interface HmpRuns { heights?: HmpRun[]; blend?: HmpRun[]; }

export interface Scene {
  format: SceneFormat;
  stem: string; title: string;
  base: SceneBase;
  registry: Registry;
  databank: Databank;
  water: { level: number | null };
  spawns: { mode: SpawnMode };
  hmp: { mode: HmpMode; ref?: number };
  units: { worldPerXan: number };
  frames: Frame[];
  details: Detail[];
  blobs: Blob[];
  kind?: { survivor: boolean };
  objects?: ObjectSpec[];
  script?: ScriptMeta;
}

export interface DetailFields { name?: string; resource?: string; pos?: Vec3; rot?: Vec3; scale?: Vec3; voxelPos?: Vec3; }
export type VoxelRun = [start: number, count: number, value: number];
export type Op =
  | ({ op: "set"; src: number } & DetailFields)
  | { op: "add"; frame: number; detail: DetailFields & { name: string; resource: string; pos: Vec3 } }
  | { op: "remove"; src: number }
  | { op: "voxels"; frame: number; runs: VoxelRun[] }
  | ({ op: "addFrame" } & NewFrame)
  | ({ op: "hmp" } & HmpRuns);

export interface Patch {
  format: PatchFormat;
  stem: string; title: string;
  base: PatchBase;
  databank?: Partial<Databank>;
  water?: { level: number | null };
  spawns?: { mode: SpawnMode };
  hmp?: { mode: HmpMode };
  ops: Op[];
  kind?: { survivor: boolean };
  objects?: ObjectSpec[];
  script?: ScriptMeta;
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

/** Knot rules: CRATE_<n>, TP_<g>_<n> (g = the group), TRIG_<n> (n 0-255, no leading zeros) and minefactory. */
export function validKnot(knot: string, type: ObjectType, group = 0): boolean {
  const n = "(0|[1-9][0-9]{0,2})";
  const idx = (m: RegExpMatchArray | null) => !!m && Number(m[1]) <= 255;
  switch (type) {
    case "crate": return idx(knot.match(new RegExp(`^CRATE_${n}$`)));
    case "trigger": return idx(knot.match(new RegExp(`^TRIG_${n}$`)));
    case "telepad": return Number.isInteger(group) && group >= 1 && group <= LIMITS.telepadGroups && idx(knot.match(new RegExp(`^TP_${group}_${n}$`)));
    case "minefactory": return knot === "minefactory";
  }
  return false;
}

export const validContentsName = (s: unknown): s is string => typeof s === "string" && /^[A-Za-z][A-Za-z0-9_]{0,62}$/.test(s);

export function sceneUsesV2(s: Scene): boolean {
  return !!s.kind?.survivor || !!s.objects?.length || !!s.script?.present || s.hmp.mode === "paint" || s.hmp.ref !== undefined ||
    s.frames.some((f) => f.new) || s.blobs.some((b) => b.kind === "hmp");
}

export function patchUsesV2(p: Patch): boolean {
  return !!p.kind?.survivor || !!p.objects?.length || !!p.script?.present || p.hmp?.mode === "paint" ||
    p.ops.some((o) => o.op === "addFrame" || o.op === "hmp" || (o.op === "voxels" && o.frame < 0));
}

/** A /1 scene as a /2 one in memory (no /2 feature is added, so it still saves as /1). */
export function upgrade(s: Scene): Scene {
  const out: Scene = structuredClone(s);
  out.format = SCENE_FORMAT_2;
  out.kind = out.kind ?? { survivor: false };
  out.objects = out.objects ?? [];
  out.script = out.script ?? { present: false, sha256: null };
  return out;
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

function checkMode(c: Checker, v: unknown, path: string, modes: readonly string[], extra: readonly string[] = []) {
  if (!isObj(v) || !c.keys(v, ["mode", ...extra], path) || !modes.includes(v.mode as string)) c.fail(`${path}.mode`, `must be one of ${modes.join(", ")}`);
}

const OBJ_TYPES = ["crate", "telepad", "trigger", "minefactory"];

function checkObject(c: Checker, o: unknown, p: string) {
  if (!isObj(o) || typeof o.knot !== "string" || !OBJ_TYPES.includes(o.type as string)) return c.fail(p, "knot and type (crate, telepad, trigger, minefactory) are required");
  const int = (v: unknown, lo: number, hi: number, path: string, optional = true) =>
    (optional && v === undefined) || isInt(v, lo, hi) || c.fail(path, `must be an integer ${lo}-${hi}`);
  switch (o.type) {
    case "crate": {
      if (!c.keys(o, ["knot", "type", "crate"], p)) return;
      const cr = o.crate;
      if (!isObj(cr)) return c.fail(`${p}.crate`, "must be an object");
      if (cr.kind === "health") {
        if (!c.keys(cr, ["kind", "amount", "hitpoints", "parachute"], `${p}.crate`)) return;
        int(cr.amount, 1, 500, `${p}.crate.amount`, false);
      } else if (cr.kind === "weapon" || cr.kind === "utility") {
        if (!c.keys(cr, ["kind", "contents", "count", "hitpoints", "parachute"], `${p}.crate`)) return;
        if (!validContentsName(cr.contents)) c.fail(`${p}.crate.contents`, "must be a weapon or utility name");
        int(cr.count, 1, 99, `${p}.crate.count`);
      } else return c.fail(`${p}.crate.kind`, "must be weapon, health or utility");
      int(cr.hitpoints, 1, 1000, `${p}.crate.hitpoints`);
      if (cr.parachute !== undefined && typeof cr.parachute !== "boolean") c.fail(`${p}.crate.parachute`, "must be a boolean");
      break;
    }
    case "telepad":
      if (!c.keys(o, ["knot", "type", "group"], p)) return;
      int(o.group, 1, LIMITS.telepadGroups, `${p}.group`, false);
      break;
    case "trigger": {
      if (!c.keys(o, ["knot", "type", "trigger"], p)) return;
      const t = o.trigger;
      if (!isObj(t) || !c.keys(t, ["index", "radius", "teamCollect", "teamDestroy", "hitpoints", "wormCollect"], `${p}.trigger`)) return c.fail(`${p}.trigger`, "must be an object");
      int(t.index, 0, 255, `${p}.trigger.index`, false);
      if (t.radius !== undefined && (!isNum(t.radius, 1000) || (t.radius as number) < 1)) c.fail(`${p}.trigger.radius`, "must be 1-1000");
      int(t.teamCollect, 0, 8, `${p}.trigger.teamCollect`);
      int(t.teamDestroy, 0, 8, `${p}.trigger.teamDestroy`);
      int(t.hitpoints, 0, 1000, `${p}.trigger.hitpoints`);
      if (t.wormCollect !== undefined && typeof t.wormCollect !== "boolean") c.fail(`${p}.trigger.wormCollect`, "must be a boolean");
      break;
    }
    case "minefactory":
      c.keys(o, ["knot", "type"], p);
      break;
  }
  if (!validKnot(o.knot as string, o.type as ObjectType, o.type === "telepad" ? (o.group as number) : 0))
    c.fail(`${p}.knot`, `'${String(o.knot)}' is not a ${String(o.type)} knot name`);
}

/** Knots against the details (after the ops): each object needs exactly one added detail of its name. */
function checkObjects(c: Checker, objects: unknown, details: { src: number | null; name: string }[]) {
  if (!Array.isArray(objects) || objects.length > LIMITS.objects) return c.fail("objects", "must be an array of at most 256 objects");
  const added = new Map<string, number>(), based = new Set<string>();
  for (const d of details) {
    if (d.src === null) added.set(d.name, (added.get(d.name) ?? 0) + 1);
    else based.add(d.name);
  }
  const knots = new Set<string>(), groups = new Set<number>();
  let factories = 0;
  objects.forEach((o, i) => {
    const p = `objects[${i}]`;
    checkObject(c, o, p);
    if (!isObj(o) || typeof o.knot !== "string") return;
    if (knots.has(o.knot)) c.fail(`${p}.knot`, `'${o.knot}' is used by another object`);
    knots.add(o.knot);
    if (based.has(o.knot)) c.fail(`${p}.knot`, `the base level already has a detail named '${o.knot}'`);
    else if (added.get(o.knot) !== 1) c.fail(`${p}.knot`, `needs exactly one added detail named '${o.knot}'`);
    if (o.type === "telepad") groups.add(o.group as number);
    if (o.type === "minefactory" && ++factories > 1) c.fail(p, "at most one mine factory");
  });
  if (groups.size > LIMITS.telepadGroups) c.fail("objects", "at most 8 telepad groups");
}

function checkScript(c: Checker, v: unknown) {
  if (!isObj(v) || !c.keys(v, ["present", "sha256"], "script") || typeof v.present !== "boolean") return c.fail("script", "present is required");
  if (v.sha256 !== null && v.sha256 !== undefined && !isHex64(v.sha256)) c.fail("script.sha256", "must be 64 lower-case hex digits");
  if (v.present !== (typeof v.sha256 === "string")) c.fail("script", "sha256 is set exactly when present is true");
}

function checkKind(c: Checker, v: unknown) {
  if (!isObj(v) || !c.keys(v, ["survivor"], "kind") || (v.survivor !== undefined && typeof v.survivor !== "boolean")) c.fail("kind", "survivor must be a boolean");
}

const underScene = (frames: Map<number, Obj>, id: number) => {
  let cur: unknown = id;
  for (let hops = 0; typeof cur === "number" && hops <= frames.size; hops++) {
    const f = frames.get(cur);
    if (!f) return false;
    if (f.name === "Scene") return true;
    cur = f.parent;
  }
  return false;
};

function checkWater(c: Checker, v: unknown) {
  if (!isObj(v) || !c.keys(v, ["level"], "water")) return c.fail("water", "must be an object");
  if (v.level !== null && v.level !== undefined && !isNum(v.level, 1000)) c.fail("water.level", "must be -1000..1000 world units");
}

export function validateScene(s: unknown): Validation {
  const c = new Checker();
  if (!isObj(s)) return { ok: false, errors: ["scene: must be an object"] };
  const v2 = s.format === SCENE_FORMAT_2;
  c.keys(s, v2 ? ["format", "stem", "title", "base", "registry", "kind", "databank", "water", "spawns", "hmp", "units", "frames", "details", "blobs", "objects", "script"]
    : ["format", "stem", "title", "base", "registry", "databank", "water", "spawns", "hmp", "units", "frames", "details", "blobs"], "scene");
  if (s.format !== SCENE_FORMAT && !v2) c.fail("scene.format", `must be "${SCENE_FORMAT}" or "${SCENE_FORMAT_2}" (a newer format needs a newer Melange)`);
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
  checkMode(c, s.hmp, "hmp", v2 ? ["copy", "none", "flat", "paint"] : ["copy", "none", "flat"], v2 ? ["ref"] : []);
  if (v2) {
    if (s.kind !== undefined) checkKind(c, s.kind);
    if (s.script !== undefined) checkScript(c, s.script);
  }
  if (!isObj(s.units) || s.units.worldPerXan !== WORLD_PER_XAN) c.fail("units.worldPerXan", "must be 20");
  const frames = s.frames, details = s.details, blobs = s.blobs;
  if (!Array.isArray(frames) || frames.length > LIMITS.frames) return { ok: false, errors: [...c.errors, "frames: must be an array of at most 4096 frames"] };
  if (!Array.isArray(details) || details.length > LIMITS.details) return { ok: false, errors: [...c.errors, "details: must be an array of at most 65536 details"] };
  if (!Array.isArray(blobs) || blobs.length > 2 * LIMITS.frames) return { ok: false, errors: [...c.errors, "blobs: must be an array"] };

  const blobByRef = new Map<number, Obj>();
  blobs.forEach((b, i) => {
    const p = `blobs[${i}]`;
    if (!isObj(b) || !c.keys(b, ["ref", "kind", "frame", "bytes"], p)) return c.fail(p, "must be an object");
    if (v2 && b.kind === "hmp") {
      if (!isInt(b.ref, 0, 1 << 24) || b.frame !== 0 || b.bytes !== LIMITS.hmpBytes || (s.hmp as Obj | undefined)?.ref !== b.ref)
        return c.fail(p, "the hmp blob must be the surround (frame 0, 50000 bytes, hmp.ref)");
    } else if (!isInt(b.ref, 0, 1 << 24) || (b.kind !== "voxels" && b.kind !== "heightMap") || !isInt(b.frame, v2 ? -LIMITS.newFrames : 1, 1 << 24) ||
        b.frame === 0 || !isInt(b.bytes, 0, 4 * LIMITS.frameVoxels))
      return c.fail(p, "ref, kind (voxels or heightMap), frame and bytes are required");
    if (blobByRef.has(b.ref as number)) return c.fail(p, `duplicate ref ${b.ref}`);
    blobByRef.set(b.ref as number, b);
  });
  const frameById = new Map<number, Obj>();
  let roots = 0;
  frames.forEach((f, i) => {
    const p = `frames[${i}]`;
    if (!isObj(f) || !c.keys(f, v2 ? ["id", "new", "parent", "name", "pos", "rot", "scale", "size", "voxels", "heightMap", "folder"]
      : ["id", "parent", "name", "pos", "rot", "scale", "size", "voxels", "heightMap", "folder"], p)) return c.fail(p, "must be an object");
    if (f.new !== undefined && typeof f.new !== "boolean") c.fail(`${p}.new`, "must be a boolean");
    if (f.new ? !isInt(f.id, -LIMITS.newFrames, -1) : !isInt(f.id, 1, 1 << 24)) return c.fail(`${p}.id`, f.new ? "a new frame's id is -1..-64" : "must be a positive integer");
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
  let newFrames = 0;
  for (const f of frameById.values()) {
    if (!f.new) continue;
    const p = `frame ${f.id}`;
    if (++newFrames > LIMITS.newFrames) c.fail("frames", "at most 64 new frames");
    const par = frameById.get(f.parent as number);
    if (!par || par.new || !underScene(frameById, f.parent as number)) c.fail(p, "a new frame's parent must be the Scene frame or under it");
    const size = f.size as number[];
    if (!isVec(f.rot) || !(f.rot as number[]).every((v) => v === 0) || !isVec(f.scale) || !(f.scale as number[]).every((v) => v === 1) || f.folder || f.heightMap !== null)
      c.fail(p, "a new frame has identity rotation and scale, no heightMap, and is not a folder");
    if (!size.every((v) => v >= 1 && v <= LIMITS.newFrameSide)) c.fail(`${p}.size`, "each size must be 1..32");
  }
  if (v2) {
    const hmp = s.hmp as Obj | undefined;
    if (hmp?.mode === "paint" && !blobs.some((b) => isObj(b) && b.kind === "hmp")) c.fail("hmp.ref", "hmp.mode paint needs the painted surround's blob");
    if (hmp?.ref !== undefined && hmp.mode !== "paint" && hmp.mode !== "copy") c.fail("hmp.ref", "needs hmp.mode paint or copy");
  }
  for (const f of frameById.values()) {
    let cur: unknown = f.id;
    for (let hops = 0; cur !== null && cur !== undefined; hops++) {
      const fr = frameById.get(cur as number);
      if (!fr) { c.fail(`frame ${f.id}`, "parent is not a frame"); break; }
      if (hops > frames.length) { c.fail(`frame ${f.id}`, "the parent chain has a cycle"); break; }
      cur = fr.parent;
    }
  }
  for (const b of blobByRef.values()) if (b.kind !== "hmp" && !frameById.has(b.frame as number)) c.fail(`blob ${b.ref}`, "names a missing frame");
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
    if (d.src === null && d.name === "telepad") c.fail(p, "an added detail may not be named 'telepad' (place a telepad pair)");
  });
  if (v2 && s.objects !== undefined) checkObjects(c, s.objects, details.filter(isObj) as unknown as Detail[]);
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
  const v2 = p.format === PATCH_FORMAT_2;
  c.keys(p, v2 ? ["format", "stem", "title", "base", "kind", "databank", "water", "spawns", "hmp", "ops", "objects", "script"]
    : ["format", "stem", "title", "base", "databank", "water", "spawns", "hmp", "ops"], "patch");
  if (p.format !== PATCH_FORMAT && !v2) c.fail("patch.format", `must be "${PATCH_FORMAT}" or "${PATCH_FORMAT_2}" (a newer format needs a newer Melange)`);
  if (v2) {
    if (p.kind !== undefined) checkKind(c, p.kind);
    if (p.script !== undefined) checkScript(c, p.script);
    if (p.objects !== undefined) {
      if (!Array.isArray(p.objects) || p.objects.length > LIMITS.objects) c.fail("objects", "must be an array of at most 256 objects");
      else p.objects.forEach((o, i) => checkObject(c, o, `objects[${i}]`));
    }
  }
  const tmps = new Set<number>();
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
  if (p.hmp !== undefined) checkMode(c, p.hmp, "hmp", v2 ? ["copy", "none", "flat", "paint"] : ["copy", "none", "flat"]);
  if (!Array.isArray(p.ops)) return { ok: false, errors: [...c.errors, "ops: must be an array"] };
  if (p.ops.length > LIMITS.ops) c.fail("ops", "at most 20000 ops");
  const runsPerFrame = new Map<number, number>();
  let covered = 0;
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
        else {
          checkFields(c, o.detail, `${path}.detail`, true);
          if (o.detail.name === "telepad") c.fail(`${path}.detail.name`, "a detail may not be named 'telepad' (place a telepad pair)");
        }
        break;
      case "remove":
        if (c.keys(o, ["op", "src"], path) && !isInt(o.src, 1, 1 << 24)) c.fail(`${path}.src`, "must be an object index");
        break;
      case "voxels": {
        if (!c.keys(o, ["op", "frame", "runs"], path)) break;
        if (!isInt(o.frame, v2 ? -LIMITS.newFrames : 1, 1 << 24) || o.frame === 0 || ((o.frame as number) < 0 && !tmps.has(o.frame as number))) {
          c.fail(`${path}.frame`, "must be a frame id or a frame added by an earlier op");
          break;
        }
        if (!Array.isArray(o.runs) || !o.runs.length) { c.fail(`${path}.runs`, "must be a non-empty array"); break; }
        const total = (runsPerFrame.get(o.frame as number) ?? 0) + o.runs.length;
        runsPerFrame.set(o.frame as number, total);
        if (total > LIMITS.runsPerFrame) c.fail(`${path}.runs`, `more than 2000 runs for frame ${o.frame}`);
        o.runs.forEach((r: unknown, j: number) => {
          const rp = `${path}.runs[${j}]`;
          if (!Array.isArray(r) || r.length !== 3 || !r.every((v) => isInt(v, 0, 0xffffffff))) return c.fail(rp, "must be [start, count, value]");
          if (r[1] === 0 || r[0] + r[1] > LIMITS.frameVoxels) return c.fail(rp, "the run is empty or past 262144 voxels");
          covered += r[1];
          if (covered > LIMITS.runVoxels) return c.fail(rp, `the runs of a patch cover more than ${LIMITS.runVoxels} voxels`);
          if (!validRunValue(r[2])) c.fail(rp, "the value must keep bits 24-31 at 0 and the solid bits at 0 or 3");
        });
        break;
      }
      case "addFrame":
        if (!v2) { c.fail(`${path}.op`, "unknown op 'addFrame'"); break; }
        if (!c.keys(o, ["op", "tmp", "parent", "name", "pos", "size"], path)) break;
        if (!isInt(o.tmp, -LIMITS.newFrames, -1) || tmps.has(o.tmp as number)) c.fail(`${path}.tmp`, "must be an unused id -1..-64");
        else tmps.add(o.tmp as number);
        if (!isInt(o.parent, 1, 1 << 24)) c.fail(`${path}.parent`, "must be a frame of the base");
        if (typeof o.name !== "string" || !/^[A-Za-z0-9_]{1,31}$/.test(o.name)) c.fail(`${path}.name`, "must be 1-31 letters, digits or '_'");
        if (!isVec(o.pos)) c.fail(`${path}.pos`, "must be 3 finite numbers");
        if (!Array.isArray(o.size) || o.size.length !== 3 || !o.size.every((v) => isInt(v, 1, LIMITS.newFrameSide))) c.fail(`${path}.size`, "each size must be 1..32");
        break;
      case "hmp": {
        if (!v2) { c.fail(`${path}.op`, "unknown op 'hmp'"); break; }
        if (!c.keys(o, ["op", "heights", "blend"], path)) break;
        if ((p.hmp as Obj | undefined)?.mode !== "paint") c.fail(path, "hmp ops need hmp.mode paint");
        let cells = 0;
        for (const [k, blend] of [["heights", false], ["blend", true]] as const) {
          const runs = o[k];
          if (runs === undefined) continue;
          if (!Array.isArray(runs)) { c.fail(`${path}.${k}`, "must be an array of runs"); continue; }
          runs.forEach((r: unknown, j: number) => {
            const rp = `${path}.${k}[${j}]`;
            if (!Array.isArray(r) || r.length !== 3 || !isInt(r[0], 0, LIMITS.hmpCells) || !isInt(r[1], 1, LIMITS.hmpCells) || r[0] + r[1] > LIMITS.hmpCells)
              return c.fail(rp, "must be [start, count, value] within the 10000 cells");
            if (blend ? !isInt(r[2], 0, 255) : !(isNum(r[2], 1) && r[2] >= 0)) c.fail(rp, blend ? "a blend value is an integer 0..255" : "a height is 0..1");
            cells++;
          });
        }
        if (!cells) c.fail(path, "an hmp op changes at least one cell");
        break;
      }
      default:
        c.fail(`${path}.op`, `unknown op '${String(o.op)}'`);
    }
  });
  return { ok: c.errors.length === 0, errors: c.errors };
}

/** The patch's objects against the details the patch leaves on its base (knots, limits, one added detail per knot). */
export function validatePatchObjects(p: Patch, base: Scene): Validation {
  const c = new Checker();
  const names = base.details.filter((d) => d.src !== null).map((d) => ({ src: d.src, name: d.name }));
  for (const op of p.ops) {
    if (op.op === "remove") {
      const at = names.findIndex((n) => n.src === op.src);
      if (at >= 0) names.splice(at, 1);
    }
    else if (op.op === "set" && op.name !== undefined) { const n = names.find((x) => x.src === op.src); if (n) n.name = op.name; }
    else if (op.op === "add") names.push({ src: null, name: op.detail.name });
  }
  checkObjects(c, p.objects ?? [], names);
  return { ok: c.errors.length === 0, errors: c.errors };
}

export function validate(doc: unknown): Validation {
  if (isObj(doc) && (doc.format === PATCH_FORMAT || doc.format === PATCH_FORMAT_2)) return validatePatch(doc);
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
