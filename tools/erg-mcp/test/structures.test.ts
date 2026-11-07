// Tool group "structures" on the synthetic 12-frame level (bundled for Node by test.ps1): the shape builders, writing into
// existing (rotated, scaled) frames and into new blocks, undo, the patch the editor would save, limits.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, LIMITS, isSolid, material, type Loaded, type Scene } from "../src/core";
import { tools } from "../src/tools/structures";
import { cellCentre, grids, limitProblems } from "../src/lib/voxel";
import { laneTop } from "../src/lib/structures-shapes";
import type { Session } from "../src/session";

function loaded(): Loaded {
  const s: Scene = JSON.parse(readFileSync(new URL("../../../../tests/fixtures/erg/synthetic-12.json", import.meta.url), "utf8"));
  s.frames.find((f) => f.id === 1)!.name = "Scene";   // blocks go under it; the synthetic level has none
  const blobs = new Map<number, ArrayBuffer>();
  for (const [ref, buf] of syntheticBlobs(s) as Map<number, Uint8Array>) blobs.set(ref, buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength) as ArrayBuffer);
  return { scene: s, blobs };
}

function session() {
  const store = new EditorStore("t", loaded(), loaded());
  const s = { requireProject: () => ({ store, id: "t", info: {} }), get store() { return store; } } as unknown as Session;
  return { store, s };
}

const tool = (name: string) => tools.find((t) => t.name === name)!;
async function call(s: Session, name: string, args: Record<string, unknown>): Promise<any> {
  return tool(name).handler(s, args);
}

/** Every solid voxel of the added blocks as absolute lattice cells "x,y,z" (the Scene frame is the identity at the origin). */
function blockCells(store: EditorStore): Map<string, number> {
  const out = new Map<string, number>();
  for (const g of grids(store)) {
    if (!g.frame.new) continue;
    const o = g.frame.pos.map((p, a) => p - g.size[a] / 2);
    for (let z = 0; z < g.size[2]; z++) for (let x = 0; x < g.size[0]; x++) for (let y = 0; y < g.size[1]; y++) {
      const w = g.words[(z * g.size[0] + x) * g.size[1] + y];
      if (isSolid(w)) out.set(`${o[0] + x},${o[1] + y},${o[2] + z}`, material(w));
    }
  }
  return out;
}
const has = (c: Map<string, number>, x: number, y: number, z: number) => c.has(`${x},${y},${z}`);

test("every tool is registered with a valid schema and unique name", () => {
  const names = tools.map((t) => t.name);
  assert.equal(new Set(names).size, names.length);
  for (const n of ["build_castle", "build_wall", "build_tower", "build_keep", "build_ramp", "build_stairs", "dig_crater", "dig_tunnel", "build_arch", "build_bridge"])
    assert.ok(names.includes(n), n);
  for (const t of tools) assert.equal(t.inputSchema.additionalProperties, false, t.name);
});

test("castle in empty space: new blocks, correct shape, one undo step, a patch the validator accepts", async () => {
  const { store, s } = session();
  const r = await call(s, "build_castle", { at: [500, 10, 500], size: [30, 24], wallHeight: 6, wallThickness: 3, material: 5, foundation: 2 });
  assert.equal(r.structure, "castle");
  assert.ok(r.voxels.added > 1000, `added ${r.voxels.added}`);
  assert.ok(r.newBlocks.length >= 2 && r.newBlocks.length <= 12, `blocks ${r.newBlocks.length}`);
  for (const b of r.newBlocks) assert.ok(b.size.every((v: number) => v >= 1 && v <= 32));
  assert.equal(store.stack.depth, 1);
  assert.deepEqual(limitProblems(store), []);
  const c = blockCells(store);
  assert.equal(c.size, r.voxels.added);
  assert.ok([...c.values()].every((m) => m === 5));
  // north wall, x 485..515, z 488..491 (24 deep => 488..512), y 10..16
  assert.ok(has(c, 500, 10, 488) && has(c, 500, 15, 489) && !has(c, 500, 16, 490), "wall body is 6 high, merlons only on the outer row");
  assert.ok(has(c, 490, 8, 500) && has(c, 490, 9, 500) && !has(c, 490, 7, 500), "foundation 2 under the courtyard");
  // gate on the south side, 4 wide, carved through the wall
  assert.ok(!has(c, 500, 10, 510) && !has(c, 501, 11, 510) && has(c, 505, 10, 510), "gate opening");
  // courtyard floor at y=10 is open: the cell above the foundation is empty away from the keep
  assert.ok(!has(c, 490, 10, 500));
  // towers on the corners rise above the wall
  assert.ok(has(c, 487, 16, 490), "tower top is wallHeight+3");
  assert.ok(r.usage.newBlocks.startsWith(`${r.newBlocks.length}/`));
  const p = store.patch();
  assert.equal(p.ops.filter((o) => o.op === "add").length >= 0, true);
  assert.equal(store.scene.frames.filter((f) => f.new).length, r.newBlocks.length);
  store.undo();
  assert.equal(store.scene.frames.filter((f) => f.new).length, 0);
  assert.equal(store.patch().ops.length, 0);
  store.redo();
  assert.equal(store.scene.frames.filter((f) => f.new).length, r.newBlocks.length);
});

test("dryRun changes nothing and reports what would be used", async () => {
  const { store, s } = session();
  const r = await call(s, "build_tower", { at: [500, 0, 500], radius: 5, height: 12, dryRun: true });
  assert.equal(r.dryRun, true);
  assert.ok(r.voxels.added > 100 && r.newBlocks.length === 1);
  assert.equal(store.stack.depth, 0);
  assert.equal(store.scene.frames.filter((f) => f.new).length, 0);
});

test("hollow tower: wall, door, open top, rim merlons", async () => {
  const { store, s } = session();
  await call(s, "build_tower", { at: [500, 0, 500], shape: "square", radius: 5, height: 10, hollow: true, wallThickness: 2, door: "e", material: 3 });
  const c = blockCells(store);
  assert.ok(!has(c, 504, 0, 499) && !has(c, 504, 2, 499) && has(c, 504, 0, 497), "door on the east side is carved through the wall");
  assert.ok(has(c, 504, 5, 490) === false);
  assert.ok(!has(c, 500, 0, 500) && !has(c, 500, 5, 500), "interior is empty");
  assert.ok(has(c, 495, 5, 500), "west wall stands");
  assert.ok(has(c, 495, 9, 500), "wall to the top");
  assert.ok([...c.keys()].some((k) => k.split(",")[1] === "10"), "merlons one row above the top");
});

test("wall along a sloping polyline", async () => {
  const { store, s } = session();
  const r = await call(s, "build_wall", { points: [[500, 0, 500], [520, 10, 500], [520, 10, 520]], height: 4, thickness: 2, crenellations: false, material: 1 });
  assert.equal(r.structureInfo.segments, 2);
  const c = blockCells(store);
  // base follows the slope; the wall column at x=510 starts near y=5
  const col = (x: number, z: number) => [...c.keys()].map((k) => k.split(",").map(Number)).filter((p) => p[0] === x && p[2] === z).map((p) => p[1]);
  const at510 = col(510, 500);
  assert.ok(at510.length >= 3 && Math.min(...at510) >= 4 && Math.max(...at510) <= 9, at510.join(","));
  assert.ok(col(520, 515).length === 4 && Math.min(...col(520, 515)) === 10);
  await assert.rejects(() => call(s, "build_wall", { points: [[1, 0, 1], [1, 5, 1]] }), /same place/);
});

test("ramp: slope limit, switchback lanes, walkable top", async () => {
  const { store, s } = session();
  await assert.rejects(() => call(s, "build_ramp", { from: [500, 0, 500], to: [510, 20, 500] }), /switchback/);
  const gentle = await call(s, "build_ramp", { from: [500, 0, 500], to: [520, 10, 500], width: 3, material: 2 });
  assert.equal(gentle.structureInfo.lanes, 1);
  assert.ok(gentle.structureInfo.slopeDegrees < 27);
  // heights along the ramp rise by at most 1 per column
  const c = blockCells(store);
  const top = (x: number, z: number) => Math.max(...[...c.keys()].map((k) => k.split(",").map(Number)).filter((p) => p[0] === x && p[2] === z).map((p) => p[1]));
  let prev = top(500, 500);
  for (let x = 501; x < 520; x++) { const t = top(x, 500); assert.ok(t - prev >= 0 && t - prev <= 1, `x=${x} step ${t - prev}`); prev = t; }
  assert.ok(prev >= 9);

  const sw = await call(s, "build_ramp", { from: [500, 0, 560], to: [520, 30, 560], width: 3, switchback: true, maxSlope: 40, material: 2 });
  assert.ok(sw.structureInfo.lanes >= 2, JSON.stringify(sw.structureInfo));
  assert.ok(sw.structureInfo.slopeDegrees <= 40.01);
  assert.ok(sw.structureInfo.top.map[1] === 30);
  const c2 = blockCells(store);
  const hi = Math.max(...[...c2.keys()].map((k) => Number(k.split(",")[1])));
  assert.ok(hi >= 29);
});

test("stairs: step count and tread", async () => {
  const { store, s } = session();
  const r = await call(s, "build_stairs", { from: [500, 0, 500], to: [508, 4, 500], width: 3, material: 4 });
  assert.equal(r.structureInfo.steps, 4);
  assert.equal(r.structureInfo.tread, 2);
  const c = blockCells(store);
  const top = (x: number) => Math.max(...[...c.keys()].map((k) => k.split(",").map(Number)).filter((p) => p[0] === x && p[2] === 500).map((p) => p[1]));
  assert.deepEqual([500, 502, 504, 506].map(top), [0, 1, 2, 3]);
  assert.equal(top(501), 0);
  await assert.rejects(() => call(s, "build_stairs", { from: [500, 0, 500], to: [502, 10, 500] }), /under 1 voxel/);
  await assert.rejects(() => call(s, "build_stairs", { from: [500, 0, 500], to: [510, 0, 500] }), /same height/);
  assert.equal(laneTop({ a: [0, 0], d: [1, 0], len: 8, w: 3, yA: 4, yB: 0, steps: 4, floorY: 0, thick: 1, mat: 1 }, 0.5), 4);
});

test("crater into a plateau block, then a tunnel through it", async () => {
  const { store, s } = session();
  // a solid plateau: a tower 30 wide would be round; use a wide square tower of height 8 (radius = half side)
  await call(s, "build_tower", { at: [500, 0, 500], shape: "square", radius: 14, height: 8, crenellations: false, material: 6 });
  const before = blockCells(store);
  assert.ok(has(before, 500, 7, 500) && !has(before, 500, 8, 500));
  const cr = await call(s, "dig_crater", { at: [500, 8, 500], radius: 8, depth: 4, rimHeight: 2, rimWidth: 3, material: 6, bowlMaterial: 9 });
  assert.ok(cr.voxels.removed > 100, JSON.stringify(cr.voxels));
  const c = blockCells(store);
  assert.ok(!has(c, 500, 4, 500) && has(c, 500, 3, 500), "bowl bottom is depth 4 under the surface at the centre");
  assert.equal(c.get("500,3,500"), 9, "bowl surface repainted");
  assert.ok(has(c, 509, 8, 500) || has(c, 508, 8, 500), "rim stands above the plateau");
  assert.ok(!has(c, 500, 8, 500));
  const t = await call(s, "dig_tunnel", { points: [[490, 3, 490], [510, 3, 510]], radius: 1.5, shape: "round" });
  assert.ok(t.voxels.removed > 20, JSON.stringify(t.voxels));
  assert.ok(!has(blockCells(store), 495, 3, 495) && has(blockCells(store), 495, 6, 495));
  assert.equal(store.stack.depth, 3);
  store.undo(); store.undo();
  assert.deepEqual([...blockCells(store)].sort(), [...before].sort());
});

test("arch and bridge", async () => {
  const { store, s } = session();
  await call(s, "build_arch", { at: [500, 0, 500], span: 6, rise: 3, pier: 2, depth: 3, crown: 2, material: 1 });
  const c = blockCells(store);
  assert.ok(!has(c, 500, 0, 499) && !has(c, 497, 2, 499) && has(c, 495, 0, 499) && has(c, 503, 0, 499), "piers either side of the opening");
  assert.ok(!has(c, 500, 5, 499) && has(c, 500, 6, 499) && has(c, 500, 7, 499) && !has(c, 500, 8, 499), "opening to 6, crown to 8");
  const b = await call(s, "build_bridge", { from: [600, 10, 600], to: [630, 10, 600], width: 4, deckThickness: 2, pierSpacing: 10, downTo: 0, material: 2 });
  assert.equal(b.structureInfo.length, 30);
  const cc = blockCells(store);
  assert.ok(has(cc, 610, 9, 600) && has(cc, 610, 0, 600), "deck and pier");
  assert.ok(has(cc, 615, 10, 598) && !has(cc, 615, 10, 600) && has(cc, 615, 9, 600), "rail on the edge, deck 2 thick");
  assert.ok(!has(cc, 615, 3, 600), "no support between piers");
  const arched = await call(s, "build_bridge", { from: [700, 12, 700], to: [740, 12, 700], width: 4, pierSpacing: 10, pierWidth: 2, downTo: 0, arched: true, material: 2 });
  assert.ok(arched.voxels.added > 100);
  const ca = blockCells(store);
  assert.ok(!has(ca, 705, 3, 700) && has(ca, 710, 3, 700) && has(ca, 705, 11, 700), "arch opens under the deck between piers");
});

test("existing, rotated and scaled frames are edited in place; place existing leaves the rest out", async () => {
  const { store, s } = session();
  const g = grids(store).find((x) => x.id === 4)!;   // land0: rotated, scaled 1.5, 7x2x11 voxels
  const mid = cellCentre(g, 3, 1, 5);
  const sz = g.words.filter((w) => isSolid(w)).length;
  const r = await call(s, "dig_crater", { at: mid, radius: 3, depth: 2, place: "existing", rimWidth: 0, material: 1 });
  assert.equal(r.newBlocks.length, 0);
  assert.ok(r.frames.some((f: { id: number }) => f.id === 4), JSON.stringify(r.frames));
  assert.ok(r.voxels.removed >= 0);
  assert.ok(g.words.filter((w) => isSolid(w)).length <= sz);
  const t = await call(s, "build_tower", { at: mid, radius: 2, height: 3, place: "existing", material: 7, crenellations: false });
  assert.equal(t.newBlocks.length, 0);
  assert.ok(t.voxels.added + t.voxels.repainted > 0, JSON.stringify(t.voxels));
  assert.equal(store.stack.depth, 2);
  const ops = store.patch().ops.filter((o) => o.op === "voxels");
  assert.ok(ops.length >= 1);
  assert.deepEqual(limitProblems(store), []);
  store.undo(); store.undo();
  assert.equal(store.patch().ops.length, 0);
});

test("limits: too many blocks, an oversized box, a level without a Scene frame", async () => {
  const { store, s } = session();
  // 3 x 3 chunks of towers well apart use many blocks each; a wide wall needs ceil(len/32) per row
  await assert.rejects(() => call(s, "build_wall", { points: [[0, 0, 0], [3000, 0, 0]], height: 20, thickness: 20 }), /at most 6000000|new blocks/);
  await assert.rejects(() => call(s, "build_wall", { points: [[0, 0, 900], [32 * 70, 0, 900]], height: 2, thickness: 2, crenellations: false }), /new blocks but the level has room/);
  assert.equal(store.stack.depth, 0);
  assert.equal(LIMITS.newFrames, 64);
  const lone = new EditorStore("t", loaded(), loaded());
  lone.scene.frames.find((f) => f.id === 1)!.name = "root";
  const s2 = { requireProject: () => ({ store: lone }), get store() { return lone; } } as unknown as Session;
  const r = await call(s2, "build_tower", { at: [500, 0, 500], dryRun: true });
  assert.ok(r.notes.some((n: string) => /no Scene frame/.test(n)));
  assert.equal(r.newBlocks.length, 0);
});

test("world units scale every length", async () => {
  const { s } = session();
  const a = await call(s, "build_tower", { at: [500, 0, 500], radius: 4, height: 10, dryRun: true, crenellations: false });
  const b = await call(s, "build_tower", { at: [10000, 0, 10000], radius: 80, height: 200, dryRun: true, crenellations: false, units: "world" });
  assert.equal(a.voxels.added, b.voxels.added);
});
