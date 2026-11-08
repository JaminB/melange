// Tool group "cone": sculpt_cone, an organic cone (a volcano, a hill, a spire) from a radial profile, built with the
// heightfield tools: the outline wanders by angle (per-angle noise on the radius), the flank follows a slope profile, a
// crater can be cut at the top, and by default the surface is draped (heightfield_drape's corner offsets) so the flanks
// are slopes instead of voxel stairs. The cone is an expression of the field (src/lib/cone.ts) handed to the planners.
import { coneExpr, type ConeArgs } from "../lib/cone";
import { applyDrape, planDrape } from "../lib/heightfield-drape";
import { apply, planFit, type FieldArgs } from "../lib/heightfield-plan";
import { usage } from "../lib/voxel";
import { ToolError, type Schema, type Tool } from "../registry";
import { schema as field } from "./heightfield";

const pair = (what: string): Schema => ({ type: "array", items: { type: "number" }, minItems: 2, maxItems: 2, description: what });
const pass = ["units", "voxelSize", "material", "materials", "reuseFrames", "maxBlocks", "maxCells", "partial", "runsHeadroom", "edges", "packer", "baseDepth", "fitBudget", "clip", "exclude", "blend"];

const schema: Schema & { type: "object" } = {
  type: "object", additionalProperties: false, required: ["at", "floor", "radius", "height"],
  properties: {
    ...Object.fromEntries(pass.map((k) => [k, field.properties![k]])),
    at: pair("[x, z] of the cone's axis."),
    floor: { type: "number", description: "The y the cone stands on (its foot)." },
    radius: { type: "number", minimum: 0.5, description: "The foot's radius (before angleNoise)." },
    height: { type: "number", minimum: 0.5, description: "The summit's height above the floor (before the crater and surfaceNoise)." },
    profile: { description: "The flank, height against the distance from the axis as a fraction t of the radius: concave (default; (1 - t)^1.6, steep top, long foot: a volcano), linear ((1 - t), a straight cone), convex (1 - t^2, a dome), gaussian (exp(-3 t^2) less its rim), or points [[t, h], ...] (0 <= t <= 1 rising, h the fraction of height; linear between them, 0 past the last)." },
    angleNoise: { type: "object", additionalProperties: false, required: ["amount"], description: "The outline wanders by angle: the radius times 1 + amount x fbm round the circle (amount 0.25 moves it by up to about a quarter); lobes about how many bulges round it (default 5); seed.",
      properties: { amount: { type: "number", minimum: 0, maximum: 0.9 }, lobes: { type: "number", minimum: 0.5, maximum: 64 }, seed: { type: "integer" } } },
    surfaceNoise: { type: "object", additionalProperties: false, required: ["amount"], description: "Roughness on the flank: amount (height units) x fbm over x, z with features about scale across (default radius / 6), fading to 0 at the summit and the foot.",
      properties: { amount: { type: "number" }, scale: { type: "number", minimum: 0.01 }, seed: { type: "integer" } } },
    gullies: { type: "object", additionalProperties: false, required: ["count", "depth"], description: "Radial gullies down the flank: count of them (at even angles from `angle` degrees), depth (height units at mid-flank), width (radians, default 0.25).",
      properties: { count: { type: "integer", minimum: 1, maximum: 24 }, depth: { type: "number" }, width: { type: "number", minimum: 0.02, maximum: 2 }, angle: { type: "number" } } },
    crater: { type: "object", additionalProperties: false, required: ["radius", "depth"], description: "A crater at the summit: radius, depth below the cone's surface there, rim (a ridge's height at its edge, default 0).",
      properties: { radius: { type: "number", minimum: 0.1 }, depth: { type: "number" }, rim: { type: "number" } } },
    smooth: { type: "boolean", description: "true (default): drape the surface (heightfield_drape: corner offsets, the flanks are slopes in the game, for drawing and collision); false: plain voxel columns (heightfield_region: terraces, see edges)." },
    thickness: { type: "integer", minimum: 1, maximum: 8, description: "smooth: the skin's voxels (default 2)." },
    core: { type: "boolean", description: "smooth: fill under the skin (default true)." },
    skirtTo: { type: "number", description: "smooth: as heightfield_drape's skirtTo (the ground under the skin reaches down to this level y)." },
    skirtTop: { type: "number", description: "smooth: as heightfield_drape's skirtTop." },
    dryRun: { type: "boolean", description: "Plan and report only." },
  },
};

export const tools: Tool[] = [
  {
    name: "sculpt_cone",
    description: "An organic cone (a volcano, a hill, a spire) standing on `floor` at `at`: a flank `profile` (concave, linear, convex, gaussian or [[t, h]] points), an outline that wanders by angle (angleNoise), roughness (surfaceNoise), radial gullies and a summit crater. smooth (default) drapes it so the flanks are real slopes in the game, not voxel stairs; smooth: false builds plain columns. Built by heightfield_drape or heightfield_region, which take its voxelSize, materials, maxBlocks, maxCells and the rest; the answer is theirs plus the expression used (to reuse in heightfield_* with the same centre). One undo step.",
    inputSchema: schema,
    handler(s, a: ConeArgs & FieldArgs & { smooth?: boolean; thickness?: number; core?: boolean; skirtTo?: number; skirtTop?: number; dryRun?: boolean }) {
      const store = s.store;
      const { expr, half } = coneExpr(a);
      const args: FieldArgs = {
        ...Object.fromEntries(pass.filter((k) => (a as unknown as Record<string, unknown>)[k] !== undefined).map((k) => [k, (a as unknown as Record<string, unknown>)[k]])),
        min: [a.at[0] - half, a.at[1] - half], max: [a.at[0] + half, a.at[1] + half], floor: a.floor, centre: a.at, expr,
      };
      const smooth = a.smooth ?? true;
      if (!smooth && (a.thickness !== undefined || a.core !== undefined || a.skirtTo !== undefined || a.skirtTop !== undefined)) throw new ToolError("thickness, core and the skirt are for smooth: true");
      if (smooth && a.fitBudget) throw new ToolError("fitBudget is for smooth: false (a drape packs its skin and core itself)");
      const cone = { expr, rectangle: { min: args.min, max: args.max }, centre: a.at, smooth };
      const u = () => { const x = usage(store); return { newFrames: x.newFrames, voxelCells: x.voxelCells, patchBytes: x.patchBytes, runsPerFrame: x.runsPerFrame }; };
      if (smooth) {
        const p = planDrape(store, { ...args, thickness: a.thickness, core: a.core, skirtTo: a.skirtTo, skirtTop: a.skirtTop });
        if (a.dryRun) return { cone, ...p.report, applied: false };
        const r = applyDrape(store, p);
        return { cone, ...p.report, applied: true, added: r.blocks, limits: u(), undo: "undo takes the whole cone out", next: "save" };
      }
      const p = planFit(store, args);
      if (a.dryRun) return { cone, ...p.report, applied: false };
      const r = apply(store, p);
      return { cone, ...p.report, applied: true, added: r.blocks, limits: u(), undo: "undo takes the whole cone out", next: "save" };
    },
  },
];
