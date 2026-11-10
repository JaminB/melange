// Detail layer round 6: STRATA BANDS on the steep faces, replacing round 5's cliff "strata" (clusters of the round
// boulder stuck on near-vertical faces, which read as rocks glued to a wall). A level artist's strata are horizontal:
// long, low, flattened slabs following the contour, half sunk into the face, in bands at repeated heights. erg-mcp
// plan_ledges finds the faces (connected cells over 52 degrees) and traces their contours at LEVEL heights shared by every
// face (every 7 map units from 3: the same layer shows on each face it crosses, so the cone reads as stacked flows), and
// cuts each contour into slabs with gaps and breaks; place_aligned lays each one as a flattened copy of the rock mesh
// (length : depth : thickness about 3 : 1 : 0.6), its length along the contour, its depth out of the face pitched a
// little toward the normal, sunk 40-60% of its depth into the face, dipping a few degrees. Lower bands are a little
// thicker (older, heavier flows). Every slab's footprint keeps clear like everything else (trail band, castles, spawns,
// pads, the factory, lava). Slate-charcoal tint: a step lighter than the face so the shelf's lit top reads, never lighter
// than the castle stone. Tagged _ldg (a strata shelf overhangs its face on purpose, so the perched lint, which reads _dtl, skips it).
import * as T from "./terrain.mjs";
import { keepOutFoot } from "./detail-keepout.mjs";
import { ROCK, rng, onTongue } from "./detail-rock.mjs";

// (round 6 in the game: the lip's top catches the sky and draws pale; a darker slate keeps it a ledge, not a scratch)
export const STRATA = ["#55565b", "#4f5055", "#5a5b60", "#525358"];
const PADS = T.LAUNCHERS.map(([rho, th]) => T.pointAt(rho, th));
const WATER = 1;

export async function ledgePass(ctx) {
  const { call } = ctx, R = rng(20261112);
  const row = (await call("scenery_catalog", { bases: [ROCK.base], roles: ["scenery"], search: ROCK.resource, limit: 5 })).json.list.find((r) => r.resource === ROCK.resource);
  const stats = { rejected: {}, faces: 0, bands: 0, planned: 0, slabs: 0, pieces: 0, byHeight: {} };
  const rej = (why) => { stats.rejected[why] = (stats.rejected[why] ?? 0) + 1; };
  const plan = (await call("plan_ledges", { box: [-100, -100, 100, 100], units: "map", minY: WATER + 3, step: 1.5, minSlope: 52, minCells: 20,
    spacing: 7, offset: 3, length: [5, 8.5], gap: [0, 0.5], breaks: 0.1, minRun: 6, seed: 61 })).json;
  stats.faces = plan.faces.length; stats.bands = plan.bands.length; stats.planned = plan.slabs.length;
  const clear = (x, z, rad) => {
    if (onTongue(x, z, rad * 0.5 + 0.8)) return "lava";
    if (Math.hypot(x - T.LAKE.c[0], z - T.LAKE.c[1]) < T.LAKE.R + 1 + rad) return "lava lake";
    if (PADS.some((p) => Math.hypot(x - p[0], z - p[1]) < 11 + rad)) return "near a pad";
    const why = keepOutFoot(x, z, rad, rad, 0, ctx.spawns, 0.4);
    return why ? why.replace(/^castle .*/, "castle") : null;
  };
  const pieces = [];
  for (const s of plan.slabs) {
    const L = s.length * (0.95 + R() * 0.1), why = clear(s.at[0], s.at[2], L / 2 + 0.3);
    if (why) { rej(why); continue; }
    // lower bands a little heavier
    // round 6 in the game: short slabs standing half out of the face drew as mushroom caps (the squashed boulder is a lens);
    // long ones mostly in the face show only their lip, a continuous ledge line
    const heavy = 1 + Math.max(0, (40 - s.height) / 40) * 0.35;
    const D = (1.4 + R() * 0.6) * heavy, Tk = (0.75 + R() * 0.35) * heavy;
    pieces.push({ pos: s.at, normal: s.normal, tangent: s.tangent, size: [+(L * 1.12).toFixed(2), +Tk.toFixed(2), +D.toFixed(2)],
      pitch: +(0.08 + R() * 0.12).toFixed(2), embed: +(0.58 + R() * 0.08).toFixed(2),
      jitter: [+((R() - 0.5) * 4).toFixed(1), +((R() - 0.5) * 4).toFixed(1), +((R() - 0.5) * 5).toFixed(1)], tint: STRATA[Math.floor(R() * STRATA.length)] });
    stats.byHeight[s.height] = (stats.byHeight[s.height] ?? 0) + 1;
    // a third of the slabs get a thinner under-layer, a little shorter and set back: the band reads as a layered edge
    if (R() < 0.2) {
      pieces.push({ pos: s.at, normal: s.normal, tangent: s.tangent, size: [+(L * 0.8).toFixed(2), +(Tk * 0.7).toFixed(2), +(D * 0.9).toFixed(2)],
        pitch: 0.2, embed: 0.72, lift: +(-Tk * 0.85).toFixed(2), jitter: [0, +((R() - 0.5) * 6).toFixed(1), +((R() - 0.5) * 4).toFixed(1)], tint: STRATA[Math.floor(R() * STRATA.length)] });
    }
  }
  for (let i = 0; i < pieces.length; i += 200) {
    const r = (await call("place_aligned", { base: ROCK.base, id: row.example.id, allowOtherTheme: true, units: "map", tag: "ldg", pieces: pieces.slice(i, i + 200) })).json;
    stats.pieces += r.placed;
  }
  stats.slabs = Object.values(stats.byHeight).reduce((a, b) => a + b, 0);
  ctx.say?.("ledge", JSON.stringify({ faces: stats.faces, bands: stats.bands, planned: stats.planned, slabs: stats.slabs, pieces: stats.pieces }));
  return stats;
}
