// Tool group "drape": a heightfield as a smooth skin. Blocks of whole columns a few voxels thick whose corner offsets
// (each block's HeightMap, which the game uses to move its columns up, drawn and for collision) carry the surface's
// height, over the usual stepped field cut just under it. The planner is src/lib/heightfield-drape.ts.
import { applyDrape, planDrape, type DrapeArgs } from "../lib/heightfield-drape";
import { usage } from "../lib/voxel";
import type { Schema, Tool } from "../registry";
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
    dryRun: { type: "boolean", description: "Plan and report only." },
  },
};

export const tools: Tool[] = [
  {
    name: "heightfield_drape",
    description: "Raise a heightfield (the same arguments as heightfield_region) as a smooth skin instead of voxel terraces: blocks of up to 31 x 31 columns, `thickness` voxels thick, sheared by per-corner height offsets the game applies to drawing and collision (a cone's flanks become slopes, not rings), over the stepped field cut just under the skin (`core`). Skin blocks cost X*thickness*Z cells each. The skin is read at its sheared height by query_ground, the renders, check_reachability and drops; structures, heightfield fills and sculpt edit it unsheared, so build those before draping or away from it. dryRun answers the plan. One undo step.",
    inputSchema: schema,
    handler(s, a: DrapeArgs & { dryRun?: boolean }) {
      const { dryRun, ...args } = a;
      const p = planDrape(s.store, args);
      if (dryRun) return { ...p.report, applied: false };
      const r = applyDrape(s.store, p);
      const u = usage(s.store);
      return { ...p.report, applied: true, added: r.blocks, undo: "undo takes the whole drape out",
        limits: { newFrames: u.newFrames, voxelCells: u.voxelCells, patchBytes: u.patchBytes, runsPerFrame: u.runsPerFrame }, next: "save" };
    },
  },
];
