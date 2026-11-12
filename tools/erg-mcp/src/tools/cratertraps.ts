// Tool group "cratertraps": check_crater_traps, a lint for what a blast does to the ground a worm walks on. It cuts
// trial craters into copies of the open level's voxels (nothing is edited) along the walks from the spawns and says
// which of them a worm could not get out of on foot: the crater's walls are whole voxels, and where a voxel is as tall
// as a jump the hole is a trap (lib/crater-traps.ts). It names the voxel height that keeps the walls under a jump.
import type { Vec3 } from "../core";
import { JUMP_RISE, craterVerdict, trapAdvice, type CraterShape, type CraterVerdict, type TrapRules } from "../lib/crater-traps";
import { markers, spansAt, waterMap } from "../lib/inspect-field";
import { search, standing, surfaces, type ReachOptions, type Region } from "../lib/reach";
import { WORLD_PER_MAP, grids, isSolid, type Grid } from "../lib/voxel";
import { ToolError, type Tool } from "../registry";

const r2 = (v: number) => Math.round(v * 100) / 100;

/** Blast radii in map units, as measured in a Test (the hole a blast leaves in 1-unit voxels; see docs/erg-mcp.md). */
export const BLASTS: Record<string, number> = { bazooka: 3, grenade: 3, mine: 3, dynamite: 4.5, airstrike: 2.5 };

interface Args { points?: number[][]; along?: number[][]; x0?: number; z0?: number; x1?: number; z1?: number; spacing?: number; radius?: number; weapon?: string; depth?: number; rule?: "centre" | "touch";
  frames?: "skin" | "all"; walked?: boolean; jump?: number; maxSlope?: number; step?: number; margin?: number; water?: boolean; units?: "map" | "world"; limit?: number; maxPoints?: number; }

/** The lint over grids: the trial points, each one's verdict, and the advice. Pure but for the arguments. */
export function checkCraterTraps(gs0: Grid[], a: Args, spawns: Vec3[], waterY: number | null) {
  const k = a.units === "world" ? 1 / WORLD_PER_MAP : 1, out = (v: number) => r2(v / k), outP = (p: Vec3) => p.map(out);
  if (a.radius !== undefined && a.weapon !== undefined) throw new ToolError("give radius or weapon, not both");
  if (a.weapon !== undefined && !(a.weapon in BLASTS)) throw new ToolError(`weapon '${a.weapon}' is not one of ${Object.keys(BLASTS).join(", ")}: give radius`);
  const radius = a.weapon !== undefined ? BLASTS[a.weapon] : (a.radius ?? 3 / k) * k;
  if (!(radius > 0)) throw new ToolError("radius is above 0");
  const solid = gs0.filter((g) => g.words.some(isSolid));
  if (!solid.length) throw new ToolError("the level has no voxels");
  // skin: the points stand on draped blocks (the destructible ground skin); all: on any ground
  const skins = solid.filter((g) => g.frame.corners), on = a.frames === "all" || !skins.length ? solid : skins;
  const water = a.water === false ? null : waterY;
  const dry = (y: number) => water === null || y > water;
  const topOn = (x: number, z: number): number | null => { const t = spansAt(on, x, z)[0]; if (!t) return null; const all = spansAt(solid, x, z)[0]; return all && all.top > t.top + 1e-6 ? null : t.top; };
  let points: Vec3[] = [], how: string;
  if (a.points?.length) {
    how = "points";
    points = a.points.map((p, i) => {
      if (!Array.isArray(p) || (p.length !== 2 && p.length !== 3) || !p.every(Number.isFinite)) throw new ToolError(`points[${i}]: [x, z] (on the ground there) or [x, y, z]`);
      if (p.length === 3) return [p[0] * k, p[1] * k, p[2] * k] as Vec3;
      const top = spansAt(solid, p[0] * k, p[1] * k)[0];
      if (!top) throw new ToolError(`points[${i}]: no ground at ${p[0]}, ${p[1]}`);
      return [p[0] * k, top.top, p[1] * k] as Vec3;
    });
  } else {
    const sp = (a.spacing ?? 6 / k) * k;
    if (!(sp > 0)) throw new ToolError("spacing is above 0");
    let cand: [number, number][] = [];
    if (a.along?.length) {
      how = "along";
      if (a.along.length < 2) throw new ToolError("along: at least two points [x, z]");
      const path = a.along.map((p, i) => { if (!Array.isArray(p) || p.length < 2 || !p.every(Number.isFinite)) throw new ToolError(`along[${i}]: [x, z]`); return [p[0] * k, p[p.length - 1] * k] as [number, number]; });
      for (let i = 1; i < path.length; i++) {
        const [ax, az] = path[i - 1], [bx, bz] = path[i], len = Math.hypot(bx - ax, bz - az), n = Math.max(1, Math.round(len / sp));
        for (let t = i === 1 ? 0 : 1; t <= n; t++) cand.push([ax + ((bx - ax) * t) / n, az + ((bz - az) * t) / n]);
      }
    } else {
      how = "lattice";
      const bx0 = Math.min(...on.map((g) => g.bounds.min[0])), bx1 = Math.max(...on.map((g) => g.bounds.max[0]));
      const bz0 = Math.min(...on.map((g) => g.bounds.min[2])), bz1 = Math.max(...on.map((g) => g.bounds.max[2]));
      const x0 = a.x0 === undefined ? bx0 : a.x0 * k, x1 = a.x1 === undefined ? bx1 : a.x1 * k, z0 = a.z0 === undefined ? bz0 : a.z0 * k, z1 = a.z1 === undefined ? bz1 : a.z1 * k;
      for (let z = Math.min(z0, z1) + sp / 2; z < Math.max(z0, z1); z += sp) for (let x = Math.min(x0, x1) + sp / 2; x < Math.max(x0, x1); x += sp) cand.push([x, z]);
    }
    for (const [x, z] of cand) { const y = topOn(x, z); if (y !== null && dry(y)) points.push([x, y, z]); }
    // only the ground a spawn's walk gets to (a crater nobody can walk into traps nobody): the plain walking rules
    if (how === "lattice" && a.walked !== false && spawns.length && points.length) {
      const st = 2, ext: Region = { x0: Math.min(...solid.map((g) => g.bounds.min[0])), z0: Math.min(...solid.map((g) => g.bounds.min[2])), x1: Math.max(...solid.map((g) => g.bounds.max[0])), z1: Math.max(...solid.map((g) => g.bounds.max[2])) };
      const o: ReachOptions = { step: st, maxStep: 0.5, maxSlope: 45, maxDrop: 8, maxGap: 0, headroom: 1.5, water, slopeOver: "skin" };
      const S = surfaces(solid, ext, st, o.headroom), got = new Uint8Array(S.nodes);
      for (const p of spawns) { const n0 = standing(S, p, o, solid); if (n0 < 0) continue; const q = search(S, n0, o); for (let n = 0; n < S.nodes; n++) if (q.reached[n]) got[n] = 1; }
      const walked = (p: Vec3) => {
        const c = Math.floor((p[2] - ext.z0) / st) * S.w + Math.floor((p[0] - ext.x0) / st);
        for (let n = S.start[c]; n < S.start[c + 1]; n++) if (got[n] && Math.abs(S.y[n] - p[1]) <= 3) return true;
        return false;
      };
      const kept = points.filter(walked);
      how = `lattice, on the ground a spawn's walk reaches (${kept.length} of ${points.length} points)`;
      points = kept;
    }
    if (!points.length) throw new ToolError("no ground to try a crater on (frames: \"all\" tries every ground; walked: false every lattice point)");
  }
  const cap = a.maxPoints ?? 400;
  // a lattice over a large level is thinned evenly to the cap rather than refused (the default call must just work)
  if (points.length > cap && how.startsWith("lattice") && a.maxPoints === undefined) {
    const n = points.length, stride = n / cap;
    points = Array.from({ length: cap }, (_, i) => points[Math.floor(i * stride)]);
    how += `, thinned evenly from ${n} to ${cap} (maxPoints or a region tries more)`;
  }
  if (points.length > cap) throw new ToolError(`${points.length} trial craters is over maxPoints (${cap}): a wider spacing, a smaller region or a higher maxPoints`);
  const rules: TrapRules = { jump: (a.jump ?? JUMP_RISE / k) * k, maxSlope: a.maxSlope ?? 45, step: (a.step ?? 1 / k) * k, headroom: 1.5, water, margin: (a.margin ?? 5 / k) * k };
  const shape: CraterShape = { radius, depth: (a.depth ?? 0) * k, rule: a.rule };
  const verdicts = points.map((p) => craterVerdict(solid, p, shape, rules));
  const traps = verdicts.filter((v) => v.trap), holes = verdicts.filter((v) => v.holed), limit = a.limit ?? 30;
  const row = (v: CraterVerdict) => ({ at: outP(v.at), depth: v.depth === null ? null : out(v.depth), floorY: v.floor === null ? null : out(v.floor), wall: v.wall === null ? null : out(v.wall),
    ...(v.wallAt ? { wallAt: outP(v.wallAt) } : {}), layers: v.layers, voxel: out(v.voxel), carvedCells: v.carved });
  const advice = trapAdvice(traps, rules.jump);
  return {
    ok: traps.length === 0,
    summary: traps.length ? `${traps.length} of ${verdicts.length} trial craters are traps: a worm in one cannot get out on foot (the lowest wall is ${out(Math.min(...traps.map((t) => t.wall ?? Infinity)))}, a jump rises ${out(rules.jump)})`
      : `none of ${verdicts.length} trial craters is a trap: a jump clears every wall`,
    traps: traps.sort((p, q) => (q.wall ?? 0) - (p.wall ?? 0)).slice(0, limit).map(row), ...(traps.length > limit ? { more: traps.length - limit } : {}),
    ...(holes.length ? { holes: { count: holes.length, first: holes.slice(0, 8).map((v) => outP(v.at)), note: "the blast went through the ground there and nothing is left to stand on under it (a thin skin over a void: a shell under it, or a thicker skin)" } } : {}),
    craters: { tried: verdicts.length, how, radius: out(radius), ...(a.weapon ? { weapon: a.weapon } : {}), rule: a.rule ?? "centre", carvedNothing: verdicts.filter((v) => !v.carved).length,
      alreadyClosed: verdicts.filter((v) => !v.wasOpen).length, frames: on === skins ? "skin" : "all" },
    ...(advice ? { fix: { voxelSize: out(advice.voxelSize), why: advice.why,
      how: `heightfield_drape {voxelSize: ${out(advice.voxelSize)}} (or craterSlope: true, which picks it and says what it costs in cells) for the skin the craters are in; or a shell close under a thin skin, so the hole is shallow` } } : {}),
    rules: { jump: out(rules.jump), maxSlope: rules.maxSlope, step: out(rules.step), margin: out(rules.margin), water: water === null ? "not used" : out(water), units: a.units ?? "map" },
    note: "trial craters cut offline, nothing edited: a sphere of `radius` centred `depth` under each point takes every voxel whose centre lies in it (rule touch: every voxel it reaches into). The walk graph is rebuilt round the hole with a jump as the step a worm takes (a draped skin's slope counts: on a dune's face the downhill wall is lower), and a crater is a trap when the walk from its floor gets no further than its own edge although the same walk left the spot before the blast. A hole in a level pad is the usual trap: its walls are whole voxels all round",
  };
}

export const tools: Tool[] = [{
  name: "check_crater_traps",
  description: "Can a worm get out of the hole a blast leaves? Cuts trial craters into the open level's voxels offline (nothing is edited) and walks out of each with a jump as the step a worm takes: a crater's walls are whole voxels, so in a skin whose voxels are as tall as a jump (2.48 map units by game_riser_test; Dustcrown's sand skin has 2.5-unit voxels) a one-voxel hole on level ground is a trap, and a worm knocked into it sits there. The craters are tried at `points`, `along` a route (every `spacing`), or on a lattice over the ground the spawns' walks reach (default: the draped skin blocks, `frames: \"all\"` for every ground). Answers the traps (where, the hole's depth, the lowest wall and where it is, the voxel layers it took), holes blown through to nothing, and `fix`: the voxel height that keeps the walls under a jump. Give `radius`, or `weapon` (bazooka, grenade, mine, dynamite, airstrike) for a measured one. Read-only. Works on any level with a voxel ground skin.",
  inputSchema: { type: "object", additionalProperties: false, properties: {
    points: { type: "array", maxItems: 2000, items: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 3 }, description: "Blast points [x, z] (on the ground there) or [x, y, z]." },
    along: { type: "array", minItems: 2, maxItems: 400, items: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 3 }, description: "A route [x, z], ...: a crater every `spacing` along it (check_reachability {route: true}'s route)." },
    x0: { type: "number" }, z0: { type: "number" }, x1: { type: "number" }, z1: { type: "number" },
    spacing: { type: "number", minimum: 0.5, description: "Between trial craters (units; default 6 map units)." },
    radius: { type: "number", minimum: 0.25, description: "The blast's radius (units; default 3 map units)." },
    weapon: { type: "string", enum: Object.keys(BLASTS), description: "A measured blast instead of radius." },
    depth: { type: "number", description: "How far under the ground the blast is centred (units; default 0: on the surface)." },
    rule: { type: "string", enum: ["centre", "touch"], description: "centre (default): a voxel goes when its centre is in the sphere; touch: when the sphere reaches into it (the widest hole)." },
    frames: { type: "string", enum: ["skin", "all"], description: "Where the lattice's craters are tried: skin (default: on draped blocks, the destructible ground skin) or all." },
    walked: { type: "boolean", description: "Lattice: only points on ground a spawn knot's walk reaches (default true when the level has spawns)." },
    jump: { type: "number", minimum: 0.1, description: "The step a worm takes out of a hole (units; default 2.48 map units, a jump's rise)." },
    maxSlope: { type: "number", minimum: 0, maximum: 89, description: "Degrees a worm walks up (default 45)." },
    step: { type: "number", minimum: 0.25, maximum: 4, description: "Column spacing of the walk round a crater (units; default 1 map unit)." },
    margin: { type: "number", minimum: 1, description: "Ground read round the crater (units; default 5 map units); a walk that gets half of it past the crater's edge is out." },
    water: { type: "boolean", description: "Ground at or under the project's water is not ground (default true)." },
    limit: { type: "integer", minimum: 1, maximum: 400, description: "Traps listed (default 30), the highest walls first." },
    maxPoints: { type: "integer", minimum: 1, maximum: 4000, description: "Refuse more trial craters than this (default 400)." },
    units: { type: "string", enum: ["map", "world"] },
  } },
  handler(s, a: Args) {
    const { store } = s.requireProject();
    return checkCraterTraps(grids(store), a, markers(store).filter((m) => m.kind === "spawn").map((m) => m.pos as Vec3), waterMap(store));
  },
}];
