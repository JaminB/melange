// Dustcrown's detail layer (round 1): sim-neutral dressing, run AFTER build.mjs on the saved project, never editing
// build.mjs (the map and its gameplay stay as built; DETAIL.md has what the layer adds and how it was judged). The passes
// (detail-passes.mjs) write only scenery copies: Dustcrown's own meshes in the ONE bank (meshes/bank.mjs) and the ARABIAN
// base's props. The run proves it: the sim_fingerprint after every pass must equal the one before the first, or nothing is
// saved; the saved project is opened again and must hash the same.
//
//   tools\node\node.exe tools\erg-mcp\bench\dustcrown\detail.mjs [--out <dir>] [--project dustcrown] [--dry] [--passes a,b]
//
// Connects like build.mjs (MELANGE_GAME_DIR, default testenv C, with the game closed; MELANGE_EXE, default this checkout's
// build\x86-release\Melange.exe). It first removes its own earlier pieces (name tags _dtl1 and _pal1), so a second run does not double.
// build.mjs's boulder-field meshes (_dtl) are left as they are. The answers go to <out>\detail.json.
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { startServer, ROOT } from "../caldera/mcpclient.mjs";
import { BANK } from "./meshes/bank.mjs";
import { renderViews } from "./detail-views.mjs";
import { TAG, PALACE_TAG, mesaPass, hoodooPass, litterPass, floraPass, palacePass, lintPass } from "./detail-passes.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const argv = process.argv.slice(2);
const opt = (f, d) => { const i = argv.indexOf(f); return i >= 0 ? argv[i + 1] : d; };
const OUT = path.resolve(opt("--out", path.join(HERE, "out", "detail")));
const ID = opt("--project", process.env.DUSTCROWN_ID || "dustcrown");
const GAME = process.env.MELANGE_GAME_DIR || "C:/Users/Jamin/Desktop/WUMFix/testenv/C";
process.env.MELANGE_EXE ??= path.join(ROOT, "build/x86-release/Melange.exe");
const ONLY = opt("--passes", "") ? opt("--passes").split(",") : null;
fs.mkdirSync(OUT, { recursive: true });
const TAGS = [TAG, PALACE_TAG];
const say = (...a) => console.log(new Date().toISOString().slice(11, 19), ...a);

export const PASSES = [
  { name: "mesas", run: mesaPass },
  { name: "hoodoos", run: hoodooPass },
  { name: "litter", run: litterPass },
  { name: "flora", run: floraPass },
  { name: "palace", run: palacePass },
  { name: "lint", run: lintPass },
];

const { call, stop, log } = await startServer();
const report = { project: ID, passes: [], startedAt: new Date().toISOString() };
try {
  say("connect", (await call("connect", { gameDir: GAME })).text.slice(0, 120));
  await call("open_project", { id: ID });
  // the layer's own earlier pieces, so a second run replaces rather than doubles
  let old = 0;
  for (const t of TAGS) { const n = (await call("list_scenery", { search: `_${t}`, limit: 1 })).json.total; if (n) { await call("remove_scenery", { search: `_${t}` }); old += n; } }
  if (old) say("removed an earlier run's", old, "pieces");
  const fp0 = (await call("sim_fingerprint", { detail: true })).json;
  report.before = { sim: fp0.sim, visual: fp0.visual, counts: fp0.counts, parts: fp0.parts };
  say("fingerprint before", fp0.sim.slice(0, 16), JSON.stringify(fp0.counts));
  // the bank's meshes and their boxes (dryRun registers them for render_view {scenery: true})
  const rb = await call("build_rock_bank", { ...BANK, dryRun: true });
  const boxes = Object.fromEntries(rb.json.meshes.map((m) => [m.name.slice(m.name.indexOf(".") + 1), m.box]));
  const ctx = { call, boxes, say };
  for (const p of PASSES) {
    if (ONLY && !ONLY.includes(p.name)) continue;
    const t0 = Date.now();
    const stats = await p.run(ctx);
    const fp = (await call("sim_fingerprint", { against: fp0 })).json;
    report.passes.push({ name: p.name, ms: Date.now() - t0, stats, simSame: fp.against.same, visualParts: fp.against.visualParts });
    say("pass", p.name, JSON.stringify(stats).slice(0, 300), "sim", fp.against.same ? "unchanged" : "CHANGED " + JSON.stringify(fp.against.simParts));
    if (!fp.against.same) throw new Error(`pass ${p.name} changed the sim (${JSON.stringify(fp.against)}): nothing is saved`);
  }
  const fp1 = (await call("sim_fingerprint", { against: fp0, detail: true })).json;
  report.after = { sim: fp1.sim, visual: fp1.visual, counts: fp1.counts, parts: fp1.parts, against: fp1.against };
  if (!fp1.against.same) throw new Error(`the detail layer changed the sim: ${JSON.stringify(fp1.against)}`);
  if (opt("--render")) {
    await renderViews(ctx, path.resolve(opt("--render")), report.passes.find((p) => p.name === "hoodoos")?.stats?.picks ?? []);
    say("renders", opt("--render"));
  }
  report.budget = (await call("detail_budget", { gameDir: GAME }, { soft: true })).json;
  say("budget", JSON.stringify(report.budget && { details: report.budget.details, pool: report.budget.pool, headroom: report.budget.headroom, ship: report.budget.ship }));
  if (argv.includes("--dry")) {
    say("--dry: not saved");
    await call("close", { discard: true });
  } else {
    const sv = await call("save");
    report.save = { projectHash: sv.json?.projectHash, warnings: sv.json?.warnings ?? [] };
    say("save", sv.json?.projectHash?.slice(0, 16), JSON.stringify(sv.json?.warnings ?? []));
    await call("close", {});
    await call("open_project", { id: ID });
    const fp2 = (await call("sim_fingerprint", { against: fp0 })).json;
    report.reopened = { sim: fp2.sim, visual: fp2.visual, same: fp2.against.same, against: fp2.against };
    say("reopened", fp2.against.same ? "sim unchanged" : "SIM CHANGED " + JSON.stringify(fp2.against), "visual", fp2.visual === fp1.visual ? "as saved" : "differs from the session's");
    await call("close", { discard: true });
    if (!fp2.against.same) throw new Error("the saved project's sim differs from the one before the detail layer");
  }
  report.ok = true;
} catch (e) {
  report.ok = false;
  report.error = String(e.message ?? e);
  await call("close", { discard: true }, { soft: true });
  throw e;
} finally {
  report.finishedAt = new Date().toISOString();
  fs.writeFileSync(path.join(OUT, "detail.json"), JSON.stringify(report, null, 1));
  fs.writeFileSync(path.join(OUT, "detail-calls.json"), JSON.stringify(log, null, 1));
  await stop();
}
