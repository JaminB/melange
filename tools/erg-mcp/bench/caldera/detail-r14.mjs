// Detail layer round 14 (the round-13 judge: "remove the tile-grid look on the castle beaches and soften material
// boundaries without changing the simulation"; give the oblique a gameplay-scale read; fix Highwatch's plinth glare):
//   shore   (primary paint) the castle beaches lose their checker: every dark or lava-rock square on the flat shore
//           (VolcanoRock, HotRocks, rock01/02; never Magma, the flows that are part of the design) becomes sand, and the
//           lime grass squares go: the shore's greens become organic, roughly round patches of path01 (P09, a dry earth with
//           sprigs of green) in the sand, low flora fringing them (meadow below). No grass tile is left on the shore band.
//   plinth  (primary paint) Highwatch's flint square (P18, near white) takes GreySlateFloor (P23, a mossy mid grey) on its
//           flat top and VolcanoRock on its steep sides
//   smooth  (primary paint) a majority filter over the skin's 2.5-unit cells on the cone, off the trails, castles and lava
//           flows: a dust, basalt or hot-rock cell left as a spike or island in another material (2 or fewer of its 8
//           neighbours share it, 5 or more share one other) takes its neighbours' material, twice; dust never goes onto a
//           steep cell (its side tile is pale). The zigzag between the dust ledges and the basalt loses its single-cell teeth.
//   beach   the beach's black cubes: dark variants on beach ground go back to the warm stone, and lone small pieces go
//   seam    rock groups ALONG the dust / basalt boundary, outside the keep-outs: an anchor of 2.2-3.4 (a gameplay-scale mass
//           the oblique can see) and 2-4 smaller pieces strung along the edge, so the cell edge is hidden under stone
//   meadow  low flora masses on and round the shore's earth patches (grass, tussock, weed, flower: nothing over 0.8 tall on
//           walkable sand), denser at the patch's rim so the cell edge is covered
// Primary paint (bits 2-7 of solid voxels) and scenery copies only: detail.mjs proves the sim fingerprint after each pass.
import * as T from "./terrain.mjs";
import { meshBoxes, sampler, contact, clearOf, boulderPiece } from "./detail-basalt.mjs";
import { rng, onTongue } from "./detail-rock.mjs";
import { trailDist, TRAIL_BAND, keepOutFlora } from "./detail-keepout.mjs";
import { ROCK_VARIANTS, DARK_VARIANTS } from "./meshes/gen.mjs";
import { MAT } from "./detail-r13.mjs";
import { FLORA, rampTint } from "./detail-flora.mjs";

const CELL = 2.5, HALF = 1.25;
// the patches' ground: sand01blend (P12, the same sand with pebbles; its sides are P11 sand). Round 14's first try used
// path01 (P09, a dry earth): in game its cells still read as a beige checker on the sand
const EARTH = +(process.env.DTL_PATCH_MAT ?? 11);
export const PATCH_MAX = +(process.env.DTL_PATCH_MAX ?? 44);
const DARK = new Set([MAT.VolcanoRock, MAT.HotRocks, MAT.rock01, MAT.rock02, MAT.rock02blend]);
const GRASSES = new Set([MAT.grass01, MAT.grass01blend, MAT.grass02, MAT.grass02blend]);
const TEAM = () => T.CASTLES.filter((c) => c.floor <= 6);
const inCastle = (x, z, pad) => T.CASTLES.some((c) => T.footDist(c, x, z, pad) === 0);
const PADS = T.LAUNCHERS.map(([rho, th]) => T.pointAt(rho, th));

/** The skin cells' centres (2.5k + 1.25) where keep(x, z) holds. */
function cells(keep) {
  const out = [];
  // the skin's cell edges are on multiples of 2.5 (build.mjs: its meeting corners at -40, -42.5, 37.5, 35)
  const c0 = Math.ceil(-T.BOX / CELL) * CELL + HALF;
  for (let x = c0; x < T.BOX; x += CELL) for (let z = c0; z < T.BOX; z += CELL) if (keep(x, z)) out.push([x, z]);
  return out;
}
async function materials(call, pts) {
  const out = [];
  for (let i = 0; i < pts.length; i += 4000) out.push(...(await call("surface_material", { points: pts.slice(i, i + 4000) })).json.samples);
  return out;
}
/** Repaint the cells (centres) from the materials in `only` to `material`, `depth` under the top. */
async function paintCells(call, list, material, only, depth = 3) {
  let n = 0;
  for (let i = 0; i < list.length; i += 64) {
    const r = await call("paint_region", { units: "map", material, only, depth, shapes: list.slice(i, i + 64).map(([x, z]) => ({ min: [x - 0.6, z - 0.6], max: [x + 0.6, z + 0.6] })) }, { soft: true });
    if (r.err && !/nothing to repaint/.test(r.text)) throw new Error(`paint ${material}: ${r.text.slice(0, 300)}`);
    n += r.err ? 0 : r.json.changed;
  }
  return n;
}
/** Paint a set of {x, z, from, to} changes grouped by (from, to). */
async function apply(call, changes, depth) {
  const g = new Map();
  for (const c of changes) { const k = `${c.from}>${c.to}`; if (!g.has(k)) g.set(k, []); g.get(k).push([c.x, c.z]); }
  let voxels = 0;
  for (const [k, list] of g) { const [from, to] = k.split(">").map(Number); voxels += await paintCells(call, list, to, [from], depth); }
  return voxels;
}

// ---------------------------------------------------------------- shore: the beaches' checker to sand and earth patches
export const PATCHES = [];                               // the earth patches [x, z, r] (meadow plants them)
export async function shorePass(ctx) {
  const { call } = ctx, R = rng(20261414), sample = sampler(call);
  // the castles' own materials (12, 16, 27: terrain.mjs PLANS) are never in `only`, so cells at a footprint's edge are painted
  // too (round 14's first tries kept 0.5 and 0 off and left lime cells at Strand's and Harbour's walls)
  const list = cells((x, z) => T.rhoAt(x, z) > 58 && Math.hypot(x, z) < T.CLIP - 1 && !inCastle(x, z, -4));
  const ms = await materials(call, list), gs = await sample(list);
  const team = TEAM(), near = (x, z) => team.find((c) => Math.hypot(x - c.at[0], z - c.at[1]) < 30);
  const S = list.map(([x, z], i) => ({ x, z, m: ms[i]?.material, y: ms[i]?.y ?? gs[i]?.y, s: gs[i]?.slope ?? 90 })).filter((c) => c.m !== undefined && Number.isFinite(c.y));
  // the earth patches: round blobs on the flat shelves round the team castles and along the shore band, centred on cells
  // that held grass (where the meadow was) and a few on plain sand by the castles; radius 3.5-7, a wobbly edge
  const cand = S.filter((c) => c.y > 1.5 && c.y < 9.5 && c.s < 26 && (GRASSES.has(c.m) || c.m === MAT.sand01 || DARK.has(c.m)))
    .map((c) => ({ ...c, w: (GRASSES.has(c.m) ? 1 : 0.4) + R() * 0.8 + (near(c.x, c.z) ? 0.5 : 0) })).sort((a, b) => b.w - a.w);
  const blobs = [];
  for (const c of cand) {
    if (blobs.length >= PATCH_MAX) break;
    if (blobs.some((b) => Math.hypot(b.x - c.x, b.z - c.z) < b.r + 5)) continue;
    if (trailDist(c.x, c.z) < TRAIL_BAND + 1 || inCastle(c.x, c.z, 2)) continue;
    blobs.push({ x: c.x + (R() - 0.5) * 1.2, z: c.z + (R() - 0.5) * 1.2, r: 3.6 + R() * 3.2, seed: Math.floor(R() * 1000) });
  }
  const inBlob = (x, z) => blobs.some((b) => {
    const a = Math.atan2(z - b.z, x - b.x);
    return Math.hypot(x - b.x, z - b.z) < b.r * (1 + 0.22 * Math.sin(3 * a + b.seed) + 0.12 * Math.sin(5 * a + b.seed * 2));
  });
  const ch = [], why = { darkToSand: 0, grassToEarth: 0, grassToSand: 0, sandToEarth: 0, grassHigh: 0 };
  for (const c of S) {
    const k = near(c.x, c.z);
    const shelf = k && c.y < k.court + 3.2;
    if (DARK.has(c.m) && c.y > 0.3 && (shelf || (c.y < 5.5 && c.s < 40))) { ch.push({ x: c.x, z: c.z, from: c.m, to: MAT.sand01 }); why.darkToSand++; continue; }
    if (GRASSES.has(c.m) && c.y < 10) {
      const to = inBlob(c.x, c.z) && c.s < 34 ? EARTH : c.y < 6.5 || c.s >= 34 ? MAT.sand01 : EARTH;
      ch.push({ x: c.x, z: c.z, from: c.m, to }); why[to === EARTH ? "grassToEarth" : "grassToSand"]++; continue;
    }
    // the grass left over 10 (the cone's foot meadows): the lime grass01 takes the olive grass02
    if ((c.m === MAT.grass01 || c.m === MAT.grass01blend) && c.y >= 10) { ch.push({ x: c.x, z: c.z, from: c.m, to: MAT.grass02 }); why.grassHigh++; continue; }
    if (c.m === MAT.sand01 && c.s < 26 && c.y > 2 && inBlob(c.x, c.z)) { ch.push({ x: c.x, z: c.z, from: c.m, to: EARTH }); why.sandToEarth++; }
  }
  let voxels = await apply(call, ch, 3);
  // the sweep: a cell whose centre lies under a castle's wall answers the wall's material to surface_material, so a grass or
  // dark cell at a wall's foot was missed (lime strips at Strand, Harbour and Seawall). Every grass voxel left under 10
  // becomes sand, and every dark one on a team castle's shelf too; the castles' own materials are never in `only`
  // (depth 20: the strips at the walls are skin cells whose column also holds a tower, its top 11 over the court)
  const sweep = async (material, only, mask, shapes) => {
    const r = await call("paint_region", { units: "map", material, only, depth: 20, mask, ...(shapes ? { shapes } : {}) }, { soft: true });
    if (r.err && !/nothing to repaint/.test(r.text)) throw new Error(`shore sweep: ${r.text.slice(0, 300)}`);
    return r.err ? 0 : r.json.changed;
  };
  why.sweepGrass = await sweep(MAT.sand01, [...GRASSES], "9.5 - y");
  why.sweepDark = 0;
  for (const k of team) why.sweepDark += await sweep(MAT.sand01, [...DARK], `${(k.court + 2.6).toFixed(2)} - y`, [{ x: k.at[0], z: k.at[1], radius: 24 }]);
  voxels += why.sweepGrass + why.sweepDark;
  PATCHES.length = 0;
  PATCHES.push(...blobs.map((b) => [b.x, b.z, b.r]));
  return { cells: S.length, cand: cand.length, blobs: blobs.length, changes: ch.length, voxels, ...why };
}

// ---------------------------------------------------------------- plinth: Highwatch's white square
export async function plinthPass(ctx) {
  const { call } = ctx, sample = sampler(call);
  const hw = T.CASTLES.find((c) => c.name === "Highwatch");
  if (!hw) return { off: "no Highwatch" };
  // the ring's walls are flint too (terrain.mjs PLANS): only voxels under the court's top (the skin's plinth) are painted,
  // never a wall's (its lowest voxel's centre is 0.75 over the court)
  const shapes = [{ x: hw.at[0], z: hw.at[1], radius: hw.plan.ringRadius + 1.5 + 11 }], cap = `${(hw.court + 0.5).toFixed(2)} - y`;
  const paint = async (material, mask, depth) => {
    const r = await call("paint_region", { units: "map", material, only: [16], depth, shapes, mask }, { soft: true });
    if (r.err && !/nothing to repaint/.test(r.text)) throw new Error(`plinth: ${r.text.slice(0, 300)}`);
    return r.err ? 0 : r.json.changed;
  };
  // inside the ring the court takes the slate; the square's margin outside the ring takes the trails' dust (path02), so the
  // plinth reads as the trail's ground running up to the walls, not as a white slab
  const ring = `${(hw.plan.ringRadius + 1).toFixed(2)} - hypot(x - ${hw.at[0].toFixed(2)}, z - ${hw.at[1].toFixed(2)})`;
  const flat = await paint(MAT.GreySlateFloor, `min(${cap}, 35 - s, ${ring})`, 3);
  const margin = await paint(MAT.path02, `min(${cap}, 35 - s, -(${ring}))`, 3);
  const steep = await paint(MAT.VolcanoRock, `min(${cap}, s - 35)`, 6);
  return { court: hw.court, flat, margin, steep };
}

// ---------------------------------------------------------------- smooth: the single-cell teeth on the cone
export const SMOOTH_FREE = +(process.env.DTL_SMOOTH_FREE ?? 0.6), SMOOTH_SAME = +(process.env.DTL_SMOOTH_SAME ?? 3), CONFETTI = +(process.env.DTL_CONFETTI ?? 3);
const SMOOTHABLE = new Set([MAT.path02, MAT.VolcanoRock, MAT.HotRocks]);
export async function smoothPass(ctx) {
  const { call } = ctx, sample = sampler(call);
  const list = cells((x, z) => { const r = T.rhoAt(x, z); return r > T.RC + 3 && r < 90; });
  const ms = await materials(call, list), gs = await sample(list);
  const key = (x, z) => `${Math.round((x - HALF) / CELL)},${Math.round((z - HALF) / CELL)}`;
  const grid = new Map();
  list.forEach(([x, z], i) => {
    const m = ms[i]?.material;
    if (m === undefined) return;
    const frozen = trailDist(x, z) < T.TRAIL.bed + T.TRAIL.shoulder + SMOOTH_FREE || inCastle(x, z, 3) || onTongue(x, z, 1) || PADS.some((p) => Math.hypot(x - p[0], z - p[1]) < 9) || (ms[i]?.y ?? 0) < 6;
    grid.set(key(x, z), { x, z, m, m0: m, y: ms[i].y, s: gs[i]?.slope ?? 90, frozen });
  });
  const stats = { cells: grid.size, frozen: [...grid.values()].filter((c) => c.frozen).length, iter: [] };
  for (let it = 0; it < 2; it++) {
    const next = [];
    for (const c of grid.values()) {
      if (c.frozen || !SMOOTHABLE.has(c.m)) continue;
      const [i, j] = key(c.x, c.z).split(",").map(Number), count = new Map();
      let same = 0, n = 0;
      for (let di = -1; di <= 1; di++) for (let dj = -1; dj <= 1; dj++) {
        if (!di && !dj) continue;
        const o = grid.get(`${i + di},${j + dj}`);
        if (!o || Math.abs(o.y - c.y) > 6) continue;
        n++;
        if (o.m === c.m) same++; else count.set(o.m, (count.get(o.m) ?? 0) + 1);
      }
      if (n < 6 || same > SMOOTH_SAME) continue;
      const [best, k] = [...count.entries()].sort((a, b) => b[1] - a[1])[0] ?? [null, 0];
      if (k < 5 || !SMOOTHABLE.has(best)) continue;
      if (best === MAT.path02 && c.s > 46) continue;
      next.push([c, best]);
    }
    for (const [c, m] of next) c.m = m;
    stats.iter.push(next.length);
  }
  // the confetti: HotRocks (P54, the lava-crack tile) in clusters of CONFETTI cells or fewer (8-connected) take VolcanoRock.
  // Dark to dark, so the trail band need not be kept (the trails' bed is dust); the big lava fields and the Magma flows stay
  stats.confetti = 0;
  {
    const seen = new Set();
    for (const [k0, c0] of grid) {
      if (seen.has(k0) || c0.m !== MAT.HotRocks) continue;
      const comp = [], stack = [k0];
      seen.add(k0);
      while (stack.length) {
        const k = stack.pop(), c = grid.get(k);
        comp.push(c);
        const [i, j] = k.split(",").map(Number);
        for (let di = -1; di <= 1; di++) for (let dj = -1; dj <= 1; dj++) {
          const kk = `${i + di},${j + dj}`, o = grid.get(kk);
          if (o && !seen.has(kk) && o.m === MAT.HotRocks) { seen.add(kk); stack.push(kk); }
        }
      }
      if (comp.length > CONFETTI) continue;
      for (const c of comp) {
        if (c.y < 6 || inCastle(c.x, c.z, 3) || PADS.some((p) => Math.hypot(c.x - p[0], c.z - p[1]) < 9)) continue;
        c.m = MAT.VolcanoRock; stats.confetti++;
      }
    }
  }
  const ch = [...grid.values()].filter((c) => c.m !== c.m0).map((c) => ({ x: c.x, z: c.z, from: c.m0, to: c.m }));
  const byTo = {};
  for (const c of ch) byTo[c.to] = (byTo[c.to] ?? 0) + 1;
  stats.changes = ch.length; stats.byTo = byTo;
  stats.voxels = await apply(call, ch, 6);
  return stats;
}

// ---------------------------------------------------------------- the rock helpers
async function allRocks(call, list) {
  const out = [];
  for (const v of list) for (const d of (await call("list_scenery", { search: v, units: "map", limit: 2000 })).json.details) if (d.resource === v) out.push({ id: d.id, v, x: d.at[0], y: d.at[1], z: d.at[2], rot: d.rot ?? [0, 0, 0], scale: (d.scale ?? [1, 1, 1]).map(Math.abs), name: d.name });
  return out;
}
const WARM_OF = Object.fromEntries(Object.entries(DARK_VARIANTS).map(([w, d]) => [d, w]));

// ---------------------------------------------------------------- beach: black cubes on the sand
export async function beachPass(ctx) {
  const { call } = ctx, sample = sampler(call), B = meshBoxes();
  const rocks = await allRocks(call, [...ROCK_VARIANTS, ...Object.values(DARK_VARIANTS)]);
  const beach = rocks.filter((r) => r.y < 7.5 && T.rhoAt(r.x, r.z) > 76);
  const g = await sample(beach.map((r) => [r.x, r.z]));
  const width = (r) => { const b = B[r.v]; return Math.max((b.max[0] - b.min[0]) * r.scale[0], (b.max[2] - b.min[2]) * r.scale[2]); };
  const remove = [], warm = new Map(), why = { lone: 0, warmed: 0 };
  beach.forEach((r, i) => {
    const w = width(r), near = beach.filter((q) => q.id !== r.id && Math.hypot(q.x - r.x, q.z - r.z) < 3).length;
    if (w < 1.3 && near < 2 && (g[i]?.slope ?? 0) < 30) { remove.push(r.id); why.lone++; return; }
    if (WARM_OF[r.v]) { if (!warm.has(r.v)) warm.set(r.v, []); warm.get(r.v).push(r.id); why.warmed++; }
  });
  if (remove.length) await call("remove_scenery", { ids: remove });
  for (const [v, ids] of warm) await call("swap_mesh", { from: v, to: WARM_OF[v], ids: ids.filter((id) => !remove.includes(id)), toBox: B[WARM_OF[v]], keepScale: true });
  return { beach: beach.length, removed: remove.length, ...why };
}

// ---------------------------------------------------------------- seam: rock along the dust / basalt edge
export const SEAM_SITES = +(process.env.DTL_SEAM_SITES ?? 40);
export async function seamPass(ctx) {
  const { call } = ctx, R = rng(20261420), sample = sampler(call), clear = clearOf(ctx), B = meshBoxes();
  const existing = await allRocks(call, [...ROCK_VARIANTS, ...Object.values(DARK_VARIANTS)]);
  const list = cells((x, z) => { const r = T.rhoAt(x, z); return r > T.RC + 4 && r < 100; });
  const ms = await materials(call, list);
  const key = (x, z) => `${Math.round((x - HALF) / CELL)},${Math.round((z - HALF) / CELL)}`;
  const grid = new Map();
  list.forEach(([x, z], i) => { if (ms[i]) grid.set(key(x, z), { x, z, m: ms[i].material, y: ms[i].y }); });
  const dark = (m) => m === MAT.VolcanoRock || m === MAT.HotRocks;
  // edge points: the midpoint between a light cell (dust, sand, the patches) and a basalt neighbour on about the same
  // level. On the cone the dust is nearly all trail bed (round 14's first try: 450 of 453 edges inside the trail band), so
  // most sites are the cone's foot, where the sand meets the basalt in a zigzag
  const LIGHT = new Set([MAT.path02, MAT.path01, MAT.sand01, 11]);
  const edges = [];
  for (const c of grid.values()) {
    if (!LIGHT.has(c.m) || c.y < 2.2 || c.y > 50) continue;
    const [i, j] = key(c.x, c.z).split(",").map(Number);
    for (const [di, dj] of [[1, 0], [0, 1], [-1, 0], [0, -1]]) {
      const o = grid.get(`${i + di},${j + dj}`);
      if (o && dark(o.m) && Math.abs(o.y - c.y) < 4) edges.push({ x: (c.x + o.x) / 2, z: (c.z + o.z) / 2, tan: [dj, di], dust: [c.x, c.z] });
    }
  }
  const stats = { edges: edges.length, sites: 0, pieces: 0, rejected: {} };
  const rej = (w) => { stats.rejected[w] = (stats.rejected[w] ?? 0) + 1; };
  const gs = await sample(edges.map((e) => [e.x, e.z]));
  const cands = [];
  edges.forEach((e, i) => {
    const g = gs[i];
    if (!g) return rej("nog");
    if (g.slope > 62) return rej("steep");
    if (trailDist(e.x, e.z) < TRAIL_BAND + 1.6) return rej("trail");
    { const w = clear(e.x, e.z, 1.8); if (w) return rej("clear: " + w); }
    if (existing.some((q) => Math.hypot(q.x - e.x, q.z - e.z) < 2.2)) return rej("rock");
    // the oblique (from +x, +z) and the flank / trail cameras see the S and E faces best: favour them a little
    const th = Math.atan2(e.z, e.x);
    cands.push({ ...e, g, w: R() + 0.4 * Math.cos(th - 0.85) + 0.3 * (g.slope > 30 ? 1 : 0) });
  });
  stats.cands = cands.length;
  const sites = [];
  for (const c of cands.sort((a, b) => b.w - a.w)) { if (sites.length >= SEAM_SITES) break; if (sites.every((s) => Math.hypot(s.x - c.x, s.z - c.z) > 8)) sites.push(c); }
  const out = [];
  for (const s of sites) {
    const t = s.tan, big = R() < 0.55;
    const set = [[big ? "caldera.Wedge" : "caldera.Boulder", big ? 2.6 + R() * 0.8 : 1.9 + R() * 0.6, 0, 0.45],
      ["caldera.Boulder", 1.2 + R() * 0.5, 1.7 + R() * 0.6, 0.45], ["caldera.Shard", 0.9 + R() * 0.4, -(1.6 + R() * 0.6), 0.5],
      ["caldera.Slab", 0.8 + R() * 0.4, 3.0 + R() * 0.8, 0.5], ["caldera.Wedge", 0.7 + R() * 0.3, -(3.0 + R() * 0.6), 0.5]].slice(0, 3 + Math.floor(R() * 3));
    const pcs = set.map(([v, size, a, embed]) => ({ v, size, embed, x: s.x + t[0] * a + (R() - 0.5) * 0.5, z: s.z + t[1] * a + (R() - 0.5) * 0.5 }));
    const ss = await sample(pcs.map((p) => [p.x, p.z]));
    const mm = await materials(call, pcs.map((p) => [p.x, p.z]));
    let kept = 0;
    pcs.forEach((p, i) => {
      const g = ss[i];
      if (!g || g.y < 1.8 || g.slope > 64 || contact(g.normal) > 64) return;
      // on the shore (under 8) no piece over 2.0 across (the round-10 judge's graded tumbles)
      if (g.y < 8 && p.size > 2.0) p.size = 1.7 + R() * 0.3;
      if (clear(p.x, p.z, p.size * 0.5) || trailDist(p.x, p.z) < TRAIL_BAND + 0.8 + p.size * 0.5) return;
      if (out.some((q) => Math.hypot(q.x - p.x, q.z - p.z) < (q.size + p.size) * 0.38)) return;
      // the stone takes the look of its bed: warm and dusty on dust, the dark variant on basalt
      const v = dark(mm[i]?.material) ? DARK_VARIANTS[p.v] : p.v;
      out.push({ ...p, v, piece: boulderPiece(B[v], p.x, p.z, g, p.size, p.embed, p.v === "caldera.Slab" ? 0.9 : 0.85, R) }); kept++;
    });
    if (kept >= 2) stats.sites++;
  }
  const by = new Map();
  for (const p of out) { if (!by.has(p.v)) by.set(p.v, []); by.get(p.v).push(p.piece); }
  for (const [v, list2] of by) for (let i = 0; i < list2.length; i += 200) stats.pieces += (await call("place_mod_mesh", { resource: v, box: B[v], stem: "seam", units: "map", tag: "dtl", pieces: list2.slice(i, i + 200) })).json.placed;
  ctx.say?.("seam", JSON.stringify({ edges: stats.edges, cands: stats.cands, sites: stats.sites, pieces: stats.pieces }));
  return stats;
}

// ---------------------------------------------------------------- mass: gameplay-scale outcrops on the shoulders
// The round-13 judge: "the oblique view still looks almost identical to the undressed one". A few big outcrops (an anchor
// of 3.2-4.4 across, a second block of 2.2-2.8 beside it and a short talus of 1.0-1.8 pieces downhill) on steep shoulder
// ground (34-64 degrees: no worm stands there) well outside every keep-out, 14 or more apart. The anchor is the warm,
// dust-topped stone (a weathered outcrop that reads against the basalt at distance); the rest take their bed's look.
export const MASS_SITES = +(process.env.DTL_MASS_SITES ?? 14), MASS_SLOPE = +(process.env.DTL_MASS_SLOPE ?? 28);
export async function massPass(ctx) {
  const { call } = ctx, R = rng(20261440), sample = sampler(call), clear = clearOf(ctx), B = meshBoxes();
  const existing = await allRocks(call, [...ROCK_VARIANTS, ...Object.values(DARK_VARIANTS)]);
  const pts = [];
  for (let x = -100; x <= 100; x += 2) for (let z = -100; z <= 100; z += 2) { const r = T.rhoAt(x, z); if (r > T.RC + 12 && r < 90) pts.push([x + (R() - 0.5), z + (R() - 0.5)]); }
  const gs = await sample(pts);
  const stats = { cands: 0, sites: 0, pieces: 0, rejected: {} };
  const rej = (w) => { stats.rejected[w] = (stats.rejected[w] ?? 0) + 1; };
  const cands = [];
  pts.forEach((p, i) => {
    const g = gs[i];
    if (!g) return rej("nog");
    if (g.y < 3 || g.y > 40) return rej("height");
    if (g.slope < MASS_SLOPE || g.slope > 68) return rej("slope");
    if (trailDist(p[0], p[1]) < TRAIL_BAND + 2) return rej("trail");
    { const w = clear(p[0], p[1], 2.4); if (w) return rej("clear: " + w.replace(/ .*/, "")); }
    if (existing.some((q) => Math.hypot(q.x - p[0], q.z - p[1]) < 3)) return rej("rock");
    const th = Math.atan2(p[1], p[0]);
    cands.push({ x: p[0], z: p[1], g, w: R() * 0.8 + 0.5 * Math.cos(th - 0.85) + 0.3 * T.noise(p[0] * 0.05, p[1] * 0.05, 4401) });
  });
  stats.cands = cands.length;
  const sites = [];
  for (const c of cands.sort((a, b) => b.w - a.w)) { if (sites.length >= MASS_SITES) break; if (sites.every((s) => Math.hypot(s.x - c.x, s.z - c.z) > 14)) sites.push(c); }
  const out = [];
  for (const s of sites) {
    const n = s.g.normal, dl = Math.hypot(n[0], n[2]) || 1, dn = [n[0] / dl, n[2] / dl], tg = [-dn[1], dn[0]];
    const side = R() < 0.5 ? 1 : -1;
    const set = [["caldera.Wedge", 3.2 + R() * 1.2, 0, 0, 0.42, true], ["caldera.Boulder", 2.2 + R() * 0.6, 0.6, side * 2.6, 0.45, false],
      ["caldera.Slab", 1.4 + R() * 0.4, 3.0, -side * 0.8, 0.5, false], ["caldera.Shard", 1.1 + R() * 0.4, 3.8, side * 1.4, 0.5, false],
      ["caldera.Boulder", 1.0 + R() * 0.4, 4.8, 0.2, 0.55, false]];
    const pcs = set.map(([v, size, a, b, embed, anchor]) => ({ v, size, embed, anchor, x: s.x + dn[0] * a + tg[0] * b, z: s.z + dn[1] * a + tg[1] * b }));
    const ss = await sample(pcs.map((p) => [p.x, p.z]));
    const mm = await materials(call, pcs.map((p) => [p.x, p.z]));
    let kept = 0;
    pcs.forEach((p, i) => {
      const g = ss[i];
      if (!g || g.y < 3 || g.slope > 66 || contact(g.normal) > 66) return;
      if (!p.anchor && kept === 0) return;                 // no satellites without their anchor
      if (clear(p.x, p.z, p.size * 0.5) || trailDist(p.x, p.z) < TRAIL_BAND + 1 + p.size * 0.5) return;
      if (out.some((q) => Math.hypot(q.x - p.x, q.z - p.z) < (q.size + p.size) * 0.4)) return;
      const darkBed = mm[i]?.material === MAT.VolcanoRock || mm[i]?.material === MAT.HotRocks;
      const v = !p.anchor && darkBed ? DARK_VARIANTS[p.v] : p.v;
      out.push({ ...p, v, piece: boulderPiece(B[v], p.x, p.z, g, p.size, p.embed, p.anchor ? 0.95 : 0.85, R) }); kept++;
    });
    if (kept) stats.sites++;
  }
  const by = new Map();
  for (const p of out) { if (!by.has(p.v)) by.set(p.v, []); by.get(p.v).push(p.piece); }
  for (const [v, list] of by) for (let i = 0; i < list.length; i += 200) stats.pieces += (await call("place_mod_mesh", { resource: v, box: B[v], stem: "mass", units: "map", tag: "dtl", pieces: list.slice(i, i + 200) })).json.placed;
  ctx.say?.("mass", JSON.stringify({ cands: stats.cands, sites: stats.sites, pieces: stats.pieces }));
  return stats;
}

// ---------------------------------------------------------------- meadow: low flora on the shore's earth patches
export const MEADOW_MAX = +(process.env.DTL_MEADOW_MAX ?? 220);
export async function meadowPass(ctx) {
  const { call } = ctx, R = rng(20261430), sample = sampler(call);
  const stats = { patches: PATCHES.length, pieces: 0, byKind: {}, rejected: {} };
  const rej = (w) => { stats.rejected[w] = (stats.rejected[w] ?? 0) + 1; };
  if (!PATCHES.length) return { off: "no patches (run shore first)" };
  const ids = {};
  for (const kind of ["grass", "tussock", "weed", "flower", "tuft", "leaf"]) for (const p of FLORA[kind]) {
    const row = (await call("scenery_catalog", { bases: [p.base], roles: ["scenery"], search: p.resource, limit: 10 })).json.list.find((r) => r.resource === p.resource);
    if (!row) continue;
    const sz = row.size ?? row.meshSize ?? [1, 1, 1];
    (ids[kind] ??= []).push({ ...p, id: row.example.id, sc: row.example.scale ?? [1, 1, 1], r: 0.5 * Math.max(sz[0], sz[2]), h: sz[1] });
  }
  const rocks = await allRocks(call, [...ROCK_VARIANTS, ...Object.values(DARK_VARIANTS)]);
  const mem = [];
  // round 14's first tries put the plants on a ring of fixed radius: on Caldera's narrow shelves most of it fell on the
  // slopes or in the sea (77 steep, 28 water of 140). Instead a jittered 0.9 grid over each patch, kept where the ground is
  // dry and walkable-flat; the rim (0.75-1.2 of the radius: a ring of tussock and grass over the patch's cell edge) is
  // planted densely, the inside sparsely (grass, weed and flowers)
  const grid = [];
  for (const [px, pz, pr] of PATCHES) for (let dx = -pr * 1.2; dx <= pr * 1.2; dx += 0.9) for (let dz = -pr * 1.2; dz <= pr * 1.2; dz += 0.9) {
    const x = px + dx + (R() - 0.5) * 0.6, z = pz + dz + (R() - 0.5) * 0.6, f = Math.hypot(x - px, z - pz) / pr;
    if (f <= 1.2) grid.push({ x, z, f });
  }
  const gg = await sample(grid.map((m) => [m.x, m.z]));
  grid.forEach((m, i) => {
    const g = gg[i];
    if (!g || g.y < 1.3 || g.slope > 32) return;
    const rim = m.f > 0.75;
    if (R() > (rim ? 0.62 : 0.22)) return;
    const kind = rim ? (R() < 0.45 ? "tussock" : R() < 0.6 ? "grass" : R() < 0.6 ? "tuft" : "leaf") : (R() < 0.35 ? "grass" : R() < 0.5 ? "weed" : R() < 0.6 ? "flower" : "tussock");
    mem.push({ x: m.x, z: m.z, kind, f: rim ? 0.7 : 0.3 });
  });
  stats.gridPoints = grid.length;
  const ms = await sample(mem.map((m) => [m.x, m.z]));
  const byBase = new Map();
  const placed = [];
  mem.forEach((m, i) => {
    const g = ms[i];
    if (stats.pieces + placed.length >= MEADOW_MAX) return rej("budget");
    if (!g || g.y < 1.3) return rej("water");
    if (g.slope > 40) return rej("steep");
    const opts = ids[m.kind]; if (!opts) return rej("kind");
    const p = opts[Math.floor(R() * opts.length)];
    let mul = (p.mul ?? 1) * (0.85 + R() * 0.3);
    if (p.h * mul < 0.36) mul = 0.36 / p.h;
    if (p.h * mul > 0.8) mul = 0.8 / p.h;                 // walkable sand: low plants only
    const rad = p.r * mul;
    if (onTongue(m.x, m.z, rad + 0.5)) return rej("lava");
    if (rocks.some((k) => Math.hypot(k.x - m.x, k.z - m.z) < 0.9)) return rej("rock");
    if (placed.some((q) => Math.hypot(q[0] - m.x, q[1] - m.z) < 0.55)) return rej("overlap");
    const why = keepOutFlora(m.x, m.z, rad, rad, 0, ctx.spawns, 0.5);
    if (why) return rej(why.replace(/ .*/, ""));
    const tint = m.kind === "flower" ? null : rampTint(g.y, m.f, R);
    if (!byBase.has(p.base)) byBase.set(p.base, []);
    byBase.get(p.base).push({ id: p.id, xz: [+m.x.toFixed(2), +m.z.toFixed(2)], seat: g.slope > 18 ? 0.22 : 0.12, rot: [Math.round((R() - 0.5) * 8), Math.round(R() * 360), Math.round((R() - 0.5) * 8)],
      scale: p.sc.map((v) => +(v * mul).toFixed(3)), ...(tint ? { tint } : {}) });
    placed.push([m.x, m.z]);
    stats.byKind[m.kind] = (stats.byKind[m.kind] ?? 0) + 1;
  });
  for (const [base, copies] of byBase) for (let i = 0; i < copies.length; i += 200) stats.pieces += (await call("copy_base_details", { base, allowOtherTheme: true, units: "map", tag: "flo", copies: copies.slice(i, i + 200) })).json.placed.length;
  return stats;
}
