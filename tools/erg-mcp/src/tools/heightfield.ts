// Tool group "heightfield": terrain from a height per column (islands, volcanoes, hills), packed into the voxels a patch
// can carry. heightfield_plan answers the packing and the budget without editing; heightfield_region applies it as one
// undo step. The planner is src/lib/heightfield-plan.ts, the expression language src/lib/heightfield-expr.ts.
import { FUNCTIONS, VARIABLES } from "../lib/heightfield-expr";
import { apply, plan, type FieldArgs } from "../lib/heightfield-plan";
import { usage } from "../lib/voxel";
import type { Schema, Tool } from "../registry";

const pair = (what: string): Schema => ({ type: "array", items: { type: "number" }, minItems: 2, maxItems: 2, description: what });
const material: Schema = { type: "integer", minimum: 0, maximum: 63 };

const schema: Schema & { type: "object" } = {
  type: "object", additionalProperties: false, required: ["min", "max", "floor"],
  properties: {
    units: { type: "string", enum: ["map", "world"], description: "Units of every length and height here (default map: 1 voxel = 1 map unit = 20 world units)." },
    min: pair("[x, z] of one corner of the rectangle, level coordinates."),
    max: pair("[x, z] of the opposite corner. Corners snap to the Scene frame's whole map units."),
    floor: { type: "number", description: "Level y the columns stand on (heights count up from here)." },
    expr: { type: "string", description: `Height above the floor per column, e.g. "40*exp(-(r/30)^2) + 3*noise2(x/12, z/12, 7)". Variables: ${VARIABLES.join(", ")} (x, z from the rectangle's centre; u, v 0..1 across it; r, theta polar about the centre; W, D its size; R half the shorter side). Functions: ${FUNCTIONS.join(", ")}. Operators + - * / ^ and parentheses. A height under half a voxel leaves the column empty.` },
    grid: { type: "array", maxItems: 512, items: { type: "array", maxItems: 512, items: { type: "number" } },
      description: "Heights instead of expr: rows along z (row 0 at min z), columns along x, stretched over the rectangle (corner values at its corners), sampled bilinearly." },
    maxHeight: { type: "number", description: "Caps every height." },
    baseDepth: { type: "number", minimum: 0, description: "Also fill this far under the floor below every column (a skirt into the water). Default 0." },
    crater: { type: "object", additionalProperties: false, required: ["radius", "depth"], description: "Lowers heights inside radius by depth*(1-(d/radius)^2); rim adds a ridge at the edge.",
      properties: { radius: { type: "number", minimum: 0.0001 }, depth: { type: "number" }, x: { type: "number", description: "Centre (default: the rectangle's)." }, z: { type: "number" }, rim: { type: "number" } } },
    material: { ...material, description: "Material (0-63) where no rule matches. Default 0. The level's material file names them (the Erg palette)." },
    materials: { type: "array", maxItems: 16, description: "Rules, first match wins: a voxel takes the first rule all of whose given bounds hold.",
      items: { type: "object", additionalProperties: false, required: ["material"], properties: {
        material,
        minHeight: { type: "number", description: "The voxel's height above the floor is at least this." },
        maxHeight: { type: "number", description: "... and under this." },
        minSlope: { type: "number", description: "The column's slope in degrees is at least this." },
        maxSlope: { type: "number", description: "... and at most this." },
        surface: { type: "integer", minimum: 1, description: "Only the top n voxels of a column." },
      } } },
    reuseFrames: { type: "string", enum: ["aligned", "any", "none"], description: "Fill voxel frames that overlap the field before adding blocks: aligned (default) only frames on the lattice (earlier blocks; the game's base frames are all rotated, scaled or off the whole units), any also rotated and scaled frames (approximate at the surface; not frames scaled under 0.9), none no frames." },
    maxBlocks: { type: "integer", minimum: 0, maximum: 64, description: "Add at most this many blocks (keep some of the 64 for other edits)." },
    partial: { type: "boolean", description: "When the blocks do not all fit: place the ones holding the most voxels (never one floating over a dropped one) instead of refusing." },
  },
};

export const tools: Tool[] = [
  {
    name: "heightfield_plan",
    description: "Dry run of heightfield_region: how the field packs into voxels without editing. Answers the cells lattice-aligned frames can take, the blocks (each at most 32 voxels a side and 2000 runs; tall fields stack them), what does not fit, and the budget left (64 added blocks, patch bytes, run voxels).",
    inputSchema: schema,
    handler(s, a: FieldArgs) {
      return { ...plan(s.store, a).report, applied: false };
    },
  },
  {
    name: "heightfield_region",
    description: "Raise terrain from a height function over a rectangle: each column is filled from floor (less baseDepth) up to its height, materials by height bands, slope or surface layer, optional crater. Fills overlapping voxel frames first (reuseFrames), then adds blocks of at most 32 voxels a side (tall fields stack them). One undo step. Refused when it does not fit the 64-block budget unless partial; heightfield_plan shows the plan first.",
    inputSchema: schema,
    handler(s, a: FieldArgs) {
      const store = s.store;
      const p = plan(store, a);
      const r = apply(store, p);
      const u = usage(store);
      return { ...p.report, applied: true, added: r.blocks, undo: "undo takes the whole heightfield out",
        limits: { newFrames: u.newFrames, ops: u.ops, patchBytes: u.patchBytes, runsPerFrame: u.runsPerFrame }, next: "save" };
    },
  },
];
