// The mine factory's launchers (several launch points of the one factory the engine allows): set_mine_factory and
// place_object take them in `units` ([x, y, z], or [x, z] on the ground's top), the spec holds whole world units, [] and
// launch false clear them, the answer's launchPlan gives each launcher's flight and landing radius, and the factory check warns of a
// launcher under the water. On the synthetic 12-frame level (its root renamed Scene), away from its own frames.
// Bundled for Node by test.ps1.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, type Loaded, type Scene } from "../src/core";
import { checkFactory, factoryLines } from "../src/lib/factory-check";
import { apply, plan } from "../src/lib/heightfield-plan";
import { validate } from "../src/registry";
import { tools as gameTools } from "../src/tools/game";
import { tools as placeTools } from "../src/tools/place";
import type { Session } from "../src/session";

function loaded(): Loaded {
  const s: Scene = JSON.parse(readFileSync(new URL("../../../../tests/fixtures/erg/synthetic-12.json", import.meta.url), "utf8"));
  s.frames.find((f) => f.id === 1)!.name = "Scene";
  const blobs = new Map<number, ArrayBuffer>();
  for (const [ref, buf] of syntheticBlobs(s) as Map<number, Uint8Array>) blobs.set(ref, buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength) as ArrayBuffer);
  return { scene: s, blobs };
}
function session(extra: Record<string, unknown> = {}) {
  const store = new EditorStore("t", loaded(), loaded());
  const project = { store, id: "t", info: {}, erg: { script: async () => "" } };
  const s = { requireProject: () => project, get store() { return store; }, project,
    levels: async () => { throw new Error("no server"); }, client: async () => { throw new Error("no server"); }, ...extra } as unknown as Session;
  return { store, s };
}
const call = async (s: Session, name: string, args: Record<string, unknown>): Promise<any> => {
  const t = [...placeTools, ...gameTools].find((x) => x.name === name)!;
  const bad = validate(t.inputSchema, args);
  if (bad) throw new Error(bad);
  return await t.handler(s, args);
};
const spec = (store: EditorStore) => (store.scene.objects!.find((o) => o.type === "minefactory") as { factory?: Record<string, unknown> }).factory;

test("launchers: map points to whole world units, [x, z] on the ground's top, a launch plan, [] and launch false clear them", async () => {
  const { store, s } = session();
  // a 4-high block round x 500 (the factory) and a 10-high rim at x 510-520
  apply(store, plan(store, { min: [480, -10], max: [520, 10], floor: 0, expr: "4", material: 1 }));
  apply(store, plan(store, { min: [510, -10], max: [520, 10], floor: 0, expr: "10", material: 1 }));
  await call(s, "place_object", { type: "minefactory", units: "map", pos: [500, 4, 0], snap: "surface", keepKnot: true, launchRadius: 6,
    launchers: [[515.03, 0], [490, 6, 5]] });
  const f = spec(store)!;
  assert.deepEqual(f.launchers, [[10301, 200, 0], [9800, 120, 100]], "[x, z] takes the column's top (10 map), [x, y, z] is kept; world units");
  const r = await call(s, "set_mine_factory", { units: "map", launchers: [[515, 0], [485, 0], [500, 8]], launchHeight: 12 });
  assert.equal(r.factory.launchers.length, 3);
  assert.equal(r.launchPlan.length, 3);
  assert.equal(r.launchPlan[0].toFactory, 15);
  assert.equal(r.launchPlan[0].apex, 10 + 1 + 12, "the eruption tops out launchHeight over the launcher's spawn, 1 over its ground");
  assert.ok(r.launchPlan[0].flightSeconds > 2 && r.launchPlan[0].flightSeconds < 6, String(r.launchPlan[0].flightSeconds));
  assert.equal(r.launchPlan[0].landsWithin, 6, "the mines land within launchRadius of their launcher");
  await call(s, "set_mine_factory", { launchers: [] });
  assert.equal(spec(store)!.launchers, undefined, "[] clears the launchers");
  await call(s, "set_mine_factory", { units: "map", launchers: [[515, 0]] });
  await call(s, "set_mine_factory", { launch: false });
  assert.deepEqual(spec(store), { keepKnot: true }, "launch false clears height, radius and launchers");
  await assert.rejects(() => call(s, "set_mine_factory", { units: "map", launchers: [[700, 700]] }), /no ground in the column/);
  await assert.rejects(() => call(s, "set_mine_factory", { launchers: Array.from({ length: 9 }, () => [0, 0, 0]) }), /at most 8|maxItems|must NOT have more/);
});

test("throw: set in the server's stored form, targets in the call's units, merged by later calls, cleared by false, planned", async () => {
  const { store, s } = session();
  apply(store, plan(store, { min: [480, -10], max: [520, 10], floor: 0, expr: "4", material: 1 }));
  await call(s, "place_object", { type: "minefactory", units: "map", pos: [500, 4, 0], snap: "surface", keepKnot: true, throw: { maxRange: 40, apex: 15, pattern: "random" } });
  assert.deepEqual(spec(store), { keepKnot: true, throw: { maxRange: 40 } }, "defaults given are left out, as the server stores them");
  const r = await call(s, "set_mine_factory", { units: "map", throw: { minRange: 10, pattern: "targets", targets: [[515.04, 2], [490, -5]], scatter: 6, targetShare: 40 } });
  assert.deepEqual(spec(store)!.throw, { minRange: 10, maxRange: 40, pattern: "targets", targets: [[10301, 40], [9800, -100]], scatter: 6, targetShare: 40 },
    "a later call merges into the throw there; targets become whole world units");
  assert.equal(r.launchPlan.length, 1);
  assert.equal(r.launchPlan[0].launcher, "factory", "without launchers the factory is the launch point");
  assert.deepEqual(r.launchPlan[0].landsBetween, [10, 40]);
  assert.ok(r.launchPlan[0].flightSeconds > 1.5 && r.launchPlan[0].flightSeconds < 5, String(r.launchPlan[0].flightSeconds));
  await assert.rejects(() => call(s, "set_mine_factory", { throw: { minRange: 50 } }), /minRange must not be over/);
  await assert.rejects(() => call(s, "set_mine_factory", { throw: true }), /give its settings/);
  await assert.rejects(() => call(s, "set_mine_factory", { throw: { maxRange: 600 } }), /1-500|maximum|at most/);
  await call(s, "set_mine_factory", { units: "map", launchers: [[515, 0]] });
  const r2 = await call(s, "set_mine_factory", { throw: { apex: 20 } });
  assert.equal(r2.launchPlan[0].launcher, 0, "with launchers the plan is per launcher");
  assert.equal(r2.launchPlan[0].apexOverEnds, 20);
  await call(s, "set_mine_factory", { throw: false });
  assert.equal(spec(store)!.throw, undefined, "false clears the throw");
  assert.ok(spec(store)!.launchers, "and leaves the launchers");
  await call(s, "set_mine_factory", { throw: { maxRange: 30 } });
  await call(s, "set_mine_factory", { launch: false });
  assert.deepEqual(spec(store), { keepKnot: true }, "launch false clears the throw too");
  await assert.rejects(() => call(s, "place_object", { type: "minefactory", units: "map", pos: [490, 4, 0], snap: "surface", throw: { maxRange: 20, pattern: "targets" } }), /needs throw.targets|already has/);
});

test("launchers: one under the water is warned of", async () => {
  const { store, s } = session();
  apply(store, plan(store, { min: [480, -10], max: [520, 10], floor: 0, expr: "4", material: 1 }));
  await call(s, "place_object", { type: "minefactory", units: "map", pos: [500, 4, 0], snap: "surface", keepKnot: true, launchers: [[505, -2, 0], [495, 4, 0]] });
  store.scene.water.level = 0;
  const lines = factoryLines(checkFactory(store, (v) => `${v} map`));
  assert.ok(lines.some((l) => /launcher 0 .* under the water/.test(l)), lines.join("\n"));
  assert.ok(!lines.some((l) => /launcher 1 .* under the water/.test(l)), lines.join("\n"));
});

test("game_mine_drop: [x, z] over the ground top, the drops sent in world units, each point held, low or gone", async () => {
  const calls: { m: string; p: any }[] = [];
  let listed = 0;
  // before the drop: one old mine; after: one on the first point's ground, one far under the second, none at the third
  const client = { call: async (m: string, p: any) => {
    calls.push({ m, p });
    if (m === "entities.list") return listed++ === 0 ? [{ handle: 1, kind: "projectile", type: "ParabolicPayloadLogicEntity", label: "Landmine", pos: { x: 10000, y: 80, z: 0 } }]
      : [{ handle: 1, kind: "projectile", type: "ParabolicPayloadLogicEntity", label: "Landmine", pos: { x: 10000, y: 80, z: 0 } },
         { handle: 2, kind: "projectile", type: "ParabolicPayloadLogicEntity", label: "Landmine", pos: { x: 9810, y: 80, z: 0 } },
         { handle: 3, kind: "projectile", type: "ParabolicPayloadLogicEntity", label: "Landmine", pos: { x: 10000, y: 15, z: 100 } }];
    return { queued: true };
  } };
  const { store, s } = session({ ensure: async () => ({ methods: ["level.test.dropMines", "entities.list"] }), client: async () => client });
  apply(store, plan(store, { min: [480, -10], max: [520, 10], floor: 0, expr: "4", material: 1 }));
  const r = await call(s, "game_mine_drop", { points: [[490.5, 0], [500, 5], [510, -5]], seconds: 3 });
  const drop = calls.find((c) => c.m === "level.test.dropMines")!;
  assert.deepEqual(drop.p.points[0], [9810, (4 + 8) * 20, 0], "8 map units over the ground top, world units");
  assert.deepEqual(r.points.map((p: { verdict: string }) => p.verdict), ["held", "low", "gone"]);
  assert.deepEqual(r.counts, { held: 1, low: 1, gone: 1 });
});
