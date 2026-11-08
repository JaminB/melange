// Caldera's terrain, as pure functions of the map position (map units, y up, the sea at about -1, the island centred on 0, 0):
// a round volcanic cone (a radial profile, shaken by angle so the outline and the flanks are irregular), a crater with a
// breach facing south, lava gullies, three climbing roads that run through the castle shelves, the shelves themselves,
// and the expressions (lava tongues) that paint the flows. build.mjs turns it into a height grid for heightfield_drape.

// ---- numbers ----
export const R = 104;            // radius of the cone's foot (the sea line is a little inside it)
export const H0 = 96;            // the profile's height at the axis (the crater takes the top 24 units off it)
export const Q = 1.15;           // the profile h = H0 (1 - rho / R)^Q: a straight flank with a slightly concave foot
export const RC = 24;            // radius of the rim crest
export const FLOOR_Y = 46;       // the crater floor
export const BREACH = Math.PI / 2;   // +z, the south: the default camera looks from there, so the notch and the flow show
export const SP = 0.5;           // the roads' climb per unit of length (26.6 degrees)
export const BOX = 115;          // half side of the heightfield rectangle

export const coneAt = (rho) => H0 * Math.pow(Math.max(0, 1 - rho / R), Q);
const gradOfCone = (rho) => (H0 * Q / R) * Math.pow(Math.max(1e-6, 1 - rho / R), Q - 1);

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
// exactly the cone's height under it.
const HEADS = [[-0.62, 0.30, 0.075], [2.75, 0.36, 0.06]];   // angle, width (rad), how far the coast is pushed out (fraction of R)
const COVES = [[0.10, 0.30, 0.09], [2.05, 0.22, 0.07], [4.35, 0.25, 0.07]];
export function scaleAt(th) {
  let s = 1 - 0.065 * fbm(Math.cos(th) * 1.9 + 9, Math.sin(th) * 1.9 + 9, 5, 3);
  for (const [a, w, k] of HEADS) s -= k * gaussA(th, a, w);
  for (const [a, w, k] of COVES) s += k * gaussA(th, a, w);
  return s;
}
export const rhoAt = (x, z) => Math.hypot(x, z) * scaleAt(Math.atan2(z, x));
// the point at rho and angle th
export const pointAt = (rho, th) => { const r = rho / scaleAt(th); return [r * Math.cos(th), r * Math.sin(th)]; };

// ---- the lava tongues: centre angle as a function of the radius (shared by the gullies and the paint) ----
// a: centre angle, r0 / r1: radius range, w0 / w1: half width (map units) at both ends, wob / wl / ph: a wander, rb / dr: a branch
// that leaves the line at radius rb and bends dr radians per unit of radius.
export const TONGUES = [
  { name: "breach", a: BREACH, r0: 6, r1: 112, w0: 5.2, w1: 2.0, wob: 0.05, wl: 11, ph: 0.3, rb: 0, dr: 0, core: true },
  { name: "breach-e", a: BREACH, r0: 48, r1: 100, w0: 1.8, w1: 1.1, wob: 0.03, wl: 8, ph: 1.7, rb: 48, dr: -0.0058 },
  { name: "breach-w", a: BREACH, r0: 34, r1: 90, w0: 1.9, w1: 1.1, wob: 0.03, wl: 9, ph: 4.0, rb: 34, dr: 0.0050 },
  { name: "ese", a: 0.42, r0: RC - 2, r1: 92, w0: 3.4, w1: 1.3, wob: 0.06, wl: 10, ph: 2.0, rb: 0, dr: 0 },
  { name: "ese-b", a: 0.42, r0: 56, r1: 98, w0: 1.5, w1: 1.0, wob: 0.03, wl: 8, ph: 0.6, rb: 56, dr: 0.0075 },
  { name: "sw", a: 2.62, r0: RC - 2, r1: 86, w0: 3.2, w1: 1.3, wob: 0.06, wl: 12, ph: 5.1, rb: 0, dr: 0 },
  { name: "sw-b", a: 2.62, r0: 46, r1: 90, w0: 1.5, w1: 1.0, wob: 0.03, wl: 8, ph: 2.2, rb: 46, dr: -0.0068 },
  { name: "ne", a: -0.82, r0: RC - 2, r1: 82, w0: 3.0, w1: 1.2, wob: 0.07, wl: 9, ph: 3.3, rb: 0, dr: 0 },
  { name: "nw", a: -2.25, r0: RC - 2, r1: 78, w0: 2.8, w1: 1.2, wob: 0.06, wl: 13, ph: 0.9, rb: 0, dr: 0 },
  { name: "nw-b", a: -2.25, r0: 44, r1: 84, w0: 1.4, w1: 1.0, wob: 0.03, wl: 8, ph: 4.4, rb: 44, dr: 0.0060 },
];
const tongueAngle = (t, r) => t.a + t.wob * Math.sin(r / t.wl + t.ph) + t.dr * Math.max(0, r - t.rb);
const tongueWidth = (t, r) => t.w0 + (t.w1 - t.w0) * Math.min(1, Math.max(0, (r - t.r0) / (t.r1 - t.r0)));
// the tongues as one mask expression for the heightfield materials (above 0 inside any of them; the rule list holds 16, so all
// the tongues share one rule): `scale` narrows them (a hot core)
export function tonguesMask(list, scale = 1) {
  const f = (v) => Number(v.toFixed(5));
  const call = (t) => `tg(${f(t.a)}, ${f(t.wob)}, ${t.wl}, ${t.ph}, ${f(t.dr)}, ${t.rb}, ${t.r0}, ${t.r1}, ${f(t.w0 * scale)}, ${f(t.w1 * scale)})`;
  const defs = "fn wrap(d) = atan2(sin(d), cos(d)); " +
    "fn tg(a, wob, wl, ph, dr, rb, r0, r1, w0, w1) = min(lerp(w0, w1, clamp((r - r0) / (r1 - r0), 0, 1)) - r * abs(wrap(theta - (a + wob * sin(r / wl + ph) + dr * max(0, r - rb)))), min(r - r0, r1 - r)); ";
  return defs + list.map(call).reduce((acc, c) => (acc ? `max(${acc}, ${c})` : c), "");
}

// ---- the roads and the castles on them ----
// A road climbs the flank at SP a unit: its heading leans from the contour towards the top by the angle that gives SP on the
// cone's own gradient. When the climb reaches a castle's height the road runs straight and level through the castle's
// footprint (along its long axis; the castle's shelf is cut and filled to that level), then goes on climbing.
function surf0(x, z) { return coneAt(Math.max(rhoAt(x, z), RC)); }
function grad(x, z) {
  const e = 0.6;
  return [(surf0(x + e, z) - surf0(x - e, z)) / (2 * e), (surf0(x, z + e) - surf0(x, z - e)) / (2 * e)];
}
const SPECS = [
  // dir 1: the angle grows (counter-clockwise on the map: x right, z down is clockwise on screen); flats: castle heights
  { name: "R1", dir: 1, theta: 2.55, h0: 10, flats: [{ h: 12, castle: "Harbour" }, { h: 24, castle: "Ashgate" }, { h: 34, castle: "Highwatch" }] },
  { name: "R2", dir: 1, theta: -1.55, h0: 11, flats: [{ h: 13, castle: "Seawall" }, { h: 27, castle: "Cinder" }] },
  { name: "R3", dir: -1, theta: 0.9, h0: 10, flats: [{ h: 11, castle: "Strand" }, { h: 21, castle: "Ember" }] },
];
// the castles' plans: size [along the road, across it], keep, towers, stone (pale or warm; the cone is black and slate)
const PLANS = {
  Harbour: { size: [28, 22], wallHeight: 6, towerShape: "round", towerRadius: 3.2, towerHeight: 11, keep: [7, 6], keepHeight: 11, mat: 12 },
  Ashgate: { size: [26, 20], wallHeight: 6, towerShape: "square", towerRadius: 3, towerHeight: 12, keep: [8, 7], keepHeight: 12, mat: 16 },
  Highwatch: { size: [24, 20], wallHeight: 7, towerShape: "round", towerRadius: 3.4, towerHeight: 14, keep: [8, 8], keepHeight: 14, mat: 55 },
  Seawall: { size: [30, 18], wallHeight: 6, towerShape: "square", towerRadius: 3, towerHeight: 11, keep: [6, 6], keepHeight: 10, mat: 16 },
  Cinder: { size: [26, 22], wallHeight: 6, towerShape: "round", towerRadius: 3.6, towerHeight: 13, keep: [8, 7], keepHeight: 12, mat: 12 },
  Strand: { size: [24, 20], wallHeight: 6, towerShape: "round", towerRadius: 3.2, towerHeight: 11, keep: [6, 6], keepHeight: 10, mat: 55 },
  Ember: { size: [26, 20], wallHeight: 6, towerShape: "square", towerRadius: 3.2, towerHeight: 12, keep: [8, 6], keepHeight: 12, mat: 12 },
};
const GATE_SLACK = 4;            // the straight stretch overruns the castle by this on each end

export function marchRoad(spec) {
  const [x0, z0] = pointAt(R * (1 - Math.pow(spec.h0 / H0, 1 / Q)), spec.theta);
  let x = x0, z = z0, y = surf0(x, z);
  const pts = [[x, z, y]], castles = [];
  let flat = 0, steps = 0;
  const L = (c) => PLANS[c].size[0];
  while (rhoAt(x, z) > RC + 4 && steps++ < 1500) {
    const g = grad(x, z), gm = Math.hypot(g[0], g[1]) || 1e-6;
    const U = [g[0] / gm, g[1] / gm], T = [spec.dir * U[1], -spec.dir * U[0]];
    const psi = Math.asin(Math.min(0.95, SP / gm));
    const d = [Math.cos(psi) * T[0] + Math.sin(psi) * U[0], Math.cos(psi) * T[1] + Math.sin(psi) * U[1]];
    if (flat < spec.flats.length && y >= spec.flats[flat].h - 0.05) {
      const f = spec.flats[flat++], pl = PLANS[f.castle];
      y = Math.round(y);                              // the castle floor is a whole unit
      // a level run along the contour (a circle about the axis, at the cone's radius here), through the castle: the castle's
      // long side runs along it, the shelf is cut and filled to this height
      const len = pl.size[0] + 2 * GATE_SLACK, rho = rhoAt(x, z);
      let th = Math.atan2(z, x);
      const run = [[x, z]];
      for (let s = 1; s <= len; s++) { th += spec.dir / (rho / scaleAt(th)); const p = pointAt(rho, th); run.push(p); pts.push([p[0], p[1], y]); }
      const mid = run[Math.round(len / 2)], a = run[0], e = run[len];
      const side = (vx, vz) => (Math.abs(vx) >= Math.abs(vz) ? (vx > 0 ? "e" : "w") : (vz > 0 ? "s" : "n"));
      const sa = side(a[0] - mid[0], a[1] - mid[1]);
      // the long side of the castle runs along x when the run is more along x than z
      const ax = Math.abs(e[0] - a[0]) >= Math.abs(e[1] - a[1]) ? "x" : "z";
      castles.push({ name: f.castle, road: spec.name, at: mid, floor: y, axis: ax, plan: pl, along: pl.size[0], across: pl.size[1],
        arrive: sa, depart: side(e[0] - mid[0], e[1] - mid[1]), run });
      [x, z] = run[len];
      continue;
    }
    x += d[0]; z += d[1];
    y = Math.min(y + SP, Math.max(y, surf0(x, z)));
    pts.push([x, z, y]);
  }
  return { ...spec, pts, castles, end: pts[pts.length - 1] };
}
export const ROADS = SPECS.map(marchRoad);
export const CASTLES = ROADS.flatMap((r) => r.castles);

// ---- the height of the island at (x, z), map units above the floor ----
export function island(x, z) {
  const th = Math.atan2(z, x), r = Math.hypot(x, z), rho = r * scaleAt(th);
  // the cone: ridges grow with the radius from the rim; gullies are cut under the lava tongues
  const ridges = 1 + 0.07 * Math.cos(9 * th + 1.5 * noise(x / 40, z / 40, 9)) * smooth(RC + 2, RC + 22, rho);
  let dip = 0;
  for (const t of TONGUES) {
    if (r < t.r0 || r > t.r1) continue;
    const w = tongueWidth(t, r) * 1.6 + 1.2;
    const q = (r * angd(th, tongueAngle(t, r))) / w;
    dip = Math.max(dip, (t.name === "breach" ? 3.2 : 2.0) * Math.exp(-q * q) * smooth(t.r0, t.r0 + 6, r) * (1 - smooth(t.r1 - 10, t.r1, r)));
  }
  dip *= smooth(RC + 1, RC + 6, rho);
  // the rim: spurs of varied height (never a regular row of teeth), lower where a road reaches it
  let roadMask = 0;
  for (const rd of ROADS) roadMask = Math.max(roadMask, Math.exp(-((angd(th, Math.atan2(rd.end[1], rd.end[0])) / 0.2) ** 2)));
  const spur = 3 + Math.max(0, noise(Math.cos(th) * 3.3 + 4, Math.sin(th) * 3.3 + 4, 17) + 0.2) * 7 * (1 - roadMask);
  const crest = spur * Math.exp(-(((rho - RC) / 3.6) ** 2));
  const outer = coneAt(Math.max(rho, RC)) * ridges - dip + crest;
  const crestH = coneAt(RC) + 3;
  const bowl = FLOOR_Y + (crestH - FLOOR_Y) * smooth(9, RC + 1, rho);
  let c = bowl + (outer - bowl) * smooth(RC - 4, RC + 1.5, rho);
  // the breach: the rim falls away in a notch facing south, down to the bowl's floor, so the bowl, its mines and the lava
  // flow that runs out of it are seen from the default camera
  const bw = Math.exp(-((angd(th, BREACH) / 0.27) ** 2));
  const ramp = FLOOR_Y + 5 + Math.max(0, rho - (RC - 8)) * 0.8;
  c -= bw * smooth(RC + 20, RC + 6, rho) * smooth(4, 10, rho) * Math.max(0, c - ramp);
  // the beach: the foot flattens towards the sea
  c = Math.max(0, c - 1.5);
  c *= 0.35 + 0.65 * smooth(0, 11, c);
  let h = c;
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
  h += 1.1 * fbm(x / 8, z / 8, 21, 2) * smooth(2, 14, h) * (1 - wShelf) * (1 - wRoad);
  return Math.max(0, h);
}
export function gridOf(min = -BOX, max = BOX, step = 1) {
  const rows = [];
  for (let z = min; z <= max + 1e-6; z += step) {
    const row = [];
    for (let x = min; x <= max + 1e-6; x += step) row.push(Math.round(island(x, z) * 100) / 100);
    rows.push(row);
  }
  return rows;
}
