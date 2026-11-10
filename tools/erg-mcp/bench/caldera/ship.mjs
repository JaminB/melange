// Ships the DRESSED Caldera (round 11; DETAIL.md "Shipping"): the saved project with the detail layer exported as a source
// pack, Caldera's mesh banks built into its assets/meshes/ and listed under spice.json `meshes`, so the installed pack draws
// the rock layer (without the banks every caldera.* copy draws nothing). Run after build.mjs then detail.mjs, the game in
// MELANGE_GAME_DIR closed (default testenv D):
//   tools\node\node.exe tools\erg-mcp\bench\caldera\ship.mjs [--out <pack dir>] [--project caldera] [--version 2.1.0]
// --out defaults to pack\ next to this script (the committed pack). Only the banks whose meshes the level names are shipped
// (meshes/gen.mjs `ship`), and the script refuses when the level names a caldera.* mesh the pack does not carry.
import fs from "node:fs";
import path from "node:path";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { startServer, ROOT } from "./mcpclient.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const argv = process.argv.slice(2);
const opt = (f, d) => { const i = argv.indexOf(f); return i >= 0 ? argv[i + 1] : d; };
const OUT = path.resolve(opt("--out", path.join(HERE, "pack")));
const ID = opt("--project", "caldera"), MOD = "caldera", VERSION = opt("--version", "2.1.0");
const GAME = process.env.MELANGE_GAME_DIR || "C:/Users/Jamin/Desktop/WUMFix/testenv/D";
process.env.MELANGE_EXE ??= path.join(ROOT, "build/x86-release/Melange.exe");
const say = (...a) => console.log(new Date().toISOString().slice(11, 19), ...a);

const modDir = path.join(GAME, "Mods", MOD);
if (fs.existsSync(modDir)) throw new Error(`${modDir} exists: the export writes there; move it aside first`);
const { call, stop } = await startServer();
let ship, used;
try {
  await call("connect", { gameDir: GAME });
  await call("open_project", { id: ID });
  // the caldera.* meshes the level names
  used = Object.keys((await call("list_scenery", { search: "caldera.", limit: 1 })).json.byResource ?? {}).filter((r) => r.startsWith(`${MOD}.`)).sort();
  const ex = await call("export", { modId: MOD, name: "Caldera", version: VERSION, mode: "source", survivor: false });
  ship = ex.json?.gate ?? null;
  say("export", ex.text.slice(0, 300));
  await call("close", { discard: true }, { soft: true });
} finally { await stop(); }

// the banks (meshes/build.mjs: procedural and deterministic)
const banks = path.join(OUT, "..", ".ship-banks");
execFileSync(path.join(ROOT, "tools/node/node.exe"), [path.join(HERE, "meshes/build.mjs"), banks, "--game", GAME], { stdio: "inherit" });
const info = JSON.parse(fs.readFileSync(path.join(banks, "meshes.json"), "utf8"));
const missing = used.filter((r) => !info[r] || info[r].ship === false);
if (missing.length) throw new Error(`the level names ${missing.join(", ")} but the pack would not ship it (meshes/gen.mjs ship)`);

fs.rmSync(OUT, { recursive: true, force: true });
fs.cpSync(modDir, OUT, { recursive: true });
fs.rmSync(modDir, { recursive: true, force: true });
fs.rmSync(path.join(OUT, "assets", "levels"), { recursive: true, force: true });   // built per machine by build.ps1
fs.mkdirSync(path.join(OUT, "assets", "meshes"), { recursive: true });
const meshes = [];
for (const r of used) {
  fs.copyFileSync(path.join(banks, `${r}.xom`), path.join(OUT, "assets", "meshes", `${r}.xom`));
  meshes.push({ file: `assets/meshes/${r}.xom` });
}
fs.rmSync(banks, { recursive: true, force: true });
const spiceFile = path.join(OUT, "spice.json"), spice = JSON.parse(fs.readFileSync(spiceFile, "utf8"));
spice.meshes = meshes;
fs.writeFileSync(spiceFile, JSON.stringify(spice));
// the committed pack keeps its level builds out of git, but its mesh banks in (they are the shipped models)
fs.writeFileSync(path.join(OUT, ".gitignore"), "assets/levels/\n");
say("pack", OUT, JSON.stringify({ meshes, range: spice.melange?.range, gate: ship }));
