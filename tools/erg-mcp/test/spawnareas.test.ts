// place_spawn_knots {mode: "areas"} on the synthetic 12-frame level: the areas' knots WORM0.. in area order, the
// spawns block (pattern, opposite pairs by name), the deal preview (2 teams: opposite sand; 3: + the palace; 4: three
// sand + the palace), the TypeScript deal against Melange's (src/erg/luagen.cpp DealAreas, the same vectors as
// tests/erg_spawn_deal_selftest.cpp), the refusals, one undo step, and the older-server message. Bundled by test.ps1.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, toPatch, validatePatch, type Loaded, type Scene } from "../src/core";
import { dealAreas } from "../src/lib/spawn-deal";
import { validate } from "../src/registry";
import { olderServer, type Session } from "../src/session";
import { tools as placeTools } from "../src/tools/place";

function loaded(): Loaded {
  const s: Scene = JSON.parse(readFileSync(new URL("../../../../tests/fixtures/erg/synthetic-12.json", import.meta.url), "utf8"));
  s.frames.find((f) => f.id === 1)!.name = "Scene";
  const blobs = new Map<number, ArrayBuffer>();
  for (const [ref, buf] of syntheticBlobs(s) as Map<number, Uint8Array>) blobs.set(ref, buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength) as ArrayBuffer);
  return { scene: s, blobs };
}
function session() {
  const store = new EditorStore("t", loaded(), loaded());
  const project = { store, id: "t", info: {}, erg: { script: async () => "" } };
  const s = { requireProject: () => project, get store() { return store; }, project,
    levels: async () => { throw new Error("no server"); }, client: async () => { throw new Error("no server"); } } as unknown as Session;
  return { store, s };
}
const call = async (s: Session, args: Record<string, unknown>): Promise<any> => {
  const t = placeTools.find((x) => x.name === "place_spawn_knots")!;
  const bad = validate(t.inputSchema, args);
  if (bad) throw new Error(bad);
  return t.handler(s, args);
};

const pad = (x: number, z: number) => [0, 1, 2, 3].map((k) => ({ pos: [x + k * 3, 4, z] }));
const DESERT = {
  mode: "areas", units: "map",
  areas: [
    { name: "SAND W", group: "sand", knots: pad(480, -20) }, { name: "SAND E", group: "sand", knots: pad(560, -20) },
    { name: "SAND N", group: "sand", knots: pad(480, 20) }, { name: "SAND S", group: "sand", knots: pad(560, 20) },
    { name: "PALACE", group: "palace", knots: pad(520, 0) },
  ],
  pattern: ["sand", "sand:opposite", "palace", "sand"],
  opposite: [["SAND W", "SAND E"], ["SAND N", 3]],
};

test("place_spawn_knots mode areas: the knots, the spawns block, the deal preview and one undo step", async () => {
  const { store, s } = session();
  const before = structuredClone(store.scene.spawns);
  const r = await call(s, DESERT);
  assert.equal(r.mode, "areas");
  assert.equal(r.changed, true);
  assert.deepEqual(store.scene.spawns, {
    mode: "areas", areas: DESERT.areas.map((a) => ({ name: a.name, group: a.group, knots: 4 })),
    pattern: DESERT.pattern, opposite: [[0, 1], [2, 3]],
  });
  assert.deepEqual(r.areas[4].knots, ["WORM16", "WORM17", "WORM18", "WORM19"]);
  assert.deepEqual(r.opposite, [["SAND W", "SAND E"], ["SAND N", "SAND S"]]);
  for (let k = 0; k < 20; k++) assert.ok(store.scene.details.some((d) => d.name === `WORM${k}` && d.role === "spawn"), `WORM${k}`);
  assert.deepEqual(r.missingKnots, []);
  const w17 = store.scene.details.find((d) => d.name === "WORM17")!;
  assert.deepEqual(store.frames.detailWorld(w17).map((v) => Math.round(v * 1000) / 1000), [523, 4, 0]);
  // the preview: 2 teams always face each other on the sand, 3 add the palace, 4 are three sand + the palace
  const by = (t: number) => r.deal.find((d: { teams: number }) => d.teams === t);
  for (const set of by(2).sets) assert.match(set.areas, /^(SAND E \+ SAND W|SAND N \+ SAND S)$/);
  for (const set of by(3).sets) assert.match(set.areas, /^(PALACE \+ SAND E \+ SAND W|PALACE \+ SAND N \+ SAND S)$/);
  for (const set of by(4).sets) assert.equal(set.areas.split(" + ").filter((n: string) => n.startsWith("SAND")).length, 3);
  assert.equal(by(2).sets.length, 2);
  assert.ok(Math.abs(by(2).sets[0].share - 0.5) < 0.1, JSON.stringify(by(2).sets));
  assert.deepEqual(by(4).fixed, ["SAND W", "SAND E", "PALACE", "SAND N"]);
  // the patch carries the block and the validator takes it
  const p = toPatch(store.base, store.scene);
  assert.deepEqual(p.spawns, store.scene.spawns);
  assert.ok(validatePatch(p).ok, validatePatch(p).errors.join("; "));
  // pattern alone keeps the areas and their knots
  const r2 = await call(s, { mode: "areas", pattern: ["palace"] });
  assert.deepEqual(store.scene.spawns.pattern, ["palace"]);
  assert.equal(store.scene.spawns.areas?.length, 5);
  assert.deepEqual(store.scene.spawns.opposite, [[0, 1], [2, 3]]);
  for (const set of r2.deal.find((d: { teams: number }) => d.teams === 2).sets) assert.match(set.areas, /PALACE/);
  // one undo step each
  store.undo();
  store.undo();
  assert.deepEqual(store.scene.spawns, before);
  assert.ok(!store.scene.details.some((d) => d.name === "WORM19"));
});

test("place_spawn_knots mode areas: refusals", async () => {
  const { s } = session();
  await assert.rejects(call(s, { mode: "areas" }), /mode areas needs areas/);
  await assert.rejects(call(s, { mode: "areas", spawns: [{ worm: 0, pos: [0, 0, 0] }] }), /not spawns or balance/);
  await assert.rejects(call(s, { ...DESERT, pattern: ["dunes"] }), /no area is in group 'dunes'/);
  await assert.rejects(call(s, { ...DESERT, opposite: [["SAND W", "MOON"]] }), /"MOON" is not an area/);
  await assert.rejects(call(s, { ...DESERT, opposite: [["SAND W", "SAND E"], ["SAND E", "SAND N"]] }), /at most one pair/);
  await assert.rejects(call(s, { ...DESERT, areas: [...DESERT.areas, ...[0, 1].map((i) => ({ name: `X${i}`, group: "x", knots: [...pad(500, 40 + i), ...pad(520, 40 + i)] }))] }), /36 knots, more than 32/);
  await assert.rejects(call(s, { mode: "teams", pattern: ["sand"] }), /for mode areas/);
});

test("the TypeScript deal is Melange's: the same draws for the same seed", () => {
  const spec = { areas: DESERT.areas.map((a) => ({ name: a.name, group: a.group, knots: 4 })), pattern: DESERT.pattern, opposite: [[0, 1], [2, 3]] as [number, number][] };
  // pinned in tests/erg_spawn_deal_selftest.cpp too
  assert.deepEqual(dealAreas(spec, 4, 12345), KNOWN_12345);
  assert.deepEqual(dealAreas(spec, 5, 0xdeadbeef), KNOWN_DEADBEEF);
  assert.deepEqual(dealAreas(spec, 5, null), [0, 1, 4, 2, 3]);
  assert.deepEqual(dealAreas(spec, 6, null), [0, 1, 4, 2, 3, -1]);
});

test("an older server's refusal of spawn areas says so", () => {
  const e = olderServer(new Error("spawns: unknown key 'areas'"), "0.14.1");
  assert.match(e.message, /spawn areas \(spawns.mode "areas"/);
  assert.match(olderServer(new Error("spawns.mode: must be \"random\", \"knots\" or \"teams\"")).message, /spawn areas/);
});

const KNOWN_12345 = [2, 3, 1, 4];
const KNOWN_DEADBEEF = [1, 4, 0, 2, 3];
