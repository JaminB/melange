// Caldera's detail layer: decoration that changes nothing the simulation reads (rock detail on the volcano, then flora),
// run AFTER build.mjs on the saved project, never editing build.mjs or terrain.mjs, so the map's gameplay work and this
// layer reconcile by running one after the other. Every pass goes through erg-mcp's sim-neutral channels only
// (docs/erg-mcp.md "Detail layer": scenery copies, lights, emitters, sounds, the primary paint of solid voxels), and the
// run proves it: the sim_fingerprint after the passes must equal the one before, or nothing is saved.
//
//   tools\node\node.exe tools\erg-mcp\bench\caldera\detail.mjs [--out <dir>] [--project caldera] [--dry] [--passes a,b]
//
// Connects like build.mjs (MELANGE_GAME_DIR with the game closed, default testenv D here; MELANGE_EXE default this
// checkout's build\x86-release\Melange.exe). Steps: open the project; fingerprint (detail: true); each pass of PASSES in
// order (a pass is async (ctx) => stats; ctx has call, the keepOut helper, the spawns, say); fingerprint against the first:
// a different sim throws (the project is closed without saving); save; open again and fingerprint once more (the saved
// project must hash the same: a round trip through the server). --dry stops before the save. The answers go to
// <out>\detail.json. Round 0 has no pass: it proves the skeleton (the fingerprint is stable over a save and reopen).
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { startServer, ROOT } from "./mcpclient.mjs";
import { keepOut, CLEAR } from "./detail-keepout.mjs";
import { rockPass } from "./detail-rock.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const argv = process.argv.slice(2);
const opt = (f, d) => { const i = argv.indexOf(f); return i >= 0 ? argv[i + 1] : d; };
const OUT = path.resolve(opt("--out", path.join(HERE, "out", "detail")));
const ID = opt("--project", process.env.CALDERA_ID || "caldera");
const GAME = process.env.MELANGE_GAME_DIR || "C:/Users/Jamin/Desktop/WUMFix/testenv/D";
process.env.MELANGE_EXE ??= path.join(ROOT, "build/x86-release/Melange.exe");
const ONLY = opt("--passes", "") ? opt("--passes").split(",") : null;
fs.mkdirSync(OUT, { recursive: true });
const say = (...a) => console.log(new Date().toISOString().slice(11, 19), ...a);

// The passes, in order. Each takes ctx and answers its stats (what it placed). Round 0: none.
//   ctx.call(tool, args, opts)   erg-mcp tool call (throws on a tool error unless {soft: true})
//   ctx.keepOut(x, z, r)         the first reason a prop of radius r may not stand at (x, z), or null (detail-keepout.mjs)
//   ctx.spawns                   the level's spawn knots [[x, z], ...] (map units)
export const PASSES = [
  { name: "flags", run: flagsPass },
  { name: "rock", run: rockPass },
];

// build.mjs placed its four Camelot flags with copy_base_scenery's default WORLD units though it meant map units (tower tops at
// map (53.7, 31, 6.4) and the like), so they stand 20 times too close to the origin, inside the volcano, and show nowhere. This pass
// puts each where build.mjs meant (the same numbers read as map units); copies are visual-only, so the fingerprint cannot change.
async function flagsPass(ctx) {
  const { call } = ctx;
  const list = (await call("list_scenery", { role: "scenery", search: "flag" })).json.details.filter((d) => !d.base && /^CAMELOT(16|19)$/.test(d.resource));
  const bad = list.filter((d) => Math.max(...d.at.map(Math.abs)) < 200);          // world units under 10 map units from the origin: the 20x slip
  if (!bad.length) return { fixed: 0 };
  await call("remove_scenery", { ids: bad.map((d) => d.id) });
  const copies = bad.map((d) => ({ resource: d.resource, pos: d.at, rot: d.rot, scale: d.scale[0] }));
  await call("copy_base_scenery", { base: "Multi.NiceToSiege", allowOtherTheme: true, units: "map", copies });
  return { fixed: bad.length };
}

const { call, stop, log } = await startServer();
const report = { project: ID, passes: [], startedAt: new Date().toISOString() };
try {
  say("connect", (await call("connect", opt("--url") ? { url: opt("--url") } : { gameDir: GAME })).text.slice(0, 120));
  await call("open_project", { id: ID });
  const fp0 = (await call("sim_fingerprint", { detail: true })).json;
  report.before = { sim: fp0.sim, visual: fp0.visual, counts: fp0.counts };
  say("fingerprint before", fp0.sim.slice(0, 16), "visual", fp0.visual.slice(0, 16), JSON.stringify(fp0.counts));
  if (fp0.counts.emitter > 0 || (await call("list_scenery", { search: "_dtl" })).json.total > 0) throw new Error("the project already holds the detail layer: run detail.mjs on a freshly built project (restore it or run build.mjs first)");
  const spawns = ((await call("describe_level", { limit: 1 })).json.spawns ?? []).map((s) => {
    const p = s.map ?? s.at ?? s.pos ?? [];
    return Array.isArray(p) ? [p[0], p[2] ?? p[1]] : [p.x, p.z];
  }).filter((p) => Number.isFinite(p[0]) && Number.isFinite(p[1]));
  const ctx = { call, keepOut: (x, z, r = 0) => keepOut(x, z, r, spawns), spawns, clear: CLEAR, say };
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
  report.after = { sim: fp1.sim, visual: fp1.visual, counts: fp1.counts, against: fp1.against };
  if (!fp1.against.same) throw new Error(`the detail layer changed the sim: ${JSON.stringify(fp1.against)}`);
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
