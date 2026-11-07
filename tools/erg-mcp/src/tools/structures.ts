// Tool group "structures": voxel architecture written into the open level as one undo step each. Every tool samples a
// shape list (lib/structures-shapes.ts) at the voxel cells it overlaps, so a structure lands in the existing frames and,
// where it reaches outside all of them, in new terrain blocks (lib/structures-apply.ts; the shared budget of 64 blocks).
import type { Vec3 } from "../core";
import { applyPlan, dominantMaterial } from "../lib/structures-apply";
import {
  SIDES, arch, bridge, castle, crater, keep, lane, place, tower, tunnel, wallSegment, type Plan, type Shape, type Side,
} from "../lib/structures-shapes";
import { tidy, toWorld } from "../lib/voxel";
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
};
const foundation: Schema = { type: "integer", minimum: 0, maximum: 32, description: "Voxels of solid filled below the base (y) so the structure reaches the ground. Default 0." };
const rotation: Schema = { type: "number", description: "Yaw in degrees about the structure's centre (counter-clockwise seen from above; 0 = sides parallel to the map axes). Default 0." };
const crenellations = (d: string): Schema => ({ type: "boolean", description: d });

const SIDE_NOTE = "Sides are map directions: n = -z, s = +z, e = +x, w = -x (before the rotation).";

interface Common { material?: number; place?: "auto" | "existing" | "new"; dryRun?: boolean; units?: "map" | "world"; }

const finite = (v: number[], what: string) => { if (!v.every(Number.isFinite)) throw new ToolError(`${what} must be finite numbers`); };

/** Scales every length the caller gave by the unit, resolves the material, plans and applies. */
function run(s: Session, a: Common, anchor: Vec3, structure: string, make: (k: number, mat: number, L: (v: number | undefined, d: number) => number) => Plan): unknown {
  const { store } = s.requireProject();
  const k = a.units === "world" ? 1 / 20 : 1;
  const at: Vec3 = [anchor[0] * k, anchor[1] * k, anchor[2] * k];
  const mat = a.material ?? dominantMaterial(store, at) ?? 1;
  const plan = make(k, mat, (v, d) => (v === undefined ? d : v * k));
  const r = applyPlan(store, plan, { place: a.place ?? "auto", dryRun: !!a.dryRun });
  return { structure, material: mat, ...(a.material === undefined ? { materialNote: "chosen from the voxels near the structure; pass material to set it" } : {}), ...r,
    structureInfo: plan.info, undo: r.dryRun || !r.voxels.changed ? undefined : "undo reverts the whole structure in one step; save writes it" };
}

const sideOf = (v: string | undefined, dflt: Side | null): Side | null => (v === undefined ? dflt : v === "none" ? null : (v as Side));
const sc = (v: Vec3, k: number): Vec3 => [v[0] * k, v[1] * k, v[2] * k];

// ------------------------------------------------------------------ tools

export const tools: Tool[] = [
  {
    name: "build_castle",
    description: "Build a castle: a rectangular curtain wall with a crenellated walkway, a tower on each corner, an optional keep in the courtyard and a gate. `at` is the centre of the footprint at floor level (y = the first solid layer; the courtyard is left as it is, use `foundation` to fill it). One undo step. " + SIDE_NOTE,
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
        keepSize: vec2("Keep outer size [x, z]. Default a third of the castle, at least 8."),
        keepHeight: { type: "number", minimum: 3, description: "Default wallHeight + 4." },
        gate: sideSchema("Side of the gate, or none. Default s."),
        gateWidth: { type: "number", minimum: 2, description: "Default 4." },
        gateHeight: { type: "number", minimum: 2, description: "Default 5 (round-topped from 3 wide)." },
        crenellations: crenellations("Merlons on the walls, towers and keep. Default true."),
        foundation, rotation, ...common,
      },
    },
    handler(s, a: Common & { at: Vec3; size?: [number, number]; wallHeight?: number; wallThickness?: number; towerShape?: "round" | "square"; towerRadius?: number;
      towerHeight?: number; keep?: boolean; keepSize?: [number, number]; keepHeight?: number; gate?: string; gateWidth?: number; gateHeight?: number;
      crenellations?: boolean; foundation?: number; rotation?: number }) {
      finite(a.at, "at");
      return run(s, a, a.at, "castle", (k, mat, U) => {
        const size: [number, number] = a.size ? [a.size[0] * k, a.size[1] * k] : [30, 30];
        const wh = U(a.wallHeight, 6), t = U(a.wallThickness, 3);
        const tr = a.towerRadius === undefined ? Math.max(3, t + 1) : a.towerRadius * k, notes: string[] = [];
        if (size[0] < 2 * tr + 4 || size[1] < 2 * tr + 4) throw new ToolError(`the castle is too small for its towers: each side needs at least ${2 * tr + 4} (2 x tower radius + 4)`);
        if (size[0] <= 2 * t + 4 || size[1] <= 2 * t + 4) throw new ToolError("the walls leave no courtyard: make the castle bigger or the walls thinner");
        let keepSize: [number, number] | null = null;
        if (a.keep !== false) {
          keepSize = a.keepSize ? [a.keepSize[0] * k, a.keepSize[1] * k] : [Math.max(8, Math.round(size[0] / 3)), Math.max(8, Math.round(size[1] / 3))];
          if (keepSize[0] > size[0] - 2 * t - 4 || keepSize[1] > size[1] - 2 * t - 4) {
            if (a.keep === true || a.keepSize) throw new ToolError(`the keep does not fit in the courtyard (${size[0] - 2 * t - 4} x ${size[1] - 2 * t - 4} free)`);
            notes.push("no keep: the courtyard is too small for one");
            keepSize = null;
          }
        }
        const gate = sideOf(a.gate, "s");
        const spec = {
          size, wallHeight: wh, wallThickness: t, towerShape: a.towerShape ?? "round", towerRadius: tr, towerHeight: a.towerHeight === undefined ? wh + 3 : a.towerHeight * k,
          keepSize, keepHeight: a.keepHeight === undefined ? wh + 4 : a.keepHeight * k, gate, gateWidth: U(a.gateWidth, 4), gateHeight: U(a.gateHeight, 5),
          material: mat, cren: a.crenellations !== false, found: U(a.foundation, 0),
        };
        const shapes = place(castle(spec), sc(a.at, k), a.rotation ?? 0);
        return { label: "Build castle", shapes, notes, info: { size, wallHeight: wh, wallThickness: t, towerRadius: tr, towerHeight: spec.towerHeight, keep: keepSize,
          gate: gate ? { side: gate, width: spec.gateWidth, height: spec.gateHeight } : null, courtyardFloorY: a.at[1] * k } };
      });
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
        foundation, ...common,
      },
    },
    handler(s, a: Common & { points: Vec3[]; closed?: boolean; height?: number; thickness?: number; crenellations?: boolean; parapet?: "both" | "left" | "right" | "all"; foundation?: number }) {
      a.points.forEach((p, i) => finite(p, `points[${i}]`));
      return run(s, a, a.points[0], "wall", (k, mat, U) => {
        const pts = a.points.map((p) => sc(p, k));
        if (a.closed) pts.push(pts[0]);
        const h = U(a.height, 5), t = U(a.thickness, 2);
        const shapes: Shape[] = [];
        let length = 0;
        for (let i = 0; i + 1 < pts.length; i++) {
          shapes.push(...wallSegment(pts[i], pts[i + 1], t, h, mat, { found: U(a.foundation, 0), cren: a.crenellations !== false, parapet: a.parapet ?? "both" }));
          length += Math.hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][2] - pts[i][2]);
        }
        if (!shapes.length) throw new ToolError("all points are in the same place in x and z: there is no wall to build");
        return { label: "Build wall", shapes, notes: [], info: { segments: pts.length - 1, length: tidy([length, 0, 0])[0], height: h, thickness: t } };
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
        foundation, rotation, ...common,
      },
    },
    handler(s, a: Common & { at: Vec3; shape?: "round" | "square"; radius?: number; height?: number; hollow?: boolean; wallThickness?: number; door?: string;
      doorWidth?: number; doorHeight?: number; crenellations?: boolean; foundation?: number; rotation?: number }) {
      finite(a.at, "at");
      return run(s, a, a.at, "tower", (k, mat, U) => {
        const r = U(a.radius, 4), wall = U(a.wallThickness, 2);
        if (a.hollow && wall >= r - 0.5) throw new ToolError("the wall is as thick as the tower is wide: lower wallThickness or raise radius");
        const door = sideOf(a.door, a.hollow ? "s" : null);
        const spec = { shape: a.shape ?? "round", radius: r, height: U(a.height, 14), material: mat, hollow: !!a.hollow, wall, door,
          doorWidth: U(a.doorWidth, 2), doorHeight: U(a.doorHeight, 3), cren: a.crenellations !== false, found: U(a.foundation, 0) };
        return { label: "Build tower", shapes: place(tower(spec), sc(a.at, k), a.rotation ?? 0), notes: [],
          info: { shape: spec.shape, radius: r, height: spec.height, hollow: spec.hollow, door } };
      });
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
        foundation, rotation, ...common,
      },
    },
    handler(s, a: Common & { at: Vec3; size?: [number, number]; height?: number; wallThickness?: number; hollow?: boolean; roof?: boolean; door?: string;
      doorWidth?: number; doorHeight?: number; windows?: boolean; crenellations?: boolean; foundation?: number; rotation?: number }) {
      finite(a.at, "at");
      return run(s, a, a.at, "keep", (k, mat, U) => {
        const size: [number, number] = a.size ? [a.size[0] * k, a.size[1] * k] : [12, 12], wall = U(a.wallThickness, 2);
        if (a.hollow !== false && (size[0] <= 2 * wall + 1 || size[1] <= 2 * wall + 1)) throw new ToolError("the walls leave no room inside: make the keep bigger or the walls thinner");
        const spec = { size, height: U(a.height, 10), material: mat, wall, hollow: a.hollow !== false, roof: a.roof !== false, door: sideOf(a.door, "s"),
          doorWidth: U(a.doorWidth, 3), doorHeight: U(a.doorHeight, 4), windows: a.windows !== false, cren: a.crenellations !== false, found: U(a.foundation, 0) };
        return { label: "Build keep", shapes: place(keep(spec), sc(a.at, k), a.rotation ?? 0), notes: [], info: { size, height: spec.height, door: spec.door } };
      });
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
        ...common,
      },
    },
    handler(s, a: Common & { from: Vec3; to: Vec3; width?: number; maxSlope?: number; switchback?: boolean; turn?: "left" | "right"; support?: "solid" | "slab"; thickness?: number; below?: number }) {
      finite(a.from, "from"); finite(a.to, "to");
      return run(s, a, a.from, "ramp", (k, mat, U) => {
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
          shapes.push(lane({ a: start, d: forward ? d : [-d[0], -d[1]], len: L, w, yA: A[1] + dh * i, yB: A[1] + dh * (i + 1), floorY, thick: U(a.thickness, 2), mat }));
        }
        const last = legs - 1, lastForward = last % 2 === 0, off = sign * last * w;
        const top: Vec3 = lastForward ? [B[0] + p[0] * off, B[1], B[2] + p[1] * off] : [A[0] + p[0] * off, B[1], A[2] + p[1] * off];
        const deg = (Math.atan(Math.abs(dh) / L) * 180) / Math.PI;
        return { label: "Build ramp", shapes, notes: legs > 1 ? [`switchback of ${legs} lanes; the top is at ${tidy(top).join(", ")} (map units)`] : [],
          info: { slopeDegrees: tidy([deg, 0, 0])[0], lanes: legs, length: tidy([L, 0, 0])[0], rise: H, top: { map: tidy(top), world: tidy(toWorld(top)) } } };
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
    description: "Carve a tunnel (a tube) along a polyline of points [x, y, z] through the terrain, optionally lined with a shell of another material. Carving reaches every frame it passes through. Round tubes leave a curved floor; square tubes keep a level floor across the path. One undo step.",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["points"],
      properties: {
        points: { type: "array", items: vec3("[x, y, z] on the tunnel's axis."), minItems: 2, maxItems: 200, description: "The axis, at least 2 points." },
        radius: { type: "number", minimum: 1, description: "Radius of the bore (half the side when square). Default 2.5." },
        shape: { type: "string", enum: ["round", "square"], description: "Default round." },
        liningMaterial: { type: "integer", minimum: 0, maximum: 63, description: "Add a lining shell of this material round the bore." },
        liningThickness: { type: "number", minimum: 1, description: "Lining thickness. Default 1." },
        ...common,
      },
    },
    handler(s, a: Common & { points: Vec3[]; radius?: number; shape?: "round" | "square"; liningMaterial?: number; liningThickness?: number }) {
      a.points.forEach((p, i) => finite(p, `points[${i}]`));
      return run(s, a, a.points[0], "tunnel", (k, mat, U) => {
        const pts = a.points.map((p) => sc(p, k)), r = U(a.radius, 2.5);
        let length = 0;
        for (let i = 0; i + 1 < pts.length; i++) length += Math.hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1], pts[i + 1][2] - pts[i][2]);
        if (length < 1) throw new ToolError("the points are the same place: there is no tunnel to dig");
        const lining = a.liningMaterial !== undefined ? { material: a.liningMaterial, thickness: U(a.liningThickness, 1) } : null;
        void mat;
        return { label: "Dig tunnel", shapes: tunnel(pts, r, a.shape ?? "round", lining), notes: [], info: { length: tidy([length, 0, 0])[0], radius: r, lined: !!lining } };
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
];
