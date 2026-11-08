// Tool group "drape": a heightfield as a smooth skin. Blocks of whole columns a few voxels thick whose corner offsets
// (each block's HeightMap, which the game uses to move its columns up, drawn and for collision) carry the surface's
// height, over the usual stepped field cut just under it. The planner is src/lib/heightfield-drape.ts.
import { applyDrape, planDrape, type DrapeArgs } from "../lib/heightfield-drape";
import { usage } from "../lib/voxel";
import type { Schema, Tool } from "../registry";
import type { Session } from "../session";
import { schema as field } from "./heightfield";

// fitBudget is heightfield_region's: a drape packs its skin and core itself
const { fitBudget: _fit, ...fieldProps } = field.properties!;

const schema: Schema & { type: "object" } = {
  ...field,
  properties: {
    ...fieldProps,
    thickness: { type: "integer", minimum: 1, maximum: 8, description: "The skin's voxels (default 2). The core under it is cut to its lowest corner less a quarter voxel, so where one column's corners differ by more than thickness - 1 the skin can leave air under it (not seen until dug)." },
    core: { type: "boolean", description: "Fill under the skin with the stepped field (default true; packed as heightfield_region packs, with maxBlocks/maxCells less the skin's). false: a hollow skin, about a fifth of the cells of a solid island, that weapons dig through into air." },
    skirtTo: { type: "number", description: "Level y (units) the ground under the skin reaches down to: the core is filled from here up to just under the skin, instead of from the floor, and a hollow skin (core: false) gets one too, so a rising sea shows solid ground instead of the skin's underside and its jagged edge. The sea floor, or at least 10 map units under the water's cap. dryRun answers its cells (core.cells, core.neededCells)." },
    skirtTop: { type: "number", description: "With skirtTo: fill only under the columns whose skin dips to this level y (units) or lower (the skin's lowest corner less its thickness): the shore that the sea reaches, at a fraction of the cells. Default every column." },
    steepMaterial: { type: "object", additionalProperties: false, required: ["material"], description: "Cliffs in rock: a skin column whose slope (from its four corners) is at least minSlope takes `material` in every skin voxel, whatever the materials rules give it. A sheared skin voxel's texture is stretched up its face, so a top material on a steep face shows vertical streaks; a rock material reads as a cliff. feather (degrees) dithers the edge over minSlope +- feather / 2.",
      properties: { material: { type: "integer", minimum: 0, maximum: 63 }, minSlope: { type: "number", minimum: 1, maximum: 89, description: "Degrees (default 50)." }, feather: { type: "number", minimum: 0, maximum: 40, description: "Degrees of dithered edge (default 0)." } } },
    dryRun: { type: "boolean", description: "Plan and report only." },
    estimateSkirts: { type: "boolean", description: "With dryRun: also plan the same skin as a hollow skin, with its full core, and (when skirtTo is given) with the skirt alone and with skirtTop, and answer each one's blocks and cells (skirtEstimates), to choose before building." },
  },
};

/** The same drape planned in each skirt mode: its blocks, skin and core cells, whether it fits (dryRun estimateSkirts). */
function skirtEstimates(s: Session, a: DrapeArgs) {
  const { skirtTo, skirtTop, core: _core, ...base } = a;
  const modes: [string, DrapeArgs][] = [["hollow (core: false)", { ...base, core: false }], ["core (from the floor)", { ...base, core: true }]];
  if (skirtTo !== undefined) {
    modes.push(["skirtTo (hollow skin, ground from skirtTo)", { ...base, core: false, skirtTo }]);
    if (skirtTop !== undefined) modes.push(["skirtTo + skirtTop (only the shore)", { ...base, core: false, skirtTo, skirtTop }]);
  }
  return modes.map(([mode, args]) => {
    try {
      const r = planDrape(s.store, args).report as { fits: boolean; why?: string; skin: { blocks: number; cells: number }; core: { blocks: number; cells: number; neededCells: number } | null };
      return { mode, fits: r.fits, ...(r.why ? { why: r.why } : {}), blocks: r.skin.blocks + (r.core?.blocks ?? 0), skinCells: r.skin.cells, coreCells: r.core?.cells ?? 0,
        cells: r.skin.cells + (r.core?.cells ?? 0), ...(r.core ? { coreNeededCells: r.core.neededCells } : {}) };
    } catch (e) {
      return { mode, fits: false, why: e instanceof Error ? e.message : String(e) };
    }
  });
}

export const tools: Tool[] = [
  {
    name: "heightfield_drape",
    description: "Raise a heightfield (the same arguments as heightfield_region) as a smooth skin instead of voxel terraces: blocks of up to 31 x 31 columns, `thickness` voxels thick, sheared by per-corner height offsets the game applies to drawing and collision (a cone's flanks become slopes, not rings), over the stepped field cut just under the skin (`core`). Skin blocks cost X*thickness*Z cells each. The skin is read at its sheared height by query_ground, the renders, check_reachability and drops; structures, heightfield fills and sculpt edit it unsheared, so build those before draping or away from it. dryRun answers the plan. One undo step.",
    inputSchema: schema,
    handler(s, a: DrapeArgs & { dryRun?: boolean; estimateSkirts?: boolean }) {
      const { dryRun, estimateSkirts, ...args } = a;
      const p = planDrape(s.store, args);
      if (dryRun) return { ...p.report, ...(estimateSkirts ? { skirtEstimates: skirtEstimates(s, args) } : {}), applied: false };
      const r = applyDrape(s.store, p);
      const u = usage(s.store);
      return { ...p.report, applied: true, added: r.blocks, undo: "undo takes the whole drape out",
        limits: { newFrames: u.newFrames, voxelCells: u.voxelCells, patchBytes: u.patchBytes, runsPerFrame: u.runsPerFrame }, next: "save" };
    },
  },
];
