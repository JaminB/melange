// Tool group "propcheck" (detail layer round 8): the round-7 judge's tool gaps.
//   lint_support   a placed prop's ground support (lib/prop-support.ts): props on steep faces with no ground under most of
//                  their footprint and no talus under their downhill edge read as floating
//   surface_material  the top voxel's primary material at many points (where a paint band's cells change: tile edges)
//   tint_variants  per-copy tint variance (lib/tint-jitter.ts): seeded hue/saturation/value jitter of a palette, with an
//                  edge falloff, so a cluster of one piece is not one flat colour
// Both read-only.
import { dropPoint } from "../core";
import { DEG, topOf } from "../lib/place-geom";
import { clumps, type Piece } from "../lib/dress-lint";
import { supportOf, type Ground, type SupportClump } from "../lib/prop-support";
import { tintVariants } from "../lib/tint-jitter";
import { classifyDetail } from "../lib/sim-fingerprint";
import { placedMeshes } from "../lib/scenery-mesh";
import { spansAt } from "../lib/inspect-field";
import { grids } from "../lib/voxel";
import { ToolError, type Tool } from "../registry";
import type { Session } from "../session";

const r2 = (v: number) => Math.round(v * 100) / 100;

function groundOf(s: Session, h = 0.75) {
  const store = s.store, top = topOf(store);
  const vox = (id: number) => store.voxelsOf({ id });
  const at = (x: number, z: number) => dropPoint(store.scene, store.frames, vox, [x, top, z])?.[1] ?? null;
  const cache = new Map<string, Ground | null>();
  return (x: number, z: number): Ground | null => {
    const key = `${x.toFixed(2)},${z.toFixed(2)}`;
    if (cache.has(key)) return cache.get(key)!;
    const y = at(x, z);
    let g: Ground | null = null;
    if (y !== null) {
      const xm = at(x - h, z), xp = at(x + h, z), zm = at(x, z - h), zp = at(x, z + h);
      const dx = ((xp ?? y) - (xm ?? y)) / ((xp !== null ? h : 0) + (xm !== null ? h : 0) || 1);
      const dz = ((zp ?? y) - (zm ?? y)) / ((zp !== null ? h : 0) + (zm !== null ? h : 0) || 1);
      const n = Math.hypot(dx, 1, dz);
      g = { y, slope: Math.atan(Math.hypot(dx, dz)) * DEG, normal: [-dx / n, 1 / n, -dz / n] };
    }
    cache.set(key, g);
    return g;
  };
}

const hexRe = /^#[0-9a-fA-F]{6}$/;

export const tools: Tool[] = [
  {
    name: "lint_support",
    description: "Ground support of a decoration layer's props: the scenery copies whose names contain any of `search` (default [\"_dtl\"]) as drawn, grouped into clumps (pieces within `gap`, default 0.3 map). Each clump's footprint is sampled on a `grid` x `grid` (default 5) and a sample is in contact where the ground comes up to the clump's underside there (within `tol`, default 0.15 map). A clump on ground steeper than `minSlope` (default 55 degrees, the steepest under its footprint) is supported when `minContact` (default 0.6) of its footprint touches the ground, or another clump of the searched layers reaches its underside on its downhill side (talus, a ledge). Answers the unsupported clumps (ids, centre, size, slope, contact share) and counts by verdict (flat, ground, talus, unsupported). Read-only: remove or re-seat what it lists. Map units.",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      search: { type: "array", items: { type: "string" }, minItems: 1, maxItems: 8 },
      holders: { type: "array", items: { type: "string" }, maxItems: 8, description: "More name fragments whose copies may hold a clump up (talus) without being judged themselves." },
      gap: { type: "number", minimum: 0 }, grid: { type: "integer", minimum: 2, maximum: 9 }, tol: { type: "number", minimum: 0 },
      minSlope: { type: "number", minimum: 0, maximum: 90 }, minContact: { type: "number", minimum: 0, maximum: 1 },
      limit: { type: "integer", minimum: 0, maximum: 2000 },
    } },
    async handler(s, a: { search?: string[]; holders?: string[]; gap?: number; grid?: number; tol?: number; minSlope?: number; minContact?: number; limit?: number }) {
      const { store } = s.requireProject();
      const qs = (a.search ?? ["_dtl"]).map((q) => q.toLowerCase()), hs = (a.holders ?? []).map((q) => q.toLowerCase());
      const scen = store.scene.details.filter((d) => d.src === null && classifyDetail(d).role === "scenery");
      const judged = scen.filter((d) => qs.some((q) => d.name.toLowerCase().includes(q)));
      if (!judged.length) throw new ToolError(`no scenery copy's name contains any of ${JSON.stringify(a.search ?? ["_dtl"])}`);
      const extra = scen.filter((d) => !judged.includes(d) && hs.some((q) => d.name.toLowerCase().includes(q)));
      const { meshes, without } = await placedMeshes(s, store.frames, [...judged, ...extra]);
      const judgedIds = new Set(judged.map((d) => d.id));
      const vertsOf = new Map<number, number[]>();
      const toPiece = (m: (typeof meshes)[number]): Piece => {
        const min: [number, number, number] = [Infinity, Infinity, Infinity], max: [number, number, number] = [-Infinity, -Infinity, -Infinity];
        const v: number[] = [];
        for (let i = 0; i < m.tris.length; i += 3) {
          for (let k = 0; k < 3; k++) { min[k] = Math.min(min[k], m.tris[i + k]); max[k] = Math.max(max[k], m.tris[i + k]); }
          v.push(m.tris[i], m.tris[i + 1], m.tris[i + 2]);
        }
        vertsOf.set(m.detail, v);
        return { id: m.detail, box: { min, max }, lum: 0 };
      };
      const pj = meshes.filter((m) => judgedIds.has(m.detail)).map(toPiece), ph = meshes.filter((m) => !judgedIds.has(m.detail)).map(toPiece);
      const asClump = (c: { ids: number[]; box: { min: [number, number, number]; max: [number, number, number] } }): SupportClump =>
        ({ ids: c.ids, min: c.box.min, max: c.box.max, verts: c.ids.flatMap((id) => vertsOf.get(id) ?? []) });
      const cj = clumps(pj, a.gap ?? 0.3).map(asClump);
      const all = cj.concat(clumps(ph, a.gap ?? 0.3).map(asClump));
      const g = groundOf(s);
      const out = cj.map((c) => supportOf(c, g, all, { grid: a.grid, tol: a.tol, minSlope: a.minSlope, minContact: a.minContact }));
      const bad = out.filter((v) => !v.supported);
      const by = (k: string | null) => out.filter((v) => v.by === k).length;
      return { units: "map", pieces: pj.length, clumps: cj.length, noMesh: without.slice(0, 10),
        counts: { flat: by("flat"), ground: by("ground"), talus: by("talus"), unsupported: bad.length },
        unsupported: bad.slice(0, a.limit ?? 200).map((v) => ({ ids: v.ids, at: v.at.map(r2), size: r2(v.size), slope: Math.round(v.slope), contact: r2(v.contact), samples: v.samples })) };
    },
  },
  {
    name: "surface_material",
    description: "The top voxel's primary material at many points ([[x, z], ...], map units, up to 4000): for each, {y, material} (the highest solid stretch's top and its material index in the level's material file, as query_ground answers it) or null where the column is empty. For finding where a painted band's cells change (the edges of grass tiles, so low plants can break them up). Read-only.",
    inputSchema: { type: "object", additionalProperties: false, required: ["points"], properties: {
      points: { type: "array", minItems: 1, maxItems: 4000, items: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 } },
    } },
    handler(s, a: { points: number[][] }) {
      const { store } = s.requireProject();
      const gs = grids(store);
      return { units: "map", samples: a.points.map(([x, z]) => { const sp = spansAt(gs, x, z); return sp.length ? { y: r2(sp[0].top), material: sp[0].material } : null; }) };
    },
  },
  {
    name: "tint_variants",
    description: "Per-copy tint variance for scenery tints: `count` (default 6) seeded variants of each `palette` colour (#rrggbb), each moved in HSV by a random hue shift (+-`hue` degrees, default 8), a saturation scale (+-`sat`, default 0.12) and a value scale (+-`value`, default 0.14); `falloff` 0-1 (e.g. a plant's distance to its cluster's edge) darkens by up to `edgeValue` (default 0.18) and greys by up to `edgeSat` (default 0.2) on top. Deterministic for `seed`. Answers {palette: [{colour, variants}]} to hand to copy_base_details / tint_scenery. Read-only, no project needed.",
    inputSchema: { type: "object", additionalProperties: false, required: ["palette"], properties: {
      palette: { type: "array", minItems: 1, maxItems: 32, items: { type: "string" } }, count: { type: "integer", minimum: 1, maximum: 64 }, seed: { type: "integer" },
      hue: { type: "number", minimum: 0, maximum: 180 }, sat: { type: "number", minimum: 0, maximum: 1 }, value: { type: "number", minimum: 0, maximum: 1 },
      falloff: { type: "number", minimum: 0, maximum: 1 }, edgeValue: { type: "number", minimum: 0, maximum: 1 }, edgeSat: { type: "number", minimum: 0, maximum: 1 },
    } },
    handler(_s, a: { palette: string[]; count?: number; seed?: number; hue?: number; sat?: number; value?: number; falloff?: number; edgeValue?: number; edgeSat?: number }) {
      const bad = a.palette.filter((c) => !hexRe.test(c));
      if (bad.length) throw new ToolError(`palette colours must be #rrggbb: ${bad.join(", ")}`);
      const v = tintVariants(a.palette, a.count ?? 6, a.seed ?? 1, { hue: a.hue, sat: a.sat, value: a.value, falloff: a.falloff, edgeValue: a.edgeValue, edgeSat: a.edgeSat });
      return { palette: a.palette.map((c, i) => ({ colour: c, variants: v[i] })) };
    },
  },
];
