// Detail layer, round 3: the volcano's ROCK as a level artist lays it (see detail.mjs). Sim-neutral channels only: scenery
// copies tagged _dtl (no collision). Round 2's point lights are cut (they never showed in a shot) and so are emitters (their
// replay proof is not done). One piece, Horror5 (Multi.W3DAppleCoreIsland, albedo #36374a), the darkest rock the bases hold,
// but no longer one round boulder per prop: every rock is a KITBASHED crag (erg-mcp place_crags, lib/scatter.ts), 1-4
// interpenetrating copies, each drawn at most twice as tall as wide, as spires, stepped ledges, tors, sea stacks or single
// blocks, SEATED 30-45% of its height under the lowest ground sample of its footprint and LEANED toward the slope's normal
// (at most 15 degrees, spires nearer upright). Vertical faces catch less of Sunstone's blue sky light than round 2's domes.
// Tiers:
//   heroes       flank crags 11-20 tall between the switchback columns (spires, ledges, tors), low ledges on the OUTER rim slope
//                and the inner crater wall (none taller than 8 near the crater, none within 15 of a launcher pad)
//   sea stacks   3-4 narrow stacks clustered off each of 3-4 headlands, sunk into the sea bed, no taller than a castle tower
//   secondaries  small blocks and tors clustered on each hero's downhill side
//   scree        at slope breaks and cliff feet found on the real surface (surface_sample), graded big at the toe and small up
//                the slope, thinned to almost nothing on faces over 60 degrees; patchy graded bands along the lava tongues
//   crater       rubble clumps against the inner walls between the pads, big at the wall to small toward the floor; a crust
//                round the lava lake
//   breakers     small blocks in patches at the waterline
// Every crag keeps its whole footprint (its radius from place_crags) out of the trail band, courts, spawns, pads and the
// factory (detail-keepout.mjs), off the lava, and and shows at least 15% of its height over the highest ground of its footprint (seated on the lowest, it never floats).
import * as T from "./terrain.mjs";
import { keepOut, keepOutFoot, trailDist, TRAIL_BAND } from "./detail-keepout.mjs";

export const ROCK = { base: "Multi.W3DAppleCoreIsland", resource: "Horror5" };
const WATER = 1;                       // the sea's level (map), as build.mjs sets it

export function rng(seed) { let s = seed >>> 0; return () => ((s = (s * 1664525 + 1013904223) >>> 0) / 4294967296); }

/** The tongue's centre angle and half width at plain radius r. */
function tongueAt(t, r) {
  return { a: t.a + t.wob * Math.sin(r / t.wl + t.ph) + t.dr * Math.max(0, r - t.rb), w: t.w0 + (t.w1 - t.w0) * Math.min(1, Math.max(0, (r - t.r0) / (t.r1 - t.r0))) };
}
/** Whether (x, z) is on a lava tongue (its wandering centre line and its width at that radius, plus a margin). */
export function onTongue(x, z, margin = 1.5) {
  const r = Math.hypot(x, z), th = Math.atan2(z, x);
  for (const t of T.TONGUES) {
    if (r < t.r0 - 2 || r > t.r1 + 2) continue;
    const { a, w } = tongueAt(t, r);
    const d = Math.abs(((th - a + Math.PI * 3) % (Math.PI * 2)) - Math.PI);
    if (d * r < w + margin) return true;
  }
  return false;
}
const angDiff = (a, b) => Math.abs(((a - b + Math.PI * 3) % (Math.PI * 2)) - Math.PI);
const PADS = T.LAUNCHERS.map(([rho, th]) => T.pointAt(rho, th));
const padDist = (x, z) => Math.min(...PADS.map((p) => Math.hypot(p[0] - x, p[1] - z)));

export async function rockPass(ctx) {
  const { call, say } = ctx;
  const R = rng(20261111);
  const row = (await call("scenery_catalog", { bases: [ROCK.base], roles: ["scenery"], search: ROCK.resource, limit: 5 })).json.list.find((r) => r.resource === ROCK.resource);
  if (!row) throw new Error(`no ${ROCK.resource} in ${ROCK.base}`);
  const stats = { rejected: {}, sizes: {}, kinds: {}, pieces: 0 };
  const rej = (tier, why) => { const k = `${tier}: ${why}`; stats.rejected[k] = (stats.rejected[k] ?? 0) + 1; };
  const placed = [];                    // accepted footprints {x, z, r}
  let seed = 1000;
  const common = { base: ROCK.base, id: row.example.id, allowOtherTheme: true, units: "map", tag: "dtl", maxTilt: 15, jitter: 4 };
  const sample = async (pts) => {
    const res = [];
    for (let i = 0; i < pts.length; i += 3500) res.push(...(await call("surface_sample", { points: pts.slice(i, i + 3500), units: "map", step: 1.2 })).json.samples);
    return res;
  };

  // A crag candidate: {x, z, height, width, archetype, tier, kind, sink?, align?}. crags() asks place_crags for each one's
  // seated pose (dry), keeps those whose footprint is clear (keep-out, lava, overlap, ground: dry land, the sea or the
  // waterline, no step under it over half its height), then places the kept ones with the same seeds (the same poses).
  async function crags(list, o = {}) {
    for (const c of list) c.seed ??= seed++;
    const spec = (c) => ({ xz: [+c.x.toFixed(2), +c.z.toFixed(2)], height: +c.height.toFixed(2), width: +c.width.toFixed(2), archetype: c.archetype, seed: c.seed,
      ...(c.sink !== undefined ? { sink: c.sink } : {}), ...(c.align !== undefined ? { align: c.align } : {}) });
    const sink = o.sink ?? [0.3, 0.45];
    for (let i = 0; i < list.length; i += 120) {
      const chunk = list.slice(i, i + 120);
      const r = (await call("place_crags", { ...common, sink, crags: chunk.map(spec), dry: true })).json;
      for (const cr of r.crags) chunk[cr.i].dry = cr;
    }
    const ok = [];
    for (const c of list) {
      const d = c.dry;
      if (!d) { rej(c.tier, "no ground"); continue; }
      const rad = d.radius * 0.9;
      if (!o.allowCrater && T.rhoAt(c.x, c.z) < T.RC - 6) { rej(c.tier, "crater"); continue; }
      const why = keepOutFoot(c.x, c.z, rad, rad, 0, ctx.spawns, o.pad ?? 0.5);
      if (why) { rej(c.tier, why.replace(/^castle .*/, "castle")); continue; }
      if (o.padClear && padDist(c.x, c.z) < o.padClear + rad) { rej(c.tier, "near a pad"); continue; }
      if (onTongue(c.x, c.z, Math.min(rad, 4) + 0.3)) { rej(c.tier, "lava"); continue; }
      if (Math.hypot(c.x - T.LAKE.c[0], c.z - T.LAKE.c[1]) < T.LAKE.R + 0.6 + rad * 0.6) { rej(c.tier, "lava lake"); continue; }
      if (o.sea) { if (d.groundLow > WATER - 0.6) { rej(c.tier, "not sea"); continue; } }
      else if (o.waterline) { if (d.groundLow < WATER - 1.4 || d.groundLow > WATER + 0.9) { rej(c.tier, "not waterline"); continue; } }
      else if (d.groundLow < WATER + (o.shore ?? 0.3)) { rej(c.tier, "water"); continue; }
      if (d.top < WATER + 0.4) { rej(c.tier, "under the water"); continue; }
      // seated on the lowest sample, nothing floats; on a step the crag sinks into it, so keep only those that still show:
      // the top at least 15% of the height (and a unit, or 30% of a small one) over the HIGHEST sample of the footprint
      if (!o.sea && d.above < (o.show !== undefined ? c.height * o.show : Math.max(c.height * 0.15, Math.min(1, c.height * 0.3)))) { rej(c.tier, "buried in a step"); if (process.env.DTL_DEBUG && (stats.dbg ??= []).length < 12) stats.dbg.push({ tier: c.tier, h: +c.height.toFixed(1), a: c.archetype, ...Object.fromEntries(["groundLow", "groundSpread", "sink", "slope", "above", "radius", "top", "bottom"].map((k) => [k, d[k]])) }); continue; }
      if (placed.some((p) => Math.hypot(p.x - c.x, p.z - c.z) < (p.r + rad) * (o.overlap ?? 0.8))) { rej(c.tier, "overlap"); continue; }
      ok.push(c);
      placed.push({ x: c.x, z: c.z, r: rad });
    }
    for (let i = 0; i < ok.length; i += 120) {
      const chunk = ok.slice(i, i + 120);
      const r = (await call("place_crags", { ...common, sink, crags: chunk.map(spec) })).json;
      stats.pieces += r.placed;
    }
    for (const c of ok) {
      const b = c.height >= 10 ? "10+" : c.height >= 3 ? "3-10" : "<3";
      stats.sizes[b] = (stats.sizes[b] ?? 0) + 1;
      stats.kinds[c.kind] = (stats.kinds[c.kind] ?? 0) + 1;
    }
    return ok;
  }
  const pick = (w) => { let t = R() * w.reduce((a, b) => a + b[1], 0); for (const [k, v] of w) if ((t -= v) <= 0) return k; return w[0][0]; };

  // ---------------- heroes. Spots are searched (the trails cover most of the cone): on a 1.5-unit grid, each point's largest
  // clear radius (to the trail band, castles, spawns, pads, the factory), then greedily the roomiest points of each zone.
  const grid = [];
  for (let x = -100; x <= 100; x += 1.5) for (let z = -100; z <= 100; z += 1.5) {
    const rho = T.rhoAt(x, z);
    if (rho > 94 || rho < T.FLAT_R) continue;
    let r = Math.min(10, trailDist(x, z) - TRAIL_BAND - 1.5);
    while (r >= 2.8 && keepOut(x, z, r, ctx.spawns)) r -= 0.5;
    if (r < 2.8 || onTongue(x, z, r * 0.6)) continue;
    grid.push({ x, z, rho, r });
  }
  // only ground a crag can stand on: dry, and under 50 degrees (the summit's outer wall is a 70-degree cliff where a seated
  // crag would vanish into the face)
  const gs = await sample(grid.map((p) => [p.x, p.z]));
  for (let i = grid.length - 1; i >= 0; i--) if (!gs[i] || gs[i].y < WATER + 0.4 || gs[i].slope > 50) grid.splice(i, 1);
  stats.heroSpots = grid.length;
  grid.sort((p, q) => q.r - p.r || p.x - q.x || p.z - q.z);
  const ZONES = [
    // the outer rim slope: low ledges and tors, none taller than 8 (a pad is 6 across), 15 clear of any pad
    { kind: "rim", lo: T.RC + 2, hi: T.RC + 12, n: 6, h: [5.5, 8], sep: 16, arch: [["ledge", 3], ["tor", 2], ["spire", 1]], padClear: 12 },
    // the inner crater wall between the pads: low ledges leaning on the wall
    { kind: "wall", lo: T.FLAT_R + 3, hi: T.RC - 3, n: 4, h: [4.5, 7], sep: 14, arch: [["ledge", 3], ["tor", 1]], padClear: 12, crater: true,
      ok: (p) => Math.hypot(p.x - T.LAKE.c[0], p.z - T.LAKE.c[1]) > T.LAKE.R + 5 + p.r && angDiff(Math.atan2(p.z, p.x), T.BREACH) > 0.35 },
    // the flanks between the switchback columns: the big crags
    { kind: "flank", lo: T.RC + 9, hi: 94, n: 26, h: [6, 18], sep: 9, arch: [["spire", 5], ["ledge", 3], ["tor", 2]] },
  ];
  const heroes = [];
  for (const zn of ZONES) {
    let got = 0;
    for (const p of grid) {
      if (got >= zn.n) break;
      if (p.rho < zn.lo || p.rho > zn.hi || (zn.ok && !zn.ok(p))) continue;
      if (heroes.some((h) => Math.hypot(h.x - p.x, h.z - p.z) < zn.sep)) continue;
      const archetype = pick(zn.arch);
      // the footprint radius is about 0.42 of a spire's height, 0.75 of a ledge's or tor's (width = 1.4 height)
      let height = Math.max(zn.h[0], Math.min(zn.h[1], archetype === "spire" ? p.r / 0.42 : p.r / 0.75));
      for (let tries = 0; tries < 2; tries++, height *= 0.75) {
        const c = { x: p.x, z: p.z, height, width: archetype === "spire" ? height / 2.1 : height * 1.4, archetype, tier: `hero ${zn.kind}`, kind: `${zn.kind} ${archetype}`, zone: zn.kind };
        const got1 = await crags([c], { allowCrater: zn.crater, padClear: zn.padClear, shore: -0.2 });
        if (got1.length) { heroes.push(got1[0]); got++; break; }
      }
    }
    stats[`heroes_${zn.kind}`] = got;
  }

  // ---------------- sea stacks: clusters of 3-4 off 3-4 headlands (the waterline's furthest bulges), off the castles' fronts
  const castleTh = T.CASTLES.filter((c) => Math.hypot(...c.at) > 70).map((c) => Math.atan2(c.at[1], c.at[0]));
  const radial = [], ths = [];
  for (let th = 0; th < Math.PI * 2; th += 0.03) { const pts = []; for (let rho = 80; rho <= 118; rho += 1) pts.push(T.pointAt(rho, th)); radial.push(pts); ths.push(th); }
  const rs = await sample(radial.flat());
  const wl = radial.map((pts, k) => {
    const s = rs.slice(k * pts.length, (k + 1) * pts.length);
    const i = s.findIndex((q, j) => j > 0 && q && s[j - 1] && s[j - 1].y >= WATER && q.y < WATER);
    return i < 0 ? null : { th: ths[k], pt: pts[i], r: Math.hypot(...pts[i]) };
  });
  const heads = wl.filter((w) => w && wl.every((v) => !v || angDiff(v.th, w.th) > 0.22 || v.r <= w.r) && castleTh.every((a) => angDiff(w.th, a) > 0.45))
    .sort((a, b) => b.r - a.r);
  const headlands = [];
  for (const h of heads) if (headlands.length < 4 && headlands.every((g) => angDiff(g.th, h.th) > 1.1)) headlands.push(h);
  const sea = [];
  for (const h of headlands) {
    const ux = Math.cos(h.th), uz = Math.sin(h.th), vx = -uz, vz = ux, n = 3 + (R() < 0.5 ? 1 : 0), cl = [];
    for (let i = 0; i < n * 3; i++) {
      const out = 4 + R() * 9, side = (R() - 0.5) * 12, above = (i === 0 ? 10 : 4.5 + R() * 5);       // the cluster's main stack first
      cl.push({ x: h.pt[0] + ux * out + vx * side, z: h.pt[1] + uz * out + vz * side, above, archetype: "stack", tier: "sea", kind: "sea stack", sink: 0.3 });
    }
    // the stack reaches `above` over the water from its sea bed (a unit under the centre's ground, for the footprint's lowest)
    const bed = await sample(cl.map((c) => [c.x, c.z]));
    const ready = cl.filter((c, i) => bed[i] && bed[i].y < WATER - 0.6);
    for (const c of ready) { const b = bed[cl.indexOf(c)].y - 1; c.height = (WATER + c.above - b) / (1 - c.sink); c.width = Math.min(4.5, Math.max(2.4, c.above * 0.38)); }
    let got = 0;
    for (const c of ready) { if (got >= n) break; const k = await crags([c], { sea: true, overlap: 1.1, sink: 0.3 }); if (k.length) { sea.push(k[0]); got++; } }
  }
  stats.headlands = headlands.map((h) => +h.th.toFixed(2)); stats.sea = sea.length;

  // ---------------- secondaries: small blocks and tors on each land hero's downhill side
  const sec = [];
  for (const h of heroes) {
    const dh = Math.atan2(h.z, h.x), n = 4 + Math.floor(R() * 3), hr = h.dry.radius;
    for (let i = 0; i < n * 2; i++) {
      const a = dh + (R() - 0.5) * (R() < 0.7 ? 2.2 : 5.5), d = hr * (0.85 + R() * 0.9), size = Math.max(1.6, h.height * (0.15 + R() * 0.2));
      sec.push({ x: h.x + Math.cos(a) * d, z: h.z + Math.sin(a) * d, height: size, width: size * (R() < 0.5 ? 1.2 : 0.8), archetype: R() < 0.35 ? "tor" : "single", tier: "secondary", kind: `${h.zone} secondary`, align: 0.7 });
    }
  }
  stats.secondaries = (await crags(sec, { allowCrater: true, overlap: 0.7, padClear: 9, sink: [0.2, 0.3], show: 0.25 })).length;

  // ---------------- scree at slope breaks and cliff feet: along radial lines on the real surface, where the ground above is
  // steeper than here by 14 degrees or more and here is under 38 degrees, a fan runs downhill for 2-6 units with the
  // biggest blocks at its toe. Faces over 60 degrees take almost nothing (3% of the candidates).
  const lines = [], lth = [];
  for (let th = 0; th < Math.PI * 2; th += 0.028) { const pts = []; for (let rho = T.RC + 1; rho <= 92; rho += 1) pts.push(T.pointAt(rho, th)); lines.push(pts); lth.push(th); }
  const ls = await sample(lines.flat());
  const scree = [];
  const patchy = (x, z) => T.noise(x * 0.16, z * 0.16, 77) > -0.15;
  lines.forEach((pts, k) => {
    const s = ls.slice(k * pts.length, (k + 1) * pts.length);
    for (let i = 3; i < s.length - 1; i++) {
      if (!s[i] || !s[i - 2] || !s[i - 3]) continue;
      const above = Math.max(s[i - 2].slope, s[i - 3].slope), here = s[i].slope;
      const [x, z] = pts[i];
      if (here > 60) { if (R() < 0.03) scree.push({ x, z, height: 0.5 + R() * 0.4, archetype: "single", tier: "scree", kind: "steep face" }); continue; }
      if (above - here < 12 || here > 42 || !patchy(x, z) || R() > 0.85) continue;
      const len = 2 + R() * 4, n = 2 + Math.floor(R() * 4);
      for (let q = 0; q < n; q++) {
        const t = R(), [px, pz] = T.pointAt(T.rhoAt(x, z) + t * len, lth[k] + (R() - 0.5) * 0.04);
        const size = (0.45 + t * 1.9) * (0.75 + R() * 0.5);          // small near the break, big at the toe
        scree.push({ x: px, z: pz, height: size, width: size * (0.9 + R() * 0.6), archetype: "single", tier: "scree", kind: "slope break", align: 0.9 });
      }
      i += 3;
    }
  });
  // the lava tongues' edges, in patches, graded: small against the lava, bigger further out
  for (const t of T.TONGUES) for (let r = t.r0 + 3; r < t.r1 - 2; r += 1.6) for (const side of [-1, 1]) {
    const { a, w } = tongueAt(t, r), off = R(), th = a + side * (w + 1.0 + off * 2.2) / r, x = r * Math.cos(th), z = r * Math.sin(th);
    if (!patchy(x, z) || R() > 0.7) continue;
    const size = (0.5 + off * 1.6) * (0.75 + R() * 0.5);
    scree.push({ x, z, height: size, width: size * 1.2, archetype: "single", tier: "scree", kind: "lava edge", align: 0.9 });
  }
  for (const c of scree) c.width ??= c.height * 1.1;
  stats.scree = (await crags(scree, { overlap: 0.9, sink: [0.12, 0.22], padClear: 8, show: 0.25 })).length;

  // ---------------- the crater: rubble clumps against the inner walls between the pads (big at the wall, small toward the
  // floor), none near a pad, the breach's lava, the factory's apron or the lake; and a crust round the lava lake
  const crater = [];
  for (let i = 0; i < 14; i++) {
    const th = i / 14 * Math.PI * 2 + R() * 0.3;
    const [wx, wz] = T.pointAt(T.FLAT_R + 2.5, th);
    if (padDist(wx, wz) < 13 || angDiff(th, T.BREACH) < 0.35) continue;
    const n = 7 + Math.floor(R() * 5);
    for (let q = 0; q < n; q++) {
      const t = R(), rho = T.FLAT_R + 3.5 - t * 6.5, [x, z] = T.pointAt(rho, th + (R() - 0.5) * 0.35 * (1 + t));
      const size = (q === 0 ? 3.2 + R() * 1.3 : 0.5 + (1 - t) * 2.2) * (0.8 + R() * 0.4);   // the wall end big, the floor end small
      crater.push({ x, z, height: size, width: size * 1.2, archetype: q === 0 ? "tor" : "single", tier: "crater", kind: "crater clump", align: 0.8 });
    }
  }
  for (let i = 0; i < 22; i++) {
    const a = i / 22 * Math.PI * 2 + R() * 0.2, d = T.LAKE.R + 1.6 + R() * 2;
    const size = 0.6 + R() * 1.4;
    crater.push({ x: T.LAKE.c[0] + Math.cos(a) * d, z: T.LAKE.c[1] + Math.sin(a) * d, height: size, width: size * 1.3, archetype: "single", tier: "crater", kind: "lake crust", align: 0.9 });
  }
  stats.crater = (await crags(crater, { allowCrater: true, overlap: 0.85, sink: [0.15, 0.25], padClear: 7, show: 0.25 })).length;

  // ---------------- breakers: small blocks in patches about the waterline, off the castles' fronts
  const shore = [];
  for (const w of wl) {
    if (!w || castleTh.some((a) => angDiff(w.th, a) < 0.22) || T.noise(Math.cos(w.th) * 6, Math.sin(w.th) * 6, 31) < -0.1) continue;
    const ux = Math.cos(w.th), uz = Math.sin(w.th);
    for (let q = 0; q < 2; q++) {
      const t = (R() - 0.4) * 3.5, size = 0.7 + R() * 1.8 * (q ? 0.6 : 1);
      shore.push({ x: w.pt[0] + ux * t + (R() - 0.5) * 1.5, z: w.pt[1] + uz * t + (R() - 0.5) * 1.5, height: size, width: size * 1.3, archetype: "single", tier: "breaker", kind: "breaker", align: 0.8 });
    }
  }
  stats.breakers = (await crags(shore, { waterline: true, overlap: 0.9, sink: 0.2, show: 0.25 })).length;

  stats.heroes = heroes.length;
  stats.crags = placed.length;
  say?.("rock", JSON.stringify({ heroes: stats.heroes, sea: stats.sea, sec: stats.secondaries, scree: stats.scree, crater: stats.crater, breakers: stats.breakers, pieces: stats.pieces }));
  ctx.rockOut = { heroes: heroes.map((h) => ({ x: +h.x.toFixed(1), z: +h.z.toFixed(1), height: +h.height.toFixed(1), kind: h.kind, dry: h.dry })), sea: sea.map((h) => ({ x: +h.x.toFixed(1), z: +h.z.toFixed(1), height: +h.height.toFixed(1), dry: h.dry })) };
  return stats;
}
