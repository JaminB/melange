// Detail layer round 7: FLORA, the volcanic island's plant community (round 6's judge: "it exists but is not visible as
// dressing ... 2-4x the scale and real clustering: big tussock masses against castle walls, rock bases and cliff feet, a
// brighter green tint, the judge's assets, something that reads at flank distance").
//
// Pieces: each was checked once in the game in this PREHISTORIC level (round 7 scratchpad caldet/r7/caldet_r7_flora_check_
// sheet.png), since another theme's resource can draw as something else here (GhostHill's WILDWEST18 "grass" draws as a
// dinosaur skull, LandWormsForgot's PREHISTORIC8 is a blue bulb that reads as a pickup: both out):
//   bush     GhostHill WILDWEST2         a leafy bush with a trunk, about 1.7 tall: the masses' cores, the flank-distance read
//   shrub    W3DEarnYourCrust ENGLAND2/3 a low round dark bush: the masses' first ring
//   tussock  Escape PREHISTORIC1, GhostHill WILDWEST1 (draws as an agave-like spiky clump here)  big spiky grass
//   tuft     JoustAboutIt CAMELOT1, W3DAppleCoreIsland PIRATE1 (round 6's)  small grass
//   cover    JoustAboutIt CAMELOT4       a low leafy ground plant with flowers
// Where (a volcanic island: lush low, hardy higher, nothing near the crater):
//   wall       against a castle's outer walls (0.6-2.6 from its footprint, under 14): hedges of bushes along the wall
//              (shrubs instead where the ground there is under 15 degrees)
//   cliff foot gentle ground under a steep face (the ground 2.5 uphill is over 50 degrees), under 30
//   rock base  round the larger crags of the rock pass, under 26
//   lawn       the build's painted grass and olive lawns (4-9.5, under 24 degrees, walkable): LOW plants only (flat tussock,
//              cover, tufts), never a shrub or bush
//   green      the lower slopes and greens, 3.5-12: shrubs, tussocks and cover in noise patches
//   shore      the dry sand over the waterline, under 3.5: sparse dune clumps
//   dry        the mid slopes, 12-30: sparse hardy clumps (agave and straw tufts), thinning upward
// Each cluster has a core and rings falling in size and in density outward (the outer members are dropped more often), so
// masses read as masses and fade into the ground; the bushes (the only pieces over 1.3) grow only at walls, cliff feet,
// rock bases or on slopes over 25 degrees, never where worms walk. Every member's footprint keeps clear of the trail band,
// spawns, pads and the factory (detail-keepout.mjs keepOutFlora; castles: only their walls' footprint), off the lava and
// off the crater (rho under RC + 10). One palette: lush greens low (brighter than round 6's olive), sage and straw higher.
// Tagged _flo.
import * as T from "./terrain.mjs";
import { keepOutFlora } from "./detail-keepout.mjs";
import { rng, onTongue } from "./detail-rock.mjs";

const WATER = 1;
export const FLORA = {
  bush: [{ base: "Multi.GhostHill", resource: "WILDWEST2", r: 0.85, h: 1.66 }],
  shrub: [{ base: "Multi.W3DEarnYourCrust", resource: "ENGLAND2", r: 0.65, h: 0.6, mul: 1.9 }, { base: "Multi.W3DEarnYourCrust", resource: "ENGLAND3", r: 0.65, h: 0.6, mul: 1.9 }],
  tussock: [{ base: "Multi.Escape", resource: "PREHISTORIC1", r: 1.15, h: 0.45, mul: 1.2 }, { base: "Multi.GhostHill", resource: "WILDWEST1", r: 1.0, h: 0.9 }],
  tuft: [{ base: "Multi.JoustAboutIt", resource: "CAMELOT1", r: 0.85, h: 0.5, mul: 1.1 }, { base: "Multi.W3DAppleCoreIsland", resource: "PIRATE1", r: 0.4, h: 0.3, mul: 1.6 }],
  cover: [{ base: "Multi.JoustAboutIt", resource: "CAMELOT4", r: 0.6, h: 0.3, mul: 1.4 }],
};
// drawn radius and height per unit of the piece's own scale multiplier (r, h: at the example's scale), for the readability check
export const FLORA_SIZE = Object.fromEntries(Object.values(FLORA).flat().map((p) => [p.resource, { r: p.r, h: p.h }]));
// one palette: the tint draws as colour x the texel's luminance / 0.5
export const GREENS = {
  bush: ["#4f9a3a", "#5aa443", "#468f35"], shrub: ["#5fa846", "#6bb04c", "#579f40"], tussock: ["#86c34e", "#7cba48", "#92c955"],
  tuft: ["#9ccd5c", "#a8d264"], cover: ["#78b84a", "#84c050"],
};
export const DRY = { tussock: ["#a9b05c", "#9ea656", "#b4b466"], tuft: ["#bdb46e", "#b0aa66"], cover: ["#a3ad60"] };
export const DUNE = { tussock: ["#a8b860", "#9db058"], tuft: ["#b3be68", "#a9b862"], cover: ["#9fb35c"] };

// cluster recipes: rings [kind, count range, radius range, keep chance]
const RECIPES = {
  mass: [["bush", [1, 2], [0, 0.9], 1], ["shrub", [2, 4], [1.1, 2.0], 0.9], ["tussock", [2, 4], [1.8, 3.0], 0.75], ["tuft", [3, 5], [2.6, 4.2], 0.6], ["cover", [1, 3], [2.2, 4.2], 0.55]],
  hedge: [["bush", [2, 3], [0, 0], 1], ["shrub", [1, 3], [0.9, 1.8], 0.9], ["tussock", [1, 3], [1.6, 2.6], 0.75], ["tuft", [2, 4], [2.2, 3.6], 0.6], ["cover", [1, 2], [1.6, 3.2], 0.5]],
  green: [["shrub", [1, 2], [0, 0.8], 1], ["tussock", [2, 4], [1.0, 2.2], 0.85], ["cover", [2, 3], [1.4, 2.8], 0.7], ["tuft", [2, 5], [2.0, 3.6], 0.55]],
  shore: [["tussock", [1, 1], [0, 0], 1], ["tuft", [2, 4], [0.9, 2.2], 0.75], ["tussock", [0, 1], [1.4, 2.4], 0.5]],
  dry: [["tussock", [1, 2], [0, 0.9], 1], ["tuft", [1, 3], [1.0, 2.2], 0.6]],
  lawn: [["tussock", [1, 1], [0, 0], 1], ["cover", [2, 3], [0.8, 1.8], 0.85], ["tuft", [3, 5], [1.2, 2.8], 0.7], ["tussock", [0, 1], [1.6, 2.6], 0.5]],
};
const RECIPE_OF = { wall: "hedge", cliff: "mass", rock: "mass", lawn: "lawn", green: "green", shore: "shore", dry: "dry" };
const SEP = { wall: 4.5, cliff: 5.5, rock: 5.5, lawn: 4, green: 5, shore: 6, dry: 7.5 };
const CAP = { wall: 50, cliff: 30, rock: 36, lawn: 45, green: 50, shore: 26, dry: 30 };

export async function floraPass(ctx) {
  const { call } = ctx, R = rng(20261215);
  const stats = { clusters: {}, pieces: 0, byKind: {}, byBase: {}, rejected: {}, candidates: {} };
  const rej = (why) => { stats.rejected[why] = (stats.rejected[why] ?? 0) + 1; };
  const ids = {};
  for (const [kind, list] of Object.entries(FLORA)) for (const p of list) {
    const row = (await call("scenery_catalog", { bases: [p.base], roles: ["scenery"], search: p.resource, limit: 10 })).json.list.find((r) => r.resource === p.resource);
    if (!row) throw new Error(`no ${p.resource} in ${p.base}`);
    (ids[kind] ??= []).push({ ...p, id: row.example.id, sc: row.example.scale ?? [1, 1, 1] });
  }
  const sample = async (pts) => {
    const res = [];
    for (let i = 0; i < pts.length; i += 3500) res.push(...(await call("surface_sample", { points: pts.slice(i, i + 3500), units: "map", step: 0.8 })).json.samples);
    return res;
  };
  // the rock already placed (its footprints: plants grow round them, not inside them)
  const rocks = [];
  for (const q of ["_dtl", "_ldg", "_cpl"]) for (const d of (await call("list_scenery", { search: q, units: "map", limit: 2000 })).json.details) {
    const s = d.scale ?? [1, 1, 1];
    rocks.push({ x: d.at[0], y: d.at[1], z: d.at[2], r: 0.7 * 1.55 * Math.max(Math.abs(s[0]), Math.abs(s[2])), h: 2.05 * Math.abs(s[1]) });
  }
  const inRock = (x, z, pad = 0) => rocks.some((k) => Math.hypot(k.x - x, k.z - z) < k.r * 0.85 + pad);
  const lava = (x, z, m) => onTongue(x, z, m) || Math.hypot(x - T.LAKE.c[0], z - T.LAKE.c[1]) < T.LAKE.R + 2 + m;
  // ---- candidate centres on a 1.5 grid
  const grid = [];
  for (let x = -114; x <= 114; x += 1.5) for (let z = -114; z <= 114; z += 1.5) {
    if (T.rhoAt(x, z) < T.RC + 10) continue;
    grid.push([x + (R() - 0.5) * 0.9, z + (R() - 0.5) * 0.9]);
  }
  const gs = await sample(grid);
  const pts = [];
  grid.forEach(([x, z], i) => { const g = gs[i]; if (g && g.y > WATER + 0.6 && g.y < 32 && g.slope < 58) pts.push({ x, z, g }); });
  // cliff feet: the ground 2.5 uphill (against the horizontal normal) is a steep face
  const low = pts.filter((p) => p.g.slope < 42 && p.g.y < 30);
  const up = await sample(low.map((p) => { const n = p.g.normal, l = Math.hypot(n[0], n[2]) || 1; return [p.x - n[0] / l * 2.5, p.z - n[2] / l * 2.5]; }));
  low.forEach((p, i) => { if (up[i] && up[i].slope > 46 && up[i].y > p.g.y + 1) p.cliff = true; });
  const bigRocks = rocks.filter((k) => k.h >= 1.6 && k.y < 26);
  const cands = [];
  for (const p of pts) {
    const { x, z, g } = p;
    if (lava(x, z, 1.5) || inRock(x, z)) continue;
    if (keepOutFlora(x, z, 1.2, 1.2, 0, ctx.spawns, 0.5)) continue;
    const wallD = Math.min(...T.CASTLES.map((c) => T.footDist(c, x, z, 0)));
    const rockD = Math.min(Infinity, ...bigRocks.map((k) => Math.hypot(k.x - x, k.z - z) - k.r));
    const n = T.noise(x * 0.07, z * 0.07, 733);
    let zone = null, w = 0;
    if (wallD > 0.6 && wallD < 2.6 && g.y < 14) { zone = "wall"; w = 1.5 - wallD * 0.1; }
    else if (p.cliff) { zone = "cliff"; w = 1.3 - g.y / 60; }
    else if (rockD > 0 && rockD < 2.5 && g.y < 26) { zone = "rock"; w = 1.15 - g.y / 60; }
    // the build's painted lawns (grass and olive, 4-9 map units, under 22 degrees: walkable): LOW plants only
    else if (g.y >= 4 && g.y < 9.5 && g.slope < 24) { zone = "lawn"; w = (n > -0.35 ? 1 : 0.35) * 0.95; }
    else if (g.y >= 3.5 && g.y < 12 && g.slope < 55) { zone = "green"; w = (n > -0.15 ? 1 : 0.2) * (1.05 - (g.y - 3.5) / 20); }
    else if (g.y < 3.5 && g.slope < 50) { zone = "shore"; w = (0.3 + 0.5 * (g.y - WATER - 0.6) / 2.9) * (n > 0 ? 1 : 0.25); }
    else if (g.y >= 12 && g.y < 30 && g.slope < 56) { zone = "dry"; w = 0.55 * (1 - (g.y - 12) / 22) * (n > 0.1 ? 1 : 0.15); }
    if (!zone) continue;
    stats.candidates[zone] = (stats.candidates[zone] ?? 0) + 1;
    cands.push({ x, z, g, zone, w: w * (0.65 + 0.35 * R()), wallD });
  }
  cands.sort((a, b) => b.w - a.w);
  const centres = [];
  for (const c of cands) {
    if (c.w < 0.12 || (stats.clusters[c.zone] ?? 0) >= CAP[c.zone]) continue;
    if (centres.some((o) => Math.hypot(o.x - c.x, o.z - c.z) < Math.min(SEP[o.zone], SEP[c.zone]))) continue;
    centres.push(c); stats.clusters[c.zone] = (stats.clusters[c.zone] ?? 0) + 1;
  }
  // ---- members
  const clusters = centres.map((c) => {
    const recipe = RECIPES[RECIPE_OF[c.zone]], mem = [];
    // a hedge runs along the wall: its bushes step along the wall's tangent (perpendicular to the way out of the castle)
    let tx = 1, tz = 0;
    if (c.zone === "wall") {
      const cs = T.CASTLES.reduce((a, k) => (T.footDist(k, c.x, c.z, 0) < T.footDist(a, c.x, c.z, 0) ? k : a));
      const ox = c.x - cs.at[0], oz = c.z - cs.at[1], L = Math.hypot(ox, oz) || 1;
      const n = c.g.normal, nl = Math.hypot(n[0], n[2]);
      const [ux, uz] = nl > 0.15 ? [n[0] / nl, n[2] / nl] : [ox / L, oz / L];
      tx = -uz; tz = ux;
    }
    for (const [kind, [n0, n1], [r0, r1], keep] of recipe) {
      const n = n0 + Math.floor(R() * (n1 - n0 + 1));
      for (let i = 0; i < n; i++) {
        if (R() > keep) continue;
        let x, z;
        if (c.zone === "wall" && kind === "bush") { const d = (i - (n - 1) / 2) * 1.3 + (R() - 0.5) * 0.3; x = c.x + tx * d; z = c.z + tz * d; }
        else { const a = R() * Math.PI * 2, d = r0 + (r1 - r0) * Math.sqrt(R()); x = c.x + Math.cos(a) * d; z = c.z + Math.sin(a) * d; }
        // size falls off outward a little
        const fall = r1 > 0 ? 1 - 0.25 * (Math.hypot(x - c.x, z - c.z) / Math.max(1, r1)) : 1;
        mem.push({ x, z, kind, s: (0.85 + R() * 0.35) * fall });
      }
    }
    return { c, mem };
  });
  const ms = await sample(clusters.flatMap((k) => k.mem.map((m) => [m.x, m.z])));
  const room0 = ctx.floraMax ?? (process.env.DTL_FLORA_MAX ? +process.env.DTL_FLORA_MAX : Infinity);
  const byBase = new Map();
  let room = room0, k0 = 0;
  for (const cl of clusters) {
    const list = [];
    for (let i = 0; i < cl.mem.length; i++) {
      const m = cl.mem[i], g = ms[k0 + i];
      if (!g || g.y < WATER + 0.5) { rej("water"); continue; }
      if (g.slope > 62) { rej("steep"); continue; }
      const zone = cl.c.zone;
      // bushes only where worms do not walk: walls, cliff feet, rock bases, or slopes over 25 degrees
      let kind = m.kind;
      if (kind === "bush" && !((zone === "wall" && g.slope > 15) || zone === "cliff" || zone === "rock" || g.slope > 25)) kind = "shrub";
      if (zone === "lawn" && (kind === "shrub" || kind === "bush")) kind = "tussock";
      if (zone === "dry" && kind !== "tussock" && kind !== "tuft") kind = "tuft";
      const opts = ids[kind], p = zone === "dry" && kind === "tussock" ? opts.find((o) => o.resource === "WILDWEST1") ?? opts[0]
        : zone === "lawn" && kind === "tussock" ? opts.find((o) => o.resource === "PREHISTORIC1") ?? opts[0] : opts[Math.floor(R() * opts.length)];
      const mul = (p.mul ?? 1) * m.s * (kind === "bush" ? 0.95 + R() * 0.5 : 1);
      const rad = p.r * mul;
      if (lava(m.x, m.z, rad + 0.5)) { rej("lava"); continue; }
      if (inRock(m.x, m.z, -0.2)) { rej("in a rock"); continue; }
      const why = keepOutFlora(m.x, m.z, rad, rad, 0, ctx.spawns, 0.5);
      if (why) { rej(why); continue; }
      const pal = zone === "dry" ? DRY : zone === "shore" ? DUNE : GREENS, cols = pal[kind] ?? GREENS[kind];
      list.push({ base: p.base, kind, copy: { id: p.id, xz: [+m.x.toFixed(2), +m.z.toFixed(2)], seat: g.slope > 40 ? 0.32 : g.slope > 25 ? 0.25 : 0.12,
        rot: [Math.round((R() - 0.5) * (kind === "bush" ? 6 : 10)), Math.round(R() * 360), Math.round((R() - 0.5) * (kind === "bush" ? 6 : 10))],
        scale: p.sc.map((v) => +(v * mul).toFixed(3)), tint: cols[Math.floor(R() * cols.length)] } });
    }
    k0 += cl.mem.length;
    if (list.length > room) { rej("over the detail budget"); continue; }
    room -= list.length;
    for (const it of list) {
      if (!byBase.has(it.base)) byBase.set(it.base, []);
      byBase.get(it.base).push(it.copy);
      stats.byKind[it.kind] = (stats.byKind[it.kind] ?? 0) + 1;
      stats.byBase[it.base] = (stats.byBase[it.base] ?? 0) + 1;
    }
  }
  for (const [base, copies] of byBase) for (let i = 0; i < copies.length; i += 200) {
    const r = (await call("copy_base_details", { base, allowOtherTheme: true, units: "map", tag: "flo", copies: copies.slice(i, i + 200) })).json;
    stats.pieces += r.placed.length;
  }
  ctx.say?.("flora", JSON.stringify({ clusters: stats.clusters, pieces: stats.pieces, byKind: stats.byKind }));
  return stats;
}
