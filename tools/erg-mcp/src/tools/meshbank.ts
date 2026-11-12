// Tool group "meshbank" (detail layer round 10; the round-9 judge's tool gaps: "mod mesh-bank import for custom non-colliding
// meshes" and "a placement filter for contact normal and slope break (cliff foot vs face)"):
//   place_mod_mesh    scenery copies of a MOD mesh (a mesh bank a content mod loads through its manifest's `meshes`, see
//                     docs/spice.md and docs/meshes.md; e.g. bench/caldera/meshes' caldera.Boulder), placed free or aligned
//                     to the surface like place_aligned. The detail is an ordinary VISIBLE_ scenery copy whose resource is
//                     the mod mesh's name: visual only (no collision; sim_fingerprint's scenery part).
//   plan_slope_break  the feet of steep faces (lib/slope-break.ts): where talus rests, with the downhill direction, and
//                     optional talus fans (big blocks at the toe, small at the apex, a half-buried skirt). Read-only.
//   lint_contact      the ground's contact angle under a layer's props; `remove` drops those over `maxDeg`.
//   lint_mesh_banks   the engine section each mod mesh bank loads in with the installed plugins, across plugin sets.
//   swap_mesh         re-points scenery copies of one mesh at another (e.g. the faceted HORROR5 rock at the matte
//                     caldera.Boulder), each copy rescaled per axis and moved so the new mesh fills the old one's box.
import { AddDetail, Group, RemoveDetail, apply as applyMat, dropPoint, setMany, targetFrame, type Detail, type Vec3 } from "../core";
import { meshBounds } from "../lib/backdrop";
import { matchesSearch, searchSchema, searchTerms, type Search } from "../lib/detail-search";
import { loadScene } from "./dress";
import { DEG, commit, topOf, unitsSchema, type Units } from "../lib/place-geom";
import { classifyDetail } from "../lib/sim-fingerprint";
import { alignedPose, surfaceFrame, type V3 } from "../lib/ledge";
import { withTint } from "../lib/tint";
import { contactAngle, slopeBreaks, talusFan, type Ground } from "../lib/slope-break";
import { execFileSync } from "node:child_process";
import { existsSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { assignSections, compareSets, freeBuiltSections, loadOrder, type BankIn, type BankOut, type ModIn } from "../lib/meshbank-lint";
import { findXomtool } from "../lib/rockbank";
import { ToolError, type Tool } from "../registry";
import type { Session } from "../session";

const r2 = (v: number) => Math.round(v * 100) / 100;
const r3 = (v: number) => Math.round(v * 1000) / 1000;
/** A mod mesh's name: "<modId>.<Name>" (docs/spice.md: every name in a bank starts with the mod's id and a dot). */
export const MOD_MESH = /^[a-z0-9][a-z0-9_-]{0,63}\.[A-Za-z0-9_.-]{1,96}$/;

/** A free piece's rotation as the detail stores it (radians): taken as given by default (what every call before `rotUnits` was
 *  written for), or converted from degrees. */
export function freeRot(rot: number[] | undefined, units?: "radians" | "degrees"): Vec3 {
  const r = rot ?? [0, 0, 0];
  return (units === "degrees" ? r.map((v) => v / DEG) : r) as Vec3;
}

/** The voxels' ground (map units) with the slope and normal from the top `h` either side (ledge.ts's, kept local). */
function groundOf(s: Session, h = 0.75) {
  const store = s.store!, top = topOf(store), vox = (id: number) => store.voxelsOf({ id });
  const tops = new Map<string, number | null>(), cache = new Map<string, Ground | null>();
  const at = (x: number, z: number) => {
    const key = `${x.toFixed(3)},${z.toFixed(3)}`;
    let y = tops.get(key);
    if (y === undefined) { y = dropPoint(store.scene, store.frames, vox, [x, top, z])?.[1] ?? null; tops.set(key, y); }
    return y;
  };
  return (x: number, z: number): Ground | null => {
    const key = `${x.toFixed(3)},${z.toFixed(3)}`;
    if (cache.has(key)) return cache.get(key)!;
    const y = at(x, z);
    let out: Ground | null = null;
    if (y !== null) {
      const xm = at(x - h, z), xp = at(x + h, z), zm = at(x, z - h), zp = at(x, z + h);
      const dx = ((xp ?? y) - (xm ?? y)) / ((xp !== null ? h : 0) + (xm !== null ? h : 0) || 1);
      const dz = ((zp ?? y) - (zm ?? y)) / ((zp !== null ? h : 0) + (zm !== null ? h : 0) || 1);
      const n = Math.hypot(dx, 1, dz);
      out = { y, slope: Math.atan(Math.hypot(dx, dz)) * DEG, normal: [-dx / n, 1 / n, -dz / n] };
    }
    cache.set(key, out);
    return out;
  };
}

const v2 = { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 } as const;
const v3s = { type: "array", items: { type: "number" }, minItems: 3, maxItems: 3 } as const;

export const tools: Tool[] = [
  {
    name: "place_mod_mesh",
    description: "Place scenery copies of a MOD MESH: `resource` \"<modId>.<Name>\" from a mesh bank a content mod loads (its spice.json `meshes`; docs/spice.md), `box` its bounds {min, max} in map units (the bank's meshes.json; the editor has no preview of a mod mesh, so the size comes from here). Each piece is either FREE (`pos` or `xz` dropped onto the voxels, `rot` about x, y, z, `scale`: `rot` is taken as RADIANS unless `rotUnits` is \"degrees\", which is what every earlier call was written for; the stored detail's rot is in radians, `list_scenery` shows it in degrees) or ALIGNED like place_aligned (`size` [length, thickness, depth], the ground normal sampled at `xz` or given, `tangent`, `pitch`, `embed`, `lift`, `jitter`). The detail is named VISIBLE_<stem>_<tag> (+ the tint tag): a visual scenery copy (no collision; sim_fingerprint's scenery part). The mesh draws only where the mod's bank is loaded (the level's pack must ship it). Ship ONE bank per map holding all its meshes (bench/caldera/meshes/merge.mjs joins one-mesh banks): every bank takes one of the 44 mod mesh sections all enabled mods share. `dry` answers the poses. One undo step.",
    inputSchema: { type: "object", additionalProperties: false, required: ["resource", "box", "pieces"], properties: {
      resource: { type: "string" },
      box: { type: "object", additionalProperties: false, required: ["min", "max"], properties: { min: v3s, max: v3s } },
      stem: { type: "string", description: "The name's stem (default the mesh name after the dot)." },
      pieces: { type: "array", minItems: 1, maxItems: 256, items: { type: "object", additionalProperties: false, properties: {
        xz: v2, pos: v3s, rot: v3s, scale: { ...v3s, description: "[x, y, z] (a free piece; default [1, 1, 1])" },
        size: { type: "array", items: { type: "number", minimum: 0.01 }, minItems: 3, maxItems: 3 },
        normal: v3s, tangent: v2, pitch: { type: "number", minimum: 0, maximum: 1 }, embed: { type: "number", minimum: 0, maximum: 0.95 },
        lift: { type: "number" }, jitter: v3s, tint: { type: "string" },
      } } },
      pitch: { type: "number", minimum: 0, maximum: 1 }, embed: { type: "number", minimum: 0, maximum: 0.95 }, tint: { type: "string" },
      tag: { type: "string" }, dry: { type: "boolean" }, units: unitsSchema,
      rotUnits: { enum: ["radians", "degrees"], description: "The unit of a free piece's `rot` (default radians, as every call so far has been written: an aligned piece's pose is computed in degrees and needs no unit). With degrees, [0, 90, 0] turns a piece a quarter turn; as radians it turns it 90 rad (about 116 degrees), which only a random yaw can use." },
    } },
    handler(s, a: { resource: string; rotUnits?: "radians" | "degrees"; box: { min: number[]; max: number[] }; stem?: string; tag?: string; dry?: boolean; units?: Units; pitch?: number; embed?: number; tint?: string;
      pieces: { xz?: number[]; pos?: number[]; rot?: number[]; scale?: number[]; size?: number[]; normal?: number[]; tangent?: number[]; pitch?: number; embed?: number; lift?: number; jitter?: number[]; tint?: string }[] }) {
      const { store } = s.requireProject();
      if (!MOD_MESH.test(a.resource)) throw new ToolError(`resource '${a.resource}': a mod mesh is "<modId>.<Name>" (lower-case mod id, a dot, the mesh name)`);
      const tag = a.tag ?? "dtl";
      if (!/^[A-Za-z0-9]{1,12}$/.test(tag)) throw new ToolError("tag: 1-12 letters or digits");
      const stem = a.stem ?? a.resource.slice(a.resource.indexOf(".") + 1);
      if (!/^[A-Za-z0-9]{1,40}$/.test(stem)) throw new ToolError("stem: 1-40 letters or digits");
      const k = (a.units ?? "world") === "map" ? 1 : 20;
      const box = { min: a.box.min.map((v) => v / k) as V3, max: a.box.max.map((v) => v / k) as V3 };
      if ([0, 1, 2].some((i) => !(box.max[i] > box.min[i]))) throw new ToolError("box: max must exceed min on every axis");
      const g = groundOf(s), frame = targetFrame(store.scene, "scenery");
      const cmds: AddDetail[] = [], out: object[] = [], skipped: object[] = [];
      a.pieces.forEach((p, i) => {
        let name: string;
        try { name = withTint(`VISIBLE_${stem}_${tag}`, p.tint ?? a.tint ?? null); } catch (e) { throw new ToolError(`pieces[${i}].${(e as Error).message}`); }
        const cl = classifyDetail({ name, resource: a.resource });
        if (cl.cls !== "visual" || cl.role !== "scenery") throw new ToolError(`pieces[${i}]: ${name} would not be a visual scenery copy (${cl.why})`);
        if (p.size) {
          let at: V3, normal: V3;
          if (p.pos) { at = [p.pos[0] / k, p.pos[1] / k, p.pos[2] / k]; const gg = p.normal ? null : g(at[0], at[2]); if (!p.normal && !gg) { skipped.push({ i, why: "no ground" }); return; } normal = (p.normal as V3) ?? gg!.normal; }
          else if (p.xz) { const gg = g(p.xz[0] / k, p.xz[1] / k); if (!gg) { skipped.push({ i, why: "no ground" }); return; } at = [p.xz[0] / k, gg.y, p.xz[1] / k]; normal = (p.normal as V3) ?? gg.normal; }
          else throw new ToolError(`pieces[${i}]: give xz or pos`);
          const f = surfaceFrame(normal, p.pitch ?? a.pitch ?? 0.25, p.tangent as [number, number] | undefined);
          const pose = alignedPose(box, at, f, [p.size[0] / k, p.size[1] / k, p.size[2] / k], p.embed ?? a.embed ?? 0.5, (p.lift ?? 0) / k, (p.jitter as V3) ?? [0, 0, 0]);
          cmds.push(new AddDetail(frame, { name, resource: a.resource, pos: store.frames.toLocal(frame, pose.pos), rot: pose.rot.map((v) => v / DEG) as Vec3, scale: pose.scale }));
          out.push({ i, name, at: at.map((v) => r2(v * k)), centre: pose.centre.map((v) => r2(v * k)), top: r2(pose.top * k), bottom: r2(pose.bottom * k), front: r2(pose.front * k),
            slope: r2(contactAngle(normal)), ...(a.dry ? { pose: { pos: pose.pos.map((v) => r2(v * k)), rot: pose.rot.map(r2), scale: pose.scale.map(r3) } } : {}) });
        } else {
          let at: V3;
          if (p.pos) at = [p.pos[0] / k, p.pos[1] / k, p.pos[2] / k];
          else if (p.xz) { const gg = g(p.xz[0] / k, p.xz[1] / k); if (!gg) { skipped.push({ i, why: "no ground" }); return; } at = [p.xz[0] / k, gg.y, p.xz[1] / k]; }
          else throw new ToolError(`pieces[${i}]: give xz or pos (and size for an aligned piece)`);
          const sc = p.scale ?? [1, 1, 1];
          const rot = freeRot(p.rot, a.rotUnits);
          cmds.push(new AddDetail(frame, { name, resource: a.resource, pos: store.frames.toLocal(frame, at), rot, scale: sc as Vec3 }));
          out.push({ i, name, at: at.map((v) => r2(v * k)), rot, scale: sc });
        }
      });
      if (!a.dry && cmds.length) commit(store, cmds.length === 1 ? cmds[0] : new Group(`Place ${cmds.length} ${a.resource} piece(s)`, cmds));
      return { units: a.units ?? "world", resource: a.resource, placed: a.dry ? 0 : cmds.length, pieces: out.slice(0, 300), ...(skipped.length ? { skipped } : {}),
        note: "decoration only: a scenery copy of a mod mesh has no collision and draws only where its mod's mesh bank is loaded; sim_fingerprint should be unchanged" };
    },
  },
  {
    name: "plan_slope_break",
    description: "The FEET of steep faces in a box (map units if units:\"map\"): grid cells (`step`, default 1.5) whose own ground is gentle (slope <= `footMax`, default 32) with a face (>= `faceMin`, default 45) rising at least `rise` (default 2) within 2 x `probe` (default 2.5) uphill. That is where fallen rock comes to rest (talus), not on the face. Sites are chained into runs along the contour; each has y, its slope, the face's slope and rise, and `down` (the horizontal downhill direction, where a fan spreads). `minY`/`maxY` bound the height; `exclude` circles are skipped. With `fans` {every, n, length, big, small, skirt, spread, seed} it also answers talus fans at every `every`-th site of each run: pieces graded big at the toe, small at the apex by the face, with a half-buried skirt, each with its ground and contact angle and `ok` false when the contact is over `maxContact` (default 50 degrees) or there is no ground. Read-only.",
    inputSchema: { type: "object", additionalProperties: false, required: ["box"], properties: {
      box: { type: "array", items: { type: "number" }, minItems: 4, maxItems: 4 },
      step: { type: "number", minimum: 0.3, maximum: 10 }, faceMin: { type: "number", minimum: 10, maximum: 89 }, footMax: { type: "number", minimum: 0, maximum: 60 },
      probe: { type: "number", minimum: 0.3 }, rise: { type: "number", minimum: 0 }, minY: { type: "number" }, maxY: { type: "number" },
      exclude: { type: "array", maxItems: 4000, items: { type: "object", additionalProperties: false, required: ["xz", "r"], properties: { xz: v2, r: { type: "number", minimum: 0 } } } },
      fans: { type: "object", additionalProperties: false, properties: { every: { type: "integer", minimum: 1 }, n: { type: "integer", minimum: 1, maximum: 40 }, length: { type: "number", minimum: 0.1 },
        big: { type: "number", minimum: 0.05 }, small: { type: "number", minimum: 0.05 }, skirt: { type: "integer", minimum: 0, maximum: 40 }, spread: { type: "number", minimum: 0, maximum: 3.2 }, seed: { type: "integer" } } },
      maxContact: { type: "number", minimum: 0, maximum: 90 }, limit: { type: "integer", minimum: 1, maximum: 20000 }, units: unitsSchema,
    } },
    handler(s, a: { box: number[]; step?: number; faceMin?: number; footMax?: number; probe?: number; rise?: number; minY?: number; maxY?: number; exclude?: { xz: number[]; r: number }[];
      fans?: { every?: number; n?: number; length?: number; big?: number; small?: number; skirt?: number; spread?: number; seed?: number }; maxContact?: number; limit?: number; units?: Units }) {
      s.requireProject();
      const k = (a.units ?? "world") === "map" ? 1 : 20, g = groundOf(s), d = (v?: number) => (v === undefined ? undefined : v / k);
      const ex = (a.exclude ?? []).map((e) => ({ x: e.xz[0] / k, z: e.xz[1] / k, r: e.r / k }));
      const res = slopeBreaks(g, { box: a.box.map((v) => v / k) as [number, number, number, number], step: d(a.step), faceMin: a.faceMin, footMax: a.footMax, probe: d(a.probe), rise: d(a.rise),
        minY: d(a.minY), maxY: d(a.maxY), keep: (x, z) => ex.every((e) => Math.hypot(x - e.x, z - e.z) >= e.r) });
      const maxC = a.maxContact ?? 50;
      let fans: object[] | undefined;
      if (a.fans) {
        let seed = (a.fans.seed ?? 1) >>> 0;
        const R = () => ((seed = (seed * 1664525 + 1013904223) >>> 0) / 4294967296);
        const every = a.fans.every ?? 4, seen = new Map<number, number>();
        fans = [];
        for (const site of res.sites) {
          const c = seen.get(site.run) ?? 0; seen.set(site.run, c + 1);
          if (c % every) continue;
          const pieces = talusFan(site, { n: a.fans.n ?? 7, length: (a.fans.length ?? 3) / k, big: (a.fans.big ?? 1.2) / k, small: (a.fans.small ?? 0.4) / k, skirt: a.fans.skirt, spread: a.fans.spread, R });
          fans.push({ site: [r2(site.x * k), r2(site.y * k), r2(site.z * k)], run: site.run, down: site.down.map(r3), pieces: pieces.map((p) => {
            const gg = g(p.x, p.z), ang = gg ? contactAngle(gg.normal) : null;
            return { xz: [r2(p.x * k), r2(p.z * k)], y: gg ? r2(gg.y * k) : null, size: r2(p.size * k), kind: p.kind, embed: r2(p.embed), contact: ang === null ? null : r2(ang),
              normal: gg ? gg.normal.map(r3) : null, ok: !!gg && ang! <= maxC };
          }) });
        }
      }
      return { units: a.units ?? "world", cells: res.cells, runs: res.runs, count: res.sites.length,
        sites: res.sites.slice(0, a.limit ?? 2000).map((p) => ({ xz: [r2(p.x * k), r2(p.z * k)], y: r2(p.y * k), slope: r2(p.slope), faceSlope: r2(p.faceSlope), faceRise: r2(p.faceRise * k), down: p.down.map(r3), run: p.run })),
        ...(fans ? { fans } : {}) };
    },
  },
  {
    name: "lint_contact",
    description: "The ground's contact angle (degrees between the ground normal and up, sampled over `step` either side) under each scenery copy whose name or resource contains `search` (a fragment or a list of them, default _dtl; lint_support reads it the same way): answers those over `maxDeg` (default 50: a prop there reads as stuck to a face, not resting on ground) and, with `remove`, removes them (one undo step). Props with no ground under them (over the sea) are counted apart and removed with `removeNoGround`.",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      search: searchSchema, maxDeg: { type: "number", minimum: 0, maximum: 90 }, step: { type: "number", minimum: 0.1, maximum: 5 },
      remove: { type: "boolean" }, removeNoGround: { type: "boolean" }, limit: { type: "integer", minimum: 1, maximum: 4000, description: "How many of the props over maxDeg to list (default 50)." }, units: unitsSchema,
    } },
    handler(s, a: { search?: Search; maxDeg?: number; step?: number; remove?: boolean; removeNoGround?: boolean; limit?: number; units?: Units }) {
      const { store } = s.requireProject();
      const k = (a.units ?? "world") === "map" ? 1 : 20, q = searchTerms(a.search), max = a.maxDeg ?? 50, g = groundOf(s, a.step ?? 0.75);
      const list = store.scene.details.filter((d: Detail) => d.src === null && matchesSearch(d, q) && classifyDetail(d).cls === "visual");
      const over: { id: number; at: number[]; contact: number }[] = [], none: number[] = [];
      for (const d of list) {
        const p = store.frames.detailWorld(d), gg = g(p[0], p[2]);
        if (!gg) { none.push(d.id); continue; }
        const c = contactAngle(gg.normal);
        if (c > max) over.push({ id: d.id, at: [r2(p[0] * k), r2(p[1] * k), r2(p[2] * k)], contact: r2(c) });
      }
      const drop = [...(a.remove ? over.map((o) => o.id) : []), ...(a.removeNoGround ? none : [])];
      if (drop.length) commit(store, drop.length === 1 ? new RemoveDetail(drop[0]) : new Group(`Remove ${drop.length} props over ${max} degrees`, drop.map((id) => new RemoveDetail(id))));
      return { units: a.units ?? "world", checked: list.length, over: over.length, noGround: none.length, removed: drop.length, first: over.slice(0, a.limit ?? 50) };
    },
  },
  {
    name: "swap_mesh",
    description: "Re-point scenery copies at another mesh: every copy (ids, or name/resource contains `search`) whose resource is `from` takes `to` (a mod mesh \"<modId>.<Name>\" or a game resource), rescaled per axis and moved so the new mesh fills the old one's box: `toBox` {min, max} map units (a mod mesh's meshes.json box), `fromBox` likewise or looked up from `fromBase`'s preview of `from`. The copies keep their names (and tints), rotations and places: a material or model swap that the sim cannot see (scenery copies have no collision; sim_fingerprint's scenery part). `dryRun` answers the count. One undo step.",
    inputSchema: { type: "object", additionalProperties: false, required: ["from", "to", "toBox"], properties: {
      from: { type: "string" }, to: { type: "string" }, ids: { type: "array", items: { type: "integer" }, maxItems: 4000 }, search: { type: "string" },
      toBox: { type: "object", additionalProperties: false, required: ["min", "max"], properties: { min: v3s, max: v3s } },
      fromBox: { type: "object", additionalProperties: false, required: ["min", "max"], properties: { min: v3s, max: v3s } },
      fromBase: { type: "string" }, keepScale: { type: "boolean", description: "Keep each copy's scale (no box fit)." }, dryRun: { type: "boolean" },
    } },
    async handler(s, a: { from: string; to: string; ids?: number[]; search?: string; toBox: { min: number[]; max: number[] }; fromBox?: { min: number[]; max: number[] }; fromBase?: string; keepScale?: boolean; dryRun?: boolean }) {
      const { store } = s.requireProject();
      if (!MOD_MESH.test(a.to) && !/^[A-Za-z0-9_.-]{1,96}$/.test(a.to)) throw new ToolError(`to '${a.to}': not a mesh name`);
      let from = a.fromBox;
      if (!from && !a.keepScale) {
        if (!a.fromBase) throw new ToolError("give fromBox, or fromBase (a base whose preview of `from` gives its box), or keepScale");
        const b = await meshBounds(s, await loadScene(s, a.fromBase), a.from);
        if (!b) throw new ToolError(`${a.from} has no preview mesh in ${a.fromBase}: pass fromBox`);
        from = { min: [...b.min], max: [...b.max] };
      }
      const q = a.search?.toLowerCase(), lc = a.from.toLowerCase();
      const list = (a.ids ? a.ids.map((id) => { const d = store.detail(id); if (!d) throw new ToolError(`no detail ${id}`); return d; }) : store.scene.details)
        .filter((d: Detail) => d.src === null && d.resource.toLowerCase() === lc && (!q || `${d.name} ${d.resource}`.toLowerCase().includes(q)));
      const changes: [number, { resource: string; pos?: Vec3; scale?: Vec3 }][] = [];
      for (const d of list) {
        const cl = classifyDetail({ name: d.name, resource: a.to });
        if (cl.cls !== "visual" || cl.role !== "scenery") throw new ToolError(`detail ${d.id} (${d.name}) with ${a.to} would not be a visual scenery copy (${cl.why})`);
        if (a.keepScale || !from) { changes.push([d.id, { resource: a.to }]); continue; }
        const fs = [0, 1, 2].map((i) => from!.max[i] - from!.min[i]), ts = [0, 1, 2].map((i) => a.toBox.max[i] - a.toBox.min[i]);
        const fc = [0, 1, 2].map((i) => (from!.max[i] + from!.min[i]) / 2) as Vec3, tc = [0, 1, 2].map((i) => (a.toBox.max[i] + a.toBox.min[i]) / 2);
        const ratio = [0, 1, 2].map((i) => fs[i] / (ts[i] || 1));
        const M = store.frames.detailMatrix(d), o = applyMat(M, [0, 0, 0]), lin = (v: Vec3) => { const p = applyMat(M, v); return [p[0] - o[0], p[1] - o[1], p[2] - o[2]] as Vec3; };
        const target = applyMat(M, fc), off = lin([ratio[0] * tc[0], ratio[1] * tc[1], ratio[2] * tc[2]]);
        const world: Vec3 = [target[0] - off[0], target[1] - off[1], target[2] - off[2]];
        const fw = store.frames.worldOf(d.frame), parentLocal = fw ? store.frames.toLocal(d.frame, world) : world;
        changes.push([d.id, { resource: a.to, pos: parentLocal, scale: [d.scale[0] * ratio[0], d.scale[1] * ratio[1], d.scale[2] * ratio[2]] as Vec3 }]);
      }
      if (!changes.length) return { changed: 0, matched: 0 };
      if (!a.dryRun) commit(store, setMany(changes, `Swap ${changes.length} ${a.from} copies to ${a.to}`));
      return { changed: a.dryRun ? 0 : changes.length, matched: changes.length, dryRun: !!a.dryRun, fromBox: from ?? null,
        note: "the copies keep names, tints and rotations; sim_fingerprint should be unchanged (scenery copies have no collision)" };
    },
  },
  {
    name: "lint_mesh_banks",
    description: "Which engine section each mod mesh bank loads in with the plugins that are installed, and what moves: Melange loads every enabled content mod's banks once per launch in load order (a mod after the mods it depends on, else by id; a mod's banks in its spice.json order), a bank keeps the section it was built with when that is one of the 44 mod sections (476..519) and still free, else it is rewritten into the first free one, and past 44 banks the rest do not load (docs/meshes.md). So a map's bank built on a section another plugin's bank was built on moves one of the two, and which one depends on which plugins are on. Reads <gameDir>\\Mods\\<id>\\spice.json and each bank's own SectionId and mesh names (xomtool inspect; nothing is changed, the game may run). `mods`: the plugin ids to load (default: those turned on in Mods\\thumper-state.json); `sets`: several plugin sets to compare instead ([[\"sunstone\"], [\"sunstone\", \"kindjal\", \"sandtide\"]]); `pack`: a pack folder that is not installed (an export's output, with its spice.json) added to every set; `focus`: the mod whose banks are followed across the sets (default the pack's id). Answers per set the load order, every bank that moved or did not load (and all of the focus mod's) with the mesh names it carries, the sections used and free, mesh names two banks define, and `suggest`: the sections no bank of any set was built with (build_rock_bank {section}) so the map's bank keeps its section whatever is on.",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      gameDir: { type: "string", description: "The game folder whose Mods are read (default MELANGE_GAME_DIR)." },
      mods: { type: "array", maxItems: 64, items: { type: "string" }, description: "The plugin ids that are on (default: thumper-state.json's)." },
      sets: { type: "array", minItems: 1, maxItems: 8, items: { type: "array", maxItems: 64, items: { type: "string" } }, description: "Plugin sets to compare, each a list of ids." },
      pack: { type: "string", description: "A pack folder outside Mods (its spice.json and banks), added to every set." },
      focus: { type: "string", description: "The mod id whose banks are compared across the sets (default the pack's)." },
      xomtool: { type: "string", description: "xomtool.exe (default: the checkout's dist\\tools\\xomtool.exe, or $XOMTOOL)." },
    } },
    handler(_s: Session, a: { gameDir?: string; mods?: string[]; sets?: string[][]; pack?: string; focus?: string; xomtool?: string }) {
      const game = a.gameDir ?? process.env.MELANGE_GAME_DIR;
      if (!game || !existsSync(join(game, "Mods"))) throw new ToolError("gameDir: a game folder with a Mods folder (or set MELANGE_GAME_DIR)");
      if (a.mods && a.sets) throw new ToolError("give mods (one set) or sets (several), not both");
      let xomtool: string;
      try { xomtool = findXomtool(a.xomtool); } catch (e) { throw new ToolError((e as Error).message); }
      const inspect = (...args: string[]) => execFileSync(xomtool, ["inspect", ...args], { encoding: "utf8", windowsHide: true, timeout: 60000 });
      // a bank's facts: its descriptors' names, and the section of the first (a bank holds one section)
      const readBank = (mod: string, dir: string, file: string): BankIn => {
        const f = join(dir, file);
        if (!existsSync(f)) return { mod, file, section: null, meshes: [], error: "the file is missing" };
        try {
          const list = [...inspect(f, "--type", "XMeshDescriptor").matchAll(/^#(\d+) XMeshDescriptor\s+ResourceId=(\S+)/gm)];
          if (!list.length) return { mod, file, section: null, meshes: [], error: "not a mesh bank (no XMeshDescriptor)" };
          const sec = /^\s*SectionId = (\d+)/m.exec(inspect(f, "--object", `#${list[0][1]}`));
          return { mod, file, section: sec ? Number(sec[1]) : null, meshes: list.map((m) => m[2]) };
        } catch (e) {
          return { mod, file, section: null, meshes: [], error: `xomtool could not read it: ${(e as Error).message.split("\n")[0]}` };
        }
      };
      const cache = new Map<string, ModIn | null>();
      const readMod = (dir: string): ModIn | null => {
        const f = join(dir, "spice.json");
        if (!existsSync(f)) return null;
        let j: { id?: string; kind?: string; meshes?: { file?: string }[]; dependencies?: unknown[] };
        try { j = JSON.parse(readFileSync(f, "utf8").replace(/^﻿/, "")); } catch (e) { throw new ToolError(`${f}: ${(e as Error).message}`); }
        if (typeof j.id !== "string") throw new ToolError(`${f}: no id`);
        // a dependency is "id" or "id >=1.2.0"
        const deps = (Array.isArray(j.dependencies) ? j.dependencies : []).map((d) => String(typeof d === "object" && d ? (d as { id?: string }).id ?? "" : d).trim().split(/[\s<>=@]/)[0]).filter(Boolean);
        const banks = j.kind === "content" && Array.isArray(j.meshes) ? j.meshes.filter((m) => typeof m?.file === "string").map((m) => readBank(j.id!, dir, m.file!)) : [];
        return { id: j.id, dependencies: deps, banks };
      };
      const modOf = (id: string) => {
        if (!cache.has(id)) cache.set(id, readMod(join(game, "Mods", id)));
        return cache.get(id) ?? null;
      };
      const pack = a.pack ? readMod(a.pack) : null;
      if (a.pack && !pack) throw new ToolError(`pack: ${a.pack} has no spice.json`);
      let sets: string[][];
      if (a.sets) sets = a.sets;
      else if (a.mods) sets = [a.mods];
      else {
        const st = join(game, "Mods", "thumper-state.json");
        const en = existsSync(st) ? (JSON.parse(readFileSync(st, "utf8")) as { enabled?: Record<string, boolean> }).enabled ?? {} : {};
        sets = [Object.keys(en).filter((id) => en[id] === true && existsSync(join(game, "Mods", id, "spice.json")))];
      }
      const focus = a.focus ?? pack?.id;
      const all: BankIn[] = [];
      const results = sets.map((ids) => {
        const missing = ids.filter((id) => id !== pack?.id && !modOf(id));
        const mods = [...ids.filter((id) => id !== pack?.id).map(modOf).filter((m): m is ModIn => !!m), ...(pack ? [pack] : [])];
        const order = loadOrder(mods), banks = order.flatMap((m) => m.banks);
        all.push(...banks);
        const as = assignSections(banks);
        return { name: [...ids, ...(pack && !ids.includes(pack.id) ? [pack.id] : [])].join("+") || "(none)", ids, missing, order, as };
      });
      const suggest = freeBuiltSections(all);
      const brief = (b: BankOut) => ({ mod: b.mod, file: b.file, built: b.section, section: b.resolved, ...(b.relocated ? { relocated: true } : {}), ...(b.why ? { why: b.why } : {}),
        meshes: b.meshes.length > 6 ? [...b.meshes.slice(0, 6), `... ${b.meshes.length - 6} more`] : b.meshes });
      const warnings: string[] = [];
      const out = results.map((r) => {
        const moved = r.as.banks.filter((b) => b.relocated || b.resolved === null);
        for (const b of moved) warnings.push(`${r.name}: ${b.mod}/${b.file} ${b.resolved === null ? "does not load" : `moves from section ${b.section} to ${b.resolved}`}: ${b.why}`);
        for (const d of r.as.duplicateNames) warnings.push(`${r.name}: the mesh ${d.name} is defined by ${d.banks.join(" and ")}: the second is refused by the game`);
        if (r.missing.length) warnings.push(`${r.name}: no plugin ${r.missing.join(", ")} in ${join(game, "Mods")}`);
        return { set: r.name, loadOrder: r.order.filter((m) => m.banks.length).map((m) => `${m.id} (${m.banks.length})`), banks: r.as.banks.length, sectionsUsed: r.as.used, sectionsFree: r.as.free,
          relocated: r.as.relocated, notLoaded: r.as.notLoaded, moved: moved.map(brief), ...(focus ? { [focus]: r.as.banks.filter((b) => b.mod === focus).map(brief) } : {}) };
      });
      const cmp = focus ? compareSets(results.map((r) => ({ name: r.name, assignment: r.as })), focus) : [];
      const shifting = cmp.filter((c) => c.shifts);
      return { ok: !warnings.length, sets: out, ...(focus ? { focus, across: cmp } : {}),
        suggest: { sections: suggest.slice(0, 12), free: suggest.length,
          note: suggest.length ? `no bank of these sets was built on these: a bank built on one keeps its section in every set (build_rock_bank {section: ${suggest[0]}})` : "every mod section is some bank's built section: any new bank is moved" },
        ...(shifting.length ? { shifts: `${focus}'s ${shifting.map((c) => c.file).join(", ")} is not in the section it was built with in every set: rebuild it on a suggested section` } : {}),
        warnings,
        note: "load order here is the plugins' default: a mod after its dependencies, else by id; a player who reorders the Mods page changes it. A bank that moves is loaded from a copy with its section rewritten (Melange\\cache\\meshes); the game's log says where each one loaded ([meshes] ... section N is taken; the bank is loaded as section M)" };
    },
  },
];
