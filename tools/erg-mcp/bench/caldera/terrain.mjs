// Caldera's terrain, as pure functions of the map position (map units, y up, the sea at about -1, the island centred on 0, 0):
// a round volcanic cone whose flank climbs in treads between three bands of cliffs (so the roads that cut through the cliffs
// are the way up), a crater with a breach facing south and a dais for the mine factory, a coast of beaches, headlands and
// coves that slopes under the sea, a few lava tongues that follow gullies and widen downslope, three roads that run through
// seven castle shelves (three on the shore, two on the lower treads, two high up), and the expressions (lava tongues, roads)
// that paint them. build.mjs turns it into a height grid for heightfield_drape.

// ---- numbers ----
export const RF = 92;            // the coast, in the profile's radius (rho): the beach is at 0, the sea floor falls away outside
export const RC = 27;            // radius of the rim crest
export const FLOOR_Y = 46;       // the crater floor
export const BREACH = Math.PI / 2;   // +z, the south: the default camera looks from there, so the notch and the flow show
export const SP = 0.62;          // the roads' climb per unit of length (32 degrees)
export const BOX = 118;          // half side of the heightfield rectangle (the skin is clipped to a circle inside it)
export const CLIP = 117;         // radius of that circle
export const SEA_FLOOR = -9;     // the skin's lowest level (the offshore slope runs down to it)
export const TREAD = 0.52;       // slope of the flank between the cliffs (27 degrees)
// the cliff bands: the flank steps up by `a` over about 5 units at rho (wobbled by angle); roads climb through them
export const CLIFFS = [{ rho: 36, a: 10 }, { rho: 56, a: 10 }, { rho: 76, a: 10 }];

// the flank without cliffs: a soft foot (a beach) that turns into the tread's slope; offshore the sea floor falls more gently
const F = (d) => (d >= 0 ? TREAD * (Math.sqrt(d * d + 16) - 4) : -0.3 * (Math.sqrt(d * d + 16) - 4));

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
const HEADS = [[-0.62, 0.22, 0.07], [2.75, 0.2, 0.065], [1.25, 0.16, 0.05], [-2.0, 0.18, 0.055]];   // angle, width (rad), push (fraction)
const COVES = [[0.10, 0.26, 0.11], [2.05, 0.2, 0.09], [4.35, 0.22, 0.1], [-1.35, 0.15, 0.08], [0.95, 0.12, 0.06]];
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
export function profile(rho, th) {
  let h = F(RF - rho);
  CLIFFS.forEach((c, k) => {
    const rk = c.rho + 3.2 * noise(Math.cos(th) * 1.6 + k * 7, Math.sin(th) * 1.6 + k * 3, 40 + k);
    const a = c.a * (0.85 + 0.15 * noise(Math.cos(th) * 2.3 + k, Math.sin(th) * 2.3, 60 + k));
    h += a * (1 - smooth(rk - 1.75, rk + 1.75, rho));
  });
  return h;
}

// ---- the lava tongues: centre angle as a function of the radius (shared by the gullies and the paint) ----
// a: centre angle, r0 / r1: radius range, w0 / w1: half width (map units) at both ends (narrow at the crater, wide at the foot),
// wob / wl / ph: a wander, rb / dr: a branch that leaves the line at radius rb and bends dr radians per unit of radius.
export const TONGUES = [
  { name: "breach", a: BREACH, r0: 6, r1: 104, w0: 3.2, w1: 6.0, wob: 0.05, wl: 11, ph: 0.3, rb: 0, dr: 0, core: true },
  { name: "breach-w", a: BREACH, r0: 40, r1: 92, w0: 1.3, w1: 2.8, wob: 0.03, wl: 9, ph: 4.0, rb: 40, dr: 0.0058 },
  { name: "ese", a: 0.5, r0: RC - 2, r1: 92, w0: 1.5, w1: 4.2, wob: 0.06, wl: 10, ph: 2.0, rb: 0, dr: 0, core: true },
  { name: "nw", a: -2.3, r0: RC - 2, r1: 86, w0: 1.4, w1: 3.8, wob: 0.06, wl: 13, ph: 0.9, rb: 0, dr: 0, core: true },
  { name: "sw", a: 2.55, r0: RC - 2, r1: 80, w0: 1.3, w1: 3.2, wob: 0.07, wl: 9, ph: 5.1, rb: 0, dr: 0 },
];
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
const PLANS = {
  Harbour: { size: [28, 22], wallHeight: 6, towerShape: "round", towerRadius: 3.2, towerHeight: 11, keep: [7, 6], keepHeight: 11, mat: 12 },
  Ashgate: { size: [26, 20], wallHeight: 6, towerShape: "square", towerRadius: 3, towerHeight: 12, keep: [8, 7], keepHeight: 12, mat: 27 },
  Highwatch: { size: [24, 18], wallHeight: 7, towerShape: "round", towerRadius: 3.4, towerHeight: 13, keep: [8, 8], keepHeight: 13, mat: 16 },
  Seawall: { size: [30, 18], wallHeight: 6, towerShape: "square", towerRadius: 3, towerHeight: 11, keep: [6, 6], keepHeight: 10, mat: 27 },
  Cinder: { size: [26, 20], wallHeight: 6, towerShape: "round", towerRadius: 3.6, towerHeight: 13, keep: [8, 7], keepHeight: 12, mat: 12 },
  Strand: { size: [24, 20], wallHeight: 6, towerShape: "round", towerRadius: 3.2, towerHeight: 11, keep: [6, 6], keepHeight: 10, mat: 16 },
  Ember: { size: [26, 20], wallHeight: 6, towerShape: "square", towerRadius: 3.2, towerHeight: 12, keep: [8, 6], keepHeight: 12, mat: 12 },
};
const GATE_SLACK = 4;            // the straight stretch overruns the castle by this on each end
// where each castle stands, as the profile's radius: the beach (shore), the tread under the second cliff band, the tread above it
const SHORE = 82, LOW = 66, HIGH = 46;
// the three roads: castles [name, angle, radius] in the order the road meets them (dir 1: the angle grows, clockwise on screen),
// and the angle where it reaches the rim. A castle's long side runs along x or z, whichever is nearer the contour there; the
// road runs straight through its two gates, and between castles it climbs at no more than SP, in switchbacks when the way is
// shorter than the climb needs.
const LAYOUT = [
  { name: "R1", sites: [["Strand", -0.15, SHORE], ["Ashgate", 0.45, LOW], ["Cinder", 1.05, HIGH]], end: 1.8 },
  { name: "R2", sites: [["Harbour", 2.25, SHORE], ["Ember", 3.0, LOW]], end: 3.55 },
  { name: "R3", sites: [["Seawall", 4.3, SHORE], ["Highwatch", 5.15, HIGH]], end: 5.7 },
];

// from P (at height yP) to Q (yQ): a polyline resampled every unit whose climb is at most SP a unit; when the straight way is
// too short for the climb it swings to the side in a sine of m half waves (switchbacks), to the side that keeps off the other castles
function link(P, yP, Q, yQ, avoid) {
  const D = Math.hypot(Q[0] - P[0], Q[1] - P[1]), need = Math.max(D, Math.abs(yQ - yP) / (SP * 0.95));
  const n = [-(Q[1] - P[1]) / D, (Q[0] - P[0]) / D];
  const poly = (amp, m, sign) => {
    const out = [];
    for (let i = 0; i <= 400; i++) {
      const t = i / 400, w = amp * Math.sin(Math.PI * m * t) * Math.sin(Math.PI * t) ** 0.5;
      out.push([P[0] + (Q[0] - P[0]) * t + n[0] * sign * w, P[1] + (Q[1] - P[1]) * t + n[1] * sign * w]);
    }
    return out;
  };
  const length = (pl) => pl.reduce((acc, p, i) => acc + (i ? Math.hypot(p[0] - pl[i - 1][0], p[1] - pl[i - 1][1]) : 0), 0);
  const bad = (pl) => pl.reduce((acc, p, i) => acc + (avoid(p, i / pl.length) ? 1 : 0) + (rhoAt(p[0], p[1]) < RC + 3 || rhoAt(p[0], p[1]) > RF + 8 ? 1 : 0), 0);
  let best = poly(0, 1, 1);
  if (need > D * 1.02) {
    best = null;
    let bestScore = 1e9;
    for (let m = 1; m <= Math.max(1, Math.floor(D / 12)); m++) {
      for (const sign of [1, -1]) {
        let lo = 0, hi = 30;
        for (let i = 0; i < 30; i++) { const amp = (lo + hi) / 2; if (length(poly(amp, m, sign)) < need) lo = amp; else hi = amp; }
        const pl = poly((lo + hi) / 2, m, sign), sc = bad(pl) * 10 + m + (lo + hi) / 2 * 0.2;
        if (length(pl) >= need * 0.98 && hi < 29 && sc < bestScore) { best = pl; bestScore = sc; }
      }
    }
    if (!best) throw new Error(`no way from ${P} to ${Q} climbing ${yQ - yP} in ${need}`);
  }
  // resample every unit, the height linear in the arc length
  const L = length(best), pts = [];
  let acc = 0, j = 1;
  for (let d = 0; d <= L; d += 1) {
    while (j < best.length - 1 && acc + Math.hypot(best[j][0] - best[j - 1][0], best[j][1] - best[j - 1][1]) < d) { acc += Math.hypot(best[j][0] - best[j - 1][0], best[j][1] - best[j - 1][1]); j++; }
    const seg = Math.hypot(best[j][0] - best[j - 1][0], best[j][1] - best[j - 1][1]) || 1, f = Math.min(1, (d - acc) / seg);
    pts.push([best[j - 1][0] + (best[j][0] - best[j - 1][0]) * f, best[j - 1][1] + (best[j][1] - best[j - 1][1]) * f, yP + (yQ - yP) * (d / L)]);
  }
  return pts;
}

function layRoad(spec) {
  const castles = [];
  for (const [name, th, rho] of spec.sites) {
    const pl = PLANS[name], at = pointAt(rho, th), floor = Math.round(profile(rho, th));
    const tx = -Math.sin(th), tz = Math.cos(th);
    const ax = Math.abs(tx) >= Math.abs(tz) ? "x" : "z", sg = (ax === "x" ? tx : tz) >= 0 ? 1 : -1;
    const len = pl.size[0] + 2 * GATE_SLACK, h = len / 2;
    const run = [];
    for (let i = 0; i <= len; i++) run.push(ax === "x" ? [at[0] + sg * (i - h), at[1]] : [at[0], at[1] + sg * (i - h)]);
    castles.push({ name, road: spec.name, at, floor, axis: ax, plan: pl, along: pl.size[0], across: pl.size[1],
      arrive: ax === "x" ? (sg > 0 ? "w" : "e") : (sg > 0 ? "n" : "s"), depart: ax === "x" ? (sg > 0 ? "e" : "w") : (sg > 0 ? "s" : "n"), run, rho });
  }
  const inFoot = (p, c, pad) => Math.abs(p[0] - c.at[0]) < (c.axis === "x" ? c.along : c.across) / 2 + pad && Math.abs(p[1] - c.at[1]) < (c.axis === "x" ? c.across : c.along) / 2 + pad;
  const pts = [];
  castles.forEach((c, i) => {
    for (const p of c.run) pts.push([p[0], p[1], c.floor]);
    const next = castles[i + 1], P = c.run[c.run.length - 1];
    const Q = next ? next.run[0] : pointAt(RC + 3, spec.end), yQ = next ? next.floor : Math.round(profile(RC + 3, spec.end)) - 1;
    // the castles' own footprints are not obstacles for the road leaving them
    // (the road keeps off the other roads' castles, and off its own except where it leaves one and arrives at the next)
    const avoid = (p, t) => ALL_SITES.some((o) => o.road !== spec.name && Math.hypot(p[0] - o.at[0], p[1] - o.at[1]) < 24) ||
      castles.some((k) => (k !== c && k !== next ? inFoot(p, k, 7) : t > 0.12 && t < 0.88 && inFoot(p, k, 7)));
    for (const p of link(P, c.floor, Q, yQ, avoid)) pts.push(p);
  });
  return { ...spec, pts, castles, end: pts[pts.length - 1] };
}
// every castle's site first (so a road can keep off the other roads' castles)
const ALL_SITES = LAYOUT.flatMap((sp) => sp.sites.map(([name, th, rho]) => ({ road: sp.name, at: pointAt(rho, th) })));
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
  const ridges = 1 + 0.05 * Math.cos(9 * th + 1.5 * noise(x / 40, z / 40, 9)) * smooth(RC + 2, RC + 22, rho) * smooth(8, 24, profile(rho, th));
  let dip = 0;
  for (const t of TONGUES) {
    if (r < t.r0 || r > t.r1) continue;
    const w = tongueWidth(t, r) * 1.5 + 1.4;
    const q = (r * angd(th, tongueAngle(t, r))) / w;
    dip = Math.max(dip, (t.name === "breach" ? 3.0 : 2.0) * Math.exp(-q * q) * smooth(t.r0, t.r0 + 6, r) * (1 - smooth(t.r1 - 10, t.r1, r)));
  }
  dip *= smooth(RC + 1, RC + 6, rho);
  // the rim: spurs of varied height (never a regular row of teeth), lower where a road reaches it
  let roadMaskH = 0;
  for (const rd of ROADS) roadMaskH = Math.max(roadMaskH, Math.exp(-((angd(th, Math.atan2(rd.end[1], rd.end[0])) / 0.2) ** 2)));
  const spur = 3 + Math.max(0, noise(Math.cos(th) * 3.3 + 4, Math.sin(th) * 3.3 + 4, 17) + 0.2) * 7 * (1 - roadMaskH);
  const crest = spur * Math.exp(-(((rho - RC) / 3.6) ** 2));
  const outer = profile(Math.max(rho, RC), th) * ridges - dip + crest;
  const crestH = profile(RC, th) + 3;
  // the crater: a flat floor, steep walls, a dais for the mine factory in the middle
  const bowl = FLOOR_Y + (crestH - FLOOR_Y) * smooth(12, RC - 1, rho) + 2.6 * (1 - smooth(4.5, 7.5, rho));
  let c = bowl + (outer - bowl) * smooth(RC - 9, RC + 1.5, rho);
  // the breach: the rim falls away in a notch facing south, down to the bowl's floor, so the bowl, its mines and the lava
  // flow that runs out of it are seen from the default camera
  const bw = Math.exp(-((angd(th, BREACH) / 0.27) ** 2));
  const ramp = FLOOR_Y + 5 + Math.max(0, rho - (RC - 8)) * 0.8;
  c -= bw * smooth(RC + 20, RC + 6, rho) * smooth(4, 10, rho) * Math.max(0, c - ramp);
  let h = c;
  // rocks and bars in the shallows and on the beach
  h += 2.0 * fbm(x / 9, z / 9, 33, 2) * Math.exp(-((h / 5) ** 2));
  // shelves under the castles (cut and fill to the castle's floor), roads cut and filled into the flank
  let wShelf = 0, wRoad = 0;
  for (const k of CASTLES) {
    const hx = (k.axis === "x" ? k.along : k.across) / 2 + 3, hz = (k.axis === "x" ? k.across : k.along) / 2 + 3;
    const d = Math.hypot(Math.max(0, Math.abs(x - k.at[0]) - hx), Math.max(0, Math.abs(z - k.at[1]) - hz));
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
