// Detail layer, round 2: the volcano's ROCK as a level artist lays it (see detail.mjs). Sim-neutral channels only:
// scenery copies tagged _dtl (no collision) and point lights (render-only); round 1's particle emitters are cut (their
// replay neutrality proof is not done). One piece, dark basalt: Horror5 (Multi.W3DAppleCoreIsland, albedo #36374a,
// scenery_catalog {sort: "dark"}), the darkest rock the bases hold; round 1's pale moss, tan and gold pieces are gone
// (round 2 found ARABIAN6 never draws in this level at any scale or rotation). Variety comes from scale (0.5 to 36 map
// units), yaw, a little pitch and roll, and a mild squash. Three tiers:
//   heroes       14-36 u: rim crags, inner crater wall, hairpin anchors off the kerb, gully mouths; sea stacks off the shore
//   secondaries  3-10 u clustered round each hero, leaning downhill
//   scree        0.5-3 u: fans spilling downhill of the heroes, bands along the lava tongues' edges and the cliffs' feet
//                (masking the drape's sliver seams), breakers at the waterline
// Every footprint keeps out of the trail band, courts, spawns, pads and the factory (detail-keepout.mjs keepOutFoot),
// off the lava, and is seated by surface_sample on the low side of the ground under it, so nothing floats.
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

export async function rockPass(ctx) {
  const { call, say } = ctx;
  const R = rng(20261110);
  const row = (await call("scenery_catalog", { bases: [ROCK.base], roles: ["scenery"], search: ROCK.resource, limit: 5 })).json.list.find((r) => r.resource === ROCK.resource);
  if (!row) throw new Error(`no ${ROCK.resource} in ${ROCK.base}`);
  const mesh = row.meshSize, big = Math.max(...mesh);            // about [3.11, 2.05, 3.21]
  const stats = { heroes: 0, sea: 0, secondaries: 0, scree: 0, bands: 0, breakers: 0, lights: 0, emitters: 0, rejected: {}, sizes: {} };
  const rej = (why) => { stats.rejected[why] = (stats.rejected[why] ?? 0) + 1; };
  const placed = [];                                            // accepted footprints {x, z, r}
  const out = [];                                               // the copies

  const sample = async (pts) => {
    const res = [];
    for (let i = 0; i < pts.length; i += 3500) res.push(...(await call("surface_sample", { points: pts.slice(i, i + 3500), units: "map", step: 1.2 })).json.samples);
    return res;
  };
  const RING = 8;
  // A candidate: {x, z, size (the longest side, map units), tier, yaw, tilt [pitch, roll], squash, seat}. place() tests a
  // batch: footprint keep-out, lava, crater floor, overlap with what is placed, then the ground under the footprint (the
  // centre and RING points at 0.8 of its half axes): land props need dry ground, `sea` ones a sea bed 0.3-`sea` under the
  // water, `waterline` ones the ground about at the water. y = the lower of the centre and the mean of the lowest and the centre.
  async function place(cands, opts = {}) {
    const ok = [];
    for (const c of cands) {
      const k = c.size / big, rx = mesh[0] * k / 2 * 0.85, rz = mesh[2] * k / 2 * 0.85;
      Object.assign(c, { k, rx, rz, r: Math.max(rx, rz) });
      const why = keepOutFoot(c.x, c.z, rx, rz, c.yaw, ctx.spawns, c.tier === "hero" ? 2 : c.tier === "secondary" ? 1 : 0.5);
      if (why) { rej(`${c.tier}: ${why.replace(/^castle .*/, "castle")}`); continue; }
      if (onTongue(c.x, c.z, Math.min(c.r, 3) + 0.4)) { rej(`${c.tier}: lava`); continue; }
      if (!opts.allowCrater && T.rhoAt(c.x, c.z) < T.RC - 8) { rej(`${c.tier}: crater floor`); continue; }
      if (placed.some((p) => Math.hypot(p.x - c.x, p.z - c.z) < (p.r + c.r) * (opts.overlap ?? 0.75))) { rej(`${c.tier}: overlap`); continue; }
      ok.push(c);
      placed.push(c.foot = { x: c.x, z: c.z, r: c.r });
    }
    const pts = [];
    for (const c of ok) {
      pts.push([c.x, c.z]);
      for (let i = 0; i < RING; i++) { const a = i / RING * Math.PI * 2; pts.push([c.x + Math.cos(a) * c.rx * 0.8, c.z + Math.sin(a) * c.rz * 0.8]); }
    }
    const sm = pts.length ? await sample(pts) : [];
    const kept = [];
    ok.forEach((c, i) => {
      const s = sm.slice(i * (RING + 1), (i + 1) * (RING + 1));
      const drop = (why) => { rej(`${c.tier}: ${why}`); placed.splice(placed.indexOf(c.foot), 1); };
      if (s.some((q) => !q)) return drop("no ground");
      const ys = s.map((q) => q.y), lo = Math.min(...ys), mid = ys[0];
      if (opts.sea) { if (mid > WATER - 0.3 || lo < WATER - opts.sea) return drop("not shallow sea"); }
      else if (opts.waterline) { if (mid < WATER - 1.2 || mid > WATER + 1.6) return drop("not at the waterline"); }
      else if (lo < WATER + 0.4) return drop("water");
      c.y = opts.sea ? lo : Math.min(mid, (lo + mid) / 2);
      if (c.y + mesh[1] * c.k * (c.squash ?? 1) * (1 - (c.seat ?? 0.25)) * 0.75 < WATER + 0.5) return drop("under the water");
      c.slope = s[0].slope; c.normal = s[0].normal;
      kept.push(c);
    });
    for (const c of kept) {
      const sq = c.squash ?? 1;
      out.push({ pos: [+c.x.toFixed(2), +c.y.toFixed(2), +c.z.toFixed(2)], rot: [+(c.tilt?.[0] ?? 0).toFixed(1), +c.yaw.toFixed(1), +(c.tilt?.[1] ?? 0).toFixed(1)],
        scale: Math.abs(sq - 1) < 0.02 ? +c.k.toFixed(3) : [+c.k.toFixed(3), +(c.k * sq).toFixed(3), +c.k.toFixed(3)], seat: c.seat ?? 0.25, tier: c.tier, kind: c.kind, size: +c.size.toFixed(2) });
      const b = c.size >= 14 ? "14+" : c.size >= 3 ? "3-14" : "<3";
      stats.sizes[b] = (stats.sizes[b] ?? 0) + 1;
    }
    return kept;
  }
  const tilt = (deg) => [(R() - 0.5) * 2 * deg, (R() - 0.5) * 2 * deg];
  const cand = (x, z, size, tier, extra = {}) => ({ x, z, size, tier, yaw: R() * 360, tilt: tilt(tier === "hero" ? 8 : tier === "secondary" ? 16 : 25), squash: 0.8 + R() * 0.45, ...extra });
  const downhill = (h) => (h.normal && Math.hypot(h.normal[0], h.normal[2]) > 0.05 ? Math.atan2(h.normal[2], h.normal[0]) : Math.atan2(h.z, h.x));

  // ---------------- tier 1: heroes where the ground is clear enough for them. The trails cover most of the cone (seven
  // switchback columns, legs 9.5 apart), so the spots are searched, not fixed: on a 1.5-unit grid, each point's largest
  // footprint radius (its distance to the trail band, the castles, spawns, pads and the factory), then greedily the roomiest
  // points of each zone, kept apart: rim crags on the crest, the inner crater wall, the flank pockets between the columns
  // (hairpin Vs and gully mouths among them).
  const heroes = [];
  const grid = [];
  for (let x = -100; x <= 100; x += 1.5) for (let z = -100; z <= 100; z += 1.5) {
    const rho = T.rhoAt(x, z);
    if (rho > 86 || rho < T.RC - 9) continue;
    let r = Math.min(11, trailDist(x, z) - TRAIL_BAND - 2);
    while (r >= 5 && keepOut(x, z, r, ctx.spawns)) r -= 0.5;
    if (r < 5 || onTongue(x, z, r * 0.7)) continue;
    grid.push({ x, z, rho, r });
  }
  grid.sort((p, q) => q.r - p.r || p.x - q.x || p.z - q.z);
  const ZONES = [
    { kind: "rim", lo: T.RC - 4, hi: T.RC + 8, n: 7, size: [12, 20], sepTh: 0.5, seat: 0.35 },
    { kind: "wall", lo: T.RC - 9, hi: T.RC - 3, n: 3, size: [12, 17], sepTh: 1.2, seat: 0.3, crater: true, ok: (p) => Math.hypot(p.x - T.LAKE.c[0], p.z - T.LAKE.c[1]) > T.LAKE.R + 6 + p.r },
    { kind: "flank", lo: T.RC + 7, hi: 86, n: 9, size: [13, 22], sepTh: 0.3, seat: 0.3 },
  ];
  for (const zn of ZONES) {
    let got = 0;
    for (const p of grid) {
      if (got >= zn.n) break;
      if (p.rho < zn.lo || p.rho > zn.hi || (zn.ok && !zn.ok(p))) continue;
      const th = Math.atan2(p.z, p.x);
      if (heroes.some((h) => (h.kind === zn.kind && angDiff(th, Math.atan2(h.z, h.x)) < zn.sepTh) || Math.hypot(h.x - p.x, h.z - p.z) < 18)) continue;
      const size = Math.max(zn.size[0], Math.min(zn.size[1], (2 * p.r) / 0.85 * (0.85 + R() * 0.15)));
      const c = await place([cand(p.x, p.z, size, "hero", { seat: zn.seat, kind: zn.kind })], { allowCrater: zn.crater });
      if (c.length) { heroes.push(c[0]); got++; }
    }
    stats[`heroes_${zn.kind}`] = got;
  }
  stats.heroes = heroes.length;

  // sea stacks: up to 7 big ones in the shallows past the sand, never in front of a castle (its camera looks from the sea)
  const castleTh = T.CASTLES.filter((c) => Math.hypot(...c.at) > 70).map((c) => Math.atan2(c.at[1], c.at[0]));   // the shore castles
  const sea = [];
  for (let i = 0; i < 80 && sea.length < 7; i++) {
    const th = R() * Math.PI * 2;
    if (castleTh.some((a) => angDiff(th, a) < 0.42) || angDiff(th, 1.07) < 0.3 || sea.some((s) => angDiff(th, Math.atan2(s.z, s.x)) < 0.45)) continue;
    const [x, z] = T.pointAt(99 + R() * 12, th);
    const got = await place([cand(x, z, 14 + R() * 14, "hero", { seat: 0.1, kind: "sea", tilt: tilt(6) })], { sea: 8, overlap: 1 });
    if (got.length) sea.push(got[0]);
  }
  stats.sea = sea.length;
  heroes.push(...sea);

  // ---------------- tier 2: secondaries round each hero, mostly on its downhill side
  const secLand = [], secSea = [];
  for (const h of heroes) {
    const n = h.kind === "sea" ? 4 : 5 + Math.floor(R() * 3), dh = downhill(h);
    for (let i = 0; i < n * 2; i++) {
      const a = dh + (R() - 0.5) * (R() < 0.65 ? 2.4 : 6.28), d = h.r * (0.75 + R() * 0.8);
      (h.kind === "sea" ? secSea : secLand).push(cand(h.x + Math.cos(a) * d, h.z + Math.sin(a) * d, Math.max(3, h.size * (0.14 + R() * 0.2)), "secondary", { seat: h.kind === "sea" ? 0.1 : 0.3 }));
    }
  }
  stats.secondaries = (await place(secLand, { overlap: 0.7, allowCrater: true })).length + (await place(secSea, { sea: 6, overlap: 0.7 })).length;

  // ---------------- tier 3: scree fans spilling downhill of each land hero, small and many far from it
  const scree = [];
  for (const h of heroes.filter((x) => x.kind !== "sea")) {
    const dh = downhill(h);
    for (let i = 0; i < 30; i++) {
      const t = R(), a = dh + (R() - 0.5) * (0.5 + t * 0.8), d = h.r * (1.05 + t * 2.8);
      scree.push(cand(h.x + Math.cos(a) * d, h.z + Math.sin(a) * d, 0.5 + (1 - t) * 2.6 * (0.6 + R() * 0.6), "scree", { seat: 0.4, kind: "fan" }));
    }
  }
  stats.scree = (await place(scree, { overlap: 0.9, allowCrater: true })).length;

  // ---------------- bands along the lava tongues' edges and the cliffs' feet (they also hide the drape's sliver seams)
  const band = [];
  const patchy = (x, z) => T.noise(x * 0.18, z * 0.18, 77) > -0.1;      // in patches, not an even line
  for (const t of T.TONGUES) for (let r = t.r0 + 3; r < t.r1 - 2; r += 1.5) for (const side of [-1, 1]) {
    const { a, w } = tongueAt(t, r), th = a + side * (w + 1.1 + R() * 1.6) / r, x = r * Math.cos(th), z = r * Math.sin(th);
    if (patchy(x, z)) band.push(cand(x, z, 0.6 + R() * 2.3, "scree", { seat: 0.4, kind: "lava edge" }));
  }
  // cliff feet, found on the real surface: along each radial line, the point just past the steepest drop
  const lines = [];
  for (const c of T.CLIFFS) for (let th = 0; th < Math.PI * 2; th += 0.045) {
    const pts = [];
    for (let d = -5; d <= 8; d += 0.75) pts.push(T.pointAt(c.rho + d, th));
    lines.push(pts);
  }
  const ls = await sample(lines.flat());
  let li = 0;
  for (const pts of lines) {
    const s = ls.slice(li, li + pts.length); li += pts.length;
    let best = -1, bd = 1.2;
    for (let i = 1; i < s.length; i++) if (s[i] && s[i - 1] && s[i - 1].y - s[i].y > bd) { bd = s[i - 1].y - s[i].y; best = i; }
    if (best < 0) continue;
    let j = best;
    while (j + 1 < s.length && s[j + 1] && s[j].y - s[j + 1].y > 0.5) j++;
    const [x, z] = pts[j];
    if (!patchy(x, z)) continue;
    for (let q = 0; q < 2; q++) band.push(cand(x + (R() - 0.5) * 2.5, z + (R() - 0.5) * 2.5, (0.8 + R() * 2.6) * (q ? 0.6 : 1), "scree", { seat: 0.4, kind: "cliff foot" }));
  }
  stats.bands = (await place(band, { overlap: 0.85 })).length;

  // ---------------- the crater: rubble fields on the floor between the pads (kept off the factory's apron and the pads by
  // the keep-out) and a crust of rocks round the lava lake's shore
  const crater = [];
  for (let i = 0; i < 9; i++) {
    const [cx, cz] = T.pointAt(T.RC - 10 - R() * 8, i / 9 * Math.PI * 2 + R() * 0.4);
    crater.push(cand(cx, cz, 6 + R() * 4, "secondary", { seat: 0.3, kind: "crater" }));
    for (let k = 0; k < 10; k++) { const a = R() * 6.28, d = 3 + R() * 7; crater.push(cand(cx + Math.cos(a) * d, cz + Math.sin(a) * d, 0.6 + R() * 2.2, "scree", { seat: 0.4, kind: "crater" })); }
  }
  for (let i = 0; i < 26; i++) { const a = i / 26 * Math.PI * 2 + R() * 0.2, d = T.LAKE.R + 1 + R() * 2.5; crater.push(cand(T.LAKE.c[0] + Math.cos(a) * d, T.LAKE.c[1] + Math.sin(a) * d, 0.8 + R() * 2.4, "scree", { seat: 0.35, kind: "lake crust" })); }
  stats.crater = (await place(crater, { allowCrater: true, overlap: 0.85 })).length;

  // ---------------- the shore: breakers in patches about the waterline (found on the real surface along radial lines),
  // half in the water, kept off the shore castles' fronts
  const shore = [], radial = [];
  for (let th = 0; th < Math.PI * 2; th += 0.04) {
    if (castleTh.some((a) => angDiff(th, a) < 0.2) || T.noise(Math.cos(th) * 6, Math.sin(th) * 6, 31) < -0.15) continue;
    const pts = []; for (let rho = 78; rho <= 112; rho += 1) pts.push(T.pointAt(rho, th));
    radial.push(pts);
  }
  const rs = await sample(radial.flat());
  let ri = 0;
  for (const pts of radial) {
    const s = rs.slice(ri, ri + pts.length); ri += pts.length;
    const i = s.findIndex((q, k) => k > 0 && q && s[k - 1] && s[k - 1].y >= WATER && q.y < WATER);
    if (i < 0) continue;
    for (let q = 0; q < 3; q++) {
      const t = (R() - 0.35) * 4, [x0, z0] = pts[i], o = Math.hypot(x0, z0), ux = x0 / o, uz = z0 / o;
      shore.push(cand(x0 + ux * t + (R() - 0.5) * 1.5, z0 + uz * t + (R() - 0.5) * 1.5, 1 + R() * 4 * (q ? 0.6 : 1), "scree", { seat: 0.2, kind: "breaker" }));
    }
  }
  stats.breakers = (await place(shore, { waterline: true, overlap: 0.9 })).length;
  say?.("rock", JSON.stringify({ heroes: stats.heroes, sea: stats.sea, sec: stats.secondaries, scree: stats.scree, bands: stats.bands, breakers: stats.breakers }));

  // ---------------- write the copies
  for (let i = 0; i < out.length; i += 200)
    await call("copy_base_details", { base: ROCK.base, allowOtherTheme: true, units: "map", tag: "dtl",
      copies: out.slice(i, i + 200).map((o) => ({ id: row.example.id, pos: o.pos, rot: o.rot, scale: o.scale, seat: o.seat })) });
  stats.placed = out.length;

  // ---------------- glow: point lights round the lava lake and along the lava tongues (render-only; no emitters)
  const lt = (await call("scenery_catalog", { bases: ["Multi.ValleyOfDinoWorms"], roles: ["light"], search: "PNTLGHT 255 100 0 300", limit: 5 })).json.list[0];
  const L = T.LAKE, glow = [];
  for (let i = 0; i < 6; i++) { const a = i / 6 * Math.PI * 2 + 0.2; glow.push([L.c[0] + Math.cos(a) * (L.R + 1), L.c[1] + Math.sin(a) * (L.R + 1)]); }
  for (const g of T.TONGUES.filter((t) => t.core)) for (const rr of [T.RC + 12, 60, 76]) glow.push(T.pointAt(rr, g.a));
  const gs = await sample(glow);
  const lights = glow.map((p, i) => gs[i] && { id: lt.example.id, pos: [+p[0].toFixed(2), +(gs[i].y + 2.5).toFixed(2), +p[1].toFixed(2)] }).filter(Boolean);
  if (lights.length) await call("copy_base_details", { base: "Multi.ValleyOfDinoWorms", allowOtherTheme: true, units: "map", copies: lights });
  stats.lights = lights.length;
  ctx.rockOut = out;
  return stats;
}
