// Detail layer round 9: lib/contour.ts (plan_contour), lib/prop-pixels.ts (prop_pixels), lib/perf-guard.ts (game_overlap,
// game_perf_guarded).
import assert from "node:assert/strict";
import { test } from "node:test";
import { contourRuns, isoLines, resample, type HeightGrid } from "../src/lib/contour";
import { pixelReport, seenBy } from "../src/lib/prop-pixels";
import { parseCim, splitGames, testenvOf } from "../src/lib/perf-guard";

// a cone: y = 40 - r (45 degree flanks), r over a 81 x 81 grid of cell 1 centred on 0
function cone(f: (r: number) => number = (r) => 40 - r): HeightGrid {
  const nx = 81, nz = 81, y: (number | null)[] = [];
  for (let iz = 0; iz < nz; iz++) for (let ix = 0; ix < nx; ix++) y.push(f(Math.hypot(ix - 40, iz - 40)));
  return { x0: -40, z0: -40, cell: 1, nx, nz, y };
}

test("isoLines on a cone: one closed ring at the level's radius", () => {
  const lines = isoLines(cone(), 20);
  assert.equal(lines.length, 1);
  const l = lines[0];
  assert.ok(l.length > 60);
  for (const [x, z] of l) assert.ok(Math.abs(Math.hypot(x, z) - 20) < 0.15, `${x},${z}`);
  const first = l[0], last = l[l.length - 1];
  assert.ok(Math.hypot(first[0] - last[0], first[1] - last[1]) < 1e-6, "closed");
});

test("resample spaces points by arc length", () => {
  const pts = resample([[0, 0], [10, 0], [10, 10]], 2.5);
  assert.deepEqual(pts.map((p) => p.p.map((v) => Math.round(v * 100) / 100)), [[0, 0], [2.5, 0], [5, 0], [7.5, 0], [10, 0], [10, 2.5], [10, 5], [10, 7.5], [10, 10]]);
  assert.deepEqual(pts[6].dir, [0, 1]);
});

test("contourRuns: tangent along the ring, normal leaning out, slope 45", () => {
  const runs = contourRuns(cone(), [10, 25], { spacing: 3 });
  assert.equal(runs.length, 2);
  for (const r of runs) for (const s of r.sites) {
    const [x, , z] = s.pos, rr = Math.hypot(x, z);
    // tangent perpendicular to the radial direction
    assert.ok(Math.abs((s.tangent[0] * x + s.tangent[1] * z) / rr) < 0.08, JSON.stringify(s));
    // normal leans outward (downhill) and up equally (45 degrees)
    assert.ok((s.normal[0] * x + s.normal[2] * z) / rr > 0.6 && s.normal[1] > 0.6);
    assert.ok(Math.abs(s.slope - 45) < 3);
    assert.equal(s.pos[1], r.level);
  }
  // about circumference / spacing sites
  const ring10 = runs.find((r) => r.level === 10)!;   // radius 30
  assert.ok(Math.abs(ring10.sites.length - (2 * Math.PI * 30) / 3) < 4, String(ring10.sites.length));
});

test("contourRuns: a run breaks where the slope leaves the band or accept refuses", () => {
  const all = contourRuns(cone(), [20], { spacing: 2 });
  assert.equal(all.length, 1);
  const none = contourRuns(cone(), [20], { spacing: 2, minSlope: 50 });
  assert.equal(none.length, 0);
  // refuse the half with x > 0: the ring splits into runs on the x < 0 side only
  const half = contourRuns(cone(), [20], { spacing: 2, accept: (x) => x < 0 });
  assert.ok(half.length >= 1 && half.length <= 2);
  for (const r of half) for (const s of r.sites) assert.ok(s.pos[0] < 0);
  // minRun drops short runs
  assert.equal(contourRuns(cone(), [20], { spacing: 2, accept: (x) => x < 0, minRun: 1000 }).length, 0);
});

test("seenBy: a 1-unit box 100 away at fov 55, 720 tall: about 13 px; behind the camera: not in view", () => {
  const cam = { pos: [0, 0, 0] as [number, number, number], target: [0, 0, 100] as [number, number, number], fov: 55, width: 1280, height: 720 };
  const s = seenBy(cam, [-0.5, -0.5, 99.5], [0.5, 0.5, 100.5]);
  const expect = (Math.sqrt(3) * 360) / (100 * Math.tan((27.5 * Math.PI) / 180));
  assert.ok(s.inView); assert.ok(Math.abs(s.px - expect) < 0.01, `${s.px} ${expect}`);
  assert.ok(Math.abs(s.screen![0] - 640) < 1e-6 && Math.abs(s.screen![1] - 360) < 1e-6);
  assert.equal(seenBy(cam, [-0.5, -0.5, -100.5], [0.5, 0.5, -99.5]).inView, false);
  // far off to the side: out of the frame
  assert.equal(seenBy(cam, [300, 0, 99.5], [301, 1, 100.5]).inView, false);
});

test("pixelReport: subPixel lists props small in every camera that sees them, and props no camera sees", () => {
  const cams = [{ name: "near", pos: [0, 0, 0] as [number, number, number], target: [0, 0, 1] as [number, number, number] },
    { name: "far", pos: [0, 0, -900] as [number, number, number], target: [0, 0, 1] as [number, number, number] }];
  const box = (id: number, z: number, w: number) => ({ id, min: [-w / 2, -w / 2, z - w / 2] as [number, number, number], max: [w / 2, w / 2, z + w / 2] as [number, number, number] });
  const r = pixelReport([box(1, 10, 1), box(2, 2000, 0.3), box(3, -50, 1)], cams, 3);
  // 1: big in "near"; 2: tiny in both; 3: behind "near", 850 from "far" (about 1.4 px)
  assert.deepEqual(r.subPixel.sort(), [2, 3]);
  assert.equal(r.cams[0].inView, 2);
});

test("perf guard: other folders' games are others; testenv names are read", () => {
  assert.equal(testenvOf("C:\\Users\\J\\Desktop\\WUMFix\\testenv\\B\\WormsMayhem.exe"), "B");
  assert.equal(testenvOf(null), null);
  const procs = parseCim('[{"ProcessId":40344,"ExecutablePath":"C:\\\\WUMFix\\\\testenv\\\\B\\\\WormsMayhem.exe"},{"ProcessId":7,"ExecutablePath":"C:\\\\WUMFix\\\\testenv\\\\D\\\\WormsMayhem.exe"},{"ProcessId":9,"ExecutablePath":null}]');
  assert.equal(procs.length, 3);
  const sp = splitGames(procs, "C:/WUMFix/testenv/D/");
  assert.deepEqual(sp.mine.map((p) => p.pid), [7]);
  assert.deepEqual(sp.others.map((p) => [p.pid, p.testenv]), [[40344, "B"], [9, null]]);
  // a sibling folder whose name starts the same is not this one
  assert.equal(splitGames([{ pid: 1, path: "C:\\WUMFix\\testenv\\D2\\WormsMayhem.exe" }], "C:\\WUMFix\\testenv\\D").others.length, 1);
  assert.deepEqual(parseCim(""), []);
  assert.equal(parseCim('{"ProcessId":5,"ExecutablePath":"x"}')[0].pid, 5);
});
