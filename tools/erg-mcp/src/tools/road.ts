// Tool group "road": sculpt_road, a road that climbs the ground at no more than a grade, graded into it as one undo step.
// The centreline is marched over the ground (lib/road.ts: towards `to`, leaning round a slope that is too steep, or a
// zigzag of switchbacks); a draped skin (heightfield_drape's corner offsets) is graded by moving its corners to the
// road's height across the road and blending back over the shoulder (the skin block is replaced by the same voxels with
// the new corners), and undraped voxels on the road are cut over it and filled under it, in the frames that are there.
import { RemoveFrame, AddFrame, type Command, type Frame, type Vec3 } from "../core";
import { spansAt } from "../lib/inspect-field";
import { RoadIndex, marchRoad, roadWeight, type RoadPoint } from "../lib/road";
import { applyPlan, dominantMaterial } from "../lib/structures-apply";
import type { Shape } from "../lib/structures-shapes";
import { grids, tidy, type Grid } from "../lib/voxel";
import { ToolError, type Schema, type Tool } from "../registry";

interface RoadArgs {
  from: number[]; to: number[]; maxGrade?: number; width?: number; shoulder?: number; switchbacks?: number; spread?: number; step?: number; maxLength?: number;
  headroom?: number; support?: number; material?: number; skin?: boolean; units?: "map" | "world"; dryRun?: boolean;
}

/** The skin blocks' new corners: each corner within the shoulder of the road moved toward the road's height (map units),
 * the block's column top being its grid top plus the corner offset. Only draped added blocks (corners) are graded. */
function gradeSkins(gs: Grid[], idx: RoadIndex, hw: number, shoulder: number) {
  const out: { g: Grid; corners: number[]; moved: number; up: number; down: number; clamped: number }[] = [];
  for (const g of gs) {
    const c = g.frame.corners;
    if (!c || !g.frame.new) continue;
    const X = g.size[0], Z = g.size[2], T = g.size[1], m = g.toMap, sy = m[5];
    if (Math.abs(m[1]) > 1e-9 || Math.abs(m[9]) > 1e-9 || Math.abs(sy) < 1e-9) continue;   // skins are never rotated
    const next = c.slice();
    let moved = 0, up = 0, down = 0, clamped = 0;
    for (let k = 0; k <= Z; k++) for (let i = 0; i <= X; i++) {
      const x = m[0] * i + m[2] * k + m[3], z = m[8] * i + m[10] * k + m[11];
      const r = idx.near(x, z);
      if (!r) continue;
      const w = roadWeight(r.d, hw, shoulder);
      if (w <= 0) continue;
      const n = k * (X + 1) + i, top = m[4] * i + sy * (T + c[n]) + m[6] * k + m[7];
      let v = c[n] + ((r.y - top) * w) / sy;
      if (v > 64 || v < -64) { v = Math.max(-64, Math.min(64, v)); clamped++; }
      v = Math.round(v * 1000) / 1000;
      if (Math.abs(v - c[n]) < 1e-3) continue;
      const d = (v - c[n]) * sy;
      up = Math.max(up, d); down = Math.max(down, -d);
      next[n] = v;
      moved++;
    }
    if (moved) out.push({ g, corners: next, moved, up, down, clamped });
  }
  return out;
}

export const tools: Tool[] = [{
  name: "sculpt_road",
  description: "A road from `from` to `to` that climbs no steeper than maxGrade (degrees), graded into the ground as one undo step, and its centreline. The centreline is marched over the ground (the highest solid top, a draped skin at its sheared height): straight for `to` where the ground allows, else leaning round the slope by the angle that climbs at the grade (a road up a cone winds round it), turning gently; or, with `switchbacks`, a zigzag of that many legs whose turns lie spread / 2 to alternate sides. The road's own height follows the ground but changes by at most the grade, so a hump is cut and a dip filled. Grading: a draped skin (heightfield_drape, sculpt_cone) has its corner offsets moved to the road's height across `width` and blended back over `shoulder` (smooth in the game, drawn and collided; the skin's blocks are replaced by the same voxels with the new corners, new ids), and in undraped frames the voxels over the road are cut `headroom` high and the empty ones `support` under it filled (existing frames only, no new blocks). Answers the centreline (`centreline`, in `units`), its length, climb, steepest stretch, the deepest cut and fill, what was graded, and `paintPath` (paint_path's points and width, to surface it).",
  inputSchema: {
    type: "object", additionalProperties: false, required: ["from", "to"],
    properties: {
      from: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 3, description: "Start [x, z] (or [x, y, z], y unused): the road starts on the ground there." },
      to: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 3, description: "End [x, z] (or [x, y, z], y unused)." },
      maxGrade: { type: "number", minimum: 1, maximum: 45, description: "The steepest climb, degrees (default 20; worms walk about 45)." },
      width: { type: "number", minimum: 1, description: "The road's width (units; default 4 map units): graded flat across." },
      shoulder: { type: "number", minimum: 0, description: "Past the road's edge, the skin blends back to the ground over this (units; default 3 map units)." },
      switchbacks: { type: "integer", minimum: 2, maximum: 24, description: "A zigzag of this many straight legs between from and to instead of the march." },
      spread: { type: "number", minimum: 1, description: "switchbacks: how far apart the turns lie across the line from -> to (units; default half its length, at least 8 map units)." },
      step: { type: "number", minimum: 0.25, maximum: 8, description: "The march's step (units; default 1 map unit)." },
      maxLength: { type: "number", minimum: 1, description: "Stop marching after this long (units; default 2000 map units)." },
      headroom: { type: "number", minimum: 0, description: "Undraped voxels cut over the road this high (units; default 3 map units; 0 cuts none)." },
      support: { type: "number", minimum: 0, description: "Empty undraped cells this deep under the road filled (units; default 1 map unit; 0 fills none)." },
      material: { type: "integer", minimum: 0, maximum: 63, description: "The fill's material (default the most common solid material near the start)." },
      skin: { type: "boolean", description: "Grade draped skins' corners (default true; false leaves them)." },
      units: { type: "string", enum: ["map", "world"], description: "Units of every position and length here: map (default) or world (x 20)." },
      dryRun: { type: "boolean", description: "Plan and answer only: nothing changes." },
    } as Record<string, Schema>,
  },
  handler(s, a: RoadArgs) {
    const { store } = s.requireProject();
    const k = a.units === "world" ? 1 / 20 : 1, out = (v: number) => tidy([v / k, 0, 0])[0];
    const L = (v: number | undefined, d: number) => (v === undefined ? d : v * k);
    for (const [n, p] of [["from", a.from], ["to", a.to]] as const) if (!p.every(Number.isFinite)) throw new ToolError(`${n} must be finite numbers`);
    const xz = (p: number[]): [number, number] => (p.length === 3 ? [p[0] * k, p[2] * k] : [p[0] * k, p[1] * k]);
    const from = xz(a.from), to = xz(a.to);
    const gs = grids(store);
    const ground = (x: number, z: number) => spansAt(gs, x, z)[0]?.top ?? null;
    if (ground(...from) === null) throw new ToolError("from: no voxel ground there to start the road on");
    const hw = L(a.width, 4) / 2, shoulder = L(a.shoulder, 3), headroom = L(a.headroom, 3), support = L(a.support, 1);
    const march = marchRoad(ground, from, to, { maxGrade: a.maxGrade ?? 20, step: L(a.step, 1), maxLength: L(a.maxLength, 2000), switchbacks: a.switchbacks,
      spread: a.spread === undefined ? undefined : a.spread * k });
    if (march.pts.length < 2) throw new ToolError(`the road has no length (${march.stop})`);
    const pts = march.pts;
    const idx = new RoadIndex(pts, hw + shoulder + 1);
    // the skins
    const skins = a.skin === false ? [] : gradeSkins(gs, idx, hw, shoulder);
    const extra: Command[] = [];
    for (const sk of skins) {
      const frame: Frame = { ...structuredClone(sk.g.frame), corners: sk.corners };
      extra.push(new RemoveFrame(store.voxels, sk.g.id), new AddFrame(store.voxels, frame, sk.g.words.slice()));
    }
    // undraped voxels: cut over the road, fill under it
    const roadAt = (x: number, z: number) => { const r = idx.near(x, z); return r && r.d < hw ? r : null; };
    let min: Vec3 = [Infinity, Infinity, Infinity], max: Vec3 = [-Infinity, -Infinity, -Infinity];
    for (const p of pts) {
      min = [Math.min(min[0], p[0] - hw), Math.min(min[1], p[2] - support - 1), Math.min(min[2], p[1] - hw)];
      max = [Math.max(max[0], p[0] + hw), Math.max(max[1], p[2] + headroom + 1), Math.max(max[2], p[1] + hw)];
    }
    const mat = a.material ?? dominantMaterial(store, [from[0], pts[0][2], from[1]]) ?? 1;
    const shapes: Shape[] = [];
    if (support > 0) shapes.push({ op: "fill", mat, min, max, hit: (x, y, z) => { const r = roadAt(x, z); return !!r && y < r.y && y >= r.y - support; } });
    if (headroom > 0) shapes.push({ op: "carve", mat: 0, min, max, hit: (x, y, z) => { const r = roadAt(x, z); return !!r && y >= r.y && y < r.y + headroom; } });
    let voxels: unknown = null;
    if (shapes.length || extra.length) {
      const plan = { label: "Sculpt road", shapes: shapes.length ? shapes : [{ op: "carve" as const, mat: 0, min, max, hit: () => false }], notes: [], info: {} };
      try {
        const r = applyPlan(store, plan, { place: "existing", dryRun: !!a.dryRun, drape: "skip", extra });
        voxels = { ...r.voxels, frames: r.frames.slice(0, 12), usage: r.usage };
      } catch (e) {
        if (!(e instanceof ToolError) || !/nothing was built/.test(e.message)) throw e;
        voxels = { changed: 0, note: "no undraped voxel lies on the road, and no skin needed grading" };
      }
    }
    // the centreline, thinned to at most 200 points
    const every = Math.max(1, Math.ceil(pts.length / 200));
    const line = pts.filter((_, i) => i % every === 0 || i === pts.length - 1).map((p: RoadPoint) => [out(p[0]), out(p[2]), out(p[1])]);
    const paintEvery = Math.max(1, Math.ceil(pts.length / 400));
    return {
      dryRun: !!a.dryRun,
      road: { stop: march.stop, length: out(march.length), climb: out(march.climb), steepestDegrees: Math.round(march.steepest * 10) / 10, maxGrade: a.maxGrade ?? 20,
        deepestCut: out(march.cut), deepestFill: out(march.fill), width: out(hw * 2), shoulder: out(shoulder), ...(a.switchbacks ? { switchbacks: a.switchbacks } : {}),
        note: "deepestCut / deepestFill: the most the road runs under / over the ground it crosses (the march holds it to the grade)" },
      centreline: { points: line, note: `[x, y, z] in ${a.units ?? "map"} units, every ${every} of ${pts.length} steps; y is the road's surface` },
      skins: { graded: skins.length, cornersMoved: skins.reduce((n, x) => n + x.moved, 0), raisedUpTo: out(Math.max(0, ...skins.map((x) => x.up))),
        loweredUpTo: out(Math.max(0, ...skins.map((x) => x.down))), clamped: skins.reduce((n, x) => n + x.clamped, 0),
        blocks: skins.map((x) => ({ id: x.g.id, name: x.g.frame.name, cornersMoved: x.moved })),
        note: skins.length ? "a regraded skin block is replaced by the same voxels with new corners (its id can change: describe_level again); a skin raised more than its thickness leaves air under it, lowered it leaves the core under it, which the cut carves where it lies on the road" : "no draped skin lies on the road" },
      voxels,
      paintPath: { points: pts.filter((_, i) => i % paintEvery === 0 || i === pts.length - 1).map((p) => [out(p[0]), out(p[1])]), width: out(hw * 2), units: a.units ?? "map", blend: true,
        note: "paint_path's points, width and blend: surface the road with a material; blend paints its edge per corner (the game's second-material gradient), so on a steep flank the edge follows the road instead of the voxel stairs (a heightfield or drape rule's edge does the same for a road in its materials)" },
      ...(march.stop !== "arrived" ? { warning: march.stop === "maxLength" ? "the march stopped at maxLength before reaching to: raise maxLength or maxGrade, or use switchbacks" : "the march ran off the ground (no voxels under it)" } : {}),
      undo: a.dryRun ? undefined : "undo reverts the road in one step",
    };
  },
}];
