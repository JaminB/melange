// Tool group "heightfield": terrain from a height per column (islands, volcanoes, hills), packed into the voxels a patch
// can carry. heightfield_plan answers the packing and the budget without editing; heightfield_region applies it as one
// undo step. The planner is src/lib/heightfield-plan.ts, the expression language src/lib/heightfield-expr.ts.
import { FUNCTIONS, MAX_NODES, MAX_SOURCE, VARIABLES } from "../lib/heightfield-expr";
import { MASK_VARIABLES, apply, planFit, type FieldArgs } from "../lib/heightfield-plan";
import { usage } from "../lib/voxel";
import type { Schema, Tool } from "../registry";

const pair = (what: string): Schema => ({ type: "array", items: { type: "number" }, minItems: 2, maxItems: 2, description: what });
const material: Schema = { type: "integer", minimum: 0, maximum: 63 };
const areas = (description: string): Schema => ({
  type: "array", maxItems: 16, description,
  items: { type: "object", additionalProperties: false, description: "A circle (x, z, radius) or a rectangle (min, max), in the call's units.",
    properties: { x: { type: "number" }, z: { type: "number" }, radius: { type: "number", minimum: 0 }, min: pair("[x, z] corner."), max: pair("[x, z] corner.") } },
});


export const schema: Schema & { type: "object" } = {
  type: "object", additionalProperties: false, required: ["min", "max", "floor"],
  properties: {
    units: { type: "string", enum: ["map", "world"], description: "Units of every length and height here (default map: 1 voxel = 1 map unit = 20 world units)." },
    voxelSize: { type: "number", minimum: 0.5, maximum: 4, description: "Map units per voxel: the new blocks' scale (default 1; under 1 is finer than the base map, 0.75 played in the game). The game counts every block's cells (X*Y*Z, at most 65535 over the whole level), so 2.5 covers 6.25 times the ground per cell, with voxels 2.5 map units (50 world units) across. Always map units, whatever `units` says." },
    min: pair("[x, z] of one corner of the rectangle, level coordinates."),
    max: pair("[x, z] of the opposite corner. Corners snap to whole voxels of the Scene frame's lattice."),
    floor: { type: "number", description: "Level y the columns stand on (heights count up from here). The blocks lie on the Scene frame's lattice of voxelSize, so the floor snaps to the nearest lattice height and every height stands that much lower or higher (floor -9 at voxel 2.5 is -10: a height of 14 has its ground at 4, not 5); the answer's `floor` says by how much (`shift`) and where a height h stands (`surface`). keepFloor counts the heights from the floor as given." },
    keepFloor: { type: "boolean", description: "Count the heights from `floor` as given even when it is off the lattice: a height h has its ground at floor + h (rounded to voxels; a draped skin's corners carry it exactly), not at the snapped floor + h. Default false (the heights count from the snapped floor, as before)." },
    expr: { type: "string", description: `Height above the floor per column, e.g. "40*exp(-(r/30)^2) + 3*noise2(x/12, z/12, 7)". Variables: ${VARIABLES.join(", ")} (x, z from the rectangle's centre; u, v 0..1 across it; r, theta polar about the centre; W, D its size; R half the shorter side). Functions: ${FUNCTIONS.join(", ")}. Operators + - * / ^ and parentheses. Bindings before the expression, each ending in ';': \`let name = expr;\` (computed once per column) and \`fn name(a, b) = expr;\` (a function of its parameters, the column's variables and the lets), e.g. "let cone = 44*exp(-(r/26)^2); fn bump(cx, cz, h) = h*exp(-((x-cx)^2+(z-cz)^2)/50); cone + bump(30, 0, 6)". At most ${MAX_SOURCE} characters and ${MAX_NODES} parts. A height under half a voxel leaves the column empty (see edges).` },
    grid: { type: "array", maxItems: 512, items: { type: "array", maxItems: 512, items: { type: "number" } },
      description: "Heights instead of expr: rows along z (row 0 at min z), columns along x, stretched over the rectangle (corner values at its corners), sampled bilinearly." },
    maxHeight: { type: "number", description: "Caps every height." },
    baseDepth: { type: "number", minimum: 0, description: "Also fill this far under the floor below every column (a skirt into the water). Default 0." },
    crater: { type: "object", additionalProperties: false, required: ["radius", "depth"], description: "Lowers heights inside radius by depth*(1-(d/radius)^2); rim adds a ridge at the edge.",
      properties: { radius: { type: "number", minimum: 0.0001 }, depth: { type: "number" }, x: { type: "number", description: "Centre (default: the rectangle's)." }, z: { type: "number" }, rim: { type: "number" } } },
    material: { ...material, description: "Material (0-63) where no rule matches. Default 0. The level's material file names them (the Erg palette)." },
    materials: { type: "array", maxItems: 64, description: "Rules (at most 64), first match wins: a voxel takes the first rule all of whose given bounds hold.",
      items: { type: "object", additionalProperties: false, required: ["material"], properties: {
        material,
        minHeight: { type: "number", description: "The voxel's height above the floor is at least this." },
        maxHeight: { type: "number", description: "... and under this." },
        minSlope: { type: "number", description: "The column's slope in degrees is at least this." },
        maxSlope: { type: "number", description: "... and at most this." },
        surface: { type: "integer", minimum: 1, description: "Only the top n voxels of a column." },
        minDepth: { type: "number", minimum: 0, description: "The voxel's depth under its column's top (in units) is at least this: rock under a skin, lava deep in a crater." },
        maxDepth: { type: "number", minimum: 0, description: "... and under this." },
        exposed: { type: "boolean", description: "true: only voxels with air above or beside them (the visible surface, cliff faces included); false: only buried ones." },
        jitter: { type: "object", additionalProperties: false, required: ["amount"], description: "Moves this rule's minHeight/maxHeight by up to amount (Perlin noise over x, z with feature size scale, default 8 units), so a band's edge wanders instead of running level.",
          properties: { amount: { type: "number" }, scale: { type: "number", minimum: 0.01 }, seed: { type: "integer" } } },
        region: areas("Only columns whose centre is in one of these areas (in the call's units): a beach on one side, lava in the crater."),
        mask: { type: "string", description: `Only where this expression (the expr language) is above 0: of the column's ${VARIABLES.join(", ")} and ${MASK_VARIABLES.join(", ")} (h the column's height above the floor, y the voxel's, depth under the column's top, all in units; slope in degrees). A lava flow down one flank: "0.3 - abs(theta - 0.8) + 0.1*noise2(x/6, z/6, 3)".` },
        masks: { type: "array", minItems: 1, maxItems: 64, items: { type: "string" }, description: "More masks in the same language: the rule applies where at least one of them is above 0 (and `mask`, when given, too). Ten lava tongues are one rule with ten masks." },
        feather: { type: "object", additionalProperties: false, required: ["width"], description: "A feathered edge instead of a hard one: the rule's mask(s) must pass a threshold, and its minHeight/maxHeight move by one, that varies per voxel within +-width/2 (in the mask's own units for masks, the call's units for heights): the boundary becomes a dithered band `width` wide instead of a staircase of whole voxels.",
          properties: { width: { type: "number", minimum: 0 }, pattern: { type: "string", enum: ["dither", "noise", "blue", "perlin"], description: "dither (default): a 4 x 4 ordered pattern over x, z and y (a checkerboard of single voxels); noise: a hash per voxel; blue: a 16 x 16 blue-noise tile (even, no checkerboard or clumps); perlin: smooth noise, so the edge wanders in lobes instead of breaking into squares." },
            cell: { type: "number", minimum: 0.01, description: "dither, noise, blue: the pattern's squares are this big (units; default one voxel: on a 2.5-voxel skin, cell 5 makes 2 x 2-voxel patches); perlin: its feature size (default 4 voxels)." },
            seed: { type: "integer", description: "noise, blue, perlin: another arrangement of the same pattern." } } },
        paths: { type: "array", minItems: 1, maxItems: 64, description: "Only within a path: the columns within width / 2 of one of these polylines (feather applies to the distance as to a mask). A road is one path instead of tens of sd() segments in a mask.",
          items: { type: "object", additionalProperties: false, required: ["width"], properties: {
            points: { type: "array", minItems: 2, maxItems: 512, items: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 }, description: "[x, z] level coordinates in the call's units, in order." },
            width: { type: "number", minimum: 0.01, description: "The path's width at its start (units)." },
            taper: { type: "number", minimum: 0, description: "Its width at the end as a multiple of width, linear along its length (default 1; 2 widens to twice)." },
            switchback: { type: "object", additionalProperties: false, required: ["from", "to", "legs", "spread"], description: "Instead of points: a zigzag from `from` to `to` in `legs` straight legs, each turn `spread` / 2 to alternate sides of the line between them (a path up a steep flank).",
              properties: { from: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 }, to: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 }, legs: { type: "integer", minimum: 1, maximum: 64 }, spread: { type: "number", minimum: 0 } } },
          } } },
        exclude: areas("Never in these areas (in the call's units), whatever else holds: a vegetation rule kept off the crater floor is exclude: [{x, z, radius}] of the crater."),
        excludeMask: { type: "string", description: "Never where this expression (the mask language) is above 0: the exclusion as an expression, for a shape no circle or rectangle covers (`18 - r`: inside the crater's rim)." },
        edge: { type: "object", additionalProperties: false, description: "The rule's own edge, drawn between voxel centres: a surface voxel next to the rule's area that a later rule (or the default) paints takes this rule's material as its second material on the corners where the rule holds (its area tested at the column's four corners), so the game draws the boundary where the mask or path really runs instead of a staircase of whole voxels: a road's edge on a 35-degree flank, the shore of a lava pool. The rule needs an area (region, mask, masks, noise or paths). depth: the top voxels of a column this applies to (1-8, default 1). Adds runs where it paints: Test the level.",
          properties: { depth: { type: "integer", minimum: 1, maximum: 8 } } },
        noise: { type: "object", additionalProperties: false, description: "Only where Perlin noise over x, z (feature size scale, default 8 units; octaves > 1 is fbm) is above threshold (default 0, about half the area): patches of ash, scree, grass.",
          properties: { threshold: { type: "number" }, scale: { type: "number", minimum: 0.01 }, seed: { type: "integer" }, octaves: { type: "integer", minimum: 1, maximum: 8 } } },
      } } },
    blend: { description: "Blend material edges per corner (default off): true, or {depth} (1-8 voxels, default 1). The top `depth` voxels of each column that touch another material (the 26 voxels round them, in the field) take the most common such material as their second material, on the corners they share with it, as the Erg panel's 2nd material brush does, so the game draws a gradient across the boundary voxel instead of a hard step. A drape compares its skin layer by layer. It adds runs where materials meet, and the corner mask also shapes collision slightly: Test the level.",
      anyOf: [{ type: "boolean" }, { type: "object", additionalProperties: false, properties: { depth: { type: "integer", minimum: 1, maximum: 8 } } }] },
    reuseFrames: { type: "string", enum: ["aligned", "any", "none"], description: "Fill voxel frames that overlap the field before adding blocks: aligned (default) only frames on the lattice (earlier blocks; the game's base frames are all rotated, scaled or off the whole units), any also rotated and scaled frames (approximate at the surface; not frames scaled under 0.9), none no frames." },
    maxBlocks: { type: "integer", minimum: 0, maximum: 64, description: "Add at most this many blocks (keep some of the 64 for other edits)." },
    maxCells: { type: "integer", minimum: 0, maximum: 65535, description: "The new blocks hold at most this many of the game's voxel cells (keep some of the 65535 for other edits, such as castles). While over it the planner halves the boxes that waste the most cells, using more blocks." },
    partial: { type: "boolean", description: "When the blocks do not all fit: place the ones holding the most voxels (never one floating over a dropped one) instead of refusing." },
    runsHeadroom: { type: "integer", minimum: 0, maximum: 1900, description: "Leave this many of each new block's 2000 runs free (default 0), for edits after it: sculpt_crater, painted bands, structures built into the blocks. Blocks are halved sooner, so it costs blocks." },
    centre: pair("[x, z] origin of the expression's x, z, r and theta (and the crater's default centre) instead of the rectangle's centre. Two fields of different voxelSize given the same rectangle and centre sample the same surface."),
    clip: areas("Keep only the columns whose centre is inside one of these areas: the fine part of a multi-resolution field."),
    edges: { type: "string", enum: ["round", "dither", "noise"], description: "How a height becomes whole voxels: round (default; a cone becomes concentric terraces), dither (a 4 x 4 ordered pattern) or noise (a hash per column): the terrace edges break up into a ragged line instead of square rings, at the same cost. The game's voxels are whole cubes (no slope or partial shapes), and its mesh smooths the steps." },
    packer: { type: "string", enum: ["tiles", "greedy", "best"], description: "How blocks are cut before the cell budget halves them: tiles (default) a grid of 31 x 31 columns, greedy tiles that start on the field's own columns (fewer blocks for rings and sparse fields), best both, keeping the fewer blocks." },
    fitBudget: { type: "boolean", description: "When the field does not fit whole within maxBlocks and maxCells (and the game's limits) at voxelSize, take the smallest coarser voxelSize (steps of 0.25, up to 4) at which it does, instead of halving boxes until the blocks are spent or refusing. The answer's fitted says the size taken and what each size tried cost. Two calls with clip and exclude (a fine summit, coarse lowlands), each with its own maxBlocks and fitBudget, choose a size per region." },
    costAt: { type: "array", maxItems: 8, items: { type: "number", minimum: 0.5, maximum: 4 }, description: "Also plan the same field at these voxel sizes and report each one's blocks and cells (costAt in the answer), to choose a size before building." },
    relax: { type: "integer", minimum: 0, maximum: 50, description: "Smooth the heights first: this many passes of a 3 x 3 mean over the field's columns (default 0). With heightfield_drape the skin and its core follow the smoothed surface." },
    fillBasins: { description: "No closed basins: true, or {maxDepth, outletBelow}. Every column lying more than maxDepth (units, default 0) under the level its basin would fill to before spilling is raised to that level less maxDepth (a priority flood over the field's columns, after relax and maxGradient), so the surface has no closed depression deeper than maxDepth: no pit a worm walks into and cannot leave, where a pad's flat ring or a mesa's foot cuts a dune trough off. What falls off the field leaves it: the rectangle's edge, columns outside clip and columns with no ground are outlets, and ground at or under outletBelow (a level y, in units: the sea) is one too; columns inside exclude are walls (the structure that stands there). 0.5 map units (a worm's step) keeps the dunes' shallow dishes. The answer's region.basins lists what was found and raised. With heightfield_drape the skin and its core follow the filled surface. Default off.",
      anyOf: [{ type: "boolean" }, { type: "object", additionalProperties: false, properties: { maxDepth: { type: "number", minimum: 0, description: "The deepest closed depression left, in units (default 0: none)." }, outletBelow: { type: "number", description: "Ground at or under this level y (units) is an outlet: the sea." } } }] },
    maxGradient: { type: "number", minimum: 1, maximum: 89, description: "A slope limiter: no column steeper than this (degrees) against its neighbours; each too-steep pair moves toward each other by half the excess (the volume stays), after relax. A cliff band of 70 degrees with maxGradient 40 becomes a 40-degree slope a few columns wide. The plan's region.slopeLimit gives the passes and the steepest slope left." },
    exclude: areas("Leave out the columns lying wholly inside one of these areas: the coarse part of a multi-resolution field (it overlaps a field clipped to the same areas at the edge, so no gap opens)."),
  },
};

export const tools: Tool[] = [
  {
    name: "heightfield_plan",
    description: "Dry run of heightfield_region: how the field packs into voxels without editing. Answers the cells lattice-aligned frames can take, the blocks (each at most 31 voxels a side and 2000 runs; tall fields stack them) with the game cells each costs, what does not fit, and the budget left (64 added blocks, the game's 65535 voxel cells, patch bytes, run voxels).",
    inputSchema: schema,
    handler(s, a: FieldArgs) {
      return { ...planFit(s.store, a).report, applied: false };
    },
  },
  {
    name: "heightfield_region",
    description: "Raise terrain from a height function over a rectangle: each column is filled from floor (less baseDepth) up to its height, materials by height bands, slope or surface layer, optional crater. Fills overlapping voxel frames first (reuseFrames), then adds blocks of at most 31 voxels a side (tall fields stack them), scaled by voxelSize. One undo step. Refused when it does not fit the 64 blocks or the game's 65535 voxel cells unless partial; heightfield_plan shows the plan first.",
    inputSchema: schema,
    handler(s, a: FieldArgs) {
      const store = s.store;
      const p = planFit(store, a);
      const r = apply(store, p);
      const u = usage(store);
      return { ...p.report, applied: true, added: r.blocks, undo: "undo takes the whole heightfield out",
        limits: { newFrames: u.newFrames, voxelCells: u.voxelCells, ops: u.ops, patchBytes: u.patchBytes, runsPerFrame: u.runsPerFrame }, next: "save" };
    },
  },
];
