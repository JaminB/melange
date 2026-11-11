// Material bands and trim on build_castle, build_wall, build_tower, build_keep and build_compound: by height above the
// floor, the topmost layer, the merlon row and a one-voxel course; no style leaves the voxels as they were.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, isSolid, material, type Loaded, type Scene } from "../src/core";
import { tools } from "../src/tools/structures";
import { grids } from "../src/lib/voxel";
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
const call = async (s: Session, name: string, args: Record<string, unknown>): Promise<any> => tools.find((t) => t.name === name)!.handler(s, args);
const err = async (s: Session, name: string, args: Record<string, unknown>) => { try { await call(s, name, args); } catch (e) { return String((e as Error).message); } return ""; };

/** Every solid voxel of the added blocks as absolute cells "x,y,z" -> material. */
function cells(store: EditorStore): Map<string, number> {
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
/** The materials seen at each height, as {y: sorted distinct materials}. */
function byY(c: Map<string, number>): Map<number, number[]> {
  const out = new Map<number, Set<number>>();
  for (const [k, m] of c) { const y = Number(k.split(",")[1]); (out.get(y) ?? out.set(y, new Set()).get(y)!).add(m); }
  return new Map([...out].map(([y, v]) => [y, [...v].sort((a, b) => a - b)]));
}
const same = (a: Map<string, number>, b: Map<string, number>) => a.size === b.size && [...a].every(([k, v]) => b.get(k) === v);

const TOWER = { at: [0, 0, 0], radius: 4, height: 12, material: 1, place: "new", crenellations: true };

test("every structure tool takes bands and trim", () => {
  for (const n of ["build_castle", "build_wall", "build_tower", "build_keep", "build_compound"]) {
    const p = (tools.find((t) => t.name === n)!.inputSchema as any).properties;
    assert.ok(p.bands && p.trim, n);
    assert.deepEqual(Object.keys(p.bands.items.properties), ["fromY", "toY", "material"]);
  }
  for (const n of ["build_castle", "build_wall", "build_tower", "build_keep"])
    assert.ok((tools.find((t) => t.name === n)!.inputSchema as any).properties.trim.properties.merlonCap, n);
  assert.equal((tools.find((t) => t.name === "build_compound")!.inputSchema as any).properties.trim.properties.merlonCap, undefined);
});

test("no style, an empty list and an empty trim leave the voxels as they were", async () => {
  for (const [name, args] of [["build_tower", TOWER], ["build_keep", { at: [0, 0, 0], size: [12, 10], height: 8, material: 1, place: "new" }],
    ["build_wall", { points: [[0, 0, 0], [14, 0, 0], [14, 2, 9]], material: 1, place: "new", height: 6 }],
    ["build_castle", { at: [0, 0, 0], size: [30, 26], material: 1, place: "new" }]] as [string, Record<string, unknown>][]) {
    const a = session(), b = session(), c = session();
    await call(a.s, name, args);
    await call(b.s, name, { ...args, bands: [], trim: {} });
    await call(c.s, name, { ...args, trim: { topDepth: undefined } });
    assert.ok(cells(a.store).size > 50, name);
    assert.ok(same(cells(a.store), cells(b.store)), name + " empty");
    assert.ok(same(cells(a.store), cells(c.store)), name + " undefined");
  }
});

test("build_tower: bands by height, top layer, merlon cap, a course and their precedence", async () => {
  const { s, store } = session();
  const r = await call(s, "build_tower", { ...TOWER, bands: [{ toY: 4, material: 5 }, { fromY: 4, toY: 8, material: 6 }],
    trim: { top: 7, merlonCap: 8, course: { y: 6, material: 9 } } });
  const m = byY(cells(store));
  for (const y of [0, 1, 2, 3]) assert.deepEqual(m.get(y), [5], "y" + y);
  for (const y of [4, 5, 7]) assert.deepEqual(m.get(y), [6], "y" + y);
  assert.deepEqual(m.get(6), [9], "the course beats the band");
  for (const y of [8, 9, 10]) assert.deepEqual(m.get(y), [1], "unbanded keeps the call's material y" + y);
  assert.deepEqual(m.get(11), [7], "top layer");
  assert.deepEqual(m.get(12), [8], "merlon row");
  assert.deepEqual(r.structureInfo.style, { bands: 2, trim: { top: 7, merlonCap: 8, course: { y: 6, material: 9 } } });
});

test("trim.topDepth thickens the top and a course takes a height", async () => {
  const { s, store } = session();
  await call(s, "build_tower", { ...TOWER, crenellations: false, trim: { top: 7, topDepth: 3, course: { y: 2, material: 9, height: 2 } } });
  const m = byY(cells(store));
  assert.deepEqual([m.get(1), m.get(2), m.get(3), m.get(4)], [[1], [9], [9], [1]]);
  assert.deepEqual([m.get(8), m.get(9), m.get(10), m.get(11)], [[1], [7], [7], [7]]);
  assert.equal(m.get(12), undefined);
});

test("build_keep: the walls, the roof row and the merlons", async () => {
  const { s, store } = session();
  await call(s, "build_keep", { at: [0, 0, 0], size: [12, 12], height: 8, material: 1, place: "new", bands: [{ toY: 2, material: 4 }], trim: { top: 7, merlonCap: 8 } });
  const m = byY(cells(store));
  assert.deepEqual([m.get(0), m.get(1)], [[4], [4]]);
  assert.deepEqual(m.get(4), [1]);
  assert.deepEqual(m.get(7), [7]);
  assert.deepEqual(m.get(8), [8]);
});

test("build_wall: bands follow a sloping base line", async () => {
  const { s, store } = session();
  await call(s, "build_wall", { points: [[0, 0, 0], [20, 10, 0]], height: 5, thickness: 2, material: 1, place: "new", crenellations: false, bands: [{ toY: 2, material: 4 }] });
  const c = cells(store);
  // the wall's bottom two rows are material 4 wherever the base is
  const col = (x: number) => [...c].filter(([k]) => k.startsWith(`${x},`)).map(([k, v]) => [Number(k.split(",")[1]), v] as const)
    .sort((p, q) => p[0] - q[0]);
  for (const x of [2, 16]) {
    const cl = col(x).filter((p, i, all) => all.findIndex((q) => q[0] === p[0]) === i);
    assert.deepEqual(cl.slice(0, 2).map((p) => p[1]), [4, 4], "x" + x);
    assert.ok(cl.slice(2).every((p) => p[1] === 1), "x" + x);
  }
  assert.ok(col(16)[0][0] > col(2)[0][0] + 5, "the base rises with the wall");
});

test("build_castle: the same bands on the walls, towers and keep, in voxels of scale", async () => {
  const { s, store } = session();
  const r = await call(s, "build_castle", { at: [0, 0, 0], size: [30, 30], wallHeight: 8, scale: 1.5, material: 1, place: "new",
    bands: [{ toY: 3, material: 4 }], trim: { course: { y: 6, material: 9 } } });
  const m = byY(cells(store));
  assert.equal(r.structureInfo.style.bands, 1);
  // rows are voxels of 1.5 map units: the lowest 2 (3 units) are band 4, one row (voxel 4, from 6 units up) is the course
  const rows = [...m.keys()].sort((a, b) => a - b), mats = rows.map((y) => m.get(y)!.join());
  assert.deepEqual(mats.slice(0, 2), ["4", "4"]);
  assert.equal(mats.filter((v) => v === "9").length, 1, "one course row");
  assert.equal(mats[4], "9");
  assert.ok(m.get(rows[rows.length - 1])!.every((v) => v === 1), "the rest is the call material");
});

test("build_castle: merlonCap with crenellations off is said, not silent", async () => {
  const { s } = session();
  const r = await call(s, "build_castle", { at: [0, 0, 0], size: [30, 30], material: 1, place: "new", crenellations: false, trim: { merlonCap: 3 }, dryRun: true });
  assert.ok(JSON.stringify(r).includes("no merlons to cap"));
});

test("build_compound: call-level bands and trim, an entry's own trim, a carve has none", async () => {
  const { s, store } = session();
  await call(s, "build_compound", { at: [0, 0, 0], place: "new", material: 1, bands: [{ toY: 2, material: 4 }], trim: { top: 7 }, shapes: [
    { kind: "box", size: [10, 10], height: 6 },
    { kind: "cylinder", at: [30, 0, 0], radius: 4, height: 6, trim: { course: { y: 3, material: 9 } } },
    { kind: "cylinder", at: [30, 0, 30], radius: 4, height: 6, bands: [{ material: 5 }] },
  ] });
  const c = cells(store);
  const at = (x: number, y: number, z: number) => c.get(`${x},${y},${z}`);
  assert.deepEqual([at(0, 0, 0), at(0, 1, 0), at(0, 2, 0), at(0, 5, 0)], [4, 4, 1, 7], "box: bands, then top");
  assert.deepEqual([at(30, 0, 0), at(30, 3, 0), at(30, 5, 0)], [4, 9, 1], "cylinder: its own trim replaces the call's (no top), bands stay");
  assert.deepEqual([at(30, 0, 30), at(30, 5, 30)], [5, 7], "an entry's own bands replace the call's, and its top trim still applies");
  assert.match(await err(s, "build_compound", { at: [0, 0, 0], shapes: [{ kind: "box", size: [4, 4], height: 4, carve: true, trim: { top: 1 } }] }), /carve/);
  assert.match(await err(s, "build_compound", { at: [0, 0, 0], trim: { merlonCap: 2 }, shapes: [{ kind: "box", size: [4, 4], height: 4 }] }), /merlonCap/);
});

test("bad bands and trim are refused", async () => {
  const { s } = session();
  assert.match(await err(s, "build_tower", { ...TOWER, bands: [{ fromY: 5, toY: 2, material: 3 }] }), /fromY must be under toY/);
  assert.match(await err(s, "build_tower", { ...TOWER, bands: [{ material: 99 }] }), /material index/);
  assert.match(await err(s, "build_tower", { ...TOWER, trim: { topDepth: 2 } }), /goes with trim.top/);
  assert.match(await err(s, "build_tower", { ...TOWER, trim: { course: { y: 1, material: -1 } } }), /material index/);
});

test("units world: heights are in world units", async () => {
  const { s, store } = session();
  await call(s, "build_tower", { ...TOWER, at: [0, 0, 0], radius: 80, height: 240, units: "world", crenellations: false, bands: [{ toY: 80, material: 5 }] });
  const m = byY(cells(store));
  assert.deepEqual([m.get(0), m.get(3), m.get(4)], [[5], [5], [1]]);
});
