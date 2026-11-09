// Where decoration may not stand on Caldera (the detail layer, detail.mjs): props have no collision, so a worm would walk
// through one, and they must never hide a worm, a trail, a castle court, a spawn, a launcher pad or the mine factory.
// keepOut(x, z) answers the first reason a point is kept clear, or null where a prop may go. Map units throughout; the
// geometry is terrain.mjs's own (the trails' centrelines and branches, the castles' footprints, the launchers, the plug).
import * as T from "./terrain.mjs";

// how far from each thing a prop's footprint must keep (map units, to the prop's centre: add the prop's own radius)
export const CLEAR = {
  trail: T.TRAIL.bed + T.TRAIL.shoulder + 3.5,   // the bed (2.6 half), the level shoulder (2) and the kerb ridge past it
  castle: 3,                                     // round a castle's footprint (walls, towers, gates and the court)
  spawn: 4,                                      // round a spawn knot (team castles: the courts already cover them)
  pad: 6,                                        // round a launcher's pad centre (its 6 x 6 pad and its lip)
  plug: 9,                                       // round the mine factory's plug
  apron: T.APRON_R,                              // the dry ash round the plug the mines land on
};

const lines = () => T.TRAILS.flatMap((t) => [t.pts, ...(t.branches ?? []).map((b) => b.pts).filter(Boolean)]);
let LINES = null;

function segDist(px, pz, a, b) {
  const dx = b[0] - a[0], dz = b[1] - a[1], L = dx * dx + dz * dz;
  const t = L ? Math.max(0, Math.min(1, ((px - a[0]) * dx + (pz - a[1]) * dz) / L)) : 0;
  return Math.hypot(px - a[0] - t * dx, pz - a[1] - t * dz);
}

/** The distance from (x, z) to the nearest trail centreline (a trail's legs and its castle branches). */
export function trailDist(x, z) {
  LINES ??= lines();
  let d = Infinity;
  for (const pl of LINES) for (let i = 1; i < pl.length; i++) {
    const a = pl[i - 1], b = pl[i];
    if (Math.abs(x - a[0]) > d + 20 && Math.abs(x - b[0]) > d + 20) continue;
    d = Math.min(d, segDist(x, z, a, b));
  }
  return d;
}

const PLUG = T.pointAt(T.PLUG_RHO, T.PLUG_TH);
const PADS = T.LAUNCHERS.map(([rho, th]) => T.pointAt(rho, th));

/** The first reason (x, z) is kept clear of props of radius r, or null. spawns: [[x, z], ...] (the level's spawn knots). */
export function keepOut(x, z, r = 0, spawns = []) {
  if (trailDist(x, z) < CLEAR.trail + r) return "trail";
  for (const c of T.CASTLES) if (T.footDist(c, x, z, CLEAR.castle + r) === 0) return `castle ${c.name}`;
  for (const s of spawns) if (Math.hypot(x - s[0], z - s[1]) < CLEAR.spawn + r) return "spawn";
  for (const p of PADS) if (Math.hypot(x - p[0], z - p[1]) < CLEAR.pad + r) return "launcher pad";
  if (Math.hypot(x - PLUG[0], z - PLUG[1]) < Math.max(CLEAR.plug, CLEAR.apron) + r) return "mine factory";
  return null;
}
