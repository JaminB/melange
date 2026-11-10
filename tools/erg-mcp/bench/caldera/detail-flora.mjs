// Detail layer round 6: FLORA in the low-risk zones (round 5's judge: "the castle aprons, the green grass patches by the
// castles and the sand ring are all flat, and nothing living grows anywhere"). A volcanic island's hardy lowland: small,
// low clusters of grass and shrubs, never cacti or toy-coloured flowers, from the catalog's non-MORTAL grass pieces (MORTAL
// tufts are sim: explosions remove them); see FLORA for which bases. Tinted toward a muted, sun-dried palette (olive, sage, straw) so the four themes' greens read as one plant
// community against the basalt; Sunstone off they draw their own colours.
//   sand ring     y 1.7-4.4 (dry sand above the wet strip), under 20 degrees: sparse dune-grass clumps of 3-6 tufts,
//                 denser toward the vegetation line, none on the wet sand
//   green patches y 4-9.5 (the build's grass and olive bands), under 22 degrees: a shrub or two with tussocks round them,
//                 in noise patches, density falling off with height
//   castle aprons 3.5-8 from each castle's footprint (outside its keep-out), under 30 degrees: tufts hugging the walls'
//                 feet, more at the corners
// Clusters, not sprinkles: each cluster is a core (the biggest piece) and children falling in size outward. Every piece keeps
// its footprint off the trail band (bed, shoulder and kerb), the courts, spawns and pads like the rock; nothing over 1.3
// tall, so no worm is hidden and nothing a worm walks through looks broken. Tagged _flo.
import * as T from "./terrain.mjs";
import { keepOut, keepOutFoot, CLEAR } from "./detail-keepout.mjs";
import { rng, onTongue } from "./detail-rock.mjs";

const WATER = 1;
// the pieces (scenery_catalog rows; h = the example's map height). Round 6's first Test with the judge's list (ENGLAND2/3 of
// W3DEarnYourCrust, CAMELOT1 of JoustAboutIt, PIRATE1 of W3DShiverMeTimbers, WAR3 of W3DHoldUntilRelieved) CRASHED at load: the
// game is not large-address-aware and had 224 MB of its 2 GB free (largest block 63 MB) when it faulted; every other base
// a level copies from loads that base's resources too. So the flora comes from the base the rock already loads
// (Multi.W3DAppleCoreIsland: its grass tufts and low leafy plants) and from the level's own theme (PREHISTORIC1 grass).
export const FLORA = {
  // (PIRATE11-13, the base's "leafy" pieces, drew as pale rounded stones by the Harbour court in round 6's Test: out; the
  // cluster's core is a big leafy grass mat instead)
  shrub: [{ base: "Multi.W3DAppleCoreIsland", resource: "Pirate3", h: 0.37, mul: 1.7 }],
  mat: [{ base: "Multi.W3DAppleCoreIsland", resource: "Pirate3", h: 0.37 }, { base: "Multi.LandWormsForgot", resource: "PREHISTORIC1", h: 0.21 }],
  tuft: [{ base: "Multi.W3DAppleCoreIsland", resource: "PIRATE1", h: 0.3, mul: 1.3 }, { base: "Multi.W3DAppleCoreIsland", resource: "PIRATE3", h: 0.18, mul: 1.5 }],
  tussock: [{ base: "Multi.W3DAppleCoreIsland", resource: "PIRATE1", h: 0.3, mul: 1.9 }],
};
// sun-dried greens (the tint draws as colour x the texel's luminance / 0.5)
export const GREENS = { shrub: ["#7f8f4e", "#738545", "#86904f"], mat: ["#93955a", "#8a8f55"], tuft: ["#a6a066", "#9c9a62"], tussock: ["#8c9455", "#7f8a4d"] };

export async function floraPass(ctx) {
  const { call } = ctx, R = rng(20261115);
  const stats = { clusters: { sand: 0, green: 0, apron: 0 }, pieces: 0, byKind: {}, rejected: {} };
  const rej = (why) => { stats.rejected[why] = (stats.rejected[why] ?? 0) + 1; };
  // resolve each piece's example id
  const ids = {};
  for (const [kind, list] of Object.entries(FLORA)) for (const p of list) {
    const row = (await call("scenery_catalog", { bases: [p.base], roles: ["scenery"], search: p.resource, limit: 10 })).json.list.find((r) => r.resource === p.resource);
    if (!row) throw new Error(`no ${p.resource} in ${p.base}`);
    (ids[kind] ??= []).push({ ...p, id: row.example.id, mesh: row.meshSize, sc: row.example.scale });
  }
  const sample = async (pts) => {
    const res = [];
    for (let i = 0; i < pts.length; i += 3500) res.push(...(await call("surface_sample", { points: pts.slice(i, i + 3500), units: "map", step: 0.8 })).json.samples);
    return res;
  };
  // candidate cluster centres: a 2-unit grid over the lowland ring and round the castles
  const grid = [];
  for (let x = -112; x <= 112; x += 2) for (let z = -112; z <= 112; z += 2) {
    const rho = T.rhoAt(x, z);
    if (rho < T.RC + 20) continue;
    grid.push([x + (R() - 0.5) * 1.2, z + (R() - 0.5) * 1.2]);
  }
  const gs = await sample(grid);
  const cands = [];
  grid.forEach(([x, z], i) => {
    const g = gs[i];
    if (!g) return;
    const near = T.CASTLES.map((c) => T.footDist(c, x, z, 0)).reduce((a, b) => Math.min(a, b), Infinity);
    let zone = null;
    // (the island's lowland is steep where it is free: the gentle ground is courts, trails and their landings, and the
    // sampled beach runs 30-50 degrees, so the grasses take slopes up to 40-45 degrees)
    if (near > CLEAR.castle + 0.5 && near < 8 && g.slope < 42 && g.y > WATER + 1) zone = "apron";
    else if (g.y >= 4 && g.y <= 9.5 && g.slope < 42) zone = "green";
    else if (g.y >= WATER + 0.7 && g.y < 4.4 && g.slope < 46) zone = "sand";
    if (!zone) return;
    if (onTongue(x, z, 2)) return;
    if (keepOut(x, z, 1.2, ctx.spawns)) return;
    // weights: sand denser toward the vegetation line, green patchy and thinning upward, aprons dense at corners (near < 5)
    const n = T.noise(x * 0.08, z * 0.08, 611);
    const w = zone === "sand" ? (g.y - WATER - 0.7) / 3 * (n > -0.1 ? 1 : 0.2)
      : zone === "green" ? (n > -0.3 ? 1 : 0.25) * (1 - (g.y - 4) / 9)
      : 1.2 - (near - 3.5) / 8;
    cands.push({ x, z, g, zone, w: w * (0.6 + 0.4 * R()), near });
  });
  stats.candidates = cands.reduce((m, c) => (m[c.zone] = (m[c.zone] ?? 0) + 1, m), {});
  cands.sort((a, b) => b.w - a.w);
  const sep = { sand: 6, green: 5, apron: 3.5 }, cap = { sand: 40, green: 45, apron: 45 };
  const centres = [];
  for (const c of cands) {
    if (c.w < 0.15 || stats.clusters[c.zone] >= cap[c.zone]) continue;
    if (centres.some((o) => Math.hypot(o.x - c.x, o.z - c.z) < Math.min(sep[o.zone], sep[c.zone]))) continue;
    centres.push(c); stats.clusters[c.zone]++;
  }
  // the members of each cluster: a core and children falling in size outward
  const members = [];
  for (const c of centres) {
    const plan = c.zone === "sand" ? { core: "mat", kids: ["tuft", "tuft", "tuft", "mat", "tuft"], n: [3, 6], r: 2.2 }
      : c.zone === "green" ? { core: "shrub", kids: ["tussock", "tussock", "mat", "shrub", "tuft", "tussock"], n: [3, 6], r: 2.4 }
      : { core: R() < 0.5 ? "shrub" : "tussock", kids: ["tussock", "tuft", "mat", "tussock"], n: [2, 4], r: 1.8 };
    const n = plan.n[0] + Math.floor(R() * (plan.n[1] - plan.n[0] + 1));
    members.push({ x: c.x, z: c.z, kind: plan.core, s: 1 + R() * 0.25, zone: c.zone });
    for (let i = 0; i < n; i++) {
      const a = R() * Math.PI * 2, d = plan.r * (0.35 + 0.65 * Math.sqrt((i + 1) / n)) * (0.8 + R() * 0.4);
      members.push({ x: c.x + Math.cos(a) * d, z: c.z + Math.sin(a) * d, kind: plan.kids[i % plan.kids.length], s: (1 - 0.35 * (i / n)) * (0.75 + R() * 0.3), zone: c.zone });
    }
  }
  const ms = await sample(members.map((m) => [m.x, m.z]));
  const byBase = new Map();
  members.forEach((m, i) => {
    const g = ms[i];
    if (!g || g.y < WATER + 0.5) { rej("water"); return; }
    if (g.slope > 50) { rej("steep"); return; }
    const p = ids[m.kind][Math.floor(R() * ids[m.kind].length)];
    const scale = m.s * (p.sc?.[0] ?? 1) * (p.mul ?? 1), foot = Math.max(p.mesh?.[0] ?? 1, p.mesh?.[2] ?? 1) * scale / 2;
    if (p.h * m.s * (p.mul ?? 1) > 1.35) { rej("too tall"); return; }
    const why = keepOutFoot(m.x, m.z, foot, foot, 0, ctx.spawns, 0.5);
    if (why) { rej(why.replace(/^castle .*/, "castle")); return; }
    if (onTongue(m.x, m.z, foot + 1)) { rej("lava"); return; }
    if (!byBase.has(p.base)) byBase.set(p.base, []);
    byBase.get(p.base).push({ id: p.id, xz: [+m.x.toFixed(2), +m.z.toFixed(2)], seat: g.slope > 25 ? 0.22 : 0.12, rot: [0, Math.round(R() * 360), 0], scale: +scale.toFixed(3),
      tint: GREENS[m.kind][Math.floor(R() * GREENS[m.kind].length)] });
    stats.byKind[m.kind] = (stats.byKind[m.kind] ?? 0) + 1;
  });
  // a cap (DTL_FLORA_MAX, or ctx.floraMax from the layer's detail budget)
  let room = ctx.floraMax ?? (process.env.DTL_FLORA_MAX ? +process.env.DTL_FLORA_MAX : Infinity);
  for (const [base, copies] of byBase) { if (copies.length > room) copies.splice(room); room -= copies.length; }
  for (const [base, copies] of byBase) for (let i = 0; i < copies.length; i += 200) {
    const r = (await call("copy_base_details", { base, allowOtherTheme: true, units: "map", tag: "flo", copies: copies.slice(i, i + 200) })).json;
    stats.pieces += r.placed.length;
  }
  ctx.say?.("flora", JSON.stringify({ clusters: stats.clusters, pieces: stats.pieces, byKind: stats.byKind }));
  return stats;
}
