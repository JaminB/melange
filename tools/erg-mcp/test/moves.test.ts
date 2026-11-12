// The test-harness moves' pure parts (lib/harness-moves.ts) and the tools that need no game: a worm's facing and turn
// pulses, the close-up's eye candidates, the riser verdict, a jetpack flight's inputs, the peers' spawn deals, the diff of
// two shots, the quiet wait after explosions; game_spawn_deals on log files; the tools' schemas and refusals.
import assert from "node:assert/strict";
import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test } from "node:test";
import { compareDeals, eyeCandidates, facingOf, imageDiff, jetStep, parseSpawnDeals, quietState, riserVerdict, turnPulse, wrapDeg, yawToward } from "../src/lib/harness-moves";
import { validate } from "../src/registry";
import { tools as gameTools } from "../src/tools/game";
import { tools } from "../src/tools/moves";
import type { Session } from "../src/session";

const tool = (n: string) => tools.find((t) => t.name === n)!;

test("the moves group registers its tools with strict schemas", () => {
  for (const n of ["game_face", "game_use_tool", "game_riser_test", "game_start_level", "game_shot_compare", "game_spawn_deals"]) {
    assert.ok(tool(n), n);
    assert.equal(tool(n).inputSchema.additionalProperties, false, n);
  }
  assert.equal(validate(tool("game_use_tool").inputSchema as never, { weapon: "jetpack", target: [1, 2, 3] }), null);
  assert.ok(validate(tool("game_use_tool").inputSchema as never, { weapon: "sheep", target: [1, 2, 3] }));
  assert.ok(validate(tool("game_face").inputSchema as never, { yaw: 30, speed: 2 }));
  // game_shots and game_perf took their new settings without losing the old ones
  const shots = gameTools.find((t) => t.name === "game_shots")!.inputSchema as any, perf = gameTools.find((t) => t.name === "game_perf")!.inputSchema as any;
  assert.deepEqual(shots.properties.cams.items.properties.preset.enum, ["default", "overview", "castle", "worm"]);
  assert.ok(shots.properties.afterBlasts && shots.properties.settleFrames && perf.properties.pose && perf.properties.against);
});

test("yaw: 0 is +z and 90 is +x; wrapDeg; turnPulse sizes a pulse from the measured rate and stops inside the tolerance", () => {
  assert.equal(yawToward([0, 0], [0, 5]), 0);
  assert.equal(yawToward([0, 0], [5, 0]), 90);
  assert.equal(yawToward([1, 9, 1], [1, 2, -4]), 180);
  assert.equal(yawToward([3, 3], [3, 3]), null);
  assert.deepEqual(facingOf(90).map((v) => Math.round(v * 1000) / 1000), [1, 0]);
  assert.equal(wrapDeg(190), -170);
  assert.equal(wrapDeg(-190), 170);
  assert.equal(wrapDeg(360), 0);
  assert.equal(turnPulse(10, 15, 0, 12).done, true);
  const first = turnPulse(0, 90, 0, 12);
  assert.deepEqual([first.done, first.sign, first.ms], [false, 1, 80]);
  // 0.3 degrees per ms: three quarters of 90 degrees is 225 ms
  assert.equal(turnPulse(0, 90, 0.3, 12).ms, 225);
  assert.equal(turnPulse(0, -170, 0.3, 12).sign, -1);
  assert.equal(turnPulse(170, -170, 0.3, 12).sign, 1, "the short way round through 180");
  assert.equal(turnPulse(0, 20, 5, 12).ms, 40, "never under 40 ms");
  assert.equal(turnPulse(0, 179, 0.01, 12).ms, 400, "never over 400 ms");
});

test("eyeCandidates: the asked pose first, then turned, nearer, farther and higher; all at their distance from the target", () => {
  const c = eyeCandidates([100, 20, 50], 180, 25, 140);
  assert.equal(c.length, 4 * 5 * 8);
  // azimuth 180: the eye stands south (+z) of the target
  assert.ok(Math.abs(c[0].eye[0] - 100) < 1e-6 && c[0].eye[2] > 50 && c[0].eye[1] > 20);
  assert.deepEqual([c[0].azimuth, c[0].elevation, c[0].distance], [180, 25, 140]);
  assert.deepEqual([c[1].azimuth, c[2].azimuth], [225, 135]);
  for (const e of c) assert.ok(Math.abs(Math.hypot(e.eye[0] - 100, e.eye[1] - 20, e.eye[2] - 50) - e.distance) < 1e-6);
  assert.ok(c.some((e) => e.elevation === 70) && c.every((e) => e.elevation <= 85));
});

test("riserVerdict: how high each try got and whether one ended on top", () => {
  const tries = [
    { kind: "walk" as const, startY: 6, maxY: 6.02, endY: 6, endAt: [0, 6, 0], moved: 0.1 },
    { kind: "jump" as const, startY: 6, maxY: 7.9, endY: 6, endAt: [0, 6, 0], moved: 0.4 },
    { kind: "backflip" as const, startY: 6, maxY: 9.4, endY: 6, endAt: [0, 6, 0], moved: 1.2 },
  ];
  const v = riserVerdict(6, 11, tries);
  assert.equal(v.riserHeight, 5);
  assert.equal(v.climbed, false);
  assert.equal(v.highestReached, 3.4);
  assert.equal(v.shortBy, 1.6);
  assert.deepEqual(v.tries.map((t) => t.maxRise), [0.02, 1.9, 3.4]);
  // a low step: the jump ends on it
  const low = riserVerdict(6, 7.5, [{ ...tries[1], endY: 7.5 }]);
  assert.equal(low.climbed, true);
  assert.equal(low.shortBy, 0);
});

test("jetStep: straight up first, then thrust and steer until the speed carries it over, then coast and drop", () => {
  const o = { clear: 60, arrive: 30, g: 300, band: 240, maxFall: 280 };
  // from the foot: straight up, no steering
  const up = jetStep([0, 100, 0], [0, 0, 0], [400, 300, 0], o);
  assert.deepEqual([up.thrust, up.steer, up.phase], [true, null, "climb"]);
  // over the target's height with no speed toward it: thrust and steer
  const push = jetStep([0, 340, 0], [0, 50, 0], [400, 300, 0], o);
  assert.deepEqual([push.thrust, push.steer, push.phase], [true, [1, 0], "push"]);
  // fast enough that the fall carries it there: no thrust, steering kept
  const coast = jetStep([100, 420, 0], [250, 100, 0], [400, 300, 0], o);
  assert.deepEqual([coast.thrust, coast.steer, coast.phase], [false, [1, 0], "coast"]);
  assert.ok(coast.coastReach >= 300 - 15);
  // too slow for the distance: push on; over the ceiling (clear + band): steer without thrust
  assert.equal(jetStep([100, 420, 0], [60, 0, 0], [400, 300, 0], o).phase, "push");
  assert.equal(jetStep([100, 700, 0], [10, 0, 0], [400, 300, 0], o).thrust, false);
  // once it has climbed it does not go back to the climb when it sinks under the target's height
  assert.equal(jetStep([100, 310, 0], [20, -50, 0], [400, 300, 0], { ...o, climbed: true }).phase, "push");
  // over the target: drop, braking only a fast fall near the ground
  const over = jetStep([390, 380, 5], [0, -100, 0], [400, 300, 0], o);
  assert.deepEqual([over.over, over.thrust, over.steer, over.phase], [true, false, null, "drop"]);
  assert.equal(jetStep([390, 340, 5], [0, -400, 0], [400, 300, 0], o).thrust, true);
});

test("parseSpawnDeals and compareDeals: the peers of a match agree on seed and areas, or the match is named", () => {
  const a = "x\n12:00:01 [levels] spawn areas: seed 1a2b3c4d, 3 teams -> areas [2,0,4] (by team rank)\n12:09:00 [levels] spawn areas: seed 00ff00ff, 3 teams -> areas [1, 3, 4] (by team rank)\n";
  const b = "12:09:01 [levels] spawn areas: seed 00ff00ff, 3 teams -> areas [1,3,4] (by team rank)\n";
  const da = parseSpawnDeals(a), db = parseSpawnDeals(b);
  assert.deepEqual(da.map((d) => [d.seed, d.teams, d.areas, d.line]), [["1a2b3c4d", 3, [2, 0, 4], 2], ["00ff00ff", 3, [1, 3, 4], 3]]);
  const ok = compareDeals([{ name: "A", deals: da }, { name: "B", deals: db }]);
  assert.deepEqual([ok.compared, ok.agree, ok.matches[0].peers.map((p) => p.seed)], [1, true, ["00ff00ff", "00ff00ff"]]);
  // a peer with another seed (or the same seed and other areas) does not agree
  const bad = compareDeals([{ name: "A", deals: da }, { name: "B", deals: parseSpawnDeals(b.replace("00ff00ff", "00ff00fe")) }]);
  assert.deepEqual([bad.agree, bad.matches[0].sameSeed], [false, false]);
  const bad2 = compareDeals([{ name: "A", deals: da }, { name: "B", deals: parseSpawnDeals(b.replace("[1,3,4]", "[3,1,4]")) }]);
  assert.deepEqual([bad2.agree, bad2.matches[0].sameSeed, bad2.matches[0].sameAreas], [false, true, false]);
  // only as many matches as every peer has; none: not agreed, and why
  assert.equal(compareDeals([{ name: "A", deals: da }, { name: "B", deals: db }], 5).compared, 1);
  const none = compareDeals([{ name: "A", deals: da }, { name: "B", deals: [] }]);
  assert.deepEqual([none.agree, none.compared], [false, 0]);
  assert.match(none.why!, /B/);
});

test("game_spawn_deals reads the peers' logs", async () => {
  const dir = mkdtempSync(join(tmpdir(), "erg-deals-"));
  try {
    for (const [peer, seed] of [["A", "0badcafe"], ["B", "0badcafe"], ["C", "0badcaff"]]) {
      mkdirSync(join(dir, peer, "Melange"), { recursive: true });
      writeFileSync(join(dir, peer, "Melange", "Melange.log"), `[levels] spawn areas: seed ${seed}, 2 teams -> areas [0,1] (by team rank)\n`);
    }
    const h = tool("game_spawn_deals").handler as (s: Session, a: unknown) => any;
    assert.equal(h({} as Session, { gameDirs: [join(dir, "A"), join(dir, "B")] }).agree, true);
    assert.equal(h({} as Session, { gameDirs: [join(dir, "A"), join(dir, "C")] }).agree, false);
    const one = h({} as Session, { logs: [join(dir, "A", "Melange", "Melange.log")] });
    assert.deepEqual([one.count, one.distinctSeeds], [1, 1]);
    assert.throws(() => h({} as Session, {}), /gameDirs/);
    assert.throws(() => h({} as Session, { logs: [join(dir, "none.log")] }), /no such log/);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test("imageDiff: the mean difference, the changed share and its box; the same picture has none", () => {
  const w = 8, h = 4, a = new Uint8Array(w * h * 3).fill(100), b = Uint8Array.from(a);
  assert.deepEqual([imageDiff(a, b, w, h).changed, imageDiff(a, b, w, h).box], [0, null]);
  // a 2 x 2 patch at (5..6, 1..2) turned red
  for (const [x, y] of [[5, 1], [6, 1], [5, 2], [6, 2]]) { const i = (y * w + x) * 3; b[i] = 255; b[i + 1] = 0; b[i + 2] = 0; }
  const d = imageDiff(a, b, w, h);
  assert.equal(d.changed, 4 / 32);
  assert.deepEqual(d.box, { x0: 5, y0: 1, x1: 6, y1: 2 });
  assert.ok(d.mean[0] > d.mean[2] * 1.4);
  assert.deepEqual([...d.rgb.subarray((1 * w + 5) * 3, (1 * w + 5) * 3 + 3)], [255, 0, 255]);
  assert.deepEqual([...d.rgb.subarray(0, 3)], [25, 25, 25]);
  // a small shift under the threshold is not a change
  assert.equal(imageDiff(a, a.map((v) => v + 10), w, h).changed, 0);
});

test("quietState: quiet once no explosion was seen for the span; a new one starts the wait again", () => {
  let q = quietState(3, null, 1000, 5000);
  assert.equal(q.quiet, true, "nothing is known of an earlier blast: the caller starts its own wait");
  q = quietState(3, { seen: 3, at: 1000 }, 3000, 5000);
  assert.deepEqual([q.quiet, q.waitMs], [false, 3000]);
  q = quietState(4, q.last, 4000, 5000);
  assert.deepEqual([q.quiet, q.waitMs, q.last], [false, 5000, { seen: 4, at: 4000 }]);
  assert.equal(quietState(4, q.last, 9000, 5000).quiet, true);
});

test("game_use_tool refuses the tools it cannot drive, saying why", async () => {
  const h = tool("game_use_tool").handler as (s: Session, a: unknown) => Promise<unknown>;
  for (const [w, why] of [["teleport", /cursor/], ["girder", /Girder\.Place/], ["rope", /swing/]] as const)
    await assert.rejects(h({} as Session, { weapon: w, target: [0, 0, 0] }), why);
});
