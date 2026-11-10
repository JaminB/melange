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
import { landPass } from "./detail-land.mjs";
import { cliffPass } from "./detail-cliff.mjs";
import { crownPass } from "./detail-crown.mjs";
import { coastPass } from "./detail-coast.mjs";
import { floraPass } from "./detail-flora.mjs";
import { lintPass } from "./detail-lint.mjs";
import { perchPass } from "./detail-perch.mjs";
import { offshorePass } from "./detail-offshore.mjs";
import { supportPass } from "./detail-support.mjs";
import { slabPass } from "./detail-slab.mjs";
import { retintPass } from "./detail-retint.mjs";
import { lawnPass } from "./detail-lawn.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const argv = process.argv.slice(2);
const opt = (f, d) => { const i = argv.indexOf(f); return i >= 0 ? argv[i + 1] : d; };
const OUT = path.resolve(opt("--out", path.join(HERE, "out", "detail")));
const ID = opt("--project", process.env.CALDERA_ID || "caldera");
const BUILD_OUT = opt("--build-out", path.join(HERE, "out"));   // build.mjs's --out (castles.json: the castle cameras the perched lint reads)
const GAME = process.env.MELANGE_GAME_DIR || "C:/Users/Jamin/Desktop/WUMFix/testenv/D";
process.env.MELANGE_EXE ??= path.join(ROOT, "build/x86-release/Melange.exe");
// round 7: 1100 (round 6's 505 was the engine's 550-instance mesh pool, which Melange's [Fixes] MeshInstances now raises to
// 2000: the rest is headroom for worms' hats, thrown mines and effects)
// round 9: 560 (the round-8 judge: round 7's dressed level cost 8.4 ms of GPU frame time against 4.9 undressed: "cut the
// draw count before adding anything"; fewer, bigger props: round 8's level held 573 details). Round 9 measured (testenv D,
// Sunstone on, the oblique, B idle): undressed 4.95 / 4.62 ms, rock only 6.08, flora only 6.09, both (544 details) 6.57:
// most of the cost comes with the first dressed props, not with each one
export const DETAIL_BUDGET = +(process.env.DTL_BUDGET ?? 580);
const ONLY = opt("--passes", "") ? opt("--passes").split(",") : null;
fs.mkdirSync(OUT, { recursive: true });
const say = (...a) => console.log(new Date().toISOString().slice(11, 19), ...a);

// The passes, in order. Each takes ctx and answers its stats (what it placed). Round 0: none.
//   ctx.call(tool, args, opts)   erg-mcp tool call (throws on a tool error unless {soft: true})
//   ctx.keepOut(x, z, r)         the first reason a prop of radius r may not stand at (x, z), or null (detail-keepout.mjs)
//   ctx.spawns                   the level's spawn knots [[x, z], ...] (map units)
export const PASSES = [
  { name: "flags", run: flagsPass },
  { name: "land", run: landPass },
  // round 9: the lawn tiles' textures pulled to a muted olive green (detail-lawn.mjs); blunt shore stacks with boulder skirts
  // and no specks (detail-coast.mjs, detail-offshore.mjs); round rim-rock cliff piles (detail-cliff.mjs); contour slabs on
  // the bare flank faces (detail-slab.mjs, erg-mcp plan_contour); one rock tint per elevation band (detail-retint.mjs);
  // flora as fewer, bigger clumps with a height ramp from lush to ash (detail-flora.mjs)
  { name: "lawn", run: lawnPass },
  { name: "rock", run: rockPass },
  // round 6: horizontal strata bands replace round 5's boulder "strata" (detail-cliff.mjs, removed); columnar crowns on the
  // crests and cliff tops; the coast (stacks, half-sunk blocks, stepped columns); the first flora; the perched lint
  // round 7: continuous strata ROWS on 4-6 hero faces replaced round 6's isolated lens slabs; round 5's cliff piles came back
  // round 8: the strata rows are gone (the judge: "better none than necklaces": a round rock mesh in a row reads as beads);
  // the cliff piles are laid along their faces with talus below; the offshore singles are cleared; every rock clump hanging
  // off a steep face with no ground or talus under it is removed (detail-support.mjs); then the flora
  { name: "cliff", run: cliffPass },
  { name: "crown", run: crownPass },
  { name: "coast", run: coastPass },
  { name: "offshore", run: offshorePass },
  { name: "slab", run: slabPass },
  { name: "retint", run: retintPass },
  { name: "support", run: supportPass },
  { name: "flora", run: floraPass },
  { name: "lint", run: lintPass },
  { name: "perch", run: perchPass },
];

// build.mjs places four Camelot flags (CAMELOT16, CAMELOT19 of Multi.NiceToSiege) for the castles' towers, in world units though
// its numbers are map units, so they stand inside the volcano and show nowhere; round 1 moved them to the tower tops, where
// the game drew them as purple plant pods (another theme's resources resolve to something else in this PREHISTORIC level).
// Round 3 removes them (scenery copies: visual only, the fingerprint cannot change).
async function flagsPass(ctx) {
  const { call } = ctx;
  const list = (await call("list_scenery", { role: "scenery", search: "flag" })).json.details.filter((d) => !d.base && /^CAMELOT(16|19)$/.test(d.resource));
  if (list.length) await call("remove_scenery", { ids: list.map((d) => d.id) });
  return { removed: list.length };
}

const { call, stop, log } = await startServer();
const report = { project: ID, passes: [], startedAt: new Date().toISOString() };
try {
  say("connect", (await call("connect", opt("--url") ? { url: opt("--url") } : { gameDir: GAME })).text.slice(0, 120));
  await call("open_project", { id: ID });
  const fp0 = (await call("sim_fingerprint", { detail: true })).json;
  report.before = { sim: fp0.sim, visual: fp0.visual, counts: fp0.counts };
  const script0 = (await call("get_script", {}, { soft: true })).text;
  say("fingerprint before", fp0.sim.slice(0, 16), "visual", fp0.visual.slice(0, 16), JSON.stringify(fp0.counts));
  if (fp0.counts.emitter > 0 || (await Promise.all(["_dtl", "_ldg", "_flo", "_cpl", "_slb"].map(async (q) => (await call("list_scenery", { search: q })).json.total))).some((n) => n > 0) || (await call("list_scenery", { search: "_landtint" })).json.total > 0) throw new Error("the project already holds the detail layer: run detail.mjs on a freshly built project (restore it or run build.mjs first)");
  const spawns = ((await call("describe_level", { limit: 1 })).json.spawns ?? []).map((s) => {
    const p = s.map ?? s.at ?? s.pos ?? [];
    return Array.isArray(p) ? [p[0], p[2] ?? p[1]] : [p.x, p.z];
  }).filter((p) => Number.isFinite(p[0]) && Number.isFinite(p[1]));
  const ctx = { call, buildOut: BUILD_OUT, keepOut: (x, z, r = 0) => keepOut(x, z, r, spawns), spawns, clear: CLEAR, say };
  for (const p of PASSES) {
    if (ONLY && !ONLY.includes(p.name)) continue;
    if (p.name === "flora") {
      // the layer's DETAIL BUDGET: round 6's crash at load with 546 details (WormsMayhem+0x1d0fad) was the engine's mesh-instance
      // pool running out (550, "Out of mesh instances": round 7 pinned it in a full dump); Melange raises the pool (see
      // docs/erg-mcp.md round 7), and the layer keeps the level's details (the fingerprint's count, sim and visual) at
      // DETAIL_BUDGET or under, the flora taking what is left
      const c = (await call("sim_fingerprint", {})).json.counts;
      ctx.floraMax = Math.max(0, DETAIL_BUDGET - (c.simDetails + c.scenery + c.light + c.emitter + c.sound));
      say("budget", DETAIL_BUDGET, "flora may add", ctx.floraMax);
    }
    const t0 = Date.now();
    const stats = await p.run(ctx);
    const fp = (await call("sim_fingerprint", { against: fp0 })).json;
    report.passes.push({ name: p.name, ms: Date.now() - t0, stats, simSame: fp.against.same, visualParts: fp.against.visualParts });
    say("pass", p.name, JSON.stringify(stats).slice(0, 300), "sim", fp.against.same ? "unchanged" : "CHANGED " + JSON.stringify(fp.against.simParts));
    if (!fp.against.same && fp.against.simParts?.includes("script")) {
      // keep both texts for the diagnosis (round 6: one run's script part changed under a pass that never touches it)
      fs.writeFileSync(path.join(OUT, "script-before.lua"), script0);
      fs.writeFileSync(path.join(OUT, "script-after.lua"), (await call("get_script", {}, { soft: true })).text);
    }
    if (!fp.against.same) throw new Error(`pass ${p.name} changed the sim (${JSON.stringify(fp.against)}): nothing is saved`);
  }
  const fp1 = (await call("sim_fingerprint", { against: fp0, detail: true })).json;
  report.after = { sim: fp1.sim, visual: fp1.visual, counts: fp1.counts, against: fp1.against };
  if (!fp1.against.same) throw new Error(`the detail layer changed the sim: ${JSON.stringify(fp1.against)}`);
  // round 7: the engine budget left (erg-mcp detail_budget: the mesh-instance pool from the game folder's Melange.ini)
  report.budget = (await call("detail_budget", { gameDir: GAME }, { soft: true })).json;
  // round 8: the ship gate (erg-mcp export raises the pack's melange.range by this when the level needs the bigger mesh pool)
  report.ship = report.budget?.ship;
  say("ship", JSON.stringify(report.ship));
  say("budget", JSON.stringify(report.budget && { details: report.budget.details, pool: report.budget.pool, headroom: report.budget.headroom, vanillaHeadroom: report.budget.vanillaHeadroom }));
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
