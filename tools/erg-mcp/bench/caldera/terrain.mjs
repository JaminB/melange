// Caldera's terrain, as pure functions of the map position (map units, y up, the sea at about -1, the island centred on 0, 0):
// a round volcanic cone whose flank climbs in treads between three bands of cliffs (so the roads that cut through the cliffs
// are the way up), a crater with a breach facing south and a dais for the mine factory, a coast of beaches, headlands and
// coves that slopes under the sea, a few lava tongues that follow gullies and widen downslope, three roads that run through
// seven castle shelves (three on the shore, two on the lower treads, two high up), and the expressions (lava tongues, roads)
// that paint them. build.mjs turns it into a height grid for heightfield_drape.

// ---- numbers ----
export const RF = 92;            // the coast, in the profile's radius (rho): the beach is at 0, the sea floor falls away outside
export const RC = 36;            // radius of the rim crest (a wide bowl: the crater floor and the plug the mine factory stands on lie inside it)
export const FLOOR_Y = 58;       // the crater floor (the rim is 10-20 over it)
export const FLAT_R = 26;        // the floor is level out to this radius, the walls climb from there to the rim
export const PLUG_RHO = 19, PLUG_TH = -1.15;   // the basalt plug of the mine factory: on the floor, off the centre, on the side the south camera sees over the breach
export const BREACH = Math.PI / 2;   // +z, the south: the default camera looks from there, so the notch and the flow show
export const SP = 0.62;          // the roads' climb per unit of length (32 degrees)
export const BOX = 118;          // half side of the heightfield rectangle (the skin is clipped to a circle inside it)
export const CLIP = 117;         // radius of that circle
export const SEA_FLOOR = -9;     // the skin's lowest level (the offshore slope runs down to it)
export const TREAD = 0.52;       // slope of the flank between the cliffs (27 degrees)
// the cliff bands: the flank steps up by `a` over about 5 units at rho (wobbled by angle); roads climb through them
export const CLIFFS = [{ rho: 47, a: 13 }, { rho: 78, a: 6 }];
export const BOOST = 20;         // extra height toward the summit: the cone steepens over its top third, so it is the tallest thing from every side

// the flank without cliffs: a soft foot (a beach) that turns into the tread's slope; offshore the sea floor falls more gently
// (a wide soft foot: the beach is 10 map units of sand before the tread climbs)
const F = (d) => (d >= 0 ? TREAD * (Math.sqrt(d * d + 64) - 8) : -0.3 * (Math.sqrt(d * d + 64) - 8));

// ---- noise ----
function hash(i, j, seed) {
  let h = (Math.imul(i, 374761393) + Math.imul(j, 668265263) + Math.imul(seed, 2147483647)) | 0;
  h = Math.imul(h ^ (h >>> 13), 1274126177);
  return ((h ^ (h >>> 16)) >>> 0) / 4294967295 * 2 - 1;
}
export function noise(x, z, seed) {
  const i = Math.floor(x), j = Math.floor(z), fx = x - i, fz = z - j;
  const u = fx * fx * (3 - 2 * fx), v = fz * fz * (3 - 2 * fz);
  const a = hash(i, j, seed), b = hash(i + 1, j, seed), c = hash(i, j + 1, seed), d = hash(i + 1, j + 1, seed);
  return (a + (b - a) * u) + ((c + (d - c) * u) - (a + (b - a) * u)) * v;
}
export function fbm(x, z, seed, oct) {
  let sum = 0, amp = 1, norm = 0;
  for (let o = 0; o < oct; o++) { sum += amp * noise(x, z, seed + o * 31); norm += amp; x *= 2; z *= 2; amp *= 0.5; }
  return sum / norm;
}
const smooth = (e0, e1, v) => { const t = Math.min(1, Math.max(0, (v - e0) / (e1 - e0))); return t * t * (3 - 2 * t); };
const angd = (a, b) => Math.atan2(Math.sin(a - b), Math.cos(a - b));
const gaussA = (th, c, s) => Math.exp(-((angd(th, c) / s) ** 2));

// ---- the outline: rho is the radius the profile sees, the true radius times a factor that depends on the angle only ----
// (a headland pushes the coast out, a cove pulls it in; noise breaks up the circle), so a road marched in rho and angle has
// exactly the profile's height under it. Two to three times the iteration-4 irregularity.
const HEADS = [[-0.62, 0.22, 0.07], [2.75, 0.2, 0.065], [1.25, 0.16, 0.05], [-2.0, 0.18, 0.055], [0.38, 0.12, 0.07], [3.55, 0.13, 0.06], [5.1, 0.12, 0.055]];   // angle, width (rad), push (fraction)
// the last one is the harbour inlet: narrow and deep, between two headlands (angle 1.62)
const COVES = [[0.10, 0.26, 0.11], [2.05, 0.2, 0.09], [4.35, 0.22, 0.1], [-1.35, 0.15, 0.08], [0.95, 0.12, 0.06], [1.62, 0.065, 0.19], [3.15, 0.09, 0.08]];
export function scaleAt(th) {
  let s = 1 - 0.07 * fbm(Math.cos(th) * 3.1 + 9, Math.sin(th) * 3.1 + 9, 5, 4);
  for (const [a, w, k] of HEADS) s -= k * gaussA(th, a, w);
  for (const [a, w, k] of COVES) s += k * gaussA(th, a, w);
  return s;
}
export const rhoAt = (x, z) => Math.hypot(x, z) * scaleAt(Math.atan2(z, x));
// the point at rho and angle th
export const pointAt = (rho, th) => { const r = rho / scaleAt(th); return [r * Math.cos(th), r * Math.sin(th)]; };

// the smooth profile: tread flank plus the cliff bands (their radius and height wander with the angle, so no ring is regular)
// the outer band is a foothill shoulder (a wider step), the inner one a cliff
const k0 = (k) => (k === 1 ? 1 : 0);
export function profile(rho, th) {
  let h = F(RF - rho) + BOOST * (1 - smooth(RC - 2, RC + 46, rho));
  CLIFFS.forEach((c, k) => {
    const rk = c.rho + 4.5 * noise(Math.cos(th) * 1.6 + k * 7, Math.sin(th) * 1.6 + k * 3, 40 + k);
    const a = c.a * (0.85 + 0.15 * noise(Math.cos(th) * 2.3 + k, Math.sin(th) * 2.3, 60 + k));
    h += a * (1 - smooth(rk - 1.75 - 1.5 * k0(k), rk + 1.75 + 1.5 * k0(k), rho));
  });
  return h;
}

// ---- outlying islets: rock stacks off the coast (angle, radius from the centre in map units, radius of the stack, crest above the sea) ----
export const ISLETS = [{ th: 0.5, rho: 104, w: 6.5, top: 7 }, { th: 2.4, rho: 103, w: 6, top: 6.5 }, { th: 3.95, rho: 105, w: 6.5, top: 7 }, { th: 5.5, rho: 102, w: 5.5, top: 6 },
  { th: 1.55, rho: 98, w: 3.2, top: 9 }, { th: 3.2, rho: 100, w: 3.6, top: 8 }, { th: 4.9, rho: 99, w: 3, top: 10 }, { th: 0.1, rho: 106, w: 3.4, top: 8 }, { th: 6.0, rho: 101, w: 3.2, top: 7 }];
const ISLET_XZ = ISLETS.map((q) => [q.rho * Math.cos(q.th), q.rho * Math.sin(q.th)]);

// ---- the lava tongues: centre angle as a function of the radius (shared by the gullies and the paint) ----
// a: centre angle, r0 / r1: radius range, w0 / w1: half width (map units) at both ends (narrow at the crater, wide at the foot),
// wob / wl / ph: a wander, rb / dr: a branch that leaves the line at radius rb and bends dr radians per unit of radius.
export const TONGUES = [
  { name: "breach", a: BREACH, r0: RC - 8, r1: 90, w0: 4.4, w1: 8.5, wob: 0.05, wl: 11, ph: 0.3, rb: 0, dr: 0, core: true },
  { name: "breach-w", a: BREACH, r0: 48, r1: 88, w0: 1.8, w1: 3.8, wob: 0.03, wl: 9, ph: 4.0, rb: 48, dr: 0.0075 },
  { name: "ene", a: 0.72, r0: RC - 2, r1: 88, w0: 2.4, w1: 5.2, wob: 0.06, wl: 10, ph: 2.0, rb: 0, dr: 0, core: true },
  { name: "nw", a: -2.45, r0: RC - 2, r1: 86, w0: 2.2, w1: 4.6, wob: 0.06, wl: 13, ph: 0.9, rb: 0, dr: 0, core: true },
  { name: "sw", a: 2.62, r0: RC - 2, r1: 82, w0: 2.0, w1: 4.2, wob: 0.07, wl: 9, ph: 5.1, rb: 0, dr: 0 },
];
// dry gullies: deep radial cuts in the flank without lava (angle, half width in radians, depth, radius range), so the cone has ribs
export const GULLIES = [{ a: -1.62, w: 0.075, d: 8, r0: 38, r1: 84 }, { a: 3.45, w: 0.07, d: 7.5, r0: 38, r1: 82 }, { a: 5.88, w: 0.065, d: 7, r0: 40, r1: 80 }, { a: 0.1, w: 0.06, d: 6, r0: 40, r1: 74 },
  { a: 1.0, w: 0.055, d: 6.5, r0: 40, r1: 78 }, { a: 4.7, w: 0.06, d: 7, r0: 40, r1: 80 }, { a: 2.25, w: 0.05, d: 6, r0: 42, r1: 76 }];
const tongueAngle = (t, r) => t.a + t.wob * Math.sin(r / t.wl + t.ph) + t.dr * Math.max(0, r - t.rb);
const tongueWidth = (t, r) => t.w0 + (t.w1 - t.w0) * Math.min(1, Math.max(0, (r - t.r0) / (t.r1 - t.r0)));
// the tongues as one mask expression for the heightfield materials (above 0 inside any of them; the rule list holds 64, so all
// the tongues share one rule): `scale` narrows or widens them (a hot core, a margin of hot rock)
export function tonguesMask(list, scale = 1) {
  const f = (v) => Number(v.toFixed(5));
  const call = (t) => `tg(${f(t.a)}, ${f(t.wob)}, ${t.wl}, ${t.ph}, ${f(t.dr)}, ${t.rb}, ${t.r0}, ${t.r1}, ${f(t.w0 * scale)}, ${f(t.w1 * scale)})`;
  const defs = "fn wrap(d) = atan2(sin(d), cos(d)); " +
    "fn tg(a, wob, wl, ph, dr, rb, r0, r1, w0, w1) = min(lerp(w0, w1, clamp((r - r0) / (r1 - r0), 0, 1)) - r * abs(wrap(theta - (a + wob * sin(r / wl + ph) + dr * max(0, r - rb)))), min(r - r0, r1 - r)); ";
  return defs + list.map(call).reduce((acc, c) => (acc ? `max(${acc}, ${c})` : c), "");
}

// ---- the roads and the castles on them ----
// A road climbs the flank at SP a unit: its heading leans from the contour towards the top by the angle that gives SP on the
// profile's own gradient (across a cliff band it slants through it, cut into the cliff). When the climb reaches a castle's
// height the road runs straight and level through the castle's footprint (along the contour; the castle's shelf is cut and
// filled to that level), then goes on climbing.
function surf0(x, z) { return profile(Math.max(rhoAt(x, z), RC), Math.atan2(z, x)); }
function grad(x, z) {
  const e = 0.6;
  return [(surf0(x + e, z) - surf0(x - e, z)) / (2 * e), (surf0(x, z + e) - surf0(x, z - e)) / (2 * e)];
}
// the castles' plans: size [along the road, across it], keep, towers, stone
// kind: castle (build_castle), ring (a round curtain wall round a round keep, on a crag), lfort (a castle with a wing and a sea gate)
const PLANS = {
  Strand: { kind: "castle", size: [26, 20], wallHeight: 6, towerShape: "round", towerRadius: 3.2, towerHeight: 11, keep: [7, 6], keepHeight: 10, mat: 16 },
  Ashgate: { kind: "castle", size: [24, 18], wallHeight: 5, towerShape: "square", towerRadius: 2.6, towerHeight: 10, keep: [7, 6], keepHeight: 11, mat: 27 },
  Cinder: { kind: "castle", size: [22, 20], wallHeight: 5, towerShape: "round", towerRadius: 2.6, towerHeight: 10, keep: [10, 9], keepHeight: 16, mat: 12 },
  Harbour: { kind: "lfort", size: [26, 18], wing: [14, 14], wallHeight: 6, towerShape: "round", towerRadius: 3, towerHeight: 11, keep: [6, 5], keepHeight: 10, mat: 12 },
  Ember: { kind: "castle", size: [38, 12], wallHeight: 7, towerShape: "square", towerRadius: 2.8, towerHeight: 12, keep: null, keepHeight: 0, mat: 16 },
  Seawall: { kind: "castle", size: [30, 22], wallHeight: 6, towerShape: "square", towerRadius: 3, towerHeight: 11, keep: [9, 7], keepHeight: 12, mat: 27 },
  Highwatch: { kind: "ring", size: [25, 25], ringRadius: 11, wallHeight: 6, keepRadius: 4.5, keepHeight: 14, towerRadius: 2.6, towerHeight: 10, mat: 16 },
};
const GATE_SLACK = 3;            // the straight stretch overruns the castle by this on each end
const TARGET = 0.6;              // the road's climb per unit of length on the cone
const LEAN = 0.72;               // the least the road leans from the uphill direction (radians): it always winds round the cone
// the three roads, each a march from a shore castle: [name, floor] of the castles in the order the road meets them. The road
// starts level through the first one (along the contour, counter-clockwise from above), then climbs: each step it heads
// at the angle from the uphill direction that gives TARGET on the profile's own gradient (across a cliff band it runs nearly along
// the contour: a ledge cut into the cliff), its heading turning at most 0.08 radians a unit. When its height reaches the next
// castle's floor it runs straight and level through that castle's footprint (along x or z, whichever its heading is nearer), the
// castle's shelf being cut and filled to that floor, and goes on. The castles thus stand where the road arrives, not on a ring.
const LAYOUT = [
  { name: "R1", th0: -0.65, rho0: 82, stops: [["Strand", 4], ["Ashgate", 17], ["Cinder", 30]] },
  { name: "R2", th0: 2.0, rho0: 79, stops: [["Harbour", 3], ["Ember", 23]] },
  { name: "R3", th0: 4.1, rho0: 80, stops: [["Seawall", 4], ["Highwatch", 26]] },
];

// a castle's footprint, [x0, z0, x1, z1] in map units, grown by pad: the courtyard's box (along the road's axis by across it), and an L fort's wing on the sea side
export function footRect(c, pad = 0) {
  const hx = (c.axis === "x" ? c.along : c.across) / 2 + pad, hz = (c.axis === "x" ? c.across : c.along) / 2 + pad;
  let r = [c.at[0] - hx, c.at[1] - hz, c.at[0] + hx, c.at[1] + hz];
  const w = c.plan.wing;
  if (w) {
    const [wa, wc] = w, ex = c.axis === "x" ? [wa, wc] : [wc, wa], d = c.wdir;
    const cx = c.at[0] + d[0] * (c.across / 2 + wc / 2), cz = c.at[1] + d[1] * (c.across / 2 + wc / 2);
    r = [Math.min(r[0], cx - ex[0] / 2 - pad), Math.min(r[1], cz - ex[1] / 2 - pad), Math.max(r[2], cx + ex[0] / 2 + pad), Math.max(r[3], cz + ex[1] / 2 + pad)];
  }
  return r;
}
function layRoad(spec) {
  let [x, z] = pointAt(spec.rho0, spec.th0);
  let psi = spec.th0 + Math.PI / 2, y = spec.stops[0][1];
  const pts = [], castles = [];
  const place = (k) => {
    const [name, floor] = spec.stops[k], pl = PLANS[name];
    // the road runs through the castle along the contour (the tangent T, counter-clockwise), centred on the castle, in through
    // a gate in one wall and out through the opposite one; the gates are where the line meets the walls (kept off the corner towers)
    // (a chord of the circle through the arrival point P: it ends as far from the centre as it began, so the road does not run downhill)
    const rho0 = Math.hypot(x, z) || 1, hh0 = Math.min(pl.size[0] / 2 + GATE_SLACK, rho0 * 0.9), dth = 2 * Math.asin(hh0 / rho0), th1 = Math.atan2(z, x) + dth;
    const P = [x, z], Q = pointAt(rhoAt(x, z), th1), cl = Math.hypot(Q[0] - P[0], Q[1] - P[1]);
    const T = [(Q[0] - P[0]) / cl, (Q[1] - P[1]) / cl];
    const ax = Math.abs(T[0]) >= Math.abs(T[1]) ? "x" : "z", i = ax === "x" ? 0 : 1, j = 1 - i;
    const sg = T[i] >= 0 ? 1 : -1;
    const len = cl, h = len / 2;
    const c0 = [(P[0] + Q[0]) / 2, (P[1] + Q[1]) / 2];
    // the gates: where the ray from the centre along +-T meets the walls, its place along the wall kept off the corner towers; the
    // road goes A, out of the gate's outside, G1, across the court, G2, B (a diagonal contour makes it enter by one wall and leave by an adjacent one)
    const hx = ax === "x" ? pl.size[0] / 2 : pl.size[1] / 2, hz = ax === "x" ? pl.size[1] / 2 : pl.size[0] / 2, tr = pl.towerRadius ?? 3;
    const gate = (d, forbid) => {
      if (pl.kind === "ring") return { side: null, offset: 0, G: [c0[0] + d[0] * pl.ringRadius, c0[1] + d[1] * pl.ringRadius], O: [c0[0] + d[0] * (pl.ringRadius + 4), c0[1] + d[1] * (pl.ringRadius + 4)] };
      const tx = d[0] > 0 ? hx / d[0] : d[0] < 0 ? -hx / d[0] : 1e9, tz = d[1] > 0 ? hz / d[1] : d[1] < 0 ? -hz / d[1] : 1e9;
      let sd = tx < tz ? (d[0] > 0 ? "e" : "w") : (d[1] > 0 ? "s" : "n"), o = tx < tz ? d[1] * tx : d[0] * tz;
      if (sd === forbid) { sd = d[0] * (ax === "x" ? 1 : 0) + d[1] * (ax === "z" ? 1 : 0) > 0 ? (ax === "x" ? "e" : "s") : (ax === "x" ? "w" : "n"); o = 0; }
      const ew = sd === "e" || sd === "w", lim = Math.max(0, (ew ? hz : hx) - tr - 3.5);
      o = Math.max(-lim, Math.min(lim, o));
      const G = ew ? [c0[0] + (sd === "e" ? hx : -hx), c0[1] + o] : [c0[0] + o, c0[1] + (sd === "s" ? hz : -hz)];
      const nrm = { e: [1, 0], w: [-1, 0], s: [0, 1], n: [0, -1] }[sd];
      return { side: sd, offset: o, G, O: [G[0] + nrm[0] * 4, G[1] + nrm[1] * 4] };
    };
    const wside = pl.wing ? (ax === "x" ? (c0[1] >= 0 ? "s" : "n") : (c0[0] >= 0 ? "e" : "w")) : null;
    const gin = gate([-T[0], -T[1]], wside), gout = gate(T, wside);
    const run = [];
    for (const [p, q] of [[P, gin.O], [gin.O, gin.G], [gin.G, gout.G], [gout.G, gout.O], [gout.O, Q]]) {
      const n = Math.max(1, Math.round(Math.hypot(q[0] - p[0], q[1] - p[1])));
      for (let u = run.length ? 1 : 0; u <= n; u++) run.push([p[0] + (q[0] - p[0]) * u / n, p[1] + (q[1] - p[1]) * u / n]);
    }
    const wdir = ax === "x" ? [0, c0[1] >= 0 ? 1 : -1] : [c0[0] >= 0 ? 1 : -1, 0];
    castles.push({ name, road: spec.name, at: c0, floor, axis: ax, plan: pl, along: pl.size[0], across: pl.size[1], wdir, T, rho: rhoAt(c0[0], c0[1]), gin, gout,
      arrive: gin.side, depart: gout.side, run });
    for (const p of run) pts.push([p[0], p[1], floor]);
    [x, z] = Q; psi = Math.atan2(T[1], T[0]) + dth / 2; y = floor;
  };
  place(0);
  let next = 1;
  for (let step = 0; step < 2500; step++) {
    const [gx, gz] = grad(x, z), gm = Math.hypot(gx, gz) || 1e-6, ux = gx / gm, uz = gz / gm;
    const al = Math.max(LEAN, Math.acos(Math.min(1, TARGET / gm)));
    const rho = Math.hypot(x, z) || 1, tx = -z / rho, tz = x / rho;
    const cand = [al, -al].map((a) => Math.atan2(ux * Math.sin(a) + uz * Math.cos(a), ux * Math.cos(a) - uz * Math.sin(a)));
    const want = cand.reduce((b, a) => (Math.cos(a) * tx + Math.sin(a) * tz > Math.cos(b) * tx + Math.sin(b) * tz ? a : b));
    psi += Math.max(-0.08, Math.min(0.08, angd(want, psi)));
    x += Math.cos(psi); z += Math.sin(psi);
    y += Math.max(-0.05, Math.min(TARGET * 0.97, surf0(x, z) - y));
    pts.push([x, z, y]);
    if (next < spec.stops.length && y >= spec.stops[next][1] - 0.3) { place(next); next++; continue; }
    if (next >= spec.stops.length && rhoAt(x, z) <= RC + 3) break;
  }
  return { ...spec, pts, castles, end: pts[pts.length - 1] };
}
export const ROADS = LAYOUT.map(layRoad);
export const CASTLES = ROADS.flatMap((r) => r.castles);

// a road as a mask expression (above 0 within `hw` of its line; the distance field is exact, so the edge is as smooth as the
// material grid allows): the polyline is cut to a segment every `every` points
export function roadMask(rd, hw = 2.7, every = 7) {
  const f = (v) => Number(v.toFixed(1));
  const pts = rd.pts.filter((_, i, a) => i % every === 0 || i === a.length - 1);
  const segs = [];
  for (let i = 0; i + 1 < pts.length; i++) segs.push(`sd(${f(pts[i][0])}, ${f(pts[i][1])}, ${f(pts[i + 1][0])}, ${f(pts[i + 1][1])})`);
  const defs = "fn tt(ax, az, bx, bz) = clamp(((x - ax) * (bx - ax) + (z - az) * (bz - az)) / max(0.0001, (bx - ax) ^ 2 + (bz - az) ^ 2), 0, 1); " +
    "fn sd(ax, az, bx, bz) = hypot(x - (ax + (bx - ax) * tt(ax, az, bx, bz)), z - (az + (bz - az) * tt(ax, az, bx, bz))); ";
  return `${defs}${hw} - ${segs.reduce((acc, c) => (acc ? `min(${acc}, ${c})` : c), "")}`;
}

// ---- the height of the island at (x, z), map units above the sea's zero (negative: the sea floor, down to SEA_FLOOR) ----
export function island(x, z) {
  const th = Math.atan2(z, x), r = Math.hypot(x, z), rho = r * scaleAt(th);
  // the cone: ridges grow with the radius from the rim; gullies are cut under the lava tongues
  const ridges = 1 + 0.085 * Math.cos(7 * th + 1.5 * noise(x / 40, z / 40, 9)) * smooth(RC + 2, RC + 22, rho) * smooth(8, 24, profile(rho, th));
  let dip = 0;
  for (const t of TONGUES) {
    if (r < t.r0 || r > t.r1) continue;
    const w = tongueWidth(t, r) * 1.5 + 1.4;
    const q = (r * angd(th, tongueAngle(t, r))) / w;
    dip = Math.max(dip, (t.name === "breach" ? 4.5 : 3.2) * Math.exp(-q * q) * smooth(t.r0, t.r0 + 6, r) * (1 - smooth(t.r1 - 10, t.r1, r)));
  }
  for (const g of GULLIES) {
    if (rho < g.r0 || rho > g.r1) continue;
    const q = angd(th, g.a + 0.04 * Math.sin(rho / 9 + g.a * 5)) / (g.w * (0.7 + 0.5 * smooth(g.r0, g.r1, rho)));
    dip = Math.max(dip, g.d * Math.exp(-q * q) * smooth(g.r0, g.r0 + 8, rho) * (1 - smooth(g.r1 - 8, g.r1, rho)));
  }
  dip *= smooth(RC + 1, RC + 6, rho);
  // the rim: spurs of varied height (never a regular row of teeth), lower where a road reaches it
  let roadMaskH = 0;
  for (const rd of ROADS) roadMaskH = Math.max(roadMaskH, Math.exp(-((angd(th, Math.atan2(rd.end[1], rd.end[0])) / 0.2) ** 2)));
  const spur = 3 + Math.max(0, noise(Math.cos(th) * 3.3 + 4, Math.sin(th) * 3.3 + 4, 17) + 0.2) * 7 * (1 - roadMaskH);
  const crest = spur * Math.exp(-(((rho - RC) / 3.6) ** 2));
  const outer = profile(Math.max(rho, RC), th) * ridges - dip + crest;
  const crestH = profile(RC, th) + 3;
  // the crater: a wide flat floor (the mine factory's plug stands on it), walls that climb to the rim, a rough floor
  const bowl = FLOOR_Y + (crestH - FLOOR_Y) * smooth(FLAT_R, RC - 1, rho) + 0.7 * fbm(x / 6, z / 6, 51, 2) * (1 - smooth(FLAT_R - 4, FLAT_R + 4, rho));
  let c = bowl + (outer - bowl) * smooth(RC - 4, RC + 1.5, rho);
  // the breach: the rim falls away in a notch facing south, down to the bowl's floor, so the bowl, its mines and the lava
  // flow that runs out of it are seen from the default camera
  const bw = Math.exp(-((angd(th, BREACH) / 0.27) ** 2));
  const ramp = FLOOR_Y + 3 + Math.max(0, rho - (RC - 12)) * 0.8;
  c -= bw * smooth(RC + 22, RC + 6, rho) * smooth(FLAT_R - 8, FLAT_R, rho) * Math.max(0, c - ramp);
  let h = c;
  // rocks and bars in the shallows and on the beach
  h += 2.0 * fbm(x / 9, z / 9, 33, 2) * Math.exp(-((h / 5) ** 2));
  // islets: a rough stack with a crest, rising out of the sea floor
  ISLETS.forEach((q, i) => {
    const d = Math.hypot(x - ISLET_XZ[i][0], z - ISLET_XZ[i][1]) / (q.w * (1 + 0.25 * noise(x / 5, z / 5, 70 + i)));
    h = Math.max(h, -3 + (q.top + 3) * Math.exp(-d * d));
  });
  // shelves under the castles (cut and fill to the castle's floor), roads cut and filled into the flank
  let wShelf = 0, wRoad = 0;
  for (const k of CASTLES) {
    const [x0, z0, x1, z1] = footRect(k, 3);
    const d = Math.hypot(Math.max(0, x0 - x, x - x1), Math.max(0, z0 - z, z - z1));
    if (d > 6) continue;
    const w = smooth(6, 0, d);
    h += (k.floor - h) * w;
    wShelf = Math.max(wShelf, w);
  }
  for (const rd of ROADS) {
    let best = 1e9, by = 0;
    for (const p of rd.pts) {
      const dx = x - p[0], dz = z - p[1];
      if (Math.abs(dx) > 7 || Math.abs(dz) > 7) continue;
      const dd = Math.hypot(dx, dz);
      if (dd < best) { best = dd; by = p[2]; }
    }
    if (best < 7) { const w = 1 - smooth(2.6, 6.8, best); h += (by - h) * w; wRoad = Math.max(wRoad, w); }
  }
  // fine roughness where nothing was cut
  h += 1.0 * fbm(x / 8, z / 8, 21, 2) * smooth(3, 14, h) * (1 - wShelf) * (1 - wRoad);
  return Math.max(SEA_FLOOR, h);
}
// the heights above SEA_FLOOR, one row per z, one column per x (the drape call clips them to the circle CLIP)
export function gridOf(min = -BOX, max = BOX, step = 1) {
  const rows = [];
  for (let z = min; z <= max + 1e-6; z += step) {
    const row = [];
    for (let x = min; x <= max + 1e-6; x += step) row.push(Math.round((island(x, z) - SEA_FLOOR) * 100) / 100);
    rows.push(row);
  }
  return rows;
}
// no step between neighbouring columns steeper than `limit` map units a unit (about 71 degrees at 2.9): a few passes of thermal
// relaxation over a grid made by gridOf, so the drape skin never folds (its blocks are only 3 voxels thick)
export function relax(rows, limit = 2.9, passes = 60) {
  const n = rows.length, m = rows[0].length;
  for (let p = 0; p < passes; p++) {
    let moved = 0;
    for (let j = 0; j < n; j++) for (let i = 0; i < m; i++) {
      for (const [dj, di] of [[0, 1], [1, 0]]) {
        const jj = j + dj, ii = i + di;
        if (jj >= n || ii >= m) continue;
        const d = rows[j][i] - rows[jj][ii];
        if (Math.abs(d) > limit) { const k = (Math.abs(d) - limit) / 2 * Math.sign(d); rows[j][i] -= k; rows[jj][ii] += k; moved++; }
      }
    }
    if (!moved) break;
  }
  return rows;
}
