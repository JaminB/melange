// landBounds, heightLine and check_height_line on the synthetic 12-frame level (its root renamed Scene), bundled for Node by test.ps1.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, type Loaded, type Scene } from "../src/core";
import { heightLine, landBounds, sideOf } from "../src/lib/height-line";
import { grids } from "../src/lib/voxel";
import { tools as heightTools } from "../src/tools/heightline";
import { describe } from "../src/tools/project";
import type { Session } from "../src/session";

function loaded(): Loaded {
  const s: Scene = JSON.parse(readFileSync(new URL("../../../../tests/fixtures/erg/synthetic-12.json", import.meta.url), "utf8"));
  s.frames.find((f) => f.id === 1)!.name = "Scene";
  const blobs = new Map<number, ArrayBuffer>();
  for (const [ref, buf] of syntheticBlobs(s) as Map<number, Uint8Array>) blobs.set(ref, buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength) as ArrayBuffer);
  return { scene: s, blobs };
}
function session() {
  const store = new EditorStore("t", loaded(), loaded());
  const s = { requireProject: () => ({ store, id: "t", info: { id: "t" } }), get store() { return store; }, project: { store } } as unknown as Session;
  return { store, s };
}
const tool = heightTools.find((t) => t.name === "check_height_line")!;
const call = async (s: Session, a: Record<string, unknown>): Promise<any> => tool.handler(s, a);

test("heightLine: water base only when the land reaches under it", () => {
  const b = { minY: -9, maxY: 55, minFrame: 1, maxFrame: 2, minFrameName: "a", maxFrameName: "b" };
  const w = heightLine(b, 1, { fraction: 0.25 });
  assert.equal(w.baseKind, "water");
  assert.equal(w.line, 1 + 0.25 * 54);
  const dry = heightLine(b, -20, { fraction: 0.25 });          // water under all the land
  assert.equal(dry.baseKind, "minY");
  assert.equal(dry.line, -9 + 0.25 * 64);
  assert.equal(heightLine(b, null, { fraction: 0.5 }).base, -9);
  assert.equal(heightLine(b, 1, { y: 7 }).baseKind, "fixed");
  assert.equal(sideOf(7, 7.5, 2), "near");
  assert.equal(sideOf(3, 7.5, 2), "below");
  assert.equal(sideOf(12, 7.5, 2), "above");
});

test("landBounds names the frames that set each end and describe_level reports it", () => {
  const { store, s } = session();
  const gs = grids(store), lb = landBounds(gs)!;
  assert.equal(lb.minY, Math.min(...gs.map((g) => g.bounds.min[1])));
  assert.equal(lb.maxY, Math.max(...gs.map((g) => g.bounds.max[1])));
  assert.equal(gs.find((g) => g.id === lb.maxFrame)!.bounds.max[1], lb.maxY);
  const d: any = describe(s);
  assert.equal(d.landBounds.maxY.map, lb.maxY);
  assert.equal(d.landBounds.maxY.world, lb.maxY * 20);
  assert.equal(d.landBounds.maxFrame.id, lb.maxFrame);
  assert.equal(landBounds([]), null);
});

test("check_height_line: the sandtide preset, a fixed y, a bad call, and no edit", async () => {
  const { store, s } = session();
  const lb = landBounds(grids(store))!;
  const before = JSON.stringify(store.scene);
  const r = await call(s, { preset: "sandtide", water: lb.minY + 1 });
  assert.equal(r.fraction, 0.25);
  assert.equal(r.base.kind, "water");
  assert.ok(Math.abs(r.line.map - ((lb.minY + 1) + 0.25 * (lb.maxY - lb.minY - 1))) < 0.01);
  assert.equal(r.line.world, Math.round(r.line.map * 20 * 100) / 100);
  const n = r.surfaces.below.columns + r.surfaces.near.columns + r.surfaces.above.columns;
  assert.equal(n, r.surfaces.sampled);
  assert.equal(r.spawns.below + r.spawns.near + r.spawns.above, r.spawns.list.length);
  const fixed = await call(s, { y: lb.maxY + 5 });
  assert.equal(fixed.base.kind, "fixed");
  assert.ok(fixed.warnings.some((w: string) => /over the land box's top/.test(w)));
  assert.ok(fixed.warnings.some((w: string) => /every worm would be cursed|below the line/.test(w)));
  const lowWater = await call(s, { fraction: 0.25, water: lb.minY - 3 });
  assert.equal(lowWater.base.kind, "minY");
  assert.ok(lowWater.warnings.some((w: string) => /at or under the land box's bottom/.test(w)));
  await assert.rejects(() => call(s, {}), /give one of/);
  await assert.rejects(() => call(s, { preset: "sandtide", y: 3 }), /give one of/);
  assert.equal(JSON.stringify(store.scene), before);
});

test("check_height_line flags surfaces and knots within the band", async () => {
  const { store, s } = session();
  const first = await call(s, { y: 0, band: 1e6 });             // everything is near
  assert.equal(first.surfaces.below.columns + first.surfaces.above.columns, 0);
  assert.ok(first.surfaces.near.columns > 0);
  assert.equal(first.ok, false);
  assert.ok(first.flagged.surfaceGroups > 0);
  const hi = first.surfaces.highest.map;
  const none = await call(s, { y: hi + 100 });
  assert.equal(none.surfaces.near.columns, 0);
  assert.equal(none.flagged.surfaceGroups, 0);
  void store;
});
