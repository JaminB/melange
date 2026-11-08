// Tool group "terrain": sculpting with the panel's brushes (one dab or along a path), added blocks, clearing the base
// terrain, raw box fills and the painted surround. The engine is src/lib/terrain-ops.ts.
import { SURROUND_SIDE, cellBudget, type EditorStore, type Vec3 } from "../core";
import {
  addBlock, clearBase, compileFormula as compile, downhillPath, enableSurroundPaint, fillBox, frameGrids, patchLeft, pathPoints, pickAnchor, pointToMap, removeBlock,
  setSurroundGrid, stroke, surroundDabs, surroundSummary, taperSizes, type StrokeFilter, type Units,
} from "../lib/terrain-ops";
import { waterMap } from "../lib/inspect-field";
import { paintSurface, type Shape } from "../lib/surface-paint";
import { applyTrims, carveFrames, clearTrims, pickBaseFrames, planTrims, type TrimRow } from "../lib/trims";
import { grids, tidy, toWorld } from "../lib/voxel";
import { ToolError, type Schema, type Tool } from "../registry";

const vec = (description: string, integer = false): Schema => ({ type: "array", description, items: { type: integer ? "integer" : "number" }, minItems: 3, maxItems: 3 });
const units: Schema = { type: "string", enum: ["world", "map", "grid"], description: "Units of the coordinates: world (the Erg panel's, = map x 20; default), map (the level file's; 1 voxel = 1 map unit on an unscaled frame) or grid (the voxel cells of `frame`)." };
const dryRun: Schema = { type: "boolean", description: "Work out the change and report it without applying it." };
const material: Schema = { description: "Material 0-63 (the palette index). fill also takes 'column'.", type: "integer", minimum: 0, maximum: 63 };

const brushProps = {
  mode: { type: "string", enum: ["carve", "fill", "paint"], description: "carve clears solid voxels in every frame the brush overlaps (keeping their material bits); fill turns empty voxels solid with `material` (anchor frame only); paint changes the material of solid voxels (anchor frame only)." },
  shape: { type: "string", enum: ["box", "sphere"], description: "A voxel is inside when its centre is: a box, or the ellipsoid of the size (default sphere)." },
  size: vec("Brush size in voxels of the anchor frame, each 1-32, fractions allowed (a sphere of diameter 5 is [5,5,5]; 2.5 is the cells whose centre is within 1.25)."),
  material,
  materialColumn: { type: "boolean", description: "fill only: take each voxel's material from the highest solid voxel in its column of the frame (the panel's 'column' material); ignores `material`." },
  frame: { type: "integer", description: "Anchor frame id (describe_level). Fill and paint touch only this frame (unless allFrames); carve reaches every frame. Default: the frame holding the point (a solid voxel there first, then the smallest)." },
  allFrames: { type: "boolean", description: "Fill and paint also edit every other voxel frame the brush overlaps (rotated and scaled frames too, at their cell centres), as carve does. Default false: the anchor frame only, as the panel's brush." },
  units,
  snap: { type: "boolean", description: "Snap each dab's centre to the middle of its voxel, as the panel does (default true). false keeps fractional centres." },
  frames: { type: "array", items: { type: "integer" }, minItems: 1, maxItems: 256, description: "Edit only these frames (ids from describe_level): carve a road into a skin without touching the castle block beside it, or the reverse. The anchor is picked among them." },
  onlyBlocks: { type: "boolean", description: "Edit only blocks added in this project (heightfields, drapes, structures' blocks), never the base map's frames." },
  skipFrames: { type: "array", items: { type: "integer" }, minItems: 1, maxItems: 256, description: "Never edit these frames." },
  drape: { type: "string", enum: ["sheared", "grid"], description: "How a draped block (heightfield_drape's skin: corner offsets) is read: sheared (default) at the height the game draws and collides it, each column moved up by its corner offsets, so a dab on the skin's surface cuts the skin there; grid as its unsheared grid says (the Erg panel's brush, and erg-mcp before: a dab on the surface can miss the skin or punch a hole elsewhere in it)." },
  dryRun,
} as const;

interface Brushy {
  mode: "carve" | "fill" | "paint"; shape?: "box" | "sphere"; size: Vec3; material?: number; materialColumn?: boolean; frame?: number;
  allFrames?: boolean; units?: Units; snap?: boolean; dryRun?: boolean; frames?: number[]; onlyBlocks?: boolean; skipFrames?: number[]; drape?: "sheared" | "grid";
}

const filter = (a: Brushy): StrokeFilter => ({ frames: a.frames, onlyBlocks: a.onlyBlocks, skipFrames: a.skipFrames, shear: a.drape !== "grid" });

function spec(a: Brushy) {
  if (a.materialColumn && a.mode !== "fill") throw new ToolError("materialColumn is for fill only");
  return { mode: a.mode, shape: a.shape ?? "sphere", size: a.size, material: a.materialColumn ? ("column" as const) : a.material };
}

const gridUnitsNeedFrame = (a: { units?: Units; frame?: number }) => {
  if (a.units === "grid" && a.frame === undefined) throw new ToolError("units 'grid' needs `frame`");
};

const surfaceProps = {
  material: { ...material, description: "Material 0-63 to paint." },
  depth: { type: "number", minimum: 0.01, maximum: 1280, description: "How deep under the ground's top to paint, in `units` (default 1 map unit, 20 world: the top voxel of an unscaled frame; give a frame's voxel size for a scaled one)." },
  only: { type: "array", items: { type: "integer", minimum: 0, maximum: 63 }, maxItems: 64, description: "Repaint only voxels of these materials (e.g. the rock, not a castle's walls)." },
  except: { type: "array", items: { type: "integer", minimum: 0, maximum: 63 }, maxItems: 64, description: "Never repaint voxels of these materials." },
  units: { type: "string", enum: ["world", "map"], description: "world (default, the Erg panel's) or map units, for every position, length and the mask." },
  blend: { type: "boolean", description: "Paint the edge per corner, not per voxel: a surface voxel just outside the area whose corners reach into it takes the material as its second material on those corners (the game's per-corner blend, the panel's 2nd-material brush), so the game draws a gradient across the edge voxel instead of a step: a smooth lava flow on a skin instead of stepped red and black squares. With jitter, the edge wanders as well. Adds runs where it blends: Test the level." },
  falloff: { type: "number", minimum: 0, maximum: 640, description: "blend: corners up to this far past the edge (in `units`) still take the material, so the gradient spreads over that width (default 0: only corners inside the area)." },
  exclude: { type: "array", minItems: 1, maxItems: 64, description: "Never paint inside these shapes (circles, rectangles, polygons, in `units`), whatever else holds: grass kept off a crater's floor, a road stopped short of a courtyard.",
    items: { type: "object", additionalProperties: false, description: "A circle (x, z, radius), a rectangle (min, max: [x, z]) or a polygon (points: [[x, z], ...]).",
      properties: { x: { type: "number" }, z: { type: "number" }, radius: { type: "number", minimum: 0 },
        min: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 }, max: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 },
        points: { type: "array", items: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 }, minItems: 3, maxItems: 500 } } } },
  excludeMask: { type: "string", description: "Never paint where this expression is above 0 (the mask's variables x, z, r, theta about `centre`, h, y, d)." },
  dryRun,
} as const;
const shapeSchema: Schema = { type: "object", additionalProperties: false, description: "A circle (x, z, radius), a rectangle (min, max: [x, z]) or a polygon (points: [[x, z], ...]).",
  properties: { x: { type: "number" }, z: { type: "number" }, radius: { type: "number", minimum: 0 },
    min: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 }, max: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 },
    points: { type: "array", items: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 }, minItems: 3, maxItems: 500 } } };
type SurfaceArgs = { material: number; depth?: number; only?: number[]; except?: number[]; units?: "world" | "map"; dryRun?: boolean; blend?: boolean; falloff?: number;
  exclude?: Record<string, unknown>[]; excludeMask?: string };
/** paintSurface's exclusions from the call (map units). */
const excludeOf = (a: SurfaceArgs, k: number) => ({ ...(a.exclude ? { exclude: shapesToMap(a.exclude, k, "exclude") } : {}), ...(a.excludeMask ? { excludeMask: a.excludeMask } : {}) });
/** paintSurface's blend from the call (map units). */
const blendOf = (a: SurfaceArgs, k: number) => {
  if (a.falloff !== undefined && !a.blend) throw new ToolError("falloff is for blend: true");
  return a.blend ? { falloff: (a.falloff ?? 0) / k } : undefined;
};

function shapesToMap(list: Record<string, unknown>[] | undefined, k: number, what = "shapes"): Shape[] {
  return (list ?? []).map((sh, i) => {
    const has = (n: string) => sh[n] !== undefined;
    if (has("radius") && has("x") && has("z") && !has("min") && !has("points")) return { x: (sh.x as number) / k, z: (sh.z as number) / k, radius: (sh.radius as number) / k };
    if (has("min") && has("max") && !has("radius") && !has("points")) return { min: (sh.min as number[]).map((v) => v / k) as [number, number], max: (sh.max as number[]).map((v) => v / k) as [number, number] };
    if (has("points") && !has("radius") && !has("min")) return { points: (sh.points as number[][]).map((q) => [q[0] / k, q[1] / k] as [number, number]) };
    throw new ToolError(`${what}[${i}]: give x, z and radius (a circle), min and max (a rectangle) or points (a polygon)`);
  });
}

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
        : pickAnchor(store, gs, pointToMap(a.center, a.units ?? "world"), a.frame, a.mode === "fill", filter(a));
      const at = pointToMap(a.center, a.units ?? "world", anchor);
      return stroke(store, spec(a), [at], anchor, { snap: a.snap ?? true, dryRun: a.dryRun, allFrames: a.allFrames, filter: filter(a) });
    },
  },
  {
    name: "sculpt_path",
    description: "Drag a terrain brush along a polyline: dabs every `spacing` units from the first point to the last, each seeing the one before, all anchored on the frame the stroke started on (the first point's frame, or `frame`). Fill and paint stay inside that frame unless `allFrames`, carve reaches through every frame it overlaps. `taper` narrows (or widens) the brush along the path. `followSurface: \"downhill\"` makes the path itself: from the first point it walks downhill over the voxel ground (steepest descent, a step of `spacing`) until water, a pit, `maxLength` or `maxSteps`, and dabs along it at the ground (+ `depth`): a lava river or a stream bed; the answer gives the path. One undo step; refused as a whole if a frame would need over 2000 runs or the patch outgrow its budget.",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["mode", "size", "points"],
      properties: {
        ...brushProps,
        points: { type: "array", minItems: 1, maxItems: 500, items: vec("A path point."), description: "The path, as [x,y,z] points in `units`. followSurface: only the first is used, the start (the ground under it, or the top of the ground it is in)." },
        spacing: { type: "number", minimum: 0.1, description: "Distance between dabs in map units (default: half the smallest brush side, at least 1). followSurface: also the walk's step." },
        taper: { type: "number", minimum: 0.05, maximum: 4, description: "Brush size at the end of the path as a multiple of `size`, linear along the path's length (0.3 narrows to 30 %; never under 1 voxel). Default 1 (no taper)." },
        followSurface: { type: "string", enum: ["downhill"], description: "downhill: generate the path by walking downhill over the voxel ground from the first point (steepest of 16 directions, `spacing` a step; level ground is crossed straight on for up to 8 steps, it never climbs) and dab along it at the ground; stops on ground at or under the water, in a pit, at maxLength or maxSteps, or where the ground ends. Not with units grid. Use allFrames for fill and paint, since the walk crosses frames." },
        maxLength: { type: "number", minimum: 1, description: "followSurface: the longest walk in map units (plan length). Default 400." },
        maxSteps: { type: "integer", minimum: 1, maximum: 4000, description: "followSurface: the most steps of the walk. Default 1000." },
        depth: { type: "number", description: "followSurface: each dab's centre is this many map units above the ground's top (negative: into it). Default -0.5, the middle of an unscaled top voxel." },
      },
    },
    handler(s, a: Brushy & { points: Vec3[]; spacing?: number; taper?: number; followSurface?: "downhill"; maxLength?: number; maxSteps?: number; depth?: number }) {
      const { store } = s.requireProject();
      gridUnitsNeedFrame(a);
      const gs = frameGrids(store);
      const u = a.units ?? "world";
      const spacing = a.spacing ?? Math.max(1, Math.min(...a.size) / 2);
      const label = `${a.mode[0].toUpperCase()}${a.mode.slice(1)} terrain path`;
      if (!a.followSurface && (a.maxLength !== undefined || a.maxSteps !== undefined || a.depth !== undefined)) throw new ToolError("maxLength, maxSteps and depth are for followSurface");
      if (a.followSurface) {
        if (u === "grid") throw new ToolError("followSurface walks the level: give the start in world or map units");
        const walk = downhillPath(grids(store), pointToMap(a.points[0], u), { step: spacing, maxLength: a.maxLength ?? 400, maxSteps: a.maxSteps ?? 1000, water: waterMap(store) });
        const depth = a.depth ?? -0.5;
        const pts = walk.points.map((p) => [p[0], p[1] + depth, p[2]] as Vec3);
        // the anchor holds the start's top voxel (a point on the ground's top face is outside a frame whose top it is)
        const anchor = pickAnchor(store, gs, [pts[0][0], walk.points[0][1] - 0.25, pts[0][2]], a.frame, a.mode === "fill", filter(a));
        const dabs = pathPoints(pts, spacing);
        const sizes = a.taper !== undefined && a.taper !== 1 ? taperSizes(pts, spacing, dabs.length, a.size, a.taper) : undefined;
        const r = stroke(store, spec(a), dabs, anchor, { snap: a.snap ?? true, dryRun: a.dryRun, label, allFrames: a.allFrames, sizes, filter: filter(a) });
        const out = (p: Vec3) => tidy(u === "world" ? toWorld(p) : p);
        return {
          ...r, spacing, ...(a.taper !== undefined ? { taper: a.taper } : {}), pathLength: tidy([walk.length, 0, 0])[0],
          followSurface: { stop: walk.stop, steps: walk.points.length - 1, units: u, path: walk.points.map(out), start: out(walk.points[0]), end: out(walk.points[walk.points.length - 1]),
            drop: tidy([walk.points[0][1] - walk.points[walk.points.length - 1][1], 0, 0])[0] },
          ...(a.points.length > 1 ? { note: "followSurface used only the first point" } : {}),
        };
      }
      const anchor = u === "grid" ? pickAnchor(store, gs, [0, 0, 0], a.frame, false) : pickAnchor(store, gs, pointToMap(a.points[0], u), a.frame, a.mode === "fill", filter(a));
      const pts = a.points.map((p) => pointToMap(p, u, anchor));
      const dabs = pathPoints(pts, spacing);
      const sizes = a.taper !== undefined && a.taper !== 1 ? taperSizes(pts, spacing, dabs.length, a.size, a.taper) : undefined;
      const r = stroke(store, spec(a), dabs, anchor, { snap: a.snap ?? true, dryRun: a.dryRun, label, allFrames: a.allFrames, sizes, filter: filter(a) });
      return { ...r, spacing, ...(a.taper !== undefined ? { taper: a.taper } : {}), pathLength: pts.slice(1).reduce((n, p, i) => n + Math.hypot(p[0] - pts[i][0], p[1] - pts[i][1], p[2] - pts[i][2]), 0) };
    },
  },
  {
    name: "paint_path",
    description: "Paint a band along a path onto the ground's surface, over every voxel frame it crosses (base frames and added blocks alike): each solid voxel whose centre is within width / 2 of the path (in x, z) and within `depth` under the top of the ground in its column is repainted. The path's y is not used: it follows the surface below, and stretches over air paint nothing (no 'no voxel frame holds that point'). `taper` narrows it along its length, `jitter` makes the edge wander, `followSurface: \"downhill\"` makes the path itself from the first point (as sculpt_path's: a lava flow down a gully). A road or a lava river is one call. One undo step; refused as a whole when a frame would pass 2000 runs.",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["material", "points", "width"],
      properties: {
        ...surfaceProps,
        points: { type: "array", minItems: 1, maxItems: 2000, items: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 3 }, description: "The path, [x, z] or [x, y, z] points (y is not used, except followSurface's start). followSurface: the first is the start." },
        width: { type: "number", minimum: 0.01, description: "The band's width, in `units`." },
        taper: { type: "number", minimum: 0.05, maximum: 4, description: "The width at the end as a multiple of `width`, linear along the path (default 1)." },
        jitter: { type: "object", additionalProperties: false, required: ["amount"], description: "Noise on the band's edge: it moves in or out by up to `amount` (units), over features about `scale` across (units; default 8 map units).",
          properties: { amount: { type: "number" }, scale: { type: "number", minimum: 0.01 }, seed: { type: "integer" } } },
        followSurface: { type: "string", enum: ["downhill"], description: "Walk downhill from the first point over the voxel ground (as sculpt_path) and paint along that path." },
        spacing: { type: "number", minimum: 0.1, description: "followSurface: the walk's step in map units (default 1)." },
        maxLength: { type: "number", minimum: 1, description: "followSurface: the longest walk, map units (default 400)." },
      },
    },
    handler(s, a: SurfaceArgs & { points: number[][]; width: number; taper?: number; jitter?: { amount: number; scale?: number; seed?: number }; followSurface?: "downhill"; spacing?: number; maxLength?: number }) {
      const { store } = s.requireProject();
      const k = (a.units ?? "world") === "world" ? 20 : 1;
      const pts: Vec3[] = a.points.map((p) => (p.length === 2 ? [p[0] / k, 0, p[1] / k] : [p[0] / k, p[1] / k, p[2] / k]) as Vec3);
      let path = pts, walk: ReturnType<typeof downhillPath> | null = null;
      if (a.followSurface) {
        const start: Vec3 = a.points[0].length === 3 ? pts[0] : [pts[0][0], 1e6, pts[0][2]];
        walk = downhillPath(grids(store), start, { step: a.spacing ?? 1, maxLength: a.maxLength ?? 400, maxSteps: 4000, water: waterMap(store) });
        path = walk.points;
      } else if (a.spacing !== undefined || a.maxLength !== undefined) throw new ToolError("spacing and maxLength are for followSurface");
      const jitter = a.jitter ? { amount: a.jitter.amount / k, scale: (a.jitter.scale ?? 8 * k) / k, seed: a.jitter.seed } : undefined;
      const r = paintSurface(store, { material: a.material, depth: (a.depth ?? k) / k, path, width: a.width / k, taper: a.taper, jitter, only: a.only, except: a.except,
        blend: blendOf(a, k), ...excludeOf(a, k), units: a.units ?? "world", dryRun: a.dryRun, label: "Paint path" });
      const out = (p: Vec3) => tidy([p[0] * k, p[1] * k, p[2] * k] as Vec3);
      return { ...r, ...(walk ? { followSurface: { stop: walk.stop, steps: walk.points.length - 1, length: tidy([walk.length * k, 0, 0])[0], start: out(walk.points[0]), end: out(walk.points[walk.points.length - 1]) } } : {}), ...patchLeft(store) };
    },
  },
  {
    name: "paint_region",
    description: "Paint the ground's surface inside an area, over every voxel frame (base frames and added blocks): each solid voxel within `depth` under the top of the ground in its column whose centre is in one of `shapes` (circles, rectangles, polygons) and/or where `mask` is above 0. The mask is an expression in heightfield_region's language (noise2, fbm, smoothstep, step, lets, ...) of x, z (the level position in `units`), r, theta (about `centre`), h (the ground's top there), y (the voxel's height) and d (0 here). An ash band above 30 with ragged edges: `step(30 + 4*fbm(x/40, z/40, 3, 3), h) - 0.5`; a beach: `step(h, 4) - 0.5`. One undo step.",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["material"],
      properties: {
        ...surfaceProps,
        shapes: { type: "array", minItems: 1, maxItems: 64, items: shapeSchema, description: "Paint inside any of these (in `units`)." },
        mask: { type: "string", description: "Paint only where this expression is above 0 (variables x, z, r, theta, h, y, d, pi, e)." },
        centre: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2, description: "[x, z] the mask's r and theta are measured about (default 0, 0)." },
      },
    },
    handler(s, a: SurfaceArgs & { shapes?: Record<string, unknown>[]; mask?: string; centre?: [number, number] }) {
      const { store } = s.requireProject();
      const k = (a.units ?? "world") === "world" ? 20 : 1;
      if (!a.shapes && !a.mask) throw new ToolError("give shapes, a mask, or both");
      const r = paintSurface(store, { material: a.material, depth: (a.depth ?? k) / k, shapes: shapesToMap(a.shapes, k), mask: a.mask, centre: a.centre?.map((v) => v / k) as [number, number] | undefined,
        only: a.only, except: a.except, blend: blendOf(a, k), ...excludeOf(a, k), units: a.units ?? "world", dryRun: a.dryRun, label: "Paint region" });
      return { ...r, ...patchLeft(store) };
    },
  },
  {
    name: "paint_blob",
    description: "Paint an organic pool or patch onto the ground's surface (a lava lake, a pond of ash, a moss patch): a disc round `at` whose radius wanders with the bearing (`noise`: amount, the radius times 1 + amount x fbm round the circle; lobes, the bumps round it; seed), painted as paint_region paints (every voxel frame, `depth` under the top). The edge is soft by default: per corner (the game's per-corner blend, over `falloff`, default an eighth of the radius), so a voxel just outside whose corners reach into the blob takes the material as its second material on those corners and the shore is a gradient along the wandering outline instead of hard stairs of whole voxels. `blobs` paints several (each its own at, radius, noise) in one undo step. Answers what paint_region answers.",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["material"],
      properties: {
        ...surfaceProps,
        at: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 3, description: "The centre [x, z] (or [x, y, z], y unused), in `units`." },
        radius: { type: "number", minimum: 0.01, description: "The mean radius, in `units`." },
        noise: { type: "object", additionalProperties: false, description: "How the outline wanders: amount (0-0.9 of the radius, default 0.3), lobes (bumps round the circle, default 5), seed.",
          properties: { amount: { type: "number", minimum: 0, maximum: 0.9 }, lobes: { type: "number", minimum: 1, maximum: 40 }, seed: { type: "integer" } } },
        soft: { type: "boolean", description: "The per-corner edge (default true; false paints whole voxels only). The same switch as blend." },
        blobs: { type: "array", minItems: 1, maxItems: 32, description: "Several blobs instead of at, radius and noise: each {at, radius, noise}.",
          items: { type: "object", additionalProperties: false, required: ["at", "radius"], properties: {
            at: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 3 }, radius: { type: "number", minimum: 0.01 },
            noise: { type: "object", additionalProperties: false, properties: { amount: { type: "number", minimum: 0, maximum: 0.9 }, lobes: { type: "number", minimum: 1, maximum: 40 }, seed: { type: "integer" } } } } } },
      },
    },
    handler(s, a: SurfaceArgs & { at?: number[]; radius?: number; noise?: { amount?: number; lobes?: number; seed?: number }; soft?: boolean;
      blobs?: { at: number[]; radius: number; noise?: { amount?: number; lobes?: number; seed?: number } }[] }) {
      const { store } = s.requireProject();
      const k = (a.units ?? "world") === "world" ? 20 : 1;
      if (a.blobs && (a.at !== undefined || a.radius !== undefined || a.noise)) throw new ToolError("give at, radius and noise, or blobs, not both");
      if (!a.blobs && (a.at === undefined || a.radius === undefined)) throw new ToolError("give at and radius (one blob), or blobs");
      if (a.soft !== undefined && a.blend !== undefined && a.soft !== a.blend) throw new ToolError("soft and blend are the same switch: give one");
      const soft = a.soft ?? a.blend ?? true;
      if (!soft && a.falloff !== undefined) throw new ToolError("falloff is for the soft edge (soft: true)");
      const list = a.blobs ?? [{ at: a.at!, radius: a.radius!, noise: a.noise }];
      const shapes: Shape[] = list.map((b, i) => {
        const [x, z] = b.at.length === 3 ? [b.at[0], b.at[2]] : [b.at[0], b.at[1]];
        if (![x, z, b.radius].every(Number.isFinite)) throw new ToolError(`blobs[${i}]: at and radius are finite numbers`);
        return { x: x / k, z: z / k, radius: b.radius / k, wobble: { amount: b.noise?.amount ?? 0.3, lobes: b.noise?.lobes ?? 5, seed: b.noise?.seed ?? i + 1 } };
      });
      const blend = soft ? { falloff: a.falloff !== undefined ? a.falloff / k : Math.min(...list.map((b) => b.radius)) / k / 8 } : undefined;
      const r = paintSurface(store, { material: a.material, depth: (a.depth ?? k) / k, shapes, only: a.only, except: a.except, blend, ...excludeOf(a, k),
        units: a.units ?? "world", dryRun: a.dryRun, label: list.length > 1 ? `Paint ${list.length} blobs` : "Paint blob" });
      return { ...r, blobs: list.length, soft, ...(blend ? { falloff: tidy([blend.falloff * k, 0, 0])[0] } : {}), ...patchLeft(store) };
    },
  },
  {
    name: "add_block",
    description: "Add a terrain block: a new solid frame of size X x Y x Z voxels (each 1-31; at most 64 blocks per level) of one material, under the Scene frame, optionally scaled (`scale` map units per voxel, 1-4). Refused when the level's voxel frames would pass the game's 65535 cells (X*Y*Z of every voxel frame; describe_level limits.voxelCells). Its corner snaps to whole units. Give `at` (x and z are the block's centre, y its bottom: it rests on that point) or `center`. Shape it afterwards with sculpt (carve) or fill_box_raw. `fill` empty adds an empty block (a canvas), hollow a closed shell. Answers `frameId`, the new frame's id (negative until saved; also as `id`), and its bounds.",
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
        fill: { type: "string", enum: ["solid", "empty", "hollow"], description: "solid (default): every cell of `material`; empty: no solid cell (a canvas for sculpt fill, fill_box_raw or structures with place existing; the box costs its cells all the same); hollow: a closed shell `shell` voxels thick round an empty inside." },
        shell: { type: "integer", minimum: 1, maximum: 15, description: "fill hollow: the shell's thickness in voxels (default 1)." },
      },
    },
    handler(s, a: { size: Vec3; material: number; at?: Vec3; center?: Vec3; units?: "world" | "map"; scale?: number; dryRun?: boolean; fill?: "solid" | "empty" | "hollow"; shell?: number }) {
      const { store } = s.requireProject();
      return { ...addBlock(store, { ...a, units: a.units ?? "world" }), ...patchLeft(store) };
    },
  },
  {
    name: "remove_block",
    description: "Remove a terrain block added in this project (by its frame id from add_block or describe_level). Base map frames: clear_base_terrain carves their voxels, remove_base_frames takes them out of the cell count too.",
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
    name: "trim_base_frames",
    description: "Give back the game's voxel cells that base-map frames spend on empty space. The game counts X*Y*Z of every voxel frame, solid or empty, against 65 535, so a wiped base (clear_base_terrain) still spends all its cells (Multi.DinerMight: 11 652). A trim keeps only a box of a base frame's grid in the built level: `keep: 'solid'` (default) the box of its solid voxels, a frame with none one empty cell; `keep: 'empty'` trims only frames with no solid voxel. The project keeps the whole grid (every tool reads and edits it as before); the box is the patch's trimFrame op (erg-patch/2: a Melange without it refuses the save). Every solid voxel must stay in the box: filling outside it later makes save refuse until you trim again or `clear`. A frame holding a detail tied to a voxel (voxelPos) is skipped. A box with solid voxels moves the frame by its offset from the grid's centre and its details and child frames back, so nothing moves in the level. Choose frames by `frames`, `search`, `box` (frames whose voxel box overlaps it) or `all`. One undo step; `dryRun` reports the cells it would free. `clear: true` drops the trims of the chosen frames (or every trim).",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        frames: { type: "array", items: { type: "integer" }, minItems: 1, maxItems: 4096, description: "Base frame ids (describe_level)." },
        search: { type: "string", description: "Frames whose name contains this (any case)." },
        box: { type: "object", additionalProperties: false, required: ["min", "max"], properties: { min: vec("Box minimum corner."), max: vec("Box maximum corner.") }, description: "Frames whose voxel box overlaps this box." },
        units: { type: "string", enum: ["world", "map"], description: "Units of `box`: world (default) or map." },
        all: { type: "boolean", description: "Every base frame with voxels." },
        keep: { type: "string", enum: ["solid", "empty"], description: "solid (default): each frame to the box of its solid voxels, an empty one to one cell; empty: only frames with no solid voxel, to one cell." },
        clear: { type: "boolean", description: "Drop the trims of the chosen frames (every trim when none are chosen) instead." },
        dryRun,
      },
    },
    handler(s, a: { frames?: number[]; search?: string; box?: { min: Vec3; max: Vec3 }; units?: "world" | "map"; all?: boolean; keep?: "solid" | "empty"; clear?: boolean; dryRun?: boolean }) {
      const { store } = s.requireProject();
      const box = a.box && { min: pointToMap(a.box.min, a.units ?? "world"), max: pointToMap(a.box.max, a.units ?? "world") };
      const before = cellBudget(store.scene);
      if (a.clear) {
        const only = a.frames || a.search || a.box ? pickBaseFrames(store, { ...a, box }).map((g) => g.id) : undefined;
        const dropped = a.dryRun ? (store.scene.trims ?? []).map((t) => t.frame).filter((id) => !only || only.includes(id)) : clearTrims(store, only);
        return { cleared: dropped, cells: { before, after: a.dryRun ? null : cellBudget(store.scene) }, ...(a.dryRun ? { dryRun: true } : {}) };
      }
      const plan = planTrims(store, pickBaseFrames(store, { ...a, box }), a.keep === "empty");
      if (!a.dryRun && plan.rows.length) applyTrims(store, plan.rows, null, "Trim base frames");
      return trimAnswer(store, plan, before, !!a.dryRun);
    },
  },
  {
    name: "remove_base_frames",
    description: "Remove base-map voxel frames from the level: every voxel of the chosen frames is emptied (solid or not) and each is trimmed to one empty cell (trim_base_frames), so the terrain is gone and its cells are back for new blocks, at a cost of one cell a frame. The frame itself stays (its details, child frames and the base's references to it are untouched), which is how the game's own empty frames look. Choose by `frames`, `search`, `box` (frames whose voxel box overlaps it) or `all`; a frame holding a detail tied to a voxel (voxelPos) is skipped. One undo step; `dryRun` reports the cells it would free. Removing all of Multi.DinerMight's frames frees about 11 100 of its 11 652 cells.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        frames: { type: "array", items: { type: "integer" }, minItems: 1, maxItems: 4096, description: "Base frame ids (describe_level)." },
        search: { type: "string", description: "Frames whose name contains this (any case)." },
        box: { type: "object", additionalProperties: false, required: ["min", "max"], properties: { min: vec("Box minimum corner."), max: vec("Box maximum corner.") }, description: "Frames whose voxel box overlaps this box." },
        units: { type: "string", enum: ["world", "map"], description: "Units of `box`: world (default) or map." },
        all: { type: "boolean", description: "Every base frame with voxels." },
        dryRun,
      },
    },
    handler(s, a: { frames?: number[]; search?: string; box?: { min: Vec3; max: Vec3 }; units?: "world" | "map"; all?: boolean; dryRun?: boolean }) {
      const { store } = s.requireProject();
      const box = a.box && { min: pointToMap(a.box.min, a.units ?? "world"), max: pointToMap(a.box.max, a.units ?? "world") };
      const before = cellBudget(store.scene);
      const plan = planTrims(store, pickBaseFrames(store, { ...a, box }), false, true);
      const gs = grids(store).filter((g) => plan.rows.some((r) => r.id === g.id));
      const carve = carveFrames(store, gs);
      const emptied = carve.size;
      if (!a.dryRun && plan.rows.length) applyTrims(store, plan.rows, carve, "Remove base frames");
      return { ...trimAnswer(store, plan, before, !!a.dryRun), voxelsEmptied: emptied };
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

function trimAnswer(store: EditorStore, plan: { rows: TrimRow[]; skipped: { id: number; name: string; why: string }[] }, before: ReturnType<typeof cellBudget>, dryRun: boolean) {
  const freed = plan.rows.reduce((n, r) => n + r.cellsBefore - r.cellsAfter, 0);
  return {
    trimmed: plan.rows.length, cellsFreed: freed,
    cells: { before, after: dryRun ? { ...before, used: before.used - freed, free: before.free + freed } : cellBudget(store.scene) },
    frames: plan.rows.slice().sort((x, y) => (y.cellsBefore - y.cellsAfter) - (x.cellsBefore - x.cellsAfter)).slice(0, 60)
      .map((r) => ({ id: r.id, name: r.name, size: r.size, keep: { min: r.keep.min, size: r.keep.size }, solid: r.solid, cells: [r.cellsBefore, r.cellsAfter], ...(r.moves ? { moves: true } : {}) })),
    skipped: plan.skipped.slice(0, 60), skippedCount: plan.skipped.length,
    note: "save writes them (the patch's trimFrame ops); the game counts the kept boxes from the next Test, export or build",
    ...(dryRun ? { dryRun: true } : {}),
  };
}
