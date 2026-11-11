// The four-team harness's pure parts: game_play_turns' trail geometry (where a worm stands along its team's trail, the
// rest of the trail from there) and its shot pose, and the Test teams line; bundled for Node by test.ps1.
import assert from "node:assert/strict";
import { test } from "node:test";
import { alongTrail, restOfTrail, sidePose, turnHeading } from "../src/tools/game";
import { teamsLine } from "../src/tools/place";

const trail = [[16, -16], [16, -9], [12, -6], [17, 0], [4, 0], [0, 0]];

test("alongTrail: the nearest point, how far along, the length", () => {
  const a = alongTrail(trail, [16.5, -12]);
  assert.equal(a.seg, 0);
  assert.ok(Math.abs(a.along - 4) < 1e-9 && Math.abs(a.off - 0.5) < 1e-9, JSON.stringify(a));
  assert.ok(Math.abs(a.of - (7 + 5 + Math.hypot(5, 6) + 13 + 4)) < 1e-9);
  // a switchback's legs over each other: y tells them apart
  const sw = [[0, 0, 0], [10, 0, 0], [10, 0, 2], [0, 2, 2], [0, 2, 0], [10, 4, 0]];
  assert.equal(alongTrail(sw, [5, 4, 0.2]).seg, 4);
  assert.equal(alongTrail(sw, [5, 0, 0.2]).seg, 0);
});

test("restOfTrail starts at the worm's nearest point and keeps the later points", () => {
  const r = restOfTrail(trail, [16.5, -12]);
  assert.deepEqual(r[0], [16, -12]);
  assert.deepEqual(r.slice(1), trail.slice(1));
  assert.deepEqual(restOfTrail([[1, 2]], [5, 5]), [[1, 2]]);
});

test("the shot pose looks at the worm from the left of its heading, over the walls", () => {
  const h = turnHeading([320, 40, -320], [[320, -320], [320, -180]], 120);
  assert.ok(Math.abs(h[0]) < 1e-9 && Math.abs(h[1] - 1) < 1e-9, JSON.stringify(h));
  const p = sidePose([320, 40, -320], h);
  assert.deepEqual(p.target, [320, 52, -320]);
  assert.deepEqual(p.pos, [120, 260, -360]);
  assert.deepEqual(turnHeading([0, 0, 0], [[0, 0]], 120), [0, 1]);
});

test("teamsLine names the Quick Game start the teams make", () => {
  assert.match(teamsLine(), /QuickStartHvC/);
  const four = teamsLine([0, 1, 2, 3].map(() => ({ human: true, worms: 2 })));
  assert.match(four, /QuickStartHvHvHvH/);
  assert.match(four, /team 3 human, 2 worms/);
  assert.match(teamsLine([{ human: true }, { human: false, worms: 3 }]), /QuickStartHvC.*team 1 computer, 3 worms/);
});
