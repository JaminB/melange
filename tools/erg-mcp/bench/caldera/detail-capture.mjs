// The detail layer's shots: the fixed camera set (detail-cams.mjs) of the saved project, offline (render_view: voxels
// only, no scenery meshes, no lights or particles) or in a running Test (game_shots: what players see, Sunstone on when
// the game has it). Every file is <prefix>_<cam>.png in --out, so rounds and before/after pairs never overwrite.
//
//   tools\node\node.exe tools\erg-mcp\bench\caldera\detail-capture.mjs --mode render --prefix caldet_r0 --out <dir> [--build-out <dir>] [--project caldera]
//   tools\node\node.exe tools\erg-mcp\bench\caldera\detail-capture.mjs --mode game --url <the test game's launch url> --prefix caldet_r0 --out <dir> [--start] [--only a,b]
//
// render connects like build.mjs (MELANGE_GAME_DIR with the game closed; default testenv D). game needs the running game's
// own url (never the shared oasis_url.txt when two test games run): --start starts a Test of the project first (four
// human teams of two, sudden death off) and ends it after. The game's shots folder copies are moved into --out.
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { startServer, ROOT } from "./mcpclient.mjs";
import { cameras } from "./detail-cams.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const argv = process.argv.slice(2);
const opt = (f, d) => { const i = argv.indexOf(f); return i >= 0 ? argv[i + 1] : d; };
const MODE = opt("--mode", "render"), PREFIX = opt("--prefix", "caldet_r0"), ID = opt("--project", "caldera");
const OUT = path.resolve(opt("--out", path.join(HERE, "out", "detail")));
const BUILD_OUT = opt("--build-out", path.join(HERE, "out"));
const ONLY = opt("--only", "") ? opt("--only").split(",") : null;
const GAME = process.env.MELANGE_GAME_DIR || "C:/Users/Jamin/Desktop/WUMFix/testenv/D";
process.env.MELANGE_EXE ??= path.join(ROOT, "build/x86-release/Melange.exe");
fs.mkdirSync(OUT, { recursive: true });

const cams = cameras(BUILD_OUT).filter((c) => !ONLY || ONLY.includes(c.name));
fs.writeFileSync(path.join(OUT, `${PREFIX}_cams.json`), JSON.stringify(cams, null, 1));
const { call, stop } = await startServer();
const written = [];
try {
  if (MODE === "render") {
    await call("connect", { gameDir: GAME });
    await call("open_project", { id: ID });
    for (const c of cams) {
      const r = await call("render_view", { pos: c.pos, target: c.target, fov: c.fov, units: "map", fog: 900, width: 960, height: 540, antialias: true }, { soft: true });
      if (r.err || !r.images[0]) { console.log(c.name, "ERR", r.text.slice(0, 300)); continue; }
      const f = path.join(OUT, `${PREFIX}_${c.name}.png`);
      fs.writeFileSync(f, Buffer.from(r.images[0].data, "base64"));
      written.push(f);
    }
    await call("close", { discard: true }, { soft: true });
  } else if (MODE === "game") {
    const url = opt("--url");
    if (!url) throw new Error("--mode game needs --url (the running test game's launch url)");
    await call("connect", { url });
    await call("open_project", { id: ID });
    if (argv.includes("--start")) {
      const t = await call("test", { teams: [0, 1, 2, 3].map(() => ({ human: true, worms: 2 })), timeoutSec: 240, suddenDeath: false }, { soft: true });
      console.log("test", t.err ? "ERR " + t.text.slice(0, 400) : "ok");
    }
    const r = await call("game_shots", { prefix: PREFIX, images: false, waitPlay: true, waitSec: 60, units: "map", width: 1280,
      cams: cams.map((c) => ({ name: c.name, pos: c.pos, target: c.target, fov: c.fov })) }, { soft: true });
    if (r.err) console.log("game_shots ERR", r.text.slice(0, 600));
    const shotsDir = path.join(os.homedir(), "Documents", "Melange", "shots");
    for (const s of r.json?.shots ?? []) {
      const p = s.path ?? s.file;
      if (!p || !fs.existsSync(p)) continue;
      const f = path.join(OUT, path.basename(p));
      fs.copyFileSync(p, f);
      if (path.dirname(path.resolve(p)).toLowerCase() === shotsDir.toLowerCase() && path.basename(p).startsWith(PREFIX)) fs.rmSync(p);
      written.push(f);
    }
    if (argv.includes("--start")) await call("end_match", {}, { soft: true });
    await call("close", { discard: true }, { soft: true });
  } else throw new Error(`--mode render or game, not ${MODE}`);
} finally {
  await stop();
}
console.log(JSON.stringify({ mode: MODE, prefix: PREFIX, out: OUT, written }, null, 1));
