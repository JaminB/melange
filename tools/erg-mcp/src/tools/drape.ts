// Tool group "drape": a heightfield as a smooth skin. Blocks of whole columns a few voxels thick whose corner offsets
// (each block's HeightMap, which the game uses to move its columns up, drawn and for collision) carry the surface's
// height, over the usual stepped field cut just under it. The planner is src/lib/heightfield-drape.ts.
import { JUMP_RISE } from "../lib/crater-traps";
import { applyDrape, planDrape, type DrapeArgs } from "../lib/heightfield-drape";
import { usage } from "../lib/voxel";
import type { Schema, Tool } from "../registry";
import type { Session } from "../session";
import { schema as field } from "./heightfield";

// fitBudget is heightfield_region's: a drape packs its skin and core itself; so is floorLattice (a skin's corners carry
// its heights exactly, off any lattice)
const { fitBudget: _fit, floorLattice: _lattice, ...fieldProps } = field.properties!;

/** crestMaterial / troughMaterial: a material by local curvature of the skin (steepMaterial wins over both). */
const curve = (what: string, rule: string): Schema => ({ type: "object", additionalProperties: false, required: ["material"],
  description: `${what} by curvature: ${rule}, whatever the materials rules give it (steepMaterial still wins, and a crest over a trough). A dune's sand then reads lighter on the crest and darker in the hollow. minCurvature is per map unit (default 0.01; a dune 6 high over a 40-unit wavelength reads about 0.07 at its crest); radius (map units, default one voxel) is how far the neighbours are sampled, so a coarse skin or noisy field wants a larger one; feather (the same unit as minCurvature) dithers the edge. The plan's curveMaterials gives the column counts.`,
  properties: { material: { type: "integer", minimum: 0, maximum: 63 }, minCurvature: { type: "number", minimum: 0 }, radius: { type: "number", minimum: 0.01 }, feather: { type: "number", minimum: 0 }, blend: { type: "boolean", description: "Soften the edge: each column beside the rule's columns takes the material as its second material on the corners it shares with them (the game's per-corner blend), so the crest line is a gradient that follows the surface instead of a saw-tooth of whole columns (default false). Use it without feather (a dithered edge blends into speckles). The plan's curveMaterials.blendedEdge counts those columns." } } });

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
    crestMaterial: curve("Dune crests", "a skin column whose surface curves up (a ridge: the Laplacian of the skin's heights under -minCurvature) takes its material in every skin voxel"),
    troughMaterial: curve("The hollows between dunes", "a skin column whose surface curves down (a hollow: the Laplacian over minCurvature) takes its material in every skin voxel"),
    coast: { type: "object", additionalProperties: false, required: ["seaLevel", "rimMaterial"], description: "The island's rim against a recoloured sea, off by default: a skin column whose lowest corner is at or under seaLevel + band takes rimMaterial in every skin voxel, before steepMaterial and the crest and trough rules, so the shore is the sea's colour whatever its slope (a sand sea otherwise shows the shore's cliff or trough material as a ring round the island). blendToSea: the columns inland of the rim take rimMaterial as their second material on the corners they share with it (the game's per-corner blend), so the rim fades into the land. The answer's coast counts the columns.",
      properties: { seaLevel: { type: "number", description: "The water's level y (units) at the match's start." }, rimMaterial: { type: "integer", minimum: 0, maximum: 63 },
        band: { type: "number", minimum: 0, description: "How far over seaLevel the rim reaches (units; default one voxel)." }, blendToSea: { type: "boolean" } } },
    tileOrigin: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2, description: "[x, z] in units: a corner of the skin's tiling (its blocks of 31 x 31 columns) falls here, snapped to the lattice, instead of at the region's min corner. Where four skin blocks meet the game lets mines and worms through (Caldera v2): put those corners where no one walks." },
    cornersOff: { type: "object", additionalProperties: false, required: ["paths"], description: "Instead of tileOrigin: the tiling shifted (0-30 columns along x and z) so every corner where four skin blocks meet is at least `distance` (map units, default 4) from `paths` (the trails: polylines of [x, z] or [x, y, z] in units) where a shift does that, else as far as one can; the fewest blocks first. The answer's skin.tiling gives the shift and the nearest corner's distance (unshifted: without the shift), skin.meetingCorners lists the corners: no plug under a corner near a trail is needed.",
      properties: { paths: { type: "array", minItems: 1, maxItems: 32, items: { type: "array", minItems: 2, maxItems: 4000, items: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 3 } } },
        distance: { type: "number", minimum: 0, maximum: 40, description: "Map units (default 4)." } } },
    refine: { type: "object", additionalProperties: false, required: ["voxelSize"], description: "Fine voxels where they matter: the same field draped again at this finer voxelSize over the zone within `distance` (default 4 map units) of `paths` (the trails: polylines of [x, z] or [x, y, z] in units) and/or inside `clip` areas, the coarse skin's columns lying wholly inside the zone left out, the two overlapping by the coarse columns the zone only partly covers (as the heightfield tools' clip and exclude), so no gap opens. With one voxelSize for the whole skin a 2.5-unit column cannot hold a trail bed and its banks: the banks lean into the bed and narrow ridges form on the steep upper cone (Caldera v2 round 4). The fine skin's heights count from the coarse lattice's floor, its relax passes are scaled by (voxelSize / refine.voxelSize)^2, its tiling is shifted off the paths (cornersOff) unless tileOrigin is given; it is planned first and the coarse skin gets what maxBlocks and maxCells leave. The answer's `refine` gives its blocks, cells, columns and meetingCorners. One undo step with the coarse skin.",
      properties: { voxelSize: { type: "number", minimum: 0.5, maximum: 4, description: "Map units per voxel of the fine skin, finer than voxelSize." },
        paths: { type: "array", minItems: 1, maxItems: 32, items: { type: "array", minItems: 2, maxItems: 4000, items: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 3 } }, description: "The trails, in units." },
        distance: { type: "number", minimum: 0.5, maximum: 40, description: "The zone's half width round the paths, in units (default 4 map units)." },
        clip: { type: "array", maxItems: 32, items: { type: "object", additionalProperties: false, properties: { x: { type: "number" }, z: { type: "number" }, radius: { type: "number", minimum: 0 }, min: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 }, max: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 } } }, description: "Areas (circles x, z, radius or rectangles min, max) also in the zone: a summit." } } },
    craterSlope: { description: "Keep a blast's hole climbable, off by default: true, or {jump (map units, default 2.48: what a worm's jump rises), clear (default 0.25: the room over the lip), layers (default 1: the voxel layers a blast takes at its rim)}. The game cuts a crater out of a skin in whole voxels, so its walls are a voxel high: in a skin of 2.5-unit voxels a one-layer hole on level sand has walls a jump (2.48) does not clear, and the worm in it is stuck (check_crater_traps finds them). With craterSlope the skin's voxelSize is held to the largest quarter unit whose wall a jump clears ((jump - clear) / layers: 2 map units by the defaults); a voxelSize already at or under it is left alone. The game has no way to slope a crater's wall itself (the corner offsets move whole columns), so the walls stay steps: low enough to jump. The answer's `craterSlope` says the size asked and used and what it costs: the skin's cells at both (dryRun shows it before building).",
      anyOf: [{ type: "boolean" }, { type: "object", additionalProperties: false, properties: { jump: { type: "number", minimum: 0.5, maximum: 10 }, clear: { type: "number", minimum: 0, maximum: 2 }, layers: { type: "integer", minimum: 1, maximum: 4 } } }] },
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
    description: "Raise a heightfield (the same arguments as heightfield_region) as a smooth skin instead of voxel terraces: blocks of up to 31 x 31 columns, `thickness` voxels thick, sheared by per-corner height offsets the game applies to drawing and collision (a cone's flanks become slopes, not rings), over the stepped field cut just under the skin (`core`). Skin blocks cost X*thickness*Z cells each. The skin is read at its sheared height by query_ground, the renders, check_reachability and drops; structures, heightfield fills and sculpt edit it unsheared, so build those before draping or away from it. `refine` drapes a finer skin along the trails (or in areas) and cuts the coarse one out there, in the same undo step. dryRun answers the plan. One undo step.",
    inputSchema: schema,
    handler(s, a: DrapeArgs & { dryRun?: boolean; estimateSkirts?: boolean; craterSlope?: boolean | { jump?: number; clear?: number; layers?: number } }) {
      const { dryRun, estimateSkirts, craterSlope, ...args0 } = a;
      let args: DrapeArgs = args0, crater: Record<string, unknown> | undefined;
      if (craterSlope) {
        // the largest quarter-unit voxel whose crater wall a jump clears; the skin as asked is planned too, for its cost
        const o = typeof craterSlope === "object" ? craterSlope : {}, jump = o.jump ?? JUMP_RISE, clear = o.clear ?? 0.25, layers = o.layers ?? 1;
        const cap = Math.max(0.5, Math.floor(((jump - clear) / layers) * 4) / 4), asked = args0.voxelSize ?? 1;
        const cells = (x: DrapeArgs) => { try { const r = planDrape(s.store, x).report as { fits?: boolean; skin?: { blocks: number; cells: number } }; return { blocks: r.skin?.blocks ?? null, cells: r.skin?.cells ?? null, fits: !!r.fits }; } catch (e) { return { blocks: null, cells: null, fits: false, why: e instanceof Error ? e.message : String(e) }; } };
        if (asked > cap + 1e-9) args = { ...args0, voxelSize: cap };
        crater = { voxelSize: { asked, used: Math.min(asked, cap) }, wall: { asked: Math.round(asked * layers * 100) / 100, used: Math.round(Math.min(asked, cap) * layers * 100) / 100 }, jump, clear, layers,
          ...(asked > cap + 1e-9 ? { skinAsAsked: cells(args0), note: `voxelSize ${asked} leaves crater walls of ${Math.round(asked * layers * 100) / 100}, which a jump of ${jump} does not clear with ${clear} to spare: the skin is built at ${cap}` }
            : { note: `voxelSize ${asked} already leaves crater walls a jump of ${jump} clears with ${clear} to spare` }),
          check: "check_crater_traps cuts trial craters into the built skin and walks out of them" };
      }
      const p = planDrape(s.store, args);
      if (dryRun) return { ...p.report, ...(crater ? { craterSlope: crater } : {}), ...(estimateSkirts ? { skirtEstimates: skirtEstimates(s, args) } : {}), applied: false };
      const r = applyDrape(s.store, p);
      const u = usage(s.store);
      return { ...p.report, ...(crater ? { craterSlope: crater } : {}), applied: true, added: r.blocks, undo: "undo takes the whole drape out",
        limits: { newFrames: u.newFrames, voxelCells: u.voxelCells, patchBytes: u.patchBytes, runsPerFrame: u.runsPerFrame }, next: "save" };
    },
  },
];
