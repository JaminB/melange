// Tool group "toolreach": check_tool_reach, whether a jetpack, a ninja rope or a girder gets each spawn area to a target
// surface (a courtyard, a ledge, a summit). It complements check_reachability, which covers walking only; the geometry is
// lib/tool-reach.ts. Inspection only.
import { areaFirstKnot, type Vec3 } from "../core";
import { extentXZ, markers, waterMap, type Region } from "../lib/inspect-field";
import { nodeAt, standing, surfaces, type ReachOptions } from "../lib/reach";
import { CALIBRATION, MOVE_TOOLS, TOOL_LIMITS, USES_PER_TURN, planTo, toolReach, type Hop, type MoveTool, type ToolLimits, type ToolRun } from "../lib/tool-reach";
import { Raster, hex, type Rgb } from "../lib/inspect-png";
import { WORLD_PER_MAP, grids } from "../lib/voxel";
import { ToolError, image, type Tool } from "../registry";
import { topdownView } from "./inspect";

const r2 = (v: number) => Math.round(v * 100) / 100;
/** A plan whose tightest hop uses more than this share of its tool's limit is tight: a game test may fail it. */
const TIGHT = 0.85;
const COLOURS: Rgb[] = [[255, 70, 70], [70, 150, 255], [255, 210, 40], [200, 90, 255], [60, 230, 120], [255, 140, 30], [40, 230, 230], [255, 110, 200]];
const TOOL_RGB: Record<MoveTool, Rgb> = { jetpack: [255, 210, 40], rope: [60, 230, 120], girder: [70, 150, 255] };

interface Args {
  targets?: { x: number; z: number; y?: number; radius?: number; name?: string }[];
  target?: { x: number; z: number; y?: number; radius?: number; name?: string };
  from?: number[][]; tools?: MoveTool[];
  limits?: { jetpack?: { rise?: number; run?: number }; rope?: { length?: number }; girder?: { length?: number; maxSlope?: number } };
  maxUses?: number; hopStep?: number; perTool?: boolean; step?: number; water?: number;
  walk?: { maxStep?: number; maxSlope?: number; maxDrop?: number; maxGap?: number; headroom?: number; slopeOver?: "tread" | "riser" | "segment" | "skin"; bodyWidth?: number };
  render?: boolean; x0?: number; z0?: number; x1?: number; z1?: number; units?: "map" | "world";
}

interface Group { name: string; kind: "area" | "knot" | "from"; knots: { name: string; at: Vec3 }[]; }

export const tools: Tool[] = [{
  name: "check_tool_reach",
  description: "Does a movement weapon get each spawn area to a target surface? check_reachability covers walking only; this adds the jetpack, the ninja rope and the girder, as geometric estimates the game's tests confirm. The walk from a start (check_reachability's walk graph and rules, `walk`) is level 0; each tool use takes a worm from ground it stands on to other ground it could stand on, the walk from there is the next level, so a target's `uses` is the fewest tool uses (about a turn each) that gets there. jetpack: a flight of up to `limits.jetpack.rise` up with no run, or `run` across with no climb, trading one for the other linearly, over a clear path (straight, or up, over and down), any drop. rope: a shot as far as `limits.rope.length` at any bearing and elevation, held by the first solid ground it meets (a wall face, a tower flank, an arch's soffit), to a landing within the length of the anchor that is not higher than it and is reached through air. girder: a span up to `limits.girder.length` along a clear line no steeper than `maxSlope`. Starts are the spawn areas (spawns.mode areas: one group per area, its knots together), else each spawn knot, or the `from` points. Targets are `targets` (x, z, optional radius, default 3 map units, and optional y to pick the surface at that height instead of the highest in the circle; `name`). Answers per start group and target: walk (on foot), anyTool (reachable, uses, the plan: each hop's tool, take-off and landing, rise and run, the rope's anchor and lengths, the share of the tool's limit it uses, the tightest hop's margin, tight when over 85%), and with perTool (default true) the same for each tool alone, so `needs` names the tool a target takes; `turns` counts the plan in turns (the rope may be shot 5 times in one). Limits are map units (the answer gives them in the call's units; `calibration` says which are the game's own numbers (the rope's length, from its tweak data) and which are estimates no game test has measured (the jetpack's and the girder's); `limits` overrides one); the estimates miss ground thinner than hopStep as a landing and walls thinner than step, and the swing is not simulated, so confirm a plan with a game test (game_select_weapon, game_drive). `render: true` adds a top-down PNG of the plans. Inspection only.",
  inputSchema: {
    type: "object", additionalProperties: false,
    properties: {
      targets: { type: "array", minItems: 1, maxItems: 16, description: "The surfaces to reach: [{x, z, y?, radius?, name?}] in the call's units.",
        items: { type: "object", additionalProperties: false, required: ["x", "z"], properties: { x: { type: "number" }, z: { type: "number" }, y: { type: "number" }, radius: { type: "number", minimum: 0.1 }, name: { type: "string" } } } },
      target: { type: "object", additionalProperties: false, required: ["x", "z"], description: "One target (the same as a one-item targets).", properties: { x: { type: "number" }, z: { type: "number" }, y: { type: "number" }, radius: { type: "number", minimum: 0.1 }, name: { type: "string" } } },
      from: { type: "array", maxItems: 32, items: { type: "array", items: { type: "number" }, minItems: 3, maxItems: 3 }, description: "Start points [x, y, z] instead of the spawn areas or knots." },
      tools: { type: "array", minItems: 1, maxItems: 3, items: { type: "string", enum: ["jetpack", "rope", "girder"] }, description: "The tools to count (default all three)." },
      limits: { type: "object", additionalProperties: false, description: "Override the calibrated limits (the call's units).", properties: {
        jetpack: { type: "object", additionalProperties: false, properties: { rise: { type: "number", minimum: 0.1 }, run: { type: "number", minimum: 0.1 } } },
        rope: { type: "object", additionalProperties: false, properties: { length: { type: "number", minimum: 0.1 } } },
        girder: { type: "object", additionalProperties: false, properties: { length: { type: "number", minimum: 0.1 }, maxSlope: { type: "number", minimum: 1, maximum: 89 } } } } },
      maxUses: { type: "integer", minimum: 1, maximum: 6, description: "The most tool uses searched (default 3)." },
      hopStep: { type: "number", minimum: 1, maximum: 8, description: "Spacing of take-off and landing points (the call's units; default 2): ground narrower than this may not be landed on. Smaller is slower." },
      perTool: { type: "boolean", description: "Also answer each tool alone (default true)." },
      step: { type: "number", minimum: 0.5, maximum: 8, description: "Column spacing for the walk graph (the call's units; default 1 map unit, larger for a big region)." },
      water: { type: "number", description: "Water level in the call's units when the project keeps the level's own (describe_level water.map is null): ground at or under it is not stood on." },
      walk: { type: "object", additionalProperties: false, description: "The walking rules of level 0 and of each landing (check_reachability's): maxStep, maxSlope, maxDrop, maxGap, headroom, slopeOver, bodyWidth.",
        properties: { maxStep: { type: "number", minimum: 0 }, maxSlope: { type: "number", minimum: 0, maximum: 89 }, maxDrop: { type: "number", minimum: 0 }, maxGap: { type: "number", minimum: 0 }, headroom: { type: "number", minimum: 0.5 },
          slopeOver: { type: "string", enum: ["tread", "riser", "segment", "skin"] }, bodyWidth: { type: "number", minimum: 0 } } },
      render: { type: "boolean", description: "Also answer a top-down PNG: per start group (its colour) the plan to each target as arrows by tool (yellow jetpack, green rope, blue girder), anchors as crosses, targets ringed in white." },
      x0: { type: "number", description: "Region west edge (default: the level's extent)." }, z0: { type: "number" }, x1: { type: "number" }, z1: { type: "number" },
      units: { type: "string", enum: ["map", "world"], description: "Unit of what you give and of the answer's lengths and positions: map (default) or world (map x 20)." },
    },
  },
  async handler(s, a: Args) {
    const store = s.requireProject().store, gs = grids(store), u = a.units;
    const inMap = (v: number) => (u === "world" ? v / WORLD_PER_MAP : v);
    const out = (v: number) => r2(u === "world" ? v * WORLD_PER_MAP : v);
    const pt = (p: Vec3) => p.map(out);
    const list = a.targets?.length ? a.targets : a.target ? [a.target] : [];
    if (!list.length) throw new ToolError("give targets (or target): the surfaces to reach, [{x, z, y?, radius?, name?}]");
    if (a.targets?.length && a.target) throw new ToolError("give targets or target, not both");
    const tools_ = a.tools?.length ? [...new Set(a.tools)] : MOVE_TOOLS;
    const limits: ToolLimits = {
      jetpack: { rise: a.limits?.jetpack?.rise !== undefined ? inMap(a.limits.jetpack.rise) : TOOL_LIMITS.jetpack.rise, run: a.limits?.jetpack?.run !== undefined ? inMap(a.limits.jetpack.run) : TOOL_LIMITS.jetpack.run },
      rope: { length: a.limits?.rope?.length !== undefined ? inMap(a.limits.rope.length) : TOOL_LIMITS.rope.length },
      girder: { length: a.limits?.girder?.length !== undefined ? inMap(a.limits.girder.length) : TOOL_LIMITS.girder.length, maxSlope: a.limits?.girder?.maxSlope ?? TOOL_LIMITS.girder.maxSlope },
    };
    const ext = extentXZ(gs);
    if (!ext) throw new ToolError("the level has no voxel frames");
    const reg: Region = { x0: a.x0 === undefined ? ext.x0 : inMap(a.x0), z0: a.z0 === undefined ? ext.z0 : inMap(a.z0), x1: a.x1 === undefined ? ext.x1 : inMap(a.x1), z1: a.z1 === undefined ? ext.z1 : inMap(a.z1) };
    if (reg.x1 - reg.x0 < 1e-6 || reg.z1 - reg.z0 < 1e-6) throw new ToolError("the region is empty");
    const step = a.step !== undefined ? inMap(a.step) : 1;
    const cols = Math.ceil((reg.x1 - reg.x0) / step) * Math.ceil((reg.z1 - reg.z0) / step);
    if (cols > 400_000) throw new ToolError(`${cols} columns is too many (at most 400000): use a larger step or a smaller region`);
    const water = a.water !== undefined ? inMap(a.water) : waterMap(store);
    const w = a.walk ?? {};
    const o: ReachOptions = { step, maxStep: w.maxStep !== undefined ? inMap(w.maxStep) : 0.5, maxSlope: w.maxSlope ?? 45, maxDrop: w.maxDrop !== undefined ? inMap(w.maxDrop) : 8,
      maxGap: w.maxGap !== undefined ? inMap(w.maxGap) : 0, headroom: w.headroom !== undefined ? inMap(w.headroom) : 1.5, water, slopeOver: w.slopeOver ?? "tread",
      ...(w.bodyWidth ? { bodyWidth: inMap(w.bodyWidth) } : {}) };
    const S = surfaces(gs, reg, step, o.headroom);
    if (!S.nodes) throw new ToolError("no ground in the region");
    const hopStep = a.hopStep !== undefined ? inMap(a.hopStep) : 2;

    // the targets: the highest ground in the circle (or the surface nearest y), and the surfaces within 1.5 of it
    const targets = list.map((t, i) => {
      const x = inMap(t.x), z = inMap(t.z), rad = t.radius !== undefined ? inMap(t.radius) : 3, name = t.name ?? `targets[${i}]`;
      const inCircle = (n: number) => { const p = nodeAt(S, n); return Math.hypot(p[0] - x, p[2] - z) <= rad && (water === null || S.y[n] > water); };
      let top = -Infinity;
      if (t.y !== undefined) {
        const want = inMap(t.y);
        let bestD = Infinity;
        for (let n = 0; n < S.nodes; n++) if (inCircle(n) && Math.abs(S.y[n] - want) < bestD) { bestD = Math.abs(S.y[n] - want); top = S.y[n]; }
        if (bestD > 1.5) top = -Infinity;
      } else for (let n = 0; n < S.nodes; n++) if (inCircle(n) && S.y[n] > top) top = S.y[n];
      if (top === -Infinity) throw new ToolError(`${name}: no ground${t.y !== undefined ? ` within 1.5 map units of y ${t.y}` : ""} within ${out(rad)} of x ${t.x}, z ${t.z}${water !== null ? " above the water" : ""}`);
      const goal = new Uint8Array(S.nodes);
      let count = 0;
      for (let n = 0; n < S.nodes; n++) if (inCircle(n) && Math.abs(S.y[n] - top) <= 1.5) { goal[n] = 1; count++; }
      return { name, x, z, rad, top, goal, count };
    });

    // the start groups
    const spawnKnots = markers(store).filter((m) => m.kind === "spawn");
    const groups: Group[] = [];
    if (a.from) {
      a.from.forEach((p, i) => {
        if (p.length !== 3 || !p.every(Number.isFinite)) throw new ToolError(`from[${i}]: [x, y, z]`);
        groups.push({ name: `from[${i}]`, kind: "from", knots: [{ name: `from[${i}]`, at: [inMap(p[0]), inMap(p[1]), inMap(p[2])] }] });
      });
    } else if (store.scene.spawns.mode === "areas" && store.scene.spawns.areas?.length) {
      const areas = store.scene.spawns.areas, byName = new Map(spawnKnots.map((m) => [m.name, m.pos as Vec3]));
      areas.forEach((ar, i) => {
        const first = areaFirstKnot(areas, i), knots: Group["knots"] = [];
        for (let k = 0; k < ar.knots; k++) { const p = byName.get(`WORM${first + k}`); if (p) knots.push({ name: `WORM${first + k}`, at: p }); }
        groups.push({ name: ar.name, kind: "area", knots });
      });
    } else for (const m of spawnKnots) groups.push({ name: m.name, kind: "knot", knots: [{ name: m.name, at: m.pos as Vec3 }] });
    if (!groups.length) throw new ToolError("the level has no spawn knots: pass from");

    const maxUses = a.maxUses ?? 3, perTool = a.perTool !== false && tools_.length > 1;
    const sets: (MoveTool[] | "any")[] = ["any", ...(perTool ? tools_.map((t) => [t]) : [])];
    const planOf = (run: ToolRun, t: (typeof targets)[number]) => {
      let bestN = -1, bestKey = Infinity;
      for (let n = 0; n < S.nodes; n++) {
        if (!t.goal[n] || run.uses[n] === 255) continue;
        const plan = planTo(run, n), key = run.uses[n] * 10 + Math.max(0, ...plan.map((h) => h.ratio));
        if (key < bestKey) { bestKey = key; bestN = n; }
      }
      if (bestN < 0) return null;
      return { node: bestN, uses: run.uses[bestN], plan: planTo(run, bestN) };
    };
    // turns a plan takes: each tool use is a turn, but the rope may be shot several times in one (USES_PER_TURN)
    const turnsOf = (plan: Hop[]) => {
      let turns = 0, ropes = 0;
      for (const h of plan) {
        if (h.tool === "rope" && ropes > 0 && ropes < USES_PER_TURN.rope) { ropes++; continue; }
        turns++;
        ropes = h.tool === "rope" ? 1 : 0;
      }
      return turns;
    };
    const hopOut = (h: Hop) => {
      const pa = nodeAt(S, h.from), pb = nodeAt(S, h.to), run = Math.hypot(pb[0] - pa[0], pb[2] - pa[2]);
      return { tool: h.tool, from: pt(pa), to: pt(pb), rise: out(pb[1] - pa[1]), run: out(run), usesOfLimit: r2(h.ratio),
        ...(h.anchor ? { anchor: pt(h.anchor), ropeOut: out(h.ropeOut!), ropeIn: out(h.ropeIn!) } : {}) };
    };

    const drawn: { group: number; plans: { target: number; hops: Hop[]; end: Vec3 }[] }[] = [];
    let truncated = false;
    const summary: string[] = [];
    const outGroups = groups.map((g, gi) => {
      const starts = g.knots.map((k) => standing(S, k.at, o, gs));
      const standable = starts.filter((n) => n >= 0);
      if (!standable.length) return { name: g.name, kind: g.kind, knots: g.knots.map((k) => k.name), reachable: false, why: "none of its knots stands on ground in the walk graph (check_reachability says why)" };
      const runs = sets.map((set) => toolReach(S, standable, o, { tools: set === "any" ? tools_ : set, limits, maxUses, hopStep }));
      if (runs.some((r) => r.truncated)) truncated = true;
      const per = targets.map((t, ti) => {
        const any = planOf(runs[0], t);
        const walkReaches = any !== null && any.uses === 0;
        const only: Record<string, unknown> = {};
        const alone = new Map<MoveTool, number>();
        if (perTool) tools_.forEach((tl, k) => {
          const r = planOf(runs[k + 1], t);
          only[tl] = r ? { reachable: true, uses: r.uses } : { reachable: false };
          if (r) alone.set(tl, r.uses);
        });
        const worst = any ? Math.max(0, ...any.plan.map((h) => h.ratio)) : 0;
        const needs = !any ? null : walkReaches ? "none (walks)" : perTool ? ([...alone.keys()].length ? [...alone.keys()].join(" or ") + " alone" : "a mix of tools: " + [...new Set(any.plan.map((h) => h.tool))].join(" + ")) : [...new Set(any.plan.map((h) => h.tool))].join(" + ");
        if (any && !walkReaches) (drawn[gi] ??= { group: gi, plans: [] }).plans.push({ target: ti, hops: any.plan, end: nodeAt(S, any.node) });
        return { target: t.name, walk: walkReaches,
          anyTool: any ? { reachable: true, uses: any.uses, turns: turnsOf(any.plan), needs, ...(any.uses ? { plan: any.plan.map(hopOut), tightest: r2(worst), margin: r2(1 - worst), tight: worst > TIGHT } : {}) } : { reachable: false, uses: null, note: `not within ${maxUses} tool use(s) of any of ${tools_.join(", ")} at these limits` },
          ...(perTool ? { only } : {}) };
      });
      for (const p of per) {
        if (p.walk) summary.push(`${g.name} -> ${p.target}: walks`);
        else if (p.anyTool.reachable) summary.push(`${g.name} -> ${p.target}: ${(p.anyTool as { uses: number }).uses} tool use(s), ${(p.anyTool as { needs: string }).needs}${(p.anyTool as { tight?: boolean }).tight ? " (tight)" : ""}`);
        else summary.push(`${g.name} -> ${p.target}: NOT reachable with ${tools_.join("/")}`);
      }
      return { name: g.name, kind: g.kind, knots: g.knots.map((k) => k.name), standing: standable.length, targets: per,
        walkReach: { levels: runs[0].levels.map((l) => ({ uses: l.uses, newColumns: l.reached })) } };
    });

    const reachable = outGroups.every((g) => "targets" in g && (g.targets as { anyTool: { reachable: boolean } }[]).every((t) => t.anyTool.reachable));
    const res = {
      ok: reachable,
      summary,
      tools: tools_,
      usesPerTurn: USES_PER_TURN,
      limits: { units: u ?? "map", jetpack: { rise: out(limits.jetpack.rise), run: out(limits.jetpack.run) }, rope: { length: out(limits.rope.length) }, girder: { length: out(limits.girder.length), maxSlope: limits.girder.maxSlope } },
      calibration: a.limits ? { ...CALIBRATION, overridden: Object.keys(a.limits) } : CALIBRATION,
      targets: targets.map((t) => ({ name: t.name, at: [out(t.x), out(t.z)], top: out(t.top), surfaces: t.count })),
      groups: outGroups,
      settings: { maxUses, hopStep: out(hopStep), step: out(step), columns: S.nodes, water: water === null ? null : out(water) },
      ...(truncated ? { truncated: "the search hit its work limit: some hops may be missing (use a larger hopStep or step, or a smaller region)" } : {}),
      note: "estimates from the voxel geometry, not the game's physics: confirm a plan in the game (game_select_weapon, then game_drive / game_fire). Ground narrower than hopStep may not be landed on; walls thinner than step may be flown through; the rope's swing is not simulated.",
    };
    if (!a.render) return res;

    // the picture: the ground, each group's plans as arrows by tool, anchors, the targets
    const v = await topdownView(s, gs, reg, { units: u });
    const img: Raster = v.img, px = v.px, pz = v.pz;
    const lw = Math.max(1, Math.min(3, Math.round(v.scale / 3)));
    const ring = (x: number, zz: number, rad: number, c: Rgb) => {
      for (let k = 0; k < 48; k++) {
        const t0 = (k / 48) * Math.PI * 2, t1 = ((k + 1) / 48) * Math.PI * 2;
        img.line(px(x) + Math.round(rad * Math.cos(t0)), pz(zz) + Math.round(rad * Math.sin(t0)), px(x) + Math.round(rad * Math.cos(t1)), pz(zz) + Math.round(rad * Math.sin(t1)), c, 2);
      }
    };
    const legend: object[] = [];
    groups.forEach((g, gi) => {
      const c = COLOURS[gi % COLOURS.length];
      for (const k of g.knots) img.marker(px(k.at[0]), pz(k.at[2]), 3, c);
      for (const p of drawn[gi]?.plans ?? []) {
        const f0 = nodeAt(S, p.hops[0].from), k0 = g.knots[0].at;
        img.line(px(k0[0]), pz(k0[2]), px(f0[0]), pz(f0[2]), c, 1, 6);
        for (const h of p.hops) {
        const pa = nodeAt(S, h.from), pb = nodeAt(S, h.to), tc = TOOL_RGB[h.tool];
        if (h.anchor) { img.line(px(pa[0]), pz(pa[2]), px(h.anchor[0]), pz(h.anchor[2]), tc, lw, 3); img.cross(px(h.anchor[0]), pz(h.anchor[2]), 4, tc); img.line(px(h.anchor[0]), pz(h.anchor[2]), px(pb[0]), pz(pb[2]), tc, lw, 3); }
        else img.line(px(pa[0]), pz(pa[2]), px(pb[0]), pz(pb[2]), tc, lw + 1);
        img.marker(px(pa[0]), pz(pa[2]), 2, c);
        img.marker(px(pb[0]), pz(pb[2]), 2, tc);
        }
      }
      legend.push({ group: g.name, colour: hex(c) });
    });
    for (const t of targets) ring(t.x, t.z, 8, [255, 255, 255]);
    return { content: [image(img.png()), { type: "text", text: JSON.stringify({ ...res, picture: { drawn: "each start group's squares are its colour; a dotted line in the group's colour runs from its first knot to the first take-off (the walk); a hop is a line in the tool's colour (yellow jetpack, green rope dashed through its anchor's cross, blue girder) from the take-off to a dot at the landing; white rings are the targets", groups: legend } }, null, 1) }] };
  },
}];
