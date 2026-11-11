// Camera and water sanity of the spawn knots, for list_objects and place_spawn_knots: a few column reads around each
// spawn (about 80 within the default radius), so it is cheap enough to run on every answer. Map units throughout.
import type { EditorStore, Vec3 } from "../core";
import { markers, spansAt, waterMap } from "./inspect-field";
import { grids, type Grid } from "./voxel";

/** How far above the ground a worm stands on its turn camera sits: an assumption (the turn camera sits behind and above
 * the worm, about 120 world units up), not read from the game. */
export const CAMERA_RISE = 6;
/** How far from the spawn, horizontally, ground at the camera's height is looked for. */
export const WALL_RADIUS = 2.5;

export interface SpawnCheck {
  name: string; pos: Vec3;
  standOn: number | null;                                  // the top of the ground it stands on (null: none under it)
  wall: { distance: number; top: number; x: number; z: number } | null;   // the nearest column solid at the camera's height
  underWater: boolean;                                     // standOn is at or under the water
}

/** The nearest column within `radius` of (x, z) that is solid at height y (sampled every half map unit), or null. */
export function solidNear(gs: Grid[], x: number, z: number, y: number, radius: number): SpawnCheck["wall"] {
  let best: SpawnCheck["wall"] = null;
  const n = Math.ceil(radius / 0.5);
  for (let j = -n; j <= n; j++)
    for (let i = -n; i <= n; i++) {
      const d = Math.hypot(i, j) * 0.5;
      if (d > radius + 1e-9 || (best && d >= best.distance)) continue;
      for (const s of spansAt(gs, x + i * 0.5, z + j * 0.5))
        if (s.bottom <= y && s.top > y) { best = { distance: d, top: s.top, x: x + i * 0.5, z: z + j * 0.5 }; break; }
    }
  return best;
}

export function checkSpawns(store: EditorStore, radius = WALL_RADIUS, rise = CAMERA_RISE): SpawnCheck[] {
  const gs = grids(store), water = waterMap(store);
  return markers(store).filter((m) => m.kind === "spawn").map((m) => {
    // what it stands on: the highest ground not above 1.5 over the knot (check_reachability's rule)
    const g = spansAt(gs, m.pos[0], m.pos[2], m.pos[1] + 1.5)[0];
    const standOn = g ? g.top : null;
    return { name: m.name, pos: m.pos, standOn, wall: standOn === null ? null : solidNear(gs, m.pos[0], m.pos[2], standOn + rise, radius),
      underWater: standOn !== null && water !== null && standOn <= water };
  });
}

/** The checks as warning lines; `len` writes a map length in the caller's units (with the unit). */
export function spawnWarnings(store: EditorStore, len: (map: number) => string, radius = WALL_RADIUS, rise = CAMERA_RISE): string[] {
  const out: string[] = [];
  const water = waterMap(store);
  for (const c of checkSpawns(store, radius, rise)) {
    if (c.wall) out.push(`warning: ${c.name}'s turn camera may start inside a wall: ground ${len(c.wall.distance)} away rises to ${len(c.wall.top - c.standOn!)} above the ground it stands on (the camera sits about ${len(rise)} above the worm; checked within ${len(radius)})`);
    if (c.underWater) out.push(`warning: ${c.name} stands on ground under the water (ground y ${len(c.standOn!)}, water y ${len(water!)})`);
  }
  return out;
}

// ------------------------------------------------------------------ treads
// A spawn knot is where the game puts the worm, which then falls to the ground under it. A knot inside solid ground
// (a hand-placed knot in a tower's wall) leaves the worm stuck or dropped elsewhere, a knot with no ground near under it
// drops the worm far, and one on a ledge narrower than a worm (a spawn spot rounded off a wall-walk's edge) lets it
// slide off. These checks read the voxels' spans untruncated, so a knot inside solid is not read as standing on it.

/** Half a worm's width, map units: its body is about 11 world units wide (0.55 map), plus a margin. */
export const WORM_HALF = 0.4;
/** Ground farther under a knot than this (map units) is a drop, not a stand. */
export const GROUND_BELOW = 3;
/** The air a worm needs over the ground it stands on (map units). */
export const WORM_HEADROOM = 1.5;

export interface Tread {
  ok: boolean;
  problems: string[];          // why not, in words
  kinds: ("inSolid" | "noGround" | "noHeadroom" | "edge")[];
  standOn: number | null;      // the ground it would stand on (the highest top not over the knot), null: none
  snap?: Vec3 | null;          // the nearest point that passes (asked for), null: none within the radius
}

const r2 = (v: number) => Math.round(v * 100) / 100;

/** The spans of the column at (x, z), untruncated, highest first. */
const column = (gs: Grid[], x: number, z: number) => spansAt(gs, x, z);

/** Ground about level with `top` (within half a map unit) or solid covering that height, in the column at (x, z). */
function supportAt(gs: Grid[], x: number, z: number, top: number): boolean {
  return column(gs, x, z).some((s) => s.top >= top - 0.5 && s.bottom <= top + 0.25);
}

/** The checks for a worm put at p (map units): p not inside solid, ground within `below` under it, `headroom` of air
 * over that ground, and ground round it at least `half` out in eight directions (else it is on an edge). */
export function treadAt(gs: Grid[], p: Vec3, opts: { below?: number; half?: number; headroom?: number } = {}): Tread {
  const below = opts.below ?? GROUND_BELOW, half = opts.half ?? WORM_HALF, head = opts.headroom ?? WORM_HEADROOM;
  const spans = column(gs, p[0], p[2]), problems: string[] = [], kinds: Tread["kinds"] = [];
  const buried = spans.find((s) => s.bottom <= p[1] + 0.1 && s.top > p[1] + 0.1 + 1e-6);
  if (buried) { kinds.push("inSolid"); problems.push(`the knot is inside solid ground (solid from y ${r2(buried.bottom)} to ${r2(buried.top)} at its point)`); }
  const ground = spans.find((s) => s.top <= p[1] + 0.1 + 1e-6);
  const standOn = ground ? ground.top : null;
  if (!buried && (standOn === null || p[1] - standOn > below + 1e-6)) {
    kinds.push("noGround");
    problems.push(standOn === null ? "no ground under it" : `no ground within ${below} map units under it (the ground is ${r2(p[1] - standOn)} under the knot)`);
  }
  if (standOn !== null && !buried) {
    const roof = spans.find((s) => s.bottom > standOn + 1e-6 && s.bottom < standOn + head - 1e-6);
    if (roof) { kinds.push("noHeadroom"); problems.push(`no room for a worm over its ground (solid ${r2(roof.bottom - standOn)} over it)`); }
    let missing = 0;
    for (let i = 0; i < 8; i++) {
      const a = (i * Math.PI) / 4;
      if (!supportAt(gs, p[0] + Math.cos(a) * half, p[2] + Math.sin(a) * half, standOn)) missing++;
    }
    if (missing) { kinds.push("edge"); problems.push(`on an edge narrower than a worm: no ground at its height ${half} map units out in ${missing} of 8 directions`); }
  }
  return { ok: !problems.length, problems, kinds, standOn };
}

/** The nearest point within `radius` (map units, sampled every `step`) whose tread passes, on ground within 2 map units of
 * the height the knot stands at (or is at); null when there is none. */
export function nearestTread(gs: Grid[], p: Vec3, radius = 3, step = 0.25, opts: { below?: number; half?: number; headroom?: number } = {}): Vec3 | null {
  const t0 = treadAt(gs, p, opts);
  const ref = t0.standOn !== null && !t0.kinds.includes("inSolid") ? t0.standOn : p[1];
  const n = Math.ceil(radius / step);
  const cands: { d: number; q: Vec3 }[] = [];
  for (let j = -n; j <= n; j++)
    for (let i = -n; i <= n; i++) {
      const x = p[0] + i * step, z = p[2] + j * step, dh = Math.hypot(i, j) * step;
      if (dh > radius + 1e-9) continue;
      for (const s of column(gs, x, z))
        if (Math.abs(s.top - ref) <= 2 + 1e-9) cands.push({ d: Math.hypot(dh, s.top - p[1]), q: [x, s.top, z] });
    }
  cands.sort((a, b) => a.d - b.d);
  for (const c of cands.slice(0, 600)) if (treadAt(gs, c.q, opts).ok) return [r2(c.q[0]), r2(c.q[1]), r2(c.q[2])];
  return null;
}

/** A spawn knot the base level has, where the base has it: the game's own maps stand their knots on mesh and heightmap
 * ground the voxels do not hold (Multi.DinerMight's diner floor), so the tread checks leave them alone. */
function baseKnot(store: EditorStore, id: number): boolean {
  const d = store.scene.details.find((x) => x.id === id), b = d && d.src !== null ? store.base.details.find((x) => x.id === d.id) : undefined;
  return !!d && !!b && b.frame === d.frame && b.pos.every((v, i) => Math.abs(v - d.pos[i]) < 1e-6);
}

/** The tread check of every spawn knot the project placed or moved (map units), with the nearest passing point for each
 * that fails. */
export function spawnTreads(store: EditorStore, radius = 3): { name: string; pos: Vec3; tread: Tread }[] {
  const gs = grids(store);
  return markers(store).filter((m) => m.kind === "spawn" && !baseKnot(store, m.detail)).map((m) => {
    const tread = treadAt(gs, m.pos);
    if (!tread.ok) tread.snap = nearestTread(gs, m.pos, radius);
    return { name: m.name, pos: m.pos, tread };
  });
}

/** The tread checks as warning lines; `len` writes a map length, `pt` a map point, in the caller's units. */
export function treadWarnings(store: EditorStore, pt: (p: Vec3) => string): string[] {
  return spawnTreads(store).filter((t) => !t.tread.ok).map((t) => `warning: ${t.name} at ${pt(t.pos)}: ${t.tread.problems.join("; ")}; ${t.tread.snap
    ? `the nearest point a worm stands on is ${pt(t.tread.snap)} (place_spawn_knots {snap: "tread"} moves it there)` : "no point a worm stands on within 3 map units"}`);
}
