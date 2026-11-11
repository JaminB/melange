// Tool group "ledge" (detail layer round 6): rock that follows the surface instead of standing on it. place_aligned lays
// copies of a scenery mesh as slabs turned to the face (x along the contour, z out of the face pitched toward its normal)
// and sunk part of their depth into it, at any non-uniform size; plan_ledges finds a region's steep faces and plans strata
// bands at 2-3 repeated heights along their contours; find_crests answers ridge-crest and cliff-top polylines for crowns;
// lint_perched flags clumps whose underside shows from given cameras. Pure maths in lib/ledge.ts. Copies have no collision:
// sim_fingerprint is unchanged.
import { AddDetail, Group, dropPoint, targetFrame, type Vec3 } from "../core";
import { DEG, commit, topOf, unitsSchema, type Units } from "../lib/place-geom";
import { meshBounds } from "../lib/backdrop";
import { classifyDetail } from "../lib/sim-fingerprint";
import { placedMeshes } from "../lib/scenery-mesh";
import { clumps, type Piece } from "../lib/dress-lint";
import { alignedPose, findCrests, perchedFromView, planBands, surfaceFrame, type Ground, type Region, type V3 } from "../lib/ledge";
import { loadScene } from "./dress";
import { withTint } from "../lib/tint";
import { ToolError, type Schema, type Tool } from "../registry";
import type { Session } from "../session";

const r2 = (v: number) => Math.round(v * 100) / 100;
const r3 = (v: number) => Math.round(v * 1000) / 1000;

/** The voxels' ground (map units): top, slope and normal from the top `h` either side. */
function groundFn(s: Session, h = 0.75) {
  const store = s.store!, top = topOf(store);
  const vox = (id: number) => store.voxelsOf({ id });
  const tops = new Map<string, number | null>();
  const at = (x: number, z: number) => {
    const key = `${x.toFixed(3)},${z.toFixed(3)}`;
    let y = tops.get(key);
    if (y === undefined) { y = dropPoint(store.scene, store.frames, vox, [x, top, z])?.[1] ?? null; tops.set(key, y); }
    return y;
  };
  const cache = new Map<string, Ground | null>();
  const g = (x: number, z: number): Ground | null => {
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
  return { g, at };
}

const regionSchema = {
  box: { type: "array", items: { type: "number" }, minItems: 4, maxItems: 4, description: "[x0, z0, x1, z1] (the call's units)." },
  centre: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 }, radius: { type: "number", minimum: 0.01 },
  exclude: { type: "array", maxItems: 2000, description: "Circles [{xz, r}] (the call's units) nothing is planned in: keep-outs such as trails, courts, spawns and pads.",
    items: { type: "object", additionalProperties: false, required: ["xz", "r"], properties: { xz: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 }, r: { type: "number", minimum: 0 } } } },
  minY: { type: "number", description: "Skip ground under this height (the call's units), e.g. the sea." },
} satisfies Record<string, Schema>;
type RegionArgs = { box?: number[]; centre?: number[]; radius?: number; exclude?: { xz: number[]; r: number }[]; minY?: number };

function regionOf(a: RegionArgs, k: number): { region: Region; keep: (x: number, z: number) => boolean; minY: number } {
  let region: Region;
  if (a.box) region = { box: [a.box[0] / k, a.box[1] / k, a.box[2] / k, a.box[3] / k] };
  else if (a.centre && a.radius) region = { centre: [a.centre[0] / k, a.centre[1] / k], radius: a.radius / k };
  else throw new ToolError("give box [x0, z0, x1, z1] or centre and radius");
  const ex = (a.exclude ?? []).map((e) => ({ x: e.xz[0] / k, z: e.xz[1] / k, r: e.r / k }));
  return { region, keep: (x, z) => ex.every((e) => Math.hypot(x - e.x, z - e.z) >= e.r), minY: a.minY !== undefined ? a.minY / k : -Infinity };
}

export const tools: Tool[] = [
  {
    name: "place_aligned",
    description: "Place copies of one base scenery mesh (`id`, scenery_catalog's example.id of `base`) ALIGNED TO THE SURFACE: each piece is a slab of `size` [length, thickness, depth] (map units of the call; any non-uniform size, e.g. 3:0.6:1 for a strata ledge) whose length runs along the slope's contour (or `tangent` [tx, tz]), whose depth points out of the face (the horizontal downhill pitched toward the face normal by `pitch`: 0 a level shelf, 1 a plate lying on the face; default 0.25) and which is sunk `embed` of its depth into the face along that axis (default 0.5). The ground's normal is sampled at `xz` (or given as `normal`, with `pos` the surface point); `lift` moves it up its own y; `jitter` [about x, y, z] degrees turns it in its own frame. `dry: true` answers the poses only (each piece's `front` (how far it stands out of the face), top and bottom). Tagged _<tag> (default dtl), `tint` per piece or call. One undo step. Refused for a detail sim_fingerprint counts as sim.",
    inputSchema: { type: "object", additionalProperties: false, required: ["id", "pieces"], properties: {
      base: { type: "string" }, id: { type: "integer" },
      pieces: { type: "array", minItems: 1, maxItems: 256, items: { type: "object", additionalProperties: false, required: ["size"], properties: {
        xz: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 }, pos: { type: "array", items: { type: "number" }, minItems: 3, maxItems: 3 },
        normal: { type: "array", items: { type: "number" }, minItems: 3, maxItems: 3 }, tangent: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 },
        size: { type: "array", items: { type: "number", minimum: 0.01 }, minItems: 3, maxItems: 3 },
        pitch: { type: "number", minimum: 0, maximum: 1 }, embed: { type: "number", minimum: 0, maximum: 0.95 }, lift: { type: "number" },
        jitter: { type: "array", items: { type: "number" }, minItems: 3, maxItems: 3 }, tint: { type: "string" },
      } } },
      pitch: { type: "number", minimum: 0, maximum: 1 }, embed: { type: "number", minimum: 0, maximum: 0.95 }, tint: { type: "string" },
      step: { type: "number", minimum: 0.1, maximum: 10, description: "Spacing for the sampled normal, map units (default 0.75)." },
      tag: { type: "string" }, allowOtherTheme: { type: "boolean" }, dry: { type: "boolean" }, units: unitsSchema,
    } },
    async handler(s, a: { base?: string; id: number; pieces: { xz?: number[]; pos?: number[]; normal?: number[]; tangent?: number[]; size: number[]; pitch?: number; embed?: number; lift?: number; jitter?: number[]; tint?: string }[];
      pitch?: number; embed?: number; tint?: string; step?: number; tag?: string; allowOtherTheme?: boolean; dry?: boolean; units?: Units }) {
      const { store } = s.requireProject();
      const tag = a.tag ?? "dtl";
      if (!/^[A-Za-z0-9]{1,12}$/.test(tag)) throw new ToolError("tag: 1-12 letters or digits");
      const key = a.base ?? store.scene.base.key, scene = await loadScene(s, key);
      if (scene.databank.theme !== store.scene.databank.theme && !a.allowOtherTheme)
        throw new ToolError(`${key} is ${scene.databank.theme} and this level ${store.scene.databank.theme}: pass allowOtherTheme to try (and Test it)`);
      const src = scene.details.find((d) => d.id === a.id);
      if (!src) throw new ToolError(`no detail ${a.id} in ${key}`);
      const cl = classifyDetail(src);
      if (cl.cls !== "visual" || cl.role !== "scenery") throw new ToolError(`detail ${src.id} (${src.name}) is not a visual scenery piece (${cl.cls === "sim" ? cl.why : cl.role})`);
      const mesh = await meshBounds(s, scene, src.resource);
      if (!mesh) throw new ToolError(`${src.resource} has no preview mesh: its size is unknown`);
      const k = (a.units ?? "world") === "map" ? 1 : 20, { g } = groundFn(s, a.step ?? 0.75), frame = targetFrame(store.scene, "scenery");
      const cmds: AddDetail[] = [], out: object[] = [], skipped: object[] = [];
      a.pieces.forEach((p, i) => {
        let at: V3, normal: V3;
        if (p.pos) {
          at = [p.pos[0] / k, p.pos[1] / k, p.pos[2] / k];
          const gg = p.normal ? null : g(at[0], at[2]);
          if (!p.normal && !gg) { skipped.push({ i, why: "no ground" }); return; }
          normal = (p.normal as V3) ?? gg!.normal;
        } else if (p.xz) {
          const gg = g(p.xz[0] / k, p.xz[1] / k);
          if (!gg) { skipped.push({ i, why: "no ground" }); return; }
          at = [p.xz[0] / k, gg.y, p.xz[1] / k]; normal = (p.normal as V3) ?? gg.normal;
        } else throw new ToolError(`pieces[${i}]: give xz or pos`);
        const f = surfaceFrame(normal, p.pitch ?? a.pitch ?? 0.25, p.tangent as [number, number] | undefined);
        const size: V3 = [p.size[0] / k, p.size[1] / k, p.size[2] / k];
        const pose = alignedPose(mesh, at, f, size, p.embed ?? a.embed ?? 0.5, (p.lift ?? 0) / k, (p.jitter as V3) ?? [0, 0, 0]);
        let name: string;
        try { name = withTint(`${src.name}_${tag}`, p.tint ?? a.tint ?? null); } catch (e) { throw new ToolError(`pieces[${i}].${(e as Error).message}`); }
        cmds.push(new AddDetail(frame, { name, resource: src.resource, pos: store.frames.toLocal(frame, pose.pos), rot: pose.rot.map((v) => v / DEG) as Vec3, scale: pose.scale }));
        const gc = g(pose.centre[0], pose.centre[2]);
        out.push({ i, at: at.map((v) => r2(v * k)), centre: pose.centre.map((v) => r2(v * k)), out: pose.out.map(r3), front: r2(pose.front * k), top: r2(pose.top * k), bottom: r2(pose.bottom * k),
          groundAtCentre: gc ? r2(gc.y * k) : null, slope: r2(Math.acos(Math.max(-1, Math.min(1, normal[1] / (Math.hypot(...normal) || 1)))) * DEG),
          ...(a.dry ? { pose: { pos: pose.pos.map((v) => r2(v * k)), rot: pose.rot.map(r2), scale: pose.scale.map(r3) } } : {}) });
      });
      if (!a.dry && cmds.length) commit(store, cmds.length === 1 ? cmds[0] : new Group(`Place ${cmds.length} aligned piece(s)`, cmds));
      return { units: a.units ?? "world", placed: a.dry ? 0 : cmds.length, pieces: out, ...(skipped.length ? { skipped } : {}), note: "decoration only: copies have no collision; sim_fingerprint should be unchanged" };
    },
  },
  {
    name: "plan_ledges",
    description: "Plan STRATA on a region's steep faces (`box` [x0, z0, x1, z1] or `centre` + `radius`, minus `exclude` circles and ground under `minY`): faces are connected grid cells (`step`, default 1.5 map) over `minSlope` degrees (default 52) with at least `minCells` (12); each face gets band heights: `heights` (absolute, the same on every face), or every `spacing` from `offset` (level strata shared by all faces), or `count` (default 3, fewer on a short face; at least `minGap` 3 apart) spread over the face's 10-90% height range. Each band's contour is traced across the face (cells moved down their fall line onto the height), chained into polylines (at least `minRun` long, default 4) and cut into slabs of `length` [min, max] (default [2.6, 4.2]) with `gap` [min, max] between them (default [0.2, 0.9]); `breaks` (default 0.15) of them left out so a band reads broken. Answers faces (cells, height range, band heights), the band polylines and the slabs ({at: the surface point, normal, tangent, length, face, band, height}) to pass to place_aligned with a size [length, thickness, depth]. Read-only.",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      ...regionSchema,
      step: { type: "number", minimum: 0.3, maximum: 10 }, minSlope: { type: "number", minimum: 10, maximum: 89 }, minCells: { type: "integer", minimum: 1 },
      count: { type: "integer", minimum: 1, maximum: 8 }, heights: { type: "array", items: { type: "number" }, maxItems: 32 }, spacing: { type: "number", minimum: 0.1 }, offset: { type: "number" }, minGap: { type: "number", minimum: 0 },
      length: { type: "array", items: { type: "number", minimum: 0.1 }, minItems: 2, maxItems: 2 }, gap: { type: "array", items: { type: "number", minimum: 0 }, minItems: 2, maxItems: 2 },
      breaks: { type: "number", minimum: 0, maximum: 0.95 }, minRun: { type: "number", minimum: 0 }, seed: { type: "integer" }, points: { type: "boolean", description: "Include the band polylines' points (default false: counts only)." },
      units: unitsSchema,
    } },
    handler(s, a: RegionArgs & { step?: number; minSlope?: number; minCells?: number; count?: number; heights?: number[]; spacing?: number; offset?: number; minGap?: number; length?: number[]; gap?: number[]; breaks?: number; minRun?: number; seed?: number; points?: boolean; units?: Units }) {
      s.requireProject();
      const k = (a.units ?? "world") === "map" ? 1 : 20, { g } = groundFn(s), { region, keep, minY } = regionOf(a, k);
      const ground = (x: number, z: number) => { const r = g(x, z); return r && r.y >= minY ? r : null; };
      const d = (v?: number) => (v === undefined ? undefined : v / k);
      const plan = planBands(ground, region, { step: d(a.step), minSlope: a.minSlope, minCells: a.minCells, count: a.count, heights: a.heights?.map((v) => v / k), spacing: d(a.spacing), offset: d(a.offset), minGap: d(a.minGap),
        length: a.length?.map((v) => v / k) as [number, number] | undefined, gap: a.gap?.map((v) => v / k) as [number, number] | undefined, breaks: a.breaks, minRun: d(a.minRun), seed: a.seed, keep });
      return { units: a.units ?? "world",
        faces: plan.faces.map((f) => ({ face: f.face, cells: f.cells, yMin: r2(f.yMin * k), yMax: r2(f.yMax * k), heights: f.heights.map((h) => r2(h * k)) })),
        bands: plan.bands.map((b) => ({ face: b.face, band: b.band, height: r2(b.height * k), points: a.points ? b.points.map((p) => p.map((v) => r2(v * k))) : b.points.length })),
        slabs: plan.slabs.map((sl) => ({ at: sl.at.map((v) => r2(v * k)), normal: sl.normal.map(r3), tangent: sl.tangent.map(r3), length: r2(sl.length * k), face: sl.face, band: sl.band, height: r2(sl.height * k) })) };
    },
  },
  {
    name: "find_crests",
    description: "Find the silhouette lines of a region (`box` or `centre` + `radius`, minus `exclude` circles and ground under `minY`) on a `step` grid (default 1.5 map): `crest` points stand `rise` (default 0.35 reach) over both sides at `reach` (default 2 steps) in one of four directions (ridge lines, rim crests); `cliffTop` points are gentler than `topSlope` (default 40) with a drop steeper than `cliffSlope` (default 55) within `reach` in one of eight directions (cliff edges). Chained into polylines at least `minRun` long (default 5), longest first; each point has its outward direction [dx, dz] (where the ground falls away: a crown leans and faces that way) and the drop. `kinds` picks which. Read-only.",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      ...regionSchema,
      step: { type: "number", minimum: 0.3, maximum: 10 }, reach: { type: "number", minimum: 0.1 }, rise: { type: "number", minimum: 0 }, cliffSlope: { type: "number", minimum: 10, maximum: 89 }, topSlope: { type: "number", minimum: 0, maximum: 89 },
      kinds: { type: "array", items: { type: "string", enum: ["crest", "cliffTop"] } }, minRun: { type: "number", minimum: 0 }, limit: { type: "integer", minimum: 1, maximum: 500 },
      units: unitsSchema,
    } },
    handler(s, a: RegionArgs & { step?: number; reach?: number; rise?: number; cliffSlope?: number; topSlope?: number; kinds?: ("crest" | "cliffTop")[]; minRun?: number; limit?: number; units?: Units }) {
      s.requireProject();
      const k = (a.units ?? "world") === "map" ? 1 : 20, { g, at } = groundFn(s), { region, keep, minY } = regionOf(a, k);
      const ground = (x: number, z: number) => { const r = g(x, z); return r && r.y >= minY ? r : null; };
      const d = (v?: number) => (v === undefined ? undefined : v / k);
      const lines = findCrests(ground, region, { step: d(a.step), reach: d(a.reach), rise: d(a.rise), cliffSlope: a.cliffSlope, topSlope: a.topSlope, kinds: a.kinds, minRun: d(a.minRun), keep }, at);
      return { units: a.units ?? "world", count: lines.length, lines: lines.slice(0, a.limit ?? 100).map((l) => ({ kind: l.kind, length: r2(l.length * k),
        points: l.points.map((p) => ({ at: [r2(p.x * k), r2(p.y * k), r2(p.z * k)], out: p.out.map(r3), drop: r2(p.drop * k) })) })) };
    },
  },
  {
    name: "lint_perched",
    description: "Find decoration that reads as perched or floating FROM THE CAMERAS: the scenery copies whose names contain `search` (default \"_dtl\") as drawn, grouped into clumps (pieces within `gap`, default 0.4 map), and for each clump the vertices in its lowest `low` share of its height (default 0.35) that stand over the ground beneath them by more than `hover` (default 0.3 map; nothing beneath counts) on the side facing a camera of `cams` [{name, pos}] (map units). A clump is flagged when that gap is over `maxGap` (default the larger of 0.6 and `share` (0.35) of the clump's height) — a shelf's small overhang on a face is normal, a block whose whole underside shows against a lower face or the sky is not. Answers the flagged clumps (ids, centre, size, gap, exposed share, cameras), worst first. Read-only.",
    inputSchema: { type: "object", additionalProperties: false, required: ["cams"], properties: {
      search: { type: "string" }, gap: { type: "number", minimum: 0 }, hover: { type: "number", minimum: 0 }, low: { type: "number", minimum: 0.05, maximum: 1 },
      maxGap: { type: "number", minimum: 0 }, share: { type: "number", minimum: 0 }, limit: { type: "integer", minimum: 0, maximum: 500 },
      cams: { type: "array", minItems: 1, maxItems: 64, items: { type: "object", additionalProperties: false, required: ["pos"], properties: { name: { type: "string" }, pos: { type: "array", items: { type: "number" }, minItems: 3, maxItems: 3 } } } },
    } },
    async handler(s, a: { search?: string; gap?: number; hover?: number; low?: number; maxGap?: number; share?: number; limit?: number; cams: { name?: string; pos: number[] }[] }) {
      const { store } = s.requireProject();
      const q = (a.search ?? "_dtl").toLowerCase();
      const details = store.scene.details.filter((d) => d.src === null && classifyDetail(d).role === "scenery" && d.name.toLowerCase().includes(q));
      if (!details.length) throw new ToolError(`no scenery copy's name contains ${JSON.stringify(a.search ?? "_dtl")}`);
      const { meshes } = await placedMeshes(s, store.frames, details);
      const vertsOf = new Map<number, V3[]>();
      const pieces: Piece[] = meshes.map((m) => {
        const min: V3 = [Infinity, Infinity, Infinity], max: V3 = [-Infinity, -Infinity, -Infinity], vs: V3[] = [];
        for (let i = 0; i < m.tris.length; i += 3) { const v: V3 = [m.tris[i], m.tris[i + 1], m.tris[i + 2]]; vs.push(v); for (let j = 0; j < 3; j++) { min[j] = Math.min(min[j], v[j]); max[j] = Math.max(max[j], v[j]); } }
        vertsOf.set(m.detail, vs);
        return { id: m.detail, box: { min, max }, lum: 0 };
      });
      const { at } = groundFn(s);
      const cams = a.cams.map((c, i) => ({ name: c.name ?? `cam${i}`, pos: c.pos as V3 }));
      const flagged = clumps(pieces, a.gap ?? 0.4).map((c) => {
        const vs = c.ids.flatMap((id) => vertsOf.get(id) ?? []);
        // a coarse sample of the vertices keeps it quick on big layers
        const every = Math.max(1, Math.floor(vs.length / 600));
        const r = perchedFromView(vs.filter((_, i) => i % every === 0), at, cams, a.hover ?? 0.3, a.low ?? 0.35);
        const h = c.box.max[1] - c.box.min[1], limit = a.maxGap ?? Math.max(0.6, (a.share ?? 0.35) * h);
        return { c, r, limit };
      }).filter((x) => x.r.gap > x.limit).sort((p, q2) => q2.r.gap / q2.limit - p.r.gap / p.limit);
      return { units: "map", clumps: pieces.length ? clumps(pieces, a.gap ?? 0.4).length : 0, perched: flagged.length,
        list: flagged.slice(0, a.limit ?? 40).map((x) => ({ ids: x.c.ids.slice(0, 8), at: [r2(x.c.centre[0]), r2(x.c.box.min[1]), r2(x.c.centre[1])], size: r2(x.c.size), gap: x.r.gap === Infinity ? "nothing beneath" : r2(x.r.gap), share: r2(x.r.share), cams: x.r.cams })) };
    },
  },
];
