// Tool group "kitbash": crags kitbashed from copies of one base mesh, seated in the ground and leaned to its slope
// (lib/scatter.ts), for a decoration pass (bench/caldera/detail-rock.mjs). One crag is 1-4 interpenetrating copies
// (spire, ledge, stack, tor, single) placed as one seeded unit, so a pass breaks a single mesh's silhouette without
// hand-placing every piece. Copies have no collision (decoration only: sim_fingerprint is unchanged).
import { AddDetail, Group, dropPoint, targetFrame, type Vec3 } from "../core";
import { DEG, commit, topOf, unitsSchema, type Units } from "../lib/place-geom";
import { meshBounds } from "../lib/backdrop";
import { classifyDetail } from "../lib/sim-fingerprint";
import { ARCHETYPES, composeCrag, footprint, kitbash, rng, tiltTo, type Archetype, type V3 } from "../lib/scatter";
import { loadScene } from "./dress";
import { withTint } from "../lib/tint";
import { ToolError, type Tool } from "../registry";

const r2 = (v: number) => Math.round(v * 100) / 100;

export const tools: Tool[] = [
  {
    name: "place_crags",
    description: "Place kitbashed crags: each is 1-4 interpenetrating copies of one base scenery mesh (`id`, scenery_catalog's example.id of `base`) as a seeded `archetype` (spire: a tall core, a shard out of its top and leaning shoulders; ledge: steps falling downhill; stack: a narrow column for a sea stack; tor: a squat stack of blocks; single: one piece), `height` and `width` across (map units), at `xz`. Each piece is drawn at most `stretch` (default 2) times taller than wide (the mesh's normals light pale when stretched far). The crag is SEATED: the ground is sampled under its footprint (the centre and 8 points round each piece) and each piece on the crag's base lowered so `sink` (default 0.35; a number or [lo, hi] picked per crag) of its height is below the LOWEST sample under it (`conform: false`: the crag as one under the footprint's lowest), so nothing floats on a slope; it is LEANED toward the ground's normal by `align` of the slope (default 0.5; spires stand nearer upright at 0.3), at most `maxTilt` degrees (default 15), plus up to `jitter` degrees (default 4) of seeded wobble. `yaw` (degrees, default: downhill, so a ledge steps down the slope). `dry: true` answers the poses without placing. Copies are tagged _<tag> (default dtl) so remove_scenery {search} finds them. One undo step. Refused for a detail sim_fingerprint counts as sim.",
    inputSchema: { type: "object", additionalProperties: false, required: ["id", "crags"], properties: {
      base: { type: "string", description: "Base key to copy from (default the open project's base)." },
      id: { type: "integer", description: "The scenery detail of the base to kitbash (scenery_catalog's example.id)." },
      crags: { type: "array", minItems: 1, maxItems: 128, items: { type: "object", additionalProperties: false, required: ["xz", "height"], properties: {
        xz: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 },
        height: { type: "number", minimum: 0.2, maximum: 200, description: "The crag's height before sinking." },
        width: { type: "number", minimum: 0.2, maximum: 200, description: "About how wide (default height / 2, a ledge or tor height * 1.4)." },
        archetype: { type: "string", enum: ARCHETYPES },
        seed: { type: "integer" },
        yaw: { type: "number", description: "Degrees; default downhill (the +x of the crag faces down the slope)." },
        sink: { type: "number", minimum: 0, maximum: 0.9 },
        align: { type: "number", minimum: 0, maximum: 1 },
        tint: { type: "string", description: "\"#rrggbb\" for this crag's pieces (overrides the call's tint)." },
      } } },
      tint: { type: "string", description: "\"#rrggbb\": recolour every piece toward this colour (a \"_tint<rrggbb>\" name tag Sunstone's model shaders read; tint_scenery has the details). Visual only." },
      sink: { description: "Fraction of each crag's height below its lowest footprint sample: a number, or [lo, hi] (default 0.35).", anyOf: [{ type: "number", minimum: 0, maximum: 0.9 }, { type: "array", items: { type: "number", minimum: 0, maximum: 0.9 }, minItems: 2, maxItems: 2 }] },
      align: { type: "number", minimum: 0, maximum: 1, description: "How far toward the slope normal (0 upright, 1 square to the ground; default 0.5, spires 0.3)." },
      maxTilt: { type: "number", minimum: 0, maximum: 60, description: "Most lean from upright, degrees (default 15)." },
      jitter: { type: "number", minimum: 0, maximum: 30, description: "Seeded wobble, degrees (default 4)." },
      stretch: { type: "number", minimum: 1, maximum: 2.2, description: "Most height / width per piece (default 2)." },
      step: { type: "number", minimum: 0.1, maximum: 10, description: "Spacing for the ground's normal, map units (default the crag's width / 3)." },
      tag: { type: "string", description: "Letters and digits appended as _<tag> to every piece's name (default dtl)." },
      allowOtherTheme: { type: "boolean" },
      conform: { type: "boolean", description: "Seat each piece standing on the crag's base on its own ground (the lowest of its 8 samples, `sink` of the piece's height under it), so a ledge steps down a slope with the ground; raised pieces move with the first (default true). false: the whole crag is lowered as one, `sink` of its height under the footprint's lowest sample." },
      dry: { type: "boolean", description: "Answer the poses only; place nothing." },
      units: unitsSchema,
    } },
    async handler(s, a: { base?: string; id: number; crags: { xz: number[]; height: number; width?: number; archetype?: Archetype; seed?: number; yaw?: number; sink?: number; align?: number; tint?: string }[];
      sink?: number | number[]; tint?: string; conform?: boolean; align?: number; maxTilt?: number; jitter?: number; stretch?: number; step?: number; tag?: string; allowOtherTheme?: boolean; dry?: boolean; units?: Units }) {
      const { store } = s.requireProject();
      const tag = a.tag ?? "dtl";
      if (!/^[A-Za-z0-9]{1,12}$/.test(tag)) throw new ToolError("tag: 1-12 letters or digits");
      const key = a.base ?? store.scene.base.key;
      const scene = await loadScene(s, key);
      if (scene.databank.theme !== store.scene.databank.theme && !a.allowOtherTheme)
        throw new ToolError(`${key} is ${scene.databank.theme} and this level ${store.scene.databank.theme}: pass allowOtherTheme to try (and Test it)`);
      const src = scene.details.find((d) => d.id === a.id);
      if (!src) throw new ToolError(`no detail ${a.id} in ${key}`);
      const cl = classifyDetail(src);
      if (cl.cls !== "visual" || cl.role !== "scenery") throw new ToolError(`detail ${src.id} (${src.name}) is not a visual scenery piece (${cl.cls === "sim" ? cl.why : cl.role})`);
      const mesh = await meshBounds(s, scene, src.resource);
      if (!mesh) throw new ToolError(`${src.resource} has no preview mesh: its size is unknown, so it cannot be kitbashed`);
      const k = (a.units ?? "world") === "map" ? 1 : 20, top = topOf(store);
      const vox = (id: number) => store.voxelsOf({ id });
      const ground = (x: number, z: number) => dropPoint(store.scene, store.frames, vox, [x, top, z])?.[1] ?? null;
      const frame = targetFrame(store.scene, "scenery");
      const cmds: AddDetail[] = [], crags: object[] = [], skipped: object[] = [];
      a.crags.forEach((c, i) => {
        const x = c.xz[0] / k, z = c.xz[1] / k, H = c.height / k, kind = c.archetype ?? "spire";
        const W = (c.width ?? (kind === "ledge" || kind === "tor" ? c.height * 1.4 : c.height / 2)) / k;
        const R = rng(c.seed ?? (i + 1) * 7919);
        const h = a.step ?? Math.max(0.5, W / 3);
        const y0 = ground(x, z);
        if (y0 === null) { skipped.push({ i, why: "no ground" }); return; }
        const gx = (ground(x + h, z) ?? y0) - (ground(x - h, z) ?? y0), gz = (ground(x, z + h) ?? y0) - (ground(x, z - h) ?? y0);
        const nn = Math.hypot(gx / (2 * h), 1, gz / (2 * h)), normal: V3 = [-gx / (2 * h) / nn, 1 / nn, -gz / (2 * h) / nn];
        const downhill = Math.hypot(normal[0], normal[2]) > 0.02 ? Math.atan2(normal[2], normal[0]) : R() * Math.PI * 2;
        // yaw about y by `deg` turns +x to (cos, 0, -sin): the downhill heading th is yaw -th
        const yaw = c.yaw ?? -downhill * DEG;
        const pieces = kitbash(kind, H, W, c.seed ?? (i + 1) * 7919, a.stretch ?? 2);
        const fp = footprint(pieces, x, z, yaw);
        const ys = fp.pts.map(([px, pz]) => ground(px, pz)).filter((v): v is number => v !== null);
        const low = Math.min(...ys);
        const sinkSpec = c.sink ?? a.sink ?? 0.35;
        const sink = Array.isArray(sinkSpec) ? sinkSpec[0] + R() * (sinkSpec[1] - sinkSpec[0]) : sinkSpec;
        const t = tiltTo(normal, c.align ?? a.align ?? (kind === "spire" || kind === "stack" ? 0.3 : 0.5), a.maxTilt ?? 15, a.jitter ?? 4, R);
        // conform (default): each piece on the crag's base is seated on the lowest of its own 8 samples
        const lows = pieces.map((_, j) => Math.min(...fp.pts.slice(1 + j * 8, 9 + j * 8).map(([px, pz]) => ground(px, pz) ?? Infinity), ...(j === 0 ? [y0] : [])));
        const cr = composeCrag(pieces, mesh, x, z, yaw, t.m, a.conform === false ? low : lows.map((v) => (Number.isFinite(v) ? v : low)), sink);
        for (const p of cr.poses)
          cmds.push(new AddDetail(frame, { name: withTint(`${src.name}_${tag}`, c.tint ?? a.tint ?? null), resource: src.resource, pos: store.frames.toLocal(frame, p.pos), rot: p.rot.map((v) => v / DEG) as Vec3, scale: p.scale }));
        crags.push({ i, archetype: kind, pieces: cr.poses.length, at: [r2(x * k), r2(z * k)], groundLow: r2(low * k), groundSpread: r2((Math.max(...ys) - low) * k), sink: r2(sink),
          tilt: r2(t.tilt), slope: r2(Math.acos(normal[1]) * DEG), bottom: r2(cr.bottom * k), top: r2(cr.top * k), above: r2((cr.top - Math.max(...ys)) * k), radius: r2(cr.radius * k),
          ...(a.dry ? { poses: cr.poses.map((p) => ({ pos: p.pos.map((v) => r2(v * k)), rot: p.rot.map(r2), scale: p.scale.map((v) => Math.round(v * 1000) / 1000) })) } : {}) });
      });
      if (!a.dry && cmds.length) commit(store, new Group(`Place ${crags.length} crag(s)`, cmds));
      return { units: a.units ?? "world", placed: a.dry ? 0 : cmds.length, crags, ...(skipped.length ? { skipped } : {}),
        note: "decoration only: copies have no collision; keep crags (radius) off trails, courts, spawns and pads; sim_fingerprint should be unchanged" };
    },
  },
];
