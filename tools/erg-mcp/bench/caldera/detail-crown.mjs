// Detail layer round 6: COLUMNAR CROWNS on the silhouette lines. Round 5's judge: the flat-topped columnar blocks on the
// crater rim are "the right vocabulary for this volcano"; spread them along the ridge crests and the tops of the outer
// cliffs, where they change the silhouette in the oblique and shore shots. erg-mcp find_crests answers the crest and
// cliff-top polylines; the clear runs of each (off the trail band, castles, spawns, pads, the factory and the lava) get a
// cluster of 3-6 upright columns every 4.5-8 units (gaps where the noise says so): a tall one a little behind the edge,
// the rest round it falling in height toward the drop, in a tight hexagonal pack, flat tops, leaning 2-6 degrees outward,
// each seated 15% of its height under the lowest ground of its footprint (no underside shows on the drop side). Copies of
// the rock mesh drawn narrow (height at most 2.2 times the width: the mesh lights pale when stretched further), tinted
// charcoal basalt.
import * as T from "./terrain.mjs";
import { keepOutFoot } from "./detail-keepout.mjs";
import { ROCK, rng, onTongue, BASALT } from "./detail-rock.mjs";

const PADS = T.LAUNCHERS.map(([rho, th]) => T.pointAt(rho, th));
const WATER = 1;

export async function crownPass(ctx) {
  const { call } = ctx, R = rng(20261113);
  const row = (await call("scenery_catalog", { bases: [ROCK.base], roles: ["scenery"], search: ROCK.resource, limit: 5 })).json.list.find((r) => r.resource === ROCK.resource);
  const stats = { lines: 0, runs: 0, clusters: 0, pieces: 0, rejected: {} };
  const rej = (why) => { stats.rejected[why] = (stats.rejected[why] ?? 0) + 1; };
  // two calls (each well under the server's patience): the crests, and the cliff tops with a looser test (the outer cliff
  // band is 6 high and softened: over 45 degrees within 4, from ground under 50)
  const box = [-100, -100, 100, 100];
  const cr = { lines: [
    ...(await call("find_crests", { box, units: "map", minY: WATER + 3, step: 1.5, minRun: 4, limit: 300, kinds: ["crest"] })).json.lines,
    ...(await call("find_crests", { box, units: "map", minY: WATER + 3, step: 1.5, minRun: 4, limit: 300, kinds: ["cliffTop"], topSlope: 50, cliffSlope: 45, reach: 4 })).json.lines,
  ] };
  stats.lines = cr.lines.length;
  const clearAt = (x, z, rad) => {
    if (onTongue(x, z, rad + 1)) return "lava";
    if (Math.hypot(x - T.LAKE.c[0], z - T.LAKE.c[1]) < T.LAKE.R + 2 + rad) return "lava lake";
    if (PADS.some((p) => Math.hypot(x - p[0], z - p[1]) < 12 + rad)) return "near a pad";
    const why = keepOutFoot(x, z, rad, rad, 0, ctx.spawns, 0.6);
    return why ? why.replace(/^castle .*/, "castle") : null;
  };
  // the clear runs of each line, then a cluster site every 4.5-8 units along each run
  const sites = [];
  for (const l of cr.lines) {
    let run = [];
    const flush = () => { if (run.length >= 3) { stats.runs++; sites.push(...sitesOf(run, l.kind)); } run = []; };
    for (const p of l.points) { if (clearAt(p.at[0], p.at[2], 2.2)) flush(); else run.push(p); }
    flush();
  }
  function sitesOf(run, kind) {
    const out = [];
    let acc = R() * 3;
    for (let i = 1; i < run.length; i++) {
      acc += Math.hypot(run[i].at[0] - run[i - 1].at[0], run[i].at[2] - run[i - 1].at[2]);
      if (acc < 4.5 + R() * 3.5) continue;
      acc = 0;
      const p = run[i];
      if (T.noise(p.at[0] * 0.08, p.at[2] * 0.08, 503) < -0.45) continue;   // a gap in the crown
      const q = run[Math.max(0, i - 1)], tl = Math.hypot(p.at[0] - q.at[0], p.at[2] - q.at[2]) || 1;
      out.push({ x: p.at[0], z: p.at[2], y: p.at[1], out: p.out, tan: [(p.at[0] - q.at[0]) / tl, (p.at[2] - q.at[2]) / tl], kind });
    }
    return out;
  }
  // spaced: no two clusters nearer than 4
  const kept = [];
  for (const s of sites) if (!kept.some((k) => Math.hypot(k.x - s.x, k.z - s.z) < 4)) kept.push(s);
  // the columns of each cluster: a hex pack round the site, the tallest a little behind the edge (inward, -out)
  const cols = [];
  for (const s of kept) {
    const n = 3 + Math.floor(R() * 4), w0 = 1.0 + R() * 0.45, hMain = (s.kind === "cliffTop" ? 3.2 : 2.6) + R() * 2.2;
    const ox = s.out[0], oz = s.out[1], tx = -oz, tz = ox;
    // a row along the crest, the second rank behind it (inward): nothing hangs over the drop
    const slots = [[0, -0.2], [0.95, 0], [-0.95, 0.05], [0.5, -0.85], [-0.5, -0.85], [1.9, -0.15], [-1.9, -0.1]];
    for (let i = 0; i < n; i++) {
      const [a, b] = slots[i], w = w0 * (0.8 + R() * 0.35), x = s.x + (tx * a + ox * b) * w0 * 0.95, z = s.z + (tz * a + oz * b) * w0 * 0.95;
      // falling toward the drop (b > 0 is outward), the rest within 55-90% of the main
      const h = i === 0 ? hMain : hMain * (0.55 + R() * 0.35) * (Math.abs(a) > 1.5 ? 0.75 : 1);
      cols.push({ x, z, w: Math.min(w, h / 1.15), h: Math.min(h, w * 2.2), site: s, i });
    }
  }
  // the ground under each column's footprint: the centre and four points at its half width
  const pts = cols.flatMap((c) => [[c.x, c.z], [c.x + c.w / 2, c.z], [c.x - c.w / 2, c.z], [c.x, c.z + c.w / 2], [c.x, c.z - c.w / 2]]);
  const gs = [];
  for (let i = 0; i < pts.length; i += 3500) gs.push(...(await call("surface_sample", { points: pts.slice(i, i + 3500), units: "map", step: 0.6 })).json.samples);
  const pieces = [];
  cols.forEach((c, k) => {
    const g = gs.slice(k * 5, k * 5 + 5);
    if (g.some((v) => !v)) { rej("no ground"); return; }
    const low = Math.min(...g.map((v) => v.y)), hi = Math.max(...g.map((v) => v.y));
    if (low < WATER + 0.5) { rej("water"); return; }
    if (hi - low > c.h * 0.6) { rej("too steep under it"); return; }
    const why = clearAt(c.x, c.z, c.w * 0.6);
    if (why) { rej(why); return; }
    const bottom = low - 0.15 * c.h, lean = 2 + R() * 4;
    pieces.push({ pos: [+c.x.toFixed(2), +(bottom + c.h / 2).toFixed(2), +c.z.toFixed(2)], normal: [c.site.out[0] * 0.3, 0.95, c.site.out[1] * 0.3], tangent: c.site.tan,
      size: [+c.w.toFixed(2), +c.h.toFixed(2), +(c.w * (0.9 + R() * 0.15)).toFixed(2)], pitch: 0, embed: 0.5,
      jitter: [+lean.toFixed(1), +(R() * 60 - 30).toFixed(1), +((R() - 0.5) * 3).toFixed(1)], tint: BASALT[(k + c.i) % BASALT.length] });
  });
  for (let i = 0; i < pieces.length; i += 200) {
    const r = (await call("place_aligned", { base: ROCK.base, id: row.example.id, allowOtherTheme: true, units: "map", tag: "dtl", pieces: pieces.slice(i, i + 200) })).json;
    stats.pieces += r.placed;
  }
  stats.clusters = kept.length;
  stats.byKind = { crest: kept.filter((s) => s.kind === "crest").length, cliffTop: kept.filter((s) => s.kind === "cliffTop").length };
  ctx.crowns = kept.map((s) => [+s.x.toFixed(1), +s.z.toFixed(1)]);
  ctx.say?.("crown", JSON.stringify({ lines: stats.lines, runs: stats.runs, clusters: stats.clusters, pieces: stats.pieces, byKind: stats.byKind }));
  return stats;
}
