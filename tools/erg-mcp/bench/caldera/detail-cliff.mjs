// Detail layer round 5: CLIFF OUTCROPS, the medium scale on the cone's biggest empty canvas. A free-ground map of the cone
// (2.5-unit cells: trail bed and kerb band, castle and pad keep-outs, lava, slope classes) shows the trails' columns
// covering the gentle ground and the rest steep: 718 cells over 58 degrees against 321 under 42. A crag seated on the
// lowest ground of its footprint vanishes into such a face (place_crags' seating, rounds 3-4), so these are placed the
// other way: each block is centred on the face at its own ground and sunk 45% of its height into it (copy_base_details
// {seat}), its back in the cliff and its front standing out, in strata of 3-4 along the contour (an anchor of 1.8-4
// and siblings of 70% and 50% beside it, stepping a little up or down), so the walls between the switchback columns and
// the crater's inner walls read as layered basalt instead of smooth paint. Kept clear like everything else: the whole
// footprint (and an overhang margin) off the trail band, castles, spawns, pads and the factory, off the lava.
import * as T from "./terrain.mjs";
import { keepOutFoot } from "./detail-keepout.mjs";
import { ROCK, rng, onTongue, lavaHeat } from "./detail-rock.mjs";

const WATER = 1;
// a step lighter than the free-standing crags, so a block reads against the dark red-brown and black cliff paint
export const CLIFF = ["#5a4e46", "#4e443e", "#544a43", "#4a423d"];
const SCORCHED = "#6a3a2c";
const hx = (c) => [1, 3, 5].map((i) => parseInt(c.slice(i, i + 2), 16));
const mix = (a, b, t) => "#" + hx(a).map((v, i) => Math.round(v + (hx(b)[i] - v) * Math.max(0, Math.min(1, t))).toString(16).padStart(2, "0")).join("");
const PADS = T.LAUNCHERS.map(([rho, th]) => T.pointAt(rho, th));

export async function cliffPass(ctx) {
  const { call } = ctx, R = rng(20261109);
  const row = (await call("scenery_catalog", { bases: [ROCK.base], roles: ["scenery"], search: ROCK.resource, limit: 5 })).json.list.find((r) => r.resource === ROCK.resource);
  const meshH = row.meshSize[1];
  const sample = async (pts) => {
    const res = [];
    for (let i = 0; i < pts.length; i += 3500) res.push(...(await call("surface_sample", { points: pts.slice(i, i + 3500), units: "map", step: 1.2 })).json.samples);
    return res;
  };
  const stats = { rejected: {}, anchors: 0, pieces: 0 };
  const rej = (why) => { stats.rejected[why] = (stats.rejected[why] ?? 0) + 1; };
  const others = (ctx.rockOut?.heroes ?? []).map((h) => ({ x: h.x, z: h.z, r: (h.dry?.radius ?? 3) + 1 }));
  const clear = (x, z, rad) => {
    if (onTongue(x, z, rad * 0.5 + 0.8)) return "lava";
    if (Math.hypot(x - T.LAKE.c[0], z - T.LAKE.c[1]) < T.LAKE.R + 1 + rad) return "lava lake";
    if (PADS.some((p) => Math.hypot(x - p[0], z - p[1]) < 11 + rad)) return "near a pad";
    const why = keepOutFoot(x, z, rad, rad, 0, ctx.spawns, 0.4);
    return why ? why.replace(/^castle .*/, "castle") : null;
  };
  // candidates: steep faces (over 52 degrees) on a 2-unit grid, the outer cone and the crater's inner walls
  const grid = [];
  for (let x = -100; x <= 100; x += 2) for (let z = -100; z <= 100; z += 2) {
    const rho = T.rhoAt(x, z);
    if (rho > 96 || rho < T.FLAT_R + 2) continue;
    grid.push({ x: x + (R() - 0.5), z: z + (R() - 0.5), rho });
  }
  const gs = await sample(grid.map((p) => [p.x, p.z]));
  const faces = grid.map((p, i) => ({ ...p, g: gs[i] })).filter((p) => p.g && p.g.slope >= 52 && p.g.y > WATER + 1.2);
  stats.faces = faces.length;
  // a patchy spread: noise patches, denser up the cone; the greedy pick keeps strata 6.5 apart
  faces.forEach((p) => { p.w = (T.noise(p.x * 0.09, p.z * 0.09, 401) + 0.6) * (0.6 + 0.4 * Math.min(1, (96 - p.rho) / 50)) * (0.7 + 0.6 * R()); });
  faces.sort((a, b) => b.w - a.w);
  const anchors = [];
  for (const p of faces) {
    if (anchors.length >= 160) break;
    if (p.w < 0.2) break;
    if (anchors.some((a) => Math.hypot(a.x - p.x, a.z - p.z) < 5) || others.some((o) => Math.hypot(o.x - p.x, o.z - p.z) < o.r + 2.5)) continue;
    const size = 1.8 + R() * 2.2, why = clear(p.x, p.z, size * 0.75 + 0.3);
    if (why) { rej(why); continue; }
    anchors.push({ ...p, size });
  }
  // the strata: siblings along the contour (perpendicular to downhill), each on its own steep ground
  const sibs = [];
  for (const a of anchors) {
    const n = a.g.normal, l = Math.hypot(n[0], n[2]) || 1, dx = n[0] / l, dz = n[2] / l, cx = -dz, cz = dx;
    const k = 1 + Math.floor(R() * 3);
    for (let i = 0; i < k; i++) {
      const side = i % 2 ? -1 : 1, f = i < 2 ? 0.7 : 0.5, d = a.size * (0.75 + 0.25 * R()) * (1 + Math.floor(i / 2) * 0.9);
      sibs.push({ x: a.x + cx * side * d + dx * (R() - 0.5) * 0.6, z: a.z + cz * side * d + dz * (R() - 0.5) * 0.6, size: a.size * f * (0.85 + 0.3 * R()), parent: a });
    }
  }
  const ss = await sample(sibs.map((s) => [s.x, s.z]));
  const blocks = anchors.map((a) => ({ ...a, kind: "anchor" }));
  sibs.forEach((s, i) => {
    const g = ss[i];
    if (!g || g.slope < 45 || g.y < WATER + 1) { rej("sibling off the face"); return; }
    const why = clear(s.x, s.z, s.size * 0.75 + 0.3);
    if (why) { rej(why); return; }
    blocks.push({ ...s, g, kind: "sibling" });
  });
  stats.anchors = anchors.length;
  const copies = blocks.map((b, i) => {
    const s = b.size / meshH, heat = lavaHeat(b.x, b.z), base = CLIFF[i % CLIFF.length];
    return { id: row.example.id, pos: [+b.x.toFixed(2), +b.g.y.toFixed(2), +b.z.toFixed(2)], seat: 0.45,
      rot: [Math.round((R() - 0.5) * 16), Math.round(R() * 360), Math.round((R() - 0.5) * 16)],
      scale: [+(s * (1 + R() * 0.25)).toFixed(3), +(s * (0.75 + R() * 0.2)).toFixed(3), +(s * (1 + R() * 0.25)).toFixed(3)],
      tint: heat > 0 ? mix(base, SCORCHED, 0.3 * heat) : base };
  });
  for (let i = 0; i < copies.length; i += 200) {
    await call("copy_base_details", { base: ROCK.base, allowOtherTheme: true, units: "map", tag: "dtl", copies: copies.slice(i, i + 200) });
  }
  stats.pieces = copies.length;
  stats.sizes = { "2.4+": blocks.filter((b) => b.size >= 2.4).length, "1.2-2.4": blocks.filter((b) => b.size < 2.4 && b.size >= 1.2).length, "<1.2": blocks.filter((b) => b.size < 1.2).length };
  ctx.say?.("cliff", JSON.stringify({ faces: stats.faces, anchors: stats.anchors, pieces: stats.pieces }));
  return stats;
}
