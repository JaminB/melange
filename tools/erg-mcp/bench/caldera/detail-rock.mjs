// Detail layer, round 1: the volcano's ROCK (see detail.mjs). Sim-neutral channels only: scenery copies tagged _dtl (no
// collision, VISIBLE_ names), point lights and particle emitters. Layout is a level artist's: clusters at the places rock
// gathers (cliff-foot talus, gully-mouth scree fans, the shore band), a big boulder with a ring of smaller ones and rubble, size
// falling off with distance from the cluster's seed, rock type by altitude:
//   shore band   wet, mossy white boulders (Tutorial1 BUILDING13, moss green on pale rock) + small gold rubble
//   lower flank  mossy boulders thinning out, grey-gold rubble (MineAllMine WILDWEST6), ash-tan rocks (ARABIAN6)
//   upper flank  pumice: tan/gold twin rocks (Horror9) and ARABIAN6, bigger and sparser
//   rim / wall   a few pumice blocks, steam vents, ember smoke
//   crater       nothing but lava glow (point lights round the lake), embers and the plume
// Nothing stands within the keep-out of detail-keepout.mjs (trail + kerb, castle courts, spawns, launcher pads, the factory),
// on a lava tongue, in the water or on a slope over 48 degrees, and every piece is seated by surface_sample (the voxels' own top).
import * as T from "./terrain.mjs";

// the pieces: [label, base, resource, native size (map, catalog), role]
const PIECES = {
  moss:   { base: "Multi.Tutorial1", resource: "BUILDING13", pick: (d) => d.name.toLowerCase().includes("flower") },
  rubble: { base: "Multi.MineAllMine", resource: "WILDWEST6", pick: (d) => /rubble/i.test(d.name) },
  chips:  { base: "Multi.DoomCanyon", resource: "WILDWEST6", pick: (d) => /smallrocks/i.test(d.name) },
  tan:    { base: "Multi.DesertOasis", resource: "ARABIAN6", pick: (d) => /rock/i.test(d.name) },
  tall:   { base: "Multi.TraitorousWaters", resource: "ARABIAN16", pick: (d) => /rock/i.test(d.name) },
  pumice: { base: "Multi.W3DWhenAnnelidsCollide", resource: "Horror9", pick: (d) => /visible/i.test(d.name) },
};

export function rng(seed) { let s = seed >>> 0; return () => ((s = (s * 1664525 + 1013904223) >>> 0) / 4294967296); }

const TONGUES = T.TONGUES;
/** Whether (x, z) is on a lava tongue (the centre angle, with its wander, and the width at that radius, plus a margin). */
function onTongue(x, z, margin = 1.5) {
  const r = Math.hypot(x, z), th = Math.atan2(z, x);
  for (const t of TONGUES) {
    if (r < t.r0 - 2 || r > t.r1 + 2) continue;
    const a = t.a + t.wob * Math.sin(r / t.wl + t.ph) + t.dr * Math.max(0, r - t.rb);
    const w = t.w0 + (t.w1 - t.w0) * Math.min(1, Math.max(0, (r - t.r0) / (t.r1 - t.r0)));
    let d = Math.abs(((th - a + Math.PI * 3) % (Math.PI * 2)) - Math.PI);
    if (d * r < w + margin) return true;
  }
  return false;
}

/** Find the example detail ids of each PIECES entry (scenery_catalog per base). */
async function findPieces(call) {
  const out = {};
  for (const [k, p] of Object.entries(PIECES)) {
    const rows = (await call("scenery_catalog", { bases: [p.base], roles: ["scenery"], search: p.resource, sizes: true, limit: 50 })).json.list;
    const row = rows.find((r) => r.resource === p.resource);
    if (!row) throw new Error(`piece ${k}: no ${p.resource} in ${p.base}`);
    out[k] = { ...p, id: row.example.id, size: row.meshSize ?? row.size, rot: row.example.rot, scale: row.example.scale };
  }
  return out;
}

export async function rockPass(ctx) {
  const { call, keepOut, say } = ctx;
  const R = rng(20261109);
  const pieces = await findPieces(ctx.call);
  const maxDim = (pc) => Math.max(...pc.size.slice(0, 3));
  const stats = { clusters: 0, boulders: 0, rubble: 0, lights: 0, emitters: 0, rejected: {} };
  const rej = (why) => { stats.rejected[why] = (stats.rejected[why] ?? 0) + 1; };

  // ---- the cluster seeds: [x, z, kind, strength] ----
  const seeds = [];
  const addSeed = (x, z, kind, n) => seeds.push({ x, z, kind, n });
  // 1. shore band: mossy boulder groups all round (every ~0.45 rad with jitter)
  for (let th = 0.05; th < Math.PI * 2; th += 0.17 + R() * 0.1) {
    const rho = 83 + R() * 7, [x, z] = T.pointAt(rho, th);
    addSeed(x, z, "shore", 3 + Math.floor(R() * 3));
  }
  // 2. gully mouths: scree fans of rubble with a few tan blocks
  for (const g of T.GULLIES) for (const rr of [g.r1 - 8, g.r1 - 2, g.r1 + 4, g.r1 + 9]) {
    const [x, z] = T.pointAt(rr, g.a + (R() - 0.5) * 0.04);
    addSeed(x, z, "scree", 6 + Math.floor(R() * 4));
  }
  // 3. cliff-foot talus: below each cliff ring, boulder groups round the cone
  for (const c of T.CLIFFS) for (let th = R() * 0.5; th < Math.PI * 2; th += 0.26 + R() * 0.18) {
    const [x, z] = T.pointAt(c.rho - 3.5 + R() * 2, th);
    addSeed(x, z, c.rho < 60 ? "talus-hi" : "talus", 3 + Math.floor(R() * 3));
  }
  // 4. upper flank pumice outcrops and the rim's blocks
  for (let th = R() * 0.6; th < Math.PI * 2; th += 0.2 + R() * 0.2) addSeed(...T.pointAt(40 + R() * 14, th), "pumice", 2 + Math.floor(R() * 3));
  for (let th = R() * 0.6; th < Math.PI * 2; th += 0.7 + R() * 0.5) addSeed(...T.pointAt(T.RC + 1 + R() * 3, th), "rim", 2);

  // ---- expand each seed to candidate props: a hero rock plus a ring, sizes falling off ----
  const KIND = {
    shore:      { hero: ["moss", 2.0, 3.0], ring: [["moss", 0.9, 1.6], ["chips", 2.5, 4], ["rubble", 2, 3.5]], spread: 5.5 },
    scree:      { hero: ["tan", 1.8, 2.8], ring: [["rubble", 1.8, 3.2], ["chips", 2.5, 4.5], ["tan", 0.8, 1.4]], spread: 7 },
    talus:      { hero: ["moss", 1.7, 2.6], ring: [["tan", 0.9, 1.7], ["rubble", 2, 3.2], ["chips", 2.5, 4]], spread: 6 },
    "talus-hi": { hero: ["pumice", 4, 6.5], ring: [["pumice", 2, 3.5], ["rubble", 2, 3.2], ["tan", 0.9, 1.6]], spread: 6 },
    pumice:     { hero: ["pumice", 5, 8], ring: [["pumice", 2.2, 4], ["chips", 3, 5]], spread: 5 },
    rim:        { hero: ["pumice", 4, 6], ring: [["tall", 1, 1.8], ["rubble", 2, 3]], spread: 4 },
  };
  const cands = [];
  for (const sd of seeds) {
    const k = KIND[sd.kind];
    cands.push({ pc: k.hero[0], at: [sd.x, sd.z], s: (k.hero[1] + R() * (k.hero[2] - k.hero[1])) * 2.4, kind: sd.kind, hero: true });
    for (let i = 0; i < Math.round(sd.n * 1.5); i++) {
      const [pc, s0, s1] = k.ring[Math.floor(R() * k.ring.length)];
      const a = R() * Math.PI * 2, d = 3.5 + Math.sqrt(R()) * k.spread * 1.8;       // denser near the hero, thinning outward
      const fall = 1 - 0.35 * (d / (k.spread + 1.6));                             // smaller with distance
      cands.push({ pc, at: [sd.x + Math.cos(a) * d, sd.z + Math.sin(a) * d], s: (s0 + R() * (s1 - s0)) * fall * 2.0, kind: sd.kind });
    }
  }

  // ---- filter + seat on the real surface ----
  const pts = cands.map((c) => c.at);
  const samples = [];
  for (let i = 0; i < pts.length; i += 3000) samples.push(...(await call("surface_sample", { points: pts.slice(i, i + 3000), units: "map", step: 1.5 })).json.samples);
  const placed = [], sizeOf = (c) => maxDim(pieces[c.pc]) * c.s;
  const copies = {};
  cands.forEach((c, i) => {
    const sm = samples[i];
    if (!sm) return rej("no ground");
    if (sm.y < 2.2) return rej("water");
    if (sm.slope > 80) return rej("too steep");
    if (onTongue(c.at[0], c.at[1], 1.2)) return rej("lava tongue");
    const r = sizeOf(c) / 2;
    const why = keepOut(c.at[0], c.at[1], r);
    if (why) return rej(why);
    const rho = T.rhoAt(c.at[0], c.at[1]);
    if (rho < T.RC - 1) return rej("crater");
    if (placed.some((p) => Math.hypot(p.x - c.at[0], p.z - c.at[1]) < (p.r + r) * 0.7)) return rej("overlap");
    const pc = pieces[c.pc];
    const yaw = R() * 360, tilt = c.pc === "rubble" || c.pc === "chips" ? 0 : (sm.slope > 15 ? 0 : (R() - 0.5) * 10);
    // lean a boulder with the ground a little: slope-aligned pitch is skipped (meshes are upright blocks); sink more on slopes
    const seat = Math.min(0.6, 0.15 + sm.slope / 110 + (c.hero ? 0.05 : 0));
    (copies[c.pc] ??= []).push({ id: pc.id, pos: [Math.round(c.at[0] * 100) / 100, sm.y, Math.round(c.at[1] * 100) / 100], rot: [tilt, yaw, 0], scale: Math.round(c.s * 100) / 100, seat });
    placed.push({ x: c.at[0], z: c.at[1], r });
    if (c.hero) stats.boulders++; else stats.rubble++;
  });
  for (const [k, list] of Object.entries(copies)) {
    const pc = pieces[k];
    for (let i = 0; i < list.length; i += 200)
      await call("copy_base_details", { base: pc.base, allowOtherTheme: true, units: "map", tag: "dtl", copies: list.slice(i, i + 200) });
    say("rock", k, list.length);
  }
  stats.placed = placed.length; stats.clusters = seeds.length;

  // ---- glow and steam: lights + emitters (no collision, no entity the sim reads) ----
  async function fx(base, search, atList, dy = 0.5) {
    const rows = (await call("scenery_catalog", { bases: [base], search, sizes: false, limit: 50 })).json.list;
    const hit = rows.map((r) => r.example).find((e) => e.name.includes(search));
    if (!hit) { say("fx: no", search, "in", base); return 0; }
    const cs = [];
    for (const [x, z, up] of atList) {
      const sm = (await call("surface_sample", { points: [[x, z]], units: "map" })).json.samples[0];
      if (!sm) continue;
      cs.push({ id: hit.id, pos: [Math.round(x * 100) / 100, sm.y + (up ?? dy), Math.round(z * 100) / 100] });
    }
    if (cs.length) await call("copy_base_details", { base, allowOtherTheme: true, units: "map", copies: cs });
    return cs.length;
  }
  // lava glow ringing the lake and over the crater's wall, kept off the factory plug and launchers by the keep-out
  const L = T.LAKE, glow = [];
  for (let i = 0; i < 8; i++) { const a = i / 8 * Math.PI * 2 + 0.2; glow.push([L.c[0] + Math.cos(a) * (L.R + 1), L.c[1] + Math.sin(a) * (L.R + 1), 3]); }
  glow.push([L.c[0], L.c[1], 5]);
  for (const g of T.TONGUES.filter((t) => t.core)) for (const rr of [T.RC + 12, 56, 72]) {            // the lava tongues glow along their length
    const a = g.a, [x, z] = T.pointAt(rr, a); glow.push([x, z, 2.5]);
  }
  const glowOk = glow.filter(([x, z]) => !keepOut(x, z, 0));
  stats.lights = await fx("Multi.ValleyOfDinoWorms", "PNTLGHT 255 100 0 300", glowOk);
  // the volcano's plume over the crater, steam vents on the rim and along two gullies, bonfire-like embers on the lava lake's shore
  stats.emitters += await fx("Multi.ValleyOfDinoWorms", "VolcanoPlumeSmoke", [[L.c[0], L.c[1], 4]]);
  const vents = [];
  for (let i = 0; i < 6; i++) { const [x, z] = T.pointAt(T.RC + 3 + R() * 3, i / 6 * Math.PI * 2 + 0.5); vents.push([x, z, 0.5]); }
  for (const g of T.GULLIES.slice(0, 4)) { const [x, z] = T.pointAt(g.r0 + 6, g.a); vents.push([x, z, 0.5]); }
  stats.emitters += await fx("Multi.Tutorial3", "SteamVent", vents.filter(([x, z]) => !keepOut(x, z, 1) && !onTongue(x, z, 0)));
  const embers = [];
  for (let i = 0; i < 3; i++) { const a = i * 2.1 + 0.9; embers.push([L.c[0] + Math.cos(a) * (L.R + 0.5), L.c[1] + Math.sin(a) * (L.R + 0.5), 0.3]); }
  stats.emitters += await fx("Multi.ValleyOfDinoWorms", "BonfireTall", embers.filter(([x, z]) => !keepOut(x, z, 0)));
  return stats;
}
