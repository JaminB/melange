// Tool group "structures": voxel architecture written into the open level as one undo step each. Every tool samples a
// shape list (lib/structures-shapes.ts) at the voxel cells it overlaps, so a structure lands in the existing frames and,
// where it reaches outside all of them, in new terrain blocks (lib/structures-apply.ts; the shared budget of 64 blocks).
import { cellBudget, type Vec3 } from "../core";
import { sampleField, spansAt } from "../lib/inspect-field";
import { standableAt, standing, surfaces } from "../lib/reach";
import { nearestTread, treadAt } from "../lib/spawn-check";
import { applyPlan, dominantMaterial, type Packing } from "../lib/structures-apply";
import {
  SIDES, arch, bridge, planBounds, castle, castleSpots, crater, flatTunnel, hasStyle, keep, lane, laneCut, place, placePoint, scaled, tower, tunnel, wallSegment, type Plan, type Shape,
  type GateSpec, type MatStyle, type Side, type Spot,
  castleTowers, TOWER_CORNERS, type CastleSpec, type RoofSpec, type TowerCorner, type TowerSpec,
} from "../lib/structures-shapes";
import { compound, tiersHeight, type CompoundSpec } from "../lib/structures-compound";
import { grids, tidy, toWorld } from "../lib/voxel";
import { ToolError, type Schema, type Tool } from "../registry";
import type { Session } from "../session";

// ------------------------------------------------------------------ schema pieces

const vec3 = (description: string): Schema => ({ type: "array", items: { type: "number" }, minItems: 3, maxItems: 3, description });
const vec2 = (description: string): Schema => ({ type: "array", items: { type: "number", minimum: 1 }, minItems: 2, maxItems: 2, description });
const sideSchema = (description: string): Schema => ({ type: "string", enum: [...SIDES, "none"], description });

const common: Record<string, Schema> = {
  material: { type: "integer", minimum: 0, maximum: 63, description: "Material index 0-63 (the level's palette). Default: the most common solid material near the structure." },
  place: {
    type: "string", enum: ["auto", "existing", "new"],
    description: "Where voxels go. auto (default): into the frames the structure overlaps, and into new terrain blocks where it reaches outside every frame. existing: only frames that exist (the rest is left out). new: every solid voxel goes into new blocks (carving still reaches the frames).",
  },
  dryRun: { type: "boolean", description: "Plan only: report the voxels, frames and new blocks it would use and change nothing." },
  units: { type: "string", enum: ["map", "world"], description: "Unit of every position and length in this call: map (default; 1 voxel = 1 unit) or world (map x 20, as the Erg panel shows)." },
  blockScale: { type: "number", minimum: 0.5, maximum: 4, description: "Map units per voxel of the new blocks (default 1): a larger one covers the same structure with fewer of the game's 65535 cells, in coarser voxels (detail under a voxel is lost). Use place \"new\" so the whole structure is on that lattice." },
  drape: { type: "string", enum: ["skip", "sheared"], description: "Draped blocks (heightfield_drape's skin, corner offsets): skip (default) leaves them alone; sheared edits their cells where the game draws them (each column moved up by its corner offsets), so a ramp cut, tunnel or crater cuts through the skin too. Fills into a skin follow its shear, a voxel at a time." },
  packing: { type: "string", enum: ["tiles", "parts", "fit"], description: "How new blocks are cut. tiles (default): tiles of up to 31 voxels a side, each trimmed to what it holds. parts: a block per part (each wall, tower, the keep), merged where that costs few cells, so the air between parts (a courtyard) is in no block: fewer cells, more blocks. fit: the tiles, then the box whose halving saves the most cells halved again while blocks are left (maxBlocks): a rotated structure's diagonal walls in boxes that follow them (a castle at 45 degrees takes a fraction of the cells of tiles)." },
  maxBlocks: { type: "integer", minimum: 1, maximum: 64, description: "Refuse a plan whose new blocks are more than this; packing fit splits up to it (default: the blocks the level has left)." },
  onSkin: { type: "boolean", description: "The structure stands on a draped skin on purpose (boulders bedded into dunes, a kerb, a low plinth): its new blocks are named ergonskin_N instead of ergblock_N, and save's and describe_level's drape.pokes advisory (a low undraped block poking out of a skin: a plug that should lie under it) leaves them out. Default false." },
  maxCells: { type: "integer", minimum: 1, maximum: 65535, description: "Refuse a plan whose new blocks hold more of the game's voxel cells than this (keep some of the 65535 for the rest of the level)." },
  costOnly: { type: "boolean", description: "Change nothing and answer only what the structure costs: its new blocks and the game's cells with each packing (tiles, parts, fit) at the call's blockScale, and at each costAt scale, and which fits the level's cells left. Cheaper to read than a dryRun." },
  costAt: { type: "array", items: { type: "number", minimum: 0.5, maximum: 4 }, minItems: 1, maxItems: 8, description: "Also plan the same structure all in new blocks (place \"new\") at each of these blockScales, changing nothing, and answer each one's blocks and the game's cells (costAt: [{blockScale, blocks, cells, fits}]), to choose a scale before building: a castle at 1.5 costs about 30 % of its cells at 1." },
  scale: { type: "number", minimum: 0.5, maximum: 4, description: "The same as blockScale (build_castle's name for it): map units per voxel of the new blocks." },
};
const foundation: Schema = { type: "integer", minimum: 0, maximum: 32, description: "A whole number of the structure's voxels (of `scale`, so 2 at scale 1.5 is 3 map units) of solid filled below the base (y) so the structure reaches the ground. Default 0. A castle's fills its whole footprint (a 24 x 20 castle with foundation 4: about 2000-3000 cells); seat fills under the walls and towers only." };
const rotation: Schema = { type: "number", description: "Yaw in degrees about the structure's centre (counter-clockwise seen from above; 0 = sides parallel to the map axes). Default 0." };
const crenellations = (d: string): Schema => ({ type: "boolean", description: d });
const matIdx: Schema = { type: "integer", minimum: 0, maximum: 63 };
const bandsSchema = (what: string): Schema => ({
  type: "array", minItems: 1, maxItems: 16,
  description: `Material by height: [{fromY, toY, material}], the first band holding a cell's height wins, the call's material elsewhere. Heights are above ${what}, from the first solid layer up (a foundation is below 0). fromY defaults to the bottom, toY (exclusive) to the top. Unset, nothing changes.`,
  items: { type: "object", additionalProperties: false, required: ["material"], properties: {
    fromY: { type: "number", description: "Height the band starts at. Default: below everything." }, toY: { type: "number", description: "Height it ends at (exclusive). Default: above everything." }, material: matIdx } },
});
const trimSchema = (what: string, cap: boolean): Schema => ({
  type: "object", additionalProperties: false,
  description: `Trim materials: top (the topmost layer of ${what}, topDepth thick, default 1), ${cap ? "merlonCap (the merlon row), " : ""}course ({y, material, height}: one stripe at height y above the floor, height thick, default 1). A course beats top and merlonCap, which beat bands. Unset, nothing changes. With a blockScale over 1 a layer thinner than a voxel can vanish: give topDepth or the course height a whole voxel.`,
  properties: {
    top: matIdx, topDepth: { type: "number", minimum: 0.01, description: "How thick top is. Default 1." },
    ...(cap ? { merlonCap: matIdx } : {}),
    course: { type: "object", additionalProperties: false, required: ["y", "material"], properties: { y: { type: "number" }, material: matIdx, height: { type: "number", minimum: 0.01, description: "Default 1." } } },
  },
});
interface BandIn { fromY?: number; toY?: number; material: number; }
interface TrimIn { top?: number; topDepth?: number; merlonCap?: number; course?: { y: number; material: number; height?: number }; }

/** bands and trim as a MatStyle in design voxels (k = voxels per call unit), every field checked; undefined when neither is given. */
function matStyle(a: { bands?: BandIn[]; trim?: TrimIn }, k: number, opts: { cap: boolean } = { cap: true }): MatStyle | undefined {
  const mat = (v: unknown, w: string) => {
    if (typeof v !== "number" || !Number.isInteger(v) || v < 0 || v > 63) throw new ToolError(`${w} is a material index 0-63`);
    return v;
  };
  const st: MatStyle = {};
  if (a.bands !== undefined) {
    if (!Array.isArray(a.bands) || a.bands.length > 16) throw new ToolError("bands is a list of at most 16 {fromY, toY, material}");
    st.bands = a.bands.map((b, i) => {
      for (const f of [b.fromY, b.toY]) if (f !== undefined && !Number.isFinite(f)) throw new ToolError(`bands[${i}]: fromY and toY are numbers`);
      if (b.fromY !== undefined && b.toY !== undefined && !(b.fromY < b.toY)) throw new ToolError(`bands[${i}]: fromY must be under toY`);
      return { from: b.fromY === undefined ? -Infinity : b.fromY * k, to: b.toY === undefined ? Infinity : b.toY * k, material: mat(b.material, `bands[${i}].material`) };
    });
  }
  if (a.trim !== undefined) {
    const t = a.trim, out: NonNullable<MatStyle["trim"]> = {};
    if (t === null || typeof t !== "object") throw new ToolError("trim is an object: {top, merlonCap, course}");
    if (t.top !== undefined) {
      out.top = mat(t.top, "trim.top");
      if (t.topDepth !== undefined) { if (!(t.topDepth > 0)) throw new ToolError("trim.topDepth is over 0"); out.topDepth = t.topDepth * k; }
    } else if (t.topDepth !== undefined) throw new ToolError("trim.topDepth goes with trim.top");
    if (t.merlonCap !== undefined) {
      if (!opts.cap) throw new ToolError("trim.merlonCap is for the tools with crenellations (castle, wall, tower, keep)");
      out.merlonCap = mat(t.merlonCap, "trim.merlonCap");
    }
    if (t.course !== undefined) {
      if (!Number.isFinite(t.course.y)) throw new ToolError("trim.course.y is a height");
      if (t.course.height !== undefined && !(t.course.height > 0)) throw new ToolError("trim.course.height is over 0");
      out.course = { y: t.course.y * k, material: mat(t.course.material, "trim.course.material"), ...(t.course.height !== undefined ? { h: t.course.height * k } : {}) };
    }
    st.trim = out;
  }
  return hasStyle(st) ? st : undefined;
}
/** What the answer says about a style (as asked, in the call's units). */
const styleInfo = (a: { bands?: BandIn[]; trim?: TrimIn }, st: MatStyle | undefined) =>
  (st ? { style: { ...(a.bands?.length ? { bands: a.bands.length } : {}), ...(a.trim ? { trim: a.trim } : {}) } } : {});
const CAP_NOTE = "trim.merlonCap: crenellations is off, so there are no merlons to cap";

interface RoofIn { style: "dome" | "onion" | "cone"; radius?: number; height?: number; material?: number; tipMaterial?: number; tipHeight?: number; }
const roofProps: Record<string, Schema> = {
  style: { type: "string", enum: ["dome", "onion", "cone"], description: "dome: a sphere cap (height up to the radius) or half an ellipsoid (taller); onion: a neck, a bulge a tenth wider than the radius a third of the way up, a taper to a point; cone: straight to a point." },
  radius: { type: "number", minimum: 0.5, description: "The roof's base radius. Default: the tower's radius, less the merlon ring (one voxel) when the tower is crenellated." },
  height: { type: "number", minimum: 0.5, description: "Default: the radius (dome), 1.6 x the radius (onion), 1.5 x the radius (cone)." },
  material: matIdx,
  tipMaterial: { ...matIdx, description: "Paints the topmost tipHeight of the roof (a finial)." },
  tipHeight: { type: "number", minimum: 0.01, description: "Default one voxel." },
};
const ROOF_NOTE = "A roof is built from the structure's own voxels (blockScale / scale): at 1.5 a radius of 4 is under 3 voxels and reads as stacked tiles. For a smooth dome build it apart with build_compound (kind dome) at a blockScale of 0.75 or less on the positions structureInfo.towers gives.";
const roofSchema = (what: string): Schema => ({ type: "object", additionalProperties: false, required: ["style"], description: `A roof on ${what}: a dome, onion dome or cone standing on the top floor. ${ROOF_NOTE} Unset, the top stays open (as before).`, properties: roofProps });

/** A roof in design voxels (k = voxels per call unit), the defaults filled in. */
function roofOf(r: RoofIn | undefined, k: number, towerRadius: number, cren: boolean, mat: number, what: string): RoofSpec | undefined {
  if (r === undefined) return undefined;
  if (r === null || typeof r !== "object" || !["dome", "onion", "cone"].includes(r.style)) throw new ToolError(`${what}: {style: "dome" | "onion" | "cone", radius, height, material, tipMaterial, tipHeight}`);
  const idx = (v: unknown, w: string) => { if (typeof v !== "number" || !Number.isInteger(v) || v < 0 || v > 63) throw new ToolError(`${what}.${w} is a material index 0-63`); return v; };
  const radius = r.radius === undefined ? Math.max(1, towerRadius - (cren ? 1 : 0)) : r.radius * k;
  if (!(radius > 0)) throw new ToolError(`${what}.radius is over 0`);
  const height = r.height === undefined ? radius * (r.style === "dome" ? 1 : r.style === "onion" ? 1.6 : 1.5) : r.height * k;
  if (!(height > 0)) throw new ToolError(`${what}.height is over 0`);
  return { style: r.style, radius, height, material: r.material === undefined ? mat : idx(r.material, "material"),
    ...(r.tipMaterial !== undefined ? { tip: idx(r.tipMaterial, "tipMaterial"), tipHeight: r.tipHeight === undefined ? 1 : r.tipHeight * k } : {}) };
}

const CASTLE_SCALE: Schema = { type: "number", minimum: 0.5, maximum: 4, description: "Build the castle from voxels of this many map units (default 1). Lengths stay map units; the design is laid out in voxels of `scale` (walls, merlons and gates keep their rhythm in voxels) and new blocks get that scale (blockScale, which is accepted as the same setting), so it costs about 1/scale^3 of the cells. Use place \"new\". blockScale alone keeps the design in voxels of 1 map unit and only puts its new blocks on that lattice (as before); build_wall, build_tower and the other structure tools take scale as another name for their blockScale." };

const SIDE_NOTE = "Sides are map directions: n = -z, s = +z, e = +x, w = -x (before the rotation).";

const groundProps: Record<string, Schema> = {
  ground: { type: "string", enum: ["max", "mean", "min"], description: "Set at's y from the ground under the footprint instead (the top of the highest solid voxel of every frame, draped skins at their sheared height, sampled every map unit over the rotated footprint): max (the highest point, so nothing of the floor is buried; fill the gap under the low side with foundation), mean or min. The answer's groundUnder gives all three." },
  sink: { type: "number", minimum: 0, description: "Lower the structure this far (units) under at's y, or under the ground with `ground`: bed it into a sloped or draped surface so its floor meets the ground instead of floating over a sheared skin's fraction of a voxel. Default 0." },
};

/** `at` with its y set from the ground under the footprint (half sizes hx, hz in the call's units, turned by the
 * rotation as place() turns the structure) and lowered by sink; and what was found there, in the call's units. */
function settle(s: Session, a: { at: Vec3; ground?: "max" | "mean" | "min"; sink?: number; units?: "map" | "world"; rotation?: number }, hx: number, hz: number):
  { at: Vec3; groundUnder?: Record<string, unknown> } {
  if (!a.ground) return { at: a.sink ? [a.at[0], a.at[1] - a.sink, a.at[2]] : a.at };
  const { store } = s.requireProject();
  const k = a.units === "world" ? 1 / 20 : 1, gs = grids(store);
  const cx = a.at[0] * k, cz = a.at[2] * k, ex = Math.max(0.5, hx * k), ez = Math.max(0.5, hz * k);
  const nx = Math.max(2, Math.ceil(2 * ex)), nz = Math.max(2, Math.ceil(2 * ez));
  const tops: number[] = [];
  for (let j = 0; j <= nz; j++)
    for (let i = 0; i <= nx; i++) {
      const p = placePoint([-ex + (2 * ex * i) / nx, 0, -ez + (2 * ez * j) / nz], [cx, 0, cz], a.rotation ?? 0);
      const top = spansAt(gs, p[0], p[2])[0];
      if (top) tops.push(top.top);
    }
  if (!tops.length) throw new ToolError("ground: no voxel ground under the footprint (give at's y yourself)");
  const min = Math.min(...tops), max = Math.max(...tops), mean = tops.reduce((n, v) => n + v, 0) / tops.length;
  const y = (a.ground === "min" ? min : a.ground === "mean" ? mean : max) - (a.sink ?? 0) * k;
  const out = (v: number) => tidy([v / k, 0, 0])[0];
  return { at: [a.at[0], y / k, a.at[2]], groundUnder: { min: out(min), max: out(max), mean: out(mean), samples: tops.length, of: (nx + 1) * (nz + 1), used: a.ground,
    ...(a.sink ? { sink: a.sink } : {}), y: out(y), note: tops.length < (nx + 1) * (nz + 1) ? "part of the footprint has no ground under it" : undefined } };
}

interface Common { material?: number; place?: "auto" | "existing" | "new"; dryRun?: boolean; units?: "map" | "world"; blockScale?: number; packing?: Packing; drape?: "skip" | "sheared"; costAt?: number[];
  maxBlocks?: number; maxCells?: number; costOnly?: boolean; scale?: number; onSkin?: boolean; }

/** `scale` as the other tools' name for blockScale (build_castle takes scale, build_wall and build_tower blockScale):
 * both are accepted on every structure tool; given both, they must agree. build_castle's own scale also lays its design
 * out in voxels of that size. */
function blockScaleOf(a: Common): number | undefined {
  if (a.scale !== undefined && a.blockScale !== undefined && Math.abs(a.scale - a.blockScale) > 1e-9) throw new ToolError(`scale (${a.scale}) and blockScale (${a.blockScale}) are the same setting here: give one`);
  return a.blockScale ?? a.scale;
}

const finite = (v: number[], what: string) => { if (!v.every(Number.isFinite)) throw new ToolError(`${what} must be finite numbers`); };

/** Scales every length the caller gave by the unit, resolves the material, plans and applies. */
function run(s: Session, a: Common, anchor: Vec3, structure: string, make: (k: number, mat: number, L: (v: number | undefined, d: number) => number) => Plan): unknown {
  const { store } = s.requireProject();
  const k = a.units === "world" ? 1 / 20 : 1;
  const at: Vec3 = [anchor[0] * k, anchor[1] * k, anchor[2] * k];
  const mat = a.material ?? dominantMaterial(store, at) ?? 1;
  const plan = make(k, mat, (v, d) => (v === undefined ? d : v * k));
  const bs = blockScaleOf(a);
  // each scale's cost first, while nothing has changed
  const costAt = a.costAt?.map((sc0) => {
    try {
      const d = applyPlan(store, plan, { place: "new", dryRun: true, blockScale: sc0, packing: a.packing, drape: a.drape, maxBlocks: a.maxBlocks });
      return { blockScale: sc0, blocks: d.newBlocks.length, cells: d.cells.newBlocks, fits: true };
    } catch (e) {
      return { blockScale: sc0, fits: false, why: e instanceof Error ? e.message : String(e) };
    }
  });
  if (a.costOnly) {
    // every packing at the call's own place and scale: nothing changes
    const free = cellBudget(store.scene).free;
    const byPacking = (["tiles", "parts", "fit"] as Packing[]).map((p) => {
      try {
        const d = applyPlan(store, plan, { place: a.place ?? "auto", dryRun: true, blockScale: bs, packing: p, drape: a.drape, maxBlocks: a.maxBlocks });
        return { packing: p, blocks: d.newBlocks.length, cells: d.cells.newBlocks, fits: a.maxCells === undefined || d.cells.newBlocks <= a.maxCells,
          voxels: { added: d.voxels.added, removed: d.voxels.removed } };
      } catch (e) {
        return { packing: p, fits: false, why: e instanceof Error ? e.message : String(e) };
      }
    });
    const ok = byPacking.filter((x) => x.fits && "cells" in x) as { packing: Packing; cells: number; blocks: number }[];
    const cheapest = ok.sort((x, y) => x.cells - y.cells)[0];
    return { structure, costOnly: true, blockScale: bs ?? 1, place: a.place ?? "auto", levelCellsFree: free, ...(a.maxCells !== undefined ? { maxCells: a.maxCells } : {}), byPacking,
      ...(cheapest ? { cheapest: { packing: cheapest.packing, cells: cheapest.cells, blocks: cheapest.blocks } } : { cheapest: null }), ...(costAt ? { costAt } : {}),
      structureInfo: plan.info, note: "nothing was changed; cells are the game's voxel cells (X x Y x Z of each new block) the structure's new blocks take, which must fit in levelCellsFree" };
  }
  const r = applyPlan(store, plan, { place: a.place ?? "auto", dryRun: !!a.dryRun, blockScale: bs, packing: a.packing, drape: a.drape, maxBlocks: a.maxBlocks, maxCells: a.maxCells, onSkin: a.onSkin });
  // a dig that fills nothing (no lining, no rim, no floor slab laid) has no material: the one found near it is not
  // reported as chosen
  const filled = plan.shapes.some((sh) => sh.op !== "carve") && r.voxels.added + r.voxels.repainted > 0;
  return { structure, ...(filled ? { material: mat, ...(a.material === undefined ? { materialNote: "chosen from the voxels near the structure; pass material to set it" } : {}) } : {}), ...r, ...(costAt ? { costAt } : {}),
    structureInfo: plan.info, undo: r.dryRun || !r.voxels.changed ? undefined : "undo reverts the whole structure in one step; save writes it" };
}

/** Offsets within 2 map units, nearest first (a quarter-unit grid). */
const NEAR: [number, number][] = (() => {
  const out: [number, number][] = [];
  for (let i = -8; i <= 8; i++) for (let j = -8; j <= 8; j++) if ((i || j) && i * i + j * j <= 64) out.push([i / 4, j / 4]);
  return out.sort((a, b) => a[0] ** 2 + a[1] ** 2 - b[0] ** 2 - b[1] ** 2);
})();

/** The castle's spawn spots in the caller's units; after a build, each checked against the voxels: the ground under it
 * within 1.5 map units of where the design puts it, with 1.5 map units of air above (a worm is about 1.2 tall). */
function spotReport(s: Session, spots: Spot[], units: "map" | "world" | undefined, dry: boolean) {
  const k = units === "world" ? 20 : 1;
  const gs = dry ? [] : grids(s.store);
  const list = spots.map((p) => {
    const out: Record<string, unknown> = { kind: p.kind, at: tidy([p.at[0] * k, p.at[1] * k, p.at[2] * k]) };
    if (dry) return out;
    // the one standability rule (lib/reach.ts standableAt), which check_reachability's start uses too
    const check = (x: number, z: number) => {
      const st = standableAt(gs, [x, p.at[1], z]);
      return { g: st.top === null ? undefined : { top: st.top }, ok: st.ok, why: st.why };
    };
    let x = p.at[0], z = p.at[2], c = check(x, z);
    // the design's point can fall between the voxels of a thin or scaled wall: the nearest point within 2 map units that
    // is ok instead
    if (!c.ok)
      for (const [dx, dz] of NEAR) {
        const t = check(p.at[0] + dx, p.at[2] + dz);
        if (t.ok) { x = p.at[0] + dx; z = p.at[2] + dz; c = t; out.at = tidy([x * k, p.at[1] * k, z * k]); out.moved = tidy([dx * k, 0, dz * k]); break; }
      }
    out.standOn = c.g ? tidy([x * k, c.g.top * k, z * k]) : null;
    out.ok = c.ok;
    if (!c.ok) out.why = !c.g ? "no ground under it" : Math.abs(c.g.top - p.at[1]) > 1.5 ? `the ground is at y ${tidy([c.g.top * k, 0, 0])[0]}, not the design's` : "no headroom";
    else {
      // what check_reachability will read: the same rule, on its graph of one column a map unit (the default step)
      const S = surfaces(gs, { x0: Math.floor(x) - 2, z0: Math.floor(z) - 2, x1: Math.floor(x) + 3, z1: Math.floor(z) + 3 }, 1, 1.5);
      if (standing(S, [x, p.at[1], z], { step: 1, maxStep: 0.5, maxSlope: 45, maxDrop: 8, maxGap: 0, headroom: 1.5, water: null }, gs) < 0) { out.ok = false; out.why = "a ledge too narrow for check_reachability's walk graph"; }
    }
    // the tread a spawn knot needs (lib/spawn-check.ts): not in solid, room over it, ground round it at least half a
    // worm out (a spot on a wall-walk's edge lets the worm slide off): snapped to the nearest point that passes
    if (out.ok) {
      const top = c.g!.top, t = treadAt(gs, [x, top, z]);
      if (!t.ok) {
        const to = nearestTread(gs, [x, top, z], 2);
        if (to && Math.abs(to[1] - top) <= 1.5) {
          out.tread = { snapped: true, from: tidy([x * k, top * k, z * k]), why: t.problems };
          x = to[0]; z = to[2];
          out.at = tidy([x * k, p.at[1] * k, z * k]);
          out.standOn = tidy([x * k, to[1] * k, z * k]);
        } else { out.ok = false; out.why = t.problems.join("; "); }
      }
    }
    return out;
  });
  return { note: dry ? "positions from the design (dry run: not checked against the voxels)" : "standOn is the top of the voxels there; ok spots have ground within 1.5 map units of the design and 1.5 of air above (moved: shifted that far to the nearest such point), are not in solid and have ground round them at least half a worm out (tread.snapped: moved off an edge to the nearest such point, within 2 map units). Give them to place_spawn_knots as pos.", spots: list };
}

const sideOf = (v: string | undefined, dflt: Side | null): Side | null => (v === undefined ? dflt : v === "none" ? null : (v as Side));

const sc = (v: Vec3, k: number): Vec3 => [v[0] * k, v[1] * k, v[2] * k];

/** build_castle seat: a fill under the castle's walls, towers and keep (the placed fill shapes of parts 1-9, tested on
 * the floor's first layer) from the ground up to the floor, where the ground is lower than the floor and at most
 * `depth` under it. Map units. Null when no column needs it. */
function seatShape(gs: ReturnType<typeof grids>, shapes: Shape[], floor: number, v: number, depth: number, mat: number): Shape | null {
  const solid = shapes.filter((sh) => sh.op === "fill" && sh.part !== undefined && sh.part >= 1);
  if (!solid.length) return null;
  const probe = floor + 0.5 * v;
  const min: Vec3 = [Infinity, floor - depth, Infinity], max: Vec3 = [-Infinity, floor, -Infinity];
  for (const sh of solid) { min[0] = Math.min(min[0], sh.min[0]); min[2] = Math.min(min[2], sh.min[2]); max[0] = Math.max(max[0], sh.max[0]); max[2] = Math.max(max[2], sh.max[2]); }
  const cache = new Map<string, number>();
  const groundAt = (x: number, z: number) => {
    const key = `${x},${z}`;
    let g = cache.get(key);
    if (g === undefined) {
      const top = spansAt(gs, x, z, floor)[0]?.top;
      g = top === undefined || top < floor - depth || !solid.some((sh) => x >= sh.min[0] && x < sh.max[0] && z >= sh.min[2] && z < sh.max[2] && sh.hit(x, probe, z)) ? Infinity : top;
      cache.set(key, g);
    }
    return g;
  };
  let any = false;
  for (let x = Math.floor(min[0]) + 0.5; x < max[0] && !any; x += 2) for (let z = Math.floor(min[2]) + 0.5; z < max[2] && !any; z += 2) if (groundAt(x, z) < floor - 0.25) any = true;
  if (!any) return null;
  return { op: "fill", mat, min, max, part: 0, keep: true, hit: (x, y, z) => y < floor && y >= groundAt(x, z) - 0.5 };
}

// ------------------------------------------------------------------ the gate lane

/** build_castle's gateLane, in design voxels: a lane `width` wide from the first gate straight across the courtyard,
 * kept clear of the keep by moving the keep aside (its door turned to the lane) or the gate along its wall into the band
 * beside the keep. Across is the gate wall's direction (x for a north or south gate, z for east or west); "right" is
 * the right hand of a worm walking in through the gate. */
function gateLaneFor(a: { gateLane?: boolean | number | { width?: number; mode?: "auto" | "keep" | "gate"; side?: "left" | "right" } },
  c: { gate: Side | null; size: [number, number]; t: number; tr: number; keepSize: [number, number] | null; gateWidth: number; shift0: number;
    U: (x: number | undefined, d: number) => number; cu: (d: number) => string }) {
  if (!c.gate) throw new ToolError("gateLane runs from the gate: the castle has none");
  const o = typeof a.gateLane === "object" ? a.gateLane : {};
  const width = typeof a.gateLane === "number" ? c.U(a.gateLane, 0) : c.U(o.width, c.gateWidth + 2);
  if (width < c.gateWidth - 1e-9) throw new ToolError(`gateLane: the lane (${c.cu(width)}) is narrower than the gate (${c.cu(c.gateWidth)})`);
  const ai = c.gate === "n" || c.gate === "s" ? 0 : 1;
  const right = c.gate === "s" || c.gate === "w" ? 1 : -1;            // the across sign of a walker's right hand
  const hw = c.size[ai] / 2, ia = hw - c.t;                            // half the wall's length, half the courtyard's across
  const towerEdge = hw - c.t / 2 - c.tr;                               // where a corner tower starts along the gate's wall
  const info = (mode: string, centre: number, side?: "left" | "right") => ({ mode, width, centre, ...(side ? { side } : {}) });
  if (!c.keepSize) return { info: info("open courtyard: no keep to keep clear of", c.shift0) };
  const ka = c.keepSize[ai] / 2, room = ia - 2;                        // the keep stays 2 voxels clear of the walls
  if (Math.abs(c.shift0) + width / 2 > ia + 1e-9) throw new ToolError(`gateLane: a ${c.cu(width)} lane at the gate's offset does not fit the ${c.cu(2 * ia)} courtyard`);
  const sideName = (sg: number) => (sg === right ? "right" : "left") as "left" | "right";
  const want = o.side === undefined ? null : o.side === "right" ? right : -right;
  const mode = o.mode ?? "auto";
  // the keep moved aside: its near face at the lane's edge, on the side asked for or the roomier one that fits
  const keepFits = (sg: number) => Math.abs(c.shift0 + sg * (width / 2 + ka)) + ka <= room + 1e-9;
  if (mode !== "gate") {
    const sides = want !== null ? [want] : [1, -1].sort((p, q) => (room - (q * c.shift0 + width / 2 + 2 * ka)) - (room - (p * c.shift0 + width / 2 + 2 * ka)));
    const sg = sides.find(keepFits);
    if (sg !== undefined) {
      const along = c.shift0 + sg * (width / 2 + ka);
      const keepAt: [number, number] = ai === 0 ? [along, 0] : [0, along];
      const keepDoor: Side = ai === 0 ? (sg > 0 ? "w" : "e") : (sg > 0 ? "n" : "s");
      return { keepAt, keepDoor, info: info("keep moved aside", c.shift0, sideName(sg)) };
    }
    if (mode === "keep") throw new ToolError(`gateLane: the keep (${c.cu(2 * ka)} across) does not fit beside a ${c.cu(width)} lane in the ${c.cu(2 * ia)} courtyard (mode keep): narrow the lane or the keep, or use mode gate`);
  }
  // the gate moved along its wall into the band beside the centred keep
  if (typeof a.gateLane === "object" && o.mode === "gate" && c.shift0) throw new ToolError("gateLane mode gate places the gate itself: give no offset for it");
  const band = ia - ka;
  const sides = want !== null ? [want] : [right, -right];
  for (const sg of sides) {
    const shift = sg * (ka + band / 2);
    if (band + 1e-9 < width || Math.abs(shift) + c.gateWidth / 2 > towerEdge + 1e-9) continue;
    return { gateShift: shift, info: info("gate moved beside the keep", shift, sideName(sg)) };
  }
  throw new ToolError(`gateLane: neither fits: the keep (${c.cu(2 * ka)} across) leaves bands of ${c.cu(band)} beside it for a ${c.cu(width)} lane, and moving it aside needs ${c.cu(width + 2 * ka + 2)} of the ${c.cu(2 * ia)} courtyard; make the castle wider, the keep or the lane narrower`);
}


// ------------------------------------------------------------------ build_compound

const COMPOUND_KINDS = ["box", "cylinder", "cone", "dome", "tiers"] as const;
interface CompoundEntry { kind: (typeof COMPOUND_KINDS)[number]; carve?: boolean; at?: Vec3; material?: number; part?: number; bands?: { from?: number; to?: number; fromY?: number; toY?: number; material: number }[]; trim?: TrimIn;
  size?: [number, number]; height?: number; radius?: number; topRadius?: number; profile?: "round" | "onion"; steps?: number; riser?: number; ledge?: number; round?: boolean; }

const COMPOUND_FIELDS: Record<string, readonly string[]> = {
  box: ["size", "height"], cylinder: ["radius", "height"], cone: ["radius", "height", "topRadius"], dome: ["radius", "height", "profile"],
  tiers: ["steps", "riser", "ledge", "round", "size", "radius"],
};

/** The list's entries as specs in map units (k = map units per call unit), every field checked for its kind. */
function compoundSpecs(list: CompoundEntry[], k: number, mat: number, dflt: { bands?: BandIn[]; trim?: TrimIn } = {}): CompoundSpec[] {
  const dStyle = matStyle(dflt, k, { cap: false });
  const pos = (v: unknown, what: string, i: number) => {
    if (typeof v !== "number" || !Number.isFinite(v) || v <= 0) throw new ToolError(`shapes[${i}] (${list[i].kind}): ${what} is required and must be over 0`);
    return v * k;
  };
  return list.map((e, i) => {
    if (!COMPOUND_FIELDS[e.kind]) throw new ToolError(`shapes[${i}]: kind must be one of ${COMPOUND_KINDS.join(", ")}`);
    const own = new Set(COMPOUND_FIELDS[e.kind]);
    for (const f of Object.keys(COMPOUND_FIELDS).flatMap((n) => COMPOUND_FIELDS[n])) if (!own.has(f) && (e as unknown as Record<string, unknown>)[f] !== undefined)
      throw new ToolError(`shapes[${i}]: ${f} is not a setting of a ${e.kind} (a ${e.kind} takes ${[...own].join(", ")})`);
    const at: Vec3 = e.at ? [e.at[0] * k, e.at[1] * k, e.at[2] * k] : [0, 0, 0];
    if (!at.every(Number.isFinite)) throw new ToolError(`shapes[${i}]: at must be finite numbers`);
    if (e.carve && e.bands?.length) throw new ToolError(`shapes[${i}]: bands paint a fill's material: a carve has none`);
    const c: CompoundSpec = { kind: e.kind, carve: !!e.carve, at, material: e.material ?? mat, ...(e.part !== undefined ? { part: e.part } : {}) };
    // a shape's band takes from/to, or fromY/toY as the call's bands spell them
    if (e.bands?.length) c.bands = e.bands.map((b, j) => {
      if ((b.from !== undefined && b.fromY !== undefined) || (b.to !== undefined && b.toY !== undefined))
        throw new ToolError(`shapes[${i}].bands[${j}]: give from or fromY (and to or toY), not both`);
      const from = b.from ?? b.fromY, to = b.to ?? b.toY;
      if (from !== undefined && to !== undefined && !(from < to)) throw new ToolError(`shapes[${i}].bands[${j}]: from must be under to`);
      return { from: (from ?? 0) * k, to: to === undefined ? Infinity : to * k, material: b.material };
    });
    else if (dStyle?.bands && !e.carve) c.bands = dStyle.bands;
    // the call's bands and trim are for every fill; a shape's own trim replaces the call's
    if (e.trim !== undefined && e.carve) throw new ToolError(`shapes[${i}]: trim paints a fill's material: a carve has none`);
    const trim = e.trim !== undefined ? matStyle({ trim: e.trim }, k, { cap: false })?.trim : dStyle?.trim;
    if (trim && !e.carve) c.trim = trim;
    if (e.kind === "box") {
      if (!e.size || !e.size.every((v) => Number.isFinite(v) && v > 0)) throw new ToolError(`shapes[${i}] (box): size [x, z] is required and must be over 0`);
      c.size = [e.size[0] * k, e.size[1] * k]; c.height = pos(e.height, "height", i);
    } else if (e.kind === "cylinder") { c.radius = pos(e.radius, "radius", i); c.height = pos(e.height, "height", i); }
    else if (e.kind === "cone") {
      c.radius = pos(e.radius, "radius", i); c.height = pos(e.height, "height", i);
      if (e.topRadius !== undefined) {
        if (!(e.topRadius >= 0)) throw new ToolError(`shapes[${i}] (cone): topRadius must be 0 or more`);
        c.topRadius = e.topRadius * k;
      }
    } else if (e.kind === "dome") { c.radius = pos(e.radius, "radius", i); c.height = pos(e.height ?? e.radius, "height", i); c.profile = e.profile ?? "round"; }
    else {
      c.round = !!e.round;
      if (c.round) c.radius = pos(e.radius, "radius (round tiers)", i);
      else {
        if (e.radius !== undefined) throw new ToolError(`shapes[${i}] (tiers): radius is for round tiers; give size`);
        if (!e.size || !e.size.every((v) => Number.isFinite(v) && v > 0)) throw new ToolError(`shapes[${i}] (tiers): size [x, z] of the base step is required and must be over 0`);
        c.size = [e.size[0] * k, e.size[1] * k];
      }
      if (!Number.isInteger(e.steps) || e.steps! < 1 || e.steps! > 64) throw new ToolError(`shapes[${i}] (tiers): steps is a whole number 1-64`);
      c.steps = e.steps; c.riser = pos(e.riser, "riser", i);
      if (typeof e.ledge !== "number" || !(e.ledge >= 0)) throw new ToolError(`shapes[${i}] (tiers): ledge (how far each step is inset on every side) is required and 0 or more`);
      c.ledge = e.ledge * k;
      const half = c.round ? c.radius! : Math.min(c.size![0], c.size![1]) / 2;
      if (c.ledge * (c.steps! - 1) >= half) throw new ToolError(`shapes[${i}] (tiers): ${c.steps} steps inset ${e.ledge} on each side leave nothing of the top step (the base is ${tidy([(half * 2) / k, 0, 0])[0]} across): fewer steps or a smaller ledge`);
      c.height = tiersHeight({ steps: c.steps!, riser: c.riser });
    }
    return c;
  });
}

// ------------------------------------------------------------------ tools

export const tools: Tool[] = [
  {
    name: "build_castle",
    description: "Build a castle: a rectangular curtain wall with a crenellated walkway, a tower on each corner, an optional keep in the courtyard and a gate. `at` is the centre of the footprint at floor level (y = the first solid layer; the courtyard is left as it is, use `foundation` to fill it). `scale` builds it from larger voxels (fewer cells); `packing: \"parts\"` keeps the courtyard air out of the blocks. Answers spawnSpots: open courtyard, wall-walk and tower-top positions, checked against the voxels. One undo step. " + SIDE_NOTE,
    inputSchema: {
      type: "object", additionalProperties: false, required: ["at"],
      properties: {
        at: vec3("Centre of the footprint [x, y, z]; y is the floor level the walls start from."),
        size: vec2("Outer size [x, z] of the curtain wall. Default [30, 30]."),
        wallHeight: { type: "number", minimum: 2, description: "Wall height above the floor. Default 6." },
        wallThickness: { type: "number", minimum: 1, description: "Wall thickness. 3 or more leaves a walkway behind the parapet. Default 3." },
        towerShape: { type: "string", enum: ["round", "square"], description: "Default round." },
        towerRadius: { type: "number", minimum: 2, description: "Tower radius (half the side when square). Default max(3, wallThickness + 1)." },
        towerHeight: { type: "number", minimum: 2, description: "Default wallHeight + 3." },
        keep: { type: "boolean", description: "Build a keep in the courtyard. Default true when it fits." },
        keepSize: vec2("Keep outer size [x, z] (with keepAxes \"gate\": [across, deep], across along the gate's wall and deep away from it). Default a third of the castle, at least 8."),
        keepAxes: { type: "string", enum: ["xz", "gate"], description: "How keepSize is read: xz (default: the castle's own x and z, before rotation, as size) or gate (relative to the first gate: [across, deep], across along the gate's wall, deep from it into the courtyard), so a keep is sized along and across the castle's gate axis whatever side the gate is on." },
        gateLane: { anyOf: [{ type: "boolean" }, { type: "number", minimum: 1 }, { type: "object", additionalProperties: false, properties: {
          width: { type: "number", minimum: 1 }, mode: { type: "string", enum: ["auto", "keep", "gate"] }, side: { type: "string", enum: ["left", "right"] } } }],
          description: "A clear lane from the first gate straight across the courtyard beside the keep (true: the gate's width + 2; a number: its width; or {width, mode, side}). mode keep moves the keep aside (to `side`, left or right looking in through the gate, default the roomier) so the lane runs past it, and turns its door to the lane; mode gate keeps the keep where it is and moves the gate along its wall into the band beside the keep that is wide enough; auto (default) moves the keep when it fits, else the gate. Refused when neither fits. The answer's lane gives what was done." },
        keepHeight: { type: "number", minimum: 0, description: "Default wallHeight + 4; at least 3 when a keep is built (ignored with keep: false)." },
        gate: sideSchema("Side of the gate, or none. Default s."),
        gateWidth: { type: "number", minimum: 2, description: "Default 4." },
        gateHeight: { type: "number", minimum: 2, description: "Default 5 (round-topped from 3 wide)." },
        gateBelow: { anyOf: [{ type: "number", minimum: 0 }, { type: "string", enum: ["auto"] }], description: "Carve the gate's floor this far (units) under the floor at its outer face, rising to the floor at the inner face: a ramp down to lower ground outside instead of a sill. auto: the floor less the ground 1.5 map units outside the gate's middle, at most the passage's length (45 degrees). Default auto with `ground`, else 0." },
        gates: { type: "array", minItems: 1, maxItems: 8, description: "Several gates instead of `gate`: each {side, width, height (defaults gateWidth, gateHeight), offset (along the side from its middle, default 0), below (as gateBelow; default auto), bevel (a canted head instead of the round or square one: the two top corners cut at 45 degrees, this far in from each; not for a gate cut open to the wall's top)}. The keep's door faces the first.",
          items: { type: "object", additionalProperties: false, required: ["side"], properties: {
            side: { type: "string", enum: [...SIDES] }, width: { type: "number", minimum: 2 }, height: { type: "number", minimum: 2 }, offset: { type: "number" }, bevel: { type: "number", minimum: 0 },
            below: { anyOf: [{ type: "number", minimum: 0 }, { type: "string", enum: ["auto"] }] } } } },
        seat: { type: "boolean", description: "Seat it on the ground: fill under the walls, towers and keep (not the courtyard) from the ground up to the floor, column by column, where the ground is lower than the floor (down to seatDepth). The cheap replacement for a whole-footprint foundation on a slope." },
        seatDepth: { type: "number", minimum: 0.5, description: "The deepest seat fill under the floor (units; default 8 map units): columns with no ground that close are left open." },
        crenellations: crenellations("Merlons on the walls, towers and keep. Default true."),
        bands: bandsSchema("the floor, on the foundation, walls, towers and keep alike"),
        trim: trimSchema("the walls, towers and keep", true),
        towerRoof: { ...roofSchema("each corner tower"), properties: { ...roofProps,
          only: { type: "array", items: { type: "string", enum: ["nw", "sw", "ne", "se"] }, minItems: 1, maxItems: 4, description: "Roof only these corner towers (before the rotation: n = -z, w = -x). Default all four." },
          materials: { type: "object", additionalProperties: false, description: "A roof material per corner, over `material`: {nw, sw, ne, se}.", properties: { nw: matIdx, sw: matIdx, ne: matIdx, se: matIdx } } } },
        foundation, rotation, ...groundProps, ...common, scale: CASTLE_SCALE,
      },
    },
    handler(s, a0: Common & { at: Vec3; size?: [number, number]; wallHeight?: number; wallThickness?: number; towerShape?: "round" | "square"; towerRadius?: number;
      towerHeight?: number; keep?: boolean; keepSize?: [number, number]; keepHeight?: number; gate?: string; gateWidth?: number; gateHeight?: number;
      keepAxes?: "xz" | "gate"; gateLane?: boolean | number | { width?: number; mode?: "auto" | "keep" | "gate"; side?: "left" | "right" };
      gateBelow?: number | "auto"; gates?: { side: Side; width?: number; height?: number; offset?: number; below?: number | "auto" }[]; seat?: boolean; seatDepth?: number;
      crenellations?: boolean; foundation?: number; rotation?: number; scale?: number; ground?: "max" | "mean" | "min"; sink?: number; bands?: BandIn[]; trim?: TrimIn;
      towerRoof?: RoofIn & { only?: TowerCorner[]; materials?: Partial<Record<TowerCorner, number>> } }) {
      finite(a0.at, "at");
      const sz0 = a0.size ?? [30 * (a0.units === "world" ? 20 : 1), 30 * (a0.units === "world" ? 20 : 1)];
      const settled = settle(s, a0, sz0[0] / 2, sz0[1] / 2), a = { ...a0, at: settled.at };
      // scale v: the design is made in voxels of v map units (every length divided by v) and scaled back up once planned
      const v = a.scale ?? 1;
      let spots: Spot[] = [];
      const res = run(s, { ...a, scale: undefined, blockScale: a.blockScale ?? (v !== 1 ? v : undefined) }, a.at, "castle", (k0, mat) => {
        const k = k0 / v, U = (x: number | undefined, d: number) => (x === undefined ? d : x * k);
        // a design length (voxels of v) in the call's units, for the messages
        const un = a.units === "world" ? "world" : "map", cu = (d: number) => `${tidy([(d * v) / k0, 0, 0])[0]} ${un} units`;
        const size: [number, number] = a.size ? [a.size[0] * k, a.size[1] * k] : [30 / v, 30 / v];
        const wh = U(a.wallHeight, 6 / v), t = U(a.wallThickness, 3 / v);
        const tr = a.towerRadius === undefined ? Math.max(3 / v, t + 1) : a.towerRadius * k, notes: string[] = [];
        if (size[0] < 2 * tr + 4 || size[1] < 2 * tr + 4)
          throw new ToolError(`the castle is too small for its towers: each side needs at least ${cu(2 * tr + 4)} (2 x tower radius + 4 voxels${v !== 1 ? ` of ${v} map units` : ""}); size is ${cu(size[0])} x ${cu(size[1])}`);
        if (size[0] <= 2 * t + 4 || size[1] <= 2 * t + 4)
          throw new ToolError(`the walls leave no courtyard: each side must be over ${cu(2 * t + 4)} (2 x wallThickness + 4 voxels); size is ${cu(size[0])} x ${cu(size[1])}: make the castle bigger or the walls thinner`);
        let keepSize: [number, number] | null = null;
        if (a.keep !== false) {
          if (a.keepHeight !== undefined && a.keepHeight * k < 3 / v - 1e-9) throw new ToolError(`keepHeight is at least ${cu(3 / v)} when a keep is built (keep: false builds none)`);
          keepSize = a.keepSize ? [a.keepSize[0] * k, a.keepSize[1] * k] : [Math.max(8 / v, Math.round(size[0] / 3)), Math.max(8 / v, Math.round(size[1] / 3))];
          // keepAxes gate: [across, deep] of the first gate; a gate on the east or west wall runs across along z
          const g0 = a.gates?.length ? a.gates[0].side : sideOf(a.gate, "s");
          if (a.keepAxes === "gate" && a.keepSize && (g0 === "e" || g0 === "w")) keepSize = [keepSize[1], keepSize[0]];
          if (keepSize[0] > size[0] - 2 * t - 4 || keepSize[1] > size[1] - 2 * t - 4) {
            if (a.keep === true || a.keepSize) throw new ToolError(`the keep (${cu(keepSize[0])} x ${cu(keepSize[1])}) does not fit in the courtyard (${cu(size[0] - 2 * t - 4)} x ${cu(size[1] - 2 * t - 4)} free)`);
            notes.push("no keep: the courtyard is too small for one");
            keepSize = null;
          }
        }
        const gate = a.gates?.length ? a.gates[0].side : sideOf(a.gate, "s");
        if (a.gates && a.gate !== undefined) throw new ToolError("give gate or gates, not both");
        if (a.keepAxes === "gate" && !gate) throw new ToolError("keepAxes \"gate\" reads keepSize from the gate: the castle has none");
        // the gate lane: the keep moved aside, or the gate moved along its wall into the band beside the keep
        const laneOut = a.gateLane !== undefined && a.gateLane !== false ? gateLaneFor(a, { gate, size, t, tr, keepSize, U, cu,
          gateWidth: U(a.gates?.[0]?.width ?? a.gateWidth, 4 / v), shift0: U(a.gates?.[0]?.offset, 0) }) : null;
        for (const b of [a.gateBelow, ...(a.gates ?? []).map((g) => g.below)])
          if (b !== undefined && b !== "auto" && !(typeof b === "number" && b >= 0)) throw new ToolError("a gate's below is a length of 0 or more, or \"auto\"");
        const style = matStyle(a, k);
        if (style?.trim?.merlonCap !== undefined && a.crenellations === false) notes.push(CAP_NOTE);
        const spec = {
          size, wallHeight: wh, wallThickness: t, towerShape: a.towerShape ?? "round", towerRadius: tr, towerHeight: a.towerHeight === undefined ? wh + 3 / v : a.towerHeight * k,
          keepSize, keepHeight: a.keepHeight === undefined ? wh + 4 / v : a.keepHeight * k, gate, gateWidth: U(a.gateWidth, 4 / v), gateHeight: U(a.gateHeight, 5 / v),
          material: mat, cren: a.crenellations !== false, found: a.foundation === undefined ? 0 : a.foundation / v,
          ...(laneOut?.keepAt ? { keepAt: laneOut.keepAt, keepDoor: laneOut.keepDoor } : {}),
          ...(style ? { style } : {}),
        } as CastleSpec;
        if (a.towerRoof !== undefined) {
          const { only, materials, ...rf } = a.towerRoof ?? ({} as NonNullable<typeof a.towerRoof>);
          if (only !== undefined && (!Array.isArray(only) || only.some((c) => !TOWER_CORNERS.includes(c)))) throw new ToolError("towerRoof.only is a list of nw, sw, ne, se");
          spec.towerRoofs = TOWER_CORNERS.map((c) => (only && !only.includes(c) ? null
            : roofOf({ ...rf, ...(materials?.[c] !== undefined ? { material: materials[c] } : {}) }, k, tr, spec.cren, mat, "towerRoof")!));
        }
        const at = sc(a.at, k0);
        const gs = grids(s.store);
        // a gate's below: the floor less the ground just outside its middle (auto), at most the passage's length
        const laneShift = laneOut?.gateShift;
        const gateList: GateSpec[] | undefined = a.gates?.length || a.gateBelow !== undefined || a.ground || laneShift !== undefined ? (a.gates?.length ? a.gates : [{ side: gate as Side, below: a.gateBelow }])
          .filter((g) => g.side).map((g, gi) => {
            const w = U((g as { width?: number }).width, spec.gateWidth), h = U((g as { height?: number }).height, spec.gateHeight);
            const shift = gi === 0 && laneShift !== undefined ? laneShift : U((g as { offset?: number }).offset, 0);
            const want = (g as { below?: number | "auto" }).below ?? (a.gates?.length || a.ground ? "auto" : 0);
            const alongX = g.side === "n" || g.side === "s", sign = g.side === "s" || g.side === "e" ? 1 : -1;
            const half = (alongX ? size[1] : size[0]) / 2, pass = t + 2;
            let below = 0;
            if (want === "auto") {
              const out = half + 1 + 1.5 / v, loc: Vec3 = alongX ? [shift, 0, sign * out] : [sign * out, 0, shift];
              const pm = placePoint(sc(loc, v), at, a.rotation ?? 0), top = spansAt(gs, pm[0], pm[2], at[1] + 0.5)[0];
              if (top && top.top < at[1]) below = Math.min(pass, (at[1] - top.top) / v);
            } else below = Math.min(pass, want * k);
            const bevel = (g as { bevel?: number }).bevel;
            return { side: g.side, width: w, height: h, shift, below, ...(bevel ? { bevel: bevel * k } : {}) };
          }) : undefined;
        if (gateList) spec.gates = gateList;
        // a gate as high as the wall (or leaving a lintel under a voxel) would leave the merlons over it floating: the
        // castle cuts them (castle(): the carve is opened to the top of the merlon row) and says so
        for (const g of gateList ?? (gate ? [{ side: gate, height: spec.gateHeight }] : []))
          if (g.height >= wh - 1 + 1e-9) notes.push(`gate ${g.side}: ${cu(g.height)} high in a ${cu(wh)} wall leaves no lintel, so the wall and its merlons over it are cut open to the top`);
        const shapes = place(scaled(castle(spec), v), at, a.rotation ?? 0);
        if (a.seat) {
          const seat = seatShape(gs, shapes, at[1], v, a.seatDepth === undefined ? 8 : a.seatDepth * k0, mat);
          if (seat) shapes.unshift(seat);   // first: the gates' carves after it open the passage through it
          else notes.push("seat: the ground is not lower than the floor under the walls: nothing to fill");
        }
        spots = castleSpots(spec).map((p) => ({ kind: p.kind, at: placePoint(sc(p.at, v), at, a.rotation ?? 0) }));
        const m = (x: number) => tidy([x * v, 0, 0])[0];
        // the corner towers as built: the centre of each top floor (the call's units), and its roof
        const towers = castleTowers(spec).map((tw) => {
          const p = placePoint(sc(tw.at, v), at, a.rotation ?? 0);
          const u = (x: number) => tidy([x / k0, 0, 0])[0];
          return { corner: tw.corner, at: [u(p[0]), u(p[1]), u(p[2])], topY: u(p[1]), radius: u(tr * v),
            ...(tw.roof ? { roof: { style: tw.roof.style, radius: u(tw.roof.radius * v), height: u(tw.roof.height * v), tipY: u(p[1] + tw.roof.height * v), material: tw.roof.material } } : {}) };
        });
        return { label: "Build castle", shapes, notes, info: { size: [m(size[0]), m(size[1])], wallHeight: m(wh), wallThickness: m(t), towerRadius: m(tr),
          towerHeight: m(spec.towerHeight), towers, keep: keepSize ? [m(keepSize[0]), m(keepSize[1])] : null,
          gate: gate ? { side: gate, width: m(spec.gateWidth), height: m(spec.gateHeight) } : null,
          ...(gateList ? { gates: gateList.map((g) => ({ side: g.side, width: m(g.width), height: m(g.height), offset: m(g.shift), below: m(g.below), ...(g.bevel ? { bevel: m(g.bevel) } : {}) })) } : {}),
          ...(a.seat ? { seat: { depth: a.seatDepth ?? 8 } } : {}),
          ...(spec.keepAt ? { keepAt: [m(spec.keepAt[0]), m(spec.keepAt[1])], keepDoor: spec.keepDoor } : {}),
          ...(laneOut ? { lane: { ...laneOut.info, width: m(laneOut.info.width), centre: m(laneOut.info.centre) } } : {}),
          ...(a.keepAxes === "gate" && keepSize ? { keepAxes: { across: m(gate === "e" || gate === "w" ? keepSize[1] : keepSize[0]), deep: m(gate === "e" || gate === "w" ? keepSize[0] : keepSize[1]) } } : {}),
          courtyardFloorY: at[1], ...(v !== 1 ? { scale: v } : {}), ...styleInfo(a, style) } };
      }) as Record<string, unknown>;
      const spotsOut = spotReport(s, spots, a.units, !!a.dryRun);
      // the courtyard's walking height as built: the floor asked snaps to the castle's voxels (scale 1.5 at floor 4 makes a
      // court of 4.5), so trails and shelves outside the gates are cut to the measured top, not the y asked for
      const courtTops = (spotsOut.spots as { kind: string; ok?: boolean; standOn?: number[] | null }[]).filter((x) => x.kind === "courtyard" && x.ok && x.standOn).map((x) => x.standOn![1]).sort((x, y) => x - y);
      const asked = (res as { structureInfo?: { courtyardFloorY?: number } }).structureInfo?.courtyardFloorY;
      const court = !a.dryRun && courtTops.length && asked !== undefined ? (() => {
        const kk = a.units === "world" ? 20 : 1, top = courtTops[Math.floor(courtTops.length / 2)];
        return { courtyard: { askedY: tidy([asked * kk, 0, 0])[0], walkY: top, snappedBy: tidy([top - asked * kk, 0, 0])[0], spots: courtTops.length,
          note: "walkY: the top of the courtyard's voxels under its spawn spots (the median), where a worm stands; the floor asked snaps to the castle's voxels (scale), so meet the gates with walkY, not the y asked for" } };
      })() : {};
      return { ...res, ...court, ...(settled.groundUnder ? { groundUnder: settled.groundUnder } : {}), spawnSpots: spotsOut };
    },
  },
  {
    name: "build_wall",
    description: "Build a wall along a polyline of points: each point [x, y, z] gives the wall's base level there (the base slopes between points), with crenellations along the top if wanted. Walls join at the points. One undo step.",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["points"],
      properties: {
        points: { type: "array", items: vec3("[x, y, z] of the wall's centre line at its base."), minItems: 2, maxItems: 200, description: "The centre line, at least 2 points." },
        closed: { type: "boolean", description: "Join the last point back to the first." },
        height: { type: "number", minimum: 1, description: "Height above the base. Default 5." },
        thickness: { type: "number", minimum: 1, description: "Default 2." },
        crenellations: crenellations("Merlons one row above the top (2 on, 1 off). Default true."),
        parapet: { type: "string", enum: ["both", "left", "right", "all"], description: "Which edge rows carry merlons when the wall is 3+ thick (left = left of the direction of travel); thinner walls are crenellated across. Default both." },
        bands: bandsSchema("each segment's base line (the wall slopes with its points)"),
        trim: trimSchema("the wall", true),
        foundation, ...common,
      },
    },
    handler(s, a: Common & { points: Vec3[]; closed?: boolean; height?: number; thickness?: number; crenellations?: boolean; parapet?: "both" | "left" | "right" | "all"; foundation?: number;
      bands?: BandIn[]; trim?: TrimIn }) {
      a.points.forEach((p, i) => finite(p, `points[${i}]`));
      return run(s, a, a.points[0], "wall", (k, mat, U) => {
        const style = matStyle(a, k), notes = style?.trim?.merlonCap !== undefined && a.crenellations === false ? [CAP_NOTE] : [];
        const pts = a.points.map((p) => sc(p, k));
        if (a.closed) pts.push(pts[0]);
        const h = U(a.height, 5), t = U(a.thickness, 2);
        const shapes: Shape[] = [];
        let length = 0;
        for (let i = 0; i + 1 < pts.length; i++) {
          shapes.push(...wallSegment(pts[i], pts[i + 1], t, h, mat, { found: U(a.foundation, 0), cren: a.crenellations !== false, parapet: a.parapet ?? "both", ...(style ? { style } : {}) }));
          length += Math.hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][2] - pts[i][2]);
        }
        if (!shapes.length) throw new ToolError("all points are in the same place in x and z: there is no wall to build");
        return { label: "Build wall", shapes, notes, info: { segments: pts.length - 1, length: tidy([length, 0, 0])[0], height: h, thickness: t, ...styleInfo(a, style) } };
      });
    },
  },
  {
    name: "build_tower",
    description: "Build a round or square tower, solid or hollow (open top, floor at the base level, with a door), crenellated round the rim. `at` is the centre at the base. One undo step. " + SIDE_NOTE,
    inputSchema: {
      type: "object", additionalProperties: false, required: ["at"],
      properties: {
        at: vec3("Centre of the base [x, y, z]; y is the level of the first solid layer."),
        shape: { type: "string", enum: ["round", "square"], description: "Default round." },
        radius: { type: "number", minimum: 1.5, description: "Radius (half the side when square). Default 4." },
        height: { type: "number", minimum: 2, description: "Default 14." },
        hollow: { type: "boolean", description: "Carve the inside from the base up, leaving a wall. Default false." },
        wallThickness: { type: "number", minimum: 1, description: "Wall thickness when hollow. Default 2." },
        door: sideSchema("Side of a door through the wall, or none. Default s when hollow, none otherwise."),
        doorWidth: { type: "number", minimum: 1, description: "Default 2." },
        doorHeight: { type: "number", minimum: 2, description: "Default 3." },
        crenellations: crenellations("Merlons round the rim. Default true."),
        bands: bandsSchema("the base"), trim: trimSchema("the tower", true),
        roof: roofSchema("the tower"),
        foundation, rotation, ...groundProps, ...common,
      },
    },
    handler(s, a0: Common & { at: Vec3; shape?: "round" | "square"; radius?: number; height?: number; hollow?: boolean; wallThickness?: number; door?: string;
      doorWidth?: number; doorHeight?: number; crenellations?: boolean; foundation?: number; rotation?: number; ground?: "max" | "mean" | "min"; sink?: number;
      bands?: BandIn[]; trim?: TrimIn; roof?: RoofIn }) {
      finite(a0.at, "at");
      const r0 = a0.radius ?? 4 * (a0.units === "world" ? 20 : 1), settled = settle(s, a0, r0, r0), a = { ...a0, at: settled.at };
      const res = run(s, a, a.at, "tower", (k, mat, U) => {
        const r = U(a.radius, 4), wall = U(a.wallThickness, 2);
        if (a.hollow && wall >= r - 0.5) throw new ToolError("the wall is as thick as the tower is wide: lower wallThickness or raise radius");
        const door = sideOf(a.door, a.hollow ? "s" : null);
        const spec = { shape: a.shape ?? "round", radius: r, height: U(a.height, 14), material: mat, hollow: !!a.hollow, wall, door,
          doorWidth: U(a.doorWidth, 2), doorHeight: U(a.doorHeight, 3), cren: a.crenellations !== false, found: U(a.foundation, 0), style: matStyle(a, k) } as TowerSpec;
        const rf = roofOf(a.roof, k, r, spec.cren, mat, "roof");
        if (rf) spec.roof = rf;
        const notes = spec.style?.trim?.merlonCap !== undefined && !spec.cren ? [CAP_NOTE] : [];
        const base = sc(a.at, k), o = (x: number) => tidy([x / k, 0, 0])[0];
        return { label: "Build tower", shapes: place(tower(spec), base, a.rotation ?? 0), notes,
          info: { shape: spec.shape, radius: r, height: spec.height, hollow: spec.hollow, door, ...styleInfo(a, spec.style),
            towers: [{ at: [o(base[0]), o(base[1] + spec.height), o(base[2])], topY: o(base[1] + spec.height), radius: o(r),
              ...(rf ? { roof: { style: rf.style, radius: o(rf.radius), height: o(rf.height), tipY: o(base[1] + spec.height + rf.height), material: rf.material } } : {}) }] } };
      }) as Record<string, unknown>;
      return settled.groundUnder ? { ...res, groundUnder: settled.groundUnder } : res;
    },
  },
  {
    name: "build_keep",
    description: "Build a keep: a rectangular stone building with a floor-level interior, a door, arrow slits, a flat roof and a crenellated rim. `at` is the centre of the base. One undo step. " + SIDE_NOTE,
    inputSchema: {
      type: "object", additionalProperties: false, required: ["at"],
      properties: {
        at: vec3("Centre of the base [x, y, z]; y is the level of the first solid layer (the interior floor)."),
        size: vec2("Outer size [x, z]. Default [12, 12]."),
        height: { type: "number", minimum: 3, description: "Default 10." },
        wallThickness: { type: "number", minimum: 1, description: "Default 2." },
        hollow: { type: "boolean", description: "Carve the interior. Default true." },
        roof: { type: "boolean", description: "Keep a one-layer roof over the interior. Default true." },
        door: sideSchema("Side of the door, or none. Default s."),
        doorWidth: { type: "number", minimum: 1, description: "Default 3." },
        doorHeight: { type: "number", minimum: 2, description: "Default 4." },
        windows: { type: "boolean", description: "Arrow slits (1 x 2) every 4 voxels along the walls of a hollow keep. Default true." },
        crenellations: crenellations("Merlons on the roof rim. Default true."),
        bands: bandsSchema("the base"), trim: trimSchema("the walls and roof", true),
        foundation, rotation, ...groundProps, ...common,
      },
    },
    handler(s, a0: Common & { at: Vec3; size?: [number, number]; height?: number; wallThickness?: number; hollow?: boolean; roof?: boolean; door?: string;
      doorWidth?: number; doorHeight?: number; windows?: boolean; crenellations?: boolean; foundation?: number; rotation?: number; ground?: "max" | "mean" | "min"; sink?: number;
      bands?: BandIn[]; trim?: TrimIn }) {
      finite(a0.at, "at");
      const sz0 = a0.size ?? [12 * (a0.units === "world" ? 20 : 1), 12 * (a0.units === "world" ? 20 : 1)];
      const settled = settle(s, a0, sz0[0] / 2, sz0[1] / 2), a = { ...a0, at: settled.at };
      const res = run(s, a, a.at, "keep", (k, mat, U) => {
        const size: [number, number] = a.size ? [a.size[0] * k, a.size[1] * k] : [12, 12], wall = U(a.wallThickness, 2);
        if (a.hollow !== false && (size[0] <= 2 * wall + 1 || size[1] <= 2 * wall + 1)) throw new ToolError("the walls leave no room inside: make the keep bigger or the walls thinner");
        const spec = { size, height: U(a.height, 10), material: mat, wall, hollow: a.hollow !== false, roof: a.roof !== false, door: sideOf(a.door, "s"),
          doorWidth: U(a.doorWidth, 3), doorHeight: U(a.doorHeight, 4), windows: a.windows !== false, cren: a.crenellations !== false, found: U(a.foundation, 0), style: matStyle(a, k) };
        const notes = spec.style?.trim?.merlonCap !== undefined && !spec.cren ? [CAP_NOTE] : [];
        return { label: "Build keep", shapes: place(keep(spec), sc(a.at, k), a.rotation ?? 0), notes, info: { size, height: spec.height, door: spec.door, ...styleInfo(a, spec.style) } };
      }) as Record<string, unknown>;
      return settled.groundUnder ? { ...res, groundUnder: settled.groundUnder } : res;
    },
  },
  {
    name: "build_compound",
    description: "Build one structure from an ordered list of solids: box, cylinder, cone (a frustum or a spire), dome (sphere cap or onion) and tiers (a stepped pyramid: base size, steps, riser height, ledge depth), each a fill or a carve (`carve: true`) with its own material, `part` and height `bands`. A later shape wins where shapes overlap, so a carve opens a niche or an arch in an earlier fill and a later fill closes it again. For plinths and stepped bases, domed towers, spires and niches that the castle builders do not draw. `at` is the compound's origin (y = the level its first shape's base stands on); each shape's own `at` is its base centre relative to it (default [0, 0, 0]). `blockScale`/`scale` (map units per voxel of the new blocks) cuts the cells; `ground`/`sink` seat it; `costOnly`, `costAt` and `maxCells` price it first. One undo step. " + SIDE_NOTE,
    inputSchema: {
      type: "object", additionalProperties: false, required: ["at", "shapes"],
      properties: {
        at: vec3("Origin [x, y, z]: y is the level the shapes' bases stand on."),
        shapes: {
          type: "array", minItems: 1, maxItems: 128,
          description: "The shapes, applied in this order.",
          items: {
            type: "object", additionalProperties: false, required: ["kind"],
            properties: {
              kind: { type: "string", enum: [...COMPOUND_KINDS], description: "box: size [x, z] and height. cylinder: radius, height. cone: radius (at the base), height, topRadius (default 0, a point). dome: radius (of its base circle), height (default radius), profile round (a sphere cap: a hemisphere at height = radius, flatter below it, a half ellipsoid above it; default) or onion (a neck, a bulge a third of the way up a tenth wider than the base circle, a taper to a point). tiers: a stepped pyramid of `steps` steps `riser` high, the base step `size` [x, z] (or `radius` with round: true), each next step inset by `ledge` on every side; its height is steps x riser." },
              carve: { type: "boolean", description: "Empty the shape instead of filling it (niches, arches, a hollow). Default false." },
              at: vec3("The shape's base centre [x, y, z], relative to the compound's origin. Default [0, 0, 0]."),
              material: { type: "integer", minimum: 0, maximum: 63, description: "Material 0-63 of a fill. Default: the call's material." },
              part: { type: "integer", minimum: 0, description: "The part this fill belongs to for packing \"parts\" (one block per part, merged where that costs few cells): give the plinth, a wing and a dome each their own. Default: each shape its own." },
              bands: { type: "array", minItems: 1, maxItems: 16, description: "Material by height above this shape's base (a fill only): the first band holding a cell's height wins, the shape's material elsewhere.",
                items: { type: "object", additionalProperties: false, required: ["material"], properties: {
                  from: { type: "number", description: "Height the band starts at. Default 0." }, to: { type: "number", description: "Height it ends at (exclusive). Default: the top." },
                  fromY: { type: "number", description: "Another name for from (the call's bands spell it so)." }, toY: { type: "number", description: "Another name for to." },
                  material: { type: "integer", minimum: 0, maximum: 63 } } } },
              trim: { ...trimSchema("this shape", false), description: "This shape's own trim (a fill only), instead of the call's: top (the topmost layer of the shape, topDepth thick) and course ({y, material, height}: a stripe at height y above the shape's base). Unset: the call's trim." },
              size: vec2("box and tiers: the footprint [x, z] (tiers: of the base step)."),
              height: { type: "number", minimum: 0.01, description: "box, cylinder, cone, dome." },
              radius: { type: "number", minimum: 0.01, description: "cylinder, cone (base), dome (base circle), tiers with round." },
              topRadius: { type: "number", minimum: 0, description: "cone: the radius at the top. Default 0." },
              profile: { type: "string", enum: ["round", "onion"], description: "dome: the profile. Default round." },
              steps: { type: "integer", minimum: 1, maximum: 64, description: "tiers: the number of steps." },
              riser: { type: "number", minimum: 0.01, description: "tiers: the height of each step." },
              ledge: { type: "number", minimum: 0, description: "tiers: how far each step is inset from the one below, on every side (the depth of the ledge)." },
              round: { type: "boolean", description: "tiers: round steps (give radius) instead of rectangular ones." },
            },
          },
        },
        bands: bandsSchema("each shape's base (on every fill without bands of its own)"),
        trim: trimSchema("each fill", false),
        rotation, ...groundProps, ...common,
      },
    },
    handler(s, a0: Common & { at: Vec3; shapes: CompoundEntry[]; rotation?: number; ground?: "max" | "mean" | "min"; sink?: number; bands?: BandIn[]; trim?: TrimIn }) {
      finite(a0.at, "at");
      if (!Array.isArray(a0.shapes) || !a0.shapes.length) throw new ToolError("shapes is a list of at least one shape");
      const k = a0.units === "world" ? 1 / 20 : 1;
      // the footprint, to set the ground under it (the material is a placeholder: run() chooses the real one)
      const fills0 = compoundSpecs(a0.shapes, k, 0, a0).filter((c) => !c.carve);
      if (!fills0.length) throw new ToolError("every shape is a carve: there is nothing to build (put a fill first)");
      const foot = planBounds(compound(fills0, [0, 0, 0], 0));
      const hx = Math.max(Math.abs(foot.min[0]), Math.abs(foot.max[0])) / k, hz = Math.max(Math.abs(foot.min[2]), Math.abs(foot.max[2])) / k;
      const settled = settle(s, a0, hx, hz), a = { ...a0, at: settled.at };
      const res = run(s, a, a.at, "compound", (k2, mat) => {
        const specs = compoundSpecs(a.shapes, k2, mat, a), fills = specs.filter((c) => !c.carve);
        const shapes = compound(specs, sc(a.at, k2), a.rotation ?? 0);
        const fb = planBounds(compound(fills, [0, 0, 0], 0)), o = (v: number) => tidy([v / k2, 0, 0])[0];
        const tip = a.at[1] * k2 + fb.max[1];
        return { label: "Build compound", shapes, notes: [],
          info: { shapes: specs.length, fills: fills.length, carves: specs.length - fills.length,
            size: [o(fb.max[0] - fb.min[0]), o(fb.max[1] - fb.min[1]), o(fb.max[2] - fb.min[2])],
            footprint: { x: [o(fb.min[0]), o(fb.max[0])], z: [o(fb.min[2]), o(fb.max[2])] },
            tipY: { map: tidy([tip, 0, 0])[0], world: tidy([tip * 20, 0, 0])[0] } } };
      }) as Record<string, unknown>;
      return settled.groundUnder ? { ...res, groundUnder: settled.groundUnder } : res;
    },
  },
  {
    name: "build_ramp",
    description: "Build a walkable ramp from point `from` up (or down) to point `to`. Worms walk slopes up to about 45 degrees; a steeper climb is refused unless `switchback` is set, which folds the ramp into lanes side by side (each at most maxSlope) joined at the ends, beside the straight line from -> to; the answer says where the top ends. The ramp is solid down to the lower end (support solid) or a slab. One undo step.",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["from", "to"],
      properties: {
        from: vec3("[x, y, z] where the ramp starts: y is the ground level there."),
        to: vec3("[x, y, z] where it ends: y is the level it reaches (a straight ramp ends exactly here)."),
        width: { type: "number", minimum: 2, description: "Width of the walking surface. Default 4." },
        maxSlope: { type: "number", minimum: 5, maximum: 60, description: "Steepest slope in degrees. Default 40." },
        switchback: { type: "boolean", description: "Fold the ramp into lanes when the straight slope would be steeper than maxSlope. Default false (the call fails instead)." },
        turn: { type: "string", enum: ["left", "right"], description: "Side of the line from -> to the switchback lanes stack on. Default left." },
        support: { type: "string", enum: ["solid", "slab"], description: "solid (default): filled down to the lower end; slab: only `thickness` voxels under the surface." },
        thickness: { type: "number", minimum: 1, description: "Slab thickness. Default 2." },
        below: { type: "number", minimum: 0, description: "Solid support goes this far below the lower end. Default 1." },
        mode: { type: "string", enum: ["build", "cut"], description: "build (default): a ramp of new solid on the ground. cut: the walking surface is cut into the terrain that is there (a path up a cone): the voxels above it, over its width and `headroom` high, are carved in every frame it crosses, and the support fills only empty cells under it where it passes over air, into existing frames only (place \"existing\"; never new blocks, so no block or cell cost)." },
        headroom: { type: "number", minimum: 1, description: "mode cut: how high above the walking surface is cleared. Default 3." },
        ...common,
      },
    },
    handler(s, a: Common & { from: Vec3; to: Vec3; width?: number; maxSlope?: number; switchback?: boolean; turn?: "left" | "right"; support?: "solid" | "slab"; thickness?: number; below?: number; mode?: "build" | "cut"; headroom?: number }) {
      finite(a.from, "from"); finite(a.to, "to");
      const cut = a.mode === "cut";
      if (cut && a.place !== undefined && a.place !== "existing") throw new ToolError(`mode cut edits only existing frames (place "existing"), not place "${a.place}"`);
      if (!cut && a.headroom !== undefined) throw new ToolError("headroom is for mode cut");
      return run(s, cut ? { ...a, place: "existing" } : a, a.from, "ramp", (k, mat, U) => {
        const A = sc(a.from, k), B = sc(a.to, k), w = U(a.width, 4);
        const dx = B[0] - A[0], dz = B[2] - A[2], L = Math.hypot(dx, dz), H = B[1] - A[1];
        if (L < 1) throw new ToolError("from and to must be at least 1 apart in x and z");
        const tanMax = Math.tan(((a.maxSlope ?? 40) * Math.PI) / 180), slope = Math.abs(H) / L;
        const d: [number, number] = [dx / L, dz / L], p: [number, number] = [-d[1], d[0]];
        const legs = a.switchback && slope > tanMax ? Math.ceil(Math.abs(H) / (L * tanMax) - 1e-9) : 1;
        if (!a.switchback && slope > tanMax) {
          const deg = (Math.atan(slope) * 180) / Math.PI;
          throw new ToolError(`the slope from -> to is ${deg.toFixed(1)} degrees (maxSlope ${a.maxSlope ?? 40}): set switchback to fold it into ${Math.ceil(Math.abs(H) / (L * tanMax))} lanes, or move the points`);
        }
        const sign = a.turn === "right" ? -1 : 1, dh = H / legs, floorY = (a.support === "slab" ? null : Math.min(A[1], B[1]) - U(a.below, 1));
        const shapes: Shape[] = [];
        for (let i = 0; i < legs; i++) {
          const forward = i % 2 === 0, off = sign * i * w;
          const start: [number, number] = forward ? [A[0] + p[0] * off, A[2] + p[1] * off] : [A[0] + d[0] * L + p[0] * off, A[2] + d[1] * L + p[1] * off];
          const l = { a: start, d: forward ? d : [-d[0], -d[1]] as [number, number], len: L, w, yA: A[1] + dh * i, yB: A[1] + dh * (i + 1), floorY, thick: U(a.thickness, 2), mat };
          // cut: the support fills only air (the ground keeps its own voxels), then the air over the surface is carved
          if (cut) shapes.push({ ...lane(l), keep: true }, laneCut(l, U(a.headroom, 3)));
          else shapes.push(lane(l));
        }
        const last = legs - 1, lastForward = last % 2 === 0, off = sign * last * w;
        const top: Vec3 = lastForward ? [B[0] + p[0] * off, B[1], B[2] + p[1] * off] : [A[0] + p[0] * off, B[1], A[2] + p[1] * off];
        const deg = (Math.atan(Math.abs(dh) / L) * 180) / Math.PI;
        return { label: cut ? "Cut ramp" : "Build ramp", shapes, notes: legs > 1 ? [`switchback of ${legs} lanes; the top is at ${tidy(top).join(", ")} (map units)`] : [],
          info: { slopeDegrees: tidy([deg, 0, 0])[0], lanes: legs, length: tidy([L, 0, 0])[0], rise: H, top: { map: tidy(top), world: tidy(toWorld(top)) },
            ...(cut ? { mode: "cut", headroom: U(a.headroom, 3) } : {}) } };
      });
    },
  },
  {
    name: "build_stairs",
    description: "Build a staircase from `from` to `to` (the treads rise by `rise` each; the first tread is one rise above `from` when climbing). Solid underneath. Refused when the tread would be shorter than 1 voxel. One undo step.",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["from", "to"],
      properties: {
        from: vec3("[x, y, z] of the foot: y is the ground level there."),
        to: vec3("[x, y, z] of the top end: y is the level of the last tread."),
        width: { type: "number", minimum: 1, description: "Default 3." },
        rise: { type: "number", minimum: 0.5, description: "Height of one step. Default 1; the actual rise is the height difference divided by the number of steps." },
        support: { type: "string", enum: ["solid", "slab"], description: "Default solid." },
        thickness: { type: "number", minimum: 1, description: "Slab thickness. Default 2." },
        below: { type: "number", minimum: 0, description: "Solid support goes this far below the lower end. Default 1." },
        ...common,
      },
    },
    handler(s, a: Common & { from: Vec3; to: Vec3; width?: number; rise?: number; support?: "solid" | "slab"; thickness?: number; below?: number }) {
      finite(a.from, "from"); finite(a.to, "to");
      return run(s, a, a.from, "stairs", (k, mat, U) => {
        const A = sc(a.from, k), B = sc(a.to, k), H = B[1] - A[1], dx = B[0] - A[0], dz = B[2] - A[2], L = Math.hypot(dx, dz);
        if (L < 1) throw new ToolError("from and to must be at least 1 apart in x and z");
        if (Math.abs(H) < 0.5) throw new ToolError("from and to are at the same height: a staircase needs a rise (use build_ramp for a flat strip)");
        const steps = Math.max(1, Math.round(Math.abs(H) / (U(a.rise, 1))));
        if (L / steps < 1) throw new ToolError(`${steps} steps would each be ${(L / steps).toFixed(2)} deep (under 1 voxel): lengthen the run, or raise rise`);
        const floorY = a.support === "slab" ? null : Math.min(A[1], B[1]) - U(a.below, 1);
        const shape = lane({ a: [A[0], A[2]], d: [dx / L, dz / L], len: L, w: U(a.width, 3), yA: A[1], yB: B[1], steps, floorY, thick: U(a.thickness, 2), mat });
        return { label: "Build stairs", shapes: [shape], notes: [], info: { steps, rise: tidy([H / steps, 0, 0])[0], tread: tidy([L / steps, 0, 0])[0] } };
      });
    },
  },
  {
    name: "dig_crater",
    description: "Dig a crater: a bowl carved below the surface level `at.y` with a raised rim of material round it. `at` is the bowl's centre on the surface. Optionally repaints the bowl's surface (scorch). One undo step.",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["at"],
      properties: {
        at: vec3("[x, y, z] centre of the crater; y is the ground surface there."),
        radius: { type: "number", minimum: 2, description: "Radius of the bowl at the surface. Default 8." },
        depth: { type: "number", minimum: 1, description: "Depth at the centre. Default radius / 2." },
        rimWidth: { type: "number", minimum: 0, description: "Width of the rim ring outside the bowl. Default radius / 3 (0 for none)." },
        rimHeight: { type: "number", minimum: 0, description: "Height of the rim at the bowl's edge, falling to nothing at its outer edge. Default max(1, depth / 3)." },
        bowlMaterial: { type: "integer", minimum: 0, maximum: 63, description: "Repaint the bowl surface (2 voxels deep) with this material." },
        clearance: { type: "number", minimum: 0, description: "Also carve this far above the surface level over the bowl (to remove overhangs). Default 0." },
        ...common,
      },
    },
    handler(s, a: Common & { at: Vec3; radius?: number; depth?: number; rimWidth?: number; rimHeight?: number; bowlMaterial?: number; clearance?: number }) {
      finite(a.at, "at");
      return run(s, a, a.at, "crater", (k, mat, U) => {
        const R = U(a.radius, 8), D = U(a.depth, R / 2);
        const spec = { radius: R, depth: D, rimWidth: U(a.rimWidth, R / 3), rimHeight: U(a.rimHeight, Math.max(1, D / 3)), material: mat,
          bowlMaterial: a.bowlMaterial ?? null, support: 1, clearance: U(a.clearance, 0) };
        return { label: "Dig crater", shapes: place(crater(spec), sc(a.at, k), 0), notes: [], info: { radius: R, depth: D, rimWidth: spec.rimWidth, rimHeight: spec.rimHeight } };
      });
    },
  },
  {
    name: "dig_tunnel",
    description: "Carve a tunnel (a tube) along a polyline of points [x, y, z] through the terrain, optionally lined with a shell of another material. Carving reaches every frame it passes through. Round tubes leave a curved floor; square tubes keep a level floor across the path. `floor: \"flat\"` digs a walkable tunnel instead: the points are on its floor, a flat floor that follows the slope between them (steeper than maxSlope is refused), `headroom` high. One undo step.",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["points"],
      properties: {
        points: { type: "array", items: vec3("[x, y, z] on the tunnel's axis."), minItems: 2, maxItems: 200, description: "The axis, at least 2 points." },
        radius: { type: "number", minimum: 1, description: "Radius of the bore (half the side when square). Default 2.5." },
        shape: { type: "string", enum: ["round", "square"], description: "Default round." },
        liningMaterial: { type: "integer", minimum: 0, maximum: 63, description: "Add a lining shell of this material round the bore." },
        liningThickness: { type: "number", minimum: 1, description: "Lining thickness. Default 1." },
        floor: { type: "string", enum: ["bore", "flat"], description: "bore (default): the points are the tube's axis and the floor is the bore's own (round: curved). flat: a walkable tunnel: the points are on its floor (y = the level worms walk on), which runs straight between them; the bore is 2 x radius wide and `headroom` high (round: the top radius is a half circle; square: a box), bends are rounded, and a floor slab `floorThickness` thick fills the empty cells under the floor (a floor across a gap or cave; in new blocks outside every frame unless place \"existing\"). A segment steeper than `maxSlope` is refused; the answer gives the steepest." },
        headroom: { type: "number", minimum: 1.5, description: "floor flat: floor to roof. Default 2 x radius (at least 2; a worm is about 1.2 tall)." },
        maxSlope: { type: "number", minimum: 0, maximum: 60, description: "floor flat: the steepest segment allowed, in degrees. Default 40 (worms walk about 45)." },
        floorThickness: { type: "number", minimum: 0, description: "floor flat: thickness of the floor slab under the floor, filled only where empty (in the structure's material). Default 1; 0 for none." },
        caps: { type: "string", enum: ["round", "flat"], description: "The bore's two ends. round (default): it runs a radius past the first and the last point (a dome-ended bore). flat: it stops at the plane through each, square to the tunnel there, so a mouth is flush with a wall's face and a passage through a wall does not bite a half sphere out of what is behind it; the lining stops there too." },
        ...common,
      },
    },
    handler(s, a: Common & { points: Vec3[]; radius?: number; shape?: "round" | "square"; liningMaterial?: number; liningThickness?: number; floor?: "bore" | "flat"; headroom?: number; maxSlope?: number; floorThickness?: number; caps?: "round" | "flat" }) {
      a.points.forEach((p, i) => finite(p, `points[${i}]`));
      const flat = a.floor === "flat";
      if (!flat && (a.headroom !== undefined || a.maxSlope !== undefined || a.floorThickness !== undefined)) throw new ToolError("headroom, maxSlope and floorThickness are for floor \"flat\"");
      return run(s, a, a.points[0], "tunnel", (k, mat, U) => {
        const pts = a.points.map((p) => sc(p, k)), r = U(a.radius, 2.5);
        let length = 0;
        for (let i = 0; i + 1 < pts.length; i++) length += Math.hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1], pts[i + 1][2] - pts[i][2]);
        if (length < 1) throw new ToolError("the points are the same place: there is no tunnel to dig");
        const lining = a.liningMaterial !== undefined ? { material: a.liningMaterial, thickness: U(a.liningThickness, 1) } : null;
        if (flat) {
          const maxSlope = a.maxSlope ?? 40;
          let steepest = 0, at = -1;
          for (let i = 0; i + 1 < pts.length; i++) {
            const hl = Math.hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][2] - pts[i][2]), dy = Math.abs(pts[i + 1][1] - pts[i][1]);
            if (hl < 1e-9 && dy < 1e-9) continue;
            const deg = hl < 1e-9 ? 90 : (Math.atan(dy / hl) * 180) / Math.PI;
            if (deg > steepest) { steepest = deg; at = i; }
          }
          if (steepest > maxSlope + 1e-9)
            throw new ToolError(`segment ${at} (points ${at} -> ${at + 1}) slopes ${steepest.toFixed(1)} degrees (maxSlope ${maxSlope}): add points to lengthen it, or zigzag`);
          const headroom = U(a.headroom, Math.max(2 * r, 2)), floorT = U(a.floorThickness, 1);
          return { label: "Dig tunnel", shapes: flatTunnel({ points: pts, radius: r, shape: a.shape ?? "round", headroom, floor: floorT, material: mat, lining, caps: a.caps }), notes: [],
            info: { length: tidy([length, 0, 0])[0], radius: r, lined: !!lining, floor: "flat", headroom, floorThickness: floorT, ...(a.caps ? { caps: a.caps } : {}), steepestDegrees: tidy([steepest, 0, 0])[0] } };
        }
        void mat;
        return { label: "Dig tunnel", shapes: tunnel(pts, r, a.shape ?? "round", lining, a.caps), notes: [], info: { length: tidy([length, 0, 0])[0], radius: r, lined: !!lining, ...(a.caps ? { caps: a.caps } : {}) } };
      });
    },
  },
  {
    name: "build_arch",
    description: "Build a freestanding arch (a gateway): two piers and a crown round a round-topped opening, in a vertical plane. `at` is the centre of the opening at the base. The opening is `span` wide, straight for `rise`, then semicircular. One undo step. " + SIDE_NOTE,
    inputSchema: {
      type: "object", additionalProperties: false, required: ["at"],
      properties: {
        at: vec3("[x, y, z] centre of the opening at ground level."),
        span: { type: "number", minimum: 2, description: "Width of the opening. Default 6." },
        rise: { type: "number", minimum: 0, description: "Height of the straight sides before the curve. Default 3." },
        pier: { type: "number", minimum: 1, description: "Width of each pier. Default 3." },
        depth: { type: "number", minimum: 1, description: "Thickness through the opening. Default 3." },
        crown: { type: "number", minimum: 1, description: "Thickness above the top of the curve. Default 2." },
        rotation: { type: "number", description: "Yaw in degrees: 0 puts the arch in the x-y plane (you walk through along z); 90 along x. Default 0." },
        foundation, ...common,
      },
    },
    handler(s, a: Common & { at: Vec3; span?: number; rise?: number; pier?: number; depth?: number; crown?: number; rotation?: number; foundation?: number }) {
      finite(a.at, "at");
      return run(s, a, a.at, "arch", (k, mat, U) => {
        const spec = { span: U(a.span, 6), rise: U(a.rise, 3), pier: U(a.pier, 3), depth: U(a.depth, 3), crown: U(a.crown, 2), material: mat, found: U(a.foundation, 0) };
        return { label: "Build arch", shapes: place(arch(spec), sc(a.at, k), a.rotation ?? 0), notes: [],
          info: { opening: { width: spec.span, height: spec.rise + spec.span / 2 }, height: spec.rise + spec.span / 2 + spec.crown } };
      });
    },
  },
  {
    name: "build_bridge",
    description: "Build a bridge deck from `from` to `to` (its top surface runs between the two heights) with rails along both edges, and optionally piers (or arches) down to a lower level. One undo step.",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["from", "to"],
      properties: {
        from: vec3("[x, y, z] of the deck surface at one end."),
        to: vec3("[x, y, z] of the deck surface at the other end."),
        width: { type: "number", minimum: 2, description: "Deck width. Default 4." },
        deckThickness: { type: "number", minimum: 1, description: "Default 2." },
        rails: { type: "boolean", description: "A one-voxel rail along both edges. Default true." },
        pierSpacing: { type: "number", minimum: 0, description: "Distance between piers; 0 (default) for none. Needs downTo." },
        pierWidth: { type: "number", minimum: 1, description: "Default 3." },
        downTo: { type: "number", description: "Level the piers stand on." },
        arched: { type: "boolean", description: "Fill solid between piers and carve a round arch through each bay (an arched viaduct). Default false." },
        ...common,
      },
    },
    handler(s, a: Common & { from: Vec3; to: Vec3; width?: number; deckThickness?: number; rails?: boolean; pierSpacing?: number; pierWidth?: number; downTo?: number; arched?: boolean }) {
      finite(a.from, "from"); finite(a.to, "to");
      return run(s, a, a.from, "bridge", (k, mat, U) => {
        const A = sc(a.from, k), B = sc(a.to, k), len = Math.hypot(B[0] - A[0], B[2] - A[2]);
        if (len < 2) throw new ToolError("from and to must be at least 2 apart in x and z");
        if (a.pierSpacing && a.downTo === undefined) throw new ToolError("pierSpacing needs downTo, the level the piers stand on");
        const notes: string[] = [];
        if (a.arched && !a.pierSpacing) notes.push("arched has no effect without pierSpacing and downTo");
        const { shapes, length } = bridge({ a: A, b: B, width: U(a.width, 4), deck: U(a.deckThickness, 2), rails: a.rails !== false, material: mat,
          pierSpacing: U(a.pierSpacing, 0), pierWidth: U(a.pierWidth, 3), downTo: a.downTo === undefined ? null : a.downTo * k, arched: !!a.arched });
        return { label: "Build bridge", shapes, notes, info: { length: tidy([length, 0, 0])[0], rise: B[1] - A[1] } };
      });
    },
  },
  {
    name: "sculpt_crater",
    description: "Sculpt a crater into the terrain that is there (a volcano's summit): reads the ground, raises a rim crest round the bowl that blends into the slope outside it, carves a bowl below the crest (with an optional flat floor) and paints material bands by distance from the crest. Unlike dig_crater it follows the ground instead of a fixed level, so there is no separate wall ring. The rim is raised in material, by default each column's own top material (no new runs in the blocks); fills reach new blocks where they leave every frame (blockScale gives them the field's voxel size). Each band adds about one run per column to the blocks it paints (at most 2000 per block). One undo step.",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["at", "radius", "depth"],
      properties: {
        at: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2, description: "[x, z] of the crater's centre." },
        radius: { type: "number", minimum: 2, description: "Radius of the rim crest." },
        depth: { type: "number", minimum: 1, description: "Depth of the floor below the crest." },
        floorRadius: { type: "number", minimum: 0, description: "Radius of a flat floor in the middle (default 0: a round bowl)." },
        rimHeight: { type: "number", description: "Raise the crest this far above the ground's mean height round the rim circle (default 0; negative lowers it)." },
        rimWidth: { type: "number", minimum: 0, description: "Width outside the crest over which the rim falls back to the ground's own surface (default radius / 2)." },
        bands: { type: "array", maxItems: 16, description: "Surface materials by signed distance from the crest (d - radius: negative inside the bowl, 0 on the crest), first match wins, painted bandDepth deep. For example lava on the floor and ash on the walls: [{material: 21, to: -8}, {material: 14, from: -8, to: 4}].",
          items: { type: "object", additionalProperties: false, required: ["material"], properties: {
            material: { type: "integer", minimum: 0, maximum: 63 }, from: { type: "number", description: "Distance at least this." }, to: { type: "number", description: "... and under this." } } } },
        bandDepth: { type: "number", minimum: 0.5, description: "How deep under the new surface the bands are painted (default 2)." },
        ...common,
      },
    },
    handler(s, a: Common & { at: [number, number]; radius: number; depth: number; floorRadius?: number; rimHeight?: number; rimWidth?: number;
      bands?: { material: number; from?: number; to?: number }[]; bandDepth?: number }) {
      finite(a.at, "at");
      const k = a.units === "world" ? 1 / 20 : 1;
      const cx = a.at[0] * k, cz = a.at[1] * k, R = a.radius * k, D = a.depth * k;
      const W = a.rimWidth === undefined ? R / 2 : a.rimWidth * k, r0 = Math.min(R * 0.95, (a.floorRadius ?? 0) * k), bandDepth = (a.bandDepth ?? 2) * k;
      const outer = R + W;
      // the ground before the edit, every half unit
      const step = 0.5;
      const gs = grids(s.store);
      const field = sampleField(gs, { x0: cx - outer - 1, z0: cz - outer - 1, x1: cx + outer + 1, z1: cz + outer + 1 }, step);
      const ground = (x: number, z: number) => {
        const i = Math.floor((x - field.x0) / step), j = Math.floor((z - field.z0) / step);
        return i < 0 || j < 0 || i >= field.w || j >= field.h ? -Infinity : field.top[j * field.w + i];
      };
      let sum = 0, n = 0;
      for (let q = 0; q < 96; q++) {
        const g = ground(cx + R * Math.cos((q / 96) * 2 * Math.PI), cz + R * Math.sin((q / 96) * 2 * Math.PI));
        if (g > -Infinity) { sum += g; n++; }
      }
      if (n < 48) throw new ToolError(`there is ground under only ${n} of 96 points of the rim circle: sculpt_crater shapes terrain that is there (use dig_crater, or raise the ground first)`);
      const crest = sum / n + (a.rimHeight ?? 0) * k, floorY = crest - D;
      const surface = (x: number, z: number) => {
        const d = Math.hypot(x - cx, z - cz);
        if (d < R) { const t = Math.max(0, d - r0) / (R - r0); return floorY + D * t * t; }
        const g = ground(x, z), w = Math.min(1, (d - R) / Math.max(1e-6, W));
        const sm = w * w * (3 - 2 * w);
        return g === -Infinity ? -Infinity : crest + (g - crest) * sm;
      };
      const bands = a.bands ?? [];
      const bandOf = (x: number, z: number) => {
        const sd = (Math.hypot(x - cx, z - cz) - R) / k;
        return bands.find((b) => (b.from === undefined || sd >= b.from) && (b.to === undefined || sd < b.to));
      };
      const lowY = Math.min(floorY - bandDepth, Number.isFinite(field.min) ? field.min : floorY) - 1, highY = Math.max(crest, Number.isFinite(field.max) ? field.max : crest) + 1;
      const box = (op: "fill" | "carve" | "paint", mat: number, hit: (x: number, y: number, z: number) => boolean): Shape =>
        ({ op, mat, min: [cx - outer, lowY, cz - outer], max: [cx + outer, highY, cz + outer], hit });
      const inside = (x: number, z: number) => Math.hypot(x - cx, z - cz) < outer;
      // without a material the rim is raised in each column's own top material: no new material change down the
      // column, so no new runs in the blocks it reaches (an added block holds at most 2000)
      const groundMat = (x: number, z: number) => {
        const i = Math.floor((x - field.x0) / step), j = Math.floor((z - field.z0) / step);
        const m = i < 0 || j < 0 || i >= field.w || j >= field.h ? -1 : field.mat[j * field.w + i];
        return m >= 0 ? m : 0;
      };
      return run(s, a, [cx / k, crest / k, cz / k], "crater", (_k, mat) => ({
        label: "Sculpt crater",
        shapes: [
          // up to the new surface from the ground that is there (never under it: caves stay), then down to it
          { ...box("fill", mat, (x, y, z) => { if (!inside(x, z)) return false; const g = ground(x, z); return g > -Infinity && y >= g && y < surface(x, z); }),
            ...(a.material === undefined ? { matAt: (x: number, _y: number, z: number) => groundMat(x, z) } : {}) },
          box("carve", 0, (x, y, z) => inside(x, z) && y >= surface(x, z)),
          // one paint per band, each only where it is the first band that matches
          ...bands.map((b) => box("paint", b.material, (x, y, z) => inside(x, z) && bandOf(x, z) === b && y < surface(x, z) && y >= surface(x, z) - bandDepth)),
        ],
        notes: [],
        info: { centre: [tidy([cx / k, 0, 0])[0], tidy([cz / k, 0, 0])[0]], crestY: tidy([crest / k, 0, 0])[0], floorY: tidy([floorY / k, 0, 0])[0],
          groundOnRim: tidy([sum / n / k, 0, 0])[0], radius: a.radius, rimWidth: tidy([W / k, 0, 0])[0], bands: bands.length },
      }));
    },
  },
];


