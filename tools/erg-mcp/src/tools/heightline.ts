// Tool group "heightline": check_height_line, where a height line (Sandtide's sand line) falls against the spawns and the
// ground a worm can stand on. Inspection only; the box it reads is describe_level's landBounds.
import type { Vec3 } from "../core";
import { extentXZ, markers, waterMap, type Region } from "../lib/inspect-field";
import { heightLine, landBounds, lineWarnings, sideOf, type Side } from "../lib/height-line";
import { nodeAt, standing, surfaces, type ReachOptions } from "../lib/reach";
import { WORLD_PER_MAP, grids } from "../lib/voxel";
import { ToolError, type Tool } from "../registry";

const r2 = (v: number) => Math.round(v * 100) / 100;
/** Spots that are near the line are grouped on a grid this wide (map units). */
const CLUSTER = 4;
const BAND_WORLD = 40;

interface Args {
  preset?: "sandtide"; fraction?: number; y?: number; band?: number; water?: number; step?: number; limit?: number;
  x0?: number; z0?: number; x1?: number; z1?: number; units?: "map" | "world";
}

export const tools: Tool[] = [{
  name: "check_height_line",
  description: "Where a height line falls against the land: Sandtide's sand line (`preset: \"sandtide\"`, fraction 0.25), or any `fraction` of the land box above its base, or a fixed `y`. The land box is the engine's landscape box from the voxel frames (describe_level `landBounds`: min and max y and the frame that sets each end; frames with no solid voxel are left out). The base is the water at start when the land reaches under it (minY < water < maxY, the engine's own Water.Level: here the project's water level or `water`), else minY; the line is base + fraction x (maxY - base). Answers the line (map and world), the base and its kind, each spawn knot's height and ground and which side of the line it is (below: cursed by Sandtide; near: within `band`, default 40 world units, of the line; above), every standable surface sampled (a column every `step`) counted by side with its area and its lowest and highest, the near-the-line surfaces grouped in 4-unit cells (centre, heights, area; `limit` of them, default 20) and `flagged`: the knots and surface groups within the band. Warns when the water is at or under the land's bottom or over its top, when the line is outside the land box, and when every surface is on one side of it. Inspection only: the project is not changed. The box's top end is often one tall frame; its tip sets the line, so raise or lower that frame and ask again. Not the game's own number: the box here is the frames' boxes (a draped skin's box is its corner extremes), so compare with the in-game `sandtide: sand line at Y` log line and trust that.",
  inputSchema: {
    type: "object", additionalProperties: false,
    properties: {
      preset: { type: "string", enum: ["sandtide"], description: "sandtide: fraction 0.25 (Sandtide's LINE_FRACTION)." },
      fraction: { type: "number", minimum: 0.01, maximum: 1, description: "The line's height as a fraction of (maxY - base)." },
      y: { type: "number", description: "A fixed line height in the call's units instead of a fraction." },
      band: { type: "number", minimum: 0, description: "How close to the line counts as near (the call's units; default 40 world = 2 map units)." },
      water: { type: "number", description: "The water at start in the call's units, when the project keeps the level's own (describe_level water.map is null there): without it the base is minY." },
      step: { type: "number", minimum: 0.25, maximum: 16, description: "Column spacing for the surfaces (the call's units; default: the level fits in about 250000 columns, at least 1 map unit)." },
      limit: { type: "integer", minimum: 0, maximum: 200, description: "Near-the-line surface groups to list (default 20)." },
      x0: { type: "number", description: "Region west edge for the surfaces (default: the level's extent)." }, z0: { type: "number" }, x1: { type: "number" }, z1: { type: "number" },
      units: { type: "string", enum: ["map", "world"], description: "Unit of what you give and of the answer's positions: map (default) or world (map x 20). Heights are answered in both." },
    },
  },
  handler(s, a: Args) {
    const store = s.requireProject().store, gs = grids(store);
    const lb = landBounds(gs);
    if (!lb) throw new ToolError("the level has no voxel frame with ground in it");
    const given = [a.preset !== undefined, a.fraction !== undefined, a.y !== undefined].filter(Boolean).length;
    if (given !== 1) throw new ToolError("give one of preset (\"sandtide\"), fraction or y");
    const inMap = (v: number) => (a.units === "world" ? v / WORLD_PER_MAP : v);
    const out = (v: number) => r2(a.units === "world" ? v * WORLD_PER_MAP : v);
    const both = (v: number) => ({ map: r2(v), world: r2(v * WORLD_PER_MAP) });
    const water = a.water !== undefined ? inMap(a.water) : waterMap(store);
    const h = heightLine(lb, water, { fraction: a.preset === "sandtide" ? 0.25 : a.fraction, y: a.y === undefined ? undefined : inMap(a.y) });
    const band = a.band !== undefined ? inMap(a.band) : BAND_WORLD / WORLD_PER_MAP;
    const ext = extentXZ(gs)!;
    const reg: Region = { x0: a.x0 === undefined ? ext.x0 : inMap(a.x0), z0: a.z0 === undefined ? ext.z0 : inMap(a.z0),
      x1: a.x1 === undefined ? ext.x1 : inMap(a.x1), z1: a.z1 === undefined ? ext.z1 : inMap(a.z1) };
    if (reg.x1 - reg.x0 < 1e-6 || reg.z1 - reg.z0 < 1e-6) throw new ToolError("the region is empty");
    const step = a.step !== undefined ? inMap(a.step) : Math.max(1, Math.sqrt(((reg.x1 - reg.x0) * (reg.z1 - reg.z0)) / 250000));
    const cols = Math.ceil((reg.x1 - reg.x0) / step) * Math.ceil((reg.z1 - reg.z0) / step);
    if (cols > 1_000_000) throw new ToolError(`${cols} columns is too many (at most 1000000): use a larger step or a smaller region`);
    const o: ReachOptions = { step, maxStep: 0.5, maxSlope: 45, maxDrop: 8, maxGap: 0, headroom: 1.5, water: null };
    const S = surfaces(gs, reg, step, o.headroom);

    // surfaces: counts by side, and the near ones grouped
    const count: Record<Side, number> = { below: 0, near: 0, above: 0 };
    let lo = Infinity, hi = -Infinity;
    const groups = new Map<string, { n: number; x: number; z: number; lo: number; hi: number }>();
    for (let n = 0; n < S.nodes; n++) {
      const [x, y, z] = nodeAt(S, n);
      const side = sideOf(y, h.line, band);
      count[side]++;
      lo = Math.min(lo, y); hi = Math.max(hi, y);
      if (side !== "near") continue;
      const key = `${Math.floor(x / CLUSTER)},${Math.floor(z / CLUSTER)}`;
      const g = groups.get(key) ?? { n: 0, x: 0, z: 0, lo: Infinity, hi: -Infinity };
      g.n++; g.x += x; g.z += z; g.lo = Math.min(g.lo, y); g.hi = Math.max(g.hi, y);
      groups.set(key, g);
    }
    const area = step * step;
    const near = [...groups.values()].sort((p, q) => q.n - p.n).map((g) => ({ at: [out(g.x / g.n), out(g.z / g.n)], surfaceY: { min: both(g.lo), max: both(g.hi) }, area: r2(g.n * area), columns: g.n }));

    // spawn knots: the knot and the ground under it
    const spawns = markers(store).filter((m) => m.kind === "spawn").map((m) => {
      const p = m.pos as Vec3, n = standing(S, p, o, gs);
      const ground = n >= 0 ? nodeAt(S, n)[1] : null;
      const y = ground ?? p[1];
      const side = sideOf(y, h.line, band);
      return { name: m.name, at: [out(p[0]), out(p[2])], knotY: both(p[1]), groundY: ground === null ? null : both(ground), side, margin: both(y - h.line) };
    });
    const flagged = { spawns: spawns.filter((p) => p.side === "near").map((p) => ({ name: p.name, at: p.at, groundY: p.groundY ?? p.knotY })),
      surfaceGroups: near.length, surfaceArea: r2(count.near * area) };
    const warnings = lineWarnings(lb, water, h, S.nodes ? lo : null, S.nodes ? hi : null);
    if (flagged.spawns.length) warnings.push(`${flagged.spawns.length} spawn knot(s) within ${out(band)} ${a.units ?? "map"} units of the line: ${flagged.spawns.map((p) => p.name).join(", ")}`);
    const cap = a.limit ?? 20;
    const bySide = (side: Side) => spawns.filter((p) => p.side === side).length;
    return {
      line: both(h.line), base: { ...both(h.base), kind: h.baseKind }, fraction: h.fraction,
      landBounds: { minY: both(lb.minY), maxY: both(lb.maxY), minFrame: lb.minFrame, maxFrame: lb.maxFrame, emptyFramesSkipped: lb.emptySkipped },
      water: water === null ? "the level's own height (not known here; pass water): the base is minY" : both(water),
      band: both(band),
      spawns: { below: bySide("below"), near: bySide("near"), above: bySide("above"), list: spawns },
      surfaces: { step: r2(step), sampled: S.nodes, below: { columns: count.below, area: r2(count.below * area) }, near: { columns: count.near, area: r2(count.near * area) }, above: { columns: count.above, area: r2(count.above * area) },
        lowest: S.nodes ? both(lo) : null, highest: S.nodes ? both(hi) : null, nearGroups: near.slice(0, cap), nearGroupsListed: Math.min(cap, near.length) },
      flagged,
      ok: !flagged.spawns.length && !count.near,
      warnings,
      note: "inspection only; below the line is where Sandtide's curse lands, above is safe. The game's own line is in its log: `sandtide: sand line at Y`.",
    };
  },
}];
