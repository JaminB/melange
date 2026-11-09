// Caldera's in-game test, through the erg-mcp tools only, against the running game (testenv B with a private build of this
// checkout and Sunstone on: the harness rules are the tester's, not this script's). Four human teams of two, every worm
// steered. Steps (STEPS=base,probe,play picks some):
//   base   the vanilla baseline: Multi.DinerMight, the same teams, bazooka fire, its overview (no project open: the worms'
//          box), its frame cost (game_perf, the GPU's time per frame: fps is CPU-bound on testenv B)
//   probe  a Test of its own before the walk: a mine dropped along every trail's centre and shoulders (game_mine_drop
//          {trails}); the mines stay, so the match is ended after it
//   play   the Test: spawns (game_probe), a wide labelled shot of each team castle with its worms (game_shots preset
//          castle), the announcement on screen (game_state {announceShot}), one wall of each team castle blasted with shots
//          before and after (test_explosion {game, from} at the build's blast points), the frame cost against the baseline,
//          then every worm walked up its trail (game_play_turns: a planned approach to the trail, replanning when stuck,
//          a shot of each announcement), the asserts against the baseline
//
//   tools\node\node.exe tools\erg-mcp\bench\caldera\play.mjs [--out <dir>] [--prefix <shot prefix>] [--rounds n] [--walk s]
//
// Reads <out>\waypoints.json and <out>\blasts.json (build.mjs writes them; default out\ next to this file). The shots go to
// Documents\Melange\shots\<prefix>_*.png; the answers to <out>\play\*.json and the log to <out>\play\play.log.
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { startServer } from "./mcpclient.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const argv = process.argv.slice(2);
const opt = (f, d) => { const i = argv.indexOf(f); return i >= 0 ? argv[i + 1] : d; };
const OUT = path.resolve(opt("--out", path.join(HERE, "out"))), DIR = path.join(OUT, "play");
const P = opt("--prefix", `calplay_${Date.now() % 100000}`), ROUNDS = +opt("--rounds", 2), WALK = +opt("--walk", 30);
const STEPS = (process.env.STEPS || "base,probe,play").split(",");
fs.mkdirSync(DIR, { recursive: true });
const L = (...a) => { const s = `${new Date().toISOString()} ${a.join(" ")}`; console.log(s); fs.appendFileSync(path.join(DIR, "play.log"), s + "\n"); };
const W = JSON.parse(fs.readFileSync(path.join(OUT, "waypoints.json"), "utf8"));
const B = JSON.parse(fs.readFileSync(path.join(OUT, "blasts.json"), "utf8"));
const trails = W.map((t) => t.waypoints), teams = [0, 1, 2, 3].map(() => ({ human: true, worms: 2 }));
const { call, stop } = await startServer();
const run = async (tag, name, args) => {
  const r = await call(name, args, { soft: true });
  fs.writeFileSync(path.join(DIR, `${tag}.json`), r.text || "");
  L(tag, r.err ? `ERR ${r.text.slice(0, 500)}` : (r.text || "").replace(/\s+/g, " ").slice(0, 400));
  return r;
};

try {
  await run("connect", "connect", {});
  if (STEPS.includes("base")) {
    await run("base_test", "test", { map: "Multi.DinerMight", noLevel: true, teams, turns: 3, fire: "bazooka", timeoutSec: 200 });
    await run("base_shots", "game_shots", { prefix: `${P}_base`, images: false, cams: [{ name: "ov", preset: "overview" }, { name: "pose", pos: [0, 60, 90], target: [0, 5, 0], fov: 60 }] });
    await run("base_perf", "game_perf", { seconds: 15, label: "baseline" });
    await run("base_end", "end_match", {});
  }
  await run("open", "open_project", { id: "caldera" });
  if (STEPS.includes("probe")) {
    await run("probe_test", "test", { teams, timeoutSec: 240 });
    const r = await run("probe_mines", "game_mine_drop", { trails, spacing: 3, shoulder: 2.6, seconds: 8 });
    for (const t of r.json?.trails ?? []) L("probe trail", W[t.trail].trail, JSON.stringify(t.counts), JSON.stringify((t.notHeld ?? []).slice(0, 12)));
    await run("probe_end", "end_match", {});
  }
  if (STEPS.includes("play")) {
    await run("test", "test", { teams, timeoutSec: 240 });
    const pr = await run("spawns", "game_probe", {});
    L("spawns", JSON.stringify((pr.json?.spawns ?? []).map((w) => [w.worm, w.team, w.knotName, w.fromKnot])));
    await run("announce", "game_state", { announceShot: true, prefix: `${P}_t1` });
    await run("castles", "game_shots", { prefix: `${P}_castle`, images: false, waitPlay: true, waitSec: 40,
      cams: B.map((b) => ({ name: `team${b.team}_${b.castle}`, preset: "castle", at: [b.centre[0], b.court, b.centre[1]], radius: 15, team: b.team })) });
    for (const b of B)
      await run(`blast_${b.castle}`, "test_explosion", { at: b.at, radius: 3, units: "map", game: true, prefix: `${P}_blast_${b.castle}`, settleMs: 3000 });
    await run("perf", "game_perf", { seconds: 15, label: "caldera", against: STEPS.includes("base") ? "baseline" : undefined });
    const play = await run("turns", "game_play_turns", { trails, rounds: ROUNDS, walkSeconds: WALK, end: "skip", prefix: `${P}_play`, timeoutSec: 3000 });
    for (const w of play.json?.worms ?? []) L("worm", w.worm, "team", w.team, "turns", w.turns, "along", JSON.stringify(w.along), "of", w.of, "climb", w.climb, "to", JSON.stringify(w.to));
    for (const t of play.json?.turns ?? []) if (t.approach || t.replans || t.stuck) L("turn", t.turn, t.worm, JSON.stringify({ approach: t.approach, replans: t.replans, stuck: t.stuck, recoveries: t.recoveries }));
    L("announcements", JSON.stringify(play.json?.announcements));
    await run("state_end", "game_state", { announceShot: true, prefix: `${P}_end` });
    await run("castles_after", "game_shots", { prefix: `${P}_after`, images: false, cams: B.map((b) => ({ name: `team${b.team}_${b.castle}`, preset: "castle", at: [b.centre[0], b.court, b.centre[1]], radius: 15 })) });
    await run("asserts", "test_asserts", {});
    await run("end", "end_match", {});
  }
} finally {
  await stop();
}
