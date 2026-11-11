// Detail layer round 13 (the round-12 judge: "fix cohesion at the material level, because that is now the ceiling"):
//   ground   the base terrain's look through the primary paint of solid voxels (bits 2-7, a visual channel: the fingerprint
//            hashes it apart, docs/erg-mcp.md "Detail layer"), never the second material or the corner mask (sim):
//            - steep: a draped skin cell whose steepest triangle is over STEEP degrees draws its material's SIDE texture,
//              and the side tile of path02 (dust), grass01 and rock02 is a pale grey (P05 / P07): the grey-blue wedge faces
//              on the cliffs. Those cells take VolcanoRock (P57 on every face), so a cliff reads as basalt. The released
//              Sunstone has no landscape tint, so the round-5 tint of P05 / P07 never reached the player.
//            - shore: the lawn and sand cells round the castles were a checker (single bright grass01 squares in sand, dark
//              VolcanoRock squares on the beach). They are repainted from one smooth noise field: sand at the waterline,
//              organic olive grass02 patches with grass01 cores further up, so the green reads as meadow, not tiles.
//   darken   rock copies that stand on bare basalt, lava rock or in the crater (the rim stacks the judge saw as "pale beige
//            cardboard boxes") take the dark, nearly dustless variants (meshes/gen.mjs *Dark); the warm dusty rock stays on
//            dust, ash, sand and grass
//   toe      coverage: rock groups seated at the TOE of the cone's cliff faces, on the steep ground just outside the trail
//            kerbs (no worm walks there: over WALK degrees) and on the faces between the legs, a big anchor (1.7-2.6) with
//            2-4 smaller pieces downhill, so the cone reads dressed from the gameplay cameras, not only close up
//   debris   the beach's uniform small blocks: a lone or tipped-up small block on beach ground is removed
// All visual channels: primary paint and scenery copies. detail.mjs proves the sim fingerprint after each pass.
import * as T from "./terrain.mjs";
import { meshBoxes, sampler, contact, clearOf, boulderPiece } from "./detail-basalt.mjs";
import { rng } from "./detail-rock.mjs";
import { trailDist, TRAIL_BAND } from "./detail-keepout.mjs";
import { ROCK_VARIANTS, DARK_VARIANTS } from "./meshes/gen.mjs";

// materials (ThemePrehistoric.txt order, erg-mcp lib/landtint.ts)
export const MAT = { grass01: 0, grass01blend: 1, grass02: 2, grass02blend: 3, rock01: 4, rock02: 6, rock02blend: 7, path01: 8, path02: 9, sand01: 10, GreySlateFloor: 19, GreySlateFloorBLEND: 20, VolcanoRock: 49, Magma: 50, HotRocks: 51 };
// the materials whose steep faces draw a pale tile (P05 or P07)
const PALE_SIDE = [MAT.grass01, MAT.grass01blend, MAT.grass02, MAT.grass02blend, MAT.rock01, MAT.rock02, MAT.rock02blend, MAT.path01, MAT.path02, MAT.GreySlateFloor, MAT.GreySlateFloorBLEND];
export const STEEP = +(process.env.DTL_STEEP ?? 50);
// the shore field: n = fbm over the island, grass where it is over GRASS, the bright cores over CORE
const N = "fbm(x * 0.075, z * 0.075, 31, 3)";
export const GRASS = +(process.env.DTL_GRASS ?? 0.12), CORE = +(process.env.DTL_CORE ?? 0.4);

export async function groundPass(ctx) {
  const { call } = ctx;
  const paint = async (label, args) => {
    const r = await call("paint_region", { units: "map", depth: 3, ...args }, { soft: true });
    if (r.err && !/nothing to repaint/.test(r.text)) throw new Error(`${label}: ${r.text.slice(0, 300)}`);
    return r.err ? 0 : r.json.changed;
  };
  const out = {};
  out.steep = await paint("steep", { material: MAT.VolcanoRock, only: PALE_SIDE, mask: `s - ${STEEP}`, depth: 6 });
  // the shore band: over the waterline, under the cone's foot, walkable slope
  const band = (lo) => `9.5 - h, h - ${lo}, ${STEEP} - s`;
  // the beach under the cone's foot: the basalt squares dithered into the sand there go to sand (the cone's basalt starts
  // where the ground steepens)
  out.beachRock = await paint("beach rock", { material: MAT.sand01, only: [MAT.VolcanoRock], mask: "min(6.5 - h, h - 1.2, 30 - s)" });
  out.sand = await paint("sand", { material: MAT.sand01, only: [MAT.grass01, MAT.grass02], mask: `min(${band(1.2)}, ${GRASS} - ${N})` });
  out.grass = await paint("grass", { material: MAT.grass02, only: [MAT.grass01, MAT.sand01], mask: `min(${band(2.6)}, ${N} - ${GRASS}, ${CORE} - ${N})` });
  out.core = await paint("core", { material: MAT.grass01, only: [MAT.grass02, MAT.sand01], mask: `min(${band(2.9)}, ${N} - ${CORE})` });
  return out;
}

async function allRocks(call, list) {
  const out = [];
  for (const v of list) for (const d of (await call("list_scenery", { search: v, units: "map", limit: 2000 })).json.details) if (d.resource === v) out.push({ id: d.id, v, x: d.at[0], y: d.at[1], z: d.at[2], rot: d.rot ?? [0, 0, 0], scale: (d.scale ?? [1, 1, 1]).map(Math.abs), name: d.name });
  return out;
}

export async function darkenPass(ctx) {
  const { call } = ctx, sample = sampler(call);
  const rocks = await allRocks(call, ROCK_VARIANTS);
  const mats = [];
  for (let i = 0; i < rocks.length; i += 4000) mats.push(...(await call("surface_material", { points: rocks.slice(i, i + 4000).map((r) => [r.x, r.z]) })).json.samples);
  const g = await sample(rocks.map((r) => [r.x, r.z]));
  // the warm, dusty rock only where it lies on walkable dust, ash, sand or grass; everything else is bare basalt
  const WARM = new Set([MAT.path02, MAT.path01, MAT.sand01, MAT.grass01, MAT.grass02]);
  const by = new Map(), why = { crater: 0, bare: 0, steep: 0, warm: 0 };
  rocks.forEach((r, i) => {
    const m = mats[i]?.material, sl = g[i]?.slope ?? 90;
    // the shore's graded tumbles keep their warm stone (the round-12 judge's best composition)
    const shore = /tumble|btum/.test(r.name) || (T.rhoAt(r.x, r.z) > 82 && r.y < 10);
    const w = shore ? "warm" : T.rhoAt(r.x, r.z) < T.RC + 7 ? "crater" : !WARM.has(m) ? "bare" : sl > 35 ? "steep" : "warm";
    why[w]++;
    if (w === "warm") return;
    if (!by.has(r.v)) by.set(r.v, []);
    by.get(r.v).push(r.id);
  });
  const B = meshBoxes();
  let swapped = 0;
  for (const [v, ids] of by) for (let i = 0; i < ids.length; i += 4000)
    swapped += (await call("swap_mesh", { from: v, to: DARK_VARIANTS[v], ids: ids.slice(i, i + 4000), toBox: B[DARK_VARIANTS[v]], keepScale: true })).json.changed;
  return { rocks: rocks.length, swapped, ...why };
}

export async function debrisPass(ctx) {
  const { call } = ctx, sample = sampler(call), B = meshBoxes();
  const rocks = await allRocks(call, [...ROCK_VARIANTS, ...Object.values(DARK_VARIANTS)]);
  const beach = rocks.filter((r) => r.y < 6 && T.rhoAt(r.x, r.z) > 80);
  const g = await sample(beach.map((r) => [r.x, r.z]));
  const ids = [], why = { lone: 0, tipped: 0 };
  beach.forEach((r, i) => {
    const b = B[r.v], w = Math.max((b.max[0] - b.min[0]) * r.scale[0], (b.max[2] - b.min[2]) * r.scale[2]);
    if (w > 1.4 || !g[i] || g[i].slope > 26) return;
    const tilt = Math.max(Math.abs(r.rot[0]), Math.abs(r.rot[2]));
    const near = rocks.filter((q) => q.id !== r.id && Math.hypot(q.x - r.x, q.z - r.z) < 2.6).length;
    if (tilt > 35 && tilt < 145) { ids.push(r.id); why.tipped++; }
    else if (near < 2) { ids.push(r.id); why.lone++; }
  });
  if (ids.length) await call("remove_scenery", { ids });
  return { beach: beach.length, removed: ids.length, why };
}

export const TOE_SITES = +(process.env.DTL_TOE_SITES ?? 44), WALK = 42;
export async function toePass(ctx) {
  const { call } = ctx, R = rng(20261313), sample = sampler(call), clear = clearOf(ctx), B = meshBoxes();
  const existing = await allRocks(call, ROCK_VARIANTS);
  const pts = [];
  for (let x = -96; x <= 96; x += 1.5) for (let z = -96; z <= 96; z += 1.5) { const r = T.rhoAt(x, z); if (r > T.RC + 5 && r < 84) pts.push([x + (R() - 0.5), z + (R() - 0.5)]); }
  const gs = await sample(pts);
  const stats = { cands: 0, sites: 0, pieces: 0, rejected: {} };
  const rej = (w) => { stats.rejected[w] = (stats.rejected[w] ?? 0) + 1; };
  const cands = [];
  pts.forEach((p, i) => {
    const g = gs[i];
    if (!g) return rej("nog");
    if (g.y < 4 || g.y > 50) return rej("height");
    if (g.slope < WALK || g.slope > 66) return rej("slope");
    const td = trailDist(p[0], p[1]);
    if (td < TRAIL_BAND + 1.2) return rej("trail");
    { const w = clear(p[0], p[1], 1.6); if (w) return rej("clear: " + w); }
    if (existing.some((q) => Math.hypot(q.x - p[0], q.z - p[1]) < 4)) return rej("rock");
    // the toe: steeper ground rises uphill of the point within 3 units (a face's foot), and the kerb's edge is near
    const n = g.normal, dl = Math.hypot(n[0], n[2]) || 1, up = [-n[0] / dl, -n[2] / dl];
    cands.push({ x: p[0], z: p[1], g, up, w: R() * 0.6 + (td < TRAIL_BAND + 6 ? 0.6 : 0) + 0.5 * T.noise(p[0] * 0.06, p[1] * 0.06, 4242) });
  });
  stats.cands = cands.length;
  const ups = await sample(cands.map((c) => [c.x + c.up[0] * 2.5, c.z + c.up[1] * 2.5]));
  cands.forEach((c, i) => { if (ups[i] && ups[i].slope > c.g.slope + 6) c.w += 0.5; });
  const sites = [];
  for (const c of cands.sort((a, b) => b.w - a.w)) { if (sites.length >= TOE_SITES) break; if (sites.every((s) => Math.hypot(s.x - c.x, s.z - c.z) > 7)) sites.push(c); }
  const out = [];
  for (const s of sites) {
    const dn = [-s.up[0], -s.up[1]], ang = Math.atan2(dn[1], dn[0]), c = Math.cos(ang), sn = Math.sin(ang);
    const set = [["caldera.Boulder", 1.7 + R() * 0.9, 0, 0, 0.42], ["caldera.Wedge", 1.1 + R() * 0.5, 1.5, 1.0, 0.4], ["caldera.Slab", 1.0 + R() * 0.4, 1.2, -1.3, 0.45],
      ["caldera.Shard", 0.8 + R() * 0.3, 2.6, 0.2, 0.5], ["caldera.Wedge", 0.75 + R() * 0.3, -0.6, 1.7, 0.5]].slice(0, 3 + Math.floor(R() * 3));
    const pcs = set.map(([v, size, a, b, embed]) => ({ v, size, embed, x: s.x + a * c - b * sn, z: s.z + a * sn + b * c }));
    const ss = await sample(pcs.map((p) => [p.x, p.z]));
    let kept = 0;
    pcs.forEach((p, i) => {
      const g = ss[i];
      if (!g || g.y < 3 || g.slope > 68 || contact(g.normal) > 66) return;
      if (g.slope < WALK && p.size > 1.0) return;
      if (clear(p.x, p.z, p.size * 0.5) || trailDist(p.x, p.z) < TRAIL_BAND + 0.6 + p.size * 0.5) return;
      if (out.some((q) => Math.hypot(q.x - p.x, q.z - p.z) < (q.size + p.size) * 0.38)) return;
      out.push({ ...p, piece: boulderPiece(B[p.v], p.x, p.z, g, p.size, p.embed, p.v === "caldera.Slab" ? 1 : 0.85, R) }); kept++;
    });
    if (kept >= 2) stats.sites++;
  }
  const by = new Map();
  for (const p of out) { if (!by.has(p.v)) by.set(p.v, []); by.get(p.v).push(p.piece); }
  for (const [v, list] of by) for (let i = 0; i < list.length; i += 200) stats.pieces += (await call("place_mod_mesh", { resource: v, box: B[v], stem: "toe", units: "map", tag: "dtl", pieces: list.slice(i, i + 200) })).json.placed;
  return stats;
}
