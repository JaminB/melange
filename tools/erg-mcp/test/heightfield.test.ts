// The heightfield group's planner on the synthetic 12-frame level (its root renamed Scene so blocks can be added), bundled
// for Node by test.ps1 into test/out: the expression language, sampling, packing into blocks of at most 32 a side and 2000
// runs, stacking, the 64-block budget and partial placement, lattice-aligned frames as canvas, apply as one undo step.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, LIMITS, filled, isSolid, material, type Loaded, type Scene } from "../src/core";
import { compile, fbm, noise2 } from "../src/lib/heightfield-expr";
import { apply, plan, sampleField, type FieldArgs, type Plan } from "../src/lib/heightfield-plan";
import { grids } from "../src/lib/voxel";

const scene = (): Scene => JSON.parse(readFileSync(new URL("../../../../tests/fixtures/erg/synthetic-12.json", import.meta.url), "utf8"));

function loaded(edit?: (s: Scene) => void): Loaded {
  const s = scene();
  s.frames.find((f) => f.id === 1)!.name = "Scene";
  edit?.(s);
  const blobs = new Map<number, ArrayBuffer>();
  for (const [ref, buf] of syntheticBlobs(s) as Map<number, Uint8Array>) blobs.set(ref, buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength) as ArrayBuffer);
  return { scene: s, blobs };
}
const store = (edit?: (s: Scene) => void) => new EditorStore("t", loaded(edit), loaded(edit));

// land6 (frame 10) put on the lattice: corner (0, 9, -82), 10x10x12.
const aligned = (s: Scene) => {
  const f = s.frames.find((x) => x.id === 10)!;
  f.rot = [0, 0, 0];
  f.pos = [5, 14, -76];
};

const vars = { x: 3, z: -4, u: 0.25, v: 0.75, r: 5, theta: 0.5, W: 10, D: 20, R: 5, pi: Math.PI, e: Math.E };

/** Every solid cell of the added blocks and the edited frames that the base did not have, by lattice cell. */
function solidAdded(st: EditorStore): Map<string, number> {
  const out = new Map<string, number>();
  for (const g of grids(st)) {
    const [X, Y, Z] = g.size;
    const o = [g.toMap[3], g.toMap[7], g.toMap[11]].map(Math.round);
    for (let z = 0; z < Z; z++)
      for (let x = 0; x < X; x++)
        for (let y = 0; y < Y; y++) {
          const i = (z * X + x) * Y + y;
          if (isSolid(g.words[i]) && !isSolid(g.base[i])) {
            const k = `${o[0] + x},${o[1] + y},${o[2] + z}`;
            out.set(k, (out.get(k) ?? 0) + 1);
          }
        }
  }
  return out;
}

function blocksOk(p: Plan) {
  for (const b of p.kept) {
    for (const s of [b.x1 - b.x0, b.y1 - b.y0, b.z1 - b.z0]) assert.ok(s >= 1 && s <= 32, `side ${s}`);
    assert.ok(b.runs <= LIMITS.runsPerFrame, `${b.runs} runs`);
  }
}

test("expressions: precedence, functions, variables, errors", () => {
  assert.equal(compile("1 + 2 * 3")(vars), 7);
  assert.equal(compile("2 ^ 3 ^ 2")(vars), 512);
  assert.equal(compile("-2 ^ 2")(vars), -4);
  assert.equal(compile("2 ^ -1")(vars), 0.5);
  assert.equal(compile("(1 + 2) * 3")(vars), 9);
  assert.equal(compile("x + z * 2")(vars), -5);
  assert.equal(compile("min(4, x, 9) + max(1, 2)")(vars), 5);
  assert.equal(compile("clamp(r * 10, 0, 7) + abs(-1) + sqrt(16)")(vars), 12);
  assert.equal(compile("smoothstep(0, 10, r)")(vars), 0.5);
  assert.equal(compile("1 / 0")(vars), 0);
  assert.equal(compile("W*D + R")(vars), 205);
  assert.ok(Math.abs(compile("sin(pi/2) + cos(0) + exp(0) + 1e1")(vars) - 13) < 1e-12);
  for (const [src, msg] of [["", /empty/], ["1 +", /ends early/], ["foo(1)", /unknown function 'foo'/], ["y", /unknown name 'y'/],
    ["clamp(1, 2)", /clamp takes 3 arguments, not 2/], ["(1", /expected '\)'/], ["1 2", /unexpected '2'/], ["process.exit()", /unexpected '\.'|unknown name/],
    ["x; 1", /unexpected ';'/], ["`x`", /unexpected '`'/]] as const)
    assert.throws(() => compile(src), msg, src);
  assert.throws(() => compile("(".repeat(100) + "1" + ")".repeat(100)), /nests deeper/);
  // noise: deterministic, about -1..1, 0 on whole coordinates, seed matters
  assert.equal(noise2(3, 4, 1), 0);
  assert.equal(noise2(3.3, 4.7, 1), noise2(3.3, 4.7, 1));
  assert.notEqual(noise2(3.3, 4.7, 1), noise2(3.3, 4.7, 2));
  let lo = Infinity, hi = -Infinity;
  for (let i = 0; i < 5000; i++) {
    const n = noise2(i * 0.137, i * 0.291, 5);
    lo = Math.min(lo, n);
    hi = Math.max(hi, n);
  }
  assert.ok(lo > -1.5 && hi < 1.5 && hi - lo > 0.8, `${lo}..${hi}`);
  assert.ok(Math.abs(fbm(1.5, 2.5, 3, 4)) <= 1.5);
  assert.equal(compile("noise2(x, z, 1)")(vars), 0);
});

test("sampling: columns, floor, crater, grid, slope and materials", () => {
  const st = store();
  const flat = sampleField(st, { min: [0, 0], max: [4, 3], floor: 10, expr: "2.4" });
  assert.equal(flat.nx, 4);
  assert.equal(flat.nz, 3);
  assert.ok([...flat.lo].every((v) => v === 10) && [...flat.hi].every((v) => v === 12));
  assert.ok([...flat.slope].every((v) => v === 0));
  const w = sampleField(st, { units: "world", min: [0, 0], max: [80, 60], floor: 200, expr: "40", baseDepth: 20 });
  assert.equal(w.nx, 4);
  assert.ok([...w.lo].every((v) => v === 9) && [...w.hi].every((v) => v === 12));
  const cr = sampleField(st, { min: [-10, -10], max: [10, 10], floor: 0, expr: "10", crater: { radius: 5, depth: 8 } });
  const c = (x: number, z: number) => (z + 10) * cr.nx + (x + 10);
  assert.equal(cr.hi[c(0, 0)], 2);       // 10 - 8*(1 - small)
  assert.equal(cr.hi[c(-10, -10)], 10);
  assert.ok(cr.slope[c(3, 0)] > 0);
  const g = sampleField(st, { min: [0, 0], max: [3, 3], floor: 0, grid: [[0, 6], [6, 12]] });
  assert.ok(g.hi[0] < g.hi[8], "the far corner is higher");
  assert.throws(() => sampleField(st, { min: [0, 0], max: [3, 3], floor: 0, grid: [[1, 2], [3]] }), /same number/);
  assert.throws(() => sampleField(st, { min: [0, 0], max: [3, 3], floor: 0 }), /either expr or grid/);
  assert.throws(() => sampleField(st, { min: [0, 0], max: [3, 3], floor: 0, expr: "1", grid: [[1]] }), /either expr or grid/);
  assert.throws(() => sampleField(st, { min: [0, 0], max: [3, 3], floor: 0, expr: "1e6" }), /maxHeight/);
  assert.throws(() => sampleField(st, { min: [0, 0], max: [0.2, 3], floor: 0, expr: "1" }), /less than one voxel/);
  const m = sampleField(st, { min: [0, 0], max: [2, 1], floor: 0, expr: "6", material: 1,
    materials: [{ material: 9, surface: 1 }, { material: 5, minHeight: 2, maxHeight: 4 }] });
  assert.deepEqual([0, 1, 2, 3, 4, 5].map((y) => m.materialAt(0, y)), [1, 1, 5, 5, 1, 9]);
  const noScene = new EditorStore("t", loaded((s) => { s.frames[0].name = "root"; }), loaded((s) => { s.frames[0].name = "root"; }));
  assert.equal(sampleField(noScene, { min: [0, 0], max: [3, 3], floor: 0, expr: "1" }).sceneId, null);
  const np = plan(noScene, { min: [0, 0], max: [3, 3], floor: 0, expr: "1" });
  assert.equal(np.fits, false);
  assert.match(np.why ?? "", /no Scene frame/);
  assert.throws(() => apply(noScene, np), /no Scene frame/);
  const npp = plan(noScene, { min: [0, 0], max: [3, 3], floor: 0, expr: "1", partial: true });
  assert.ok(npp.fits, "partial: the frames' part only");
  assert.throws(() => apply(noScene, npp), /nothing could be placed/);
  assert.equal(noScene.stack.depth, 0);
});

test("a cone packs into blocks, applies as one undo step and covers exactly the field", () => {
  const st = store();
  const a: FieldArgs = { min: [100, 100], max: [160, 160], floor: 0, expr: "max(0, 40 - r*1.4)", material: 2,
    materials: [{ material: 7, surface: 1 }] };
  const p = plan(st, a);
  assert.ok(p.fits, p.why);
  blocksOk(p);
  assert.ok(p.kept.length > 4 && p.kept.length <= 64);
  assert.ok(p.kept.some((b) => b.level > 0), "the cone stacks blocks");
  const voxels = p.report.voxels as { field: number; inBlocks: number; inFrames: number; alreadySolid: number };
  assert.equal(voxels.inBlocks, p.voxels);
  const r = apply(st, p);
  assert.equal(r.blocks.length, p.kept.length);
  assert.equal(st.stack.depth, 1);
  assert.equal(st.stack.peek()?.label, "Heightfield");
  assert.deepEqual(st.problems(), []);
  const patch = st.patch();
  assert.equal(patch.format, "erg-patch/2");
  assert.equal(patch.ops.filter((o) => o.op === "addFrame").length, p.kept.length);
  for (const o of patch.ops) if (o.op === "voxels") assert.ok(o.runs.length <= LIMITS.runsPerFrame);
  // exactly the field's cells, once each, the top one painted 7
  const got = solidAdded(st);
  assert.equal(got.size, p.voxels);
  assert.ok([...got.values()].every((n) => n === 1), "no cell twice");
  const f = p.field;
  for (let z = 0; z < f.nz; z++)
    for (let x = 0; x < f.nx; x++) {
      const c = z * f.nx + x;
      for (let y = f.lo[c]; y < f.hi[c]; y++) assert.ok(got.has(`${f.i0 + x},${y},${f.j0 + z}`), `missing ${x},${y},${z}`);
    }
  const tops = grids(st).filter((g) => g.frame.new);
  assert.ok(tops.some((g) => g.words.some((w) => isSolid(w) && material(w) === 7)));
  st.undo();
  assert.equal(grids(st).filter((g) => g.frame.new).length, 0);
  assert.equal(st.dirty, false);
  st.redo();
  assert.equal(grids(st).filter((g) => g.frame.new).length, p.kept.length);
});

test("budget: 256x256 at 32 high takes all 64 blocks; more is refused, or partial", () => {
  const st = store();
  const full = plan(st, { min: [0, 0], max: [256, 256], floor: 0, expr: "32", material: 1 });
  assert.ok(full.fits, full.why);
  assert.equal(full.kept.length, 64);
  assert.equal(full.dropped.length, 0);
  assert.equal((full.report.budget as { blocks: { left: number } }).blocks.left, 0);
  const over = plan(st, { min: [0, 0], max: [260, 256], floor: 0, expr: "32", material: 1 });
  assert.equal(over.fits, false);
  assert.match(over.why ?? "", /needs 72 blocks and 64 may be added/);
  assert.throws(() => apply(st, over), /needs 72 blocks/);
  assert.equal(st.stack.depth, 0);
  const capped = plan(st, { min: [0, 0], max: [64, 64], floor: 0, expr: "32", maxBlocks: 2 });
  assert.equal(capped.fits, false);
  assert.match(capped.why ?? "", /maxBlocks 2/);
  // A tall volcano with too few blocks: partial keeps the biggest, nothing floats.
  const tall: FieldArgs = { min: [-64, -64], max: [64, 64], floor: 0, expr: "max(0, 100 - r*1.6)", maxBlocks: 20 };
  const no = plan(st, tall);
  assert.equal(no.fits, false);
  const yes = plan(st, { ...tall, partial: true });
  assert.ok(yes.fits);
  assert.equal(yes.kept.length, 20);
  assert.ok(yes.dropped.length > 0);
  for (const b of yes.kept)
    for (const under of yes.pieces.filter((q) => q.stack === b.stack && q.level === b.level - 1)) assert.ok(yes.kept.includes(under), "a kept slice stands on kept ones");
  const minKept = Math.min(...yes.kept.map((b) => b.voxels));
  assert.ok(yes.dropped.every((d) => d.voxels <= minKept || yes.pieces.some((q) => q.stack === d.stack && q.level === d.level - 1 && !yes.kept.includes(q))));
  apply(st, yes);
  assert.deepEqual(st.problems(), []);
  assert.equal(st.patch().ops.filter((o) => o.op === "addFrame").length, 20);
  const after = plan(st, { min: [300, 300], max: [340, 340], floor: 0, expr: "10" });
  assert.equal((after.report.budget as { blocks: { before: number } }).blocks.before, 20, "blocks already added count against the budget");
});

test("a block over 2000 runs is split", () => {
  const st = store();
  // Stripes: every column changes material every voxel, 32 runs per column.
  const p = plan(st, { min: [0, 0], max: [32, 32], floor: 0, expr: "32", material: 1,
    materials: Array.from({ length: 16 }, (_, i) => ({ material: 2 + (i % 2), minHeight: i * 2, maxHeight: i * 2 + 1 })) });
  assert.ok(p.fits, p.why);
  blocksOk(p);
  assert.ok(p.kept.length >= 16, `${p.kept.length} blocks`);
  apply(st, p);
  for (const o of st.patch().ops) if (o.op === "voxels") assert.ok(o.runs.length <= LIMITS.runsPerFrame);
  assert.equal(solidAdded(st).size, 32 * 32 * 32);
});

test("lattice-aligned frames are filled first, cells already solid count as done", () => {
  const st = store(aligned);
  const g = grids(st).find((x) => x.id === 10)!;
  const baseSolid = [...g.base].filter(isSolid).length;
  // The frame covers x 0..10, y 9..19, z -82..-70; the field x -2..14, z -84..-66, y 9..15.
  const p = plan(st, { min: [-2, -84], max: [14, -66], floor: 9, expr: "6", material: 4 });
  assert.ok(p.fits, p.why);
  const fr = (p.report.frames as { id: number; cellsFilled: number; covers: number; alreadySolid: number; approximate?: true }[]).find((x) => x.id === 10)!;
  assert.ok(fr && fr.covers > 0 && fr.alreadySolid > 0 && !fr.approximate, JSON.stringify(fr));
  assert.equal(fr.cellsFilled, fr.covers, "on the lattice a frame cell is a field cell");
  assert.equal(fr.covers + fr.alreadySolid, 10 * 6 * 12);
  const v = p.report.voxels as { field: number; inBlocks: number; inFrames: number; alreadySolid: number };
  assert.equal(v.inBlocks + v.inFrames + v.alreadySolid, v.field);
  apply(st, p);
  assert.deepEqual(st.problems(), []);
  const after = grids(st).find((x) => x.id === 10)!;
  assert.equal([...after.words].filter(isSolid).length, baseSolid + fr.covers);
  assert.ok(after.words.some((w, i) => w === filled(4) && !isSolid(after.base[i])));
  const got = solidAdded(st);
  assert.equal(got.size, v.inBlocks + v.inFrames);
  assert.ok([...got.values()].every((n) => n === 1), "no cell in a frame and a block both");
  assert.ok(st.patch().ops.some((o) => o.op === "voxels" && o.frame === 10));
  // Off: the frame is not touched.
  const st2 = store(aligned);
  const p2 = plan(st2, { min: [-2, -84], max: [14, -66], floor: 9, expr: "6", reuseFrames: "none" });
  assert.equal((p2.report.voxels as { inFrames: number }).inFrames, 0);
  // Rotated frames are skipped and say why.
  const st3 = store();
  const p3 = plan(st3, { min: [-10, -90], max: [20, -60], floor: 9, expr: "6" });
  assert.ok((p3.report.framesSkipped as { list: { id: number; why: string }[] }).list.some((s) => s.id === 10 && /rotated/.test(s.why)));
});

test("reuseFrames any: a rotated frame is filled where its cells' centres are in the field, the rest goes to blocks", () => {
  const st = store();
  const g = grids(st).find((x) => x.id === 10)!;   // land6, turned 0.994 rad about y
  const a: FieldArgs = { min: [-10, -90], max: [20, -60], floor: 9, expr: "6", material: 4, reuseFrames: "any" };
  const p = plan(st, a);
  assert.ok(p.fits, p.why);
  const fr = (p.report.frames as { id: number; cellsFilled: number; covers: number; alreadySolid: number; approximate?: true }[]).find((x) => x.id === 10)!;
  assert.ok(fr && fr.approximate && fr.cellsFilled > 0 && fr.covers > 0, JSON.stringify(fr));
  const v = p.report.voxels as { field: number; inBlocks: number; inFrames: number; alreadySolid: number };
  assert.equal(v.inBlocks + v.inFrames + v.alreadySolid, v.field);
  // every frame cell filled has its centre inside the field's volume
  const f = p.field;
  for (const [i] of p.canvas.fills.find((x) => x.grid.id === 10)!.changes) {
    const y = i % g.size[1], x = Math.floor(i / g.size[1]) % g.size[0], z = Math.floor(i / (g.size[1] * g.size[0]));
    const c = [0, 1, 2].map((r) => g.toMap[r * 4] * (x + 0.5) + g.toMap[r * 4 + 1] * (y + 0.5) + g.toMap[r * 4 + 2] * (z + 0.5) + g.toMap[r * 4 + 3]);
    const col = (Math.floor(c[2]) - f.j0) * f.nx + Math.floor(c[0]) - f.i0;
    assert.ok(Math.floor(c[1]) >= f.lo[col] && Math.floor(c[1]) < f.hi[col]);
  }
  apply(st, p);
  assert.deepEqual(st.problems(), []);
  // land0 (frame 4) is scaled 1.5: usable; a frame scaled under 0.9 is not.
  const small = store((s) => { s.frames.find((x) => x.id === 10)!.scale = [0.5, 0.5, 0.5]; });
  const ps = plan(small, a);
  assert.ok((ps.report.framesSkipped as { list: { id: number; why: string }[] }).list.some((s) => s.id === 10 && /scaled down/.test(s.why)));
});
