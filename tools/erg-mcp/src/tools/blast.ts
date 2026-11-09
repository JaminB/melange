// Tool group "blast": proving that terrain is destructible without aiming a worm. place_blast_points adds look-at
// knots (ERGBLAST_<n>, the game's own "LOOKAT POINT" detail, which draws nothing) where explosions are wanted;
// test_explosion answers the voxel diff an explosion of a radius carves there (predicted from the project: the solid
// cells whose centre is within the radius, outside PERM frames, which the engine never carves), can carve it into the
// project (apply) to look at with the renders, and with game: true sets one off in a running Test at the knot
// (level.test.explode: the game's own lib_CreateExplosion, Explosion.Construct) with shots before and after.
import { AddDetail, Group, carved, targetFrame, type Vec3 } from "../core";
import { commit, pointOut, unitsSchema, vec3, type Units } from "../lib/place-geom";
import { VoxelEdit, WORLD_PER_MAP, grids, isSolid, type Grid } from "../lib/voxel";
import { spansAt } from "../lib/inspect-field";
import { ToolError, type Content, type Tool } from "../registry";
import type { Session } from "../session";
import { tools as gameTools } from "./game";

export const BLAST_PREFIX = "ERGBLAST_";
const LOOKAT = "LOOKAT POINT";

/** The cells an explosion of radius r (map units) at p removes: solid cells whose centre is within r, in frames whose
 * name does not tag them PERM (the engine's permanent, uncarvable frames). */
export function blastDiff(s: Session, p: Vec3, r: number) {
  const store = s.store, per: { id: number; name: string; removed: number; permanent?: boolean; untouched?: number; cells: number[][] }[] = [];
  let removed = 0, kept = 0;
  for (const g of grids(store)) {
    const b = g.bounds;
    if (b.max[0] < p[0] - r || b.min[0] > p[0] + r || b.max[1] < p[1] - r || b.min[1] > p[1] + r || b.max[2] < p[2] - r || b.min[2] > p[2] + r) continue;
    const perm = /PERM/i.test(g.frame.name), m = g.toMap, [X, Y, Z] = g.size;
    const cells: number[][] = [];
    let n = 0;
    for (let z = 0; z < Z; z++)
      for (let x = 0; x < X; x++)
        for (let y = 0; y < Y; y++) {
          if (!isSolid(g.words[(z * X + x) * Y + y])) continue;
          const cx = x + 0.5, cy = y + 0.5, cz = z + 0.5;
          const q = [m[0] * cx + m[1] * cy + m[2] * cz + m[3], m[4] * cx + m[5] * cy + m[6] * cz + m[7], m[8] * cx + m[9] * cy + m[10] * cz + m[11]];
          if ((q[0] - p[0]) ** 2 + (q[1] - p[1]) ** 2 + (q[2] - p[2]) ** 2 > r * r) continue;
          n++;
          if (cells.length < 2000) cells.push([x, y, z]);
        }
    if (!n) continue;
    if (perm) kept += n;
    else removed += n;
    per.push({ id: g.id, name: g.frame.name, removed: perm ? 0 : n, ...(perm ? { permanent: true, untouched: n } : {}), cells });
  }
  return { removed, kept, frames: per.sort((a, b) => b.removed - a.removed) };
}

/** The blast knot nearest p (map units) within `within`, or null. */
function knotNear(s: Session, p: Vec3, within: number) {
  let best: { name: string; d: number; at: Vec3 } | null = null;
  for (const d of s.store.scene.details) {
    if (!d.name.startsWith(BLAST_PREFIX)) continue;
    const at = s.store.frames.detailWorld(d), dist = Math.hypot(at[0] - p[0], at[1] - p[1], at[2] - p[2]);
    if (dist <= within && (!best || dist < best.d)) best = { name: d.name, d: dist, at };
  }
  return best;
}

const call = (name: string) => gameTools.find((t) => t.name === name)!;

/** Whether the segment from eye to p (map units) passes through solid voxels, sampled every half unit, leaving out the
 * last `skip` units next to p (the wall being shot). */
export function blocked(gs: Grid[], eye: number[], p: number[], skip = 2): boolean {
  const d = Math.hypot(eye[0] - p[0], eye[1] - p[1], eye[2] - p[2]), n = Math.ceil(d / 0.5);
  for (let i = 0; i < n; i++) {
    const t = i / n;
    if (d * (1 - t) < skip) break;
    const q = [eye[0] + (p[0] - eye[0]) * t, eye[1] + (p[1] - eye[1]) * t, eye[2] + (p[2] - eye[2]) * t];
    if (spansAt(gs, q[0], q[2]).some((sp) => q[1] < sp.top && q[1] > sp.bottom)) return true;
  }
  return false;
}

/** The shots' eye for a blast at p (map units): `back` away, the first of eight bearings (south first) and three heights
 * whose line of sight to p is clear of the project's voxels (a pose fixed to the south looked from inside Caldera's
 * cone at a castle's uphill wall), else the south pose. */
export function blastEye(gs: Grid[], p: number[], back: number): { eye: number[]; clear: boolean } {
  for (const el of [35, 55, 75])
    for (let k = 0; k < 8; k++) {
      const az = Math.PI + (k % 2 ? 1 : -1) * Math.ceil(k / 2) * (Math.PI / 4), e = (el * Math.PI) / 180;
      const eye = [p[0] + back * Math.cos(e) * Math.sin(az), p[1] + back * Math.sin(e), p[2] - back * Math.cos(e) * Math.cos(az)];
      if (!blocked(gs, eye, p)) return { eye, clear: true };
    }
  return { eye: [p[0], p[1] + back * 0.8, p[2] + back], clear: false };
}

export const tools: Tool[] = [
  {
    name: "place_blast_points",
    description: "Add explosion points to the level for test_explosion {game: true}: a look-at knot (the game's LOOKAT POINT detail, which draws nothing) named ERGBLAST_<n> at each point. The game sets an explosion off only at a named detail, so save and Test the level (test {restart: true} from a running Test) after adding them. remove deletes them. One undo step.",
    inputSchema: { type: "object", additionalProperties: false, required: ["points"], properties: {
      points: { type: "array", minItems: 1, maxItems: 32, items: vec3("[x, y, z]: where the explosion's centre is (on or in the wall to test)."), description: "Points in `units`." },
      units: unitsSchema } },
    handler(s, a: { points: number[][]; units?: Units }) {
      const { store } = s.requireProject();
      const k = (a.units ?? "world") === "world" ? WORLD_PER_MAP : 1;
      const taken = new Set(store.scene.details.map((d) => d.name));
      const resource = store.scene.details.find((d) => d.role === "camera" && /look\s*at\s*point/i.test(d.resource))?.resource ?? LOOKAT;
      const frame = targetFrame(store.scene, "camera");
      const placed: object[] = [], cmds: AddDetail[] = [];
      let n = 0;
      for (const p of a.points) {
        while (taken.has(`${BLAST_PREFIX}${n}`)) n++;
        const name = `${BLAST_PREFIX}${n}`;
        taken.add(name);
        const at: Vec3 = [p[0] / k, p[1] / k, p[2] / k];
        cmds.push(new AddDetail(frame, { name, resource, pos: store.frames.toLocal(frame, at), rot: [0, 0, 0], scale: [1, 1, 1] }));
        placed.push({ name, at: pointOut(at, a.units ?? "world") });
      }
      commit(store, cmds.length === 1 ? cmds[0] : new Group(`Add ${cmds.length} blast points`, cmds));
      return { placed, resource, next: "save, then test (test {restart: true} from a running Test), then test_explosion {game: true} at each point" };
    },
  },
  {
    name: "test_explosion",
    description: "Destructibility without aiming a worm: an explosion of `radius` (the engine's land damage radius) centred on `at`. Answers the voxel diff it carves, predicted from the open project: the solid cells whose centre is within the radius, per frame (frames tagged PERM are never carved and are listed as untouched), with up to 2000 cells each. `apply: true` carves them into the project (one undo step) to look at with render_view and render_side. `game: true` also sets it off in the running Test at the ERGBLAST_ knot at that point (place_blast_points, then save and Test): the game's own lib_CreateExplosion through level.test.explode, with shots before and after (render.shot, clean) as image content; wormDamage and impulse default 0, so only the land is hit.",
    inputSchema: { type: "object", additionalProperties: false, required: ["at", "radius"], properties: {
      at: vec3("The explosion's centre [x, y, z] in `units`."),
      radius: { type: "number", minimum: 0.05, description: "Land damage radius in `units` (a bazooka's is 35 world units, 1.75 map)." },
      units: unitsSchema,
      apply: { type: "boolean", description: "Carve the predicted cells in the project (one undo step)." },
      game: { type: "boolean", description: "Also set it off in the running Test at the ERGBLAST_ knot within 1 map unit of `at`." },
      wormDamage: { type: "number", minimum: 0, maximum: 1000, description: "game: damage to worms in wormRadius (default 0)." },
      wormRadius: { type: "number", minimum: 0, description: "game: in `units` (default 0)." },
      impulse: { type: "number", minimum: 0, maximum: 100, description: "game: the push on worms (default 0)." },
      shots: { type: "boolean", description: "game: shots before and after (default true)." },
      settleMs: { type: "integer", minimum: 0, maximum: 15000, description: "game: how long after the explosion the after shot is taken (default 3000: the smoke has thinned)." },
      from: vec3("game: the shots' eye [x, y, z] in `units`, looking at the point (default: 4 radii back, at least 8 map units, from the first of eight bearings, south first, and three heights whose line of sight to the point is clear of the project's voxels; the answer's eye gives it)."),
      prefix: { type: "string", description: "game: the shots' name prefix: <prefix>_before.png and <prefix>_after.png (default erg_blast_<n>)." },
      cells: { type: "boolean", description: "List the removed cells per frame (default false: counts only)." } } },
    async handler(s, a: { at: number[]; radius: number; units?: Units; apply?: boolean; game?: boolean; wormDamage?: number; wormRadius?: number; impulse?: number; shots?: boolean; settleMs?: number; cells?: boolean;
      from?: number[]; prefix?: string }) {
      const { store } = s.requireProject();
      const u = a.units ?? "world", k = u === "world" ? WORLD_PER_MAP : 1;
      const p: Vec3 = [a.at[0] / k, a.at[1] / k, a.at[2] / k], r = a.radius / k;
      const diff = blastDiff(s, p, r);
      const out: Record<string, unknown> = {
        at: pointOut(p, u), radius: a.radius, units: u, removed: diff.removed, ...(diff.kept ? { permanent: diff.kept } : {}),
        frames: diff.frames.map((f) => ({ id: f.id, name: f.name, removed: f.removed, ...(f.permanent ? { permanent: true } : {}), ...(a.cells ? { cells: f.cells } : {}) })),
        note: "predicted from the project: the solid cells whose centre is within the radius (the engine carves a sphere of its land damage radius; PERM frames are permanent)",
      };
      if (a.apply) {
        if (!diff.removed) throw new ToolError("nothing to carve: no solid cell of a carvable frame is within the radius");
        const e = new VoxelEdit(store);
        for (const f of diff.frames) if (!f.permanent) for (const [x, y, z] of f.cells) e.set(f.id, x, y, z, carved(e.get(f.id, x, y, z)));
        const why = e.check();
        if (why) throw new ToolError(why);
        e.commit("Test explosion");
        out.applied = { carved: e.size, undo: "undo puts the cells back", ...(diff.frames.some((f) => f.removed > f.cells.length) ? { note: "at most 2000 cells a frame were carved" } : {}) };
      }
      if (!a.game) return out;
      const conn = await s.ensure();
      if (!conn.methods.includes("level.test.explode")) throw new ToolError("this game has no level.test.explode: game needs a Melange with erg-mcp's test hooks, running a Test");
      const knot = knotNear(s, p, 1);
      if (!knot) throw new ToolError(`no ${BLAST_PREFIX} knot within 1 map unit of the point: place_blast_points {points: [at]}, save, test {restart: true}, then test_explosion again`);
      const parts: Content[] = [];
      const shotsOn = a.shots ?? true;
      // a camera 3 radii back to the south and up, looking at the point
      // the eye: `from`, else the first pose with a clear view of the point (warned when none is, or `from` is not)
      const gs = grids(store), back = Math.max(8, r * 4), fromMap = a.from ? [a.from[0] / k, a.from[1] / k, a.from[2] / k] : null;
      const pick = fromMap ? { eye: fromMap, clear: !blocked(gs, fromMap, p) } : blastEye(gs, p, back);
      const cam = { pos: pick.eye, target: [p[0], p[1], p[2]], units: "map", fov: 55 };
      const shot = async (name: string) => {
        const res = await call("game_camera").handler(s, { ...cam, seconds: 4, shot: true, clean: true, width: 960, prefix: tag, name }) as { content: Content[] };
        parts.push(...res.content.filter((c) => c.type === "image"));
      };
      const tag = a.prefix ?? `erg_blast_${Date.now() % 100000}`;
      if (shotsOn) await shot("before");
      const res = await (await s.client()).call<{ queued: boolean; tick: number }>("level.test.explode", {
        detail: knot.name, landRadius: r * WORLD_PER_MAP, wormDamage: a.wormDamage ?? 0, wormRadius: (a.wormRadius ?? 0) / k * WORLD_PER_MAP, impulse: a.impulse ?? 0, impulseRadius: 0,
      }, 15000).catch((e: Error) => { throw new ToolError(`level.test.explode refused: ${e.message}`); });
      await new Promise((ok) => setTimeout(ok, a.settleMs ?? 3000));
      if (shotsOn) {
        await shot("after");
        await call("game_camera").handler(s, { clear: true });
      }
      out.game = { detail: knot.name, knotAt: pointOut(knot.at, u), queuedAtTick: res.tick, ...(shotsOn ? { shots: [`${tag}_before`, `${tag}_after`], note: "Documents\\Melange\\shots",
        eye: pointOut(pick.eye as Vec3, u), ...(pick.clear ? {} : { warning: a.from ? "solid voxels lie between `from` and the point: the shots may show the inside of the ground" : "no pose round the point has a clear view of it: the shots look from the south" }) } : {}) };
      return { content: [...parts, { type: "text", text: JSON.stringify(out, null, 1) }] };
    },
  },
];

