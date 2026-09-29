import assert from "node:assert/strict";
import { test } from "node:test";
import { deepDiff } from "../../src/panels/desync/model";
import { entriesOf, formatBytes, formatDuration, sortNewestFirst, withEntry, type ReplayEntry } from "../../src/panels/replays/model";
import {
  changedMask, clampView, fpuFaultTicks, fracToTick, panView, tickAtOrBefore, tickToFrac, zoomView,
} from "../../src/panels/timeline/model";
import type { TickRecord } from "../../src/sdk/wsr";

// ---------------------------------------------------------------- replays/model
function entry(over: Partial<ReplayEntry> = {}): ReplayEntry {
  return {
    name: "a.wsr", bytes: 1000, ticks: 100, inputs: 5, remoteInputs: 0, startUnix: 1700000000, exeBuild: "1077",
    melange: "0.0.0", land: "cropcircle", contentHash: "", online: false, complete: true, pinned: false, flagged: false,
    ...over,
  };
}

test("entriesOf keeps only well-formed entries and defaults missing fields", () => {
  const list = entriesOf([
    { name: "a.wsr", bytes: 10, ticks: 5, complete: true },
    { name: "b.wsr" }, // every optional field missing
    { bytes: 10 }, // no name: dropped
    "not an object",
    null,
  ]);
  assert.equal(list.length, 2);
  assert.equal(list[0].name, "a.wsr");
  assert.equal(list[0].bytes, 10);
  assert.equal(list[0].ticks, 5);
  assert.equal(list[0].complete, true);
  assert.equal(list[0].inputs, 0, "missing numeric fields default to 0");
  assert.equal(list[0].exeBuild, "", "missing string fields default to empty");
  assert.equal(list[1].name, "b.wsr");
  assert.equal(list[1].complete, false, "missing booleans default to false");
});

test("sortNewestFirst orders by startUnix descending, then by name", () => {
  const list = [entry({ name: "old", startUnix: 100 }), entry({ name: "new", startUnix: 300 }), entry({ name: "mid", startUnix: 200 })];
  assert.deepEqual(sortNewestFirst(list).map((e) => e.name), ["new", "mid", "old"]);
});

test("withEntry replaces an existing entry by name, or inserts and re-sorts", () => {
  const list = [entry({ name: "a", startUnix: 100 }), entry({ name: "b", startUnix: 200 })];
  const replaced = withEntry(list, entry({ name: "a", startUnix: 100, pinned: true }));
  assert.equal(replaced.length, 2);
  assert.equal(replaced.find((e) => e.name === "a")!.pinned, true);
  const inserted = withEntry(list, entry({ name: "c", startUnix: 300 }));
  assert.deepEqual(inserted.map((e) => e.name), ["c", "b", "a"]);
});

test("formatBytes scales through KB/MB/GB", () => {
  assert.equal(formatBytes(500), "500 B");
  assert.equal(formatBytes(2048), "2.0 KB");
  assert.equal(formatBytes(5 * 1024 * 1024), "5.0 MB");
});

test("formatDuration renders mm:ss, and h:mm:ss past an hour (20 ms per tick)", () => {
  assert.equal(formatDuration(0), "0:00");
  assert.equal(formatDuration(50 * 65), "1:05"); // 65 s at 50 ticks/s
  assert.equal(formatDuration(50 * 3661), "1:01:01");
});

// ---------------------------------------------------------------- timeline/model
function tick(n: number, c: readonly string[] = ["0", "0", "0", "0", "0", "0"], fpucw = 0x027f): TickRecord {
  return { tick: n, engine: "0", mods: "0", c: [...c], rngLogic: 0, rng2: 0, fpucw, inputs: 0 };
}

test("clampView keeps a minimum span and stays inside [0, totalTicks)", () => {
  assert.deepEqual(clampView({ from: -5, to: 5 }, 100, 5), { from: 0, to: 10 });
  assert.deepEqual(clampView({ from: 90, to: 99 }, 100, 5), { from: 90, to: 99 });
  assert.deepEqual(clampView({ from: 95, to: 150 }, 100, 5), { from: 44, to: 99 });
  assert.deepEqual(clampView({ from: -1000, to: 2000 }, 100, 5), { from: 0, to: 99 });
  // a span under minSpan (here 4, right at the file's end) is widened to fit, not left as-is
  assert.deepEqual(clampView({ from: 95, to: 99 }, 100, 5), { from: 94, to: 99 });
});

test("zoomView keeps the point under `aroundFrac` fixed and clamps to the file", () => {
  const view = { from: 0, to: 99 };
  const zoomedIn = zoomView(view, 100, 0.5, 0.5); // zoom in around the middle
  assert.ok(zoomedIn.to - zoomedIn.from < 99);
  const centerBefore = view.from + (view.to - view.from) * 0.5;
  const centerAfter = zoomedIn.from + (zoomedIn.to - zoomedIn.from) * 0.5;
  assert.ok(Math.abs(centerBefore - centerAfter) <= 1);
  const zoomedOut = zoomView({ from: 40, to: 60 }, 100, 20, 0.5); // factor large enough to exceed the whole file
  assert.deepEqual(zoomedOut, { from: 0, to: 99 });
});

test("panView shifts the window and clamps at the edges", () => {
  assert.deepEqual(panView({ from: 10, to: 20 }, 100, 5), { from: 15, to: 25 });
  assert.deepEqual(panView({ from: 90, to: 99 }, 100, 10), { from: 90, to: 99 }, "already at the right edge");
  assert.deepEqual(panView({ from: 0, to: 10 }, 100, -20), { from: 0, to: 10 }, "already at the left edge");
});

test("tickToFrac / fracToTick round-trip within a view", () => {
  const view = { from: 100, to: 200 };
  assert.equal(tickToFrac(150, view), 0.5);
  assert.equal(fracToTick(0.5, view), 150);
  assert.equal(fracToTick(tickToFrac(137, view), view), 137);
});

test("changedMask flags exactly the components that differ from the previous tick", () => {
  const prev = tick(0, ["1", "2", "3", "4", "5", "6"]);
  const cur = tick(1, ["1", "9", "3", "4", "9", "6"]); // worms(index? no: position 1 and 4 differ -> bits 1 and 4
  assert.equal(changedMask(cur, prev), (1 << 1) | (1 << 4));
  assert.equal(changedMask(cur, undefined), 0, "no previous tick: nothing to compare");
});

test("tickAtOrBefore binary-searches the last record at or before a target tick", () => {
  const ticks = [tick(10), tick(20), tick(30)];
  assert.equal(tickAtOrBefore(ticks, 25)!.tick, 20);
  assert.equal(tickAtOrBefore(ticks, 30)!.tick, 30);
  assert.equal(tickAtOrBefore(ticks, 5), undefined);
});

test("fpuFaultTicks reports only ticks whose control word left 0x027f", () => {
  const ticks = [tick(0, undefined, 0x027f), tick(1, undefined, 0x037f), tick(2, undefined, 0x027f)];
  assert.deepEqual(fpuFaultTicks(ticks), [1]);
});

// ---------------------------------------------------------------- desync/model
test("deepDiff finds only the leaves that changed, with array and object paths", () => {
  const before = { worms: [{ energy: 100 }, { energy: 50 }], team: { score: 0 } };
  const after = { worms: [{ energy: 1 }, { energy: 50 }], team: { score: 0 } };
  assert.deepEqual(deepDiff(before, after), [{ path: "worms[0].energy", before: 100, after: 1 }]);
});

test("deepDiff handles missing keys on either side and top-level scalars", () => {
  assert.deepEqual(deepDiff({ a: 1 }, { a: 1, b: 2 }), [{ path: "b", before: undefined, after: 2 }]);
  assert.deepEqual(deepDiff(5, 6), [{ path: "(root)", before: 5, after: 6 }]);
  assert.deepEqual(deepDiff(5, 5), []);
});
