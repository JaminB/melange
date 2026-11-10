// Detail layer round 9: FLANK SLABS. The round-8 judge: the flank "covers the most screen area" and "looks almost the same as
// the undressed shot ... the big black faces are still bare triangles with lava cracks"; "dress the big flank faces with low,
// flat slab and ledge pieces laid along the contour on 25-45 degree ground: wide pieces, sunk about 60%, in the rounded
// grey-black family the crater rim already uses".
// erg-mcp plan_contour (new this round) traces iso-height lines over the cone every 3 map units of height and answers sites
// along them with the slope tangent; a run of sites is a band of face to dress. The judge asked for 25-45 degree ground,
// but on Caldera that ground is the trails' (a probe of plan_contour every 2 units of height: of the sites clear of the trail
// band, castles and spawns, 39 are under 45 degrees and about 1500 are 45-75): the bare faces the judge sees ARE the steep
// ones, so slabs go on 25-74 degrees, lying nearer flat on the face the steeper it is (pitch 0.5 to 0.85: a relief band
// of the face, not a shelf sticking out), sunk 60% of their depth into it. Each LEDGE is 2-4
// overlapping slabs on consecutive sites (one continuous lip, not beads: each slab is longer than the step between them),
// the first and last shorter so the ledge tapers out; a slab is the crater rim's rock mesh drawn wide and low (length
// 3-4.6 along the contour, 0.9-1.3 thick, 2-2.6 deep), pitched half-way between a shelf and a plate lying on the face, and
// sunk 60% of its depth into the face, so only a low rounded lip shows. Ledges are patchy (noise gaps, never two ledges
// within 5 units on one contour, stacked contours offset so they do not line up into a ladder), kept clear of the trail
// band and its kerbs, castles, spawns, pads, the factory, the lava and the other rock. Tagged _slb; tinted per elevation
// band by detail-retint.mjs.
import * as T from "./terrain.mjs";
import { keepOutFoot } from "./detail-keepout.mjs";
import { ROCK, rng, onTongue } from "./detail-rock.mjs";

const WATER = 1;
const PADS = T.LAUNCHERS.map(([rho, th]) => T.pointAt(rho, th));
// OFF by default (round 9, in game): the rock mesh drawn as a slab on these faces reads as a row of beads (1.5 x overlap)
// or a chain of links (2.6-3.3 x overlap) at the flank and shore cameras: "better none than necklaces" (the round-7
// judge). A real slab or ledge mesh needs the mod mesh-bank import (not built). DTL_SLAB_MAX=40 turns it back on.
export const SLAB_MAX = +(process.env.DTL_SLAB_MAX ?? 0);

export async function slabPass(ctx) {
  if (SLAB_MAX <= 0) return { off: "DTL_SLAB_MAX is 0: the slabs read as beads in game (round 9)" };
  const { call } = ctx, R = rng(20261110);
  const row = (await call("scenery_catalog", { bases: [ROCK.base], roles: ["scenery"], search: ROCK.resource, limit: 5 })).json.list.find((r) => r.resource === ROCK.resource);
  const stats = { runs: 0, sites: 0, ledges: 0, pieces: 0, rejected: {} };
  const rej = (why) => { stats.rejected[why] = (stats.rejected[why] ?? 0) + 1; };
  // the rock already there (its footprints): a slab never cuts into a crag
  const rocks = [];
  for (const q of ["_dtl", "_cpl"]) for (const d of (await call("list_scenery", { search: q, units: "map", limit: 2000 })).json.details) {
    const s = d.scale ?? [1, 1, 1];
    rocks.push({ x: d.at[0], z: d.at[2], r: 0.7 * 2.05 * Math.max(Math.abs(s[0]), Math.abs(s[2])) });
  }
  const clear = (x, z, rx, rz, yaw) => {
    if (T.rhoAt(x, z) < T.RC + 3) return "crater";
    if (onTongue(x, z, Math.max(rx, rz) * 0.6 + 0.8)) return "lava";
    if (PADS.some((p) => Math.hypot(x - p[0], z - p[1]) < 11 + rx)) return "near a pad";
    if (rocks.some((k) => Math.hypot(k.x - x, k.z - z) < k.r + rx * 0.6)) return "rock";
    const why = keepOutFoot(x, z, rx, rz, yaw, ctx.spawns, 0.6);
    return why ? why.replace(/^castle .*/, "castle") : null;
  };
  const plan = (await call("plan_contour", { area: { centre: [0, 0], rMin: T.RC + 3, rMax: 92 }, band: { from: 5, to: 44, every: 2.5 }, cell: 1.25,
    spacing: 2, jitter: 0.1, seed: 9109, minSlope: 25, maxSlope: 74, minRun: 2, limit: 20000 })).json;
  stats.runs = plan.runs.length; stats.sites = plan.total;
  // ledges: windows of 2-4 consecutive sites on a run, picked by noise, the best first
  const cands = [];
  for (const run of plan.runs) {
    for (let i = 0; i + 1 < run.sites.length; i += 1) {
      const s = run.sites[i], [x, , z] = s.pos;
      if (s.pos[1] < WATER + 3) continue;
      const w = (T.noise(x * 0.06, z * 0.06, 9911) + 0.55) * (0.7 + 0.6 * R()) * (s.slope < 70 ? 1 : 0.75);
      if (w < 0.35) continue;
      const n = Math.min(run.sites.length - i, 4 + Math.floor(R() * 3));
      if (n < 4) continue;
      cands.push({ w, sites: run.sites.slice(i, i + n), level: run.level });
    }
  }
  cands.sort((a, b) => b.w - a.w);
  const ledges = [], used = [];
  for (const c of cands) {
    if (ledges.reduce((k, l) => k + l.pieces.length, 0) >= SLAB_MAX) break;
    const mid = c.sites[c.sites.length >> 1].pos;
    // spacing: 7 from any ledge on the same or a neighbouring contour, 5 from any other (no ladders, no necklaces)
    if (used.some((u) => Math.hypot(u.x - mid[0], u.z - mid[2]) < (Math.abs(u.level - c.level) <= 3 ? 7 : 5))) { rej("crowded"); continue; }
    const pieces = [];
    let bad = null;
    c.sites.forEach((s, k) => {
      if (bad) return;
      const end = k === 0 || k === c.sites.length - 1;
      // [length along the contour, height of the band on the face, depth out of it]
      // each slab 2.6-3.3 times the step between them: one continuous lip (round 9 first try, 1.5 times, read as beads)
      const len = (end ? 3.6 : 5.2) + R() * 1.2, thick = 1.0 + R() * 0.3, depth = 1.8 + R() * 0.6;
      const yaw = Math.atan2(s.tangent[1], s.tangent[0]) * 180 / Math.PI;   // keepOutFoot takes degrees (was radians before round 11)
      const why = clear(s.pos[0], s.pos[2], len / 2, depth * 0.45, yaw);
      if (why) { bad = why; return; }
      pieces.push({ pos: s.pos.map((v) => +v.toFixed(2)), normal: s.normal, tangent: s.tangent, size: [+len.toFixed(2), +thick.toFixed(2), +depth.toFixed(2)],
        pitch: +Math.min(0.75, 0.55 + Math.max(0, s.slope - 40) / 150 + R() * 0.05).toFixed(2), embed: +(0.45 + R() * 0.06).toFixed(2),
        jitter: [+((R() - 0.5) * 4).toFixed(1), +((R() - 0.5) * 6).toFixed(1), +((R() - 0.5) * 4).toFixed(1)], tint: "#3a3b3f" });
    });
    if (bad) { rej(bad); continue; }
    ledges.push({ level: c.level, pieces });
    used.push({ x: mid[0], z: mid[2], level: c.level });
  }
  const all = ledges.flatMap((l) => l.pieces).slice(0, SLAB_MAX);
  for (let i = 0; i < all.length; i += 200) {
    const r = (await call("place_aligned", { base: ROCK.base, id: row.example.id, allowOtherTheme: true, units: "map", tag: "slb", pieces: all.slice(i, i + 200) })).json;
    stats.pieces += r.placed;
  }
  stats.ledges = ledges.length;
  stats.byLevel = Object.fromEntries([...new Set(ledges.map((l) => l.level))].sort((a, b) => a - b).map((lv) => [lv, ledges.filter((l) => l.level === lv).length]));
  ctx.say?.("slab", JSON.stringify({ runs: stats.runs, sites: stats.sites, ledges: stats.ledges, pieces: stats.pieces }));
  return stats;
}
