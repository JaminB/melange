// Tool group "terrain": sculpting with the panel's brushes (one dab or along a path), added blocks, clearing the base
// terrain, raw box fills and the painted surround. The engine is src/lib/terrain-ops.ts.
import { SURROUND_SIDE, type Vec3 } from "../core";
import {
  addBlock, clearBase, compileFormula as compile, enableSurroundPaint, fillBox, frameGrids, patchLeft, pathPoints, pickAnchor, pointToMap, removeBlock, setSurroundGrid,
  stroke, surroundDabs, surroundSummary, type Units,
} from "../lib/terrain-ops";
import { ToolError, type Schema, type Tool } from "../registry";

const vec = (description: string, integer = false): Schema => ({ type: "array", description, items: { type: integer ? "integer" : "number" }, minItems: 3, maxItems: 3 });
const units: Schema = { type: "string", enum: ["world", "map", "grid"], description: "Units of the coordinates: world (the Erg panel's, = map x 20; default), map (the level file's; 1 voxel = 1 map unit on an unscaled frame) or grid (the voxel cells of `frame`)." };
const dryRun: Schema = { type: "boolean", description: "Work out the change and report it without applying it." };
const material: Schema = { description: "Material 0-63 (the palette index). fill also takes 'column'.", type: "integer", minimum: 0, maximum: 63 };

const brushProps = {
  mode: { type: "string", enum: ["carve", "fill", "paint"], description: "carve clears solid voxels in every frame the brush overlaps (keeping their material bits); fill turns empty voxels solid with `material` (anchor frame only); paint changes the material of solid voxels (anchor frame only)." },
  shape: { type: "string", enum: ["box", "sphere"], description: "A voxel is inside when its centre is: a box, or the ellipsoid of the size (default sphere)." },
  size: vec("Brush size in voxels of the anchor frame, each 1-32 (a sphere of diameter 5 is [5,5,5]).", true),
  material,
  materialColumn: { type: "boolean", description: "fill only: take each voxel's material from the highest solid voxel in its column of the frame (the panel's 'column' material); ignores `material`." },
  frame: { type: "integer", description: "Anchor frame id (describe_level). Fill and paint touch only this frame; carve reaches every frame. Default: the frame holding the point (a solid voxel there first, then the smallest)." },
  units,
  snap: { type: "boolean", description: "Snap each dab's centre to the middle of its voxel, as the panel does (default true). false keeps fractional centres." },
  dryRun,
} as const;

interface Brushy {
  mode: "carve" | "fill" | "paint"; shape?: "box" | "sphere"; size: Vec3; material?: number; materialColumn?: boolean; frame?: number;
  units?: Units; snap?: boolean; dryRun?: boolean;
}

function spec(a: Brushy) {
  if (a.materialColumn && a.mode !== "fill") throw new ToolError("materialColumn is for fill only");
  return { mode: a.mode, shape: a.shape ?? "sphere", size: a.size, material: a.materialColumn ? ("column" as const) : a.material };
}

const gridUnitsNeedFrame = (a: { units?: Units; frame?: number }) => {
  if (a.units === "grid" && a.frame === undefined) throw new ToolError("units 'grid' needs `frame`");
};

export const tools: Tool[] = [
  {
    name: "sculpt",
    description: "Apply one terrain brush dab (the Terrain tab's brush) centred on a point: carve a hole, fill empty space with a material, or repaint solid voxels' material. One undo step. Needs the project open (open_project). Fill places voxels where the brush is: centre it in the empty cells to add (e.g. just above a floor), in a frame that has room there; to add outside every frame use add_block.",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["mode", "size", "center"],
      properties: { ...brushProps, center: vec("The brush centre, in `units`.") },
    },
    handler(s, a: Brushy & { center: Vec3 }) {
      const { store } = s.requireProject();
      gridUnitsNeedFrame(a);
      const gs = frameGrids(store);
      const anchor = a.units === "grid"
        ? pickAnchor(store, gs, [0, 0, 0], a.frame, false)
        : pickAnchor(store, gs, pointToMap(a.center, a.units ?? "world"), a.frame, a.mode === "fill");
      const at = pointToMap(a.center, a.units ?? "world", anchor);
      return stroke(store, spec(a), [at], anchor, { snap: a.snap ?? true, dryRun: a.dryRun });
    },
  },
  {
    name: "sculpt_path",
    description: "Drag a terrain brush along a polyline: dabs every `spacing` units from the first point to the last, each seeing the one before, all anchored on the frame the stroke started on (the first point's frame, or `frame`). Fill and paint stay inside that frame, carve reaches through every frame it overlaps. One undo step; refused as a whole if a frame would need over 2000 runs or the patch outgrow its budget.",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["mode", "size", "points"],
      properties: {
        ...brushProps,
        points: { type: "array", minItems: 1, maxItems: 500, items: vec("A path point."), description: "The path, as [x,y,z] points in `units`." },
        spacing: { type: "number", minimum: 0.1, description: "Distance between dabs in map units (default: half the smallest brush side, at least 1)." },
      },
    },
    handler(s, a: Brushy & { points: Vec3[]; spacing?: number }) {
      const { store } = s.requireProject();
      gridUnitsNeedFrame(a);
      const gs = frameGrids(store);
      const u = a.units ?? "world";
      const anchor = u === "grid" ? pickAnchor(store, gs, [0, 0, 0], a.frame, false) : pickAnchor(store, gs, pointToMap(a.points[0], u), a.frame, a.mode === "fill");
      const pts = a.points.map((p) => pointToMap(p, u, anchor));
      const spacing = a.spacing ?? Math.max(1, Math.min(...a.size) / 2);
      const dabs = pathPoints(pts, spacing);
      const r = stroke(store, spec(a), dabs, anchor, { snap: a.snap ?? true, dryRun: a.dryRun, label: `${a.mode[0].toUpperCase()}${a.mode.slice(1)} terrain path` });
      return { ...r, spacing, pathLength: pts.slice(1).reduce((n, p, i) => n + Math.hypot(p[0] - pts[i][0], p[1] - pts[i][1], p[2] - pts[i][2]), 0) };
    },
  },
  {
    name: "add_block",
    description: "Add a terrain block: a new solid frame of size X x Y x Z voxels (each 1-31; at most 64 blocks per level) of one material, under the Scene frame, optionally scaled (`scale` map units per voxel, 1-4). Refused when the level's voxel frames would pass the game's 65535 cells (X*Y*Z of every voxel frame; describe_level limits.voxelCells). Its corner snaps to whole units. Give `at` (x and z are the block's centre, y its bottom: it rests on that point) or `center`. Shape it afterwards with sculpt (carve) or fill_box_raw. Answers the new frame id (negative until saved) and its bounds.",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["size", "material"],
      properties: {
        size: vec("Block size in voxels, each 1-31.", true),
        scale: { type: "number", minimum: 0.5, maximum: 4, description: "Map units per voxel, the same on every axis (default 1; 0.5-1 is finer than the base map, for detail such as a crater lip: a cell costs the same, so a block of 31 at 0.75 covers 23.25 map units): a block of 31 at scale 2.5 is 77.5 map units across for the same cells." },
        material: { ...material, description: "Material 0-63 the block is filled with." },
        at: vec("The point the block rests on: [centre x, bottom y, centre z], in `units`."),
        center: vec("The block's centre, in `units`."),
        units: { type: "string", enum: ["world", "map"], description: "world (default) or map units." },
        dryRun: { type: "boolean", description: "Report the block's cost (the game's cells before and after, its bounds) without adding it." },
      },
    },
    handler(s, a: { size: Vec3; material: number; at?: Vec3; center?: Vec3; units?: "world" | "map"; scale?: number; dryRun?: boolean }) {
      const { store } = s.requireProject();
      return { ...addBlock(store, { ...a, units: a.units ?? "world" }), ...patchLeft(store) };
    },
  },
  {
    name: "remove_block",
    description: "Remove a terrain block added in this project (by its frame id from add_block or describe_level). Base map frames cannot be removed: use clear_base_terrain.",
    inputSchema: { type: "object", additionalProperties: false, required: ["id"], properties: { id: { type: "integer", description: "The block's frame id." } } },
    handler(s, a: { id: number }) {
      return removeBlock(s.requireProject().store, a.id);
    },
  },
  {
    name: "clear_base_terrain",
    description: "Carve away every solid voxel of chosen base-map frames, optionally only inside a box, to start from open space. Choose frames by `frames` ids and/or `search` name; with a `box` and no frames, every base frame is considered. Added blocks are kept unless includeBlocks. Needs frames, search or box (never clears the whole level by accident); use `all` to say so. One undo step. dryRun counts first.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        frames: { type: "array", items: { type: "integer" }, minItems: 1, maxItems: 2000, description: "Frame ids to clear." },
        search: { type: "string", description: "Frames whose name contains this (any case)." },
        box: { type: "object", additionalProperties: false, required: ["min", "max"], properties: { min: vec("Box minimum corner."), max: vec("Box maximum corner.") }, description: "Only voxels whose centre is in [min, max)." },
        units,
        all: { type: "boolean", description: "Clear every base frame (required when none of frames, search or box is given)." },
        includeBlocks: { type: "boolean", description: "Also clear blocks added in this project." },
        wipe: { type: "boolean", description: "Write empty words (0) instead of carving (which keeps each voxel's material bits): smaller patch runs." },
        frame: { type: "integer", description: "Frame for units 'grid'." },
        dryRun,
      },
    },
    handler(s, a: { frames?: number[]; search?: string; box?: { min: Vec3; max: Vec3 }; units?: Units; all?: boolean; includeBlocks?: boolean; wipe?: boolean; frame?: number; dryRun?: boolean }) {
      const { store } = s.requireProject();
      if (!a.frames && !a.search && !a.box && !a.all) throw new ToolError("give frames, search or box, or all: true to clear the whole base");
      if (a.units === "grid") {
        if (a.frame === undefined || a.frames) throw new ToolError("units 'grid' takes a single `frame` (and no `frames`)");
        return clearBase(store, { ...a, frames: [a.frame], units: "grid" });
      }
      return { ...clearBase(store, { ...a, units: a.units ?? "world" }), ...patchLeft(store) };
    },
  },
  {
    name: "fill_box_raw",
    description: "Write voxels directly into a box of any voxel frame, base or added block: solid with a material, empty, or a raw voxel word. The box is the voxels whose centre is in [min, max) in `units` (grid: voxel indices, so min [0,0,0] max [2,1,1] is two voxels). Words must be valid edits of the base word (bits 24-31 clear, solid bits 0 or 3, bits 8-23 zero or the base's). One undo step.",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["frame"],
      properties: {
        frame: { type: "integer", description: "Frame id (negative for a block added in this project)." },
        min: vec("Box minimum corner (default: the frame's own corner)."),
        max: vec("Box maximum corner (default: the frame's far corner)."),
        units: { type: "string", enum: ["world", "map", "grid"], description: "grid (default): voxel indices of the frame. world or map: level coordinates." },
        state: { type: "string", enum: ["solid", "empty"], description: "solid needs `material`. empty carves (keeping material bits), or `wipe`s to 0." },
        material,
        word: { type: "integer", minimum: 0, maximum: 16777215, description: "A raw voxel word, overriding state/material (bits 0-1 solid 3 or empty 0, 2-7 material)." },
        only: { type: "string", enum: ["empty", "solid"], description: "Touch only voxels that are now empty (or solid)." },
        wipe: { type: "boolean", description: "state empty: write 0 instead of carving." },
        dryRun,
      },
    },
    handler(s, a: { frame: number; min?: Vec3; max?: Vec3; units?: Units; state?: "solid" | "empty"; material?: number; word?: number; only?: "empty" | "solid"; wipe?: boolean; dryRun?: boolean }) {
      const { store } = s.requireProject();
      const g = frameGrids(store).find((x) => x.frame.id === a.frame);
      if (!g) throw new ToolError(`frame ${a.frame} has no voxels`);
      const u = a.units ?? "grid";
      if ((a.min || a.max) && !(a.min && a.max)) throw new ToolError("give both min and max");
      const box = a.min && a.max ? { min: a.min, max: a.max } : undefined;
      if (a.word === undefined && !a.state) throw new ToolError("give state (solid or empty) or a raw word");
      return { ...fillBox(store, { frame: a.frame, box, units: u, state: a.state ?? "solid", material: a.material, word: a.word, only: a.only, wipe: a.wipe, dryRun: a.dryRun }), ...patchLeft(store) };
    },
  },
  {
    name: "paint_surround",
    description: `Paint the surround (.hmp), the far-off scenery ring: a ${SURROUND_SIDE}x${SURROUND_SIDE} top-down height grid, heights 0-1 (relative), row-major (y then x), cell (0,0) the first. First use switches the level to a painted surround (reported). Ops raise/lower/flatten/smooth dab the panel's brush at \`points\`; set_grid writes a whole grid from \`heights\` or a \`formula\`. One undo step. read_surround shows the result.`,
    inputSchema: {
      type: "object", additionalProperties: false, required: ["op"],
      properties: {
        op: { type: "string", enum: ["raise", "lower", "flatten", "smooth", "set_grid"] },
        points: { type: "array", minItems: 1, maxItems: 500, items: { type: "array", items: { type: "number", minimum: 0, maximum: SURROUND_SIDE }, minItems: 2, maxItems: 2 }, description: "Brush centres [x, y] in cells (0-100, fractional allowed); several are a path dabbed every `spacing` cells." },
        radius: { type: "number", minimum: 1, maximum: 25, description: "Brush radius in cells (default 4)." },
        strength: { type: "number", minimum: 0.05, maximum: 1, description: "0.05-1 (default 0.5). Raise/lower step 0.1 x strength x falloff per dab." },
        level: { type: "number", minimum: 0, maximum: 1, description: "flatten: the height to pull toward (default 0)." },
        passes: { type: "integer", minimum: 1, maximum: 100, description: "Repeat the dabs this many times (default 1)." },
        spacing: { type: "number", minimum: 0.25, description: "Cells between dabs along a path (default 1)." },
        heights: { type: "array", description: `set_grid: ${SURROUND_SIDE} rows (y) of ${SURROUND_SIDE} heights (x), or ${SURROUND_SIDE * SURROUND_SIDE} numbers y*${SURROUND_SIDE}+x; clamped to 0-1.`, items: {} },
        formula: { type: "string", description: "set_grid: an expression of x, y (cells 0-99), u, v (0-1 across) and h (current height), with sin cos tan abs min max pow sqrt floor ceil round exp log sign hypot atan atan2 PI E, e.g. 0.3 + 0.2 * sin(u * 6.28) * cos(v * 6.28)." },
        region: { type: "object", additionalProperties: false, required: ["x0", "y0", "x1", "y1"], properties: { x0: { type: "integer" }, y0: { type: "integer" }, x1: { type: "integer" }, y1: { type: "integer" } }, description: "set_grid: only cells x0..x1, y0..y1 inclusive." },
        mix: { type: "number", minimum: 0, maximum: 1, description: "set_grid: how far to move each cell from its height to the target (default 1)." },
        enablePaint: { type: "boolean", description: "Switch the level to a painted surround when it is not (default true); false refuses instead." },
      },
    },
    handler(s, a: { op: "raise" | "lower" | "flatten" | "smooth" | "set_grid"; points?: [number, number][]; radius?: number; strength?: number; level?: number; passes?: number; spacing?: number; heights?: number[] | number[][]; formula?: string; region?: { x0: number; y0: number; x1: number; y1: number }; mix?: number; enablePaint?: boolean }) {
      const { store } = s.requireProject();
      if (a.op === "set_grid") {
        if (!a.heights && !a.formula) throw new ToolError("set_grid needs `heights` or `formula`");
        if (a.points) throw new ToolError("set_grid takes heights/formula/region, not points");
      } else {
        if (!a.points) throw new ToolError(`${a.op} needs \`points\``);
        if (a.heights || a.formula) throw new ToolError("heights and formula are for set_grid");
      }
      if (a.enablePaint === false && store.scene.hmp.mode !== "paint") throw new ToolError(`the surround is '${store.scene.hmp.mode}': set enablePaint (default) to switch it to painted`);
      // validate the work before switching modes, so a bad request changes nothing
      if (a.op === "set_grid") {
        const probe = setSurroundGridDry(a);
        if (probe) throw new ToolError(probe);
      }
      const switched = enableSurroundPaint(store);
      const r = a.op === "set_grid"
        ? setSurroundGrid(store, { heights: a.heights, formula: a.formula, region: a.region, mix: a.mix ?? 1 })
        : surroundDabs(store, { mode: a.op, points: a.points!, radius: a.radius ?? 4, strength: a.strength ?? 0.5, level: a.level ?? 0, passes: a.passes ?? 1, spacing: a.spacing ?? 1 });
      return { ...r, ...(switched ? { switchedToPaint: true } : {}), surround: surroundSummary(store, 20), ...patchLeft(store) };
    },
  },
  {
    name: "read_surround",
    description: "The open project's painted surround heights (what paint_surround edits; the base's heights until painted): min, max, mean and the average of each step x step block of cells (rows are y, columns x). Optionally the cells of a region exactly.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        step: { type: "integer", minimum: 1, maximum: 100, description: "Block size in cells for the summary grid (default 10)." },
        region: { type: "object", additionalProperties: false, required: ["x0", "y0", "x1", "y1"], properties: { x0: { type: "integer", minimum: 0, maximum: 99 }, y0: { type: "integer", minimum: 0, maximum: 99 }, x1: { type: "integer", minimum: 0, maximum: 99 }, y1: { type: "integer", minimum: 0, maximum: 99 } }, description: "Also return these cells (at most 400)." },
      },
    },
    handler(s, a: { step?: number; region?: { x0: number; y0: number; x1: number; y1: number } }) {
      const { store } = s.requireProject();
      const out: Record<string, unknown> = { mode: store.scene.hmp.mode, canPaint: store.canPaintSurround, ...surroundSummary(store, a.step ?? 10) };
      if (a.region) {
        const r = a.region, w = r.x1 - r.x0 + 1, h = r.y1 - r.y0 + 1;
        if (w < 1 || h < 1 || w * h > 400) throw new ToolError("region: x0 <= x1, y0 <= y1 and at most 400 cells");
        out.region = { ...r, heights: Array.from({ length: h }, (_, j) => Array.from({ length: w }, (_, i) => Math.round(store.surround.heights[(r.y0 + j) * SURROUND_SIDE + r.x0 + i] * 256) / 256)) };
      }
      return out;
    },
  },
];

/** Problems set_grid would find in its arguments, found before any state changes (a formula that does not compile, the
 * wrong array shape). */
function setSurroundGridDry(a: { heights?: number[] | number[][]; formula?: string; region?: { x0: number; y0: number; x1: number; y1: number } }): string | null {
  try {
    const N = SURROUND_SIDE;
    if (a.heights && a.formula) return "give exactly one of `heights` or `formula`";
    if (a.region && (a.region.x0 > a.region.x1 || a.region.y0 > a.region.y1)) return "region: x0 <= x1 and y0 <= y1";
    if (a.heights) {
      const h = a.heights;
      if (Array.isArray(h[0])) { if (h.length !== N || (h as number[][]).some((r) => !Array.isArray(r) || r.length !== N)) return `heights: ${N} rows of ${N} numbers`; }
      else if (h.length !== N * N) return `heights: ${N * N} numbers (or ${N} rows of ${N})`;
    }
    if (a.formula) {
      const f = compile(a.formula);
      for (const [x, y] of [[0, 0], [N - 1, N - 1], [50, 50]]) if (!Number.isFinite(f(x, y, x / (N - 1), y / (N - 1), 0.5))) return `formula gave a non-number at x=${x}, y=${y}`;
    }
    return null;
  } catch (e) {
    return (e as Error).message;
  }
}

