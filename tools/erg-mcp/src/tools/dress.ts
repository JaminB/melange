// Tool group "dress": the art catalog for a decoration pass (bench/caldera/detail.mjs). scenery_catalog lists the base
// levels' sim-neutral pieces (scenery, lights, emitters, sounds: lib/sim-fingerprint.ts classifyDetail) by resource,
// with the size of each piece's preview mesh, its theme and a category guessed from its names (rock, flora, fx), so an
// agent picks rocks and plants without opening every base by hand.
import { withTint } from "../lib/tint";
import { AddDetail, Frames, Group, dropPoint, targetFrame, type Detail, type Scene, type Vec3 } from "../core";
import { DEG, commit, pointOut, resolvePoint, topOf, unitsSchema, vec3, type Units } from "../lib/place-geom";
import { meshBounds } from "../lib/backdrop";
import { classifyDetail } from "../lib/sim-fingerprint";
import { kindOf, scaleOf } from "./scenery";
import { tidy } from "../lib/voxel";
import { hex, luminance, sceneryMesh } from "../lib/scenery-mesh";
import { drawCheck } from "../lib/scatter";
import { ToolError, type Tool } from "../registry";
import type { Session } from "../session";

const CATEGORY: [string, RegExp][] = [
  ["rock", /rock|boulder|stone|cliff|rubble|scree|crystal|basalt|outcrop|pebble|cave|crag|slab|menhir|monolith|lava|meteor|geode|stalag|skull|bone|fossil|egg/i],
  ["flora", /tree|palm|bush|shrub|grass|fern|plant|flower|vine|cact|weed|leaf|leaves|log|stump|mushroom|toadstool|reed|bamboo|hedge|moss|foliage|frond|branch|root|ivy|lily|kelp|seaweed|thorn|bramble|cycad|conifer|pine|oak|willow|hay|straw|corn|crop|veg|tuft/i],
  ["fx", /smoke|steam|fire|flame|ember|spark|glow|dust|torch|lava|magma|bubble|mist|fog|drip|splash|water|candle|lamp|light/i],
];
export const categoryOf = (text: string) => CATEGORY.filter(([, re]) => re.test(text)).map(([c]) => c);

const sceneCache = new WeakMap<object, Map<string, Promise<Scene>>>();
export async function loadScene(s: Session, key: string): Promise<Scene> {
  const c = await s.client();
  let per = sceneCache.get(c);
  if (!per) sceneCache.set(c, (per = new Map()));
  if (!per.has(key)) per.set(key, (await s.levels()).loadBase(key, "game").then((r) => r.scene));
  try { return await per.get(key)!; } catch (e) { per.delete(key); throw e; }
}

const r2 = (v: number) => Math.round(v * 100) / 100;

export const tools: Tool[] = [
  {
    name: "scenery_catalog",
    description: "The base levels' sim-neutral pieces for decoration, by resource: scenery meshes, lights, emitters and sounds whose names carry no engine tag but VISIBLE (sim_fingerprint's visual channels), each with its base and theme, role, kinds (names without tags and numbers), count, an example detail (id, name, scale, rot) for copy_base_scenery, the preview mesh's size in map units (`meshSize`, unscaled, and `size` at the example's scale; null where the server serves no mesh), its `albedo` (#rrggbb, the average of its texture) and `luminance` (0-1) and `category` guessed from its names and resource (rock, flora, fx; empty when nothing matched) and, with a project open, `inLevel`: whether the piece is known to draw in a level of the open level's theme (another theme's piece may not draw or may draw as something else; lib/scatter.ts DRAW_CHECKS holds what Tests showed). Default: every game base; or `bases`, `themes`. Filters: `roles`, `category`, `search`. `sizes: false` skips the mesh fetches. Read-only; works with no project open.",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      bases: { type: "array", items: { type: "string" }, minItems: 1, maxItems: 80, description: "Base keys (list_bases); default every game base (or those of `themes`)." },
      themes: { type: "array", items: { type: "string" }, minItems: 1, maxItems: 11, description: "Only bases of these themes." },
      roles: { type: "array", items: { type: "string", enum: ["scenery", "light", "emitter", "sound"] }, minItems: 1, maxItems: 4, description: "Roles to list (default all four)." },
      category: { type: "string", enum: ["rock", "flora", "fx", "none"], description: "Only pieces of this guessed category (none: nothing matched)." },
      search: { type: "string", description: "Kind, name or resource contains this (any case)." },
      sizes: { type: "boolean", description: "Fetch each resource's preview mesh for its size (default true)." },
      includeSim: { type: "boolean", description: "Also list the details sim_fingerprint counts as sim (other roles, tagged names), each with `sim`: why. Not for decoration: to see what a base has (default false)." },
      limit: { type: "integer", minimum: 1, maximum: 2000, description: "Resources to answer (default 400)." },
      sort: { type: "string", enum: ["count", "dark", "light"], description: "Order: count (default, most used first), dark or light (by the mesh's albedo luminance; pieces with no mesh last)." },
      maxLuminance: { type: "number", minimum: 0, maximum: 1, description: "Only pieces whose albedo luminance (0 black - 1 white) is at most this: dark rock for a basalt cone." },
    } },
    async handler(s, a: { bases?: string[]; themes?: string[]; roles?: string[]; category?: string; search?: string; sizes?: boolean; limit?: number; includeSim?: boolean; sort?: string; maxLuminance?: number }) {
      const list = await (await s.levels()).list();
      let bases = list.bases.filter((b) => b.source === "game");
      if (a.bases) bases = a.bases.map((k) => { const b = list.bases.find((x) => x.key === k); if (!b) throw new ToolError(`no base ${k} (list_bases)`); return b; });
      if (a.themes) bases = bases.filter((b) => a.themes!.includes(b.theme));
      if (!bases.length) throw new ToolError("no base matches");
      const roles = new Set(a.roles ?? ["scenery", "light", "emitter", "sound"]), q = a.search?.toLowerCase();
      type Row = { resource: string; base: string; theme: string; role: string; sim: string | null; kinds: Set<string>; names: Set<string>; count: number; example: Detail; scene: Scene };
      const rows = new Map<string, Row>(), failed: { base: string; error: string }[] = [];
      for (const b of bases) {
        let scene: Scene;
        try { scene = await loadScene(s, b.key); } catch (e) { failed.push({ base: b.key, error: e instanceof Error ? e.message : String(e) }); continue; }
        for (const d of scene.details) {
          const c = classifyDetail(d);
          if (c.cls !== "visual" ? !a.includeSim || ["spawn", "object", "camera"].includes(c.role) : !roles.has(c.role)) continue;
          const kind = kindOf(d.name);
          if (q && !`${kind} ${d.name} ${d.resource}`.toLowerCase().includes(q)) continue;
          const k = `${b.key}|${d.resource}|${c.cls}`;
          const r = rows.get(k) ?? rows.set(k, { resource: d.resource, base: b.key, theme: scene.databank.theme, role: c.role, sim: c.cls === "sim" ? c.why : null, kinds: new Set(), names: new Set(), count: 0, example: d, scene }).get(k)!;
          r.kinds.add(kind); r.names.add(d.name); r.count++;
        }
      }
      let out = [...rows.values()].map((r) => ({ r, category: categoryOf(`${[...r.names].join(" ")} ${r.resource}`) }));
      if (a.category) out = out.filter((x) => (a.category === "none" ? !x.category.length : x.category.includes(a.category!)));
      out = out.sort((x, y) => y.r.count - x.r.count);
      const lum = new Map<string, { albedo: string; luminance: number } | null>();
      if (a.sizes !== false && (a.sort === "dark" || a.sort === "light" || a.maxLuminance !== undefined)) {
        for (const { r } of out) if (r.role === "scenery" && !lum.has(r.resource.toLowerCase())) {
          const m = await sceneryMesh(s, r.resource);
          lum.set(r.resource.toLowerCase(), m && { albedo: hex(m.albedo), luminance: luminance(m.albedo) });
        }
        const L = (x: (typeof out)[number]) => lum.get(x.r.resource.toLowerCase())?.luminance;
        if (a.maxLuminance !== undefined) out = out.filter((x) => (L(x) ?? 2) <= a.maxLuminance!);
        if (a.sort === "dark" || a.sort === "light") out.sort((x, y) => ((L(x) ?? 9) - (L(y) ?? 9)) * (a.sort === "dark" ? 1 : -1) || y.r.count - x.r.count);
      }
      out = out.slice(0, a.limit ?? 400);
      const answer = [];
      const levelTheme = s.project?.store.scene.databank.theme ?? null;
      for (const { r, category } of out) {
        let meshSize: number[] | null = null;
        if (a.sizes !== false && (r.role === "scenery" || r.sim)) {
          const m = await meshBounds(s, r.scene, r.resource).catch(() => null);
          if (m) meshSize = [0, 1, 2].map((i) => r2(m.max[i] - m.min[i]));
          if (!lum.has(r.resource.toLowerCase()) && r.role === "scenery") { const mm = await sceneryMesh(s, r.resource); lum.set(r.resource.toLowerCase(), mm && { albedo: hex(mm.albedo), luminance: luminance(mm.albedo) }); }
        }
        const al = lum.get(r.resource.toLowerCase());
        const sc = r.example.scale;
        answer.push({ resource: r.resource, base: r.base, theme: r.theme, role: r.role, ...(r.sim ? { sim: r.sim } : {}), category, kinds: [...r.kinds].slice(0, 6), names: [...r.names].slice(0, 4), count: r.count,
          meshSize, ...(al ? { albedo: al.albedo, luminance: al.luminance } : {}), ...(levelTheme && r.role === "scenery" ? { inLevel: drawCheck(levelTheme, r.theme, r.resource) } : {}), size: meshSize ? meshSize.map((v, i) => r2(v * Math.abs(sc[i]))) : null,
          example: { id: r.example.id, name: r.example.name, at: tidy(new Frames(r.scene).detailWorld(r.example)), rot: tidy(r.example.rot.map((v) => v * DEG) as Vec3), scale: tidy(sc) } });
      }
      return { bases: bases.length, resources: rows.size, answered: answer.length, units: "map", list: answer, ...(failed.length ? { failed } : {}),
        note: "copy_base_scenery {base, copies: [{id: example.id, pos, rot, scale}]} places scenery (allowOtherTheme for another theme's base: Test that it loads); lights, emitters and sounds are not copied by copy_base_scenery. Copies have no collision: keep them off trails, courts, spawns and pads" };
    },
  },
  {
    name: "copy_base_details",
    description: "Place copies of a base level's sim-neutral details: scenery meshes and also lights (PNTLGHT r g b radius), particle emitters (EMITTER_WXP_..., the name picks the effect) and sound effects, which copy_base_scenery does not copy. Each copy is `id` (a detail of that base: scenery_catalog's example.id) at `pos` or `xz` (dropped onto the voxels), with rot (degrees) and scale (default the source's). Refused for a detail sim_fingerprint counts as sim (a spawn, object or camera, a collision volume, a name tagged MORTAL and the like), so a dressing pass cannot change the sim. Copies have no collision. Another theme's base needs allowOtherTheme (Test that it loads). The copies go into the level's folder frame for their role. One undo step.",
    inputSchema: { type: "object", additionalProperties: false, required: ["copies"], properties: {
      base: { type: "string", description: "Base key to copy from (default the open project's base)." },
      copies: { type: "array", minItems: 1, maxItems: 256, items: { type: "object", additionalProperties: false, required: ["id"], properties: {
        id: { type: "integer", description: "A detail id of the base." },
        pos: vec3("[x, y, z] in the level."), xz: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2, description: "[x, z]: dropped onto the voxels below." },
        drop: { type: "boolean", description: "Drop pos onto the voxels below it." },
        rot: vec3("Rotation in degrees about x, y, z (default the source's)."),
        scale: { description: "Scale (default the source's): a number (0.05-20) or [x, y, z]." },
        seat: { type: "number", minimum: 0, maximum: 0.8, description: "Seat a scenery mesh in the ground: lower it so this fraction of its height (the preview mesh at the copy's scale, upright) is below the point; 0 rests the mesh's lowest point on the point, so a base never floats." },
        tint: { type: "string", description: "\"#rrggbb\": recolour this scenery copy (overrides the call's tint; tint_scenery has the details)." },
      } } },
      tint: { type: "string", description: "\"#rrggbb\": recolour every scenery copy toward this colour (a \"_tint<rrggbb>\" name tag Sunstone's model shaders read; the texture's detail is kept; Sunstone off: the piece's own texture). Visual only." },
      tag: { type: "string", description: "Letters and digits. Append _<tag> to the names of scenery copies (never lights, emitters or sounds: their names pick the effect), so a pass finds and removes its own (remove_scenery {search: \"_<tag>\"}) and can run twice without doubling." },
      allowOtherTheme: { type: "boolean", description: "Copy from a base of another theme than the level's." },
      frame: { type: "integer", description: "Put the copies in this frame (an id from describe_level {allFrames}) instead of the level's folder for their role: positions are still level positions, converted into the frame's space. The engine may place a detail from its frame's chain differently from the editor (round 1 found copies in the 'Worms' folder away from where the editor showed them): test a frame in game before relying on it." },
      units: unitsSchema,
    } },
    async handler(s, a: { base?: string; copies: { id: number; pos?: number[]; xz?: number[]; drop?: boolean; rot?: number[]; scale?: unknown; seat?: number; tint?: string }[]; allowOtherTheme?: boolean; units?: Units; tag?: string; frame?: number; tint?: string }) {
      const { store } = s.requireProject();
      if (a.tag !== undefined && !/^[A-Za-z0-9]{1,12}$/.test(a.tag)) throw new ToolError("tag: 1-12 letters or digits");
      const key = a.base ?? store.scene.base.key;
      const scene = await loadScene(s, key);
      if (scene.databank.theme !== store.scene.databank.theme && !a.allowOtherTheme)
        throw new ToolError(`${key} is ${scene.databank.theme} and this level ${store.scene.databank.theme}: pass allowOtherTheme to try (and Test it)`);
      const u = a.units ?? "world";
      const cmds: AddDetail[] = [], placed: object[] = [];
      const bounds = new Map<string, Awaited<ReturnType<typeof meshBounds>>>();
      for (const c of a.copies) if (c.seat !== undefined) {
        const src = scene.details.find((d) => d.id === c.id);
        if (src && !bounds.has(src.resource)) bounds.set(src.resource, await meshBounds(s, scene, src.resource).catch(() => null));
      }
      a.copies.forEach((c, i) => {
        const src = scene.details.find((d) => d.id === c.id);
        if (!src) throw new ToolError(`copies[${i}]: no detail ${c.id} in ${key}`);
        const cl = classifyDetail(src);
        if (cl.cls !== "visual") throw new ToolError(`copies[${i}]: detail ${src.id} (${src.name}, ${src.resource}) is sim (${cl.why}): only scenery, lights, emitters and sounds with no engine tag are copied`);
        const at = resolvePoint(store, c, u, `copies[${i}]`).map;
        const frame = a.frame ?? targetFrame(store.scene, cl.role as "scenery");
        const rot: Vec3 = c.rot ? [c.rot[0] / DEG, c.rot[1] / DEG, c.rot[2] / DEG] : [src.rot[0], src.rot[1], src.rot[2]];
        const scale: Vec3 = c.scale !== undefined ? scaleOf(c.scale, `copies[${i}].scale`) : [src.scale[0], src.scale[1], src.scale[2]];
        if (c.seat !== undefined && cl.role === "scenery") {
          const mb = bounds.get(src.resource);
          if (mb) at[1] += -mb.min[1] * Math.abs(scale[1]) - c.seat * (mb.max[1] - mb.min[1]) * Math.abs(scale[1]);
        }
        const tagged = a.tag && cl.role === "scenery" ? `${src.name}_${a.tag}` : src.name;
        const tint = cl.role === "scenery" ? c.tint ?? a.tint : undefined;
        let name = tagged;
        if (tint !== undefined) { try { name = withTint(tagged, tint); } catch (e) { throw new ToolError(`copies[${i}].${(e as Error).message}`); } }
        cmds.push(new AddDetail(frame, { name, resource: src.resource, pos: store.frames.toLocal(frame, at), rot, scale }));
        placed.push({ from: src.id, name, resource: src.resource, role: cl.role, frame, at: pointOut(at, u) });
      });
      commit(store, cmds.length === 1 ? cmds[0] : new Group(`Copy ${cmds.length} details`, cmds));
      return { placed, note: "decoration only: copies have no collision; sim_fingerprint should be unchanged", undo: "undo removes them all" };
    },
  },
  {
    name: "surface_sample",
    description: "Sample the open level's ground at many points: for each [x, z] the top of the voxels under it (`y`), the slope in degrees (from the surface `step` either side) and the surface normal, or null where nothing is below. For seating decoration on the real surface (a scatter pass filters by slope and height) instead of a formula that may disagree with sheared or draped cells. Up to 4000 points per call. Read-only.",
    inputSchema: { type: "object", additionalProperties: false, required: ["points"], properties: {
      points: { type: "array", minItems: 1, maxItems: 4000, items: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 }, description: "[[x, z], ...]" },
      step: { type: "number", minimum: 0.1, maximum: 10, description: "Spacing for the slope, in map units (default 0.75)." },
      units: unitsSchema,
    } },
    handler(s, a: { points: number[][]; step?: number; units?: Units }) {
      const { store } = s.requireProject();
      const u = a.units ?? "world", k = u === "map" ? 1 : 20, h = a.step ?? 0.75, top = topOf(store);
      const vox = (id: number) => store.voxelsOf({ id });
      const at = (x: number, z: number) => dropPoint(store.scene, store.frames, vox, [x, top, z])?.[1] ?? null;
      const out = a.points.map(([px, pz]) => {
        const x = px / k, z = pz / k, y = at(x, z);
        if (y === null) return null;
        const xm = at(x - h, z), xp = at(x + h, z), zm = at(x, z - h), zp = at(x, z + h);
        const dx = ((xp ?? y) - (xm ?? y)) / ((xp !== null ? h : 0) + (xm !== null ? h : 0) || 1);
        const dz = ((zp ?? y) - (zm ?? y)) / ((zp !== null ? h : 0) + (zm !== null ? h : 0) || 1);
        const n = Math.hypot(dx, 1, dz);
        return { y: r2(y * k), slope: r2(Math.atan(Math.hypot(dx, dz)) * DEG), normal: [r2(-dx / n), r2(1 / n), r2(-dz / n)] };
      });
      return { units: u, samples: out };
    },
  },
];
