// Tool group "boulders": build_boulder_field, a seeded scatter of low voxel boulders that does not stack into stairs, and
// lint_scatter_stairs, which reads the scatter a level has (lib/boulders.ts). The boulders are build_compound domes in
// one block, bedded into the ground they stand on.
import type { Vec3 } from "../core";
import { lintStairs, planField, type FieldSpec, type ScatterBlock } from "../lib/boulders";
import { spansAt } from "../lib/inspect-field";
import { ON_SKIN_PREFIX, SCATTER_PREFIX } from "../lib/structures-apply";
import { WORLD_PER_MAP, grids, isSolid, type Grid } from "../lib/voxel";
import { ToolError, type Schema, type Tool } from "../registry";
import type { Session } from "../session";
import { tools as structureTools } from "./structures";

const r2 = (v: number) => Math.round(v * 100) / 100;
const pair = (what: string): Schema => ({ type: "array", items: { type: "number" }, minItems: 2, maxItems: 2, description: what });
const groundOf = (gs: Grid[]) => (x: number, z: number) => spansAt(gs, x, z)[0]?.top ?? null;

interface FieldArgs { at: [number, number]; radius: number; count?: number; seed?: number; size?: [number, number]; height?: [number, number]; maxTiers?: number; scale?: number; jitter?: number;
  gap?: number; keepOut?: number; riserMin?: number; materials?: number[]; material?: number; avoid?: { x: number; z: number; radius: number }[]; sink?: number; units?: "map" | "world";
  maxCells?: number; dryRun?: boolean; }

export const tools: Tool[] = [
  {
    name: "build_boulder_field",
    description: "A field of low voxel boulders bedded into the ground, in one block (build_compound domes, named ergonskin_N). Seeded: `count` boulders in the disc `radius` round `at`, each a dome of a radius in `size` and a height in `height`, in one of `materials`. What keeps it from reading as steps: maxTiers (default 2) is the most voxel layers a boulder shows over the ground it stands on (a dome of three or four layers is a stack of discs: a staircase); gap (default 0.5) is the clear ground kept between two boulders, so none stands on another (a negative gap lets them overlap by that much); keepOut (default 6 map units) keeps every boulder's edge that far from a riser (ground that rises riserMin, default 2.5 map units, between samples one unit apart: a wall, a plinth, a mesa's face), so no boulder is a step up one; avoid circles keep them off pads and paths. jitter (0-1, default 0.6) moves each from an even sunflower pattern toward a random spot. The answer lists the boulders (place detail meshes round them with place_mod_mesh), what was turned down and why, and build_compound's result; dryRun plans only. lint_scatter_stairs checks the result.",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["at", "radius"],
      properties: {
        at: pair("[x, z] of the field's centre."), radius: { type: "number", minimum: 0.5, description: "The field's radius." },
        count: { type: "integer", minimum: 1, maximum: 64, description: "Boulders wanted (default 7); fewer are built when no more fit." },
        seed: { type: "integer" },
        size: pair("A boulder's radius, [least, most] (default [1.5, 3] map units)."),
        height: pair("Its height over the ground, [least, most] (default [1, 2] map units), built as whole voxel layers, at most maxTiers."),
        maxTiers: { type: "integer", minimum: 1, maximum: 8, description: "The most voxel layers a boulder shows over the ground (default 2)." },
        scale: { type: "number", minimum: 0.5, maximum: 4, description: "Map units per voxel of the block (default 1)." },
        jitter: { type: "number", minimum: 0, maximum: 1 },
        gap: { type: "number", description: "Clear ground between two boulders (default 0.5 map units; negative: they may overlap by that much)." },
        keepOut: { type: "number", minimum: 0, description: "No boulder's edge within this of a riser (default 6 map units; 0: anywhere)." },
        riserMin: { type: "number", minimum: 0.5, description: "A rise of this much between ground samples one map unit apart is a riser (default 2.5 map units: more than a jump clears)." },
        materials: { type: "array", minItems: 1, maxItems: 8, items: { type: "integer", minimum: 0, maximum: 63 }, description: "The boulders' materials, one picked per boulder (default: the call's material)." },
        material: { type: "integer", minimum: 0, maximum: 63 },
        avoid: { type: "array", maxItems: 64, items: { type: "object", additionalProperties: false, required: ["x", "z", "radius"], properties: { x: { type: "number" }, z: { type: "number" }, radius: { type: "number", minimum: 0 } } }, description: "Circles no boulder touches (pads, paths, a gate's mouth)." },
        sink: { type: "number", minimum: 0, description: "How far each boulder is bedded under its ground (default 0.8 map units)." },
        units: { type: "string", enum: ["map", "world"] },
        maxCells: { type: "integer", minimum: 1, maximum: 65535 },
        dryRun: { type: "boolean" },
      },
    },
    handler(s: Session, a: FieldArgs) {
      const store = s.requireProject().store, k = a.units === "world" ? 1 / WORLD_PER_MAP : 1, U = (v: number | undefined, d: number) => (v === undefined ? d : v * k);
      const mats = a.materials ?? (a.material !== undefined ? [a.material] : null);
      if (!mats) throw new ToolError("materials (or material): the boulders' rock");
      const size = (a.size ?? [1.5 / k, 3 / k]).map((v) => v * k) as [number, number], height = (a.height ?? [1 / k, 2 / k]).map((v) => v * k) as [number, number];
      if (!(size[0] > 0 && size[1] >= size[0])) throw new ToolError("size: [least, most], above 0");
      if (!(height[0] > 0 && height[1] >= height[0])) throw new ToolError("height: [least, most], above 0");
      const voxel = a.scale ?? 1;
      const spec: FieldSpec = { at: [a.at[0] * k, a.at[1] * k], radius: a.radius * k, count: a.count ?? 7, seed: a.seed ?? 1, size, height, maxTiers: a.maxTiers ?? 2, voxel, jitter: a.jitter ?? 0.6,
        gap: U(a.gap, 0.5), keepOut: U(a.keepOut, 6), riserMin: U(a.riserMin, 2.5), materials: mats, avoid: (a.avoid ?? []).map((c) => ({ x: c.x * k, z: c.z * k, radius: c.radius * k })) };
      const gs = grids(store).filter((g) => g.words.some(isSolid));
      const plan = planField(spec, groundOf(gs));
      if (!plan.boulders.length) throw new ToolError(`no boulder fits: of ${plan.tries} spots ${plan.rejected.noGround} had no ground, ${plan.rejected.riser} were within keepOut of a riser, ${plan.rejected.crowded} too near another, ${plan.rejected.avoid} in an avoid circle`);
      const sink = U(a.sink, 0.8), out = (v: number) => r2(v / k);
      // each dome's base is `sink` under its ground and its top a whole number of voxel layers over it
      const shapes = plan.boulders.map((b) => ({ kind: "dome", radius: r2(b.radius), height: r2(b.height + sink), material: b.material,
        at: [r2(b.x - spec.at[0]), r2(b.ground - sink), r2(b.z - spec.at[1])] as Vec3 }));
      const built = structureTools.find((t) => t.name === "build_compound")!.handler(s, { at: [spec.at[0], 0, spec.at[1]], shapes, scale: voxel, place: "new", packing: "fit", maxBlocks: 1, onSkin: true, scatter: true,
        ...(a.maxCells !== undefined ? { maxCells: a.maxCells } : {}), ...(a.dryRun ? { dryRun: true } : {}) } as never) as Record<string, unknown>;
      return {
        boulders: plan.boulders.map((b) => ({ at: [out(b.x), out(b.ground), out(b.z)], radius: out(b.radius), height: out(b.height), tiers: b.tiers, material: b.material })),
        built: plan.boulders.length, asked: spec.count, ...(plan.boulders.length < spec.count ? { rejected: plan.rejected, note: `${spec.count - plan.boulders.length} did not fit in ${plan.tries} tries (a larger radius, a smaller gap or keepOut, or fewer)` } : {}),
        rules: { maxTiers: spec.maxTiers, gap: out(spec.gap), keepOut: out(spec.keepOut), riserMin: out(spec.riserMin), sink: out(sink), voxel },
        compound: { dryRun: built.dryRun, voxels: built.voxels, newBlocks: built.newBlocks, cells: built.cells, notes: built.notes },
        next: "lint_scatter_stairs checks every scatter block for stacks and for footholds beside a riser",
      };
    },
  },
  {
    name: "lint_scatter_stairs",
    description: "Scatter that makes stairs. Reads every scatter block (build_boulder_field's ergonskin_scatter_N blocks, and the other ergonskin_N blocks that structures built onSkin when they are at most maxFootprint across, so a mesa is not one; or `frames`) against the ground of the other frames: `tiers` (the distinct voxel levels its columns stand at over that ground), `lift` (its tallest), the nearest riser within `near` of it (ground that rises riserMin between samples one map unit apart) and `left`, what the block leaves of that riser. It flags `stack` (more tiers than maxTiers: a pile of discs that reads as a staircase) and `foothold` (a riser within `near` whose top the block's top brings within `jump`, where the ground under the block does not: a step up a wall that was meant to need a tool). ok when nothing is flagged. Nothing is edited.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        frames: { type: "array", maxItems: 64, items: { type: "integer" }, description: "The blocks to read, by frame id (default: every ergonskin_N block within maxFootprint)." },
        near: { type: "number", minimum: 0, description: "How far from a block a riser counts (default 6 map units)." },
        riserMin: { type: "number", minimum: 0.5, description: "Default 2.5 map units." },
        jump: { type: "number", minimum: 0, description: "The rise a jump or backflip clears (default 2.5 map units, measured in the game)." },
        maxTiers: { type: "integer", minimum: 1, maximum: 16, description: "Default 2." },
        maxFootprint: { type: "number", minimum: 1, description: "An on-skin block wider than this is not scatter unless a boulder field built it (default 14 map units)." },
        units: { type: "string", enum: ["map", "world"] },
      },
    },
    handler(s: Session, a: { frames?: number[]; near?: number; riserMin?: number; jump?: number; maxTiers?: number; maxFootprint?: number; units?: "map" | "world" }) {
      const store = s.requireProject().store, k = a.units === "world" ? 1 / WORLD_PER_MAP : 1, U = (v: number | undefined, d: number) => (v === undefined ? d : v * k), out = (v: number) => r2(v / k);
      const all = grids(store).filter((g) => g.words.some(isSolid)), foot = U(a.maxFootprint, 14);
      const picked = a.frames ? all.filter((g) => a.frames!.includes(g.id))
        : all.filter((g) => !g.frame.corners && (g.frame.name.startsWith(SCATTER_PREFIX) ||
          (g.frame.name.startsWith(ON_SKIN_PREFIX) && g.bounds.max[0] - g.bounds.min[0] <= foot && g.bounds.max[2] - g.bounds.min[2] <= foot)));
      if (a.frames && picked.length !== a.frames.length) throw new ToolError(`no voxel frame ${a.frames.filter((id) => !picked.some((g) => g.id === id)).join(", ")}`);
      const ids = new Set(picked.map((g) => g.id)), rest = all.filter((g) => !ids.has(g.id)), ground = groundOf(rest);
      const blocks: ScatterBlock[] = picked.map((g) => {
        const [X, Y, Z] = g.size, m = g.toMap, cols: ScatterBlock["columns"] = [];
        for (let z = 0; z < Z; z++) for (let x = 0; x < X; x++) {
          let top = -1;
          for (let y = Y - 1; y >= 0; y--) if (isSolid(g.words[(z * X + x) * Y + y])) { top = y; break; }
          if (top < 0) continue;
          const px = m[0] * (x + 0.5) + m[1] * (top + 1) + m[2] * (z + 0.5) + m[3], py = m[4] * (x + 0.5) + m[5] * (top + 1) + m[6] * (z + 0.5) + m[7], pz = m[8] * (x + 0.5) + m[9] * (top + 1) + m[10] * (z + 0.5) + m[11];
          cols.push({ x: px, z: pz, top: py, ground: ground(px, pz), i: x, j: z });
        }
        return { id: g.id, name: g.frame.name, columns: cols, voxel: Math.hypot(m[1], m[5], m[9]) };
      });
      const finds = lintStairs(blocks, ground, { near: U(a.near, 6), riserMin: U(a.riserMin, 2.5), jump: U(a.jump, 2.5), maxTiers: a.maxTiers ?? 2 });
      const row = (f: (typeof finds)[number]) => ({ id: f.id, name: f.name, at: f.at.map(out), tiers: f.tiers, lift: out(f.lift), topY: out(f.top),
        ...(f.riser ? { riser: { distance: out(f.riser.distance), topY: out(f.riser.top), at: f.riser.at.map(out), left: out(f.riser.left) } } : {}),
        ...(f.stack ? { stack: true } : {}), ...(f.foothold ? { foothold: true } : {}) });
      const bad = finds.filter((f) => f.stack || f.foothold);
      return {
        ok: bad.length === 0,
        summary: !finds.length ? "no scatter block to read" : bad.length ? `${bad.filter((f) => f.stack).length} stacked, ${bad.filter((f) => f.foothold).length} a foothold beside a riser, of ${finds.length} scatter blocks` : `${finds.length} scatter blocks: none stacked, none a foothold beside a riser`,
        flagged: bad.map(row), blocks: finds.length, ...(finds.length <= 40 ? { all: finds.map(row) } : {}),
        rules: { near: out(U(a.near, 6)), riserMin: out(U(a.riserMin, 2.5)), jump: out(U(a.jump, 2.5)), maxTiers: a.maxTiers ?? 2, maxFootprint: out(foot) },
        ...(bad.length ? { fix: "build_boulder_field keeps to maxTiers and keepOut; a stacked block: fewer voxel layers or boulders apart (gap); a foothold: move it out of `near` of the riser" } : {}),
      };
    },
  },
];
