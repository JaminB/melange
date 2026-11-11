// The detail layer's channel probe and contact sheet: dresses the saved project with one of each candidate channel
// (scenery copies from other themes' bases, an orange point light and the volcano plume in the crater, steam vents,
// bonfire smoke and a smoke plume on the flanks, a steam vent beside the Strand castle, wind and creaking-tree sounds, a
// primary repaint of a patch of the Seawall court) plus the contact sheet: the ~30 candidate rock and flora pieces in a
// grid floating over the sea south of the island, each scaled to about 4 map units, with a camera pose per piece. A test
// only: it is never part of the shipped layer. Checks the sim_fingerprint before and after (it must not change) and saves.
//
//   tools\node\node.exe tools\erg-mcp\bench\caldera\detail-probe.mjs --out <dir> [--project caldera] [--url <a running game's url>] [--no-sheet] [--no-channels] [--dry]
//
// Writes <out>\probe.json (what was placed, the fingerprints) and <out>\sheet-cams.json (one pose per sheet piece, map units).
import fs from "node:fs";
import path from "node:path";
import { startServer, ROOT } from "./mcpclient.mjs";
import * as T from "./terrain.mjs";

const argv = process.argv.slice(2);
const opt = (f, d) => { const i = argv.indexOf(f); return i >= 0 ? argv[i + 1] : d; };
const OUT = path.resolve(opt("--out", "out/probe")), ID = opt("--project", "caldera");
const GAME = process.env.MELANGE_GAME_DIR || "C:/Users/Jamin/Desktop/WUMFix/testenv/D";
process.env.MELANGE_EXE ??= path.join(ROOT, "build/x86-release/Melange.exe");
fs.mkdirSync(OUT, { recursive: true });

// the contact sheet: [label, base, resource] (scenery_catalog's rows; the first piece of that resource in that base)
export const SHEET = [
  ["rock ARABIAN6", "Multi.DesertOasis", "ARABIAN6"], ["rock ARABIAN6 small", "Multi.NoRoomForError", "ARABIAN6"],
  ["rock ARABIAN16", "Multi.TraitorousWaters", "ARABIAN16"], ["small rocks WILDWEST6", "Multi.DoomCanyon", "WILDWEST6"],
  ["rubble WILDWEST6", "Multi.MineAllMine", "WILDWEST6"], ["skull WILDWEST8", "Multi.GhostHill", "WILDWEST8"],
  ["eggs PREHISTORIC12", "Multi.ValleyOfDinoWorms", "PREHISTORIC12"], ["eggs PREHISTORIC16", "Multi.ValleyOfDinoWorms", "PREHISTORIC16"],
  ["eggs PREHISTORIC9", "Multi.ValleyOfDinoWorms", "PREHISTORIC9"], ["eggs PREHISTORIC11", "Multi.ValleyOfDinoWorms", "PREHISTORIC11"],
  ["untitled D07_04", "Multi.NoRoomForError", "D07_04"], ["pirate Pirate5", "Multi.W3DOkCoraleReef", "Pirate5"],
  ["horror object HORROR20", "Multi.W3DGraveDanger", "HORROR20"], ["horror Horror9", "Multi.W3DWhenAnnelidsCollide", "Horror9"],
  ["grass PREHISTORIC1", "Multi.Escape", "PREHISTORIC1"], ["flower PREHISTORIC8", "Multi.LandWormsForgot", "PREHISTORIC8"],
  ["cactus WILDWEST1", "Multi.MineAllMine", "WILDWEST1"], ["cactus WILDWEST2", "Multi.GhostHill", "WILDWEST2"],
  ["grass WILDWEST18", "Multi.GhostHill", "WILDWEST18"], ["grass CAMELOT1", "Multi.ThisGameDoesDragon", "CAMELOT1"],
  ["grass HORROR1", "Multi.StormTheCastle", "HORROR1"], ["shrooms CAMELOT10", "Multi.WindyWizard", "CAMELOT10"],
  ["yellow flower CAMELOT3", "Multi.WindyWizard", "CAMELOT3"], ["leafy flower HORROR8", "Multi.WindyWizard", "HORROR8"],
  ["grass ENGLAND3", "Multi.W3DAllCoopedUp", "ENGLAND3"], ["grass ENGLAND2", "Multi.W3DEarnYourCrust", "ENGLAND2"],
  ["grass PIRATE1", "Multi.W3DShiverMeTimbers", "PIRATE1"], ["weed BUILDING5", "Multi.Tutorial1", "BUILDING5"],
  ["flower BUILDING13", "Multi.Tutorial1", "BUILDING13"], ["grass WAR3", "Multi.W3DHoldUntilRelieved", "WAR3"],
  ["grass LUNAR1", "Multi.W3DAlienJuiceSuckers", "LUNAR1"], ["england England1", "Multi.W3DSchoolsInForSummer", "England1"],
];
// the channels: [label, base, search (scenery_catalog search: the first detail whose name holds it), where [x, z] (map), y over the ground]
const crater = T.pointAt(8, 2.6), flankW = T.pointAt(52, 2.3), flankE = T.pointAt(50, 0.25), flankN = T.pointAt(55, -2.2);
const strand = T.CASTLES.find((c) => c.name === "Strand"), strandOut = [strand.at[0] + 14, strand.at[1] - 6];
export const CHANNELS = [
  ["light orange 255 100 0 300", "Multi.ValleyOfDinoWorms", "PNTLGHT 255 100 0 300", crater, 6],
  ["volcano plume smoke", "Multi.ValleyOfDinoWorms", "VolcanoPlumeSmoke", crater, 2],
  ["steam vent (flank W)", "Multi.Tutorial3", "SteamVent", flankW, 0.5],
  ["bonfire tall (flank E)", "Multi.ValleyOfDinoWorms", "BonfireTall", flankE, 0.5],
  ["smoke steamy thick (flank N)", "Multi.Saboteurs", "Smoke_SteamyThick", flankN, 0.5],
  ["smoke a (crater)", "Multi.LandWormsForgot", "smokea", T.pointAt(14, 0.4), 1],
  ["steam vent (Strand)", "Multi.Tutorial3", "SteamVent", strandOut, 0.5],
  ["sound wind", "Multi.WindyWizard", "Wind", flankW, 3],
  ["sound creaking tree", "Multi.W3DTreeVillageTrouble", "SndCreakingTree", flankE, 3],
];

const r2 = (v) => Math.round(v * 100) / 100;
const { call, stop } = await startServer();
const report = { project: ID, sheet: [], channels: [], errors: [] };
try {
  await call("connect", opt("--url") ? { url: opt("--url") } : { gameDir: GAME });
  await call("open_project", { id: ID });
  const fp0 = (await call("sim_fingerprint", {})).json;
  report.before = { sim: fp0.sim, visual: fp0.visual };
  if (!argv.includes("--no-sheet")) {
    const bases = [...new Set(SHEET.map((s) => s[1]))];
    const cat = (await call("scenery_catalog", { bases, roles: ["scenery"], limit: 2000 })).json.list;
    const copies = [], cams = [];
    SHEET.forEach(([label, base, res], i) => {
      const row = cat.find((r) => r.base === base && r.resource === res);
      if (!row) { report.errors.push(`sheet ${label}: no ${res} in ${base}`); return; }
      const col = i % 6, line = Math.floor(i / 6), x = -35 + col * 14, z = 140 + line * 14, y = 8;
      const big = Math.max(...(row.size ?? [1, 1, 1]), 0.2), k = 4 / big;
      const scale = row.example.scale.map((v) => r2(v * k));
      copies.push({ base, id: row.example.id, pos: [x, y, z], scale, label, size: row.size, meshSize: row.meshSize });
      cams.push({ name: `sheet${String(i).padStart(2, "0")}`, label, base, resource: res, size: row.size, pos: [x, y + 4, z - 10], target: [x, y + 1, z], fov: 45 });
    });
    for (const base of bases) {
      const mine = copies.filter((c) => c.base === base);
      if (!mine.length) continue;
      const r = await call("copy_base_details", { base, allowOtherTheme: true, units: "map", copies: mine.map((c) => ({ id: c.id, pos: c.pos, scale: c.scale })) }, { soft: true });
      if (r.err) report.errors.push(`sheet copy from ${base}: ${r.text.slice(0, 300)}`);
      else report.sheet.push(...mine.map((c) => ({ label: c.label, base, id: c.id, at: c.pos, scale: c.scale, size: c.size })));
    }
    fs.writeFileSync(path.join(OUT, "sheet-cams.json"), JSON.stringify(cams, null, 1));
  }
  if (!argv.includes("--no-channels")) {
    for (const [label, base, search, [x, z], dy] of CHANNELS) {
      const rows = (await call("scenery_catalog", { bases: [base], search, sizes: false, limit: 50 })).json.list;
      const hit = rows.map((r) => r.example).find((e) => e.name.includes(search));
      if (!hit) { report.errors.push(`channel ${label}: no detail named with ${search} in ${base}`); continue; }
      const y = T.island(x, z) + dy;
      const r = await call("copy_base_details", { base, allowOtherTheme: true, units: "map", copies: [{ id: hit.id, pos: [r2(x), r2(y), r2(z)] }] }, { soft: true });
      if (r.err) report.errors.push(`channel ${label}: ${r.text.slice(0, 300)}`);
      else report.channels.push({ label, base, id: hit.id, name: hit.name, at: [r2(x), r2(y), r2(z)] });
    }
    // a primary repaint: the middle of the Seawall court (flint) to ash, under team 3's spawns, so the Test shows whether a worm
    // lands and stands the same on it. (paint_region and sculpt cannot repaint Caldera's draped skin: its words carry a second
    // material and the editor's paint check refuses them, docs/erg-mcp.md "Detail layer"; the castles' blocks have none.)
    const sw = T.CASTLES.find((c) => c.name === "Seawall");
    const pr = await call("sculpt", { mode: "paint", shape: "box", size: [4, 1, 4], center: [r2(sw.at[0] + 3), sw.court - 0.5, r2(sw.at[1] + 3)], material: 49, units: "map", allFrames: true }, { soft: true });
    if (pr.err) report.errors.push(`paint: ${pr.text.slice(0, 300)}`);
    else report.paint = { at: [r2(sw.at[0] + 3), sw.court - 0.5, r2(sw.at[1] + 3)], size: [4, 1, 4], material: 49, changed: pr.json?.changed ?? null };
  }
  const fp1 = (await call("sim_fingerprint", { against: fp0 })).json;
  report.after = { sim: fp1.sim, visual: fp1.visual, against: fp1.against, counts: fp1.counts };
  console.log("sim", fp1.against.same ? "UNCHANGED" : "CHANGED", JSON.stringify(fp1.against));
  if (!fp1.against.same) throw new Error("the probe changed the sim");
  if (!argv.includes("--dry")) report.save = (await call("save")).json?.projectHash;
  await call("close", { discard: true });
} finally {
  fs.writeFileSync(path.join(OUT, "probe.json"), JSON.stringify(report, null, 1));
  await stop();
}
console.log(JSON.stringify({ sheet: report.sheet.length, channels: report.channels.length, errors: report.errors, save: report.save }, null, 1));
