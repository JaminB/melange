// Detail layer round 11: the round-10 judge's change list, after every earlier pass (detail.mjs runs these last):
//   strings   vertical strings of rock (the judge's Seawall upper right and the flank's left edge: "a column of blobs"):
//             a group of touching rock copies taller than it is wide keeps its lowest piece; a TALUS FAN with a toe and a
//             skirt is laid downhill from that foot instead
//   beach     the small dark stones off the beaches (the Strand and Seawall sands): every rock copy under 1.3 across on
//             ground under 4.5 high and under 20 degrees
//   tumble    the shore's hero stacks (pieces over 2.0 across near the sea) become GRADED TUMBLES: one 2.0 block, a ring
//             of 1.4-1.8 blocks and wedges, an outer ring of 1.0-1.3 slabs and shards, one block seated on top, every piece
//             on its own ground sample and none over 2.0
//   rim       crater rim breakup: clusters of scoria (shards), spatter (flat slabs) and small faceted blocks on the rim
//             crest, outside the pads, the trails' ends and the lava
//   flora2    the flora's missing places: the FILL (downhill) side of the switchbacks' bends; HEDGE ROWS of tussock and
//             low shrubs along the lawn tiles' edges (the hard checker edges); low scrub clumps in the dry and ash belts
//   lint11    the mine look-alike lint over the new rock (erg-mcp lint_dressing)
// All scenery copies (no collision): sim_fingerprint after every pass, as for every other pass.
import * as T from "./terrain.mjs";
import { rng, onTongue } from "./detail-rock.mjs";
import { keepOutFlora } from "./detail-keepout.mjs";
import { meshBoxes, sampler, contact, clearOf, fan, boulderPiece, talusVariant } from "./detail-basalt.mjs";
import { FLORA, rampTint } from "./detail-flora.mjs";
import { ROCK_VARIANTS } from "./meshes/gen.mjs";

const WATER = 1;
const MAXW = 2.0;   // the judge: no piece of a shore tumble over 2.0 across

/** Every rock copy of the layer (the caldera.* meshes), with its drawn width and height (map units). */
async function rocks(call) {
  const out = [], B = meshBoxes();
  for (const v of ROCK_VARIANTS) for (const d of (await call("list_scenery", { search: v, units: "map", limit: 2000 })).json.details) {
    if (d.resource !== v) continue;
    const s = (d.scale ?? [1, 1, 1]).map(Math.abs), b = B[v];
    out.push({ id: d.id, v, x: d.at[0], y: d.at[1], z: d.at[2], w: Math.max((b.max[0] - b.min[0]) * s[0], (b.max[2] - b.min[2]) * s[2]), h: (b.max[1] - b.min[1]) * s[1], name: d.name });
  }
  return out;
}
async function placeRock(call, pieces, stem, tag = "dtl") {
  const by = new Map();
  for (const p of pieces) { if (!by.has(p.v)) by.set(p.v, []); by.get(p.v).push(p.piece); }
  let n = 0;
  for (const [v, list] of by) for (let i = 0; i < list.length; i += 200) n += (await call("place_mod_mesh", { resource: v, box: meshBoxes()[v], stem, units: "map", tag, pieces: list.slice(i, i + 200) })).json.placed;
  return n;
}
/** Union-find groups of touching pieces. */
function groups(list, touch) {
  const p = list.map((_, i) => i), f = (i) => (p[i] === i ? i : (p[i] = f(p[i])));
  for (let i = 0; i < list.length; i++) for (let j = i + 1; j < list.length; j++) if (touch(list[i], list[j])) p[f(i)] = f(j);
  const g = new Map();
  list.forEach((x, i) => { const k = f(i); if (!g.has(k)) g.set(k, []); g.get(k).push(x); });
  return [...g.values()];
}

// ---------------------------------------------------------------- strings -> fans
export async function stringsPass(ctx) {
  const { call } = ctx, R = rng(20261121), sample = sampler(call), clear = clearOf(ctx);
  const all = await rocks(call);
  // round 11 in game: Seawall's string climbs a 47-50 degree face diagonally (6 to 13 high over 12 across): a chain of
  // pieces near each other (within 0.8 of their widths plus 0.6) that climbs 3 or more, most of it on ground over 44
  // degrees, is a string, whatever its slant
  const gr = await sample(all.map((p) => [p.x, p.z]));
  all.forEach((p, i) => { p.slope = gr[i]?.slope ?? 0; });
  const gs = groups(all, (a, b) => Math.hypot(a.x - b.x, a.z - b.z) < (a.w + b.w) * 0.8 + 0.6 && Math.abs(a.y - b.y) < 4);
  const remove = [], feet = [];
  for (const g of gs) {
    if (g.length < 3) continue;
    const ys = g.map((p) => p.y), yr = Math.max(...ys) - Math.min(...ys);
    let ext = 0;
    for (const a of g) for (const b of g) ext = Math.max(ext, Math.hypot(a.x - b.x, a.z - b.z));
    const steep = g.filter((p) => p.slope > 44).length / g.length;
    if (!((yr >= 2.5 && yr >= 0.9 * ext) || (yr >= 3 && steep > 0.5))) continue;
    g.sort((a, b) => a.y - b.y);
    remove.push(...g.slice(1).map((p) => p.id));
    feet.push(g[0]);
  }
  if (remove.length) await call("remove_scenery", { ids: remove });
  // a fan downhill of each kept foot
  // the fan's apex: down the fall line from the foot (the ground's horizontal normal), the first gentle ground (under 40
  // degrees) within 12; the fan spreads from there, its toe further down
  const pieces = [], sites = [];
  for (const f of feet) {
    let at = [f.x, f.z], g = (await sample([at]))[0];
    for (let k = 0; g && k < 12 && g.slope >= 40; k++) {
      const l = Math.hypot(g.normal[0], g.normal[2]) || 1;
      at = [at[0] + g.normal[0] / l, at[1] + g.normal[2] / l];
      g = (await sample([at]))[0];
    }
    if (!g || g.slope >= 40) continue;
    const l = Math.hypot(g.normal[0], g.normal[2]) || 1, site = { xz: at, down: [g.normal[0] / l, g.normal[2] / l] };
    sites.push(at.map((v) => +v.toFixed(1)));
    for (const p of fan(site, 7 + Math.floor(R() * 3), 4, 1.8, 0.55, R)) pieces.push(p);
  }
  const ps = await sample(pieces.map((p) => [p.x, p.z]));
  const out = [];
  pieces.forEach((p, i) => {
    const g = ps[i];
    if (!g || g.y < WATER + 0.3 || contact(g.normal) > 52 || clear(p.x, p.z, p.size * 0.55)) return;
    if (out.some((q) => Math.hypot(q.x - p.x, q.z - p.z) < (q.size + p.size) * 0.32)) return;
    const v = talusVariant(p.kind, R);
    out.push({ x: p.x, z: p.z, size: p.size, v, piece: boulderPiece(meshBoxes()[v], p.x, p.z, g, Math.min(p.size, MAXW), p.embed, v === "caldera.Slab" ? 1 : 0.8, R) });
  });
  const placed = await placeRock(call, out, "fan");
  return { groups: gs.length, strings: feet.length, removed: remove.length, fanPieces: placed, fanSites: sites, feet: feet.map((f) => [+f.x.toFixed(1), +f.y.toFixed(1), +f.z.toFixed(1)]) };
}

// ---------------------------------------------------------------- beach pebbles
export async function beachPass(ctx) {
  const { call } = ctx, sample = sampler(call);
  const small = (await rocks(call)).filter((p) => p.w < 1.3 && p.y < 4.5);
  const gs = await sample(small.map((p) => [p.x, p.z]));
  const ids = small.filter((p, i) => gs[i] && gs[i].y < 4.5 && gs[i].y > WATER - 0.4 && gs[i].slope < 20).map((p) => p.id);
  if (ids.length) await call("remove_scenery", { ids });
  return { checked: small.length, removed: ids.length };
}

// ---------------------------------------------------------------- shore stacks -> graded tumbles
export async function tumblePass(ctx) {
  const { call } = ctx, R = rng(20261122), sample = sampler(call), clear = clearOf(ctx);
  const all = await rocks(call);
  const big = all.filter((p) => p.w > MAXW && p.y < 6 && T.rhoAt(p.x, p.z) > 78);
  const gs = groups(big, (a, b) => Math.hypot(a.x - b.x, a.z - b.z) < 5);
  const remove = [], out = [], sites = [];
  for (const g of gs) {
    const n0 = out.length;
    let cx = g.reduce((s, p) => s + p.x, 0) / g.length, cz = g.reduce((s, p) => s + p.z, 0) / g.length;
    let ext = 0;
    for (const p of g) ext = Math.max(ext, Math.hypot(p.x - cx, p.z - cz) + p.w / 2);
    // the small rocks inside the stack's footprint go with it (its old skirt), the tumble brings its own
    for (const p of all) if (Math.hypot(p.x - cx, p.z - cz) < ext && !remove.includes(p.id)) remove.push(p.id);
    // a stack stood in the sea: its tumble comes ashore, to where the bed is at most half a unit under the water (round 11
    // in game: a tumble on a deep bed showed nothing over the water)
    for (let k = 0; k < 14; k++) {
      const b = (await sample([[cx, cz]]))[0];
      if (b && b.y > WATER + 0.1) break;
      const l = Math.hypot(cx, cz) || 1;
      cx -= (cx / l) * 0.8; cz -= (cz / l) * 0.8;
    }
    // the rings: [count, radius from the centre, size range, variants]
    const rad = Math.max(1.6, Math.min(4.5, ext * 0.7));
    const plan = [[1, 0, [MAXW, MAXW], ["caldera.Boulder"]], [3 + Math.floor(R() * 2), rad * 0.55, [1.4, 1.8], ["caldera.Boulder", "caldera.Wedge"]],
      [5 + Math.floor(R() * 3), rad, [1.0, 1.3], ["caldera.Slab", "caldera.Shard", "caldera.Wedge"]]];
    const ring = [];
    for (const [n, r, [s0, s1], vs] of plan) {
      const a0 = R() * Math.PI * 2;
      for (let i = 0; i < n; i++) {
        const a = a0 + (i / n) * Math.PI * 2 + (R() - 0.5) * 0.6, d = r * (0.85 + R() * 0.3);
        ring.push({ x: cx + Math.cos(a) * d, z: cz + Math.sin(a) * d, size: s0 + (s1 - s0) * R(), v: vs[Math.floor(R() * vs.length)], top: false });
      }
    }
    // one block seated on the ring, against the centre block (a tumble, not a fence)
    { const a = R() * Math.PI * 2; ring.push({ x: cx + Math.cos(a) * 0.9, z: cz + Math.sin(a) * 0.9, size: 1.3 + R() * 0.3, v: "caldera.Wedge", top: true }); }
    const ss = await sample(ring.map((p) => [p.x, p.z]));
    const g0 = ss[0];
    ring.forEach((p, i) => {
      let g = ss[i];
      if (!g) return;
      if (p.top && g0) g = { ...g, y: Math.max(g.y, g0.y) + 0.9 };   // on the shoulder of the centre block
      // it must show over the water: the piece's top (about 0.6 of its size) over the sea
      if (g.y + p.size * 0.55 < WATER + 0.25) return;
      if (contact(g.normal) > 46 && !p.top) return;   // a tumble rests on the beach or the bed, never up a face
      if (clear(p.x, p.z, p.size * 0.5)) return;
      out.push({ ...p, piece: boulderPiece(meshBoxes()[p.v], p.x, p.z, g, p.size, p.top ? 0.25 : 0.3 + R() * 0.12, p.v === "caldera.Slab" ? 1 : 0.85, R) });
    });
    sites.push([+cx.toFixed(1), +cz.toFixed(1), out.length - n0]);
  }
  if (remove.length) await call("remove_scenery", { ids: remove });
  const placed = await placeRock(call, out, "tumble");
  return { stacks: gs.length, removed: remove.length, placed, sites };
}

// ---------------------------------------------------------------- crater rim breakup
export const RIM_SITES = +(process.env.DTL_RIM_SITES ?? 26);
export async function rimPass(ctx) {
  const { call } = ctx, R = rng(20261123), sample = sampler(call), clear = clearOf(ctx);
  // the crest band: on each radial line from RC - 6 to RC + 10, every point within 4 under the line's highest point (the
  // crest and its shoulders both sides, where the rim reads against the sky and the crater)
  const N = 120, K = 17, lines = [];
  for (let i = 0; i < N; i++) { const th = (i / N) * Math.PI * 2; for (let k = 0; k < K; k++) lines.push({ th, pt: T.pointAt(T.RC - 6 + k, th) }); }
  const ls = await sample(lines.map((l) => l.pt));
  const crest = [];
  for (let i = 0; i < N; i++) {
    const top = Math.max(...Array.from({ length: K }, (_, k) => ls[i * K + k]?.y ?? -Infinity));
    for (let k = 0; k < K; k++) { const j = i * K + k, g = ls[j]; if (g && g.y > top - 4 && g.slope < 45) crest.push({ th: lines[j].th, xz: lines[j].pt, g }); }
  }
  const stats = { crest: crest.length, sites: 0, pieces: 0, rejected: {} };
  const rej = (w) => { stats.rejected[w] = (stats.rejected[w] ?? 0) + 1; };
  const sites = [];
  for (const c of crest.map((c) => ({ ...c, w: R() + 0.4 * T.noise(c.xz[0] * 0.1, c.xz[1] * 0.1, 4411) })).sort((a, b) => b.w - a.w)) {
    if (sites.length >= RIM_SITES) break;
    if (sites.some((s) => Math.hypot(s.xz[0] - c.xz[0], s.xz[1] - c.xz[1]) < 5.5)) continue;
    const why = clear(c.xz[0], c.xz[1], 1.6);
    if (why) { rej(why); continue; }
    sites.push(c);
  }
  stats.sites = sites.length;
  // each site: 1-2 small faceted blocks, 2-3 scoria shards, 1-2 spatter slabs, within 2 of the centre (a clump over 1.5 across)
  const pieces = [];
  for (const s of sites) {
    const n = [["caldera.Boulder", 1 + Math.floor(R() * 2), [0.8, 1.4]], ["caldera.Shard", 2 + Math.floor(R() * 2), [0.5, 0.9]], ["caldera.Slab", 1 + Math.floor(R() * 2), [0.6, 1.1]]];
    for (const [v, k, [a, b]] of n) for (let i = 0; i < k; i++) {
      const ang = R() * Math.PI * 2, d = 0.4 + R() * 1.7;
      pieces.push({ v, x: s.xz[0] + Math.cos(ang) * d, z: s.xz[1] + Math.sin(ang) * d, size: a + (b - a) * R() });
    }
  }
  const ps = await sample(pieces.map((p) => [p.x, p.z]));
  const out = [];
  pieces.forEach((p, i) => {
    const g = ps[i];
    if (!g) return;
    if (contact(g.normal) > 50) { rej("steep"); return; }
    const why = clear(p.x, p.z, p.size * 0.5);
    if (why) { rej(why); return; }
    if (out.some((q) => Math.hypot(q.x - p.x, q.z - p.z) < (q.size + p.size) * 0.3)) { rej("overlap"); return; }
    out.push({ ...p, piece: boulderPiece(meshBoxes()[p.v], p.x, p.z, g, p.size, 0.3 + R() * 0.2, p.v === "caldera.Slab" ? 0.7 : 0.85, R) });
  });
  stats.pieces = await placeRock(call, out, "rim");
  return stats;
}

// ---------------------------------------------------------------- flora: bends, hedge rows, scrub
export const FLORA2_MAX = +(process.env.DTL_FLORA2_MAX ?? 220);
const RECIPE = {
  lush: [["tussock", 1, 2], ["shrub", 1, 2], ["weed", 0, 1], ["grass", 1, 2], ["flower", 0, 1]],
  hardy: [["succulent", 1, 2], ["agave", 0, 1], ["tussock", 1, 2], ["tuft", 1, 2], ["grass", 0, 1]],
  ash: [["tuft", 2, 3], ["tussock", 1, 2]],
  fringe: [["tussock", 1, 1], ["grass", 1, 2], ["tuft", 0, 1]],
};
export async function flora2Pass(ctx) {
  const { call } = ctx, R = rng(20261124), sample = sampler(call);
  const stats = { bends: 0, hedges: 0, scrub: 0, pieces: 0, byKind: {}, rejected: {} };
  const rej = (w) => { stats.rejected[w] = (stats.rejected[w] ?? 0) + 1; };
  const ids = {};
  for (const [kind, list] of Object.entries(FLORA)) for (const p of list) {
    const row = (await call("scenery_catalog", { bases: [p.base], roles: ["scenery"], search: p.resource, limit: 10 })).json.list.find((r) => r.resource === p.resource);
    if (!row) continue;
    const sz = row.size ?? row.meshSize ?? [1, 1, 1];
    (ids[kind] ??= []).push({ ...p, id: row.example.id, sc: row.example.scale ?? [1, 1, 1], r: 0.5 * Math.max(sz[0], sz[2]), h: sz[1] });
  }
  const rockList = await rocks(call);
  const inRock = (x, z) => rockList.some((k) => Math.hypot(k.x - x, k.z - z) < k.w * 0.45);
  const lava = (x, z, m) => onTongue(x, z, m) || Math.hypot(x - T.LAKE.c[0], z - T.LAKE.c[1]) < T.LAKE.R + 2 + m;
  const plants = (await call("list_scenery", { search: "_flo", units: "map", limit: 2000 })).json.details.map((d) => [d.at[0], d.at[2]]);
  const centres = [];
  const free = (x, z, sep) => !centres.some((c) => Math.hypot(c.x - x, c.z - z) < sep) && !plants.some((p) => Math.hypot(p[0] - x, p[1] - z) < sep * 0.6);
  // (a) the fill side of every bend: 7-10 from the hairpin, at least 0.8 under the trail there, never on the kerb band
  const ring = [];
  for (const t of T.TRAILS) for (const h of t.hairpins ?? []) for (let i = 0; i < 24; i++) {
    const a = (i / 24) * Math.PI * 2, d = 8.3 + R() * 4.5;
    ring.push({ x: h[0] + Math.cos(a) * d, z: h[1] + Math.sin(a) * d, hy: h[2] });
  }
  const rs = await sample(ring.map((p) => [p.x, p.z]));
  const bendC = ring.map((p, i) => ({ ...p, g: rs[i], zone: "bend" })).filter((p) => p.g && p.g.y < p.hy - 0.3 && p.g.y > WATER + 1 && p.g.slope < 48 && p.g.y < 30 && !lava(p.x, p.z, 1.5) && !inRock(p.x, p.z) && !keepOutFlora(p.x, p.z, 0.6, 0.6, 0, ctx.spawns, 0.2))
    .sort(() => R() - 0.5);
  stats.bendCandidates = bendC.length;
  for (const c of bendC) if (free(c.x, c.z, 4.5)) { centres.push(c); stats.bends++; }
  // (b) hedge rows on the lawn tiles' edges (grass or olive meeting another material on the same surface)
  const S = 1.25, mg = [];
  for (let x = -100; x <= 100; x += S) for (let z = -100; z <= 100; z += S) { const rho = T.rhoAt(x, z); if (rho > T.RC + 10 && rho < 112) mg.push([x, z]); }
  const mat = new Map(), key = (x, z) => `${x.toFixed(2)},${z.toFixed(2)}`;
  for (let i = 0; i < mg.length; i += 4000) (await call("surface_material", { points: mg.slice(i, i + 4000) })).json.samples.forEach((s, k) => mat.set(key(...mg[i + k]), s));
  const lawn = (m) => m === 0 || m === 2, edges = [];
  for (const [x, z] of mg) {
    const s = mat.get(key(x, z));
    if (!s || !lawn(s.material) || s.y < 2.5 || s.y > 12) continue;
    for (const [dx, dz] of [[S, 0], [0, S], [-S, 0], [0, -S]]) {
      const t = mat.get(key(x + dx, z + dz));
      if (t && !lawn(t.material) && Math.abs(t.y - s.y) < 1) edges.push({ x: x + dx / 2, z: z + dz / 2, dir: dx ? [0, 1] : [1, 0] });
    }
  }
  stats.edgePoints = edges.length;
  const rows = [];
  for (const e of edges.sort(() => R() - 0.5)) {
    if (rows.length >= 34) break;
    if (rows.some((r) => Math.hypot(r.x - e.x, r.z - e.z) < 5) || keepOutFlora(e.x, e.z, 1, 1, 0, ctx.spawns, 0.2) || lava(e.x, e.z, 1) || inRock(e.x, e.z)) continue;
    // the row: the edge points within 3.5 along the edge's line, every 1.1
    const run = edges.filter((o) => Math.abs((o.x - e.x) * e.dir[1] - (o.z - e.z) * e.dir[0]) < 0.3 && Math.hypot(o.x - e.x, o.z - e.z) < 4.8);
    if (run.length < 3) continue;
    rows.push({ x: e.x, z: e.z, run });
  }
  stats.hedges = rows.length;
  // (c) scrub in the dry and ash belts (12-27 high, under 48 degrees)
  const grid = [];
  for (let x = -96; x <= 96; x += 1.5) for (let z = -96; z <= 96; z += 1.5) { const rho = T.rhoAt(x, z); if (rho > T.RC + 8 && rho < 88) grid.push([x + (R() - 0.5) * 1.0, z + (R() - 0.5) * 1.0]); }
  const gg = await sample(grid);
  const why = (c) => !c.g ? "nog" : c.g.y < 10 || c.g.y >= 30 ? "height" : c.g.slope >= 48 ? "steep" : lava(c.x, c.z, 1.5) ? "lava" : inRock(c.x, c.z) ? "rock" : keepOutFlora(c.x, c.z, 0.8, 0.8, 0, ctx.spawns, 0.3) ? "keepout" : null;
  stats.scrubWhy = {};
  const scrubC = grid.map((p, i) => ({ x: p[0], z: p[1], g: gg[i], zone: "scrub" })).filter((c) => { const w = why(c); if (w) stats.scrubWhy[w] = (stats.scrubWhy[w] ?? 0) + 1; return !w; })
    .map((c) => ({ ...c, w: R() + 0.6 * T.noise(c.x * 0.06, c.z * 0.06, 8123) })).sort((a, b) => b.w - a.w);
  stats.scrubCandidates = scrubC.length;
  for (const c of scrubC) { if (stats.scrub >= 34) break; if (free(c.x, c.z, 3.6)) { centres.push(c); stats.scrub++; } }
  // (d) the shore fringe: low dune grass where the beach meets the greens (1.8-5 high, under 30 degrees)
  {
    const fr = [];
    for (let x = -112; x <= 112; x += 1.6) for (let z = -112; z <= 112; z += 1.6) { const rho = T.rhoAt(x, z); if (rho > 70 && rho < 112) fr.push([x + (R() - 0.5), z + (R() - 0.5)]); }
    const fg = await sample(fr);
    const fc = fr.map((p, i) => ({ x: p[0], z: p[1], g: fg[i], zone: "fringe" })).filter((c) => c.g && c.g.y > 1.8 && c.g.y < 5 && c.g.slope < 30 && !lava(c.x, c.z, 1.5) && !inRock(c.x, c.z) && !keepOutFlora(c.x, c.z, 0.8, 0.8, 0, ctx.spawns, 0.3))
      .map((c) => ({ ...c, w: R() + 0.7 * T.noise(c.x * 0.09, c.z * 0.09, 6161) })).sort((a, b) => b.w - a.w);
    stats.fringe = 0;
    for (const c of fc) { if (stats.fringe >= 30) break; if (free(c.x, c.z, 3.4)) { centres.push(c); stats.fringe++; } }
  }
  // members
  const mem = [];
  for (const c of centres) {
    const y = c.g.y, rec = RECIPE[c.zone === "fringe" ? "fringe" : y < 14 ? "lush" : y < 21 ? "hardy" : "ash"];
    for (const [kind, n0, n1] of rec) for (let i = 0, n = n0 + Math.floor(R() * (n1 - n0 + 1)); i < n; i++) {
      const a = R() * Math.PI * 2, d = 0.3 + R() * 1.4;
      mem.push({ x: c.x + Math.cos(a) * d, z: c.z + Math.sin(a) * d, kind, f: d / 1.7, zone: c.zone });
    }
  }
  for (const r of rows) for (const [i, o] of r.run.entries()) {
    const kind = i % 2 ? "shrub" : "tussock";
    mem.push({ x: o.x + (R() - 0.5) * 0.3, z: o.z + (R() - 0.5) * 0.3, kind, f: 0.3, zone: "hedge" });
    // the row straddles the edge: a second, lower line of grass and leaf on the other tile
    const side = (i % 2 ? 1 : -1) * (0.5 + R() * 0.3);
    mem.push({ x: o.x + r.run[0].dir[1] * side, z: o.z + r.run[0].dir[0] * side, kind: R() < 0.6 ? "grass" : "weed", f: 0.6, zone: "hedge" });
  }
  const ms = await sample(mem.map((m) => [m.x, m.z]));
  const byBase = new Map();
  let n = 0;
  mem.forEach((m, i) => {
    const g = ms[i];
    if (n >= FLORA2_MAX) { rej("budget"); return; }
    if (!g || g.y < WATER + 0.6) { rej("water"); return; }
    if (g.slope > 46) { rej("steep"); return; }
    const opts = ids[m.kind]; if (!opts) return;
    const p = opts[Math.floor(R() * opts.length)];
    let mul = (p.mul ?? 1) * (0.85 + R() * 0.3) * (1 - 0.3 * m.f);
    if (p.h * mul < 0.36) mul = 0.36 / p.h;
    // hedge rows stand on walkable lawn: low (under 0.8)
    if (m.zone === "hedge" && p.h * mul > 0.8) mul = 0.8 / p.h;
    const rad = p.r * mul;
    if (lava(m.x, m.z, rad + 0.5) || inRock(m.x, m.z)) { rej("lava/rock"); return; }
    const why = keepOutFlora(m.x, m.z, rad, rad, 0, ctx.spawns, 0.5);
    if (why) { rej(why); return; }
    const tint = m.kind === "flower" ? null : rampTint(g.y, m.f, R);
    if (!byBase.has(p.base)) byBase.set(p.base, []);
    byBase.get(p.base).push({ id: p.id, xz: [+m.x.toFixed(2), +m.z.toFixed(2)], seat: g.slope > 30 ? 0.3 : g.slope > 18 ? 0.22 : 0.12, rot: [Math.round((R() - 0.5) * 8), Math.round(R() * 360), Math.round((R() - 0.5) * 8)],
      scale: p.sc.map((v) => +(v * mul).toFixed(3)), ...(tint ? { tint } : {}) });
    stats.byKind[m.kind] = (stats.byKind[m.kind] ?? 0) + 1;
    n++;
  });
  for (const [base, copies] of byBase) for (let i = 0; i < copies.length; i += 200) stats.pieces += (await call("copy_base_details", { base, allowOtherTheme: true, units: "map", tag: "flo", copies: copies.slice(i, i + 200) })).json.placed.length;
  ctx.say?.("flora2", JSON.stringify({ bends: stats.bends, hedges: stats.hedges, scrub: stats.scrub, fringe: stats.fringe, pieces: stats.pieces }));
  return stats;
}

// ---------------------------------------------------------------- the mine look-alike lint over the round's rock
export async function lint11Pass(ctx) {
  const { call } = ctx;
  const lint = (await call("lint_dressing", { search: "_dtl", water: WATER + 0.3, limit: 500 })).json;
  const sus = lint.mineSuspects.list.flatMap((c) => c.ids);
  if (sus.length) await call("remove_scenery", { ids: sus });
  const tal = (await call("lint_dressing", { search: "_tal", water: WATER + 0.3, limit: 500 })).json;
  const sus2 = tal.mineSuspects.list.flatMap((c) => c.ids);
  if (sus2.length) await call("remove_scenery", { ids: sus2 });
  const flo = (await call("list_scenery", { search: "_flo", units: "map", limit: 2000 })).json.total;
  const rock = (await rocks(call)).length;
  return { removedDtl: sus.length, removedTal: sus2.length, hovering: lint.contact.hovering, buried: lint.contact.buried, flora: flo, rock };
}
