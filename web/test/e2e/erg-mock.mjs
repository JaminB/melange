// The level service for the mock server, over the committed synthetic scenes (tests/fixtures/erg): level.list,
// level.new, level.load (the scene, then one bin frame per blob), level.save, level.themes, level.palette and
// level.close, with the terrain generated from each frame's size. No game file is involved.
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

export const AFTER = Symbol("after");
const here = dirname(fileURLToPath(import.meta.url));
const fixtures = join(here, "..", "..", "..", "tests", "fixtures", "erg");
const THEMES = ["ARABIAN", "WILDWEST", "CAMELOT", "PREHISTORIC", "BUILDING", "ARCTIC", "ENGLAND", "HORROR", "LUNAR", "PIRATE", "WAR"];
const BASES = [
  { key: "Multi.Synthetic12", file: "synthetic-12.json", title: "Synthetic 12" },
  { key: "Multi.Synthetic400", file: "synthetic-400.json", title: "Synthetic 400" },
];

/** The same height field as web/test/unit/erg-editor.test.ts. */
export function syntheticBlobs(s) {
  const out = new Map();
  for (const f of s.frames) {
    const [X, Y, Z] = f.size;
    if (f.voxels !== null) {
      const v = new Uint32Array(X * Y * Z);
      for (let z = 0; z < Z; z++)
        for (let x = 0; x < X; x++) {
          const h = Math.max(1, Math.min(Y, 1 + Math.floor((Y - 1) * (0.5 + 0.5 * Math.sin(x * 0.7 + f.id) * Math.cos(z * 0.5 + f.id * 0.3)))));
          for (let y = 0; y < h; y++) v[(z * X + x) * Y + y] = 3 | (((f.id + y) % 16) << 2);
        }
      out.set(f.voxels, Buffer.from(v.buffer));
    }
    if (f.heightMap !== null) out.set(f.heightMap, Buffer.alloc((X + 1) * (Z + 1) * 4));
  }
  return out;
}

function roleOf(name, resource) {
  const n = name.toUpperCase(), r = resource.toUpperCase();
  if (/^WORM\d/.test(n) || r === "CHEESYGRINWORM") return "spawn";
  if (["MINE", "OILDRUM", "MINEFACTORY", "TELEPAD"].includes(n) || r.includes("MINE") || r.includes("OILDRUM") || r.includes("OIL DRUM") ||
      r === "TELEPAD" || r.includes("CRATE") || r === "TARGET") return "object";
  if (r === "CAMERA" || r === "LOOKAT POINT") return "camera";
  if (r === "LIGHT" || n.includes("PNTLGHT")) return "light";
  if (n.includes("VISIBLE") || n.includes("PROP") || n.includes("STANDIN")) return "scenery";
  return "other";
}

function baseScene(key) {
  const b = BASES.find((x) => x.key === key);
  if (!b) throw [-32602, `no base '${key}'`];
  const s = JSON.parse(readFileSync(join(fixtures, b.file), "utf8"));
  s.base.key = key;
  return s;
}

// The server's ApplyPatch on the model: ops in order, refusing a bad reference with its index.
function applyPatch(scene, p) {
  const s = structuredClone(scene);
  s.stem = p.stem;
  s.title = p.title;
  for (const [k, v] of Object.entries(p.databank ?? {})) if (v) s.databank[k] = v;
  s.water = { level: p.water?.level ?? null };
  s.spawns = { mode: p.spawns?.mode ?? "random" };
  s.hmp = { mode: p.hmp?.mode ?? "copy" };
  let next = s.details.reduce((m, d) => Math.max(m, d.id), 0) + 1;
  p.ops.forEach((op, i) => {
    if (op.op === "set" || op.op === "remove") {
      const at = s.details.findIndex((d) => d.src === op.src);
      if (at < 0) throw [-32602, `ops[${i}].src: ${op.src} is not a detail of the base`];
      if (op.op === "remove") return void s.details.splice(at, 1);
      const d = s.details[at];
      for (const k of ["name", "resource", "pos", "rot", "scale", "voxelPos"]) if (op[k] !== undefined) d[k] = op[k];
      d.role = roleOf(d.name, d.resource);
    } else if (op.op === "add") {
      if (!s.frames.some((f) => f.id === op.frame)) throw [-32602, `ops[${i}].frame: ${op.frame} is not a frame`];
      const d = op.detail;
      s.details.push({ id: next++, src: null, frame: op.frame, name: d.name, resource: d.resource, pos: d.pos, rot: d.rot ?? [0, 0, 0],
        scale: d.scale ?? [1, 1, 1], voxelPos: d.voxelPos ?? [0, 0, 0], role: roleOf(d.name, d.resource) });
    } else throw [-32602, `ops[${i}].op: '${op.op}' is refused`];
  });
  return s;
}

export function ergService(state) {
  const projects = new Map();
  state.erg = { projects, saves: 0, loads: 0 };
  const info = (p) => ({ id: p.id, title: p.title, stem: p.stem, base: p.base, modified: p.modified, built: false });
  // As the service's level.load: refs run on across loads, so a project's refs differ from its base's.
  let refBase = 0;
  const load = (scene) => {
    state.erg.loads++;
    const blobs = syntheticBlobs(scene);
    const maxRef = Math.max(0, ...scene.blobs.map((b) => b.ref));
    if (refBase + maxRef >= 1 << 24) refBase = 0;
    const shift = refBase;
    refBase += maxRef;
    const sent = scene.blobs.map((b) => [b.ref + shift, blobs.get(b.ref)]);
    for (const b of scene.blobs) b.ref += shift;
    for (const f of scene.frames) {
      if (f.voxels !== null) f.voxels += shift;
      if (f.heightMap !== null) f.heightMap += shift;
    }
    if (scene.hmp?.ref !== undefined) scene.hmp.ref += shift;
    return { value: scene, [AFTER]: (c) => { for (const [ref, data] of sent) c.sendBinary(ref, data); } };
  };
  return {
    methods: ["level.list", "level.new", "level.load", "level.save", "level.themes", "level.palette", "level.close"],
    mutating: ["level.new", "level.save"],
    handlers: {
      "level.list": () => ({
        bases: BASES.map((b) => ({ key: b.key, stem: b.file.replace(".json", ""), title: b.title, source: "game", theme: "BUILDING" })),
        projects: [...projects.values()].map(info),
      }),
      "level.new": (p) => {
        if (typeof p.slug !== "string" || !/^[a-z0-9]{1,24}$/.test(p.slug)) throw [-32602, "slug must be 1-24 characters a-z, 0-9"];
        if (typeof p.title !== "string" || !/^[\x20-\x7e]{1,40}$/.test(p.title)) throw [-32602, "title must be 1-40 printable characters"];
        if (projects.has(p.slug)) throw [-32000, `project '${p.slug}' exists`];
        const s = baseScene(p.base);
        const proj = { id: p.slug, title: p.title, stem: `erg_${p.slug}`, base: p.base, modified: new Date().toISOString(),
          patch: { format: "erg-patch/1", stem: `erg_${p.slug}`, title: p.title, base: { key: p.base, source: "game", sha256: s.base.sha256 }, ops: [] } };
        projects.set(p.slug, proj);
        return info(proj);
      },
      "level.load": (p) => {
        if (typeof p.base === "string") return load(baseScene(p.base));
        const proj = projects.get(p.project);
        if (!proj) throw [-32602, `no project '${p.project}'`];
        return load(applyPatch(baseScene(proj.base), proj.patch));
      },
      "level.save": (p) => {
        const proj = projects.get(p.project);
        if (!proj) throw [-32602, `no project '${p.project}'`];
        const patch = p.patch;
        if (!patch || patch.format !== "erg-patch/1" || !Array.isArray(patch.ops)) throw [-32602, "not an erg-patch/1"];
        if (patch.base?.key !== proj.base) throw [-32000, "the patch is for another base"];
        applyPatch(baseScene(proj.base), patch);
        proj.patch = patch;
        proj.title = patch.title;
        proj.modified = new Date().toISOString();
        state.erg.saves++;
        return { saved: true, warnings: [] };
      },
      "level.themes": () => ({ themes: THEMES, timesOfDay: ["DAY", "EVENING", "NIGHT"],
        materialFiles: ["ThemeBuilding\\ThemeBuilding.txt", "ThemeCamelot\\ThemeCamelot.txt"] }),
      "level.palette": () => [
        { name: "oildrum", resource: "OilDrum", role: "object", preview: "" },
        { name: "mine", resource: "Mine", role: "object", preview: "" },
      ],
      "level.close": () => ({}),
    },
  };
}
