// Tool group "exposedcore": check_exposed_core, a lint for what craters show. It cuts trial craters into the open
// level's voxels offline (nothing is edited) and names every material a crater uncovers whose colour reads as sky, ice
// or water rather than earth: the floor of a hole through Dustcrown's plinth was the top of its brick band, a white and
// blue tile (lib/exposed-core.ts). The structure tools' coreMaterial is the fix it points to.
import type { Vec3 } from "../core";
import { colourReads, exposedCore, type Reads } from "../lib/exposed-core";
import { spansAt } from "../lib/inspect-field";
import { hex } from "../lib/inspect-png";
import { facesOf, projectMaterials, type MaterialSet } from "../lib/materials";
import { WORLD_PER_MAP, grids, isSolid, type Grid } from "../lib/voxel";
import { ToolError, type Tool } from "../registry";

const r2 = (v: number) => Math.round(v * 100) / 100;

interface Args { points?: number[][]; x0?: number; z0?: number; x1?: number; z1?: number; spacing?: number; radius?: number; passes?: number; frames?: "structures" | "all";
  flag?: Reads[]; allow?: number[]; units?: "map" | "world"; limit?: number; maxPoints?: number; }

/** The lint over grids with a material set (null: no colours, nothing can be flagged). Pure but for the arguments. */
export function checkExposedCore(gs0: Grid[], a: Args, set: MaterialSet | null) {
  const k = a.units === "world" ? 1 / WORLD_PER_MAP : 1, out = (v: number) => r2(v / k);
  const radius = (a.radius ?? 3 / k) * k, passes = a.passes ?? 3;
  if (!(radius > 0)) throw new ToolError("radius is above 0");
  const solid = gs0.filter((g) => g.words.some(isSolid));
  // structures: the blocks that are not a draped skin (a skin is the ground's own surface, tinted and ruled as ground)
  const gs = a.frames === "all" ? solid : solid.filter((g) => !g.frame.corners);
  if (!gs.length) throw new ToolError(a.frames === "all" ? "the level has no voxels" : "the level has no undraped voxel block (frames: \"all\" reads the skins too)");
  let points: Vec3[];
  if (a.points?.length) {
    points = a.points.map((p, i) => {
      if (!Array.isArray(p) || (p.length !== 2 && p.length !== 3) || !p.every(Number.isFinite)) throw new ToolError(`points[${i}]: [x, z] (on the top of the ground there) or [x, y, z]`);
      if (p.length === 3) return [p[0] * k, p[1] * k, p[2] * k] as Vec3;
      const top = spansAt(gs, p[0] * k, p[1] * k)[0];
      if (!top) throw new ToolError(`points[${i}]: no ground of the frames read at ${p[0]}, ${p[1]}`);
      return [p[0] * k, top.top, p[1] * k] as Vec3;
    });
  } else {
    // a lattice over the frames' box (or the region given), one crater on the top of each column that has ground
    const sp = (a.spacing ?? 6 / k) * k;
    if (!(sp > 0)) throw new ToolError("spacing is above 0");
    const bx0 = Math.min(...gs.map((g) => g.bounds.min[0])), bx1 = Math.max(...gs.map((g) => g.bounds.max[0]));
    const bz0 = Math.min(...gs.map((g) => g.bounds.min[2])), bz1 = Math.max(...gs.map((g) => g.bounds.max[2]));
    const x0 = a.x0 === undefined ? bx0 : a.x0 * k, x1 = a.x1 === undefined ? bx1 : a.x1 * k, z0 = a.z0 === undefined ? bz0 : a.z0 * k, z1 = a.z1 === undefined ? bz1 : a.z1 * k;
    points = [];
    for (let z = Math.min(z0, z1) + sp / 2; z < Math.max(z0, z1); z += sp) for (let x = Math.min(x0, x1) + sp / 2; x < Math.max(x0, x1); x += sp) {
      const top = spansAt(gs, x, z)[0];
      if (top) points.push([x, top.top, z]);
    }
    if (!points.length) throw new ToolError("no ground of the frames read in the region");
  }
  const cap = a.maxPoints ?? 600;
  if (points.length > cap) throw new ToolError(`${points.length} trial craters is over maxPoints (${cap}): a wider spacing, a smaller region or a higher maxPoints`);
  const { finds, carved } = exposedCore(gs, points, radius, passes);
  const flagOn = new Set<Reads>(a.flag ?? ["blue", "white"]), allow = new Set(a.allow ?? []);
  const rows = finds.map((f) => {
    const faces = set ? facesOf(set, f.material) : null;
    // a top is drawn by the material's top texture, a side and an underside by its side texture
    const rgb = faces ? (f.face === "top" ? faces.top : faces.side ?? faces.top) : null;
    const reads = rgb ? colourReads(rgb) : null;
    return { material: f.material, name: set?.names[f.material] ?? null, face: f.face, colour: rgb ? hex(rgb) : null, reads, voxels: f.voxels, craters: f.craters,
      firstAt: f.firstAt.map(out), flagged: !!reads && reads !== "earth" && flagOn.has(reads) && !allow.has(f.material) };
  });
  const flagged = rows.filter((r) => r.flagged), limit = a.limit ?? 40;
  return {
    ok: set ? flagged.length === 0 : null,
    summary: !set ? "the server gave no material colours: the uncovered materials are listed, none could be judged"
      : flagged.length ? `${flagged.length} uncovered face${flagged.length === 1 ? "" : "s"} read as sky, ice or water: ${flagged.slice(0, 6).map((r) => `${r.name ?? r.material} (${r.face}, ${r.colour}, ${r.reads})`).join(", ")}`
        : "every face the trial craters uncover reads as earth",
    flagged: flagged.slice(0, limit).map(({ flagged: _f, ...r }) => r),
    uncovered: rows.slice(0, limit).map(({ flagged: _f, ...r }) => r),
    craters: { points: points.length, radius: out(radius), passes, carvedCells: carved.reduce((n, v) => n + v, 0), empty: carved.filter((v) => !v).length, frames: a.frames ?? "structures" },
    ...(flagged.length ? { fix: "give the structure a coreMaterial (build_compound, build_castle, build_wall, build_tower, build_keep: the cells deeper than coreDepth under its faces), or a band material whose top reads as earth (list_materials shows each material's colour and sideColour); allow: [material] passes one on purpose" } : {}),
    note: "trial craters cut offline, nothing edited: at each point a sphere of `radius`, then passes - 1 more, each 0.8 of a radius under the last; a solid cell left beside a carved one is uncovered, by its top (drawn by the material's top texture) when the cell above went, else by a side or from below (the side texture). A colour reads blue from cyan to violet (pale lilac included), white when bright and nearly grey",
  };
}

export const tools: Tool[] = [
  {
    name: "check_exposed_core",
    description: "What craters show: cuts trial craters into the open level's voxels offline (nothing is edited) and lists every material they uncover, by the face uncovered and that face's real colour, flagging those that read as sky, ice or water instead of earth (blue to violet, pale lilac included, or bright near-white). A structure is solid in whatever its faces were asked in, bands included, so a blast opens onto the band that lies there drawn by its TOP texture: a brick band's top can be a white and blue tile, and the floor of a hole then reads as a hole to the sky. By default the craters are on a lattice (`spacing`) over the undraped blocks (the structures: a draped skin is ground, ruled and tinted as ground); `points` puts them where wanted. Fix what it finds with the structure tools' coreMaterial, or a band whose top reads as earth. Needs the server's material colours to judge (ok is null without them).",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        points: { type: "array", maxItems: 2000, items: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 3 }, description: "Crater centres: [x, z] (on the top of the ground of the frames read) or [x, y, z]. Default: a lattice over the region." },
        x0: { type: "number", description: "The lattice's region (default: the box of the frames read)." }, z0: { type: "number" }, x1: { type: "number" }, z1: { type: "number" },
        spacing: { type: "number", minimum: 0.25, description: "The lattice's spacing in `units` (default 6 map units)." },
        radius: { type: "number", minimum: 0.05, description: "A crater's radius in `units` (default 3 map units: an air strike's bomb; a bazooka's land damage radius is 1.75)." },
        passes: { type: "integer", minimum: 1, maximum: 8, description: "Hits in the same hole, each 0.8 of a radius under the last (default 3: a hole about 2.6 radii deep)." },
        frames: { type: "string", enum: ["structures", "all"], description: "structures (default): the undraped blocks. all: the draped skins too." },
        flag: { type: "array", items: { type: "string", enum: ["blue", "white"] }, description: "What is flagged (default both)." },
        allow: { type: "array", maxItems: 64, items: { type: "integer", minimum: 0, maximum: 63 }, description: "Materials passed on purpose (a snow level's ice)." },
        units: { type: "string", enum: ["map", "world"] },
        limit: { type: "integer", minimum: 1, maximum: 400, description: "Rows listed (default 40)." },
        maxPoints: { type: "integer", minimum: 1, maximum: 5000, description: "Refuse more trial craters than this (default 600)." },
      },
    },
    async handler(s, a: Args) {
      const store = s.requireProject().store;
      return checkExposedCore(grids(store), a, await projectMaterials(s));
    },
  },
];
