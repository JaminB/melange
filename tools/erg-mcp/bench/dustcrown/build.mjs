// Dustcrown: Foldspace's second map (DESIGN.md), built only through the erg-mcp server's tools, as Caldera is. A harsh,
// water-starved desert on Multi.DinerMight in the Arabian theme's materials and DAY light: a transverse dune field (dunes()
// and ridged(), bleached crests and ochre troughs by curvature) running under a sand-coloured sea (set_water_look sand),
// two tall refuge mesas and four low ones (build_mesa), six boulder fields, and in the centre the Dustcrown, a sandstone
// fortress-palace: three sheer plinths (build_compound tiers), a crenellated curtain with four domed corner towers and a keep
// (build_castle, bands and trim), cross wings that cut the court into four, and the Crown tower, the tallest frame, which
// sets Sandtide's sand line. Five spawn areas (four in the sand, one on the palace) dealt per match (place_spawn_knots areas).
// The sea is the game's own: it starts at 20 world units and rises only in the scheme's sudden death.
//
//   tools\node\node.exe tools\erg-mcp\bench\dustcrown\build.mjs [--out <dir>] [--plan] [--terrain] [--no-export] [--keep-mod] [--no-detail]
//
// It connects like erg-mcp does: MELANGE_GAME_DIR (default testenv C) with the game closed, starting Melange.exe --serve
// (MELANGE_EXE, default this checkout's build\x86-release\Melange.exe). The project id is `dustcrown`; an old one is deleted
// first. Renders go to --out (default out\ next to this file); the source export is copied to pack\ next to this file with
// Dustcrown's one mesh bank (build_rock_bank) in its assets\meshes\. --plan stops after the desert's plans (nothing saved);
// --terrain after the desert, shell and mesas (renders, nothing saved). --no-detail leaves the boulder meshes out (the
// sim_fingerprint check: the meshes are decoration only).
import fs from "node:fs";
import path from "node:path";

import { fileURLToPath } from "node:url";
import { startServer, ROOT } from "../caldera/mcpclient.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const argv = process.argv.slice(2);
const flag = (f) => argv.includes(f);
const opt = (f, d) => { const i = argv.indexOf(f); return i >= 0 ? argv[i + 1] : d; };
const OUT = path.resolve(opt("--out", path.join(HERE, "out")));
const GAME = process.env.MELANGE_GAME_DIR || "C:/Users/Jamin/Desktop/WUMFix/testenv/C";
process.env.MELANGE_EXE ??= path.join(ROOT, "build/x86-release/Melange.exe");
const ID = process.env.DUSTCROWN_ID || "dustcrown";
const MOD = "dustcrown";
const VERSION = "1.0.0";
// The sea material (set_water_look sea): the sand's own tone, so the sea continues the dunes (a water shader mod draws it:
// Sunstone 2.4+; the game's own shader shows the sand preset's pale opaque sea)
const SEA_ALBEDO = [255, 250, 180];
// the dunes' tones under Sunstone (tint_landscape: each texture's mean becomes the colour)
const SAND_TINT = { body: "#e6c893", trough: "#c47f3c", crest: "#f6eedb" };
const WORM_W = 0.6;                     // a worm's body, map units (check_reachability's bodyWidth)

// Materials of the Arabian theme's own file (set_theme ARABIAN, materialFile "theme"; research/mat_arab.txt)
const MAT = { grass2: 2, grass2b: 3, rock1: 4, rock1b: 5, rock2: 6, rock2b: 7, path1: 8, path2: 9, sand01: 10, stone: 12, bricks: 14, splaster: 15,
  dome: 18, plaster: 21, sand: 24, gold: 25, goldEdge: 26, star: 30, tiles: 32, blue: 38, ochre: 57 };

// ---- the numbers (DESIGN.md 1): the sea at start, the floor, the Crown's tip and the sand line ----
const WATER_START = 20;                  // world units (1 map)
const FLOOR = -9;                        // the skin's floor (map): heights count from it exactly (keepFloor)
const SAND = 6;                          // the sand floor (map) at the pads and round the palace
const CROWN_TIP = 55;                    // map: the tallest frame's top, which sets Sandtide's line
const LINE_W = WATER_START + 0.25 * (CROWN_TIP * 20 - WATER_START);   // 290 world units
const BOX = 118;                         // the arena's half side
const CLIP_R = 113;                      // the desert's skin is clipped to this circle (past it, the sea's opaque floor)

// the spawn pads (sand areas) and the palace's courts, map units
const PAD_R = 86;
const SAND_AREAS = [["SAND W", -PAD_R, 0], ["SAND E", PAD_R, 0], ["SAND N", 0, -PAD_R], ["SAND S", 0, PAD_R]];
// the mesas: refuges NW and SE above the line, four low ones under it (180-degree symmetric)
const MESAS = [
  { name: "refuge NW", at: [-60, -60], radii: [14, 10], rotation: 45, top: 21.5, refuge: true, seed: 11 },
  { name: "refuge SE", at: [60, 60], radii: [14, 10], rotation: 45, top: 21.5, refuge: true, seed: 12 },
  { name: "low NE", at: [56, -58], radius: 11, top: 9, seed: 13 },
  { name: "low SW", at: [-56, 58], radius: 11, top: 9, seed: 14 },
  { name: "low W", at: [-74, -30], radius: 10, top: 9, seed: 15 },
  { name: "low E", at: [74, 30], radius: 10, top: 9, seed: 16 },
];
const MESA_FLOOR = 4;                    // a mesa stands from under the sand (buried 2), so no gap opens at its foot
const mesaR = (m) => m.radius ?? Math.max(...m.radii);
// boulder fields: one between each sand area and the palace, two by the low mesas
const FIELDS = [[-64, 8, 21], [64, -8, 22], [-8, -64, 23], [8, 64, 24], [36, -70, 25], [-36, 70, 26]];

// ---- the desert's height (map y above FLOOR), one expression for the skin and, lowered, the shell under it ----
const f1 = (v) => (Math.round(v * 10) / 10).toString();
const desertExpr = (lower = 0) => {
  const pads = SAND_AREAS.map(([, x, z]) => `pad(${x}, ${z})`).join(" * ");
  const mesas = MESAS.map((m) => `ms(${m.at[0]}, ${m.at[1]}, ${f1(mesaR(m))})`).join(" * ");
  return [
    "let sq = max(abs(x), abs(z));",
    "fn pad(px, pz) = smoothstep(7, 15, hypot(x - px, z - pz));",
    "fn ms(px, pz, rr) = smoothstep(rr + 1, rr + 7, hypot(x - px, z - pz));",
    // the dunes die down in a ring 8 wide round the plinth (no crest leans on a riser), at the pads and at the mesas' feet
    `let m = smoothstep(50, 60, sq) * ${pads} * ${mesas};`,
    // transverse dunes, crests NNE-SSW (wind from the west), 18 apart, 3.6 high with a slip face, ridged crest lines, a broad swell
    "let d = dunes(x, z, 15, 18, 3.6, 0.7, 7) + 0.8 * ridged(x, z, 22, 3, 5) + 0.6 * fbm(x / 45, z / 45, 3, 2);",
    `let land = ${SAND} + m * (d - 1.9);`,
    // the coast: from about 100 the dunes slope under the sea, the shore wobbling with the bearing
    "let rc = 105 + 4 * noise2(x / 28, z / 28, 9);",
    "let c = smoothstep(rc - 5, rc + 7, r);",
    `lerp(land, -6, c) - ${FLOOR} - ${lower}`,
  ].join(" ");
};

// ---- the MCP client ----
const say = (...a) => console.log(...a);
const brief = (r, n = 300) => r.text.slice(0, n).replace(/\s+/g, " ");
const round1 = (v) => Math.round(v * 10) / 10;
const round2 = (v) => Math.round(v * 100) / 100;

fs.mkdirSync(OUT, { recursive: true });
const { call, stop, log, stats } = await startServer();
const img = (r, name) => { if (r.images[0]) fs.writeFileSync(path.join(OUT, `${name}.png`), Buffer.from(r.images[0].data, "base64")); };
const shot = async (name, tool, args) => { const r = await call(tool, args, { soft: true }); img(r, name); say("render", name, r.err ? r.text.slice(0, 300) : "ok"); return r; };
try {
  say("connect", brief(await call("connect", { gameDir: GAME }), 160));
  if ((await call("list_bases", { search: "dinermight" })).json.projects.some((p) => p.id === ID))
    say("delete the old project", brief(await call("delete_project", { id: ID }), 200));
  const np = await call("new_project", { base: "Multi.DinerMight", slug: ID, title: "Dustcrown" });
  if (np.json?.created?.id !== ID) throw new Error(`new_project gave id ${np.json?.created?.id}, not ${ID}`);
  await call("open_project", { id: ID });

  // ---- setting: the Arabian theme's own materials, DAY, the sea low, no surround; the diner's scenery and backdrop gone,
  // its terrain wiped and its frames emptied (their cells come back) ----
  await call("set_theme", { theme: "ARABIAN", materialFile: "theme" });
  await call("set_time_of_day", { timeOfDay: "DAY" });
  await call("set_water", { level: WATER_START });
  await call("set_surround_mode", { mode: "none" });
  for (const search of ["Neon", "Grass", "BUILDING", "D05", "MusicNotes", "Applause", "JukeBox"]) say("remove", search, brief(await call("remove_scenery", { search }, { soft: true }), 120));
  say("backdrop", brief(await call("remove_backdrop", {}, { soft: true }), 200));
  say("clear", brief(await call("clear_base_terrain", { all: true, wipe: true }), 200));
  say("remove base frames", brief(await call("remove_base_frames", { all: true }, { soft: true }), 300));
  const lvl = async () => (await call("describe_level", { limit: 1 })).json.limits;
  say("cells before", JSON.stringify((await lvl()).voxelCells));

  // ---- the desert skin: a drape (voxel 2.5, 3 thick, hollow) of the dune field, clipped to the circle, the plinth's
  // footprint left out (the palace's solid fills it) ----
  const padRegion = SAND_AREAS.map(([, x, z]) => ({ x, z, radius: 6.5 }));
  const skin = {
    min: [-BOX, -BOX], max: [BOX, BOX], centre: [0, 0], floor: FLOOR, keepFloor: true, voxelSize: 2.5, thickness: 3, core: false,
    expr: desertExpr(), clip: [{ x: 0, z: 0, radius: CLIP_R }], exclude: [{ min: [-40, -40], max: [40, 40] }],
    // no closed basins deeper than a worm's step: a trough a pad's flat ring or a mesa's foot cuts off is filled to its
    // spill height (the sea, at 1, is where everything drains to)
    fillBasins: { maxDepth: 0.5, outletBelow: WATER_START / 20 },
    material: MAT.sand, maxBlocks: 18,
    materials: [
      { material: MAT.path1, region: padRegion, feather: { width: 1.5, pattern: "perlin", cell: 3 } },
      { material: MAT.sand01, noise: { threshold: 0.25, scale: 16, seed: 4 }, feather: { width: 0.6, pattern: "perlin", cell: 5 } },
    ],
    // crests in grass02blend (its own tile, A04: grass02's top is the sand's own image, so a tint of one is a tint of both)
    crestMaterial: { material: MAT.grass2b, minCurvature: 0.035, feather: 0.015 },
    troughMaterial: { material: MAT.path2, minCurvature: 0.03, feather: 0.015 },
  };
  // the shell under the skin: the same surface 6.5 lower in 4-unit voxels, 3 thick, over the playable desert, so a hole dug
  // through the skin lands on rock instead of falling to the sea bed (Caldera's lesson)
  const shell = { min: [-100, -100], max: [100, 100], centre: [0, 0], floor: FLOOR, keepFloor: true, voxelSize: 4, thickness: 3, core: false,
    expr: desertExpr(6.5), clip: [{ x: 0, z: 0, radius: 100 }], material: MAT.rock1, maxBlocks: 6,
    fillBasins: { maxDepth: 0.5, outletBelow: WATER_START / 20 - 6.5 } };
  const plan = await call("heightfield_drape", { ...skin, dryRun: true });
  fs.writeFileSync(path.join(OUT, "plan-skin.json"), plan.text);
  say("skin plan", JSON.stringify({ fits: plan.json.fits, why: plan.json.why, skin: { blocks: plan.json.skin?.blocks, cells: plan.json.skin?.cells }, curve: plan.json.curveMaterials, peak: plan.json.region?.peak, floor: plan.json.region?.floor }));
  say("skin basins", JSON.stringify(plan.json.region?.basins ?? null).slice(0, 900));
  const splan = await call("heightfield_drape", { ...shell, dryRun: true });
  say("shell plan", JSON.stringify({ fits: splan.json.fits, why: splan.json.why, skin: { blocks: splan.json.skin?.blocks, cells: splan.json.skin?.cells } }));
  if (flag("--plan")) { await call("close", { discard: true }, { soft: true }); throw new Error("--plan: stopped"); }
  const sk = await call("heightfield_drape", skin);
  say("skin", JSON.stringify({ added: sk.json.added?.length, cells: sk.json.limits?.voxelCells ?? sk.json.budget?.cells, notes: sk.json.notes ?? [] }));
  const sh = await call("heightfield_drape", shell);
  say("shell", JSON.stringify({ added: sh.json.added?.length, cells: sh.json.limits?.voxelCells ?? sh.json.budget?.cells, notes: sh.json.notes ?? [] }));
  const groundOf = async (x, z) => {
    const g = await call("query_ground", { x, z, units: "map" });
    const top = g.json?.standOn?.map;
    if (typeof top !== "number") throw new Error(`no ground at ${x}, ${z}: ${g.text.slice(0, 300)}`);
    return top;
  };
  const padGround = [];
  for (const [n, x, z] of SAND_AREAS) padGround.push([n, round2(await groundOf(x, z))]);
  say("pad ground (design", SAND, ")", JSON.stringify(padGround));

  // ---- the mesas: refuges with sheer stepped cliffs (no tread), low ones with walkable ledges ----
  const strataBands = [{ material: MAT.rock1, thickness: 2.5 }, { material: MAT.rock2, thickness: 2.5 }];
  for (const m of MESAS) {
    const shape = m.radii ? { kind: "ellipse", radii: m.radii, rotation: m.rotation } : { kind: "circle", radius: m.radius };
    const r = await call("build_mesa", { at: m.at, floor: MESA_FLOOR, top: m.top, shape, voxelSize: 2.5, wobble: { amount: 1.5, scale: 9, seed: m.seed },
      strata: { step: m.refuge ? 6 : 1, riserJitter: 0.2, bands: strataBands, bandJitter: { amount: 0.6, scale: 6, seed: m.seed } },
      tread: m.refuge ? { walkable: false, jitter: 0.8, seed: m.seed } : { width: 5, jitter: 0.8, seed: m.seed },
      talus: m.refuge ? { height: 2.5, width: 6, material: MAT.rock1b } : undefined, cap: { material: MAT.stone }, maxBlocks: 3 });
    say("mesa", m.name, JSON.stringify({ topY: r.json.mesa?.topY, steps: r.json.mesa?.steps, tread: r.json.mesa?.tread, added: r.json.added?.length, cells: r.json.limits?.voxelCells }));
  }
  if (flag("--terrain")) {
    await shot("t-topdown", "render_topdown", { step: 1, scale: 3 });
    await shot("t-view-se", "render_view", { view: "se", fog: 900 });
    await shot("t-low", "render_view", { pos: [-112, 16, 30], target: [-40, 6, 0], fov: 55, fog: 900 });
    await call("close", { discard: true }, { soft: true }); throw new Error("--plan: --terrain stopped");
  }

  // ---- the boulder fields: domes of rock bedded into the dunes, each field one compound (one block) ----
  for (const [fi, [fx, fz, seed]] of FIELDS.entries()) {
    let s = seed;
    const rnd = () => { s = (s * 16807) % 2147483647; return s / 2147483647; };
    const shapes = [];
    const lows = [];
    for (let i = 0; i < 6; i++) {
      const a = rnd() * Math.PI * 2, d = 1.5 + rnd() * 5, bx = round1(fx + Math.cos(a) * d), bz = round1(fz + Math.sin(a) * d);
      const g = await groundOf(bx, bz);
      const rad = round1(1.4 + rnd() * 1.3), ht = round1(1.6 + rnd() * 1.4);
      lows.push(g);
      shapes.push({ kind: "dome", radius: rad, height: ht + 1, at: [round1(bx - fx), round1(g - 1), round1(bz - fz)], material: i % 3 ? MAT.rock1 : MAT.rock2 });
    }
    // onSkin: boulders stand on the dunes on purpose (save's drape.pokes advisory is for plugs that should lie under a skin)
    const r = await call("build_compound", { at: [fx, 0, fz], shapes, scale: 1, place: "new", packing: "fit", maxBlocks: 1, onSkin: true }, { soft: true });
    say("boulders", fi, fx, fz, r.err ? r.text.slice(0, 300) : JSON.stringify({ cells: r.json.cells?.newBlocks, blocks: r.json.newBlocks?.length }));
  }

  // ---- the palace (DESIGN.md 2.1). Plinths: step 1 84 x 84 from under the sand to 12, then two tiers of 6 (ledges 6 deep)
  // to 18 and 24, scale 3; ochre risers with a stone top course, bricks at the base; two niches a side in each upper riser ----
  const tread = (from) => [{ fromY: from, toY: from + 3, material: MAT.ochre }, { fromY: from + 3, toY: from + 6, material: MAT.stone }];
  const niches = [];
  for (const [half, y] of [[36, 12], [30, 18]]) for (const u of [-13.5, 13.5]) {
    niches.push({ kind: "box", carve: true, size: [3, 4], height: 3, at: [u, y + 3, -half - 0.5 + 1.5] }, { kind: "box", carve: true, size: [3, 4], height: 3, at: [u, y + 3, half + 0.5 - 1.5] },
      { kind: "box", carve: true, size: [4, 3], height: 3, at: [-half - 0.5 + 1.5, y + 3, u] }, { kind: "box", carve: true, size: [4, 3], height: 3, at: [half + 0.5 - 1.5, y + 3, u] });
  }
  const plinth = await call("build_compound", { at: [0, 0, 0], scale: 3, place: "new", packing: "parts", maxBlocks: 6, material: MAT.ochre, shapes: [
    { kind: "box", size: [84, 84], height: 15, at: [0, -3, 0], part: 1, bands: [{ fromY: 0, toY: 6, material: MAT.bricks }, { fromY: 6, toY: 12, material: MAT.ochre }, { fromY: 12, toY: 15, material: MAT.stone }] },
    { kind: "tiers", size: [72, 72], steps: 2, riser: 6, ledge: 6, at: [0, 12, 0], part: 2, bands: [...tread(0), ...tread(6)] },
    ...niches.map((n) => ({ ...n, at: [n.at[0], n.at[1] - 3, n.at[2]] })),
  ] });
  say("plinth", JSON.stringify({ info: plinth.json.structureInfo?.tipY, cells: plinth.json.cells, blocks: plinth.json.newBlocks?.map((b) => [b.id, b.cells]) }));

  // ---- the curtain, corner towers and keep on step 3 (court floor 24): sandy plaster below a gold course, plaster above,
  // gold merlon caps; a gate a side, pinwheeled into the courts (N into NE, E into SE, S into SW, W into NW) ----
  const COURT = 24;
  const GATE_W = 6;       // 4.5 snapped to 4 voxels (N, E) or 5 (S, W) of 1.5; 6 clears 6 at every gate (round 2)
  const castle = await call("build_castle", { at: [0, COURT, 0], size: [52, 52], wallHeight: 8, wallThickness: 3, towerShape: "round", towerRadius: 4, towerHeight: 22,
    keep: true, keepSize: [24, 24], keepHeight: 16, gates: ["n", "e", "s", "w"].map((side, i) => ({ side, offset: i < 2 ? 13 : -13, width: GATE_W, height: 6 })),
    gateBelow: 0, crenellations: true, foundation: 0, material: MAT.plaster, scale: 1.5, place: "new", packing: "parts",
    bands: [{ toY: 6, material: MAT.splaster }], trim: { course: { y: 6, material: MAT.goldEdge, height: 1.5 }, merlonCap: MAT.gold } }, { soft: true });
  if (castle.err) throw new Error(`build_castle: ${castle.text.slice(0, 800)}`);
  const spots = castle.json.spawnSpots?.spots ?? [];
  fs.writeFileSync(path.join(OUT, "spots.json"), JSON.stringify(spots, null, 1));
  say("castle", JSON.stringify({ cells: castle.json.cells, blocks: castle.json.newBlocks?.length, court: castle.json.courtyard, towers: spots.filter((q) => q.kind === "tower").map((q) => q.at) }));

  // ---- the cross wings (galleries 6 wide from the keep to the curtain's mid-sides, roof 32) with an arch through each at
  // court level, so the four courts join; the Crown on the keep's roof; dome caps on the corner towers ----
  const wing = (cx, cz, alongX, part) => [
    { kind: "box", size: alongX ? [12, 6] : [6, 12], height: 8, at: [cx, 0, cz], part, bands: [{ fromY: 0, toY: 6, material: MAT.splaster }, { fromY: 6, toY: 7.5, material: MAT.goldEdge }] },
    { kind: "box", carve: true, size: alongX ? [4.5, 8] : [8, 4.5], height: 4.5, at: [cx, 0, cz] },
  ];
  const wings = await call("build_compound", { at: [0, COURT, 0], scale: 1.5, place: "new", packing: "parts", maxBlocks: 4, material: MAT.plaster,
    shapes: [...wing(0, -17.25, false, 1), ...wing(0, 17.25, false, 2), ...wing(-17.25, 0, true, 3), ...wing(17.25, 0, true, 4)] }, { soft: true });
  say("wings", wings.err ? wings.text.slice(0, 400) : JSON.stringify({ cells: wings.json.cells, blocks: wings.json.newBlocks?.length }));
  const merlons = Array.from({ length: 8 }, (_, i) => { const a = i * Math.PI / 4; return { kind: "box", size: [1.5, 1.5], height: 3, at: [round2(5.25 * Math.cos(a)), 12, round2(5.25 * Math.sin(a))], material: MAT.gold }; });
  const crown = await call("build_compound", { at: [0, 40, 0], scale: 1.5, place: "new", packing: "fit", maxBlocks: 1, material: MAT.plaster, shapes: [
    { kind: "cylinder", radius: 6, height: 12, bands: [{ fromY: 0, toY: 7.5, material: MAT.splaster }, { fromY: 7.5, toY: 9, material: MAT.goldEdge }, { fromY: 10.5, toY: 12, material: MAT.gold }] },
    ...merlons,
  ] }, { soft: true });
  say("crown", crown.err ? crown.text.slice(0, 400) : JSON.stringify({ tipY: crown.json.structureInfo?.tipY, cells: crown.json.cells }));
  // the Crown's gold cap: a low dome in 0.75-unit voxels inside the merlon ring, its top level with the merlons' (55, the
  // tallest point, which sets Sandtide's line)
  const cap = await call("build_compound", { at: [0, 52, 0], scale: 0.75, place: "new", packing: "fit", maxBlocks: 1, material: MAT.gold,
    shapes: [{ kind: "dome", radius: 4.5, height: 3 }] }, { soft: true });
  say("crown cap", cap.err ? cap.text.slice(0, 300) : JSON.stringify({ tipY: cap.json.structureInfo?.tipY, cells: cap.json.cells?.newBlocks }));
  // onion domes on the corner towers (structureInfo.towers: each top floor's centre), in voxels of 0.75 so they read as
  // domes, not stacked tiles (the castle's own 1.5 gives a radius of 3 voxels); a gold finial
  for (const [i, t] of (castle.json.structureInfo?.towers ?? []).entries()) {
    const blue = t.corner === "ne" || t.corner === "sw";      // NE and SW blue mosaic, the other two dome tile
    const r = await call("build_compound", { at: t.at, scale: 0.75, place: "new", packing: "fit", maxBlocks: 1, material: blue ? MAT.blue : MAT.dome,
      shapes: [{ kind: "dome", profile: "onion", radius: 4.5, height: 7.5, bands: [{ fromY: 6, toY: 7.5, material: MAT.gold }] }] }, { soft: true });
    say("dome", i, t.corner, JSON.stringify(t.at), r.err ? r.text.slice(0, 300) : JSON.stringify({ tipY: r.json.structureInfo?.tipY, cells: r.json.cells?.newBlocks }));
  }
  // the courts' floors in star mosaic (the ground at court height inside the curtain): the top voxel of each column,
  // whatever its frame's scale (depthFrom top; by its centre the plinth's 3-unit voxels are 1.5 down)
  say("courts", brief(await call("paint_region", { shapes: [{ min: [-23, -23], max: [23, 23] }], mask: `${COURT + 0.5} - h`, material: MAT.star, depth: 1, depthFrom: "top", units: "map" }, { soft: true }), 200));

  // ---- floating bits ----
  const isl = await call("check_islets", {}, { soft: true });
  for (const pc of (isl.json?.list ?? []).filter((q) => q.debris && q.frames?.length))
    for (const fr of pc.frames) say("debris", JSON.stringify(pc.min), JSON.stringify(pc.max), "frame", fr, brief(await call("fill_box_raw", { frame: fr, state: "empty", units: "map", min: pc.min.map((v) => v - 0.5), max: pc.max.map((v) => v + 0.5) }, { soft: true }), 160));

  // ---- the spawns: areas mode, four sand areas (four knots 3 apart across each pad) and the palace (a knot in each court) ----
  const tang = (x, z) => (x === 0 ? [1, 0] : [0, 1]);
  const areas = SAND_AREAS.map(([name, x, z]) => ({ name, group: "sand", knots: [-4.5, -1.5, 1.5, 4.5].map((k) => ({ xz: [x + tang(x, z)[0] * k, z + tang(x, z)[1] * k] })) }));
  const COURTS = [[7.5, -17.5], [17.5, 7.5], [-7.5, 17.5], [-17.5, -7.5]];      // NE, SE, SW, NW (pinwheel, clear of the wings and keep)
  areas.push({ name: "PALACE", group: "palace", knots: COURTS.map(([x, z]) => ({ xz: [x, z] })) });
  const ps = await call("place_spawn_knots", { mode: "areas", units: "map", snap: "tread", areas, pattern: ["sand", "sand:opposite", "palace", "sand"], opposite: [["SAND W", "SAND E"], ["SAND N", "SAND S"]] });
  fs.writeFileSync(path.join(OUT, "spawns.json"), ps.text);
  say("spawns", JSON.stringify({ deal: ps.json?.deal, warnings: ps.json?.warnings, missing: ps.json?.missingKnots, snapped: ps.json?.snapped }).slice(0, 1500));

  // ---- the sea reads as sand; warm (not orange) land light ----
  // harsh sun: a warm-white diffuse and an ambient of half of it, so the shade sides of the dunes and risers read
  const wl = await call("set_water_look", { preset: "sand", landDiffuse: [255, 250, 238], landAmbient: [128, 125, 119], sea: { albedo: SEA_ALBEDO }, gameDir: process.env.DUSTCROWN_PLAY_DIR ?? GAME });
  say("water look", brief(wl, 500));
  if (wl.json?.warnings?.length) say("WATER LOOK WARNINGS", JSON.stringify(wl.json.warnings));

  // ---- the sand's tones with Sunstone (tint_landscape, a visual channel): the body a warm, less lemon sand, the troughs
  // (path02, A10) a deep ochre, the crests (grass02blend, A04) bleached; with Sunstone off the theme's own textures draw ----
  const tn = await call("tint_landscape", { theme: "ARABIAN", base: "Multi.DesertOasis", tints: [
    { material: "Sand", faces: "all", color: SAND_TINT.body }, { material: "path02", faces: "all", color: SAND_TINT.trough }, { material: "grass02blend", faces: "top", color: SAND_TINT.crest }] });
  say("sand tints", JSON.stringify(tn.json?.tints));

  // ---- scenery copies from the Arabian bases (no collision): a fountain in each court, lamps on the wall-walk's corners ----
  const copies = [
    ...[[17.5, -17.5], [17.5, 17.5], [-17.5, 17.5], [-17.5, -17.5]].map(([x, z]) => ({ resource: "ARABIAN19", xz: [x, z] })),
    ...[[0, -24.5], [24.5, 0], [0, 24.5], [-24.5, 0]].map(([x, z]) => ({ resource: "ARABIAN9", xz: [x, z] })),
  ];
  const sc = await call("copy_base_scenery", { base: "Multi.DesertOasis", units: "map", copies }, { soft: true });
  say("scenery", sc.err ? "REFUSED " + sc.text.slice(0, 400) : brief(sc, 300));

  // ---- the detail layer (sim-neutral): Dustcrown's own sandstone rock meshes in ONE mod mesh bank (build_rock_bank; the
  // bank itself is written into the pack after the export), copies of them round the boulder fields and the mesas' feet.
  // sim_fingerprint before and after: decoration only. --no-detail leaves them out ----
  const ROCKS = [{ name: "Boulder", kind: "boulder" }, { name: "Slab", kind: "slab" }, { name: "Wedge", kind: "wedge" }, { name: "Heap", kind: "scree", scree: { rocks: 9 } }];
  const BANK = { id: MOD, palette: "sandstone", seed: 5, rocks: ROCKS };
  const fpBefore = await call("sim_fingerprint", {});
  let detail = { pieces: 0 };
  if (!flag("--no-detail")) {
    const rb = await call("build_rock_bank", { ...BANK, dryRun: true });
    const boxes = Object.fromEntries(rb.json.meshes.map((m) => [m.name, m.box]));
    let s = 77;
    const rnd = () => { s = (s * 16807) % 2147483647; return s / 2147483647; };
    const byMesh = {};
    const add = (name, x, z, sc) => (byMesh[name] ??= []).push({ xz: [round1(x), round1(z)], rot: [0, Math.round(rnd() * 360), 0], scale: [sc, round2(sc * (0.8 + rnd() * 0.3)), sc] });
    for (const [fx, fz] of FIELDS) for (let i = 0; i < 7; i++) {
      const a = rnd() * Math.PI * 2, d = 3 + rnd() * 8;
      add(`${MOD}.${["Boulder", "Wedge", "Slab", "Heap"][i % 4]}`, fx + Math.cos(a) * d, fz + Math.sin(a) * d, round2(1.2 + rnd() * 1.2));
    }
    for (const m of MESAS) for (let i = 0; i < (m.refuge ? 6 : 3); i++) {
      const a = rnd() * Math.PI * 2, d = mesaR(m) + 1.5 + rnd() * 2.5;
      add(`${MOD}.${i % 2 ? "Heap" : "Boulder"}`, m.at[0] + Math.cos(a) * d, m.at[1] + Math.sin(a) * d, round2(1.3 + rnd()));
    }
    for (const [name, pieces] of Object.entries(byMesh)) {
      const r = await call("place_mod_mesh", { resource: name, box: boxes[name], units: "map", pieces }, { soft: true });
      detail.pieces += r.json?.placed ?? 0;
      say("detail", name, r.err ? r.text.slice(0, 300) : `${r.json.placed} placed${r.json.skipped ? `, ${r.json.skipped.length} skipped` : ""}`);
    }
    const fpAfter = await call("sim_fingerprint", { against: fpBefore.json });
    detail = { ...detail, simSame: fpAfter.json?.sim === fpBefore.json?.sim };
    if (!detail.simSame) throw new Error(`the detail layer changed the sim fingerprint: ${fpAfter.text.slice(0, 600)}`);
  }
  say("detail layer", JSON.stringify(detail));

  // No level script of its own beyond the water look block: the sea is the scheme's sudden death
  const sv = await call("save");
  say("save", brief(sv, 900));
  if (sv.json?.warnings?.length) say("SAVE WARNINGS", JSON.stringify(sv.json.warnings));
  for (const k of ["pits", "waterLook", "spawns", "factory"]) if (sv.json?.[k]?.length) say(`SAVE ADVISORY ${k}`, JSON.stringify(sv.json[k]));
  if (sv.json?.drape?.pokes?.length) say("SAVE ADVISORY drape.pokes", JSON.stringify(sv.json.drape.warnings));
  const d = await call("describe_level", { limit: 100 });
  fs.writeFileSync(path.join(OUT, "describe.json"), d.text);
  say("cells", JSON.stringify(d.json.limits.voxelCells), "blocks", JSON.stringify(d.json.limits.newFrames), "engineWarnings", JSON.stringify(d.json.engineWarnings), "problems", JSON.stringify(d.json.problems ?? []));
  say("landBounds", JSON.stringify(d.json.landBounds));
  const lo = await call("list_objects", { units: "map", outsideFrames: "summary" });
  fs.writeFileSync(path.join(OUT, "objects.json"), lo.text);

  // ---- the sand line (DESIGN.md 1): check_height_line at the design's fixed line, and Sandtide's preset from the land box
  // (landBounds leaves out the emptied base frames, as the game's box does; the tester still reads `sandtide: sand line at
  // Y` from the game's log) ----
  const LINE_MAP = LINE_W / 20;
  const hl = await call("check_height_line", { y: LINE_MAP, units: "map", limit: 40 });
  fs.writeFileSync(path.join(OUT, "heightline.json"), hl.text);
  say("sand line", LINE_MAP, JSON.stringify({ spawns: { below: hl.json.spawns.below, near: hl.json.spawns.near, above: hl.json.spawns.above }, near: hl.json.surfaces?.near, flagged: hl.json.flagged, warnings: hl.json.warnings }));
  say("near-line groups", JSON.stringify((hl.json.surfaces?.nearGroups ?? []).slice(0, 12)));
  const hp = await call("check_height_line", { preset: "sandtide" });
  say("sandtide preset (tool's box)", JSON.stringify({ line: hp.json.line, landBounds: hp.json.landBounds }));
  if (Math.abs((hp.json.line?.world ?? 0) - LINE_W) > 20) throw new Error(`Sandtide's line by the land box is ${hp.json.line?.world} w, over 20 from the design's ${LINE_W}`);

  // ---- reachability on foot: every sand area to every other and to the palace's foot; none to step 2 ----
  const PADS = SAND_AREAS.map(([name, x, z]) => ({ name, x, z, radius: 4 }));
  const FEET = [["foot N", 0, -45], ["foot E", 45, 0], ["foot S", 0, 45], ["foot W", -45, 0]].map(([name, x, z]) => ({ name, x, z, radius: 2 }));
  // ---- pits: no hollow a worm walks into and cannot leave ----
  const pits = await call("check_pits", { bodyWidth: WORM_W }, { soft: true });
  fs.writeFileSync(path.join(OUT, "pits.json"), pits.text);
  say("pits", pits.err ? pits.text.slice(0, 400) : JSON.stringify({ ok: pits.json.ok, summary: pits.json.summary, pits: (pits.json.pits ?? []).slice(0, 6) }).slice(0, 1200));
  const reach = await call("check_reachability", { maxSlope: 45, bodyWidth: WORM_W, slopeOver: "skin", traps: "report", targets: [...PADS, ...FEET], targetMode: "each", render: true }, { soft: true });
  img(reach, "reach-foot");
  fs.writeFileSync(path.join(OUT, "reach-foot.json"), reach.text);
  say("reach on foot", brief(reach, 900));
  const STEP2 = [["step2 N", 0, -33], ["step2 E", 33, 0], ["step2 S", 0, 33], ["step2 W", -33, 0]].map(([name, x, z]) => ({ name, x, z, radius: 1.5 }));
  const r2 = await call("check_reachability", { maxSlope: 45, bodyWidth: WORM_W, targets: STEP2, targetMode: "each" }, { soft: true });
  fs.writeFileSync(path.join(OUT, "reach-step2.json"), r2.text);
  say("reach step 2 on foot (must be none from the sand)", brief(r2, 900));
  // ---- with tools: each sand area to a court (jetpack, rope, girder estimates) ----
  const COURT_T = COURTS.map(([x, z], i) => ({ name: `court ${["NE", "SE", "SW", "NW"][i]}`, x, z, radius: 2.5 }));
  const tr = await call("check_tool_reach", { targets: COURT_T, render: true }, { soft: true });
  img(tr, "tool-reach");
  fs.writeFileSync(path.join(OUT, "tool-reach.json"), tr.text);
  say("tool reach", tr.err ? tr.text.slice(0, 600) : JSON.stringify(tr.json.summary).slice(0, 1500));
  const is = await call("check_islets", {}, { soft: true });
  fs.writeFileSync(path.join(OUT, "islets.json"), is.text);
  say("islets", brief(is, 400));

  // ---- the tester's waypoints: each sand area's walk to the palace's nearest foot, the palace spawns' walk to the next court ----
  const waypoints = [];
  const walk = async (label, from, to, target) => {
    const w = await call("check_reachability", { maxSlope: 45, bodyWidth: WORM_W, slopeOver: "skin", centre: true, from: [from], target }, { soft: true });
    const sp = w.json?.spawns?.[0] ?? w.json?.starts?.[0] ?? null;
    waypoints.push({ area: label, from, to, reachable: sp?.reachable ?? null, length: sp?.path?.length ?? null, waypoints: sp?.waypoints ?? sp?.path?.waypoints ?? null });
  };
  for (const [i, [name, x, z]] of SAND_AREAS.entries()) { const f = FEET[[3, 1, 0, 2][i]]; await walk(name, [x, SAND, z], f.name, { x: f.x, z: f.z, radius: f.radius }); }
  for (const [i, [x, z]] of COURTS.entries()) { const [nx, nz] = COURTS[(i + 1) % 4]; await walk(`PALACE court ${["NE", "SE", "SW", "NW"][i]}`, [x, COURT, z], `court ${["NE", "SE", "SW", "NW"][(i + 1) % 4]}`, { x: nx, z: nz, radius: 1.5 }); }
  fs.writeFileSync(path.join(OUT, "waypoints.json"), JSON.stringify(waypoints, null, 1));
  say("waypoints", JSON.stringify(waypoints.map((w) => [w.area, w.to, w.reachable, w.length, (w.waypoints ?? []).length])));

  // ---- renders ----
  await shot("topdown", "render_topdown", { step: 1, scale: 3 });
  await shot("overview-se", "render_view", { view: "se", fog: 900, antialias: true });
  await shot("overview-nw", "render_view", { view: "nw", fog: 900, antialias: true });
  await shot("palace-se", "render_view", { pos: [90, 70, 90], target: [0, 26, 0], fov: 50, fog: 900, antialias: true });
  await shot("palace-nw", "render_view", { pos: [-80, 45, -95], target: [0, 28, 0], fov: 50, fog: 900, antialias: true });
  await shot("palace-courts", "render_view", { pos: [34, 70, 46], target: [0, 24, 0], fov: 55, antialias: true });
  for (const [name, x, z] of SAND_AREAS) {
    const k = Math.hypot(x, z), ux = x / k, uz = z / k;
    await shot(`area-${name.split(" ")[1]}`, "render_view", { pos: [round1(x + ux * 14), 12, round1(z + uz * 14)], target: [0, 26, 0], fov: 55, fog: 900, antialias: true });
  }
  await shot("refuge-NW", "render_view", { pos: [-95, 30, -20], target: [-60, 12, -60], fov: 50, fog: 900, antialias: true });
  for (const [name, args] of [["side-ew", { x0: -BOX, z0: 0, x1: BOX, z1: 0 }], ["side-diag", { x0: -BOX * 0.7, z0: -BOX * 0.7, x1: BOX * 0.7, z1: BOX * 0.7 }]]) {
    await shot(name, "render_side", { ...args, y0: -15, y1: 60, scale: 6, units: "map", lines: [{ y: LINE_MAP, colour: [220, 40, 30], label: `sand line ${LINE_W} w` }] });
  }
  fs.writeFileSync(path.join(OUT, "heightmap.txt"), (await call("heightmap_ascii", { step: 3 })).text);

  // ---- source export, copied next to this script, with the one mesh bank ----
  if (!flag("--no-export")) {
    const modDir = path.join(GAME, "Mods", MOD);
    if (fs.existsSync(modDir)) throw new Error(`${modDir} exists: the export writes there; move it aside first`);
    const ex = await call("export", { modId: MOD, name: "Dustcrown", version: VERSION, mode: "source", survivor: false });
    say("export", brief(ex, 600));
    const pack = path.join(HERE, "pack");
    fs.rmSync(pack, { recursive: true, force: true });
    fs.cpSync(modDir, pack, { recursive: true });
    if (!flag("--keep-mod")) fs.rmSync(modDir, { recursive: true, force: true });
    fs.rmSync(path.join(pack, "assets", "levels"), { recursive: true, force: true });   // built per machine
    if (detail.pieces) {
      const meshDir = path.join(pack, "assets", "meshes");
      const rb = await call("build_rock_bank", { ...BANK, out: meshDir, gameDir: GAME });
      say("bank", brief(rb, 400));
      for (const f of fs.readdirSync(meshDir)) if (path.join(meshDir, f) !== path.resolve(rb.json.file)) fs.rmSync(path.join(meshDir, f), { recursive: true, force: true });
      const spiceFile = path.join(pack, "spice.json"), spice = JSON.parse(fs.readFileSync(spiceFile, "utf8"));
      spice.meshes = rb.json.spice.meshes;
      fs.writeFileSync(spiceFile, JSON.stringify(spice));
    }
    // the export wrote the range the level's features need (gate.needs: spawn areas, the pack's own meshes)
    if (!ex.json?.gate?.gated) throw new Error(`the export's range is not gated: ${JSON.stringify(ex.json?.gate)}`);
    fs.writeFileSync(path.join(pack, ".gitignore"), "assets/levels/\n");
    say("pack", pack, JSON.stringify(ex.json?.gate ?? null));
  }
  await call("close", { discard: true }, { soft: true });
} catch (e) {
  if (!String(e.message).startsWith("--plan")) throw e;
  say(e.message);
} finally {
  fs.writeFileSync(path.join(OUT, "calls.json"), JSON.stringify(log, null, 1));
  say("MCP tool calls:", stats.calls);
  await stop();
}
