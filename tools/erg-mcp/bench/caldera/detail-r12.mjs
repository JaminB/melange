// Detail layer round 12 (the round-11 judge's list): the rock reads black in game -> brighter baked albedo (meshes/gen.mjs);
//   chips    the ungraded black chips and the lone flat slab on the beaches go: every rock on the shore (under 6 high, past
//            rho 78) that is a single piece (no other rock within 2.2) or a big flat slab is removed
//   beachtum seated 3-size graded tumbles on the shore's beach ground (a 1.8-2.2 block, 1.1-1.5 mid pieces, 0.6-0.9 chips,
//            all half-buried, biggest piece on the downhill/seaward side)
//   scree    mid-scale rock groups (2.4-3.2 anchor, 1.4-2.0 mids, 0.7-1.1 chips) on the cone flanks' scree and ash fields
//   accent   taller accent plants (bush cores up to 2.4 tall) standing with the scree groups on slopes over 18 degrees
//            (not where worms walk: nothing taller than a worm on walkable ground), plus mid-scale scrub on the scree
//   lint12   the mine look-alike lint again
// All scenery copies, no collision; keep-outs as every pass (trails + kerbs, courts, spawns, pads, the factory, lava).
import * as T from "./terrain.mjs";
import { rng, onTongue } from "./detail-rock.mjs";
import { keepOutFlora, trailDist, TRAIL_BAND } from "./detail-keepout.mjs";
import { meshBoxes, sampler, contact, clearOf, boulderPiece } from "./detail-basalt.mjs";
import { FLORA, rampTint } from "./detail-flora.mjs";
import { ROCK_VARIANTS } from "./meshes/gen.mjs";

const WATER = 1;
async function rocks(call) {
  const out = [], B = meshBoxes();
  for (const v of ROCK_VARIANTS) for (const d of (await call("list_scenery", { search: v, units: "map", limit: 2000 })).json.details) {
    if (d.resource !== v) continue;
    const s = (d.scale ?? [1, 1, 1]).map(Math.abs), b = B[v];
    out.push({ id: d.id, v, x: d.at[0], y: d.at[1], z: d.at[2], w: Math.max((b.max[0] - b.min[0]) * s[0], (b.max[2] - b.min[2]) * s[2]), h: (b.max[1] - b.min[1]) * s[1] });
  }
  return out;
}
async function placeRock(call, pieces, stem) {
  const by = new Map();
  for (const p of pieces) { if (!by.has(p.v)) by.set(p.v, []); by.get(p.v).push(p.piece); }
  let n = 0;
  for (const [v, list] of by) for (let i = 0; i < list.length; i += 200) n += (await call("place_mod_mesh", { resource: v, box: meshBoxes()[v], stem, units: "map", tag: "dtl", pieces: list.slice(i, i + 200) })).json.placed;
  return n;
}

export async function chipsPass(ctx) {
  const { call } = ctx, sample = sampler(call), all = await rocks(call);
  const shore = all.filter((p) => p.y < 6 && T.rhoAt(p.x, p.z) > 78);
  const g = await sample(shore.map((p) => [p.x, p.z]));
  const ids = [], why = { lone: 0, slab: 0 };
  shore.forEach((p, i) => {
    const flat = g[i]?.slope < 24;
    const near = all.filter((q) => q.id !== p.id && Math.hypot(q.x - p.x, q.z - p.z) < 2.2 + (p.w + q.w) * 0.2).length;
    if (flat && near === 0 && p.w < 2.6) { ids.push(p.id); why.lone++; }
    else if (p.v === "caldera.Slab" && p.w > 2.4 && p.h < p.w * 0.35) { ids.push(p.id); why.slab++; }
  });
  if (ids.length) await call("remove_scenery", { ids });
  return { shore: shore.length, removed: ids.length, why };
}

function tumbleSet(R, cx, cz, dir) {
  const plan = [["caldera.Boulder", 1.8 + R() * 0.4, 0, 0, 0.38], ["caldera.Wedge", 1.2 + R() * 0.3, 1.3, 1, 0.34], ["caldera.Boulder", 1.1 + R() * 0.3, 1.5, -1.2, 0.34],
    ["caldera.Slab", 0.9 + R() * 0.3, -1.4, 0.6, 0.45], ["caldera.Shard", 0.6 + R() * 0.3, 2.3, 0.2, 0.5], ["caldera.Wedge", 0.6 + R() * 0.25, -1.9, -1.0, 0.5], ["caldera.Shard", 0.55 + R() * 0.2, 0.6, 2.0, 0.55]];
  const rot = R() * Math.PI * 2, c = Math.cos(rot), s = Math.sin(rot);
  return plan.map(([v, size, a, b, embed]) => ({ v, size, embed, x: cx + dir[0] * 0.5 + a * c - b * s, z: cz + dir[1] * 0.5 + a * s + b * c }));
}

export const BEACH_SITES = +(process.env.DTL_BEACH_SITES ?? 16);
export async function beachTumblePass(ctx) {
  const { call } = ctx, R = rng(20261201), sample = sampler(call), clear = clearOf(ctx), existing = await rocks(call);
  const pts = [];
  for (let x = -112; x <= 112; x += 2) for (let z = -112; z <= 112; z += 2) { const r = T.rhoAt(x, z); if (r > 80 && r < 118) pts.push([x + (R() - 0.5), z + (R() - 0.5)]); }
  const gs = await sample(pts);
  const cands = pts.map((p, i) => ({ x: p[0], z: p[1], g: gs[i] })).filter((c) => c.g && c.g.y > 1.8 && c.g.y < 5.6 && c.g.slope < 28 && !clear(c.x, c.z, 2.2) && !keepOutFlora(c.x, c.z, 2.0, 2.0, 0, ctx.spawns, 0.3)
    && !existing.some((q) => Math.hypot(q.x - c.x, q.z - c.z) < 3 + q.w * 0.5))
    .map((c) => ({ ...c, w: R() + 0.5 * T.noise(c.x * 0.07, c.z * 0.07, 9911) })).sort((a, b) => b.w - a.w);
  const sites = [];
  for (const c of cands) { if (sites.length >= BEACH_SITES) break; if (sites.every((s) => Math.hypot(s.x - c.x, s.z - c.z) > 13)) sites.push(c); }
  const out = [];
  for (const s of sites) {
    const l = Math.hypot(s.x, s.z) || 1, dir = [s.x / l, s.z / l];
    const set = tumbleSet(R, s.x, s.z, dir), ss = await sample(set.map((p) => [p.x, p.z]));
    set.forEach((p, i) => {
      const g = ss[i];
      if (!g || g.y < WATER + 0.5 || contact(g.normal) > 38) return;
      if (clear(p.x, p.z, p.size * 0.5) || keepOutFlora(p.x, p.z, p.size * 0.5, p.size * 0.5, 0, ctx.spawns, 0.4)) return;
      out.push({ ...p, piece: boulderPiece(meshBoxes()[p.v], p.x, p.z, g, p.size, p.embed, p.v === "caldera.Slab" ? 1 : 0.8, R) });
    });
  }
  const placed = await placeRock(call, out, "btum");
  return { sites: sites.length, placed, at: sites.map((s) => [+s.x.toFixed(0), +s.z.toFixed(0)]) };
}

export const SCREE_SITES = +(process.env.DTL_SCREE_SITES ?? 40);
let SCREE = [];
export async function screePass(ctx) {
  const { call } = ctx, R = rng(20261202), sample = sampler(call), clear = clearOf(ctx), existing = await rocks(call);
  const pts = [];
  for (let x = -96; x <= 96; x += 2) for (let z = -96; z <= 96; z += 2) { const r = T.rhoAt(x, z); if (r > T.RC + 10 && r < 86) pts.push([x + (R() - 0.5) * 1.4, z + (R() - 0.5) * 1.4]); }
  const gs = await sample(pts);
  const stats = { cands: 0, sites: 0, pieces: 0, rejected: {} };
  const rej = (w) => { stats.rejected[w] = (stats.rejected[w] ?? 0) + 1; };
  const cands = [];
  pts.forEach((p, i) => {
    const g = gs[i];
    if (!g) return rej("nog");
    if (g.y < 5 || g.y > 44) return rej("height");
    if (g.slope > 64) return rej("slope");
    { const w = clear(p[0], p[1], 2.4); if (w) return rej("clear: " + w); }
    if (trailDist(p[0], p[1]) < TRAIL_BAND + 1.5) return rej("trail");
    if (existing.some((q) => Math.hypot(q.x - p[0], q.z - p[1]) < 3.5 + q.w * 0.5)) return rej("rock");
    cands.push({ x: p[0], z: p[1], g, w: R() + 0.6 * T.noise(p[0] * 0.05, p[1] * 0.05, 7733) });
  });
  stats.cands = cands.length;
  const sites = [];
  for (const c of cands.sort((a, b) => b.w - a.w)) { if (sites.length >= SCREE_SITES) break; if (sites.every((s) => Math.hypot(s.x - c.x, s.z - c.z) > 6.5)) sites.push(c); }
  const out = [];
  for (const s of sites) {
    const n = s.g.normal, dl = Math.hypot(n[0], n[2]) || 1, down = [n[0] / dl, n[2] / dl];
    const field = s.g.slope < 16;
    const set = field ? [["caldera.Slab", 1.7 + R() * 0.5, 0, 0, 0.5], ["caldera.Wedge", 1.2 + R() * 0.4, 1.6, 0.8, 0.5], ["caldera.Slab", 1.1 + R() * 0.3, -1.2, -1.3, 0.55], ["caldera.Boulder", 1.0 + R() * 0.3, 2.4, -0.9, 0.5], ["caldera.Shard", 0.6 + R() * 0.2, 3.1, 0.6, 0.55], ["caldera.Wedge", 0.7 + R() * 0.2, -1.6, 1.6, 0.55]] : [["caldera.Boulder", 2.4 + R() * 0.8, 0, 0, 0.36], ["caldera.Wedge", 1.5 + R() * 0.5, 1.8, 0.9, 0.34], ["caldera.Slab", 1.4 + R() * 0.5, -0.6, -1.9, 0.4],
      ["caldera.Boulder", 1.3 + R() * 0.4, 2.6, -1.0, 0.35], ["caldera.Shard", 0.7 + R() * 0.3, 3.4, 0.5, 0.5], ["caldera.Wedge", 0.8 + R() * 0.3, -1.5, 1.7, 0.5], ["caldera.Shard", 0.7 + R() * 0.3, 1.2, 2.8, 0.55], ["caldera.Slab", 0.8 + R() * 0.3, 4.0, -0.6, 0.55]];
    const ang = Math.atan2(down[1], down[0]), c = Math.cos(ang), sn = Math.sin(ang);
    const pcs = set.map(([v, size, a, b, embed]) => ({ v, size, embed, x: s.x + a * c - b * sn, z: s.z + a * sn + b * c }));
    const ss = await sample(pcs.map((p) => [p.x, p.z]));
    let kept = 0;
    pcs.forEach((p, i) => {
      const g = ss[i];
      if (!g || g.y < WATER + 0.8) return;
      if (contact(g.normal) > 64 || g.slope > 64) return;
      if (clear(p.x, p.z, p.size * 0.5) || trailDist(p.x, p.z) < TRAIL_BAND + 1.5 + p.size * 0.4) return;
      if (out.some((q) => Math.hypot(q.x - p.x, q.z - p.z) < (q.size + p.size) * 0.38)) return;
      out.push({ ...p, piece: boulderPiece(meshBoxes()[p.v], p.x, p.z, g, p.size, p.embed, field || p.v === "caldera.Slab" ? (field ? 0.55 : 1) : 0.8, R) }); kept++;
    });
    s.kept = kept;
  }
  stats.sites = sites.filter((s) => s.kept > 2).length;
  SCREE = sites.filter((s) => s.kept > 2);
  stats.pieces = await placeRock(call, out, "scree");
  ctx.say?.("scree", JSON.stringify({ sites: stats.sites, pieces: stats.pieces }));
  return stats;
}

export const ACCENT_MAX = +(process.env.DTL_ACCENT_MAX ?? 110);
export async function accentPass(ctx) {
  const { call } = ctx, R = rng(20261203), sample = sampler(call);
  const stats = { clumps: 0, pieces: 0, tall: 0, byKind: {}, rejected: {} };
  const rej = (w) => { stats.rejected[w] = (stats.rejected[w] ?? 0) + 1; };
  const ids = {};
  for (const [kind, list] of Object.entries(FLORA)) for (const p of list) {
    const row = (await call("scenery_catalog", { bases: [p.base], roles: ["scenery"], search: p.resource, limit: 10 })).json.list.find((r) => r.resource === p.resource);
    if (!row) continue;
    const sz = row.size ?? row.meshSize ?? [1, 1, 1];
    (ids[kind] ??= []).push({ ...p, id: row.example.id, sc: row.example.scale ?? [1, 1, 1], r: 0.5 * Math.max(sz[0], sz[2]), h: sz[1] });
  }
  const rockList = await rocks(call);
  const lava = (x, z, m) => onTongue(x, z, m) || Math.hypot(x - T.LAKE.c[0], z - T.LAKE.c[1]) < T.LAKE.R + 2 + m;
  const centres = [];
  for (const s of SCREE) {
    const n = s.g.normal, dl = Math.hypot(n[0], n[2]) || 1, up = [-n[0] / dl, -n[2] / dl];
    for (let k = 0; k < 4; k++) { const a = Math.atan2(up[1], up[0]) + [-0.9, 0.9, -2.3, 2.3][k], d = 3.0 + R() * 2.2; centres.push({ x: s.x + Math.cos(a) * d, z: s.z + Math.sin(a) * d, tall: k < 2 }); }
  }
  for (let t = 0; t < 2000 && centres.length < 110; t++) {
    const th = R() * Math.PI * 2, rho = T.RC + 12 + R() * 62, [x, z] = T.pointAt(rho, th);
    if (!centres.some((c) => Math.hypot(c.x - x, c.z - z) < 5)) centres.push({ x, z, tall: R() < 0.5 });
  }
  const cg = await sample(centres.map((c) => [c.x, c.z]));
  const mem = [];
  centres.forEach((c, i) => {
    const g = cg[i];
    if (!g || g.y < 5 || g.y > 40 || g.slope > 60) return rej("ground");
    if (lava(c.x, c.z, 2) || rockList.some((q) => Math.hypot(q.x - c.x, q.z - c.z) < q.w * 0.6 + 0.8)) return rej("rock/lava");
    c.g = g;
    const parts = c.tall ? [["bush", 1, 1, 2.1], ["shrub", 1, 2, 1], ["tussock", 1, 2, 1], ["agave", 0, 1, 1]] : [["shrub", 1, 2, 1], ["succulent", 1, 2, 1], ["tussock", 1, 2, 1], ["tuft", 1, 2, 1]];
    for (const [kind, n0, n1, boost] of parts) for (let k = 0, n = n0 + Math.floor(R() * (n1 - n0 + 1)); k < n; k++) {
      const a = R() * Math.PI * 2, d = kind === "bush" ? R() * 0.4 : 0.5 + R() * 1.3;
      mem.push({ x: c.x + Math.cos(a) * d, z: c.z + Math.sin(a) * d, kind, boost, f: d / 1.8 });
    }
  });
  const ms = await sample(mem.map((m) => [m.x, m.z]));
  const byBase = new Map();
  let n = 0;
  mem.forEach((m, i) => {
    const g = ms[i];
    if (n >= ACCENT_MAX) return rej("budget");
    if (!g || g.y < 4 || g.slope > 52) return rej("ground");
    const opts = ids[m.kind]; if (!opts) return;
    const p = opts[Math.floor(R() * opts.length)];
    let mul = (p.mul ?? 1) * m.boost * (0.9 + R() * 0.25) * (1 - 0.25 * m.f);
    if (p.h * mul > 2.4) mul = 2.4 / p.h;
    if (p.h * mul < 0.4) mul = 0.4 / p.h;
    const rad = p.r * mul;
    if (lava(m.x, m.z, rad + 0.5) || rockList.some((q) => Math.hypot(q.x - m.x, q.z - m.z) < q.w * 0.5 + rad * 0.5)) return rej("lava/rock");
    const why = keepOutFlora(m.x, m.z, rad, rad, 0, ctx.spawns, 0.8);
    if (why) return rej(why);
    if (m.kind === "bush" && (g.slope < 18 || trailDist(m.x, m.z) < TRAIL_BAND + 3)) return rej("tall on walkable");
    const tint = rampTint(g.y, m.f, R);
    if (!byBase.has(p.base)) byBase.set(p.base, []);
    byBase.get(p.base).push({ id: p.id, xz: [+m.x.toFixed(2), +m.z.toFixed(2)], seat: g.slope > 30 ? 0.3 : 0.2, rot: [Math.round((R() - 0.5) * 8), Math.round(R() * 360), Math.round((R() - 0.5) * 8)],
      scale: p.sc.map((v) => +(v * mul).toFixed(3)), tint });
    stats.byKind[m.kind] = (stats.byKind[m.kind] ?? 0) + 1;
    if (p.h * mul > 1.6) stats.tall++;
    n++;
  });
  for (const [base, copies] of byBase) for (let i = 0; i < copies.length; i += 200) stats.pieces += (await call("copy_base_details", { base, allowOtherTheme: true, units: "map", tag: "flo", copies: copies.slice(i, i + 200) })).json.placed.length;
  stats.clumps = centres.filter((c) => c.g).length;
  return stats;
}

export async function lint12Pass(ctx) {
  const { call } = ctx;
  let removed = 0;
  for (const q of ["_dtl", "_tal"]) {
    const lint = (await call("lint_dressing", { search: q, water: WATER + 0.3, limit: 500 })).json;
    const sus = lint.mineSuspects.list.flatMap((c) => c.ids);
    if (sus.length) await call("remove_scenery", { ids: sus });
    removed += sus.length;
  }
  return { removedSuspects: removed, rock: (await rocks(call)).length };
}
