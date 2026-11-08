// The Caldera benchmark's fixes on the synthetic 12-frame level (its root renamed Scene so blocks can be added), bundled
// for Node by test.ps1: the heightfield's cell-budget answer and its multi-resolution areas, scaled and part-packed
// structures with castle spawn spots, sculpt_crater, exact tops of scaled blocks, render_side without block seams,
// the walk graph of check_reachability, and the scenery tools.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { inflateSync } from "node:zlib";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, isSolid, material, voxelCells, type Loaded, type Scene } from "../src/core";
import { apply, plan, type FieldArgs } from "../src/lib/heightfield-plan";
import { columnSpans, spansAt } from "../src/lib/inspect-field";
import { search, standing, surfaces, type ReachOptions } from "../src/lib/reach";
import { grids } from "../src/lib/voxel";
import { tools as structureTools } from "../src/tools/structures";
import { tools as inspectTools } from "../src/tools/inspect";
import { tools as sceneryTools } from "../src/tools/scenery";
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
  // no server: the material colours fall back to index hues
  const s = { requireProject: () => ({ store, id: "t", info: {} }), get store() { return store; }, project: { store },
    levels: async () => { throw new Error("no server"); }, client: async () => { throw new Error("no server"); } } as unknown as Session;
  return { store, s };
}
const all = [...structureTools, ...inspectTools, ...sceneryTools];
const call = async (s: Session, name: string, args: Record<string, unknown>): Promise<any> => all.find((t) => t.name === name)!.handler(s, args);
const cells = (store: EditorStore) => voxelCells(store.scene);

test("heightfield: a cell-bound field says 'cell budget exceeded (N needed, M allowed)' and the voxelSize that fits", () => {
  const { store } = session();
  const field: FieldArgs = { min: [-125, -125], max: [125, 125], floor: 0, expr: "max(0, 60*(1 - r/110))", material: 2, maxCells: 30000 };
  const p = plan(store, field);
  assert.equal(p.fits, false);
  const needed = (p.report.budget as { cells: { needed: number } }).cells.needed;
  assert.match(p.why ?? "", new RegExp(`^cell budget exceeded \\(${needed} needed, 30000 allowed\\)`), p.why);
  assert.doesNotMatch(p.why ?? "", /^the field needs/);
  const sug = p.report.suggestedVoxelSize as { voxelSize: number; cells: number; blocks: number };
  assert.ok(sug && sug.voxelSize > 1 && sug.voxelSize <= 4, JSON.stringify(sug));
  assert.match(p.why ?? "", new RegExp(`At voxelSize ${sug.voxelSize} the same field fits whole`));
  const at = plan(store, { ...field, voxelSize: sug.voxelSize });
  assert.ok(at.fits && !at.dropped.length, at.why);
  assert.ok(sug.cells <= 30000);
  // one size smaller does not fit: the suggestion is the smallest
  if (sug.voxelSize - 0.25 >= 1) assert.ok(plan(store, { ...field, voxelSize: sug.voxelSize - 0.25 }).dropped.length > 0);
  // a block-bound field still names the blocks, with no suggestion
  const flat = plan(store, { min: [0, 0], max: [252, 240], floor: 0, expr: "1", material: 1 });
  assert.match(flat.why ?? "", /^the field needs 72 blocks and 64 may be added/);
  assert.equal(flat.report.suggestedVoxelSize, undefined);
});

test("heightfield: centre, clip and exclude make a fine field inside a coarse one, overlapping at the edge with no gap", () => {
  const { store } = session();
  const base: FieldArgs = { min: [-60, -60], max: [60, 60], floor: 0, expr: "max(0, 30 - r*0.5)", centre: [0, 0], material: 1 };
  const coarse = { ...base, voxelSize: 3, exclude: [{ x: 0, z: 0, radius: 20 }] };
  const fine = { ...base, voxelSize: 1.5, clip: [{ x: 0, z: 0, radius: 20 }] };
  const pc = plan(store, coarse), pf = plan(store, fine);
  assert.ok(pc.fits && pf.fits, `${pc.why} ${pf.why}`);
  // the coarse field has no column wholly inside the circle, the fine one none whose centre is outside it
  const f = pc.field;
  for (let b = 0; b < f.nz; b++) for (let a = 0; a < f.nx; a++) {
    const c = b * f.nx + a, x0 = (f.i0 + a) * f.s, z0 = (f.j0 + b) * f.s;
    const corners = [[x0, z0], [x0 + f.s, z0], [x0, z0 + f.s], [x0 + f.s, z0 + f.s]].every(([x, z]) => Math.hypot(x, z) <= 20);
    if (corners) assert.ok(f.hi[c] <= f.lo[c], "excluded");
  }
  const g = pf.field;
  for (let b = 0; b < g.nz; b++) for (let a = 0; a < g.nx; a++) {
    const c = b * g.nx + a, x = (g.i0 + a + 0.5) * g.s, z = (g.j0 + b + 0.5) * g.s;
    if (Math.hypot(x, z) > 20) assert.ok(g.hi[c] <= g.lo[c], "clipped");
  }
  apply(store, pc);
  apply(store, plan(store, fine));
  // every column of the circle's edge has ground, and the fine and coarse tops agree to a coarse voxel
  const gs = grids(store);
  for (let t = 0; t < 64; t++) for (const r of [17, 19.5, 21, 23]) {
    const x = r * Math.cos((t / 64) * 2 * Math.PI), z = r * Math.sin((t / 64) * 2 * Math.PI);
    const top = spansAt(gs, x, z)[0];
    assert.ok(top, `ground at ${x}, ${z}`);
    assert.ok(Math.abs(top.top - (30 - r * 0.5)) <= 3.01, `top ${top.top} at r ${r}`);
  }
  assert.throws(() => plan(store, { ...base, clip: [{ x: 0, radius: 5 }] }), /either x, z and radius/);
});

test("query_ground reads scaled blocks cell by cell: exact tops", () => {
  const { store } = session();
  apply(store, plan(store, { min: [0, 0], max: [30, 30], floor: 0, expr: "5 + x*0.3", voxelSize: 2.5, material: 3 }));
  const g = grids(store).find((x) => x.frame.new)!;
  for (let k = 0; k < 40; k++) {
    const px = g.bounds.min[0] + 0.13 + k * 0.7, pz = g.bounds.min[2] + 1.1;
    if (px >= g.bounds.max[0]) break;
    const sp = columnSpans(g, px, pz);
    const cx = Math.floor((px - g.toMap[3]) / 2.5), cz = Math.floor((pz - g.toMap[11]) / 2.5);
    let top = -1;
    for (let y = g.size[1] - 1; y >= 0; y--) if (isSolid(g.words[(cz * g.size[0] + cx) * g.size[1] + y])) { top = y; break; }
    assert.equal(sp[0].top, g.toMap[7] + (top + 1) * 2.5, `x ${px}`);
    assert.equal(sp[0].material, material(g.words[(cz * g.size[0] + cx) * g.size[1] + top]));
  }
});

test("structures: blockScale, castle scale, packing by parts, spawn spots", async () => {
  const at = [600, 10, 600];
  const tiles = session(), parts = session(), big = session();
  const castle = { at, size: [24, 24], wallHeight: 5, wallThickness: 2, towerRadius: 2.5, towerHeight: 8, keep: true, keepSize: [8, 8], material: 4, place: "new" };
  await call(tiles.s, "build_castle", castle);
  const r = await call(parts.s, "build_castle", { ...castle, packing: "parts" });
  const cT = cells(tiles.store) - voxelCells(loaded().scene), cP = cells(parts.store) - voxelCells(loaded().scene);
  assert.ok(cP < cT * 0.6, `parts ${cP} cells, tiles ${cT}`);
  assert.ok(r.newBlocks.length > 1);
  // the same voxels either way
  const solid = (st: EditorStore) => { let n = 0; for (const g of grids(st)) if (g.frame.new) for (const w of g.words) if (isSolid(w)) n++; return n; };
  assert.equal(solid(parts.store), solid(tiles.store));
  assert.deepEqual(parts.store.problems(), []);
  // spawn spots: in the courtyard (not inside a wall), on the wall-walks and tower tops, checked against the voxels
  const spots = r.spawnSpots.spots as { kind: string; at: number[]; ok: boolean; standOn: number[] }[];
  assert.ok(spots.filter((x) => x.kind === "courtyard").length >= 2);
  for (const sp of spots.filter((x) => x.kind === "rampart" || x.kind === "tower")) assert.ok(sp.ok, JSON.stringify(sp));
  for (const sp of spots.filter((x) => x.kind === "courtyard")) {
    const inner = 12 - 2;
    assert.ok(Math.abs(sp.at[0] - at[0]) < inner && Math.abs(sp.at[2] - at[2]) < inner && !(Math.abs(sp.at[0] - at[0]) < 4 && Math.abs(sp.at[2] - at[2]) < 4), JSON.stringify(sp));
  }
  // scale 2: the same castle in 2-unit voxels, about an eighth of the cells, blocks of scale 2
  const r2 = await call(big.s, "build_castle", { ...castle, scale: 2 });
  const cB = cells(big.store) - voxelCells(loaded().scene);
  assert.ok(cB < cT / 4, `scale 2: ${cB} cells, scale 1: ${cT}`);
  assert.ok(big.store.scene.frames.filter((f) => f.new).every((f) => f.scale.every((v) => v === 2)));
  assert.deepEqual(r2.structureInfo.size, [24, 24]);
  assert.equal(r2.structureInfo.scale, 2);
  // blockScale on another structure
  const w = session();
  await call(w.s, "build_wall", { points: [[700, 0, 700], [740, 0, 700]], height: 6, thickness: 2, blockScale: 2, place: "new", material: 2 });
  assert.ok(w.store.scene.frames.filter((f) => f.new).every((f) => f.scale[0] === 2));
  await assert.rejects(call(w.s, "build_wall", { points: [[700, 0, 750], [740, 0, 750]], height: 6, blockScale: 5 }), /blockScale/);
});

test("sculpt_crater: a crest that blends into the cone, a bowl below it, bands by distance from the crest", async () => {
  const { store, s } = session();
  const hf = plan(store, { min: [-30, -30], max: [30, 30], floor: 0, expr: "max(0, 20 - r*0.8)", material: 1 });
  assert.ok(hf.fits, hf.why);
  apply(store, hf);
  const gs0 = grids(store);
  const before = (x: number, z: number) => spansAt(gs0, x, z)[0]?.top ?? -Infinity;
  const ground10 = before(10, 0), ground16 = before(16, 0);
  const r = await call(s, "sculpt_crater", { at: [0, 0], radius: 8, depth: 6, rimHeight: 2, rimWidth: 6, floorRadius: 3,
    bands: [{ material: 9, to: -4 }, { material: 7, from: -4, to: 2 }], material: 1 });
  const crest = r.structureInfo.crestY as number, floorY = r.structureInfo.floorY as number;
  assert.ok(Math.abs(crest - (14 + 2)) < 1.01, `crest ${crest}`);
  assert.ok(Math.abs(floorY - (crest - 6)) < 1e-6);
  const gs = grids(store), top = (x: number, z: number) => spansAt(gs, x, z)[0];
  // the floor is flat near the middle, painted with the floor band; the wall rises to the crest
  for (const [x, z] of [[0, 0], [2, 1], [-1, -2]]) {
    assert.ok(Math.abs(top(x, z).top - floorY) <= 1, `floor at ${x}, ${z}: ${top(x, z).top} (floor ${floorY})`);
    assert.equal(top(x, z).material, 9);
  }
  assert.ok(top(7, 0).top > top(4, 0).top, "the bowl rises toward the crest");
  assert.equal(top(7.5, 0).material, 7, "the wall band");
  // outside: the rim falls back to the cone; past the blend the cone is untouched
  assert.ok(top(10, 0).top > ground10, "the rim is raised");
  assert.equal(top(16, 0).top, ground16);
  assert.deepEqual(store.problems(), []);
  assert.ok(store.undo());
  assert.equal(grids(store).length, gs0.length);
  await assert.rejects(call(s, "sculpt_crater", { at: [500, 500], radius: 5, depth: 3 }), /ground under only/);
});

test("render_side draws no seam where a block stands on the block under it", async () => {
  const { store, s } = session();
  // two stacked blocks of one material: one column of ground 0..40
  await call(s, "build_wall", { points: [[300, 0, 300], [306, 0, 300]], height: 40, thickness: 4, material: 6, place: "new", crenellations: false });
  assert.ok(store.scene.frames.filter((f) => f.new).length >= 2, "stacked blocks");
  const out = await call(s, "render_side", { x0: 299, z0: 300, x1: 307, z1: 300, scale: 4 });
  const png = Buffer.from(out.content[0].data, "base64");
  // decode: IHDR width/height, then the filtered rows
  const w = png.readUInt32BE(16), h = png.readUInt32BE(20);
  let idat = Buffer.alloc(0);
  for (let o = 8; o < png.length;) {
    const len = png.readUInt32BE(o), type = png.toString("latin1", o + 4, o + 8);
    if (type === "IDAT") idat = Buffer.concat([idat, png.subarray(o + 8, o + 8 + len)]);
    o += 12 + len;
  }
  const raw = inflateSync(idat), px = (x: number, y: number) => raw.subarray(y * (w * 3 + 1) + 1 + x * 3, y * (w * 3 + 1) + 4 + x * 3);
  const info = JSON.parse(out.content[1].text);
  // a column in the middle of the wall: below its top edge, one colour all the way down (no light line at y 31)
  const col = info.image.leftMargin + Math.floor(info.image.pixelsPerSample * 8), yTop = info.image.topMargin + 1;
  const colours = new Set<string>();
  for (let y = yTop + 2; y < h - 2; y++) colours.add(px(col, y).join(","));
  assert.equal(colours.size, 1, [...colours].join(" | "));
});

test("check_reachability: a voxel staircase cone is walked, a cliff stops it and says where", async () => {
  const { store, s } = session();
  apply(store, plan(store, { min: [-40, -40], max: [40, 40], floor: 0, expr: "max(0, 24 - r*0.6)", voxelSize: 2, material: 1 }));
  const o: ReachOptions = { step: 1, maxStep: 0.5, maxSlope: 45, maxDrop: 8, maxGap: 0, headroom: 1.5, water: null };
  const S = surfaces(grids(store), { x0: -40, z0: -40, x1: 40, z1: 40 }, 1, 1.5);
  const q = search(S, standing(S, [-35, 3, 0], o), o);
  let best = -Infinity;
  for (let n = 0; n < S.nodes; n++) if (q.reached[n]) best = Math.max(best, S.y[n]);
  assert.ok(best >= 22, `reached ${best}`);
  // the tool: a spawn on the foot reaches the summit
  const r = JSON.parse(await call(s, "check_reachability", { from: [[-35, 3, 0]], x0: -40, z0: -40, x1: 40, z1: 40 }));
  assert.equal(r.spawns[0].reachable, true, JSON.stringify(r.spawns[0]));
  assert.match(r.summary, /1 of 1/);
  // a wall 6 high round the summit stops it; the answer says where and how high the climb is
  await call(s, "build_wall", { points: [[-8, 14, -8], [8, 14, -8], [8, 14, 8], [-8, 14, 8]], closed: true, height: 10, thickness: 1, material: 2, place: "new" });
  const b = JSON.parse(await call(s, "check_reachability", { from: [[-35, 3, 0]], x0: -40, z0: -40, x1: 40, z1: 40, target: { x: 0, z: 0, radius: 2 } }));
  assert.equal(b.spawns[0].reachable, false, JSON.stringify(b.spawns[0]));
  assert.ok(b.spawns[0].blockedAt.length >= 1 && b.spawns[0].blockedAt[0].climbNeeded >= 3, JSON.stringify(b.spawns[0].blockedAt));
  // underwater ground is not walked
  store.scene.water.level = 10 * 20;
  const wet = JSON.parse(await call(s, "check_reachability", { from: [[-35, 3, 0]], x0: -40, z0: -40, x1: 40, z1: 40 }));
  assert.equal(wet.spawns[0].reachable, false);
  assert.match(wet.spawns[0].why, /water/);
});

test("scenery: list near a point, remove by filter as one undo step", async () => {
  const { store, s } = session();
  const details = store.scene.details.filter((d) => d.role !== "spawn" && d.role !== "object");
  const r = await call(s, "list_scenery", { units: "map" });
  assert.equal(r.total, details.length);
  if (!details.length) return;
  const d = details[0], at = store.frames.detailWorld(d);
  const near = await call(s, "list_scenery", { units: "map", near: { x: at[0], z: at[2], radius: 0.5 } });
  assert.ok(near.details.some((x: { id: number }) => x.id === d.id));
  const dry = await call(s, "remove_scenery", { ids: [d.id], dryRun: true });
  assert.equal(dry.wouldRemove, 1);
  assert.ok(store.detail(d.id));
  await call(s, "remove_scenery", { ids: [d.id] });
  assert.equal(store.detail(d.id), undefined);
  assert.ok(store.undo());
  assert.ok(store.detail(d.id));
  await assert.rejects(call(s, "remove_scenery", {}), /name what to remove/);
});
