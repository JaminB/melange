// Tool group "mesa": build_mesa, a terraced plateau (a sandstone mesa, a rock outcrop) built with heightfield_region. The
// outline is a circle, an ellipse or a polygon with noise wobble; the cliff is cut into strata steps (a riser of some voxels,
// a tread that is walkable or a sheer wall, a jitter per step); a talus skirt and strata material bands plus a cap are
// optional. The surface is src/lib/mesa.ts; the packing, budget and undo are heightfield_region's.
import { apply, planFit, type FieldArgs } from "../lib/heightfield-plan";
import { mesaField, type MesaArgs } from "../lib/mesa";
import { spansAt } from "../lib/inspect-field";
import { ON_SKIN_PREFIX } from "../lib/structures-apply";
import { grids, usage } from "../lib/voxel";
import { ToolError, type Schema, type Tool } from "../registry";
import { schema as field } from "./heightfield";

const pair = (what: string): Schema => ({ type: "array", items: { type: "number" }, minItems: 2, maxItems: 2, description: what });
const material: Schema = { type: "integer", minimum: 0, maximum: 63 };
// heightfield_region's own settings that a mesa takes as they are
const pass = ["voxelSize", "keepFloor", "floorLattice", "material", "baseDepth", "reuseFrames", "maxBlocks", "maxCells", "partial", "runsHeadroom", "edges", "packer", "fitBudget", "costAt", "blend", "clip", "exclude"];

const schema: Schema & { type: "object" } = {
  type: "object", additionalProperties: false, required: ["top", "shape"],
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
        bandJitter: { type: "object", additionalProperties: false, required: ["amount"], properties: { amount: { type: "number" }, scale: { type: "number", minimum: 0.01 }, seed: { type: "integer" } } },
        style: { type: "string", enum: ["bands", "beds"], description: "bands (default): the bands repeat level and even, as given. beds: sedimentary beds: each band's thickness varies 0.4-1.6 x by `seed` (never under a voxel), every edge wanders (bandJitter, default a third of the mean thickness) and is dithered (`feather`), so a small mesa in coarse voxels does not read as a striped cube." },
        seed: { type: "integer", description: "The beds' seed (style beds)." },
        feather: { type: "number", minimum: 0, description: "Dither each band's edge over this height (units; blue noise). Default one voxel with style beds, else 0." } } },
    gullies: { type: "object", additionalProperties: false, required: ["depth"], description: "Notches weathered into the cliff, off by default: noise along the outline cuts it back by up to `depth` (units) in alcoves about `spacing` apart (default 3 x depth), every step with it, so the plan is lobed (buttresses and alcoves) instead of a disc or a box. Keep depth under the plateau's half width.",
      properties: { depth: { type: "number", minimum: 0 }, spacing: { type: "number", minimum: 0.01 }, seed: { type: "integer" } } },
    onSkin: { description: "The mesa stands on a draped skin on purpose: true, or {sink} (units, default one voxel). Its blocks are named ergonskin_N, so save's and describe_level's drape.pokes advisory leaves them out; and when `floor` is not given it is seated: the floor is the lowest ground under the outline's box (skins at their sheared height) less `sink`, so the foot is under the skin all the way round (the answer's `seat`).",
      anyOf: [{ type: "boolean" }, { type: "object", additionalProperties: false, properties: { sink: { type: "number", minimum: 0 } } }] },
    tread: { type: "object", additionalProperties: false, description: "What the steps' tops are. walkable (default true): every step has a tread of `width` (units; default 4 map units, never under 1.5 map units or 2 voxels: narrower is refused) a worm can stand on and walk along, and the next riser starts at its back: the mesa climbs in ledges (rope anchors, footholds). walkable: false: no tread, a sheer wall (the steps all start at the outline; jitter then only roughens the face by under a voxel); with a `width` it is a setback instead: each step stands that far inside the one under it (any width, a stepped face for a small mesa where a walkable tread of 2 voxels does not fit), its ledges not promised walkable. A small mesa with walkable ledges takes a finer voxelSize (at most half the tread's width). The answer's mesa.ledges lists every exposed top as built (y in map and world units, the tread's width, walkable): check_height_line's inputs. jitter: each step's edge wanders by up to this much (units; default a quarter of the width, never over 0.45 of it), scale its feature size, seed.",
      properties: { walkable: { type: "boolean" }, width: { type: "number", minimum: 0.01 }, jitter: { type: "number", minimum: 0 }, scale: { type: "number", minimum: 0.01 }, seed: { type: "integer" } } },
    talus: { type: "object", additionalProperties: false, description: "A skirt of scree round the foot, off by default: it rises to `height` (units; default two risers) at the cliff and falls to nothing `width` out (default 2.5 x height: a slope of about 22 degrees), `profile` linear (default) or concave; `material` paints it (its columns are those no higher than the skirt). The cliff starts on top of the skirt, so the strata count the cliff only.",
      properties: { height: { type: "number", minimum: 0.01 }, width: { type: "number", minimum: 0.01 }, material, profile: { type: "string", enum: ["linear", "concave"] } } },
    cap: { type: "object", additionalProperties: false, required: ["material"], description: "The plateau's top `depth` voxels (1-8, default 1) in this material (stone, caprock).",
      properties: { material, depth: { type: "integer", minimum: 1, maximum: 8 } } },
    ledgeAt: { type: "array", maxItems: 63, items: { type: "number" }, description: "The steps' tops as absolute level ys (units), lowest first, in place of even risers (strata.step then only sets the default tread): one step per entry, then the plateau. Each is built on the nearest whole voxel (the answer's ledges and ledgeAt.built say where); with floorLattice those are floor + whole voxels, so a ledge asked a whole number of voxels over the floor is built exactly there." },
    avoidBand: { type: "object", additionalProperties: false, required: ["y", "band"], description: "A hazard band the ledges stay out of (Sandtide's sand line and check_height_line's band): no step's top is built within `band` of the level y `y` (units). A step that would be moves to the nearest voxel level outside the band that keeps a riser of a voxel to its neighbours, or is left out; the answer's avoidBand.moved lists each. The plateau's top is as asked (a warning when it is inside).",
      properties: { y: { type: "number" }, band: { type: "number", minimum: 0 } } },
    costOnly: { type: "boolean", description: "Plan and report only (blocks, cells, what is left of the budget): nothing is edited. With maxCells it says whether the mesa fits. dryRun is the same." },
    dryRun: { type: "boolean", description: "As costOnly." },
  },
};

export const tools: Tool[] = [
  {
    name: "build_mesa",
    description: "One mesa or rock outcrop: a plateau from an outline (circle, ellipse or polygon, with noise wobble) at `top`, its cliff cut into terraced strata (a riser of `strata.step` voxels, a `tread` that is walkable ledges or a sheer wall, a jitter per step), an optional talus skirt, strata material bands and a cap material. Built by heightfield_region, so it takes its voxelSize, maxBlocks, maxCells, fitBudget, clip, exclude, costAt and the rest, and is one undo step; costOnly plans without editing. The answer is its plan plus `mesa` (steps, risers, tread, plateau, topY in map and world units) and `exclude` / `clip`, circles covering the footprint to pass to a later heightfield_drape or heightfield_region so the dunes leave the mesa alone. Terraces are voxel stairs (a draped skin is not used): a worm needs a jetpack, rope or girder to get from tread to tread unless the riser is small.",
    inputSchema: schema,
    handler(s, a0: Omit<MesaArgs, "floor"> & FieldArgs & { floor?: number; costOnly?: boolean; dryRun?: boolean; onSkin?: boolean | { sink?: number } }) {
      const store = s.store;
      // onSkin with no floor: seat the mesa on the ground under its outline's box, a little under the lowest point
      let seat: Record<string, unknown> | undefined;
      if (a0.floor === undefined) {
        if (!a0.onSkin) throw new ToolError("floor: the y the mesa stands on (or onSkin: true to seat it on the ground under it)");
        const k = a0.units === "world" ? 1 / 20 : 1, sh = a0.shape, vs = a0.voxelSize ?? 1;
        const pts = sh?.kind === "polygon" ? sh.points : null;
        const r = sh?.kind === "circle" ? [sh.radius, sh.radius] : sh?.kind === "ellipse" ? [Math.max(...sh.radii), Math.max(...sh.radii)] : [0, 0];
        if (!pts && !(Array.isArray(a0.at) && a0.at.length === 2)) throw new ToolError("at: [x, z], the centre of a circle or ellipse");
        const x0 = (pts ? Math.min(...pts.map((p) => p[0])) : a0.at![0] - r[0]) * k, x1 = (pts ? Math.max(...pts.map((p) => p[0])) : a0.at![0] + r[0]) * k;
        const z0 = (pts ? Math.min(...pts.map((p) => p[1])) : a0.at![1] - r[1]) * k, z1 = (pts ? Math.max(...pts.map((p) => p[1])) : a0.at![1] + r[1]) * k;
        const gs = grids(store), step = Math.max(0.5, Math.min(vs, (x1 - x0) / 8, (z1 - z0) / 8));
        let lo = Infinity, hi = -Infinity, n = 0;
        for (let z = z0; z <= z1 + 1e-6; z += step) for (let x = x0; x <= x1 + 1e-6; x += step) {
          const top = spansAt(gs, x, z)[0];
          if (!top) continue;
          lo = Math.min(lo, top.top); hi = Math.max(hi, top.top); n++;
        }
        if (!n) throw new ToolError("onSkin: no ground under the mesa's outline (give floor)");
        const sink = typeof a0.onSkin === "object" && a0.onSkin.sink !== undefined ? a0.onSkin.sink * k : vs;
        const r3 = (v: number) => Math.round((v / k) * 1000) / 1000;
        a0 = { ...a0, floor: (lo - sink) / k };
        seat = { floor: r3(lo - sink), ground: { min: r3(lo), max: r3(hi), samples: n }, sink: r3(sink), note: "the floor is the lowest ground under the outline's box less sink: the foot is under the skin all the way round" };
      }
      const a = a0 as MesaArgs & FieldArgs & { costOnly?: boolean; dryRun?: boolean; onSkin?: boolean | { sink?: number } };
      if ((a as { grid?: unknown }).grid !== undefined || (a as { expr?: unknown }).expr !== undefined) throw new ToolError("build_mesa makes the height itself: no expr or grid");
      const extra = Object.fromEntries(pass.filter((k) => (a as unknown as Record<string, unknown>)[k] !== undefined).map((k) => [k, (a as unknown as Record<string, unknown>)[k]])) as Partial<FieldArgs>;
      const m = mesaField(store, a, extra);
      const p = planFit(store, m.args);
      const out = { mesa: m.info, ...(seat ? { seat } : {}), exclude: m.circles, clip: m.circles };
      if (a.costOnly || a.dryRun) return { ...out, ...p.report, applied: false };
      const r = apply(store, p, undefined, a.onSkin ? ON_SKIN_PREFIX : undefined);
      const u = usage(store);
      return { ...out, ...p.report, applied: true, added: r.blocks, limits: { newFrames: u.newFrames, voxelCells: u.voxelCells, patchBytes: u.patchBytes, runsPerFrame: u.runsPerFrame },
        undo: "undo takes the whole mesa out", next: "save" };
    },
  },
];
