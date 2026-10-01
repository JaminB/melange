import assert from "node:assert/strict";
import { readFileSync, writeFileSync } from "node:fs";
import { test } from "node:test";
import { CommandStack, apply, applyPatch, toPatch, validatePatch, validRunValue, type Frame, type Scene, type Vec3 } from "../../src/sdk/erg";
import {
  RemeshQueue, Sculptor, TerrainTool, anchorAt, carved, filled, gridFrames, index, invert, isSolid, material, painted, paletteFrom,
  pick, strokeChanges, validEdit, CORNER_BIT, maskOf, secondOf, withSecond, type Anchor, type Brush, type GridFrame, type TerrainHost,
} from "../../src/panels/erg/terrain";
import { shownOf } from "../../src/panels/erg/terrain/mesher";

const fixtureUrl = (name: string) => new URL(`../../../tests/fixtures/erg/${name}`, import.meta.url);
const scene12 = (): Scene => JSON.parse(readFileSync(fixtureUrl("synthetic-12.json"), "utf8"));

// The same words as tests/erg_voxels_selftest.cpp: solid columns of varying height, materials, some blend and second
// material bits, so carve, fill and paint all meet non-trivial words.
function word(fid: number, x: number, y: number, z: number, Y: number): number {
  const h = 1 + ((x * 7 + z * 13 + fid) % Y), m = (x * 3 + y + z * 5 + fid) % 64;
  let w = m << 2;
  if (y < h) w |= 3;
  if (((x ^ z) & 3) === 0) w |= ((x * 31 + z) & 0xff) << 16;
  if (y % 5 === 0) w |= (1 << 8) | (((m + 1) % 64) << 10);
  return w >>> 0;
}

function words(s: Scene): Map<number, Uint32Array> {
  const out = new Map<number, Uint32Array>();
  for (const f of s.frames) {
    if (f.voxels === null) continue;
    const [X, Y, Z] = f.size, w = new Uint32Array(X * Y * Z);
    for (let z = 0; z < Z; z++) for (let x = 0; x < X; x++) for (let y = 0; y < Y; y++) w[(z * X + x) * Y + y] = word(f.id, x, y, z, Y);
    out.set(f.voxels, w);
  }
  return out;
}

const refs = (s: Scene) => (id: number) => s.frames.find((f) => f.id === id)?.voxels ?? null;
const copy = (m: Map<number, Uint32Array>) => new Map([...m].map(([k, v]) => [k, v.slice()]));

function host(s: Scene, voxels = words(s)): TerrainHost & { remeshed: number[][] } {
  const remeshed: number[][] = [];
  return { scene: s, voxels, base: copy(voxels), refOf: refs(s), stack: new CommandStack(s), remesh: (ids) => remeshed.push(ids), remeshed };
}

// A scene of terrain frames laid on a grid: size [X, Y, Z] each, `rows` x `cols`, with optional rotation.
function gridScene(size: Vec3, rows: number, cols: number, rot: Vec3 = [0, 0, 0], fill?: (x: number, y: number, z: number) => number): Scene {
  const s = scene12();
  s.frames = [s.frames[0]];
  s.details = [];
  s.blobs = [];
  let id = 2, ref = 1;
  for (let r = 0; r < rows; r++)
    for (let c = 0; c < cols; c++) {
      // The grid is centred on the frame's position, so each one starts at (c*X, 0, r*Z).
      const pos: Vec3 = [(c + 0.5) * size[0], size[1] / 2, (r + 0.5) * size[2]];
      const f: Frame = { id: id++, parent: 1, name: `t${r}_${c}`, pos, rot, scale: [1, 1, 1], size,
        voxels: ref, heightMap: null, folder: false };
      s.blobs.push({ ref: ref++, kind: "voxels", frame: f.id, bytes: size[0] * size[1] * size[2] * 4 });
      s.frames.push(f);
    }
  if (fill) {
    const v = new Map<number, Uint32Array>();
    for (const f of s.frames.slice(1)) {
      const w = new Uint32Array(size[0] * size[1] * size[2]);
      for (let z = 0; z < size[2]; z++) for (let x = 0; x < size[0]; x++) for (let y = 0; y < size[1]; y++) w[index(f, x, y, z)] = fill(x, y, z);
      v.set(f.voxels!, w);
    }
    (s as unknown as { __voxels: Map<number, Uint32Array> }).__voxels = v;
  }
  return s;
}
const voxelsOf = (s: Scene) => (s as unknown as { __voxels: Map<number, Uint32Array> }).__voxels;

const at = (g: GridFrame, cell: Vec3): Anchor => ({ grid: g, center: [cell[0] + 0.5, cell[1] + 0.5, cell[2] + 0.5] });

test("voxel words follow the server's rules", () => {
  const v = 3 | (5 << 2) | (1 << 8) | (9 << 10) | (0x44 << 16);
  assert.equal(isSolid(v), true);
  assert.equal(material(v), 5);
  assert.equal(carved(v), v & ~3);
  assert.equal(filled(7), 3 | (7 << 2));
  assert.equal(painted(v, 12), (v & ~0xfc) | (12 << 2));
  assert.equal(painted(carved(v), 12), carved(v));
  const cases: [number, number, boolean][] = [
    [v, carved(v), true], [v, filled(9), true], [v, painted(v, 1), true], [v, carved(painted(v, 2)), true], [carved(v), v, true],
    [v, (3 | (0x44 << 16)) >>> 0, false], [0, 3 | (0x10 << 16), false], [v, 2, false], [v, 0x01000003, false], [0, 0, true],
  ];
  for (const [base, now, ok] of cases) assert.equal(validEdit(base, now), ok, `${base.toString(16)} -> ${now.toString(16)}`);
});

test("invert undoes a frame's world matrix", () => {
  const s = scene12(), grids = gridFrames(s, words(s), refs(s));
  assert.equal(grids.length, 12);
  for (const g of grids) {
    const p: Vec3 = [1.25, 2.5, -3], q = apply(g.toLocal, apply(g.toWorld, p));
    for (let a = 0; a < 3; a++) assert.ok(Math.abs(q[a] - p[a]) < 1e-9);
  }
  assert.equal(invert([0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3]), null);
});

test("pick finds the voxel under a ray, rotated frames included", () => {
  const solidBelow2 = (_x: number, y: number) => (y < 2 ? 3 | (4 << 2) : 0);
  const s = gridScene([4, 4, 4], 1, 1, [0, 0, 0], solidBelow2);
  s.frames[1].pos = [12, 2, 2];
  const grids = gridFrames(s, voxelsOf(s), refs(s));
  const hit = pick(grids, voxelsOf(s), [11.5, 10, 1.5], [0, -1, 0])!;
  assert.deepEqual(hit.cell, [1, 1, 1]);
  assert.deepEqual(hit.normal, [0, 1, 0]);
  assert.ok(Math.abs(hit.t - 8) < 1e-9);
  assert.equal(pick(grids, voxelsOf(s), [11.5, 10, 1.5], [0, 1, 0]), null);
  const side = pick(grids, voxelsOf(s), [0, 0.5, 2.5], [1, 0, 0])!;
  assert.deepEqual(side.cell, [0, 0, 2]);
  assert.deepEqual(side.normal, [-1, 0, 0]);

  const r = gridScene([4, 4, 4], 1, 1, [0.3, 1.1, -0.4], solidBelow2);
  const rg = gridFrames(r, voxelsOf(r), refs(r));
  for (const cell of [[0, 1, 0], [3, 1, 2], [2, 1, 3]] as Vec3[]) {
    const target = apply(rg[0].toWorld, [cell[0] + 0.5, cell[1] + 0.99, cell[2] + 0.5]);
    const up = apply(rg[0].toWorld, [cell[0] + 0.5, 9, cell[2] + 0.5]);
    const h = pick(rg, voxelsOf(r), up, [target[0] - up[0], target[1] - up[1], target[2] - up[2]])!;
    assert.deepEqual(h.cell, cell);
    assert.deepEqual(h.normal, [0, 1, 0]);
  }
});

test("a box carve clears exactly the solid voxels in the box and keeps their materials", () => {
  const s = scene12(), h = host(s), sc = new Sculptor(h);
  const g = sc.frames.find((x) => x.frame.size[0] >= 6 && x.frame.size[2] >= 6)!;
  const before = h.voxels.get(g.ref)!.slice();
  const brush: Brush = { mode: "carve", shape: "box", size: [3, 20, 3], material: 0 };
  const r = sc.step(at(g, [2, 0, 2]), brush);
  const after = h.voxels.get(g.ref)!, f = g.frame;
  let expect = 0;
  for (let z = 0; z < f.size[2]; z++)
    for (let x = 0; x < f.size[0]; x++)
      for (let y = 0; y < f.size[1]; y++) {
        const i = index(f, x, y, z), inBox = x >= 1 && x <= 3 && z >= 1 && z <= 3 && y <= 9;
        if (inBox && isSolid(before[i])) expect++;
        assert.equal(after[i], inBox && isSolid(before[i]) ? carved(before[i]) : before[i]);
      }
  assert.ok(expect > 0);
  assert.ok(r.frames.includes(f.id) && h.remeshed.flat().includes(f.id));
  assert.equal(r.changed >= expect, true);
});

test("fill writes the brush material with no blend; the column option copies the column's top material", () => {
  const s = gridScene([6, 8, 6], 1, 1, [0, 0, 0], (x, y, z) => (y < 1 + ((x + z) % 4) ? 3 | ((10 + x) << 2) | (0x22 << 16) : (5 << 2) | (0x33 << 16)));
  const h = host(s, voxelsOf(s)), sc = new Sculptor(h), g = sc.frames[0], f = g.frame;
  const base = h.base.get(g.ref)!;
  sc.step(at(g, [3, 4, 3]), { mode: "fill", shape: "box", size: [6, 8, 6], material: 7 });
  const w = h.voxels.get(g.ref)!;
  for (let i = 0; i < w.length; i++) {
    assert.ok(isSolid(w[i]));
    if (!isSolid(base[i])) assert.equal(w[i], filled(7));
    else assert.equal(w[i], base[i]);
  }
  h.stack.undo();
  assert.deepEqual([...h.voxels.get(g.ref)!], [...base]);
  sc.step(at(g, [3, 4, 3]), { mode: "fill", shape: "box", size: [6, 8, 6], material: "column" });
  for (let z = 0; z < 6; z++)
    for (let x = 0; x < 6; x++)
      for (let y = 0; y < 8; y++) {
        const i = index(f, x, y, z);
        if (!isSolid(base[i])) assert.equal(w[i], filled(10 + x));
      }
});

test("paint changes 100 solid voxels' material and nothing else", () => {
  const s = gridScene([10, 10, 10], 1, 1, [0, 0, 0], (x, y) => (y < 5 ? 3 | (2 << 2) | (1 << 8) | (4 << 10) | ((x * 9) << 16) : 0));
  const h = host(s, voxelsOf(s)), sc = new Sculptor(h), g = sc.frames[0];
  const r = sc.step(at(g, [5, 2, 5]), { mode: "paint", shape: "box", size: [10, 10, 10], material: 7 });
  assert.equal(r.changed, 500);
  h.stack.undo();
  const r2 = sc.step(at(g, [5, 4, 5]), { mode: "paint", shape: "box", size: [10, 1, 10], material: 7 });
  assert.equal(r2.changed, 100);
  const base = h.base.get(g.ref)!, w = h.voxels.get(g.ref)!;
  let n = 0;
  for (let i = 0; i < w.length; i++)
    if (w[i] !== base[i]) {
      n++;
      assert.equal(material(w[i]), 7);
      assert.equal(w[i] & ~0xfc, base[i] & ~0xfc);
    }
  assert.equal(n, 100);
});

test("a sphere reaches into neighbouring frames and every step of a drag is one undo step", () => {
  const s = gridScene([4, 6, 4], 2, 2, [0, 0, 0], (_x, y) => (y < 4 ? 3 | (1 << 2) : 0));
  const h = host(s, voxelsOf(s)), sc = new Sculptor(h), g = sc.frames[0];
  const brush: Brush = { mode: "carve", shape: "sphere", size: [5, 5, 5], material: 0 };
  sc.begin();
  const r1 = sc.step({ grid: g, center: [4, 3.5, 4] }, brush);
  assert.equal(r1.frames.length, 4);
  sc.step({ grid: g, center: [3, 3.5, 4] }, brush);
  sc.step({ grid: g, center: [2, 3.5, 4] }, brush);
  sc.end();
  assert.equal(h.stack.depth, 1);
  h.stack.undo();
  for (const [ref, b] of h.base) assert.deepEqual([...h.voxels.get(ref)!], [...b]);
  h.stack.redo();
  const redone = copy(h.voxels);
  sc.begin();
  sc.step({ grid: g, center: [1, 1.5, 1] }, { ...brush, mode: "fill", material: 3 });
  sc.end();
  assert.equal(h.stack.depth, 2);
  h.stack.undo();
  for (const [ref, v] of redone) assert.deepEqual([...h.voxels.get(ref)!], [...v]);
  assert.ok(h.remeshed.every((ids) => ids.every((id) => s.frames.some((f) => f.id === id))));
});

test("strokes diff into voxels ops the server rules accept, and the patch rebuilds the edit", () => {
  const s = scene12(), h = host(s), sc = new Sculptor(h);
  const brushes: Brush[] = [
    { mode: "carve", shape: "sphere", size: [3, 3, 3], material: 0 },
    { mode: "fill", shape: "box", size: [2, 3, 2], material: 7 },
    { mode: "paint", shape: "sphere", size: [4, 4, 4], material: 12 },
    { mode: "fill", shape: "box", size: [3, 3, 3], material: "column" },
    { mode: "carve", shape: "box", size: [12, 5, 12], material: 0 },
  ];
  sc.frames.forEach((g, k) => {
    const c = g.frame.size.map((v) => Math.floor(v / 2)) as Vec3;
    sc.begin();
    sc.step(at(g, c), brushes[k % brushes.length]);
    sc.end();
  });
  const p = toPatch(s, s, { base: h.base, edited: h.voxels });
  const v = validatePatch(p);
  assert.ok(v.ok, v.errors.join("; "));
  const vox = p.ops.filter((o) => o.op === "voxels");
  assert.ok(vox.length >= 10);
  const rebuilt = copy(h.base);
  applyPatch(s, p, rebuilt);
  for (const [ref, w] of h.voxels) {
    assert.deepEqual([...rebuilt.get(ref)!], [...w]);
    const b = h.base.get(ref)!;
    for (let i = 0; i < w.length; i++) assert.ok(validEdit(b[i], w[i]));
  }
  // The C++ self-test applies this patch with the server's rules to the same generated words.
  const text = JSON.stringify(p) + "\n";
  const file = fixtureUrl("synthetic-12-sculpt.ergpatch.json");
  if ((globalThis as { process?: { env: Record<string, string | undefined> } }).process?.env.ERG_WRITE_FIXTURES) writeFileSync(file, text);
  assert.equal(readFileSync(file, "utf8").replace(/\r\n/g, "\n"), text, "the sculpt fixture matches (ERG_WRITE_FIXTURES=1 rewrites it)");
});

test("a step that would pass 2000 runs in a frame is refused and changes nothing", () => {
  const s = gridScene([40, 4, 40], 1, 1, [0, 0, 0], (x, y, z) => 3 | (((x + y + z) % 64) << 2));
  const h = host(s, voxelsOf(s)), sc = new Sculptor(h), g = sc.frames[0];
  const r = sc.step(at(g, [20, 2, 20]), { mode: "carve", shape: "box", size: [32, 4, 32], material: 0 });
  assert.ok(r.refused && /runs/.test(r.refused), r.refused);
  assert.equal(h.stack.depth, 0);
  assert.deepEqual([...h.voxels.get(g.ref)!], [...h.base.get(g.ref)!]);
  const ok = sc.step(at(g, [20, 2, 20]), { mode: "carve", shape: "box", size: [8, 4, 8], material: 0 });
  assert.equal(ok.refused, undefined);
  assert.equal(ok.changed, 256);
});

test("a 20x20x20 brush over 30 frames steps well inside the remesh budget, and undo restores the words", () => {
  const s = gridScene([4, 20, 4], 5, 6, [0, 0.2, 0], (x, y, z) => (y < 10 + ((x + z) % 3) ? 3 | ((x + z) << 2) : 0));
  const h = host(s, voxelsOf(s)), sc = new Sculptor(h);
  const g = sc.frames[14];
  const brush: Brush = { mode: "carve", shape: "box", size: [20, 20, 20], material: 0 };
  sc.step(at(g, [2, 10, 2]), brush);
  h.stack.undo();
  const r = sc.step(at(g, [2, 10, 2]), brush);
  assert.ok(r.frames.length >= 25, `${r.frames.length} frames`);
  assert.ok(r.ms < 100, `${r.ms.toFixed(1)} ms`);
  h.stack.undo();
  for (const [ref, b] of h.base) assert.deepEqual([...h.voxels.get(ref)!], [...b]);
});

test("remeshes coalesce to the touched frames once per frame", () => {
  const runs: number[][] = [];
  let queued: (() => void) | undefined;
  const q = new RemeshQueue((ids) => runs.push(ids), (fn) => (queued = fn));
  q.add([5, 3]);
  q.add([3, 9]);
  assert.equal(runs.length, 0);
  queued!();
  assert.deepEqual(runs, [[3, 5, 9]]);
  q.add([]);
  q.flush();
  assert.equal(runs.length, 1);
});

test("the tool clamps the brush, resizes with [ and ], and steps only when the brush moves", () => {
  const s = gridScene([8, 6, 8], 1, 1, [0, 0, 0], (_x, y) => (y < 3 ? 3 | (6 << 2) : 0));
  const h = host(s, voxelsOf(s)), tool = new TerrainTool(new Sculptor(h));
  tool.setBrush({ size: [0, 99, 3], material: 70 });
  assert.deepEqual(tool.brush.size, [1, 32, 3]);
  assert.equal(tool.brush.material, 63);
  tool.setBrush({ mode: "paint", material: "column" });
  assert.equal(tool.brush.material, 0);
  assert.equal(tool.key({ key: "]" }), true);
  assert.deepEqual(tool.brush.size, [2, 32, 4]);
  assert.equal(tool.key({ key: "[", ctrlKey: true }), false);
  tool.setBrush({ mode: "carve", shape: "sphere", size: [3, 3, 3] });
  const preview = tool.hover([4.5, 10, 4.5], [0, -1, 0])!;
  assert.equal(preview.frame, s.frames[1].id);
  assert.ok(preview.box.max[1] - preview.box.min[1] > 2.9);
  const first = tool.down([4.5, 10, 4.5], [0, -1, 0])!;
  assert.ok(first.changed > 0);
  const again = tool.drag([4.5, 10, 4.5], [0, -1, 0]);
  assert.ok(again === null || again.changed >= 0);
  tool.up();
  assert.equal(h.stack.depth, 1);
  assert.equal(tool.drag([4.5, 10, 4.5], [0, -1, 0]), null);
});

test("fill anchors in front of the hit face", () => {
  const s = gridScene([4, 4, 4], 1, 1, [0, 0, 0], (_x, y) => (y < 2 ? 3 : 0));
  const grids = gridFrames(s, voxelsOf(s), refs(s));
  const hit = pick(grids, voxelsOf(s), [1.5, 10, 1.5], [0, -1, 0])!;
  assert.deepEqual(anchorAt(hit, "fill").center, [1.5, 2.5, 1.5]);
  assert.deepEqual(anchorAt(hit, "carve").center, [1.5, 1.5, 1.5]);
  const ch = strokeChanges(grids, voxelsOf(s), anchorAt(hit, "fill"), { mode: "fill", shape: "sphere", size: [1, 1, 1], material: 4 });
  assert.deepEqual([...ch.get(grids[0].ref)!.entries()], [[index(grids[0].frame, 1, 2, 1), [0, filled(4)]]]);
});

test("fill and paint touch only the frame under the cursor; carve still reaches every frame it overlaps", () => {
  const s = gridScene([4, 6, 4], 4, 4, [0, 0, 0], (_x, y) => (y < 3 ? 3 | (1 << 2) : 0));
  const h = host(s, voxelsOf(s)), sc = new Sculptor(h), g = sc.frames[0];
  assert.equal(sc.frames.length, 16);
  const big = { shape: "box" as const, size: [16, 6, 16] as Vec3 };
  const carveR = sc.step({ grid: g, center: [8, 3, 8] }, { ...big, mode: "carve", material: 0 });
  assert.equal(carveR.frames.length, 16);
  h.stack.undo();
  const fillR = sc.step({ grid: g, center: [8, 3, 8] }, { ...big, mode: "fill", material: 5 });
  assert.deepEqual(fillR.frames, [g.frame.id]);
  h.stack.undo();
  const paintR = sc.step({ grid: g, center: [8, 3, 8] }, { ...big, mode: "paint", material: 5 });
  assert.deepEqual(paintR.frames, [g.frame.id]);
});

test("the palette takes the atlas colours it can read", () => {
  const p = paletteFrom(["#112233", "bad", 5]);
  assert.equal(p.length, 64);
  assert.equal(p[0], "#112233");
  assert.ok(p[1].startsWith("hsl("));
});

test("second-material paint: flags 3, the material and mask 0xff inside; shared corners on the border; removal restores", () => {
  const floor = 3 | (4 << 2), vanilla = (floor | (3 << 8) | (4 << 10)) >>> 0;   // flags 3, second = primary, as 0x7777 floors
  const s = gridScene([6, 2, 6], 1, 1, [0, 0, 0], (x, y, z) => (y > 0 ? 0 : x === 5 && z === 5 ? vanilla : floor));
  const h = host(s, voxelsOf(s)), sc = new Sculptor(h), g = sc.frames[0];
  const w = () => h.voxels.get(g.ref)!;
  const at = (x: number, y: number, z: number) => w()[index(g.frame, x, y, z)];
  const brush: Brush = { mode: "second", shape: "box", size: [2, 1, 2], material: 40 };
  const r = sc.step({ grid: g, center: [3, 0.5, 3] }, brush);
  const bit = (cx: number, cy: number, cz: number) => 1 << CORNER_BIT[cy][cz][cx];
  for (const [x, z] of [[2, 2], [3, 2], [2, 3], [3, 3]]) assert.equal(at(x, 0, z), withSecond(floor, 40, 0xff));
  assert.equal(maskOf(at(1, 0, 2)), bit(1, 0, 0) | bit(1, 0, 1) | bit(1, 1, 0) | bit(1, 1, 1), "an edge neighbour gets the corners on the shared face");
  assert.equal(maskOf(at(1, 0, 1)), bit(1, 0, 1) | bit(1, 1, 1), "a diagonal neighbour gets the shared vertex only");
  assert.equal(maskOf(at(4, 0, 4)), bit(0, 0, 0) | bit(0, 1, 0));
  assert.equal(secondOf(at(1, 0, 2)), 40);
  assert.equal(at(0, 0, 0), floor, "voxels that touch nothing painted keep their word");
  assert.equal(at(2, 1, 2), 0, "empty voxels stay empty");
  assert.equal(r.changed, 4 + 12);
  assert.ok([...h.voxels.get(g.ref)!].every(validRunValue));

  sc.step({ grid: g, center: [5.5, 0.5, 5.5] }, { ...brush, size: [1, 1, 1] });
  assert.equal(at(5, 0, 5), withSecond(vanilla, 40, 0xff), "a vanilla floor takes the new second material");
  assert.equal(maskOf(at(4, 0, 4)), bit(0, 0, 0) | bit(0, 1, 0) | bit(1, 0, 1) | bit(1, 1, 1), "border corners add up");

  sc.step({ grid: g, center: [5.5, 0.5, 5.5] }, { ...brush, size: [1, 1, 1], material: "none" });
  assert.equal(at(5, 0, 5), vanilla, "removal restores the base's second material");
  const p = toPatch(s, s, { base: h.base, edited: h.voxels });
  assert.deepEqual(validatePatch(p).errors, []);
  assert.ok(p.ops.some((o) => o.op === "voxels" && o.runs.some((run) => run[2] === withSecond(floor, 40, 0xff))));

  const bits = CORNER_BIT.flat(2).sort((a, b) => a - b);
  assert.deepEqual(bits, [0, 1, 2, 3, 4, 5, 6, 7], "each corner has its own bit");
  assert.deepEqual([0, 1].flatMap((cz) => [0, 1].map((cx) => CORNER_BIT[1][cz][cx])).sort(), [0, 1, 2, 3], "the top corners are bits 0-3");
  assert.equal(shownOf(withSecond(floor, 40, 0xff)), 40, "the view draws a fully covered voxel in its second material");
  assert.equal(shownOf(withSecond(floor, 40, 0x0f)), 4);

  const tool = new TerrainTool(sc);
  tool.setBrush({ mode: "second", material: "none" });
  assert.equal(tool.brush.material, "none");
  tool.setBrush({ mode: "paint" });
  assert.equal(tool.brush.material, 0, "only second-material paint removes");
});
