// build_compound on the synthetic 12-frame level: each shape kind, carve order, material bands, tiers, scale, packing,
// the limits and one undo step.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, isSolid, material, type Loaded, type Scene } from "../src/core";
import { tools } from "../src/tools/structures";
import { grids, limitProblems } from "../src/lib/voxel";
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
const call = async (s: Session, args: Record<string, unknown>): Promise<any> => tool("build_compound").handler(s, args);

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
const colWidth = (c: Map<string, number>, y: number, cx: number, cz: number) => {
  let lo = Infinity, hi = -Infinity;
  for (const key of c.keys()) { const [x, yy, z] = key.split(",").map(Number); if (yy === y && z === cz && Math.abs(x - cx) <= 15) { lo = Math.min(lo, x); hi = Math.max(hi, x); } }
  return hi < lo ? 0 : hi - lo + 1;
};

test("build_compound is registered with a strict schema", () => {
  const t = tool("build_compound");
  assert.ok(t);
  assert.equal(t.inputSchema.additionalProperties, false);
  assert.deepEqual((t.inputSchema as any).required, ["at", "shapes"]);
  const item = (t.inputSchema as any).properties.shapes.items;
  assert.equal(item.additionalProperties, false);
  assert.deepEqual(item.properties.kind.enum, ["box", "cylinder", "cone", "dome", "tiers"]);
  // the older structure tools are all still there
  for (const n of ["build_castle", "build_wall", "build_tower", "build_keep", "build_ramp", "build_stairs", "dig_crater", "dig_tunnel", "build_arch", "build_bridge"])
    assert.ok(tools.some((x) => x.name === n), n);
});

test("tiers: a stepped plinth with the risers and ledges asked for, one undo step", async () => {
  const { store, s } = session();
  const r = await call(s, { at: [500, 10, 500], material: 5, shapes: [{ kind: "tiers", size: [20, 20], steps: 3, riser: 2, ledge: 3 }] });
  assert.equal(r.structure, "compound");
  assert.deepEqual(r.structureInfo.size, [20, 6, 20]);
  assert.equal(r.structureInfo.tipY.map, 16);
  assert.equal(r.structureInfo.tipY.world, 320);
  assert.equal(store.stack.depth, 1);
  const c = blockCells(store);
  // step 0 (y 10-11) spans x 490..509, step 1 (y 12-13) x 493..506, step 2 (y 14-15) x 496..503
  assert.ok(has(c, 490, 10, 500) && has(c, 509, 11, 500) && !has(c, 489, 10, 500) && !has(c, 510, 11, 500));
  assert.ok(!has(c, 490, 12, 500) && has(c, 493, 12, 500) && has(c, 506, 13, 500) && !has(c, 507, 13, 500));
  assert.ok(!has(c, 495, 14, 500) && has(c, 496, 14, 500) && has(c, 503, 15, 500) && !has(c, 504, 15, 500));
  assert.ok(!has(c, 500, 16, 500) && !has(c, 500, 9, 500), "6 high, standing on y 10");
  assert.equal(c.size, r.voxels.added);
  assert.deepEqual(limitProblems(store), []);
  store.undo();
  assert.equal(store.scene.frames.filter((f) => f.new).length, 0);
});

test("round tiers and a ledge that eats the top step is refused", async () => {
  const { store, s } = session();
  await call(s, { at: [500, 0, 500], material: 2, shapes: [{ kind: "tiers", round: true, radius: 10, steps: 2, riser: 3, ledge: 4 }] });
  const c = blockCells(store);
  assert.ok(has(c, 491, 0, 500) && !has(c, 489, 0, 500) && !has(c, 493, 3, 500) && has(c, 494, 3, 500));
  await assert.rejects(call(s, { at: [500, 0, 500], shapes: [{ kind: "tiers", size: [10, 10], steps: 3, riser: 2, ledge: 3 }] }), /leave nothing of the top step/);
});

test("carve after fill opens a niche; a fill after the carve closes it again", async () => {
  const a = session(), b = session(), c3 = session();
  const body = { kind: "box", size: [20, 8], height: 10 };
  const niche = { kind: "box", carve: true, at: [0, 3, -4], size: [4, 4], height: 4 };
  await call(a.s, { at: [500, 0, 500], material: 4, shapes: [body, niche] });
  const ca = blockCells(a.store);
  assert.ok(has(ca, 500, 2, 496) && !has(ca, 500, 3, 496) && !has(ca, 499, 6, 496) && has(ca, 500, 7, 496) && has(ca, 500, 3, 500) && has(ca, 497, 3, 496));
  // the carve first has nothing to carve; the body then fills the niche's cells
  await call(b.s, { at: [500, 0, 500], material: 4, shapes: [niche, body] });
  assert.ok(has(blockCells(b.store), 500, 3, 496));
  await call(c3.s, { at: [500, 0, 500], material: 4, shapes: [body, niche, { kind: "box", at: [0, 3, -4], size: [2, 2], height: 2, material: 9 }] });
  const cc = blockCells(c3.store);
  assert.equal(cc.get("499,3,496"), 9);
});

test("a carve may open a frame that already holds terrain", async () => {
  const { store, s } = session();
  const r = await call(s, { at: [500, 0, 500], material: 3, shapes: [{ kind: "box", size: [10, 10], height: 6 }, { kind: "box", carve: true, at: [0, 0, 0], size: [4, 4], height: 3 }] });
  assert.ok(r.voxels.added > 0);
  assert.ok(!has(blockCells(store), 500, 1, 500));
});

test("bands paint by height above each shape's base, through a rotation too", async () => {
  for (const rotation of [0, 90]) {
    const { store, s } = session();
    await call(s, { at: [500, 20, 500], material: 1, rotation, shapes: [{ kind: "cylinder", at: [0, 2, 0], radius: 5, height: 9, bands: [{ from: 0, to: 3, material: 7 }, { from: 6, material: 8 }] }] });
    const c = blockCells(store);
    assert.equal(c.get("500,22,500"), 7, `rotation ${rotation}`);
    assert.equal(c.get("500,24,500"), 7);
    assert.equal(c.get("500,25,500"), 1);
    assert.equal(c.get("500,28,500"), 8);
    assert.equal(c.get("500,30,500"), 8);
    assert.ok(!has(c, 500, 31, 500) && !has(c, 500, 21, 500));
  }
  const { s } = session();
  await assert.rejects(call(s, { at: [0, 0, 0], shapes: [{ kind: "box", size: [4, 4], height: 4 }, { kind: "box", carve: true, size: [2, 2], height: 2, bands: [{ material: 2 }] }] }), /bands paint a fill/);
});

test("cone: a spire narrows to its tip, a frustum keeps its top radius", async () => {
  const { store, s } = session();
  await call(s, { at: [500, 0, 500], material: 2, shapes: [{ kind: "cone", radius: 8, height: 16 }, { kind: "cone", at: [30, 0, 0], radius: 8, topRadius: 4, height: 16 }] });
  const c = blockCells(store);
  assert.ok(colWidth(c, 0, 500, 500) >= 15 && colWidth(c, 8, 500, 500) <= 9 && colWidth(c, 14, 500, 500) <= 4 && colWidth(c, 14, 500, 500) >= 1);
  assert.ok(colWidth(c, 15, 530, 500) >= 7 && colWidth(c, 15, 530, 500) <= 9, "the frustum's top row is about 2 x topRadius");
});

test("dome: hemisphere, flat cap and the onion's bulge", async () => {
  const { store, s } = session();
  await call(s, { at: [500, 0, 500], material: 2, shapes: [
    { kind: "dome", radius: 10, height: 10 },
    { kind: "dome", at: [40, 0, 0], radius: 10, height: 4 },
    { kind: "dome", at: [80, 0, 0], radius: 10, height: 24, profile: "onion" },
  ] });
  const c = blockCells(store);
  // hemisphere: the base row is the full circle, the top row a small disc
  assert.ok(colWidth(c, 0, 500, 500) >= 19 && colWidth(c, 9, 500, 500) <= 8 && colWidth(c, 9, 500, 500) >= 1 && !has(c, 500, 10, 500));
  // the cap of height 4 over radius 10 is flatter: 4 high, and at 3 up already narrower than the hemisphere's
  assert.ok(has(c, 540, 3, 500) && !has(c, 540, 4, 500) && colWidth(c, 3, 540, 500) <= 14);
  // the onion is wider a third of the way up than at its foot, and ends in a point
  assert.ok(colWidth(c, 7, 580, 500) > colWidth(c, 0, 580, 500), "bulge above the neck");
  assert.ok(colWidth(c, 7, 580, 500) >= 21, "a tenth wider than the base circle");
  assert.ok(colWidth(c, 22, 580, 500) <= 4);
});

test("parts, scale and costOnly: the cost is read without changing anything, and maxCells refuses", async () => {
  const { store, s } = session();
  const shapes = [{ kind: "tiers", size: [40, 40], steps: 3, riser: 6, ledge: 6 }, { kind: "cylinder", at: [0, 18, 0], radius: 6, height: 10, part: 2 }];
  const cost = await call(s, { at: [500, 0, 500], material: 3, shapes, costOnly: true });
  assert.equal(cost.costOnly, true);
  assert.equal(cost.byPacking.length, 3);
  assert.ok(cost.cheapest.cells > 0);
  assert.equal(store.stack.depth, 0);
  const coarse = await call(s, { at: [500, 0, 500], material: 3, shapes, costOnly: true, blockScale: 3, place: "new" });
  const fine = await call(s, { at: [500, 0, 500], material: 3, shapes, costOnly: true, place: "new" });
  assert.ok(coarse.cheapest.cells < fine.cheapest.cells / 5, `${coarse.cheapest.cells} vs ${fine.cheapest.cells}`);
  await assert.rejects(call(s, { at: [500, 0, 500], material: 3, shapes, maxCells: 100 }), /cells|maxCells/i);
  assert.equal(store.stack.depth, 0);
  const parts = await call(s, { at: [500, 0, 500], material: 3, shapes, scale: 3, place: "new", packing: "parts" });
  assert.ok(parts.cells.newBlocks > 0 && parts.cells.newBlocks <= coarse.cheapest.cells + 2000);
  assert.deepEqual(limitProblems(store), []);
});

test("ground and sink seat the structure on the terrain; units world scales every length", async () => {
  const a = session(), b = session();
  const m = await call(a.s, { at: [500, 12, 500], material: 3, shapes: [{ kind: "box", size: [6, 6], height: 4 }], dryRun: true });
  const w = await call(b.s, { at: [10000, 240, 10000], units: "world", material: 3, shapes: [{ kind: "box", size: [120, 120], height: 80 }], dryRun: true });
  assert.equal(m.voxels.added, w.voxels.added);
  assert.deepEqual(w.structureInfo.size, [120, 80, 120]);
  assert.equal(w.structureInfo.tipY.map, 16);
});

test("bad lists are refused with the shape named", async () => {
  const { s } = session();
  const at = [500, 0, 500];
  await assert.rejects(call(s, { at, shapes: [{ kind: "box", size: [4, 4] }] }), /shapes\[0\] \(box\): height/);
  await assert.rejects(call(s, { at, shapes: [{ kind: "box", size: [4, 4], height: 3 }, { kind: "cylinder", height: 3 }] }), /shapes\[1\] \(cylinder\): radius/);
  await assert.rejects(call(s, { at, shapes: [{ kind: "box", size: [4, 4], height: 3, radius: 2 }] }), /radius is not a setting of a box/);
  await assert.rejects(call(s, { at, shapes: [{ kind: "box", carve: true, size: [4, 4], height: 3 }] }), /every shape is a carve/);
  await assert.rejects(call(s, { at, shapes: [] }), /at least one shape/);
  await assert.rejects(call(s, { at, shapes: [{ kind: "tiers", size: [10, 10], steps: 2, riser: 2 }] }), /ledge/);
  await assert.rejects(call(s, { at, shapes: [{ kind: "pyramid" }] }), /kind must be one of/);
});
