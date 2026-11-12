// Dustcrown round 6's test-harness moves, the pure parts (lib/harness-moves.ts): the rope's solved aim and the hook
// found for a ledge, the girder's steering from two measured pulses, the game's own height line, the surfaces of a band
// grouped, and the key route into a Versus match. The tools refuse without a game and take the new arguments.
import assert from "node:assert/strict";
import { test } from "node:test";
import { aimAt, aimDir, bandGroups, comparePositions, gameLine, girderHold, landscapeIndex, localnetRoute, muzzleOf, solveHook, versusRoute, type FanHit } from "../src/lib/harness-moves";
import { validate } from "../src/registry";
import { tools as moveTools } from "../src/tools/moves";
import type { Session } from "../src/session";

const near = (a: number, b: number, eps = 1e-6) => Math.abs(a - b) <= eps;

test("aimAt and aimDir: the pitch and yaw from a worm's rope hand to a point, and back to a direction", () => {
  const mz = muzzleOf([0, 12, 0], 0.5);
  assert.deepEqual(mz, [0, 12.5, 0]);
  const a = aimAt(mz, [10, 22.5, 0]);
  assert.ok(near(a.pitch, Math.PI / 4) && near(a.yaw!, 90) && near(a.distance, Math.hypot(10, 10)));
  // straight up has no yaw; toward +z is yaw 0
  assert.equal(aimAt(mz, [0, 30, 0]).yaw, null);
  assert.ok(near(aimAt(mz, [0, 12.5, 5]).yaw!, 0) && near(aimAt(mz, [0, 12.5, 5]).pitch, 0));
  const d = aimDir(90, Math.PI / 4);
  assert.ok(near(d[0], Math.SQRT1_2) && near(d[1], Math.SQRT1_2) && near(d[2], 0, 1e-9));
  assert.ok(near(Math.hypot(...aimDir(37, 0.6)), 1));
});

// a stepped face seen from a worm at x 0, y 12 on ledge 1: riser 2 at x 5.5 (12 to 18), ledge 2 to x 11.5, riser 3 (18 to 26)
const ray = (mz: number[], pitch: number): number[] | null => {
  const dx = Math.cos(pitch), dy = Math.sin(pitch);
  // riser 2's face
  const t2 = (5.5 - mz[0]) / dx, y2 = mz[1] + dy * t2;
  if (y2 <= 18) return [5.5, y2, 0];
  // over the lip: riser 3's face, or nothing over its top
  const t3 = (11.5 - mz[0]) / dx, y3 = mz[1] + dy * t3;
  return y3 <= 26 ? [11.5, y3, 0] : null;
};
const fanFrom = (mz: number[]): FanHit[] => Array.from({ length: 70 }, (_, i) => ({ pitch: ((i + 15) * Math.PI) / 180, at: ray(mz, ((i + 15) * Math.PI) / 180) }));

test("solveHook: the lowest ray that meets land over the ledge; a hook on the riser's own face is no hook; the worm's own line must clear the lip", () => {
  const target = [9, 18, 0], o = { over: 0.75, reach: 4, lipBack: 0.5 };
  const mz = [0, 12.5, 0];
  const r = solveHook(mz, target, fanFrom(mz), o);
  assert.ok(r.hit, JSON.stringify(r));
  // it lands on riser 3's face, over the ledge, and is the first pitch that clears riser 2's lip
  assert.equal(r.hit!.at![0], 11.5);
  assert.ok(r.hit!.at![1] > 18.75 && r.hit!.at![1] <= 26);
  assert.ok(Math.tan(r.hit!.pitch) > 5.5 / 5.5 && Math.tan(r.hit!.pitch - Math.PI / 180) <= 5.5 / 5.5 + 0.04, `pitch ${(r.hit!.pitch * 180) / Math.PI}`);
  // the body's fan (from the feet): the same pitch from 0.4 lower still hits riser 2's face, so that ray drags the worm
  // into the face; the solver goes on to a pitch whose foot line clears the lip too, or says so
  const feet = [0, 12.1, 0];
  const withBody = solveHook(mz, target, fanFrom(mz), o, fanFrom(feet));
  assert.ok(withBody.hit && withBody.hit.pitch > r.hit!.pitch, "a steeper ray is taken");
  assert.equal(ray(feet, withBody.hit!.pitch)![0], 11.5);
  // from the riser's very foot nothing clears the lip under riser 3's top: every hook over the ledge is short of it
  const close = [4.5, 12.5, 0];
  const none = solveHook(close, target, fanFrom(close), o);
  assert.equal(none.hit, null);
  assert.match((none as { why: string }).why, /from further back a lower aim clears the lip/);
  // nothing over the ledge at all (a single riser): said
  const flat = solveHook(mz, target, Array.from({ length: 60 }, (_, i) => ({ pitch: (i * Math.PI) / 180, at: Math.tan((i * Math.PI) / 180) * 5.5 + 12.5 <= 18 ? [5.5, Math.tan((i * Math.PI) / 180) * 5.5 + 12.5, 0] : null })), o);
  assert.equal(flat.hit, null);
  assert.match((flat as { why: string }).why, /under the ledge's height|short of the ledge/);
  // a hook far past the target is refused by reach
  assert.match((solveHook(mz, [6, 18, 0], fanFrom(mz), { ...o, reach: 2 }) as { why: string }).why, /too far past the target/);
});

test("girderHold: how long to hold Forward and Right for a move, from what a pulse of each moved the girder", () => {
  // the girder camera looks along +z: Forward moves +z, Right moves -x (as measured in the game: 4 and 5 map units a second)
  const f = [0, 0.004], r = [-0.005, 0];
  const up = girderHold([0, 8], f, r)!;
  assert.ok(near(up.forwardMs, 2000) && near(up.rightMs, 0));
  const h = girderHold([10, -4], f, r)!;
  assert.ok(near(h.forwardMs, -1000) && near(h.rightMs, -2000));
  // a turned camera: the two axes are still solved together
  const c = Math.SQRT1_2 * 0.004, g = girderHold([4, 0], [c, c], [c, -c])!;
  assert.ok(near(g.forwardMs, g.rightMs) && near(g.forwardMs * c * 2, 4));
  // parallel pulses cannot steer
  assert.equal(girderHold([1, 1], [0, 0.004], [0, 0.008]), null);
});

test("gameLine: Sandtide's rule on the game's own land box: from the water when the land reaches under it", () => {
  assert.deepEqual(gameLine(-400, 1110, 20, 0.25), { line: 292.5, base: 20, baseKind: "water" });
  assert.deepEqual(gameLine(40, 1040, 20, 0.25), { line: 290, base: 40, baseKind: "landMin" });
  assert.deepEqual(gameLine(-400, 1110, null, 0.5), { line: 355, base: -400, baseKind: "landMin" });
});

test("bandGroups: the surfaces near a line by 4-unit cells, the nearest first, with the covered ones counted", () => {
  const g = bandGroups([{ x: 1, z: 1, y: 13.0, covered: false }, { x: 2, z: 1, y: 13.4, covered: true }, { x: 30, z: 30, y: 11.9, covered: false }], 12, 4);
  assert.equal(g.length, 2);
  assert.ok(near(g[0].nearest, -0.1) && g[0].columns === 1);
  assert.deepEqual([g[1].columns, g[1].covered, g[1].minY, g[1].maxY, g[1].at], [2, 1, 13.0, 13.4, [1.5, 1]]);
  assert.ok(near(g[1].nearest, 1));
  assert.deepEqual(bandGroups([], 12, 4), []);
});

test("landscapeIndex and versusRoute: a pack's level among the prebuilt landscapes, and the keys from the main menu to the match", () => {
  // the list: five random themes, then the prebuilt ones in alphabetical order (seen in the game: Desert Oasis, Dustcrown, Riverboat Harbour, ...)
  assert.equal(landscapeIndex("Dustcrown", ["Dustcrown"]), 6);
  assert.equal(landscapeIndex("Desert Oasis", []), 5);
  assert.equal(landscapeIndex("caldera", ["Dustcrown", "Caldera"]), 5);
  assert.equal(landscapeIndex("Dustcrown", ["Dustcrown", "Caldera"]), 7);
  assert.equal(landscapeIndex("Zanzibar", ["Dustcrown", "Zanzibar"]), 10);
  assert.equal(landscapeIndex("Nowhere", ["Dustcrown"]), -1);
  const keys = versusRoute([{ human: true }, { human: true }, { human: false, cpu: "hard" }], 6);
  const presses = keys.filter((k) => k.startsWith("key "));
  assert.equal(presses.filter((k) => k === "key RETURN").length, 3 + 6 + 2 + 1, "three menus, a list and a pick per team, the landscape and its pick, the grenade");
  // every command is a key or a wait, and the last press starts the match from the grenade beside the back arrow
  assert.ok(keys.every((k) => /^(key (UP|DOWN|LEFT|RIGHT|RETURN)|wait \d+)$/.test(k)));
  assert.deepEqual(presses.slice(-4), ["key DOWN", "key DOWN", "key RIGHT", "key RETURN"]);
  // the CPU team: past the two player teams the save still lists, then Easy, Normal, Hard
  const at = keys.lastIndexOf("key LEFT"), teamPart = keys.slice(0, at).filter((k) => k.startsWith("key "));
  let downs = 0;
  for (let i = teamPart.length - 2; i >= 0 && teamPart[i] === "key DOWN"; i--) downs++;
  assert.equal(downs, 2 + 2);
  // the landscape: from the list's top, six down
  const land = keys.slice(at).filter((k) => k.startsWith("key "));
  assert.equal(land.filter((k) => k === "key DOWN").length, 6 + 2);
  // a prompt in the way, and more player teams than the save has
  assert.deepEqual(versusRoute([{}, {}], 5, { dismiss: 1 }).slice(0, 2), ["key RETURN", "wait 4000"]);
  assert.throws(() => versusRoute([{}, {}, {}], 5, { humanTeams: 2 }), /no player team left/);
});

test("localnetRoute and comparePositions: the host's and the joiner's keys, and two peers' worms side by side", () => {
  const host = localnetRoute("host", { team: 0, landscape: 6 }), boot = localnetRoute("joinBoot", { team: 1 }), find = localnetRoute("joinFind");
  for (const keys of [host, boot, find, localnetRoute("hostReady"), localnetRoute("joinReady")]) assert.ok(keys.every((k) => /^(key (UP|DOWN|LEFT|RIGHT|RETURN)|wait \d+)$/.test(k)));
  // the host: Network Game, its team, Create Match, Max Players' first choice, the tick, then the landscape and its pick
  assert.equal(host.filter((k) => k === "key RETURN").length, 8);
  assert.equal(host.filter((k) => k === "key LEFT").length, 1);
  // the joiner picks the second team, and only searches in its second part
  assert.equal(boot.filter((k) => k === "key RETURN").length, 2);
  assert.equal(boot.filter((k) => k === "key DOWN").length, 1 + 1);
  assert.equal(find.filter((k) => k === "key RETURN").length, 3);
  assert.deepEqual(localnetRoute("hostReady").filter((k) => k.startsWith("key ")), ["key DOWN", "key DOWN", "key RIGHT", "key RETURN"]);
  assert.deepEqual(localnetRoute("joinReady"), ["key RETURN", "wait 1500"]);
  const a = [{ slot: 0, team: 0, pos: [-4.5, 6, 86] }, { slot: 4, team: 1, pos: [-4.5, 6, -86] }];
  assert.deepEqual(comparePositions(a, a.map((w) => ({ ...w, pos: [...w.pos] }))), { agree: true, worms: 2, differ: [] });
  const moved = comparePositions(a, [a[0], { slot: 4, team: 1, pos: [86, 6, -4.5] }]);
  assert.deepEqual([moved.agree, moved.differ.length, moved.differ[0].worm], [false, 1, 4]);
  // a peer that lacks a worm, or reads another team for it, does not agree
  assert.equal(comparePositions(a, [a[0]]).agree, false);
  assert.equal(comparePositions(a, [a[0], { ...a[1], team: 0 }]).agree, false);
  assert.equal(comparePositions([], []).agree, false);
});

test("the new moves take their arguments and refuse without a game", async () => {
  const use = moveTools.find((t) => t.name === "game_use_tool")!, line = moveTools.find((t) => t.name === "game_line_clearance")!, versus = moveTools.find((t) => t.name === "game_start_versus")!;
  for (const args of [{ weapon: "rope", target: [0, 18, 0], rope: { solve: true } }, { weapon: "rope", target: [0, 18, 0], rope: { hook: [3, 22, 0], releaseOver: false } },
    { weapon: "rope", target: [0, 18, 0], rope: { solve: { over: 1, reach: 3 }, refire: 1 } }, { weapon: "girder", target: [0, 9, 0], girder: { place: false, lift: 1 } }])
    assert.equal(validate(use.inputSchema as never, args), null, JSON.stringify(args));
  assert.match(validate(use.inputSchema as never, { weapon: "girder", target: [0, 9, 0], girder: { spin: 1 } }) ?? "", /spin/);
  const noGame = { ensure: async () => ({ methods: [] }), client: async () => ({ call: async () => { throw new Error("no game"); } }) } as unknown as Session;
  await assert.rejects(async () => use.handler(noGame, { weapon: "girder", target: [0, 9, 0] } as never), /needs the game's input\.drive/);
  await assert.rejects(async () => use.handler(noGame, { weapon: "rope", target: [0, 18, 0], rope: { solve: true } } as never), /needs the game's input\.drive/);
  // the teleport: the game has none, and the tool says so instead of "not yet"
  await assert.rejects(async () => use.handler(noGame, { weapon: "teleport", target: [0, 9, 0] } as never), /this game has no teleport weapon/);
  assert.equal(validate(line.inputSchema as never, { preset: "sandtide", band: 40, units: "world" }), null);
  await assert.rejects(async () => line.handler(noGame, { preset: "sandtide" } as never), /needs the game's level\.test\.landRay/);
  assert.equal(validate(versus.inputSchema as never, { modId: "dustcrown", teams: [{ human: true }, { human: false, cpu: "easy" }], dismiss: 1 }), null);
  assert.match(validate(versus.inputSchema as never, { modId: "dustcrown", teams: [{ human: true }] }) ?? "", /teams/);
  await assert.rejects(async () => versus.handler(noGame, { teams: [{}, {}], gameDir: "Z:/no/such/game" } as never), /give gameDir/);
  const net = moveTools.find((t) => t.name === "game_localnet_match")!;
  assert.equal(validate(net.inputSchema as never, { host: { gameDir: "C:/a" }, join: { gameDir: "C:/b", team: 1 }, modId: "dustcrown", dryRun: true }), null);
  await assert.rejects(async () => net.handler(noGame, { host: { gameDir: "Z:/no/a" }, join: { gameDir: "Z:/no/b" } } as never), /not a game folder/);
});
