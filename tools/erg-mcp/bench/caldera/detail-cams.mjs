// The detail layer's fixed camera set (map units), the same every round so the rounds compare: the volcano oblique,
// the crater close-up, a flank close-up (the cliff band and the scree between two trails), each castle (the poses
// build.mjs writes to out/castles.json, which clear the cone; computed from the sea when that file is missing), a trail
// leg (the NE trail's middle hairpin) and the shore (a low eye over the sea at the SE beach). Used by
// detail-capture.mjs for render_view (offline) and game_shots (in a Test).
import fs from "node:fs";
import path from "node:path";
import * as T from "./terrain.mjs";

const r1 = (v) => Math.round(v * 10) / 10;
const ground = (x, z) => T.island(x, z);

export function cameras(buildOut) {
  const cams = [];
  cams.push({ name: "oblique", pos: [135, 125, 175], target: [0, 30, 0], fov: 55 });
  cams.push({ name: "crater", pos: [8, 108, 52], target: [0, 50, -4], fov: 55 });
  // flank: the west flank between the castles Ember and Harbour (no castle or trail in front): its cliff band and scree
  {
    const th = 2.3, [tx, tz] = T.pointAt(55, th), [ex, ez] = T.pointAt(108, th);
    cams.push({ name: "flank", pos: [r1(ex), r1(ground(tx, tz) + 32), r1(ez)], target: [r1(tx), r1(ground(tx, tz)), r1(tz)], fov: 50 });
  }
  let castles = null;
  const f = buildOut && path.join(buildOut, "castles.json");
  if (f && fs.existsSync(f)) castles = JSON.parse(fs.readFileSync(f, "utf8")).filter((k) => k.pos);
  if (castles?.length) for (const k of castles) cams.push({ name: `castle_${k.castle}`, pos: k.pos, target: k.target, fov: k.fov ?? 55 });
  else for (const c of T.CASTLES) {
    const r = Math.hypot(c.at[0], c.at[1]) || 1, ux = c.at[0] / r, uz = c.at[1] / r;
    cams.push({ name: `castle_${c.name}`, pos: [r1(c.at[0] + ux * 30), r1(c.court + 30), r1(c.at[1] + uz * 30)], target: [r1(c.at[0]), r1(c.court + 2), r1(c.at[1])], fov: 55 });
  }
  {
    const t = T.TRAILS.find((x) => x.name === "NE") ?? T.TRAILS[0], h = t.hairpins[1], th = t.col.thc, c = [Math.cos(th), Math.sin(th)];
    cams.push({ name: `trail_${t.name}`, pos: [r1(h[0] + c[0] * 45), r1(h[2] + 30), r1(h[1] + c[1] * 45)], target: [r1(h[0] - c[0] * 6), r1(h[2] + 4), r1(h[1] - c[1] * 6)], fov: 55 });
  }
  {
    const th = 0.95, [tx, tz] = T.pointAt(84, th), [ex, ez] = T.pointAt(128, th + 0.12);
    cams.push({ name: "shore", pos: [r1(ex), 9, r1(ez)], target: [r1(tx), r1(Math.max(2, ground(tx, tz)) + 3), r1(tz)], fov: 60 });
  }
  return cams;
}
