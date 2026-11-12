// Dustcrown's detail layer, rounds 1 and 2: the passes detail.mjs runs after build.mjs. Every one writes only through erg-mcp's
// sim-neutral channels (scenery copies: place_mod_mesh of the one mesh bank in meshes/bank.mjs, copy_base_details of the
// ARABIAN base's own props), so sim_fingerprint cannot move; detail.mjs proves it after each pass. Units are map units.
//
// What reads where (DESIGN.md 2.2, 2.3): the pads and the four walks in from them stay clear (scenery has no collision, but
// a worm that walks through a boulder reads as a bug), so does a ring round the palace's foot (no stair of rocks up a riser:
// round 4), the boulder fields keep their own pieces, and nothing stands in the sea. The dressing is graded (a big block,
// a ring of middling ones, litter), seated on the real surface by surface_sample, then lint_contact / lint_support take out
// what hangs off a steep face.
import path from "node:path";
import { fileURLToPath } from "node:url";
import { MOD } from "./meshes/bank.mjs";
const HERE = path.dirname(fileURLToPath(import.meta.url));

// ---- the map's layout, as build.mjs sets it out (copied: build.mjs is a script, not a module) ----
export const TAG = "dtl1";                                  // the layer's own name tag (build.mjs's boulder meshes are _dtl)
export const PALACE_TAG = "pal1";                           // the palace's: its pieces stand on vertical faces, which the contact lint would remove
const PAD_R = 86;
export const PADS = [[-PAD_R, 0], [PAD_R, 0], [0, -PAD_R], [0, PAD_R]];
export const MESAS = [                                       // [name, x, z, radius (the bigger one), refuge, top]
  ["refuge NW", -60, -60, 15, true, 25], ["refuge SE", 60, 60, 15, true, 25],
  ["low NE", 56, -58, 11, false, 11.5], ["low SW", -56, 58, 11, false, 11.5], ["low W", -74, -30, 10, false, 11.5], ["low E", 74, 30, 10, false, 11.5],
];
export const FIELDS = [[-66, 22], [66, -22], [-24, -68], [24, 68], [36, -72], [-36, 72]];
const COAST = 93;                                           // the dunes slope under the sea from about here

export const mulberry = (seed) => { let a = seed >>> 0; return () => { a = (a + 0x6d2b79f5) >>> 0; let t = a; t = Math.imul(t ^ (t >>> 15), t | 1); t ^= t + Math.imul(t ^ (t >>> 7), t | 61); return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }; };
const r1 = (v) => Math.round(v * 10) / 10, r2 = (v) => Math.round(v * 100) / 100;
const DEG = 180 / Math.PI;

// The first reason a prop of `radius` may not stand at (x, z), or null. `open` means the open desert: no mesa, no field's
// own ground, and `onMesa` lets a pass that dresses a mesa through its own footprint.
export function keepOut(x, z, radius = 0, { mesa = true, field = true } = {}) {
  const r = Math.hypot(x, z);
  if (r + radius > COAST) return "coast";
  if (Math.max(Math.abs(x), Math.abs(z)) < 53 + radius) return "palace ring";
  for (const [px, pz] of PADS) if (Math.hypot(x - px, z - pz) < 14 + radius) return "pad";
  // the walks in from each pad to the palace's foot run straight along an axis: 5 each side
  if ((Math.abs(z) < 5 + radius && Math.abs(x) > 40) || (Math.abs(x) < 5 + radius && Math.abs(z) > 40)) return "walk";
  if (mesa) for (const [, mx, mz, mr] of MESAS) if (Math.hypot(x - mx, z - mz) < mr + 4 + radius) return "mesa";
  if (field) for (const [fx, fz] of FIELDS) if (Math.hypot(x - fx, z - fz) < 9 + radius) return "field";
  return null;
}

// Surface samples (the voxels' top, slope, normal) for up to any number of points, 4000 a call.
export async function sample(call, pts) {
  const out = [];
  for (let i = 0; i < pts.length; i += 4000) {
    const r = await call("surface_sample", { points: pts.slice(i, i + 4000), units: "map" });
    out.push(...r.json.samples);
  }
  return out;
}

// One place_mod_mesh call per mesh (256 pieces a call); `byMesh` is {Name: [{pos | xz, rot, scale}]}.
async function put(ctx, byMesh, tag = TAG) {
  let placed = 0;
  for (const [name, pieces] of Object.entries(byMesh)) {
    for (let i = 0; i < pieces.length; i += 250) {
      const r = await ctx.call("place_mod_mesh", { resource: `${MOD}.${name}`, box: ctx.boxes[name], units: "map", rotUnits: "degrees", tag, pieces: pieces.slice(i, i + 250) });
      placed += r.json.placed ?? 0;
    }
  }
  return placed;
}
const piece = (xz, sc, rnd, o = {}) => ({ xz: [r1(xz[0]), r1(xz[1])], rot: o.rot ?? [0, Math.round(rnd() * 360), 0], scale: Array.isArray(sc) ? sc.map(r2) : [r2(sc), r2(sc * (0.85 + rnd() * 0.25)), r2(sc)] });

// ---- pass: palette (round 2). The palace and the rock in three desaturated sandstone tones instead of one saturated lemon
// (tint_landscape, a visual channel: each texture's mean becomes the colour, given as the colour to SEE). The colours below are
// the ones ASKED, calibrated in the game (Sunstone 2.4.0, DAY): the stone asks #e0bc98 to play the ochre #cb9a5a (asking #c89a5e
// played an orange #bb6221, asking #d8c8a8 a khaki #c3ab6d; the grade file's fit, fitted on sand, extrapolates badly here).
// Warm ochre for the walls (the stone, which the mesas share), a burnt sienna
// for the stepped base and the lower courses (sandy plaster), pale bleached limestone for the caps and merlons (the gold,
// its edging a shade darker and matte), a muted copper green for the blue mosaic (the Dome meshes cover the domes; this
// takes any edge that shows), a warm tan for the courts' paving, a rust for the mesas' strata (the dark yellow plaster). ----
export const PALACE_TINTS = [
  { material: "stone", faces: "all", color: "#e0bc98" }, { material: "sandy-plaster", faces: "all", color: "#a88058" },
  { material: "Gold", faces: "all", color: "#e2d2b0" }, { material: "GoldEdging", faces: "all", color: "#b09468" },
  { material: "BlueMosaic1", faces: "all", color: "#6f8f7a" }, { material: "StarMosaic", faces: "all", color: "#b08a5a" },
  { material: "darkyellowplaster", faces: "all", color: "#8e5a34" },
];
export async function palettePass(ctx) {
  const r = await ctx.call("tint_landscape", { theme: "ARABIAN", carrier: "theme", colorSpace: "srgb", tints: PALACE_TINTS, grade: path.join(HERE, "grade-sunstone-day-shade.json") });
  return { tints: (r.json?.tints ?? []).map((t) => `${t.from}: ${t.color}`) };
}

// ---- pass: mesas. A graded tumble at each mesa's foot (a block, a ring of middling stones, litter) and, on the refuges'
// tops, a few low boulders along the rim: the tops are where a worm stands, so they are small and keep the middle clear. ----
export async function mesaPass(ctx) {
  const rnd = mulberry(1101), byMesh = {};
  const add = (name, p) => (byMesh[name] ??= []).push(p);
  let tumbles = 0, tops = 0;
  for (const [name, mx, mz, R, refuge, top] of MESAS) {
    // candidates: a fan round the mesa, on ground that is not the cliff (a refuge's talus stands to 10, an outcrop's apron to ~8)
    const cand = [];
    for (let a = 0; a < 360; a += 10) for (let d = R + 0.5; d <= R + 11; d += 1.5) cand.push([mx + Math.cos(a / DEG) * d, mz + Math.sin(a / DEG) * d, a, d]);
    const s = await sample(ctx.call, cand.map((c) => [c[0], c[1]]));
    const ok = cand.filter((c, i) => s[i] && s[i].slope < 30 && s[i].y < (refuge ? 11.5 : 9) && !keepOut(c[0], c[1], 1.5, { mesa: false, field: false }));
    // the anchors: the refuges get 5 tumbles, the outcrops 3, spread round (the angle's bucket), nearest the foot
    const n = refuge ? 5 : 3, used = [];
    for (let k = 0; k < n; k++) {
      const wedge = ok.filter((c) => ((c[2] - (k * 360) / n - rnd() * 20 + 720) % 360) < 360 / n && !used.some((u) => Math.hypot(u[0] - c[0], u[1] - c[1]) < 8));
      if (!wedge.length) continue;
      wedge.sort((a, b) => a[3] - b[3]);
      const [ax, az] = wedge[Math.floor(rnd() * Math.min(3, wedge.length))];
      used.push([ax, az]); tumbles++;
      // graded: one block, two middling beside it, a heap and two small stones in the lee
      add("Boulder", piece([ax, az], refuge ? 1.5 + rnd() * 0.4 : 1.1 + rnd() * 0.3, rnd));
      for (let i = 0; i < 2; i++) { const a = rnd() * 6.28, d = 2.2 + rnd() * 1.2; add("Boulder", piece([ax + Math.cos(a) * d, az + Math.sin(a) * d], 0.8 + rnd() * 0.3, rnd)); }
      { const a = rnd() * 6.28; add("Heap", piece([ax + Math.cos(a) * 3.6, az + Math.sin(a) * 3.6], 0.9 + rnd() * 0.5, rnd)); }
      for (let i = 0; i < 2; i++) { const a = rnd() * 6.28, d = 3 + rnd() * 2.5; add("Boulder", piece([ax + Math.cos(a) * d, az + Math.sin(a) * d], 0.38 + rnd() * 0.2, rnd)); }
    }
    if (refuge) {
      // the top: boulders along the rim ring (0.78 R, not the middle), on ground that is flat and at the top's height
      const rim = [];
      for (let a = 0; a < 360; a += 15) rim.push([mx + Math.cos(a / DEG) * R * 0.74, mz + Math.sin(a / DEG) * R * 0.62]);
      const t = await sample(ctx.call, rim);
      const flat = rim.filter((p, i) => t[i] && t[i].slope < 12 && Math.abs(t[i].y - top) < 1.2);
      for (let i = 0; i < flat.length; i += 3) { add(i % 2 ? "Heap" : "Boulder", piece(flat[i], 0.5 + rnd() * 0.35, rnd)); tops++; }
    }
  }
  const placed = await put(ctx, byMesh);
  return { tumbles, tops, placed };
}

// ---- pass: hoodoos. Wind-carved groups in the open desert, where the dunes are gentle and well apart: a balanced rock (a
// Boulder stretched into a stem, a flat Boulder for its cap, wider than the stem), a plain pillar, a leaning block and a heap.
// The stem and the cap are the one bank mesh; the cap stands by `pos` on the stem's top, the rest are dropped. ----
export async function hoodooPass(ctx) {
  const rnd = mulberry(2202), byMesh = {};
  const add = (name, p) => (byMesh[name] ??= []).push(p);
  const cand = [];
  for (let x = -92; x <= 92; x += 4) for (let z = -92; z <= 92; z += 4) if (!keepOut(x, z, 5)) cand.push([x, z]);
  const s = await sample(ctx.call, cand);
  const ok = cand.map((c, i) => [c[0], c[1], s[i]?.y, s[i]?.slope]).filter((c) => c[3] < 16 && c[2] < 9.5 && c[2] > 3.5);
  // greedy far-apart pick: 8 groups at least 34 apart (the order is seeded, so deterministic)
  const picks = [];
  const pool = ok.map((c) => ({ c, k: rnd() })).sort((a, b) => a.k - b.k).map((o) => o.c);
  for (const c of pool) { if (picks.length >= 8) break; if (picks.every((p) => Math.hypot(p[0] - c[0], p[1] - c[1]) >= 34)) picks.push(c); }
  const lean = () => r1((rnd() - 0.5) * 3);
  for (const [gx, gz, gy] of picks) {
    const yaw = rnd() * 360;
    // the balanced rock: a stem 1.5 wide and about 3.6 high, its cap 3 wide, 1 thick, sunk a little into the stem's top
    const h = 1.9 + rnd() * 0.4, stemTop = 1.66 * h;
    add("Boulder", { xz: [r1(gx), r1(gz)], rot: [lean(), r1(yaw), lean()], scale: [0.5, r2(h), 0.45] });
    add("Boulder", { pos: [r1(gx), r2(gy + stemTop - 0.3), r1(gz)], rot: [lean(), r1(yaw + 40), lean()], scale: [r2(0.95 + rnd() * 0.2), 0.5, r2(0.9 + rnd() * 0.2)] });
    // a plain pillar and a leaning block beside it
    { const a = yaw / DEG + 1.5, d = 3.4 + rnd(); add("Boulder", { xz: [r1(gx + Math.cos(a) * d), r1(gz + Math.sin(a) * d)], rot: [lean(), r1(rnd() * 360), lean()], scale: [0.42, r2(1.1 + rnd() * 0.5), 0.4] }); }
    { const a = yaw / DEG + 3.6, d = 3.2 + rnd(); add("Boulder", { xz: [r1(gx + Math.cos(a) * d), r1(gz + Math.sin(a) * d)], rot: [r1(8 + rnd() * 8), r1(rnd() * 360), 0], scale: [0.85, 0.7, 0.8] }); }
    { const a = yaw / DEG + 5.0, d = 2.8 + rnd(); add("Heap", piece([gx + Math.cos(a) * d, gz + Math.sin(a) * d], 0.9 + rnd() * 0.4, rnd)); }
  }
  const placed = await put(ctx, byMesh);
  ctx.hoodoos = picks.map((p) => [p[0], p[1]]);
  return { groups: picks.length, picks: picks.map((p) => [p[0], p[1]]), placed };
}

// ---- pass: litter. Loose boulders and heaps in the open desert, in pairs and threes (one alone reads as dropped), the
// boulder fields' neighbours; none in a trough a walk crosses (keepOut) and none steep. ----
export async function litterPass(ctx) {
  const rnd = mulberry(3303), byMesh = {};
  const add = (name, p) => (byMesh[name] ??= []).push(p);
  const cand = [];
  for (let x = -94; x <= 94; x += 5) for (let z = -94; z <= 94; z += 5) if (!keepOut(x + (rnd() - 0.5) * 3, z + (rnd() - 0.5) * 3, 4)) cand.push([x, z]);
  const s = await sample(ctx.call, cand);
  const ok = cand.filter((c, i) => s[i] && s[i].slope < 22 && s[i].y > 3.5);
  const pool = ok.map((c) => ({ c, k: rnd() })).sort((a, b) => a.k - b.k).map((o) => o.c);
  const picks = [];
  for (const c of pool) { if (picks.length >= 16) break; if (picks.every((p) => Math.hypot(p[0] - c[0], p[1] - c[1]) >= 18)) picks.push(c); }
  for (const [gx, gz] of picks) {
    const n = 2 + (rnd() < 0.5 ? 1 : 0);
    add("Boulder", piece([gx, gz], 0.7 + rnd() * 0.5, rnd));
    for (let i = 1; i < n; i++) { const a = rnd() * 6.28, d = 1.8 + rnd() * 1.4; add(i % 2 ? "Heap" : "Boulder", piece([gx + Math.cos(a) * d, gz + Math.sin(a) * d], i % 2 ? 0.8 + rnd() * 0.4 : 0.4 + rnd() * 0.25, rnd)); }
  }
  return { groups: picks.length, placed: await put(ctx, byMesh) };
}

// ---- pass: flora. Dead thorn scrub: clumps of six to nine Spikes (stretched thin shards, a dark grey-brown) in the dune troughs, sparse:
// a dune desert's few tough plants. They stand about 1.4-2.2 high (a worm is 0.75) and never on a walk or a pad. ----
export async function floraPass(ctx) {
  const rnd = mulberry(4404), byMesh = {};
  const add = (name, p) => (byMesh[name] ??= []).push(p);
  const cand = [];
  for (let x = -94; x <= 94; x += 3) for (let z = -94; z <= 94; z += 3) { const px = x + (rnd() - 0.5) * 2, pz = z + (rnd() - 0.5) * 2; if (!keepOut(px, pz, 2, { mesa: true })) cand.push([px, pz]); }
  const s = await sample(ctx.call, cand);
  // troughs: the low ground (the sand floor stands at 5-7, the crests rise to ~12) and not steep
  const ok = cand.filter((c, i) => s[i] && s[i].slope < 16 && s[i].y > 4.2 && s[i].y < 7.4);
  const pool = ok.map((c) => ({ c, k: rnd() })).sort((a, b) => a.k - b.k).map((o) => o.c);
  const picks = [];
  for (const c of pool) { if (picks.length >= 40) break; if (picks.every((p) => Math.hypot(p[0] - c[0], p[1] - c[1]) >= 9)) picks.push(c); }
  for (const [cx, cz] of picks) {
    const n = 6 + Math.floor(rnd() * 4);
    for (let i = 0; i < n; i++) {
      const a = rnd() * 6.28, d = i === 0 ? 0 : 0.3 + rnd() * 0.8, h = i === 0 ? 1.0 + rnd() * 0.35 : 0.5 + rnd() * 0.6;
      add("Spike", { xz: [r1(cx + Math.cos(a) * d), r1(cz + Math.sin(a) * d)], rot: [r1((rnd() - 0.5) * 26), Math.round(rnd() * 360), r1((rnd() - 0.5) * 26)], scale: [r2(0.3 + rnd() * 0.14), r2(h), r2(0.3 + rnd() * 0.14)] });
    }
  }
  return { clumps: picks.length, placed: await put(ctx, byMesh) };
}

// ---- pass: palace (round 2). The judge's list for a palace that reads scorched and grand, not a toy: smooth copper-green onion
// domes over the four towers' voxel domes (the Dome mesh, a build_rock_bank `lathe`, hides their stair-steps and their blue and
// gold) and a tall tapering onion over the Crown's merlon ring (the tallest silhouette from every side), eight faded banners (the gates'
// flanks and the keep's four faces, none on the plinths), squat stone pylons at the plinths' corners in the wall's stone,
// buttress pilasters along the curtain, a window rhythm on the keep and the Crown, arrow slits on the towers. All stand on faces
// of the palace's voxels at the heights build.mjs gave them (curtain 24-34, plinth risers 12/18/24, keep to 40, towers 24-46,
// onion domes to 53.5, the Crown 40.5-55.5). ----
const BANNER_TINT = "a04a38";                               // a dusty crimson: the cushion's red, faded by the sun
export async function palacePass(ctx) {
  const rnd = mulberry(5505), byMesh = {};
  const add = (name, p) => (byMesh[name] ??= []).push(p);
  // a mesh's foot: y of `pos` that puts its lowest point at `y` when scaled by `sy`
  const foot = (name, y, sy) => r2(y - ctx.boxes[name].min[1] * sy);
  // faces: [normal x, normal z, tangent x, tangent z, gate offset along the tangent]
  const faces = [
    { n: [0, -1], u: [1, 0], g: 13 }, { n: [1, 0], u: [0, 1], g: 13 },       // N (gate at x 13), E (gate at z 13)
    { n: [0, 1], u: [1, 0], g: -13 }, { n: [-1, 0], u: [0, 1], g: -13 },     // S (x -13), W (z -13)
  ];
  const yawOf = (n) => Math.atan2(n[0], n[1]) * DEG;
  // banners: the cushion (1 x 0.34 x 0.96, thin axis y) turned about x so its thin axis is horizontal, then about y to face out
  // of the wall (the rotation was checked against round 1's renders). One on each gate's flank (the gate's opening runs to the
  // lintel at 30, so a banner over it would hang from the merlons), one on each of the keep's faces over the wings' roofs
  const banners = [];
  for (const f of faces) {
    const yaw = yawOf(f.n);
    banners.push({ x: f.n[0] * 25.75 + f.u[0] * (f.g - Math.sign(f.g) * 8.5), z: f.n[1] * 25.75 + f.u[1] * (f.g - Math.sign(f.g) * 8.5), y: 25.2, yaw, w: 2.2, h: 5.2 });
    banners.push({ x: f.n[0] * 12.2, z: f.n[1] * 12.2, y: 34.8, yaw, w: 2.6, h: 4.8 });
  }
  const copies = banners.map((b) => ({ id: 8, pos: [r1(b.x), r1(b.y + b.h / 2), r1(b.z)], rot: [90, r1(b.yaw), 0], scale: [r2(b.w), 1, r2(b.h / 0.96)], tint: BANNER_TINT }));
  const rb = await ctx.call("copy_base_details", { base: "Multi.CarpetCapers", units: "map", tag: PALACE_TAG, copies });
  // domes: the towers' onion domes stand from 46 to 53.5 (radius 4.5 in 0.75 voxels), so the mesh (radius 5 high 7.5 as the bank
  // builds it) is a little wider and taller; the Crown's tall one (its voxel cap is inside it) tapers from 5.6 to a point at about 68
  const domes = [];
  for (const sx of [-1, 1]) for (const sz of [-1, 1]) domes.push({ x: sx * 24.75, z: sz * 24.75, y: 45.8, s: [1.07, 1.02, 1.07] });
  domes.push({ x: 0, z: 0, y: 54.6, s: [1.12, 1.8, 1.12] });
  for (const d of domes) add("Dome", { pos: [d.x, foot("Dome", d.y, d.s[1]), d.z], rot: [0, Math.round(rnd() * 360), 0], scale: d.s });
  // arrow slits: the towers' cardinal outward faces (flat at 3.75 from the axis) at three heights, the Crown's four faces at two
  const slits = [];
  for (const sx of [-1, 1]) for (const sz of [-1, 1]) {
    const cx = sx * 24.75, cz = sz * 24.75;
    for (const [nx, nz] of [[sx, 0], [0, sz]]) for (const y of [29.5, 35.5, 41.5]) slits.push({ x: cx + nx * 3.78, z: cz + nz * 3.78, y, nx, nz });
  }
  for (const [nx, nz] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) for (const y of [44.2, 49.6]) for (const t of [-2.4, 2.4]) {
    slits.push({ x: nx * 6.03 + (nz ? t : 0), z: nz * 6.03 + (nx ? t : 0), y, nx, nz });
  }
  // the keep's windows: a rhythm of four to a face above the wings' roofs, either side of the banner (the keep's faces at 12)
  for (const [nx, nz] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) for (const t of [-8.6, -4.8, 4.8, 8.6]) {
    slits.push({ x: nx * 12.05 + (nz ? t : 0), z: nz * 12.05 + (nx ? t : 0), y: 36.4, nx, nz });
  }
  for (const s of slits) add("Slit", { pos: [r1(s.x), r1(s.y), r1(s.z)], rot: [0, r1(Math.atan2(s.nx, s.nz) * DEG), 0], scale: [0.26, 0.85, 0.1] });
  // pylons: squat stone blocks (the Boulder, stretched a little tall) at the corners of plinth 2's ledge (corner 33, top 18) and
  // plinth 1's (corner 39, top 12), in the stone of the wall: the judge read round 1's gold cones as teeth
  const pylons = [];
  for (const sx of [-1, 1]) for (const sz of [-1, 1]) { pylons.push({ x: sx * 33, z: sz * 33, y: 18, h: 3.4 }); pylons.push({ x: sx * 39, z: sz * 39, y: 12, h: 2.8 }); }
  for (const p of pylons) { const sy = p.h / 1.9; add("Boulder", { pos: [p.x, foot("Boulder", p.y, sy), p.z], rot: [0, 45 + r1((rnd() - 0.5) * 8), 0], scale: [0.9, r2(sy), 0.9] }); }
  // pilasters: buttress columns on the curtain's outer faces every 9 (the corners are the towers, the gate's jambs stay clear):
  // 2 wide, standing 0.75 out of the wall from the terrace (24) to under the merlons (31.4)
  let pilasters = 0;
  for (const f of faces) for (const t of [-18, -9, 0, 9, 18]) {
    if (Math.abs(t - f.g) < 6.5) continue;
    const sy = 7.4 / 1.9, off = 26.1;
    add("Boulder", { pos: [r1(f.n[0] * off + f.u[0] * t), foot("Boulder", 24, sy), r1(f.n[1] * off + f.u[1] * t)], rot: [0, r1(yawOf(f.n) + (rnd() - 0.5) * 6), 0], scale: [0.66, r2(sy), 0.5] });
    pilasters++;
  }
  const placed = await put(ctx, byMesh, PALACE_TAG);
  return { banners: copies.length, bannersPlaced: rb.json?.placed ?? copies.length, domes: domes.length, slits: slits.length, pylons: pylons.length, pilasters, placed };
}

// ---- pass: dry (round 2). The cues a water-starved desert gives: bone piles near the sand spawns and the palace's foot (the
// Bone mesh, a lathe with knobbed ends, lying: three or four crossed in a pile; the base's D07_04 "skulls" are white clouds in the
// game), dry stone wells (a ring of stone blocks round a
// dark disc: the water went long ago, with a clay urn beside it), all low, off the pads and the walks (keepOut) and clear of the
// hoodoos. Round 2 tried cracked hardpan as thin dark seams (a flat Slit): they read as charred planks, so it is not placed. ----
export async function dryPass(ctx) {
  const rnd = mulberry(6606), byMesh = {};
  const add = (name, p) => (byMesh[name] ??= []).push(p);
  const out = { bones: 0, wells: 0, urns: 0, wellAt: [], boneAt: [] };
  // gentle open ground, a grid of candidates (x, z) with its height and slope, kept off the pads, the walks and the palace ring
  const cand = [];
  for (let x = -92; x <= 92; x += 4) for (let z = -92; z <= 92; z += 4) { const px = x + (rnd() - 0.5) * 3, pz = z + (rnd() - 0.5) * 3; if (!keepOut(px, pz, 3)) cand.push([px, pz]); }
  const s = await sample(ctx.call, cand);
  const ok = cand.map((c, i) => ({ x: c[0], z: c[1], y: s[i]?.y, slope: s[i]?.slope })).filter((c) => c.slope < 14 && c.y > 3.5 && c.y < 9 && !(ctx.hoodoos ?? []).some(([hx, hz]) => Math.hypot(c.x - hx, c.z - hz) < 14));
  const shuffled = (arr) => arr.map((c) => ({ c, k: rnd() })).sort((a, b) => a.k - b.k).map((o) => o.c);
  const flattest = (pool) => [...pool].sort((a, b) => a.slope - b.slope);
  const pick = (pool, n, apart, taken, order = shuffled) => { const got = []; for (const c of order(pool)) { if (got.length >= n) break; if ([...taken, ...got].every((p) => Math.hypot(p.x - c.x, p.z - c.z) >= apart)) got.push(c); } return got; };
  const nearPad = (c, lo, hi) => PADS.some(([px, pz]) => { const d = Math.hypot(c.x - px, c.z - pz); return d >= lo && d <= hi; });
  // dry wells: a collar of eight blocks about a flat dark disc, on the flattest ground, far apart
  const wells = pick(ok.filter((c) => c.slope < 9 && Math.max(Math.abs(c.x), Math.abs(c.z)) < 80 && !nearPad(c, 0, 30)), 3, 45, [], flattest);
  const urns = [];
  for (const w of wells) {
    out.wells++; out.wellAt.push([r1(w.x), r1(w.z)]);
    add("Slit", { pos: [r1(w.x), r2(w.y + 0.04), r1(w.z)], rot: [0, 0, 0], scale: [0.62, 0.03, 0.62] });
    for (let i = 0; i < 8; i++) { const a = (i / 8) * 6.2832 + rnd() * 0.2; add("Boulder", { xz: [r1(w.x + Math.cos(a) * 1.55), r1(w.z + Math.sin(a) * 1.55)], rot: [0, r1(-a * DEG + 90), 0], scale: [0.56, r2(0.34 + rnd() * 0.12), 0.34] }); }
    const a = rnd() * 6.28; urns.push({ id: 6, xz: [r1(w.x + Math.cos(a) * 3.3), r1(w.z + Math.sin(a) * 3.3)], rot: [0, r1(rnd() * 360), r1((rnd() - 0.5) * 14)], scale: [0.6, 0.6, 0.6] });
  }
  // bone piles: round each pad (16-42 out) and in the open desert; three to five long bones crossed in a pile
  const bonePick = [...pick(ok.filter((c) => nearPad(c, 16, 42)), 10, 14, wells), ...pick(ok.filter((c) => !nearPad(c, 0, 50) && Math.max(Math.abs(c.x), Math.abs(c.z)) < 80), 6, 22, wells)];
  for (const c of bonePick) {
    out.boneAt.push([r1(c.x), r1(c.z)]);
    const n = 3 + Math.floor(rnd() * 3), yaw0 = rnd() * 180;
    for (let i = 0; i < n; i++) {
      const a = rnd() * 6.28, d = rnd() * 0.9;
      // the mesh stands on its foot along y: rolled 90 about z it lies along x, its axis 0.28 over the ground (half sunk)
      add("Bone", { pos: [r1(c.x + Math.cos(a) * d), r2(c.y + 0.28), r1(c.z + Math.sin(a) * d)], rot: [r1((rnd() - 0.5) * 10), r1(yaw0 + i * 47 + rnd() * 40), 90], scale: [1, r2(0.55 + rnd() * 0.45), 1] });
      out.bones++;
    }
  }
  out.placed = await put(ctx, byMesh);
  const ur = await ctx.call("copy_base_details", { base: "Multi.DesertOasis", units: "map", tag: TAG, copies: urns }, { soft: true });
  out.urns = urns.length;
  out.basePlaced = { urns: ur.err ? ur.text.slice(0, 160) : (ur.json?.placed?.length ?? urns.length) };
  return out;
}

// ---- the lints: scenery over ground steeper than 50 degrees comes out (lint_contact), and so does a clump with nothing
// under its downhill edge on steep ground (lint_support). The layer's own tag only. ----
export async function lintPass(ctx) {
  const out = {};
  for (const [tool, args] of [["lint_contact", { search: `_${TAG}`, remove: true }], ["lint_support", { search: `_${TAG}`, remove: true }]]) {
    const r = await ctx.call(tool, args, { soft: true });
    out[tool] = r.err ? r.text.slice(0, 200) : { removed: r.json?.removed ?? r.json?.removedCount ?? 0, over: r.json?.over?.length ?? r.json?.unsupported?.length ?? 0 };
  }
  return out;
}
