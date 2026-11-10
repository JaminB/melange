// Detail layer round 10: CALDERA'S OWN BASALT (the round-9 judge: "the single rock mesh caps rock quality"; "the cliff piles
// are not seated talus: they read as strings of black beads stuck to near-vertical faces"; "the single rock mesh is a glossy,
// faceted obsidian shard ... it reads as crystal rather than weathered basalt"). Three mod meshes built by meshes/build.mjs
// (procedural, matte: their material's specular and fresnel are 0) and loaded by the level's pack through its spice.json
// `meshes` (docs/spice.md): caldera.Boulder (a rounded weathered boulder), caldera.Ledge (three stacked strata stepping back)
// and caldera.Columns (seven hexagonal columns). Placed by erg-mcp place_mod_mesh and swap_mesh (new this round).
//   talusPass   TALUS FANS at the foot of the cliffs (erg-mcp plan_slope_break: gentle ground with a face of 48+ degrees
//               rising over it, never on the face): each fan big boulders at its toe, small ones at its apex against the
//               face, a skirt of half-buried pieces; no piece where the ground's contact angle is over 50 degrees. Tag _tal.
//   ledgePass   STRATA LEDGES on the bare 45-75 degree flank faces (erg-mcp plan_contour sites, one caldera.Ledge each, laid
//               level along the contour and sunk into the face): the drape's saw-tooth broken by horizontal basalt bands, in
//               patches (never two within 6 on a contour, no ladders). Tag _ldg.
//   columnsPass a few caldera.Columns clusters on the cliff tops and crests where the ground is gentle (round 6's crowns). Tag _col.
//   basaltPass  every HORROR5 copy left (the rock pass's crags, the crater rim, the coast stacks and their skirts, the crowns)
//               re-pointed at caldera.Boulder by swap_mesh: the same box, the same tint, a matte rounded stone.
// All four are scenery copies: no collision, visual only (sim_fingerprint, every pass).
import os from "node:os";
import path from "node:path";
import * as T from "./terrain.mjs";
import { keepOutFoot } from "./detail-keepout.mjs";
import { rng, onTongue } from "./detail-rock.mjs";
import { generate } from "./meshes/gen.mjs";

const WATER = 1;
const PADS = T.LAUNCHERS.map(([rho, th]) => T.pointAt(rho, th));
let BOXES = null;
/** The mod meshes' boxes (map units), from the generator (deterministic: the same numbers meshes/build.mjs writes). */
export function meshBoxes() {
  BOXES ??= Object.fromEntries(Object.entries(generate(path.join(os.tmpdir(), "caldera-meshes-gen"))).map(([k, v]) => [k, v.box]));
  return BOXES;
}
export const TALUS_MAX = +(process.env.DTL_TALUS_MAX ?? 200);
export const LEDGE_MAX = +(process.env.DTL_LEDGE_MAX ?? 45);
export const COLUMNS_MAX = +(process.env.DTL_COLUMNS_MAX ?? 8);

const sampler = (call) => async (pts) => {
  const res = [];
  for (let i = 0; i < pts.length; i += 3500) res.push(...(await call("surface_sample", { points: pts.slice(i, i + 3500), units: "map", step: 0.8 })).json.samples);
  return res;
};
const contact = (n) => Math.acos(Math.max(-1, Math.min(1, n[1] / (Math.hypot(...n) || 1)))) * 180 / Math.PI;
function clearOf(ctx, rocks = []) {
  return (x, z, rx, rz = rx, yaw = 0) => {
    if (T.rhoAt(x, z) < T.FLAT_R + 1) return "crater floor";
    if (onTongue(x, z, Math.max(rx, rz) * 0.6 + 0.8)) return "lava";
    if (Math.hypot(x - T.LAKE.c[0], z - T.LAKE.c[1]) < T.LAKE.R + 1 + rx) return "lava lake";
    if (PADS.some((p) => Math.hypot(x - p[0], z - p[1]) < 11 + rx)) return "near a pad";
    if (rocks.some((k) => Math.hypot(k.x - x, k.z - z) < k.r + rx * 0.6)) return "rock";
    const why = keepOutFoot(x, z, rx, rz, yaw, ctx.spawns, 0.6);
    return why ? why.replace(/^castle .*/, "castle") : null;
  };
}
async function rockFootprints(call, tags) {
  const out = [];
  for (const q of tags) for (const d of (await call("list_scenery", { search: q, units: "map", limit: 2000 })).json.details) {
    const s = d.scale ?? [1, 1, 1];
    out.push({ x: d.at[0], z: d.at[2], r: 0.7 * 2.05 * Math.max(Math.abs(s[0]), Math.abs(s[2])) });
  }
  return out;
}

// ---------------------------------------------------------------- talus fans
/** One fan below a foot site: [x, z, size, kind, embed]: big at the toe, small at the apex, a half-buried skirt (lib/slope-break.ts's talusFan). */
function fan(site, n, length, big, small, R) {
  const out = [], base = Math.atan2(site.down[1], site.down[0]), spread = 0.95;
  for (let i = 0; i < n; i++) {
    const t = Math.sqrt((i + 0.5) / n) * (0.85 + 0.15 * R());
    const a = base + (R() - 0.5) * spread * (0.4 + 0.6 * t), d = t * length;
    out.push({ x: site.xz[0] + Math.cos(a) * d, z: site.xz[1] + Math.sin(a) * d, size: (small + (big - small) * t) * (0.8 + 0.4 * R()), kind: t > 0.75 ? "toe" : t < 0.3 ? "apex" : "body", embed: 0.35 + 0.15 * R() });
  }
  for (let i = 0, k = Math.ceil(n * 0.6); i < k; i++) {
    const a = base + (R() - 0.5) * spread * 1.3, d = length * (0.95 + 0.3 * R());
    out.push({ x: site.xz[0] + Math.cos(a) * d, z: site.xz[1] + Math.sin(a) * d, size: small * (0.6 + 0.5 * R()), kind: "skirt", embed: 0.55 + 0.1 * R() });
  }
  return out;
}

/** A free caldera.Boulder piece of width `size` resting on ground g, `embed` of its height buried, flattened by `flat`. */
function boulderPiece(B, x, z, g, size, embed, flat, R, tint) {
  const w = B.max[0] - B.min[0], h = B.max[1] - B.min[1], s = size / w, sy = s * flat, buried = embed * h * sy;
  // tilt toward the ground's normal (a resting stone follows its bed), a little random roll, any yaw
  const n = g.normal, tx = Math.atan2(n[2], n[1]) * 180 / Math.PI * 0.7, tz = -Math.atan2(n[0], n[1]) * 180 / Math.PI * 0.7;
  return { pos: [+x.toFixed(2), +(g.y - buried - B.min[1] * sy).toFixed(2), +z.toFixed(2)], rot: [+(tx + (R() - 0.5) * 8).toFixed(1), +(R() * 360).toFixed(1), +(tz + (R() - 0.5) * 8).toFixed(1)],
    scale: [+(s * (0.9 + R() * 0.2)).toFixed(3), +sy.toFixed(3), +(s * (0.85 + R() * 0.2)).toFixed(3)], ...(tint ? { tint } : {}) };
}

export async function talusPass(ctx) {
  const { call } = ctx, R = rng(20261110), B = meshBoxes()["caldera.Boulder"], sample = sampler(call);
  const stats = { sites: 0, runs: 0, fans: 0, pieces: 0, rejected: {}, byKind: {} };
  const rej = (why) => { stats.rejected[why] = (stats.rejected[why] ?? 0) + 1; };
  const clear = clearOf(ctx);
  const plan = (await call("plan_slope_break", { box: [-100, -100, 100, 100], units: "map", step: 1.5, faceMin: 45, footMax: 36, probe: 2.5, rise: 2, minY: WATER + 0.8, maxY: 46, limit: 20000 })).json;
  stats.sites = plan.count; stats.runs = plan.runs;
  // fan sites: the tallest faces first, 9 apart, clear of everything with the fan's reach
  const cands = plan.sites.filter((s) => T.rhoAt(...s.xz) > T.FLAT_R + 2).map((s) => ({ ...s, w: s.faceRise * (0.7 + 0.6 * R()) * (T.noise(s.xz[0] * 0.08, s.xz[1] * 0.08, 515) + 0.8) }))
    .sort((a, b) => b.w - a.w);
  const chosen = [];
  for (const s of cands) {
    if (chosen.length >= 40) break;
    if (chosen.some((c) => Math.hypot(c.xz[0] - s.xz[0], c.xz[1] - s.xz[1]) < 7)) continue;
    const reach = Math.min(4, 1.6 + s.faceRise * 0.25);
    const mid = [s.xz[0] + s.down[0] * reach * 0.5, s.xz[1] + s.down[1] * reach * 0.5];
    const why = clear(mid[0], mid[1], reach * 0.4);
    if (why) { rej("fan: " + why); continue; }
    chosen.push({ ...s, reach });
  }
  // the pieces
  const pieces = [];
  for (const s of chosen) {
    const n = 5 + Math.floor(R() * 4), big = Math.min(2.2, 1.1 + s.faceRise * 0.08), small = 0.4 + R() * 0.15;
    for (const p of fan(s, n, s.reach, big, small, R)) pieces.push({ ...p, site: s });
  }
  const gs = await sample(pieces.map((p) => [p.x, p.z]));
  const place = [];
  pieces.forEach((p, i) => {
    const g = gs[i];
    if (!g || g.y < WATER + 0.3) { rej("water"); return; }
    if (contact(g.normal) > 50) { rej("contact over 50 degrees"); return; }
    // walkable ground (under 20 degrees) is where worms walk: only a low, half-buried skirt stone there, and none on the beach
    if (g.slope < 20) {
      if (g.y < 4) { rej("open beach"); return; }
      p.kind = "skirt"; p.size = Math.min(p.size, 0.7); p.embed = Math.max(p.embed, 0.55);
    }
    if (g.y > p.site.y + 1.2) { rej("up the face"); return; }
    const why = clear(p.x, p.z, p.size * 0.55);
    if (why) { rej(why); return; }
    if (place.some((q) => Math.hypot(q.x - p.x, q.z - p.z) < (q.size + p.size) * 0.32)) { rej("overlap"); return; }
    place.push({ ...p, g });
  });
  place.splice(TALUS_MAX);
  const list = place.map((p) => boulderPiece(B, p.x, p.z, p.g, p.size, p.kind === "skirt" ? p.embed : p.embed * 0.85, p.kind === "skirt" ? 0.55 : 0.7 + R() * 0.15, R));
  for (let i = 0; i < list.length; i += 200) stats.pieces += (await call("place_mod_mesh", { resource: "caldera.Boulder", box: B, stem: "talus", units: "map", tag: "tal", pieces: list.slice(i, i + 200) })).json.placed;
  for (const p of place) stats.byKind[p.kind] = (stats.byKind[p.kind] ?? 0) + 1;
  stats.fans = new Set(place.map((p) => p.site)).size;
  ctx.say?.("talus", JSON.stringify({ sites: stats.sites, fans: stats.fans, pieces: stats.pieces, byKind: stats.byKind }));
  return stats;
}

// ---------------------------------------------------------------- strata ledges
export async function ledgePass(ctx) {
  if (LEDGE_MAX <= 0) return { off: "DTL_LEDGE_MAX is 0" };
  const { call } = ctx, R = rng(20261111), L = meshBoxes()["caldera.Ledge"];
  const stats = { runs: 0, sites: 0, ledges: 0, pieces: 0, rejected: {} };
  const rej = (why) => { stats.rejected[why] = (stats.rejected[why] ?? 0) + 1; };
  const clear = clearOf(ctx, await rockFootprints(call, ["_dtl", "_tal"]));
  const plan = (await call("plan_contour", { area: { centre: [0, 0], rMin: T.RC + 3, rMax: 92 }, band: { from: 6, to: 44, every: 3 }, cell: 1.25,
    spacing: 2, jitter: 0.1, seed: 9110, minSlope: 45, maxSlope: 75, minRun: 2, limit: 20000 })).json;
  stats.runs = plan.runs.length; stats.sites = plan.total;
  const cands = [];
  for (const run of plan.runs) for (const s of run.sites) {
    const [x, , z] = s.pos;
    if (s.pos[1] < WATER + 4) continue;
    const w = (T.noise(x * 0.05, z * 0.05, 7717) + 0.5) * (0.7 + 0.6 * R());
    if (w < 0.4) continue;
    cands.push({ w, s, level: run.level });
  }
  cands.sort((a, b) => b.w - a.w);
  const used = [], pieces = [];
  for (const c of cands) {
    if (pieces.length >= LEDGE_MAX) break;
    const [x, , z] = c.s.pos;
    // 6 from a ledge on the same or the next contour (no ladders), 4.5 from any other
    if (used.some((u) => Math.hypot(u.x - x, u.z - z) < (Math.abs(u.level - c.level) <= 3 ? 9 : 6))) { rej("crowded"); continue; }
    // round 10 in game: at 3.4-5.6 long and 1.1-1.7 thick the ledges read as thin dashes from the trail and flank cameras:
    // bigger bands (6-9 long, 1.9-2.7 thick), fewer of them
    const len = 6 + R() * 3, thick = 1.9 + R() * 0.8, depth = 2.2 + R() * 0.8, yaw = Math.atan2(c.s.tangent[1], c.s.tangent[0]) * 180 / Math.PI;   // keepOutFoot takes degrees
    // the across-contour half width at least 1.9: a ledge reaches out of the face and its bands overhang (the verify's
    // round footprint, detail-verify.mjs, caught one 9-long ledge next to a trail with depth * 0.45 alone)
    const why = clear(x, z, len / 2, Math.max(depth * 0.45, 1.9), yaw);
    if (why) { rej(why); continue; }
    used.push({ x, z, level: c.level });
    // level along the contour (pitch 0.1: a shelf, barely leaning into the face), sunk past half its depth
    pieces.push({ pos: c.s.pos.map((v) => +v.toFixed(2)), normal: c.s.normal, tangent: c.s.tangent, size: [+len.toFixed(2), +thick.toFixed(2), +depth.toFixed(2)],
      pitch: 0.1, embed: +(0.55 + R() * 0.08).toFixed(2), jitter: [+((R() - 0.5) * 3).toFixed(1), +((R() - 0.5) * 8).toFixed(1), +((R() - 0.5) * 3).toFixed(1)] });
  }
  for (let i = 0; i < pieces.length; i += 200) stats.pieces += (await call("place_mod_mesh", { resource: "caldera.Ledge", box: L, stem: "ledge", units: "map", tag: "ldg", pieces: pieces.slice(i, i + 200) })).json.placed;
  stats.ledges = pieces.length;
  ctx.say?.("ledge", JSON.stringify({ runs: stats.runs, sites: stats.sites, pieces: stats.pieces }));
  return stats;
}

// ---------------------------------------------------------------- basalt columns on the cliff tops
export async function columnsPass(ctx) {
  if (COLUMNS_MAX <= 0) return { off: "DTL_COLUMNS_MAX is 0" };
  const { call } = ctx, R = rng(20261112), C = meshBoxes()["caldera.Columns"], sample = sampler(call);
  const stats = { candidates: 0, pieces: 0, rejected: {} };
  const rej = (why) => { stats.rejected[why] = (stats.rejected[why] ?? 0) + 1; };
  const clear = clearOf(ctx, await rockFootprints(call, ["_dtl", "_tal", "_ldg"]));
  // cliff tops: gentle ground (under 25 degrees) with a face of 50+ degrees dropping 2.5 downhill of it
  const grid = [];
  for (let x = -96; x <= 96; x += 2) for (let z = -96; z <= 96; z += 2) { const rho = T.rhoAt(x, z); if (rho > T.RC + 4 && rho < 90) grid.push([x + (R() - 0.5), z + (R() - 0.5)]); }
  const gs = await sample(grid);
  const tops = [];
  grid.forEach((p, i) => { const g = gs[i]; if (g && g.slope < 25 && g.y > 8 && g.y < 44) tops.push({ x: p[0], z: p[1], g }); });
  const down = await sample(tops.map((t) => { const n = t.g.normal, l = Math.hypot(n[0], n[2]) || 1; return [t.x + n[0] / l * 2.5, t.z + n[2] / l * 2.5]; }));
  const cands = tops.filter((t, i) => down[i] && down[i].slope > 50 && down[i].y < t.g.y - 2).map((t) => ({ ...t, w: (T.noise(t.x * 0.07, t.z * 0.07, 3131) + 0.7) * (0.6 + 0.8 * R()) }))
    .sort((a, b) => b.w - a.w);
  stats.candidates = cands.length;
  const out = [];
  for (const c of cands) {
    if (out.length >= COLUMNS_MAX) break;
    if (out.some((o) => Math.hypot(o.x - c.x, o.z - c.z) < 16)) continue;
    const size = 2.2 + R() * 1.4;
    const why = clear(c.x, c.z, size * 0.55);
    if (why) { rej(why); continue; }
    const w = C.max[0] - C.min[0], s = size / w, sy = s * (0.8 + R() * 0.4);
    out.push({ x: c.x, z: c.z, piece: { pos: [+c.x.toFixed(2), +(c.g.y - 0.3 * (C.max[1] - C.min[1]) * sy - C.min[1] * sy).toFixed(2), +c.z.toFixed(2)], rot: [0, +(R() * 360).toFixed(1), 0], scale: [+s.toFixed(3), +sy.toFixed(3), +s.toFixed(3)] } });
  }
  if (out.length) stats.pieces = (await call("place_mod_mesh", { resource: "caldera.Columns", box: C, stem: "columns", units: "map", tag: "col", pieces: out.map((o) => o.piece) })).json.placed;
  ctx.say?.("columns", JSON.stringify(stats));
  return stats;
}

// ---------------------------------------------------------------- every HORROR5 copy -> the matte boulder
export async function basaltPass(ctx) {
  const { call } = ctx, B = meshBoxes()["caldera.Boulder"];
  const r = (await call("swap_mesh", { from: "Horror5", to: "caldera.Boulder", toBox: B, fromBase: "Multi.W3DAppleCoreIsland" }, { soft: true }));
  if (r.err) throw new Error("swap_mesh: " + r.text.slice(0, 300));
  // the contact rule for the whole rock layer: no stone where the ground under it is over 58 degrees unless it is big (a
  // crag set into the face), and the talus and columns at 50
  const lint = {};
  for (const [q, max] of [["_tal", 50], ["_col", 50]]) lint[q] = (await call("lint_contact", { search: q, maxDeg: max, remove: true, units: "map" })).json;
  // the rock pass's small stones (under 2.6 across: scree, skirts, crater rim pieces) on ground over 50 degrees read as beads
  // stuck to a face (the judge's Seawall and Harbour piles): removed; the big crags set into a face stay
  const over = (await call("lint_contact", { search: "_dtl", maxDeg: 50, limit: 4000, units: "map" })).json.first;
  const dtl = new Map((await call("list_scenery", { search: "_dtl", units: "map", limit: 2000 })).json.details.map((d) => [d.id, d]));
  const small = over.filter((o) => { const d = dtl.get(o.id); return d && 2.95 * Math.max(...(d.scale ?? [1, 1, 1]).map(Math.abs)) < 2.6; }).map((o) => o.id);
  if (small.length) await call("remove_scenery", { ids: small });
  lint._dtl = { checked: dtl.size, over: over.length, removed: small.length };
  return { swapped: r.json.changed, fromBox: r.json.fromBox, lint: Object.fromEntries(Object.entries(lint).map(([k, v]) => [k, { checked: v.checked, over: v.over, removed: v.removed }])) };
}
