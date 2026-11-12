// Ships the DRESSED Dustcrown: the saved project (after build.mjs and detail.mjs) exported as a source pack into pack\, with
// the one mesh bank (meshes/bank.mjs, built by build_rock_bank) in its assets\meshes\ and listed under spice.json `meshes`,
// so the installed pack draws the detail layer (without the bank every dustcrown.* copy draws nothing). Run it with the game
// in MELANGE_GAME_DIR closed (default testenv C; the export writes <game>\Mods\dustcrown, which must not exist):
//   tools\node\node.exe tools\erg-mcp\bench\dustcrown\ship.mjs [--out <pack dir>] [--project dustcrown] [--version 1.0.0]
//     [--check]   export to a scratch folder instead and compare it with the committed pack (see below)
// --check proves the layer neutral from the files: the new level patch, with every `add` op of a detail the layer placed (names
// ending _dtl1 or _pal1) removed, must equal the committed pack's patch when that was written by build.mjs (the undressed one);
// run it BEFORE shipping over a pack build.mjs wrote.
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { startServer, ROOT } from "../caldera/mcpclient.mjs";
import { BANK } from "./meshes/bank.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const argv = process.argv.slice(2);
const opt = (f, d) => { const i = argv.indexOf(f); return i >= 0 ? argv[i + 1] : d; };
const OUT = path.resolve(opt("--out", path.join(HERE, "pack")));
const ID = opt("--project", process.env.DUSTCROWN_ID || "dustcrown"), MOD = "dustcrown", VERSION = opt("--version", "1.0.0");
// the palette pass's carriers (detail-passes.mjs PALACE_TINTS: stone A13, sandy plaster A16, Gold A26, GoldEdging A27, BlueMosaic A39, StarMosaic A31,
// darkyellowplaster A58, and the dunes' four A10, A04, A08, A03, which a re-tint re-seats lower: the carrier's y is not the sim), named by their textures' digests
const LAYER_TINT = /_ltint(15403853c992d13f|54f121563df677f4|8de26622f4a709d7|20a0e64a45901dab|4347f46919b17fe6|646c35be32f39798|ab43e0b579b2fbd8|2479eddfbf9ec34e|35271f0d5eaa25c3|f6d002160c7246b7|cf7a74ae6a495453)[0-9a-f]{6}$/;
const GAME = process.env.MELANGE_GAME_DIR || "C:/Users/Jamin/Desktop/WUMFix/testenv/C";
process.env.MELANGE_EXE ??= path.join(ROOT, "build/x86-release/Melange.exe");
const say = (...a) => console.log(new Date().toISOString().slice(11, 19), ...a);
const modDir = path.join(GAME, "Mods", MOD);
if (fs.existsSync(modDir)) throw new Error(`${modDir} exists: the export writes there; move it aside first`);

const { call, stop } = await startServer();
let gate = null;
try {
  await call("connect", { gameDir: GAME });
  await call("open_project", { id: ID });
  // the level's picture in the game's map lists (a session's state, not the project's: build.mjs sets it before its export
  // too): the level camera low over the SW, drawn with the level's own sea and haze and the layer's meshes
  await call("set_level_camera", { view: "sw", elevation: 20, target: [0, 18, 0], fit: "island", margin: 1 });
  say("thumbnail", (await call("set_level_thumbnail", { render: { antialias: true, tints: true, waterLook: true, scenery: true }, size: 512 }, { soft: true })).text.slice(0, 120).replace(/\s+/g, " "));
  const ex = await call("export", { modId: MOD, name: "Dustcrown", version: VERSION, mode: "source", survivor: false, timeOfDay: "DAY" });
  gate = ex.json?.gate ?? null;
  say("export", ex.text.slice(0, 300));
  if (!gate?.gated) throw new Error(`the export's range is not gated: ${JSON.stringify(gate)}`);
  await call("close", { discard: true }, { soft: true });
  // the bank: every mesh of meshes/bank.mjs, in one file (build_rock_bank converts and merges them)
  const stage = fs.mkdtempSync(path.join(os.tmpdir(), "dustcrown-ship-"));
  const rb = await call("build_rock_bank", { ...BANK, out: stage, gameDir: GAME });
  say("bank", rb.text.slice(0, 200).replace(/\s+/g, " "));
  const levelPatch = path.join(modDir, "src", "dustcrown.ergpatch.json");
  if (argv.includes("--check")) {
    const strip = (j) => JSON.stringify(JSON.parse(j), (k, v) => (k === "ops" && Array.isArray(v) ? v.filter((o) => !(o.op === "add" && (/_(dtl1|pal1)(_tint[0-9a-f]{6})?$/.test(o.detail?.name ?? "") || LAYER_TINT.test(o.detail?.name ?? "")))) : v));
    const mine = strip(fs.readFileSync(levelPatch, "utf8")), was = strip(fs.readFileSync(path.join(OUT, "src", "dustcrown.ergpatch.json"), "utf8"));
    if (process.env.DUSTCROWN_CHECK_DUMP) { fs.writeFileSync(path.join(process.env.DUSTCROWN_CHECK_DUMP, "check-mine.json"), mine); fs.writeFileSync(path.join(process.env.DUSTCROWN_CHECK_DUMP, "check-was.json"), was); }
    say("check", mine === was ? "the patch without the layer's details is the committed one" : `DIFFERENT (${mine.length} against ${was.length} bytes)`);
    fs.rmSync(modDir, { recursive: true, force: true }); fs.rmSync(stage, { recursive: true, force: true });
    if (mine !== was) process.exitCode = 1;
  } else {
    fs.rmSync(OUT, { recursive: true, force: true });
    fs.cpSync(modDir, OUT, { recursive: true });
    fs.rmSync(modDir, { recursive: true, force: true });
    fs.rmSync(path.join(OUT, "assets", "levels"), { recursive: true, force: true });   // built per machine
    const meshDir = path.join(OUT, "assets", "meshes");
    fs.rmSync(meshDir, { recursive: true, force: true });
    fs.mkdirSync(meshDir, { recursive: true });
    fs.copyFileSync(path.resolve(rb.json.file), path.join(meshDir, path.basename(rb.json.file)));
    fs.rmSync(stage, { recursive: true, force: true });
    const spiceFile = path.join(OUT, "spice.json"), spice = JSON.parse(fs.readFileSync(spiceFile, "utf8"));
    spice.meshes = [{ file: `assets/meshes/${path.basename(rb.json.file)}` }];
    fs.writeFileSync(spiceFile, JSON.stringify(spice));
    fs.writeFileSync(path.join(OUT, ".gitignore"), "assets/levels/\n");
    say("pack", OUT, JSON.stringify({ meshes: spice.meshes, range: spice.melange?.range, gate }));
  }
} finally { await stop(); }
