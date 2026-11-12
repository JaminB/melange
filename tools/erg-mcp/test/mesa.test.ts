// build_mesa on the synthetic 12-frame level: the plateau at its top, strata steps with walkable treads or a sheer wall, jitter,
// the talus skirt, material bands and cap, circle / ellipse / polygon outlines, the covering circles, costOnly, the limits and
// one undo step.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, isSolid, material, type Loaded, type Scene } from "../src/core";
import { tools } from "../src/tools/mesa";
import { grids, limitProblems } from "../src/lib/voxel";
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
  const s = { requireProject: () => ({ store, id: "t", info: {} }), get store() { return store; } } as unknown as Session;
  return { store, s };
}

const tool = tools.find((t) => t.name === "build_mesa")!;
const call = async (s: Session, args: Record<string, unknown>): Promise<any> => tool.handler(s, args);

/** The solid voxels of the added blocks, by lattice column "x,z": the highest solid y + 1 and the materials by y. */
function columns(store: EditorStore) {
  const top = new Map<string, number>(), mat = new Map<string, Map<number, number>>();
  for (const g of grids(store)) {
    if (!g.frame.new) continue;
    const sc = g.toMap[0];
    assert.equal(sc, 1, "these tests use voxelSize 1");
    const o = g.frame.pos.map((p, a) => p - g.size[a] / 2);
    for (let z = 0; z < g.size[2]; z++) for (let x = 0; x < g.size[0]; x++) for (let y = 0; y < g.size[1]; y++) {
      const w = g.words[(z * g.size[0] + x) * g.size[1] + y];
      if (!isSolid(w)) continue;
      const key = `${o[0] + x},${o[2] + z}`, yy = o[1] + y;
      top.set(key, Math.max(top.get(key) ?? -Infinity, yy + 1));
      if (!mat.has(key)) mat.set(key, new Map());
      mat.get(key)!.set(yy, material(w));
    }
  }
  return { top, mat };
}
/** The top of the column whose centre is at (x, z) (map units, the lattice is whole units: the cell is floor(x), floor(z)). */
const topAt = (c: ReturnType<typeof columns>, x: number, z: number) => c.top.get(`${Math.floor(x)},${Math.floor(z)}`) ?? 0;

const BASE = { at: [520, 0], floor: 0, top: 12, shape: { kind: "circle", radius: 20 }, tread: { jitter: 0 }, strata: { step: 3 } };

test("build_mesa is registered with a strict schema", () => {
  assert.ok(tool);
  assert.equal(tool.inputSchema.additionalProperties, false);
  // floor is asked for by the handler unless onSkin seats the mesa (Dustcrown round 3)
  assert.deepEqual((tool.inputSchema as any).required, ["top", "shape"]);
  assert.deepEqual((tool.inputSchema as any).properties.shape.properties.kind.enum, ["circle", "ellipse", "polygon"]);
  for (const k of ["costOnly", "maxCells", "maxBlocks", "talus", "cap", "strata", "tread", "wobble"]) assert.ok((tool.inputSchema as any).properties[k], k);
});

test("walkable treads: a plateau at top, steps of the riser and tread asked for, one undo step", async () => {
  const { store, s } = session();
  const r = await call(s, { ...BASE, tread: { width: 3, jitter: 0 } });
  assert.ok(r.applied);
  assert.equal(r.mesa.steps, 4);
  assert.equal(r.mesa.topY.map, 12);
  assert.equal(r.mesa.topY.world, 240);
  assert.equal(r.mesa.tread.walkable, true);
  assert.equal(r.mesa.tread.width, 3);
  const c = columns(store);
  // from the rim in: tread k (k = 0..3) spans 3k..3k+3 inside the outline, at height 3(k + 1)
  for (const [inside, h] of [[0.5, 3], [2.5, 3], [3.5, 6], [5.5, 6], [6.5, 9], [8.5, 9], [9.5, 12], [19.5, 12]] as const)
    assert.equal(topAt(c, 520 + 20 - inside, 0.5), h, `${inside} inside the rim`);
  assert.equal(topAt(c, 520 + 21.5, 0.5), 0, "nothing outside the outline");
  assert.equal(store.stack.depth, 1);
  assert.deepEqual(limitProblems(store), []);
  store.undo();
  assert.equal(columns(store).top.size, 0);
});

test("a sheer wall: tread.walkable false puts every step at the outline", async () => {
  const { store, s } = session();
  const r = await call(s, { ...BASE, tread: { walkable: false } });
  assert.equal(r.mesa.tread.width, 0);
  const c = columns(store);
  assert.equal(topAt(c, 520 + 19.5, 0.5), 12);
  assert.equal(topAt(c, 520 + 20.5, 0.5), 0);
  assert.equal(topAt(c, 520 + 15, 0.5), 12);
  // with a width, walkable: false is a setback (a stepped face), not a refusal (round 2)
  const sb: any = await tool.handler(session().s, { ...BASE, tread: { walkable: false, width: 4, jitter: 0 } });
  assert.equal(sb.mesa.tread.setback, true);
  assert.equal(sb.mesa.tread.width, 4);
});

test("risers vary by riserJitter but always reach the top; treads wander by jitter", async () => {
  const { store, s } = session();
  const r = await call(s, { ...BASE, top: 18, strata: { step: 3, riserJitter: 0.4 }, tread: { width: 3, jitter: 1, seed: 4 } });
  assert.equal(r.mesa.steps, 6);
  assert.equal(r.mesa.risers.length, 6);
  const sum = r.mesa.risers.reduce((p: number, v: number) => p + v, 0);
  assert.ok(Math.abs(sum - 18) < 0.02, `risers add up to ${sum}`);
  assert.ok(new Set(r.mesa.risers).size > 2, "the risers differ");
  assert.equal(topAt(columns(store), 520, 0.5), 18);
  // a ragged tread edge: the 3rd step's edge is not at one distance round the mesa
  const c = columns(store);
  const edges = new Set<number>();
  for (let a = 0; a < 360; a += 10) {
    let e = -1;
    for (let d = 0; d < 20; d += 0.25) if (topAt(c, 520 + Math.cos((a * Math.PI) / 180) * (20 - d), Math.sin((a * Math.PI) / 180) * (20 - d)) >= 9) { e = d; break; }
    edges.add(Math.round(e * 2));
  }
  assert.ok(edges.size >= 3, `edge distances ${[...edges]}`);
});

test("talus: a skirt at the foot that falls to nothing, the cliff above it", async () => {
  const { store, s } = session();
  const r = await call(s, { ...BASE, top: 16, strata: { step: 3 }, tread: { width: 3, jitter: 0 }, talus: { height: 4, width: 8, material: 7 } });
  assert.equal(r.mesa.talus.height, 4);
  assert.equal(r.mesa.steps, 4);
  const c = columns(store);
  assert.equal(topAt(c, 520 + 19.5, 0.5), 4 + 3, "the first tread stands a riser above the skirt's top");
  assert.equal(topAt(c, 520 + 20.5, 0.5), 4, "the skirt's top at the cliff");
  assert.ok(topAt(c, 520 + 24.5, 0.5) < topAt(c, 520 + 21.5, 0.5), "falls away");
  assert.equal(topAt(c, 520 + 28.5, 0.5), 0);
  const m = c.mat.get(`${520 + 22},0`)!;
  assert.equal(m.get(m.size ? Math.max(...m.keys()) : 0), 7, "the skirt's material");
  const rim = c.mat.get(`${520 + 19},0`)!;
  assert.notEqual(rim.get(Math.max(...rim.keys())), 7, "the cliff is not the skirt's material");
  assert.throws(() => tool.handler(session().s, { ...BASE, top: 2, talus: { height: 5 }, strata: { step: 3 } }), /talus needs room/);
});

test("strata bands repeat from the floor and the cap tops the plateau", async () => {
  const { store, s } = session();
  const r = await call(s, { ...BASE, top: 12, strata: { step: 3, bands: [{ material: 4, thickness: 2 }, { material: 6, thickness: 2 }] }, cap: { material: 12 }, tread: { width: 3, jitter: 0 } });
  assert.equal(r.mesa.strataBands, 6);
  const c = columns(store);
  const col = c.mat.get(`${520},0`)!;
  assert.equal(col.get(0), 4);
  assert.equal(col.get(1), 4);
  assert.equal(col.get(2), 6);
  assert.equal(col.get(3), 6);
  assert.equal(col.get(4), 4);
  assert.equal(col.get(10), 6);
  assert.equal(col.get(11), 12, "the cap");
  const rim = c.mat.get(`${520 + 19},0`)!;
  assert.equal(rim.get(2), 3 > 0 ? 6 : 0, "bands run under the treads too");
  assert.notEqual(rim.get(2), 12, "the cap is the plateau's only");
  assert.throws(() => tool.handler(session().s, { ...BASE, cap: { material: 12, depth: 9 } }), /cap/);
});

test("ellipse and polygon outlines; wobble moves the edge; the footprint circles cover every standing column", async () => {
  const { store, s } = session();
  const e = await call(s, { at: [520, 0], floor: 0, top: 9, shape: { kind: "ellipse", radii: [22, 8], rotation: 30 }, strata: { step: 3 }, tread: { width: 2.5, jitter: 0 }, wobble: { amount: 1.5, seed: 3 }, costOnly: true });
  assert.equal(e.applied, false);
  assert.ok(e.exclude.length >= 2, "a long mesa needs more than one circle");
  assert.deepEqual(e.exclude, e.clip);
  const plain = await call(s, { ...BASE, shape: { kind: "ellipse", radii: [22, 8], rotation: 30 }, top: 9, tread: { width: 2.5, jitter: 0 }, wobble: { amount: 1.5, seed: 3 } });
  assert.ok(plain.applied);
  const c = columns(store);
  const circles = plain.exclude as { x: number; z: number; radius: number }[];
  for (const key of c.top.keys()) {
    const [x, z] = key.split(",").map((v) => Number(v) + 0.5);
    assert.ok(circles.some((q) => Math.hypot(x - q.x, z - q.z) <= q.radius + 0.01), `column ${key} is in a circle`);
  }
  assert.ok(c.top.size > 200);
  // a concave polygon (an L): the notch is empty
  const { store: st2, s: s2 } = session();
  const L = [[500, -10], [530, -10], [530, 0], [510, 0], [510, 20], [500, 20]];
  const p = await call(s2, { floor: 0, top: 6, shape: { kind: "polygon", points: L }, strata: { step: 3 }, tread: { width: 2, jitter: 0 } });
  assert.ok(p.applied);
  const c2 = columns(st2);
  assert.equal(topAt(c2, 505, 10), 6);
  assert.equal(topAt(c2, 525, 10), 0, "the notch");
  assert.equal(topAt(c2, 525, -5), 6);
  assert.deepEqual(limitProblems(st2), []);
  assert.ok(store.stack.depth === 1);
});

test("costOnly edits nothing and reports the plan; maxCells and the size limits refuse", async () => {
  const { store, s } = session();
  const r = await call(s, { ...BASE, costOnly: true, maxCells: 100000 });
  assert.equal(r.applied, false);
  assert.equal(store.stack.depth, 0);
  assert.equal(columns(store).top.size, 0);
  assert.ok(r.mesa.plateau.columns > 0);
  assert.ok(r.voxelCells > 0 || r.cells > 0 || r.budget, JSON.stringify(Object.keys(r)));
  // a too-narrow walkable tread, a plateau that does not survive the steps, a footprint over the grid
  assert.throws(() => tool.handler(session().s, { ...BASE, tread: { width: 1 } }), /too narrow to walk/);
  assert.throws(() => tool.handler(session().s, { ...BASE, shape: { kind: "circle", radius: 5 }, top: 30, strata: { step: 1 } }), /plateau is gone/);
  assert.throws(() => tool.handler(session().s, { ...BASE, shape: { kind: "circle", radius: 300 } }), /footprint is/);
  assert.throws(() => tool.handler(session().s, { floor: 0, top: 5, shape: { kind: "circle", radius: 3 } }), /at:/);
  assert.throws(() => tool.handler(session().s, { ...BASE, top: 0 }), /top is above floor/);
  assert.throws(() => tool.handler(session().s, { ...BASE, grid: [[1]] }), /itself/);
});

test("voxelSize 2.5 and world units: top lands where asked and the mesa fits a cell budget", async () => {
  const { s } = session();
  const r = await call(s, { at: [10400, 0], floor: 0, top: 240, units: "world", shape: { kind: "circle", radius: 600 }, voxelSize: 2.5, strata: { step: 1 }, tread: { width: 100, jitter: 0 }, costOnly: true });
  assert.equal(r.mesa.topY.map, 12);
  assert.equal(r.mesa.riser.map, 2.5);
  assert.equal(r.mesa.steps, 5);
  assert.equal(r.mesa.footprint.units, "world");
});

test("without a mesa tool call, heightfield_region is untouched: its schema keeps expr and grid", async () => {
  const hf = await import("../src/tools/heightfield");
  assert.ok((hf.schema.properties as any).expr && (hf.schema.properties as any).grid);
  assert.equal(hf.tools.length, 2);
});

test("a plateau of a few columns carries a warning", async () => {
  const { s } = session();
  const r = await call(s, { ...BASE, shape: { kind: "circle", radius: 10.4 }, tread: { width: 3, jitter: 0 }, costOnly: true });
  assert.ok(r.mesa.plateau.columns < 9, String(r.mesa.plateau.columns));
  assert.match(r.mesa.warning, /a tower, not a mesa/);
  assert.equal((await call(s, { ...BASE, costOnly: true })).mesa.warning, undefined);
});
