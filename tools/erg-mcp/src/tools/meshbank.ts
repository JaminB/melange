// Tool group "meshbank" (detail layer round 10; the round-9 judge's tool gaps: "mod mesh-bank import for custom non-colliding
// meshes" and "a placement filter for contact normal and slope break (cliff foot vs face)"):
//   place_mod_mesh    scenery copies of a MOD mesh (a mesh bank a content mod loads through its manifest's `meshes`, see
//                     docs/spice.md and docs/meshes.md; e.g. bench/caldera/meshes' caldera.Boulder), placed free or aligned
//                     to the surface like place_aligned. The detail is an ordinary VISIBLE_ scenery copy whose resource is
//                     the mod mesh's name: visual only (no collision; sim_fingerprint's scenery part).
//   plan_slope_break  the feet of steep faces (lib/slope-break.ts): where talus rests, with the downhill direction, and
//                     optional talus fans (big blocks at the toe, small at the apex, a half-buried skirt). Read-only.
//   lint_contact      the ground's contact angle under a layer's props; `remove` drops those over `maxDeg`.
//   swap_mesh         re-points scenery copies of one mesh at another (e.g. the faceted HORROR5 rock at the matte
//                     caldera.Boulder), each copy rescaled per axis and moved so the new mesh fills the old one's box.
import { AddDetail, Group, RemoveDetail, apply as applyMat, dropPoint, setMany, targetFrame, type Detail, type Vec3 } from "../core";
import { meshBounds } from "../lib/backdrop";
import { loadScene } from "./dress";
import { DEG, commit, topOf, unitsSchema, type Units } from "../lib/place-geom";
import { classifyDetail } from "../lib/sim-fingerprint";
import { alignedPose, surfaceFrame, type V3 } from "../lib/ledge";
import { withTint } from "../lib/tint";
import { contactAngle, slopeBreaks, talusFan, type Ground } from "../lib/slope-break";
import { ToolError, type Tool } from "../registry";
import type { Session } from "../session";

const r2 = (v: number) => Math.round(v * 100) / 100;
const r3 = (v: number) => Math.round(v * 1000) / 1000;
/** A mod mesh's name: "<modId>.<Name>" (docs/spice.md: every name in a bank starts with the mod's id and a dot). */
export const MOD_MESH = /^[a-z0-9][a-z0-9_-]{0,63}\.[A-Za-z0-9_.-]{1,96}$/;

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
    description: "Place scenery copies of a MOD MESH: `resource` \"<modId>.<Name>\" from a mesh bank a content mod loads (its spice.json `meshes`; docs/spice.md), `box` its bounds {min, max} in map units (the bank's meshes.json; the editor has no preview of a mod mesh, so the size comes from here). Each piece is either FREE (`pos` or `xz` dropped onto the voxels, `rot` degrees, `scale`) or ALIGNED like place_aligned (`size` [length, thickness, depth], the ground normal sampled at `xz` or given, `tangent`, `pitch`, `embed`, `lift`, `jitter`). The detail is named VISIBLE_<stem>_<tag> (+ the tint tag): a visual scenery copy (no collision; sim_fingerprint's scenery part). The mesh draws only where the mod's bank is loaded (the level's pack must ship it); `dry` answers the poses. One undo step.",
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
    } },
    handler(s, a: { resource: string; box: { min: number[]; max: number[] }; stem?: string; tag?: string; dry?: boolean; units?: Units; pitch?: number; embed?: number; tint?: string;
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
          const rot = (p.rot ?? [0, 0, 0]) as Vec3;
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
    description: "The ground's contact angle (degrees between the ground normal and up, sampled over `step` either side) under each scenery copy whose name or resource contains `search` (default _dtl): answers those over `maxDeg` (default 50: a prop there reads as stuck to a face, not resting on ground) and, with `remove`, removes them (one undo step). Props with no ground under them (over the sea) are counted apart and removed with `removeNoGround`.",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      search: { type: "string" }, maxDeg: { type: "number", minimum: 0, maximum: 90 }, step: { type: "number", minimum: 0.1, maximum: 5 },
      remove: { type: "boolean" }, removeNoGround: { type: "boolean" }, limit: { type: "integer", minimum: 1, maximum: 4000, description: "How many of the props over maxDeg to list (default 50)." }, units: unitsSchema,
    } },
    handler(s, a: { search?: string; maxDeg?: number; step?: number; remove?: boolean; removeNoGround?: boolean; limit?: number; units?: Units }) {
      const { store } = s.requireProject();
      const k = (a.units ?? "world") === "map" ? 1 : 20, q = (a.search ?? "_dtl").toLowerCase(), max = a.maxDeg ?? 50, g = groundOf(s, a.step ?? 0.75);
      const list = store.scene.details.filter((d: Detail) => d.src === null && `${d.name} ${d.resource}`.toLowerCase().includes(q) && classifyDetail(d).cls === "visual");
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
];
