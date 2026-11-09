// Caldera's terrain, as pure functions of the map position (map units, y up, the sea at about -1, the island centred on 0, 0):
// a round volcanic cone whose flank climbs in treads between two bands of cliffs (notched where the trails cross them), a
// crater with a breach facing south and a dais for the mine factory, a coast of beaches, headlands and coves that slopes under
// the sea, lava tongues down the cardinals that follow gullies and widen downslope, four switchback trails up the diagonals from
// the four team castles' shelves near the shore to the rim, three higher castles beside them, and the expressions (lava
// tongues, trails, courts) that paint them. build.mjs turns it into a height grid for heightfield_drape.

// ---- numbers ----
export const RF = 92;            // the coast, in the profile's radius (rho): the beach is at 0, the sea floor falls away outside
export const RC = 36;            // radius of the rim crest (a wide bowl: the crater floor and the plug the mine factory stands on lie inside it)
export const FLOOR_Y = 49;       // the crater floor (the rim is 20-30 over it: a deep bowl, iteration 10)
export const FLAT_R = 18;        // the floor is level out to this radius, the walls climb from there to the rim
export const PLUG_RHO = 15, PLUG_TH = -1.15;   // the basalt plug of the mine factory: on the floor, off the centre, on the side the south camera sees over the breach
export const LAKE = { c: [-5, 6], R: 8 };   // the crater's lava lake: well away from the plug (about 23 units), so the factory's landing apron stays ash
// The mine factory's launchers [rho, th]: low on the crater's inner walls (rho 23, just up from the level floor at FLAT_R,
// well under the rim crest at RC), each on a 6 x 6 pad. The bearings are the eighths round the crater (from an eighth off
// the plug's) whose pads held a dropped mine in the game: erg-mcp's game_mine_drop on 2026-11-09 (testenv B) put five
// mines on each of eight such pads, twice; the pads at -0.365 (east), 1.206 and 1.992 (either side of the breach) and
// 5.133 (north) held every one, the four others (0.421, 2.777, 3.562, 4.348) none, the same pads every time, while worms
// stand on all of them (docs/erg.md, the mine factory: why is not known). LAUNCHER_TH overrides them for such a probe.
export const LAUNCHERS = (process.env.LAUNCHER_TH ? process.env.LAUNCHER_TH.split(",").map(Number) : [-0.365, 1.206, 1.992, 5.133])
  .map((th) => [23, th]);
export const APRON_R = 10.5;     // dry ash round the plug: the mines land on it and show from the crater camera
export const BREACH = Math.PI / 2;   // +z, the south: the default camera looks from there, so the notch and the flow show
export const SP = 0.62;          // the roads' climb per unit of length (32 degrees)
export const BOX = 118;          // half side of the heightfield rectangle (the skin is clipped to a circle inside it)
export const CLIP = 117;         // radius of that circle
export const SEA_FLOOR = -9;     // the skin's lowest level (the offshore slope runs down to it)
export const TREAD = 0.52;       // slope of the flank between the cliffs (27 degrees)
// the cliff bands: the flank steps up by `a` over about 5 units at rho (wobbled by angle); roads climb through them
// v2 round 3: the inner band split in two steps (43.5 and 51.5, 7 each) with a ledge between them, so the upper cone reads as stepped
// rock instead of one sheer slab (CLIFF_SPLIT=0 keeps round 2's single band at 47, 13 high)
const SPLIT = +(globalThis.process?.env?.CLIFF_SPLIT ?? 1);
export const CLIFFS = SPLIT ? [{ rho: 43.5, a: 7 }, { rho: 78, a: 6 }, { rho: 51.5, a: 7 }] : [{ rho: 47, a: 13 }, { rho: 78, a: 6 }];
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
// where a trail crosses a cliff band its step is spread over NOTCH_W[k] more each side (within about NOTCH_S radians of the
// crossing), so the switchbacks can climb it; `notch: false` is the band unnotched (the crater's rim is raised from that, so the
// crater never moves with the trails)
let NOTCHES = [];
let TRAIL_ENDS = [];        // where the trails reach the rim (set once they are laid: the rim is lowered there)
const NOTCH_W = [8, 3.5, 6], NOTCH_S = [0.45, 0.3, 0.4];
// each band's half width (v2 round 3: the inner cliff spread from 1.75 to 3, so it reads as a steep rock band, not a sheer slab)
const CLIFF_HW = SPLIT ? [2.4, 3.25, 2.4] : [+(globalThis.process?.env?.CLIFF_HW ?? 3), 3.25];
export function profile(rho, th, notch = true) {
  let h = F(RF - rho) + BOOST * (1 - smooth(RC - 2, RC + 46, rho));
  CLIFFS.forEach((c, k) => {
    const rk = c.rho + 4.5 * noise(Math.cos(th) * 1.6 + k * 7, Math.sin(th) * 1.6 + k * 3, 40 + k);
    const a = c.a * (0.85 + 0.15 * noise(Math.cos(th) * 2.3 + k, Math.sin(th) * 2.3, 60 + k));
    let wn = 0;
    if (notch) for (const n of NOTCHES) if (n.k === k) wn = Math.max(wn, gaussA(th, n.th, NOTCH_S[k]));
    const hw = CLIFF_HW[k] + NOTCH_W[k] * wn;
    h += a * (1 - smooth(rk - hw, rk + hw, rho));
  });
  return h;
}

// ---- ledges on the flanks (v2 round 3: "soften the cone flanks between the trails with a few terraces or ledges"): the outer
// flank's height pulled toward a staircase of `step` (wandering with noise) by `mix`, each step a ledge for `flat` of it and a
// steeper riser above, fading in over the foothills and out toward the rim; the trails' hairpins take islandBase's height, so the
// legs are laid over the ledged flank
const RIBS = +(globalThis.process?.env?.RIBS ?? 2.4);
export const LEDGES = { step: 5.5, flat: 0.45, mix: +(globalThis.process?.env?.LEDGE_MIX ?? 0.55) };
function ledged(h, x, z, rho) {
  const m = LEDGES.mix * smooth(8, 16, h) * (1 - smooth(RC + 4, RC + 1, rho)) * (0.6 + 0.4 * noise(x / 18, z / 18, 78));
  if (m <= 0) return h;
  const s = LEDGES.step * (1 + 0.15 * noise(x / 30, z / 30, 77)), q = h / s, f = q - Math.floor(q);
  return h + ((Math.floor(q) + smooth(LEDGES.flat, 1, f)) * s - h) * m;
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
  { name: "breach-w", a: BREACH, r0: 48, r1: 88, w0: 1.8, w1: 3.8, wob: 0.03, wl: 9, ph: 4.0, rb: 48, dr: 0.004 },
  { name: "east", a: 0.17, r0: RC - 2, r1: 88, w0: 2.4, w1: 5.2, wob: 0.06, wl: 10, ph: 2.0, rb: 0, dr: 0, core: true },
  { name: "north", a: -1.53, r0: RC - 2, r1: 86, w0: 2.2, w1: 4.6, wob: 0.06, wl: 13, ph: 0.9, rb: 0, dr: 0, core: true },
  { name: "west", a: 3.1, r0: RC - 2, r1: 84, w0: 2.0, w1: 4.4, wob: 0.07, wl: 9, ph: 5.1, rb: 0, dr: 0, core: true },
];
// dry gullies: deep radial cuts in the flank without lava (angle, half width in radians, depth, radius range), so the cone has ribs
// (v2: off the trails' diagonals, beside the lava of the cardinals)
export const GULLIES = [{ a: -1.3, w: 0.06, d: 7, r0: 38, r1: 68 }, { a: 1.25, w: 0.06, d: 6.5, r0: 40, r1: 75 }, { a: 3.0, w: 0.06, d: 7, r0: 40, r1: 70 }, { a: -2.95, w: 0.055, d: 6, r0: 40, r1: 70 },
  { a: -0.25, w: 0.05, d: 5, r0: 38, r1: 52 }];
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

// ---- the castles and the switchback trails ----
// the flank under the rim (the trails are marched over it) and its gradient
function surf0(x, z) { return profile(Math.max(rhoAt(x, z), RC), Math.atan2(z, x)); }
function grad(x, z) {
  const e = 0.6;
  return [(surf0(x + e, z) - surf0(x - e, z)) / (2 * e), (surf0(x, z + e) - surf0(x, z - e)) / (2 * e)];
}
// the castles' plans: size [along the trail, across it], keep, towers, stone
// kind: castle (build_castle), ring (a round curtain wall round a round keep, on a crag), lfort (a castle with a wing and a sea gate)
// The four team castles (Strand, Ashgate, Harbour, Seawall) have a keep or an open court, with four courtyard spawn spots or more.
const PLANS = {
  // keep: [along the trail, across it]. The castles are 22 across so the lane between the keep and the corner towers' inner edge
  // (laneOf) is 4.5 or more clear: both gates open into it, beside the keep, so the trail never meets a keep or a pillar.
  Strand: { kind: "castle", size: [26, 22], wallHeight: 6, towerShape: "round", towerRadius: 3.2, towerHeight: 11, keep: [7, 6], keepHeight: 10, mat: 16 },
  Ashgate: { kind: "castle", size: [24, 22], wallHeight: 6, towerShape: "square", towerRadius: 2.6, towerHeight: 10, keep: [7, 6], keepHeight: 11, mat: 27 },
  Cinder: { kind: "castle", size: [24, 22], wallHeight: 6, towerShape: "round", towerRadius: 2.6, towerHeight: 10, keep: [9, 7], keepHeight: 16, mat: 12 },
  Harbour: { kind: "lfort", size: [26, 22], wing: [14, 14], wallHeight: 6, towerShape: "round", towerRadius: 3, towerHeight: 11, keep: [6, 5], keepHeight: 10, mat: 12 },
  Ember: { kind: "castle", size: [17, 15], wallHeight: 7, towerShape: "square", towerRadius: 2.6, towerHeight: 12, keep: null, keepHeight: 0, mat: 16 },   // a small square fort
  Seawall: { kind: "castle", size: [22, 20], wallHeight: 6, towerShape: "square", towerRadius: 3, towerHeight: 11, keep: null, keepHeight: 0, mat: 27 },   // a gatehouse fort round an open court
  Highwatch: { kind: "ring", size: [25, 25], ringRadius: 11, wallHeight: 6, keepRadius: 4.5, keepHeight: 14, towerRadius: 2.6, towerHeight: 10, mat: 16 },
};
// the lane between a castle's keep and its corner towers, on the +x / +z side of the keep: where both gates open (o = its middle
// across the trail's axis, w = its clear width, 4.5 or more). A gate's own height is the wall's less 1 (build.mjs).
export function laneOf(pl) {
  if (!pl.keep) return { o: 0, w: 4.5 };
  const kh = pl.keep[1] / 2, outer = pl.size[1] / 2 - (pl.towerRadius ?? 3), w = Math.min(5.5, outer - kh);
  return { o: Math.round((kh + kh + w) / 2 * 10) / 10, w: Math.round(w * 10) / 10 };
}
const SKIN_DROP = 1;             // the drape skin's walking surface under the grid's height there (measured; see castleAt)
export const CASTLE_SCALE = +(globalThis.process?.env?.CASTLE_SCALE || 1.5);   // map units per voxel of the castles (build.mjs builds them so)
// The trails (v2: the user's "obvious trails from each castle to the top of the mountain ... in the form of switchbacks").
// Each team has a castle near the shore, turned to the contour (build_castle's rotation: its local x runs along the contour, its
// local +z faces the sea), and a trail of its own from it to the rim crest: in by the gate on one end wall, along the court beside
// the keep, out by the gate on the other end (the side `dir`: +1 the bearing growing, counter-clockwise seen from above), then a
// COLUMN of switchbacks up the flank: hairpins to alternate sides of the column's centre line (`amp` 8-17 off it), spaced so
// that every leg climbs the same (but at least `legSep` radially apart, so a bank of the flank stands between two legs) and each
// as wide as climbing its leg at `grade` needs (on a cliff band's notch the legs run wider). The trail's height is the flank's own
// at each hairpin and straight between them, so each leg is one even grade (across a cliff it is cut into the top and built out
// at the foot). The higher castles stand beside a column, a level BRANCH from their gate to the leg at their floor. The column's
// centre line is `lead` beyond the out gate along the contour (v2 round 2: 24, was 9), so no leg runs over its castle's court or
// shelf (round 1's second legs passed 3-5 from the uphill walls, over a 15-unit drop into the court).
// v2 round 4: a level LANDING at every hairpin ("a wide, flat landing at least 8 units across"): each leg runs level for `flat` either
// side of a hairpin and the bed widens to `land` (half width) round it, tapering back to `bed` over `landTaper` along the legs; the legs
// are sized for the climb plus their level ends, so the grade between stays near `grade` (a spec's own grade overrides it: the NW)
export const TRAIL = { grade: +(globalThis.process?.env?.TRAIL_GRADE ?? 0.5), legSep: +(globalThis.process?.env?.TRAIL_LEGSEP ?? 9.5), ampMin: 8, ampMax: +(globalThis.process?.env?.AMP_MAX ?? 20), bed: 2.6, end: RC + 0.5, legs: 5, even: +(globalThis.process?.env?.TRAIL_EVEN ?? 0.7), ease: 0.5, lead: +(globalThis.process?.env?.TRAIL_LEAD ?? 24),
  endAmp: +(globalThis.process?.env?.TRAIL_END_AMP ?? 0.6),
  flat: +(globalThis.process?.env?.TRAIL_FLAT ?? 3), land: +(globalThis.process?.env?.TRAIL_LAND ?? 4.6), landTaper: 5,
  // (v2 round 4) the bed is cut level `shoulder` past its painted edge before the bank or the kerb starts: a skin cell whose far corner
  // stood on the cut bank tilted the bed under it (check_trail's walk refused rises of 0.5-0.9 over half a unit on the centreline:
  // the flat facets up close); the kerb moves out with it
  shoulder: +(globalThis.process?.env?.TRAIL_SHOULDER ?? 2) };
// The berm (v2 round 4: "at every hairpin, fill the inside of the V between consecutive legs up to the upper leg's height"): inside the
// wedge between a hairpin's two legs, wherever the two legs' centrelines are less than `gap` apart (summed distances), the ground is
// raised (never cut) to the upper leg's height at that point, fading out over the last `fade`: a stray worm stays on the upper leg's
// level instead of falling into a trough between the legs (team 3's NW worms stuck in one in round 3); the lower leg's bed is cut out of
// it as a bank. BERM_GAP=0 leaves the V's ground as it is.
// The berm's side over the lower leg is a bank of at most `bank` a unit from just off that leg's bed (`foot` past its edge), so the lower
// bed is never lifted (relax() would split a taller step between the two and tilt the bed).
export const BERM = { gap: +(globalThis.process?.env?.BERM_GAP ?? 17), fade: 4, bank: 2.4, foot: TRAIL.shoulder };
// The kerb (v2 round 2: "raised outer kerbs or lips about 1 unit high on the downhill side, so a worm that overshoots a hairpin
// cannot drop off a leg"): where the ground beside a bed falls away (the fill side of a leg, the tip of a hairpin), a lip `h` over
// the bed's height, level from `from` to `to` off the centreline (2.5 wide: the skin's columns are 2.5 apart, so every leg has a
// row of corners on it), on a berm at the bed's height out to `berm` that falls to the ground by `out` (relax() turns a step
// steeper than 2.9 a unit into a ramp that eats into its top, so the berm runs on past the kerb and the ramp eats the berm,
// not the kerb); not inside a castle (its footprint and 1.5 round it: the gates) or within 3.5 of a trail's rim end
// v2 round 3: the kerb is a level shelf 1 over the bed (was a ridge 1.2 high, 2.5 wide, back down to the bed's height on the berm),
// rising over 2 from the bed's edge and level out to the berm's end: a ridge that narrow on the skin's 2.5-unit corners drew as a row
// of pyramids along each leg (the sawtooth of c1_t2_w6_mid), a level shelf samples the same at every corner; its columns are also
// softened (soften(): a 3 x 3 mean) before relax. `to` (the pinned part) is the shelf's whole width now.
const KENV = (k, d) => +(globalThis.process?.env?.[k] ?? d);
// (v2 round 4) the lip rises from the level shoulder's edge (TRAIL.shoulder past the bed) to its height by KERB.from, 1 further out
// (round 3: from the bed's edge by 4.6); the pinned shelf ends at `to` as before (a wider one met the next leg's shoulder up a steep
// flank, and relax() tilted both)
export const KERB = { h: KENV("KERB_H", 1), from: KENV("KERB_FROM", TRAIL.bed + TRAIL.shoulder + 1), to: KENV("KERB_TO", 7.5), berm: KENV("KERB_BERM", 7.5), out: KENV("KERB_OUT", 12), anySide: !!+(globalThis.process?.env?.KERB_ANY ?? 0), clear: +(globalThis.process?.env?.KERB_CLEAR ?? 1.5) };
// team t spawns in TEAM_CASTLES[t]. castle: [name, floor, bearing, dir]; extra: [name, floor, side (+1 beyond the column on its
// dir side, -1 the other)]. The trails stand on the diagonals, the lava runs down the cardinals and the breach faces south.
const LAYOUT = [
  { name: "NE", team: 0, castle: ["Strand", 5, -1.15, 1], extra: ["Cinder", 22, 1] },
  { name: "SE", team: 1, castle: ["Ashgate", 5, 0.52, 1] },
  { name: "SW", team: 2, castle: ["Harbour", 5, 2.75, -1], extra: ["Ember", 26, 1] },
  // (v2 round 4) the NW legs at a gentler grade: its legs 3-4 were the steepest a worm met in round 3
  { name: "NW", team: 3, castle: ["Seawall", 5, -2.75, 1], extra: ["Highwatch", 33, 1], grade: +(globalThis.process?.env?.NW_GRADE ?? 0.44) },
];
export const TEAM_CASTLES = LAYOUT.map((t) => t.castle[0]);

// a castle's frame: ex its local +x (along the contour, the bearing shrinking), ez its local +z (out to sea); a world point's local
// coordinates; the yaw build_castle takes (degrees, counter-clockwise seen from above: local +z goes to (sin, cos) of it)
export const frameOf = (th) => ({ ex: [Math.sin(th), -Math.cos(th)], ez: [Math.cos(th), Math.sin(th)], yaw: Math.round((90 - th * 180 / Math.PI) * 10) / 10 });
export const toWorld = (c, u, v) => [c.at[0] + c.f.ex[0] * u + c.f.ez[0] * v, c.at[1] + c.f.ex[1] * u + c.f.ez[1] * v];
const toLocal = (c, x, z) => { const dx = x - c.at[0], dz = z - c.at[1]; return [dx * c.f.ex[0] + dz * c.f.ex[1], dx * c.f.ez[0] + dz * c.f.ez[1]]; };
// a castle's footprint in its own frame: [u0, v0, u1, v1] (the court and, for an L fort, the wing on the sea side), grown by pad
export function footBoxes(c, pad = 0) {
  if (c.plan.kind === "ring") { const R = c.plan.ringRadius + 1.5 + pad; return [[-R, -R, R, R]]; }
  const hx = c.along / 2 + pad, hz = c.across / 2 + pad, out = [[-hx, -hz, hx, hz]];
  if (c.plan.wing) { const [wa, wc] = c.plan.wing, cz = c.across / 2 + wc / 2 - 2; out.push([-wa / 2 - pad, cz - wc / 2 - pad, wa / 2 + pad, cz + wc / 2 + pad]); }
  return out;
}
// the distance from (x, z) to a castle's footprint grown by pad (0 inside)
export function footDist(c, x, z, pad = 0) {
  const [u, v] = toLocal(c, x, z);
  return Math.min(...footBoxes(c, pad).map(([u0, v0, u1, v1]) => Math.hypot(Math.max(0, u0 - u, u - u1), Math.max(0, v0 - v, v - v1))));
}
// the court as a mask expression for the heightfield materials (above 0 inside it, in the castle's frame)
export function courtMask(c, pad = -0.5) {
  const f = (v) => Number(v.toFixed(4));
  const d = (k, v) => (v < 0 ? `(${k} + ${f(-v)})` : `(${k} - ${f(v)})`), X = d("x", c.at[0]), Z = d("z", c.at[1]);
  const U = `(${X} * ${f(c.f.ex[0])} + ${Z} * ${f(c.f.ex[1])})`, V = `(${X} * ${f(c.f.ez[0])} + ${Z} * ${f(c.f.ez[1])})`;
  return footBoxes(c, pad).map(([u0, v0, u1, v1]) => `min(${f(u1)} - ${U}, ${U} + ${f(-u0)}, ${f(v1)} - ${V}, ${V} + ${f(-v0)})`).reduce((a, b) => `max(${a}, ${b})`);
}
// the radius (rho) on bearing th where the ground (islandBase, the mean of the point and 8 either side along the contour) first
// reaches y, coming in from the sea
function rhoForFloor(th, y) {
  for (let rho = RF + 4; rho > RC + 2; rho -= 0.25) {
    const r = rho / scaleAt(th), g = [-8, 0, 8].map((d) => { const a = th + d / r; return islandBase(r * Math.cos(a), r * Math.sin(a)); });
    if ((g[0] + g[1] + g[2]) / 3 >= y) return rho;
  }
  return RC + 2;
}
// a castle centred on its floor's contour at bearing th, the trail through it running `dir`: the in gate on the end wall behind,
// the out gate on the end wall ahead (local e / w), both in the lane beside the keep (its local +z side) or the wall's middle
function castleAt(spec, name, floor, th, dir) {
  const pl = PLANS[name], f = frameOf(th), at = pointAt(rhoForFloor(th, floor), th);
  // the court's top: build.mjs stands a castle at floor - 1 in voxels of CASTLE_SCALE (1.5), so its court is that rounded up to the
  // voxel grid (floor 5: 4.5); the trail through it and the shelf round it are cut and filled to `bed`, which the drape skin
  // stands SKIN_DROP under (query_ground on Caldera: a grid height of 5 stands at 4), so the ground outside the gates meets the court
  // (a ring castle has no floor of its own: its walls stand at floor - 1, on the skin)
  const court = pl.kind === "ring" ? floor - SKIN_DROP : Math.ceil((floor - 1) / CASTLE_SCALE - 1e-6) * CASTLE_SCALE;
  const c = { name, road: spec.name, trail: spec.name, team: TEAM_CASTLES.indexOf(name), at, th, f, floor, court, bed: court + SKIN_DROP, plan: pl, along: pl.size[0], across: pl.size[1], dir };
  // travel along -ex for dir +1 (the bearing growing): in by local e, out by local w
  const hx = pl.kind === "ring" ? pl.ringRadius : pl.size[0] / 2, o = pl.keep ? laneOf(pl).o : 0, s = -dir;
  const gate = (side, sgn) => ({ side: pl.kind === "ring" ? null : side, offset: o, G: toWorld(c, sgn * hx, o), O: toWorld(c, sgn * (hx + 4), o) });
  c.gin = gate(dir > 0 ? "e" : "w", -s); c.gout = gate(dir > 0 ? "w" : "e", s);
  c.T = [c.f.ex[0] * s, c.f.ex[1] * s];
  c.arrive = c.gin.side; c.depart = c.gout.side;
  c.run = line([c.gin.O, c.gin.G, c.gout.G, c.gout.O]);
  c.rho = rhoAt(at[0], at[1]);
  return c;
}
// A team castle's four spawn spots (v2 round 2): on the line between its two gates (the lane beside the keep, or the court's middle
// line without one), 3.5 and 8 either side of the court's middle, the played worms (places 0 and 1) on the out gate's side first,
// so each has a straight clear walk to its out gate; and a worm's lead-in to its trail: the spot, the court's middle on that line
// (when the spot is behind it), the out gate and the point outside it where the trail leaves the castle ([x, z])
export function laneSpawns(c) {
  const o = c.gin.offset, s = c.gout.G === undefined ? 1 : Math.sign(toLocal(c, c.gout.G[0], c.gout.G[1])[0]);
  return [3.5 * s, -3.5 * s, 8 * s, -8 * s].map((u) => toWorld(c, u, o));
}
export function leadIn(c, [x, z]) {
  const [u] = toLocal(c, x, z), s = Math.sign(toLocal(c, c.gout.G[0], c.gout.G[1])[0]);
  return [[x, z], ...(u * s < 0 ? [toWorld(c, 0, c.gin.offset)] : []), c.gout.G, c.gout.O];
}
// points a map unit apart along a polyline of [x, z]
function line(ps) {
  const out = [ps[0]];
  for (let i = 0; i + 1 < ps.length; i++) {
    const [a, b] = [ps[i], ps[i + 1]], n = Math.max(1, Math.round(Math.hypot(b[0] - a[0], b[1] - a[1])));
    for (let k = 1; k <= n; k++) out.push([a[0] + (b[0] - a[0]) * k / n, a[1] + (b[1] - a[1]) * k / n]);
  }
  return out;
}
// a column from the castle's out gate to the rim: [x, z] of its hairpins (the gate first, the rim last) and its centre bearing
function column(c) {
  const P = TRAIL, [ox, oz] = c.gout.O, r0 = rhoAt(ox, oz), rO = Math.hypot(ox, oz);
  const a0 = TRAIL.lead, thc = Math.atan2(oz, ox) + c.dir * a0 / rO;
  const h = (rho) => profile(rho, thc);
  // the hairpins' radii: every leg climbs the same, at least legSep apart (the steep top gets its share by squeezing the legs below)
  const hTop = h(P.end), n = P.legs, rs = [r0];
  for (let j = 1; j < n; j++) {
    const want = c.floor + (hTop - c.floor) * j / n;
    let r = rs[j - 1] - P.legSep;
    while (r > P.end + (n - j) * P.legSep && h(r) < want) r -= 0.25;
    rs.push(Math.min(r, rs[j - 1] - P.legSep));
  }
  rs.push(P.end);
  // each leg as wide across as climbing it at the grade needs (W), shared between its two ends: each hairpin's offset from the line
  // is what its lower leg still needs past the hairpin below (the gate is a0 off the line), at least ampMin; the last leg ends on
  // the rim crest, off the line to its side as a hairpin would be (the steep top gets a whole leg's width)
  // (v2 round 4) plus the level stretch at each hairpin end of the leg
  const W = rs.slice(1).map((r, j) => { const dr = rs[j] - r, L = Math.abs(h(r) - h(rs[j])) / (c.grade ?? P.grade) + P.flat * ((j > 0 ? 1 : 0) + (j + 1 < n ? 1 : 0)); return Math.sqrt(Math.max(0, L * L - dr * dr)); });
  const hp = [[ox, oz]];
  let prevA = a0;
  for (let j = 1; j <= n; j++) {
    // (v2 round 4) the last leg ends TRAIL.endAmp of its width off the line (round 3: all of it, ampMax + 3): its rim end stood over
    // the hairpin two below, 26 higher and 13 off, and relax() tilted both legs' beds to fit the bank between them
    const A0 = Math.min(P.ampMax + (j === n ? 3 : 0), Math.max(P.ampMin, W[j - 1] - prevA)), A = j === n ? A0 * P.endAmp : A0, side = (j % 2 ? 1 : -1) * c.dir;
    const rr = rs[j] / scaleAt(thc);
    hp.push(pointAt(rs[j], thc + side * A / rr));
    prevA = A;
  }
  return { hairpins: hp, thc, rs };
}
// a higher castle beside a column, on the column's `side` (+1: its dir side) at its floor, as near the column as leaves its
// footprint (and its shelf's 3) 7 clear of the trail: its out gate faces the column and a branch runs level from it to the
// column's leg at that height
function extraAt(spec, c, col, [name, floor, side]) {
  const rr = rhoForFloor(col.thc, floor), rw = rr / scaleAt(col.thc);
  const path = line(col.hairpins);
  for (let off = 8; off < 70; off += 0.5) {
    const th = col.thc + side * c.dir * off / rw;
    // its trail runs toward the column: dir is the way from the castle to the column
    const e = castleAt(spec, name, floor, th, -side * c.dir);
    if (path.every(([x, z]) => footDist(e, x, z, 3) >= +(process.env.XCLR ?? 4))) return e;
  }
  throw new Error(`no room for ${name} beside the ${spec.name} column`);
}
function layTrail(spec) {
  const [name, floor, th, dir] = spec.castle;
  const c = castleAt(spec, name, floor, th, dir);
  c.grade = spec.grade;
  const col = column(c);
  const castles = [c], branches = [];
  if (spec.extra) { const e = extraAt(spec, c, col, spec.extra); castles.push(e); branches.push({ castle: e }); }
  return { ...spec, castles, col, branches };
}
// the trail's points [x, z, y], a point a map unit: through the castle at its floor; up the column from the castle's floor, through
// each hairpin at the flank's height on the column's line at its radius (never lower than the last plus 0.5), to the rim's height. A branch: through its
// castle at its floor, then level to the nearest point of the trail at that height
const BRANCH_GRADE = +(globalThis.process?.env?.BRANCH_GRADE ?? 0.3);
function trailPoints(t) {
  const pts = t.castles[0].run.map((p) => [p[0], p[1], t.castles[0].bed]), hairpins = [];
  t.castles[0].ptIndex = 0;
  const hp = t.col.hairpins, ys = [t.castles[0].bed];
  // a hairpin's height is the ground's on the column's centre line at its radius (the legs were sized on it, so each climbs as
  // planned; a hairpin out to the side is cut in or built out by the difference)
  const onLine = ([x, z]) => { const p = pointAt(rhoAt(x, z), t.col.thc); return islandBase(p[0], p[1]); };
  for (let j = 1; j < hp.length; j++) ys.push(Math.max(ys[j - 1] + 0.5, onLine(hp[j])));
  // (v2 round 2) the climb evened out: each interior hairpin's height moves `even` of the way from the flank's to the height an even
  // grade over the whole column gives it (the cliff band made the upper legs 0.6-0.8 and the first 0.13-0.24), and each leg eases
  // into and out of a hairpin (its grade there `ease` of its mean, steeper in its middle): two legs meet at a hairpin at 40-60
  // degrees, so near it the skin's 2.5-unit corners mix their heights, and the bank between them climbs at the grade over
  // sin(half the angle); round 2's first build stuck the trail-only walk at the third hairpin of three trails at 38 degrees
  const Ls = hp.slice(1).map((p, j) => Math.hypot(p[0] - hp[j][0], p[1] - hp[j][1])), Ltot = Ls.reduce((a, b) => a + b, 0);
  for (let j = 1, acc = 0; j + 1 < hp.length; j++) { acc += Ls[j - 1]; ys[j] += TRAIL.even * (ys[0] + (ys[ys.length - 1] - ys[0]) * acc / Ltot - ys[j]); }
  for (let j = 1; j < ys.length; j++) ys[j] = Math.max(ys[j], ys[j - 1] + 0.5);
  // the eased profile of a leg: f(0) = 0, f(1) = 1, slope `ease` at an end that is a hairpin, the rest in the middle (a quadratic
  // ease at each such end over a fifth of the leg either side)
  const prof = (t, e0, e1) => {
    const a = e0 ? TRAIL.ease : 1, b = e1 ? TRAIL.ease : 1, q = 0.2;
    // piecewise: slope a over [0, q] rising linearly to c, c over [q, 1 - q], falling to b over [1 - q, 1]; c from the area being 1
    const c = (1 - q * (a + b) / 2) / (1 - q);
    if (t < q) return a * t + (c - a) * t * t / (2 * q);
    const y1 = q * (a + c) / 2;
    if (t <= 1 - q) return y1 + c * (t - q);
    const u = t - (1 - q);
    return y1 + c * (1 - 2 * q) + c * u + (b - c) * u * u / (2 * q);
  };
  // (v2 round 4) a leg is level for TRAIL.flat from each hairpin end (the landing), then eases into its climb
  const pinAt = [];          // the index in pts of each interior hairpin
  for (let j = 0; j + 1 < hp.length; j++) {
    const [ax, az] = hp[j], [bx, bz] = hp[j + 1], m = Math.max(1, Math.round(Math.hypot(bx - ax, bz - az)));
    const f0 = j > 0 ? Math.min(0.3, TRAIL.flat / m) : 0, f1 = j + 2 < hp.length ? Math.min(0.3, TRAIL.flat / m) : 0;
    if (j > 0) { hairpins.push([ax, az, ys[j]]); pinAt.push(pts.length - 1); }
    for (let w = 1; w <= m; w++) {
      const u = Math.min(1, Math.max(0, (w / m - f0) / (1 - f0 - f1)));
      pts.push([ax + (bx - ax) * w / m, az + (bz - az) * w / m, ys[j] + (ys[j + 1] - ys[j]) * prof(u, j > 0, j + 2 < hp.length)]);
    }
  }
  // the landing: each point's bed is TRAIL.land - TRAIL.bed wider (p[3]) within TRAIL.flat of a hairpin along the trail, tapering over landTaper
  const extra = TRAIL.land - TRAIL.bed;
  for (const [i, p] of pts.entries()) {
    const k = Math.min(Infinity, ...pinAt.map((q) => Math.abs(q - i)));
    p[3] = Math.round(extra * (1 - smooth(TRAIL.flat, TRAIL.flat + TRAIL.landTaper, k)) * 100) / 100;
  }
  const branches = t.branches.map((b) => {
    const e = b.castle, start = e.run.map((p) => [p[0], p[1], e.bed]);
    // the trail's point at the castle's floor nearest its gate
    const [gx, gz] = e.gout.O;
    let best = null, bd = 1e9;
    // (v2 round 4) the nearest point the link can reach at BRANCH_GRADE or less (a height difference over that costs 6 a unit): round 3's
    // level-only choice ran the SW branch 58 units at 26.5 through the V of the SW trail's second hairpin and the NE branch 63 units
    // along a causeway up to 19 over the flank
    for (const p of pts) { const h = Math.hypot(p[0] - gx, p[1] - gz), d = h + 6 * Math.max(0, Math.abs(p[2] - e.bed) - BRANCH_GRADE * h); if (d < bd) { bd = d; best = p; } }
    const link = line([e.gout.O, [best[0], best[1]]]).slice(1).map((p, i, a) => [p[0], p[1], e.bed + (best[2] - e.bed) * (i + 1) / a.length]);
    return { ...b, pts: [...start, ...link], joins: best };
  });
  return { pts, hairpins, branches, pinAt };
}
// The cliff bands are notched where a trail's column crosses them (their step spread over a wider band, so the legs can climb it
// near the grade): the bearings are the columns' own, from a first layout.
NOTCHES = LAYOUT.map(layTrail).flatMap((t) => CLIFFS.map((c, k) => ({ k, th: t.col.thc })));
const LAID = LAYOUT.map(layTrail);
// the rim is lowered where a trail ends on it (islandBase), so the trail ends are known before the heights are taken
TRAIL_ENDS = LAID.map((t) => t.col.hairpins[t.col.hairpins.length - 1]);
export const TRAILS = LAID.map((t) => { const { pts, hairpins, branches, pinAt } = trailPoints(t); return { ...t, pts, hairpins, branches, pinAt, end: pts[pts.length - 1] }; });
export const ROADS = TRAILS;
export const CASTLES = TRAILS.flatMap((r) => r.castles);
export const notches = () => NOTCHES;
// every polyline whose bed is cut into the flank: the branches first, then the trails (a later bed wins where they meet, and a
// branch runs level beside its trail to the junction)
const BEDS = [...TRAILS.flatMap((t) => t.branches.map((b) => b.pts)), ...TRAILS.map((t) => t.pts)];

// (v2 round 4) the V of every hairpin: the hairpin H, the unit vectors along its lower leg (back down, ua) and upper leg (on up, ub),
// their lengths, and the upper leg's points from H (one a map unit), for bermAt
const VEES = TRAILS.flatMap((t) => t.pinAt.map((iH, k) => {
  const iA = k ? t.pinAt[k - 1] : 0, iB = t.pinAt[k + 1] ?? t.pts.length - 1, H = t.pts[iH], A = t.pts[iA], B = t.pts[iB];
  const unit = (P) => { const L = Math.hypot(P[0] - H[0], P[1] - H[1]); return [(P[0] - H[0]) / L, (P[1] - H[1]) / L, L]; };
  const [uax, uaz, lenA] = unit(A), [ubx, ubz, lenB] = unit(B);
  return { H, ua: [uax, uaz], ub: [ubx, ubz], lenA, lenB, up: t.pts.slice(iH, iB + 1), down: t.pts.slice(iA, iH + 1).reverse() };
}));
// the berm's height at (x, z), or -1e9 outside every V: inside the wedge between a hairpin's legs (both coordinates along them
// positive), where the two centrelines are less than BERM.gap apart, the upper leg's height at the point's place along it, blended
// into the ground `h` over the last BERM.fade of the gap
function bermAt(x, z, h) {
  let out = -1e9;
  for (const V of VEES) {
    const vx = x - V.H[0], vz = z - V.H[1];
    if (Math.abs(vx) > 45 || Math.abs(vz) > 45) continue;
    const det = V.ua[0] * V.ub[1] - V.ua[1] * V.ub[0];
    if (Math.abs(det) < 0.05) continue;
    const a = (vx * V.ub[1] - vz * V.ub[0]) / det, b = (V.ua[0] * vz - V.ua[1] * vx) / det;
    if (a <= 0 || b <= 0) continue;
    const sa = vx * V.ua[0] + vz * V.ua[1], sb = vx * V.ub[0] + vz * V.ub[1];
    if (sa > V.lenA || sb > V.lenB - 2) continue;
    const da = Math.abs(vx * V.ua[1] - vz * V.ua[0]), gap = da + Math.abs(vx * V.ub[1] - vz * V.ub[0]);
    if (gap >= BERM.gap) continue;
    const lo = V.down[Math.min(V.down.length - 1, Math.max(0, Math.round(sa)))];
    const y = Math.min(V.up[Math.min(V.up.length - 1, Math.max(0, Math.round(sb)))][2], lo[2] + BERM.bank * Math.max(0, da - TRAIL.bed - (lo[3] ?? 0) - BERM.foot));
    out = Math.max(out, h + (y - h) * (1 - smooth(BERM.gap - BERM.fade, BERM.gap, gap)));
  }
  return out;
}

// a trail and its branches as a mask expression (above 0 within `hw` of a line; the distance field is exact, so the edge is as
// smooth as the material grid allows): each polyline is cut to a segment every `every` points (and at every hairpin: a leg is straight)
export function roadMask(rd, hw = 2.7, every = 7) {
  const f = (v) => Number(v.toFixed(1));
  const segs = [];
  for (const line of [rd.pts, ...(rd.branches ?? []).map((b) => b.pts)]) {
    const keep = new Set((rd.hairpins ?? []).map((h) => `${f(h[0])},${f(h[1])}`));
    const pts = line.filter((p, i, a) => i % every === 0 || i === a.length - 1 || keep.has(`${f(p[0])},${f(p[1])}`));
    for (let i = 0; i + 1 < pts.length; i++) segs.push(`sd(${f(pts[i][0])}, ${f(pts[i][1])}, ${f(pts[i + 1][0])}, ${f(pts[i + 1][1])})`);
  }
  // (v2 round 4) each hairpin's landing: a disc TRAIL.land - TRAIL.bed wider than the bed
  for (const h of rd.hairpins ?? []) segs.push(`(hypot(x - ${f(h[0])}, z - ${f(h[1])}) - ${f(TRAIL.land - TRAIL.bed)})`);
  const defs = "fn tt(ax, az, bx, bz) = clamp(((x - ax) * (bx - ax) + (z - az) * (bz - az)) / max(0.0001, (bx - ax) ^ 2 + (bz - az) ^ 2), 0, 1); " +
    "fn sd(ax, az, bx, bz) = hypot(x - (ax + (bx - ax) * tt(ax, az, bx, bz)), z - (az + (bz - az) * tt(ax, az, bx, bz))); ";
  return `${defs}${hw} - ${segs.reduce((acc, c) => (acc ? `min(${acc}, ${c})` : c), "")}`;
}

// the kerbs' lines (v2 round 3: "a darker kerb line on the downhill edge so each zigzag leg shows as a stripe"): along every bed,
// the line `off` to the side whose ground (islandBase, before the cut) lies under the bed, as [x, z] polylines for a material
// rule's `paths` (at most 64, of 2-512 points each); none where the side changes (a hairpin's tip, a level stretch), inside a
// castle or its gates, or at a trail's rim end
export function kerbLines(off = (KERB.from + KERB.to) / 2) {
  const out = [];
  for (const pl of BEDS) {
    let cur = [], curSide = 0;
    const flush = () => { for (let i = 0; i + 1 < cur.length; i += 500) out.push(cur.slice(i, i + 512)); cur = []; curSide = 0; };
    for (let i = 0; i < pl.length; i++) {
      const a = pl[Math.max(0, i - 1)], b = pl[Math.min(pl.length - 1, i + 1)], p = pl[i];
      const L = Math.hypot(b[0] - a[0], b[1] - a[1]) || 1, nx = -(b[1] - a[1]) / L, nz = (b[0] - a[0]) / L;
      let side = 0, drop = 0.4;
      const o = off + (p[3] ?? 0);
      for (const sg of [-1, 1]) { const d = p[2] - islandBase(p[0] + sg * nx * o, p[1] + sg * nz * o); if (d > drop) { drop = d; side = sg; } }
      const q = [p[0] + side * nx * o, p[1] + side * nz * o];
      const ok = side !== 0 && !TRAIL_ENDS.some((e) => Math.hypot(q[0] - e[0], q[1] - e[1]) < 3.5) &&
        !CASTLES.some((k) => Math.abs(q[0] - k.at[0]) < 40 && Math.abs(q[1] - k.at[1]) < 40 && footDist(k, q[0], q[1], 0) < 1.5);
      if (!ok || (curSide && side !== curSide)) flush();
      if (ok) { cur.push([Math.round(q[0] * 10) / 10, Math.round(q[1] * 10) / 10]); curSide = side; }
    }
    flush();
  }
  return out.slice(0, 64);
}

// ---- the height of the island at (x, z), map units above the sea's zero (negative: the sea floor, down to SEA_FLOOR) ----
// islandBase: the ground before the castles' shelves and the trails' beds are cut into it (the trails' hairpins take its height)
export function islandBase(x, z) {
  const th = Math.atan2(z, x), r = Math.hypot(x, z), rho = r * scaleAt(th);
  // the cone: ridges grow with the radius from the rim; gullies are cut under the lava tongues
  const ridges = 1 + (0.1 * Math.cos(7 * th + 1.5 * noise(x / 40, z / 40, 9)) + 0.04 * Math.cos(13 * th + 2 * noise(x / 25, z / 25, 19))) * smooth(RC + 2, RC + 22, rho) * smooth(8, 24, profile(rho, th));
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
  // the rim: spurs of varied height (never a regular row of teeth), lower where a trail reaches it
  let roadMaskH = 0;
  for (const e of TRAIL_ENDS) roadMaskH = Math.max(roadMaskH, Math.exp(-((angd(th, Math.atan2(e[1], e[0])) / 0.2) ** 2)));
  const spur = 3 + Math.max(0, noise(Math.cos(th) * 3.3 + 4, Math.sin(th) * 3.3 + 4, 17) + 0.2) * 7 * (1 - roadMaskH);
  const crest = spur * Math.exp(-(((rho - RC) / 3.6) ** 2));
  // (v2 round 3) ribs and runnels down the steep upper cone: noise stretched along the fall line (about 7 across, 22 down), up to
  // 2.4 high, from just outside the rim to the foothills, so the steep faces between the trails are fluted rock, not flat slabs
  const ribs = RIBS * fbm(th * r / 7, rho / 22, 91, 2) * smooth(RC + 2, RC + 7, rho) * (1 - smooth(62, 76, rho));
  const outer = ledged(profile(Math.max(rho, RC), th) * ridges - dip + ribs, x, z, rho) + crest;
  const crestH = profile(RC, th, false) + 6;      // the inner rim is raised so the bowl has walls (iteration 10)
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
  return h;
}
// `pin` (an object) gets pin = true where the column is a kerb or a castle's court, which relax() keeps (round 3: not a bed: a pinned bed
// makes relax() put a cut wall's whole excess into the wall, 2.9 a unit from the bed's edge, and the skin's 2.5-unit corners then tilt
// the bed across: round 2, the NW trail)
const BED_PIN = !!+(globalThis.process?.env?.BED_PIN ?? 1);   // (v2 round 4) BED_PIN=0: the bed unpinned, as in round 3
export function island(x, z, pin = null) {
  let h = islandBase(x, z);
  // shelves under the castles (cut and fill to the castle's floor), roads cut and filled into the flank
  let wShelf = 0, wRoad = 0;
  for (const k of CASTLES) {
    if (Math.abs(x - k.at[0]) > 40 || Math.abs(z - k.at[1]) > 40) continue;
    const d = footDist(k, x, z, 3);
    if (d > 6) continue;
    const w = smooth(6, 0, d);
    h += (k.bed - h) * w;
    wShelf = Math.max(wShelf, w);
  }
  // (v2 round 4) the berm inside each hairpin's V, raised to the upper leg's height (BERM)
  if (BERM.gap > 0) h = Math.max(h, bermAt(x, z, h));
  const hCut = h;
  let dMin = 1e9, yMin = 0, kerb = -1e9, kerbD = 1e9;
  // a kerb point: off a bed by d, whose height y is over the ground here (the ground falls away from it); every leg that passes
  // counts, not only the nearest, so the bank between two legs has the upper leg's kerb too (the nearest is the lower leg there)
  const kerbOf = (d, y) => hCut + (y + KERB.h * smooth(TRAIL.bed + TRAIL.shoulder, KERB.from, d) * (KERB.berm > KERB.to ? 1 - smooth(KERB.to, KERB.berm, d) : 1) - hCut) * (1 - smooth(KERB.berm, KERB.out, d));
  // (v2 round 3) a leg's kerb keeps KERB.clear off another leg's bed (a bed point over 1.5 higher or lower within bed + clear of the
  // column): the level shelf beside a hairpin's tip otherwise lifts the skin's corners on the other leg's edge, a 1.2 step on the bed
  // (the trail-only walk stuck at the SE and SW third hairpins)
  const cands = [], near = [];
  for (const pl of BEDS) {
    let best = 1e9, by = 0;
    // (v2 round 4) a landing point's bed is p[3] wider: its distance is taken from that wider edge
    const ds = pl.map((p) => (Math.abs(x - p[0]) > 14 || Math.abs(z - p[1]) > 14 ? Infinity : Math.hypot(x - p[0], z - p[1]) - (p[3] ?? 0)));
    // (v2 round 4) the height at a point's foot on the polyline (the segment to the nearer neighbour, its height and widening
    // interpolated), not the nearest point's own: round 3's beds and kerbs were stairs of a map unit's tread (one riser of the leg's
    // climb per point; check_trail's walk met them as rises of 0.5-0.7 over half a unit)
    const foot = (i) => {
      const p = pl[i], q = pl[(ds[i - 1] ?? Infinity) < (ds[i + 1] ?? Infinity) ? i - 1 : i + 1];
      if (!q) return [ds[i], p[2]];
      const ex = q[0] - p[0], ez = q[1] - p[1], L2 = ex * ex + ez * ez || 1, t = Math.min(1, Math.max(0, ((x - p[0]) * ex + (z - p[1]) * ez) / L2));
      const w = (p[3] ?? 0) + ((q[3] ?? 0) - (p[3] ?? 0)) * t;
      return [Math.hypot(x - p[0] - ex * t, z - p[1] - ez * t) - w, p[2] + (q[2] - p[2]) * t];
    };
    for (const [i, p0] of pl.entries()) {
      if (ds[i] === Infinity) continue;
      const [dd, py] = foot(i), p = [p0[0], p0[1], py];
      if (dd < best) { best = dd; by = py; }
      // (v2 round 3) a kerb from the nearest point of each leg that passes (a local minimum of the distance along the polyline) only:
      // round 2 took every point within reach, so a column took the kerb of a point several units up the leg, as high as that point
      // (an uneven kerb, higher and lower column by column: part of the sawtooth)
      if (!(ds[i] <= (ds[i - 1] ?? Infinity) && ds[i] <= (ds[i + 1] ?? Infinity))) continue;
      if (dd < TRAIL.bed + TRAIL.shoulder + KERB.clear) near.push(p[2]);
      if (dd > TRAIL.bed && dd < KERB.out && (KERB.anySide || hCut < p[2] + 0.5)) cands.push([kerbOf(dd, p[2]), dd, p[2]]);
    }
    if (best < 7 + TRAIL.shoulder) { const w = 1 - smooth(TRAIL.bed + TRAIL.shoulder, TRAIL.bed + TRAIL.shoulder + 3.2, best); h += (by - h) * w; wRoad = Math.max(wRoad, w); }
    if (best < dMin) { dMin = best; yMin = by; }
  }
  for (const [k, dd, y] of cands) if (k > kerb && !near.some((q) => Math.abs(q - y) > 1.5)) { kerb = k; kerbD = dd; }
  // fine roughness where nothing was cut
  h += 1.0 * fbm(x / 8, z / 8, 21, 2) * smooth(3, 14, h) * (1 - wShelf) * (1 - wRoad);
  // the kerb on the downhill side of the beds (none on a bed itself)
  let kerbed = false;
  if (KERB.h > 0 && dMin > TRAIL.bed && kerb > -1e9 && !TRAIL_ENDS.some((e) => Math.hypot(x - e[0], z - e[1]) < 3.5) &&
      !CASTLES.some((k) => Math.abs(x - k.at[0]) < 40 && Math.abs(z - k.at[1]) < 40 && footDist(k, x, z, 0) < 1.5) && (!KERB.anySide || kerb > h)) {
    h = Math.max(h, kerb);
    kerbed = true;
  }
  if (pin) {
    // (v2 round 4) and the bed with its level shoulder: relax() lifted the bed's uphill edge by up to 8 under a tall cut (each column
    // of a step steeper than its limit took half the excess), tilting the bed across; pinned, the cut bank takes it all
    pin.pin = (kerbed && kerbD <= KERB.to) || (BED_PIN && dMin <= TRAIL.bed + TRAIL.shoulder) || CASTLES.some((k) => Math.abs(x - k.at[0]) < 40 && Math.abs(z - k.at[1]) < 40 && footDist(k, x, z, 0) < 1);
    pin.soft = kerbed && !pin.pin ? 1 : kerbed ? 2 : 0;
  }
  return Math.max(SEA_FLOOR, h);
}
// the heights above SEA_FLOOR, one row per z, one column per x (the drape call clips them to the circle CLIP)
// (rows.pins: the same rows of island()'s pin, for relax)
export function gridOf(min = -BOX, max = BOX, step = 1) {
  const rows = [], pins = [], softs = [], q = { pin: false, soft: 0 };
  for (let z = min; z <= max + 1e-6; z += step) {
    const row = [], pr = [], sr = [];
    for (let x = min; x <= max + 1e-6; x += step) { row.push(Math.round((island(x, z, q) - SEA_FLOOR) * 100) / 100); pr.push(q.pin); sr.push(q.soft); }
    rows.push(row); pins.push(pr); softs.push(sr);
  }
  rows.pins = pins; rows.softs = softs;
  return rows;
}
// the kerbs softened (v2 round 3): each kerb column (rows.softs) takes the 3 x 3 mean of the grid round it, `passes` times, so the
// kerb is a rounded ridge the skin's 2.5-unit corners sample evenly along a leg instead of a 1-unit step they catch or miss
export function soften(rows, passes = +(globalThis.process?.env?.SOFTEN ?? 2)) {
  const n = rows.length, m = rows[0].length, sf = rows.softs;
  if (!sf) return rows;
  for (let p = 0; p < passes; p++) {
    const src = rows.map((r) => r.slice());
    for (let j = 1; j + 1 < n; j++) for (let i = 1; i + 1 < m; i++) {
      if (!sf[j][i]) continue;
      let sum = 0;
      for (let dj = -1; dj <= 1; dj++) for (let di = -1; di <= 1; di++) sum += src[j + dj][i + di];
      rows[j][i] = Math.round(sum / 9 * 100) / 100;
    }
  }
  return rows;
}
// no step between neighbouring columns steeper than `limit` map units a unit (about 71 degrees at 2.9): a few passes of thermal
// relaxation over a grid made by gridOf, so the drape skin never folds (its blocks are only 3 voxels thick)
const RELAX_SHARE = !!+(globalThis.process?.env?.RELAX_SHARE ?? 1);
// (v2 round 4) the pinned passes run until nothing moves (at most `passes`), then unpinned passes until no step is over the limit:
// two pinned columns a column or two apart and more than twice the limit apart in height (a bed beside another leg's kerb, a court
// beside a cut) can never both hold, and round 3's 60 passes left about 2 400 steps over the limit, up to 17 high (skin cells sheared
// across themselves: the kind check_trail names as mine-leak suspects). RELAX_FINAL=0 skips the unpinned passes.
const RELAX_FINAL = !!+(globalThis.process?.env?.RELAX_FINAL ?? 1);
export function relax(rows, limit = 2.9, passes = +(globalThis.process?.env?.RELAX_PASSES ?? 400)) {
  const n = rows.length, m = rows[0].length;
  const sweep = (pins) => {
    let moved = 0;
    for (let j = 0; j < n; j++) for (let i = 0; i < m; i++) {
      for (const [dj, di] of [[0, 1], [1, 0]]) {
        const jj = j + dj, ii = i + di;
        if (jj >= n || ii >= m) continue;
        const d = rows[j][i] - rows[jj][ii];
        if (Math.abs(d) <= limit + 1e-9) continue;
        // a pinned column (a kerb or a court: rows.pins) keeps its height and the other takes the whole excess (two
        // pinned columns share it, as before: between two legs the bank gives, so the skin never folds)
        // (v2 round 4) two pinned columns: the higher gives it all (the lower leg's bed in a hairpin's V stays level under the upper
        // leg's edge; RELAX_SHARE=1: both give half, as before)
        let pa = pins?.[j][i], pb = pins?.[jj][ii];
        if (pa && pb) { if (RELAX_SHARE) pa = pb = false; else if (d > 0) pa = false; else pb = false; }
        const k = (Math.abs(d) - limit) * Math.sign(d) * (pa || pb ? 1 : 0.5);
        if (!pa) rows[j][i] -= k;
        if (!pb) rows[jj][ii] += k;
        moved++;
      }
    }
    return moved;
  };
  for (let p = 0; p < passes; p++) if (!sweep(rows.pins)) break;
  if (RELAX_FINAL) for (let p = 0; p < 4000; p++) if (!sweep(null)) break;
  for (const r of rows) for (let i = 0; i < m; i++) r[i] = Math.round(r[i] * 100) / 100;
  return rows;
}
