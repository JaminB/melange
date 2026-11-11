// Tool group "mesa": build_mesa, a terraced plateau (a sandstone mesa, a rock outcrop) built with heightfield_region. The
// outline is a circle, an ellipse or a polygon with noise wobble; the cliff is cut into strata steps (a riser of some voxels,
// a tread that is walkable or a sheer wall, a jitter per step); a talus skirt and strata material bands plus a cap are
// optional. The surface is src/lib/mesa.ts; the packing, budget and undo are heightfield_region's.
import { apply, planFit, type FieldArgs } from "../lib/heightfield-plan";
import { mesaField, type MesaArgs } from "../lib/mesa";
import { usage } from "../lib/voxel";
import { ToolError, type Schema, type Tool } from "../registry";
import { schema as field } from "./heightfield";

const pair = (what: string): Schema => ({ type: "array", items: { type: "number" }, minItems: 2, maxItems: 2, description: what });
const material: Schema = { type: "integer", minimum: 0, maximum: 63 };
// heightfield_region's own settings that a mesa takes as they are
const pass = ["voxelSize", "keepFloor", "material", "baseDepth", "reuseFrames", "maxBlocks", "maxCells", "partial", "runsHeadroom", "edges", "packer", "fitBudget", "costAt", "blend", "clip", "exclude"];

const schema: Schema & { type: "object" } = {
  type: "object", additionalProperties: false, required: ["floor", "top", "shape"],
  properties: {
    ...Object.fromEntries(pass.map((k) => [k, field.properties![k]])),
    units: field.properties!.units,
    at: pair("[x, z] of a circle's or ellipse's centre (not for a polygon: its points are level coordinates)."),
    floor: { type: "number", description: "The y the mesa stands on (the desert floor). With keepFloor (default true here) the heights count from it exactly, so `top` lands where it is asked." },
    top: { type: "number", description: "The y of the plateau's top (an absolute level y in `units`, above floor). The answer's topY gives it in map and world units, to plan a height line against (check_height_line)." },
    shape: { type: "object", additionalProperties: false, required: ["kind"], description: "The outline at the cliff's foot (before wobble and talus): circle {radius}, ellipse {radii: [rx, rz], rotation degrees}, or polygon {points: [[x, z], ...]} (3-64 points, level coordinates, any winding, concave is fine).",
      properties: { kind: { type: "string", enum: ["circle", "ellipse", "polygon"] }, radius: { type: "number", minimum: 0.01 },
        radii: pair("[rx, rz]."), rotation: { type: "number" },
        points: { type: "array", minItems: 3, maxItems: 64, items: pair("[x, z].") } } },
    wobble: { type: "object", additionalProperties: false, required: ["amount"], description: "The outline wanders: noise moves it by up to `amount` (units) over features about `scale` across (default 12), seeded. The same noise shifts every step's edge, so the cliff stays one coherent face.",
      properties: { amount: { type: "number", minimum: 0 }, scale: { type: "number", minimum: 0.01 }, seed: { type: "integer" } } },
    strata: { type: "object", additionalProperties: false, description: "The cliff's layers. step: a riser's height in whole voxels (1-16, default 2); the mesa has about (top - floor - talus) / (step x voxelSize) of them. riserJitter: each riser varies by up to this fraction (0-0.5, default 0); they always add up to the cliff. bands: material bands from the floor up, repeating ([{material, thickness}] in units; the first band starts at the floor) and bandJitter {amount, scale, seed} wobbles their edges (heightfield_region's rule jitter), so a layer reads as a layer of sandstone.",
      properties: { step: { type: "integer", minimum: 1, maximum: 16 }, riserJitter: { type: "number", minimum: 0, maximum: 0.5 },
        bands: { type: "array", minItems: 1, maxItems: 32, items: { type: "object", additionalProperties: false, required: ["material", "thickness"], properties: { material, thickness: { type: "number", minimum: 0.01 } } } },
        bandJitter: { type: "object", additionalProperties: false, required: ["amount"], properties: { amount: { type: "number" }, scale: { type: "number", minimum: 0.01 }, seed: { type: "integer" } } } } },
    tread: { type: "object", additionalProperties: false, description: "What the steps' tops are. walkable (default true): every step has a tread of `width` (units; default 4 map units, never under 1.5 map units or 2 voxels: narrower is refused) a worm can stand on and walk along, and the next riser starts at its back: the mesa climbs in ledges (rope anchors, footholds). walkable: false: no tread, a sheer wall (the steps all start at the outline; jitter then only roughens the face by under a voxel); with a `width` it is a setback instead: each step stands that far inside the one under it (any width, a stepped face for a small mesa where a walkable tread of 2 voxels does not fit), its ledges not promised walkable. A small mesa with walkable ledges takes a finer voxelSize (at most half the tread's width). The answer's mesa.ledges lists every exposed top as built (y in map and world units, the tread's width, walkable): check_height_line's inputs. jitter: each step's edge wanders by up to this much (units; default a quarter of the width, never over 0.45 of it), scale its feature size, seed.",
      properties: { walkable: { type: "boolean" }, width: { type: "number", minimum: 0.01 }, jitter: { type: "number", minimum: 0 }, scale: { type: "number", minimum: 0.01 }, seed: { type: "integer" } } },
    talus: { type: "object", additionalProperties: false, description: "A skirt of scree round the foot, off by default: it rises to `height` (units; default two risers) at the cliff and falls to nothing `width` out (default 2.5 x height: a slope of about 22 degrees), `profile` linear (default) or concave; `material` paints it (its columns are those no higher than the skirt). The cliff starts on top of the skirt, so the strata count the cliff only.",
      properties: { height: { type: "number", minimum: 0.01 }, width: { type: "number", minimum: 0.01 }, material, profile: { type: "string", enum: ["linear", "concave"] } } },
    cap: { type: "object", additionalProperties: false, required: ["material"], description: "The plateau's top `depth` voxels (1-8, default 1) in this material (stone, caprock).",
      properties: { material, depth: { type: "integer", minimum: 1, maximum: 8 } } },
    costOnly: { type: "boolean", description: "Plan and report only (blocks, cells, what is left of the budget): nothing is edited. With maxCells it says whether the mesa fits. dryRun is the same." },
    dryRun: { type: "boolean", description: "As costOnly." },
  },
};

export const tools: Tool[] = [
  {
    name: "build_mesa",
    description: "One mesa or rock outcrop: a plateau from an outline (circle, ellipse or polygon, with noise wobble) at `top`, its cliff cut into terraced strata (a riser of `strata.step` voxels, a `tread` that is walkable ledges or a sheer wall, a jitter per step), an optional talus skirt, strata material bands and a cap material. Built by heightfield_region, so it takes its voxelSize, maxBlocks, maxCells, fitBudget, clip, exclude, costAt and the rest, and is one undo step; costOnly plans without editing. The answer is its plan plus `mesa` (steps, risers, tread, plateau, topY in map and world units) and `exclude` / `clip`, circles covering the footprint to pass to a later heightfield_drape or heightfield_region so the dunes leave the mesa alone. Terraces are voxel stairs (a draped skin is not used): a worm needs a jetpack, rope or girder to get from tread to tread unless the riser is small.",
    inputSchema: schema,
    handler(s, a: MesaArgs & FieldArgs & { costOnly?: boolean; dryRun?: boolean }) {
      const store = s.store;
      if ((a as { grid?: unknown }).grid !== undefined || (a as { expr?: unknown }).expr !== undefined) throw new ToolError("build_mesa makes the height itself: no expr or grid");
      const extra = Object.fromEntries(pass.filter((k) => (a as unknown as Record<string, unknown>)[k] !== undefined).map((k) => [k, (a as unknown as Record<string, unknown>)[k]])) as Partial<FieldArgs>;
      const m = mesaField(store, a, extra);
      const p = planFit(store, m.args);
      const out = { mesa: m.info, exclude: m.circles, clip: m.circles };
      if (a.costOnly || a.dryRun) return { ...out, ...p.report, applied: false };
      const r = apply(store, p);
      const u = usage(store);
      return { ...out, ...p.report, applied: true, added: r.blocks, limits: { newFrames: u.newFrames, voxelCells: u.voxelCells, patchBytes: u.patchBytes, runsPerFrame: u.runsPerFrame },
        undo: "undo takes the whole mesa out", next: "save" };
    },
  },
];
