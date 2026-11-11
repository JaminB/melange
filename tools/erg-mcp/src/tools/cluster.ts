// Tool group "cluster" (detail layer round 5): plan clusters with a size gradient round anchors (lib/cluster.ts) and lint
// a decoration layer (lib/dress-lint.ts): small dark lumps that read as mines, pieces hovering over or buried in the
// ground, and how dense each band of the island is dressed. Read-only: plan_clusters answers crag specs for place_crags.
import { dropPoint } from "../core";
import { DEG, topOf, unitsSchema, type Units } from "../lib/place-geom";
import { planClusters, type Anchor, type ClusterMode, type Ground } from "../lib/cluster";
import { clumps, contact, mineSuspects, type Piece } from "../lib/dress-lint";
import { classifyDetail } from "../lib/sim-fingerprint";
import { placedMeshes, luminance } from "../lib/scenery-mesh";
import { ToolError, type Tool } from "../registry";
import type { Session } from "../session";

const r2 = (v: number) => Math.round(v * 100) / 100;

/** The voxels' ground (map units) at x, z: top, slope and normal from the top `h` either side (surface_sample's maths). */
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

export const tools: Tool[] = [
  {
    name: "plan_clusters",
    description: "Plan rock clusters with a size gradient round `anchors` ([{xz, size, radius?}], map units or the call's units), on the real ground: mode \"outcrop\" sheds 2-4 smaller blocks on each anchor's DOWNHILL side (sizes falling from 55% to 30% of the anchor's with distance), \"scree\" a fan of 4-8 flat slabs (sizes [0.8, 1.6] absolute, the toe's the biggest) down the slope past the anchor's foot, \"skirt\" 4-7 debris pieces (12-22% of the anchor) round its contact line, more of them downhill, so a crag reads seated. Override count [min, max], sizes, spread, cone (degrees round downhill), flat (width over height), minSize, seed. Answers crag specs {xz, height, width, archetype, align, sink, parent, ring} for place_crags (filter them against your keep-outs first). Read-only.",
    inputSchema: { type: "object", additionalProperties: false, required: ["anchors", "mode"], properties: {
      anchors: { type: "array", minItems: 1, maxItems: 500, items: { type: "object", additionalProperties: false, required: ["xz", "size"], properties: {
        xz: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 }, size: { type: "number", minimum: 0.001 }, radius: { type: "number", minimum: 0.001 } } } },
      mode: { type: "string", enum: ["outcrop", "scree", "skirt"] },
      count: { type: "array", items: { type: "integer", minimum: 0, maximum: 16 }, minItems: 2, maxItems: 2 },
      sizes: { type: "array", items: { type: "number", minimum: 0.001 }, minItems: 2, maxItems: 2 },
      spread: { type: "number", minimum: 0.001 }, cone: { type: "number", minimum: 0, maximum: 180 }, flat: { type: "number", minimum: 0.001 },
      minSize: { type: "number", minimum: 0.001 }, seed: { type: "integer" },
      units: unitsSchema,
    } },
    handler(s, a: { anchors: { xz: number[]; size: number; radius?: number }[]; mode: ClusterMode; count?: [number, number]; sizes?: [number, number]; spread?: number; cone?: number; flat?: number; minSize?: number; seed?: number; units?: Units }) {
      s.requireProject();
      const u = a.units ?? "world", k = u === "map" ? 1 : 20;
      const g = groundOf(s);
      const anchors: Anchor[] = a.anchors.map((an) => ({ xz: [an.xz[0] / k, an.xz[1] / k], size: an.size / k, ...(an.radius ? { radius: an.radius / k } : {}) }));
      const sizes = a.sizes && a.mode === "scree" ? (a.sizes.map((v) => v / k) as [number, number]) : a.sizes;
      const kids = planClusters(anchors, g, { mode: a.mode, count: a.count, sizes, spread: a.spread, cone: a.cone, flat: a.flat, minSize: a.minSize !== undefined ? a.minSize / k : undefined, seed: a.seed });
      return { units: u, mode: a.mode, children: kids.map((c) => ({ ...c, xz: [r2(c.xz[0] * k), r2(c.xz[1] * k)], height: r2(c.height * k), width: r2(c.width * k), ring: r2(c.ring) })) };
    },
  },
  {
    name: "lint_dressing",
    description: "Lint a decoration layer: the scenery copies whose names contain `search` (default \"_dtl\") as drawn (their preview meshes through their matrices, in their tint), grouped into clumps (pieces within `gap`, default 0.4 map, of each other). Answers (1) `mineSuspects`: clumps under `minSize` across (default 1.5 map) darker than `darkLum` (linear luminance, default 0.12: a mid grey #606060) on ground under `maxSlope` (default 40 degrees) above `water` (map): a small dark lump a player reads as a mine; (2) `contact`: clumps hovering (their lowest point over `hover`, default 0.15, above the ground under their centre) or buried (their top under the lowest ground of their footprint); (3) `density` per band of ground height (`bands` [{name, minY, maxY}] map units; default four quartiles of the dressed ground): land area sampled on a `step` grid (default 3 map), clumps and pieces per 1000 square map units and the covered share. Read-only.",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      search: { type: "string" }, gap: { type: "number", minimum: 0 }, minSize: { type: "number", minimum: 0.001 }, darkLum: { type: "number", minimum: 0, maximum: 1 },
      maxSlope: { type: "number", minimum: 0, maximum: 90 }, water: { type: "number" }, hover: { type: "number", minimum: 0 }, step: { type: "number", minimum: 0.5, maximum: 20 },
      bands: { type: "array", maxItems: 12, items: { type: "object", additionalProperties: false, required: ["name"], properties: { name: { type: "string" }, minY: { type: "number" }, maxY: { type: "number" } } } },
      limit: { type: "integer", minimum: 0, maximum: 500 },
    } },
    async handler(s, a: { search?: string; gap?: number; minSize?: number; darkLum?: number; maxSlope?: number; water?: number; hover?: number; step?: number; bands?: { name: string; minY?: number; maxY?: number }[]; limit?: number }) {
      const { store } = s.requireProject();
      const q = (a.search ?? "_dtl").toLowerCase(), limit = a.limit ?? 40;
      const details = store.scene.details.filter((d) => d.src === null && classifyDetail(d).role === "scenery" && d.name.toLowerCase().includes(q));
      if (!details.length) throw new ToolError(`no scenery copy's name contains ${JSON.stringify(a.search ?? "_dtl")}`);
      const { meshes, without } = await placedMeshes(s, store.frames, details);
      const pieces: Piece[] = meshes.map((m) => {
        const min: [number, number, number] = [Infinity, Infinity, Infinity], max: [number, number, number] = [-Infinity, -Infinity, -Infinity];
        for (let i = 0; i < m.tris.length; i += 3) for (let k = 0; k < 3; k++) { min[k] = Math.min(min[k], m.tris[i + k]); max[k] = Math.max(max[k], m.tris[i + k]); }
        return { id: m.detail, box: { min, max }, lum: luminance(m.rgb) };
      });
      const g = groundOf(s);
      const cs = clumps(pieces, a.gap ?? 0.4);
      const mines = mineSuspects(cs, g, { minSize: a.minSize, darkLum: a.darkLum, maxSlope: a.maxSlope, water: a.water });
      const cont = cs.map((c) => ({ c, r: contact(c, g, a.hover ?? 0.15) }));
      const bad = cont.filter((x) => x.r.state === "hovering" || x.r.state === "buried");
      // density by band of ground height
      const ext = { x0: Infinity, x1: -Infinity, z0: Infinity, z1: -Infinity };
      for (const p of pieces) { ext.x0 = Math.min(ext.x0, p.box.min[0]); ext.x1 = Math.max(ext.x1, p.box.max[0]); ext.z0 = Math.min(ext.z0, p.box.min[2]); ext.z1 = Math.max(ext.z1, p.box.max[2]); }
      const step = a.step ?? 3, land: number[] = [];
      for (let x = ext.x0; x <= ext.x1; x += step) for (let z = ext.z0; z <= ext.z1; z += step) { const y = g(x, z)?.y; if (y !== undefined && (a.water === undefined || y > a.water)) land.push(y); }
      const ys = cs.map((c) => g(c.centre[0], c.centre[1])?.y ?? NaN);
      let bands = a.bands;
      if (!bands) {
        const sorted = [...land].sort((p, r) => p - r), qn = (f: number) => sorted[Math.floor(f * (sorted.length - 1))];
        bands = [0, 1, 2, 3].map((i) => ({ name: `q${i + 1}`, minY: i ? qn(i / 4) : -Infinity, maxY: i < 3 ? qn((i + 1) / 4) : Infinity }));
      }
      const density = bands.map((b) => {
        const inB = (y: number) => y >= (b.minY ?? -Infinity) && y < (b.maxY ?? Infinity);
        const area = land.filter(inB).length * step * step;
        const mine = cs.filter((_, i) => inB(ys[i]));
        const covered = mine.reduce((n, c) => n + (c.box.max[0] - c.box.min[0]) * (c.box.max[2] - c.box.min[2]) * 0.6, 0);
        const per = (n: number) => (area > 0 ? r2((n * 1000) / area) : null);
        return { name: b.name, minY: Number.isFinite(b.minY ?? NaN) ? r2(b.minY!) : null, maxY: Number.isFinite(b.maxY ?? NaN) ? r2(b.maxY!) : null, landArea: r2(area),
          clumps: mine.length, pieces: mine.reduce((n, c) => n + c.ids.length, 0), clumpsPer1000: per(mine.length), piecesPer1000: per(mine.reduce((n, c) => n + c.ids.length, 0)), coveredShare: area > 0 ? r2(covered / area) : null,
          sizes: { under1_5: mine.filter((c) => c.size < 1.5).length, "1.5-6": mine.filter((c) => c.size >= 1.5 && c.size < 6).length, "6+": mine.filter((c) => c.size >= 6).length } };
      });
      const show = (c: (typeof cs)[number]) => ({ ids: c.ids.slice(0, 8), at: [r2(c.centre[0]), r2(c.box.min[1]), r2(c.centre[1])], size: r2(c.size), lum: r2(c.lum) });
      return { units: "map", pieces: pieces.length, clumps: cs.length, noMesh: without.slice(0, 10),
        mineSuspects: { count: mines.length, list: mines.slice(0, limit).map(show) },
        contact: { hovering: bad.filter((x) => x.r.state === "hovering").length, buried: bad.filter((x) => x.r.state === "buried").length, list: bad.slice(0, limit).map((x) => ({ ...show(x.c), state: x.r.state, gap: r2(x.r.gap) })) },
        density };
    },
  },
];
