// Dustcrown round 2's builder fixes on the synthetic 12-frame level: landBounds without empty frames, paint depth from the
// voxel's top and the reason when nothing is painted, render_side lines, tower roofs and structureInfo.towers, small mesas
// and mesa.ledges, the ship range from the features used, onSkin blocks and the pokes advisory, the sparse parts note,
// check_pits, check_reachability's traps and centred openings, and fillBasins. Bundled for Node by test.ps1.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, isSolid, material, type Loaded, type Scene } from "../src/core";
import { GROUND, OUTLET, WALL, basins, fillBasins, spillLevels } from "../src/lib/basins";
import { drapePokes, onSkinBlocks } from "../src/lib/drape-pokes";
import { landBounds } from "../src/lib/height-line";
import { planDrape, applyDrape } from "../src/lib/heightfield-drape";
import { decodePng } from "../src/lib/inspect-png";
import { findTraps, movesFrom } from "../src/lib/pits";
import { search, surfaces, type ReachOptions } from "../src/lib/reach";
import { SPAWN_AREAS_MELANGE, rangeFloor, shipRange } from "../src/lib/ship-gate";
import { shipFeatures } from "../src/lib/ship-gate-export";
import { VoxelEdit, grids } from "../src/lib/voxel";
import { tools as inspectTools, checkPits, pitWarnings } from "../src/tools/inspect";
import { tools as mesaTools } from "../src/tools/mesa";
import { describe } from "../src/tools/project";
import { tools as structureTools } from "../src/tools/structures";
import { tools as terrainTools } from "../src/tools/terrain";
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
  const s = { requireProject: () => ({ store, id: "t", info: { id: "t" } }), get store() { return store; }, project: { store }, palette: async () => null } as unknown as Session;
  return { store, s };
}
const all = [...inspectTools, ...mesaTools, ...structureTools, ...terrainTools];
const call = async (s: Session, name: string, a: Record<string, unknown>): Promise<any> => {
  const r: any = await all.find((t) => t.name === name)!.handler(s, a);
  if (r && Array.isArray(r.content)) {
    const text = r.content.find((c: any) => c.type === "text");
    return { ...JSON.parse(text.text), _image: r.content.find((c: any) => c.type === "image") };
  }
  return typeof r === "string" ? JSON.parse(r) : r;
};
/** The solid voxels of the added blocks by map cell "x,y,z" (blocks of scale 1) with their material. */
function cells(store: EditorStore): Map<string, number> {
  const out = new Map<string, number>();
  for (const g of grids(store)) {
    if (!g.frame.new) continue;
    const m = g.toMap;
    for (let z = 0; z < g.size[2]; z++) for (let x = 0; x < g.size[0]; x++) for (let y = 0; y < g.size[1]; y++) {
      const w = g.words[(z * g.size[0] + x) * g.size[1] + y];
      if (isSolid(w)) out.set(`${Math.floor(m[3] + x * m[0] + 1e-6)},${Math.floor(m[7] + y * m[5] + 1e-6)},${Math.floor(m[11] + z * m[10] + 1e-6)}`, material(w));
    }
  }
  return out;
}
/** A flat slab [x0, x1) x [z0, z1), `h` thick from y 0, far from the synthetic level's own frames. */
const slab = (s: Session, x0: number, x1: number, z0: number, z1: number, h: number, extra: Record<string, unknown> = {}) =>
  call(s, "build_compound", { at: [(x0 + x1) / 2, 0, (z0 + z1) / 2], place: "new", material: 3, shapes: [{ kind: "box", size: [x1 - x0, z1 - z0], height: h }], ...extra });

// ------------------------------------------------------------------ 7: landBounds

test("landBounds leaves out voxel frames with no solid voxel and counts them", () => {
  const { store, s } = session();
  const gs = grids(store), before = landBounds(gs)!;
  // empty the frame that sets the top end, as remove_base_frames leaves one
  const top = gs.find((g) => g.id === before.maxFrame)!;
  const e = new VoxelEdit(store);
  for (let z = 0; z < top.size[2]; z++) for (let x = 0; x < top.size[0]; x++) for (let y = 0; y < top.size[1]; y++) e.set(top.id, x, y, z, 0, false);
  e.commit("empty");
  const after = landBounds(grids(store))!;
  assert.equal(after.emptySkipped, before.emptySkipped + 1);
  assert.notEqual(after.maxFrame, before.maxFrame);
  assert.ok(after.maxY <= before.maxY);
  const d = describe(s, {}) as any;
  assert.equal(d.landBounds.emptyFramesSkipped, after.emptySkipped);
  assert.equal(d.landBounds.maxFrame.id, after.maxFrame);
  // a grid given without words (the old call shape) still counts
  assert.equal(landBounds(gs.map((g) => ({ id: g.id, frame: g.frame, bounds: g.bounds })))!.emptySkipped, 0);
});

// ------------------------------------------------------------------ 8: paint depth

test("paint_region: depth from the voxel's centre refuses a tall voxel with the reason; depthFrom top paints it", async () => {
  const { store, s } = session();
  await slab(s, 600, 618, 0, 18, 6, { scale: 3 });
  const args = { units: "map", material: 9, shapes: [{ min: [600, 0], max: [618, 18] }], depth: 1 };
  await assert.rejects(() => call(s, "paint_region", args), /nothing painted: 36 top voxels in the area are up to 3 map units tall.*centre is 1.5 under.*depth >= 1.5.*depthFrom: "top"/s);
  const r = await call(s, "paint_region", { ...args, depthFrom: "top" });
  assert.equal(r.changed, 36);
  const g = grids(store).find((x) => x.frame.new)!;
  let top = 0, under = 0;
  for (let z = 0; z < g.size[2]; z++) for (let x = 0; x < g.size[0]; x++) {
    if (material(g.words[(z * g.size[0] + x) * g.size[1] + 1]) === 9) top++;
    if (material(g.words[(z * g.size[0] + x) * g.size[1]]) === 9) under++;
  }
  assert.deepEqual([top, under], [36, 0]);
});

test("paint_region: the centre rule is unchanged, and a partial skip is reported", async () => {
  const { store, s } = session();
  await slab(s, 600, 606, 0, 6, 4);                              // unscaled: 4 voxels deep
  await slab(s, 620, 626, 0, 6, 6, { scale: 3 });                // scale 3
  const r = await call(s, "paint_region", { units: "map", material: 9, shapes: [{ min: [600, 0], max: [626, 6] }], depth: 1 });
  assert.equal(r.changed, 36);                                   // the unscaled slab's top voxels only, as before
  assert.equal(r.skippedByDepth.topVoxels, 4);
  assert.match(r.skippedByDepth.why, /depthFrom: "top"/);
  const c = cells(store);
  assert.equal(c.get("600,3,0"), 9);
  assert.equal(c.get("600,2,0"), 3);
});

// ------------------------------------------------------------------ 9: render_side lines

test("render_side draws height lines and answers their rows", async () => {
  const { s } = session();
  await slab(s, 600, 640, 0, 6, 4);
  const r = await call(s, "render_side", { x0: 600, z0: 3, x1: 640, z1: 3, y0: 0, y1: 20, scale: 6, units: "map",
    lines: [{ y: 14.5, colour: "#dc281e", label: "sand line" }, { y: 9, colour: [10, 200, 30], dash: false }, { y: 99 }] });
  assert.equal(r.lines.length, 3);
  assert.equal(r.lines[0].row, r.image.topMargin + Math.round((20 - 14.5) * 6));
  assert.equal(r.lines[2].shown, false);
  const png = decodePng(Buffer.from(r._image.data, "base64"));
  const at = (x: number, y: number) => [...png.rgb.subarray((y * png.w + x) * 3, (y * png.w + x) * 3 + 3)];
  assert.deepEqual(at(r.image.leftMargin + 2, r.lines[0].row), [220, 40, 30]);          // dash on
  assert.notDeepEqual(at(r.image.leftMargin + 10, r.lines[0].row), [220, 40, 30]);      // dash off
  assert.deepEqual(at(r.image.leftMargin + 10, r.lines[1].row), [10, 200, 30]);         // solid
  await assert.rejects(() => call(s, "render_side", { x0: 600, z0: 3, x1: 640, z1: 3, lines: [{ y: 1, colour: "red" }] }), /#rrggbb/);
  const plain = await call(s, "render_side", { x0: 600, z0: 3, x1: 640, z1: 3, y0: 0, y1: 20, scale: 6, units: "map" });
  assert.equal(plain.lines, undefined);
});

// ------------------------------------------------------------------ 10: roofs

test("build_tower roof: a dome, an onion and a cone stand on the top floor; structureInfo.towers gives the top", async () => {
  for (const [style, tipH] of [["dome", 3], ["onion", 4.8], ["cone", 4.5]] as const) {
    const { store, s } = session();
    const r = await call(s, "build_tower", { at: [600, 0, 0], radius: 4, height: 6, place: "new", material: 3, roof: { style, material: 11, tipMaterial: 12 } });
    const tw = r.structureInfo.towers[0];
    assert.deepEqual(tw.at, [600, 6, 0]);
    assert.equal(tw.topY, 6);
    assert.equal(tw.roof.radius, 3);                               // the tower's 4 less the merlon ring
    assert.equal(tw.roof.tipY, 6 + tipH);
    const c = cells(store);
    assert.equal(c.get("600,6,0"), 11, `${style}: the roof stands on the top floor`);
    assert.equal(c.get("600,5,0"), 3);
    const ys = [...c.keys()].map((k) => Number(k.split(",")[1]));
    assert.equal(Math.max(...ys), Math.ceil(6 + tipH - 0.5) - 1, `${style}: reaches its height (the last voxel whose centre is under the tip)`);
    assert.equal(c.get(`600,${Math.max(...ys)},0`), 12, `${style}: the tip`);
    // narrower towards the top
    const width = (y: number) => [...c.keys()].filter((k) => Number(k.split(",")[1]) === y && c.get(k) !== 3).length;
    assert.ok(width(6) > width(Math.max(...ys)), style);
  }
});

test("build_tower without a roof is unchanged; a bad roof is refused", async () => {
  const a = session(), b = session();
  await call(a.s, "build_tower", { at: [600, 0, 0], radius: 4, height: 6, place: "new", material: 3 });
  const r = await call(b.s, "build_tower", { at: [600, 0, 0], radius: 4, height: 6, place: "new", material: 3, roof: { style: "dome", radius: 2, height: 1 } });
  const ca = cells(a.store), cb = cells(b.store);
  for (const [k, v] of ca) assert.equal(cb.get(k), v, k);
  assert.ok(cb.size > ca.size);
  assert.equal(r.structureInfo.towers[0].roof.height, 1);
  await assert.rejects(() => call(b.s, "build_tower", { at: [700, 0, 0], roof: { style: "spire" } }), /dome.*onion.*cone/);
});

test("build_castle towerRoof: only and materials per corner, towers listed, roofed tower tops are not spawn spots", async () => {
  const { store, s } = session();
  const r = await call(s, "build_castle", { at: [600, 0, 0], size: [30, 30], place: "new", material: 3, keep: false, dryRun: false,
    towerRoof: { style: "dome", material: 11, only: ["ne", "sw"], materials: { ne: 13 } } });
  const tw = r.structureInfo.towers as any[];
  assert.deepEqual(tw.map((t) => t.corner), ["nw", "sw", "ne", "se"]);
  const by = Object.fromEntries(tw.map((t) => [t.corner, t]));
  assert.deepEqual(by.ne.at, [613.5, 9, -13.5]);
  assert.equal(by.nw.roof, undefined);
  assert.equal(by.ne.roof.material, 13);
  assert.equal(by.sw.roof.material, 11);
  const c = cells(store);
  assert.equal(c.get("613,9,-14"), 13);
  assert.equal(c.get("586,9,13"), 11);
  assert.equal(c.get("586,9,-14"), undefined);                    // nw: open top
  const spots = r.spawnSpots.spots.filter((q: any) => q.kind === "tower");
  assert.equal(spots.length, 2);
  await assert.rejects(() => call(s, "build_castle", { at: [700, 0, 0], towerRoof: { style: "dome", only: ["north"] } }), /nw, sw, ne, se/);
});

// ------------------------------------------------------------------ 11: mesas

test("build_mesa: a setback steps a small mesa where a walkable tread does not fit, and ledges list the exposed tops", async () => {
  const { s } = session();
  const base = { at: [600, 0], floor: 0, top: 10, shape: { kind: "circle", radius: 9 }, voxelSize: 2.5, strata: { step: 2 }, tread: { width: 2.5, jitter: 0 } };
  await assert.rejects(() => call(s, "build_mesa", base), /too narrow to walk at voxelSize 2.5.*voxelSize 1.25 or finer.*walkable: false/s);
  const r = await call(s, "build_mesa", { ...base, tread: { walkable: false, width: 2.5, jitter: 0 } });
  assert.equal(r.mesa.tread.setback, true);
  assert.equal(r.mesa.tread.width, 2.5);
  assert.deepEqual(r.mesa.ledges.map((l: any) => [l.kind, l.y.map, l.walkable]), [["step", 5, false], ["plateau", 10, true]]);
  assert.equal(r.mesa.ledges[0].y.world, 100);
  // the step is really set back: the outline's edge stands at 5, the middle at 10
  const q = await call(s, "query_ground", { x: 600, z: 0, units: "map" });
  assert.equal(q.standOn.map, 10);
  const e = await call(s, "query_ground", { x: 608, z: 0, units: "map" });
  assert.equal(e.standOn.map, 5);
  // a finer voxel makes the same width walkable
  const f = session();
  const fine = await call(f.s, "build_mesa", { ...base, voxelSize: 1.25, strata: { step: 4 } });
  assert.equal(fine.mesa.tread.walkable, true);
  assert.deepEqual(fine.mesa.ledges.map((l: any) => l.y.map), [5, 10]);
  // a sheer mesa lists its plateau only (and a talus top when it has one)
  const g = session();
  const sheer = await call(g.s, "build_mesa", { ...base, tread: { walkable: false }, talus: { height: 2.5, width: 5 } });
  assert.deepEqual(sheer.mesa.ledges.map((l: any) => [l.kind, l.y.map]), [["talus top", 2.5], ["plateau", 10]]);
});

// ------------------------------------------------------------------ 12: the ship range

test("shipRange: the newest release a used feature needs, and a warning for a lower range asked", () => {
  assert.deepEqual(shipRange(10), { melangeRange: ">=0.2.0", gated: false, why: "10 details: within the vanilla mesh pool" });
  // the mesh pool alone: the range and wording Caldera's export had
  assert.equal(shipRange(1000).melangeRange, ">=0.14.0");
  assert.match(shipRange(1000).why, /^1000 details: over the 450 the vanilla 550-instance mesh pool loads, so the pack needs Melange 0\.14\.0\+/);
  const bank = shipRange(10, undefined, { modMeshes: ["dustcrown.Boulder"] });
  assert.equal(bank.melangeRange, ">=0.14.0");
  assert.match(bank.why, /dustcrown\.Boulder/);
  const areas = shipRange(1000, undefined, { modMeshes: ["a.B"], spawnAreas: true });
  assert.equal(areas.melangeRange, `>=${SPAWN_AREAS_MELANGE}`);
  assert.equal(areas.needs!.length, 3);
  assert.match(areas.why, /spawns\.mode areas/);
  const low = shipRange(10, ">=0.14.1", { spawnAreas: true });
  assert.equal(low.melangeRange, ">=0.14.1");
  assert.match(low.warning!, /admits a Melange older than 0\.15\.0/);
  assert.equal(shipRange(10, ">=0.15.0", { spawnAreas: true }).warning, undefined);
  assert.equal(rangeFloor(">0.14.1 <2.0.0"), "0.14.1");
  assert.equal(rangeFloor("^0.15.0"), "0.15.0");
});

test("shipFeatures reads spawn areas and the pack's own meshes from the project", () => {
  const { store } = session();
  assert.deepEqual(shipFeatures(store), { spawnAreas: false });
  store.scene.spawns = { mode: "areas", areas: [{ name: "A", group: "a", knots: 1 }] } as any;
  store.scene.details.push({ ...store.scene.details[0], id: 99999, name: "rock1", resource: "dustcrown.Boulder" } as any);
  assert.deepEqual(shipFeatures(store), { spawnAreas: true, modMeshes: ["dustcrown.Boulder"] });
});

// ------------------------------------------------------------------ 13, 14: onSkin and sparse parts

test("onSkin names a structure's blocks ergonskin_N, and the pokes advisory leaves them out", async () => {
  const { store, s } = session();
  const p = planDrape(store, { units: "map", min: [600, 0], max: [630, 30], floor: 0, expr: "4 + x / 30", voxelSize: 2.5, thickness: 2, core: false, material: 3 } as any);
  applyDrape(store, p);
  const plug = { at: [615, 0, 15], place: "new", material: 5, shapes: [{ kind: "box", size: [4, 4], height: 5.5 }] };
  const a = await call(s, "build_compound", plug);
  assert.match(a.newBlocks[0].name, /^ergblock_/);
  assert.equal(drapePokes(grids(store)).length, 1);
  store.undo();
  const b = await call(s, "build_compound", { ...plug, onSkin: true });
  assert.match(b.newBlocks[0].name, /^ergonskin_0$/);
  assert.equal(drapePokes(grids(store)).length, 0);
  assert.equal(onSkinBlocks(grids(store)), 1);
  assert.equal((describe(s, {}) as any).drape, undefined);
});

test("packing parts: a part whose box is mostly air is named in the notes", async () => {
  const { s } = session();
  const wing = (x: number, z: number, part: number) => ({ kind: "box", size: [4, 4], height: 6, at: [x, 0, z], part });
  const shared = await call(s, "build_compound", { at: [600, 0, 0], place: "new", packing: "parts", material: 3, dryRun: true,
    shapes: [wing(-12, -12, 1), wing(12, -12, 1), wing(-12, 12, 1), wing(12, 12, 1)] });
  assert.equal(shared.newBlocks.length, 1);
  assert.match(shared.notes.join(" "), /packing parts: part 1 is one block of 4704 cells \(28 x 6 x 28\) of which 384 hold a voxel \(8 %\)/);
  const apart = await call(s, "build_compound", { at: [600, 0, 0], place: "new", packing: "parts", material: 3, dryRun: true,
    shapes: [wing(-12, -12, 1), wing(12, -12, 2), wing(-12, 12, 3), wing(12, 12, 4)] });
  assert.equal(apart.newBlocks.length, 4);
  assert.equal(apart.notes.filter((n: string) => n.startsWith("packing parts")).length, 0);
});

// ------------------------------------------------------------------ 4: pits

/** A 30 x 12 slab 6 high with a 5 x 5 pit 3 deep in its middle. */
async function pitted(s: Session) {
  await call(s, "build_compound", { at: [615, 0, 6], place: "new", material: 3, shapes: [
    { kind: "box", size: [30, 12], height: 6 }, { kind: "box", carve: true, size: [5, 5], height: 3, at: [0, 3, 0] }] });
}
const REGION = { x0: 598, z0: -2, x1: 632, z1: 14, units: "map" };

test("check_pits finds a sheer pit, its floor and the climb out, and the start that walks into it", async () => {
  const { store, s } = session();
  await pitted(s);
  const r = checkPits(store, { ...REGION, from: [[602, 6, 6]] }) as any;
  assert.equal(r.ok, false);
  assert.equal(r.pits.length, 1);
  const p = r.pits[0];
  assert.equal(p.floorY, 3);
  assert.equal(p.climbOut, 3);
  assert.equal(p.area, 25);
  assert.deepEqual(p.box, { min: [612, 3], max: [617, 8] });
  assert.deepEqual(p.reachedFrom, ["from[0]"]);
  assert.match(r.summary, /1 pit a worm cannot walk out of, 1 of them walked or dropped into from a start; the deepest needs a climb of 3/);
  // a climb a jump clears: no pit
  assert.equal((checkPits(store, { ...REGION, maxStep: 3 }) as any).ok, true);
  // the tool, and save's advisory lines
  const t = await call(s, "check_pits", REGION);
  assert.equal(t.pits.length, 1);
  assert.match(pitWarnings(store).join("\n"), /a pit at \(61\d(\.\d+)?, \d(\.\d+)?\), floor y 3: 25 map units squared, the least climb out is 3/);
});

test("check_pits: a plateau and a stair are no pits; a ramp out of the pit clears it", async () => {
  const { store, s } = session();
  await pitted(s);
  await slab(s, 660, 670, 0, 10, 12);                             // a plateau: a worm drops off it
  assert.equal((checkPits(store, { x0: 655, z0: -5, x1: 675, z1: 15, units: "map" }) as any).ok, true);
  // steps of half a unit out of the pit along +x
  const steps = [];
  for (let i = 0; i < 6; i++) steps.push({ kind: "box", size: [1, 5], height: 0.5 * (i + 1), at: [617.5 - 615 - i, 3, 0] });
  await call(s, "build_compound", { at: [615, 0, 6], place: "new", scale: 0.5, material: 4, shapes: steps });
  const r = checkPits(store, REGION) as any;
  assert.equal(r.ok, true, JSON.stringify(r.pits));
});

test("movesFrom agrees with search: the nodes reached are the closure of the moves", async () => {
  const { store, s } = session();
  await pitted(s);
  const gs = grids(store), o: ReachOptions = { step: 1, maxStep: 0.5, maxSlope: 45, maxDrop: 8, maxGap: 2, headroom: 1.5, water: null, slopeOver: "tread", bodyWidth: 0.6 };
  const S = surfaces(gs, { x0: 598, z0: -2, x1: 632, z1: 14 }, 1, 1.5);
  const n0 = [...Array(S.nodes).keys()].find((n) => S.y[n] === 6)!;
  const q = search(S, n0, o);
  const seen = new Uint8Array(S.nodes), todo = [n0];
  seen[n0] = 1;
  while (todo.length) for (const b of movesFrom(S, todo.pop()!, o)) if (!seen[b]) { seen[b] = 1; todo.push(b); }
  assert.deepEqual([...seen], [...q.reached]);
  const tr = findTraps(S, o, { maxArea: 400 });
  assert.equal(tr.traps.filter((t) => t.kind === "pit").length, 1);
});

test("check_reachability traps: report names the pit a route crosses, avoid walks round it", async () => {
  const { s } = session();
  await pitted(s);
  const args = { ...REGION, from: [[601, 6, 5.5]], target: { x: 629, z: 5.5, radius: 1 }, route: true };
  const plain = await call(s, "check_reachability", args);
  assert.equal(plain.traps, undefined);
  assert.equal(plain.spawns[0].pitsReached, undefined);
  assert.equal(plain.spawns[0].reachable, true);
  // the route already goes round the pit (there is no way out of it), but the walk gets into it
  for (const p of plain.spawns[0].route) assert.ok(!(p[0] > 612 && p[0] < 617 && p[2] > 3 && p[2] < 8), JSON.stringify(p));
  const rep = await call(s, "check_reachability", { ...args, traps: "report" });
  assert.equal(rep.traps.pits, 1);
  assert.deepEqual(rep.spawns[0].pitsReached.map((p: any) => [p.floorY, p.climbOut, p.area]), [[3, 3, 25]]);
  assert.match(rep.summary, /1 pit a worm cannot walk out of, walked or dropped into from from\[0\]/);
  assert.equal(rep.spawns[0].walk, plain.spawns[0].walk);         // report changes no route
  assert.equal(rep.spawns[0].reachedColumns, plain.spawns[0].reachedColumns);
  const av = await call(s, "check_reachability", { ...args, traps: "avoid" });
  assert.equal(av.spawns[0].reachable, true);
  assert.equal(av.spawns[0].pitsReached, undefined);
  assert.equal(av.spawns[0].reachedColumns, plain.spawns[0].reachedColumns - 25);
  // a target in the pit is still reached (its own pit stays walkable)
  const inPit = await call(s, "check_reachability", { ...REGION, from: [[601, 6, 5.5]], target: { x: 614.5, z: 5.5, radius: 0.6 }, traps: "avoid" });
  assert.equal(inPit.spawns[0].reachable, true);
  assert.equal(inPit.spawns[0].path.crossesPits.length, 1);
});

// ------------------------------------------------------------------ 6: openings

test("check_reachability centre: the route goes through a gate's middle, and narrowOpenings lists a slot", async () => {
  const { s } = session();
  // a floor with a wall across it at x 615-617, a gate 4 wide at z 8-12 and a slot 1 wide at z 20-21
  await call(s, "build_compound", { at: [615, 0, 15], place: "new", material: 3, shapes: [
    { kind: "box", size: [30, 30], height: 2 }, { kind: "box", size: [2, 30], height: 6, at: [1, 2, 0] },
    { kind: "box", carve: true, size: [2, 4], height: 4, at: [1, 2, -5] }, { kind: "box", carve: true, size: [2, 1], height: 4, at: [1, 2, 5.5] }] });
  const region = { x0: 598, z0: -2, x1: 632, z1: 32, units: "map" };
  // the start and the target lie above the gate's side, so the shortest walk hugs its north jamb
  const args = { ...region, from: [[604, 2, 2.5]], target: { x: 626, z: 2.5, radius: 0.6 }, route: true };
  const plain = await call(s, "check_reachability", args);
  const inGate = (r: any) => r.spawns[0].route.filter((p: number[]) => p[0] > 615 && p[0] < 617);
  assert.ok(inGate(plain).length >= 2);
  assert.ok(inGate(plain).every((p: number[]) => p[2] === 8.5), JSON.stringify(inGate(plain)));
  assert.equal(plain.spawns[0].path.narrowOpenings, undefined);  // 4 wide: over twice a worm
  const mid = await call(s, "check_reachability", { ...args, centre: true });
  assert.ok(inGate(mid).every((p: number[]) => p[2] === 10), JSON.stringify(inGate(mid)));
  assert.equal(mid.spawns[0].walk, plain.spawns[0].walk);
  // through the slot (the gate closed off by a region that leaves it out): one narrow opening, 1 wide, at its middle
  const slot = await call(s, "check_reachability", { ...region, z0: 14, from: [[604, 2, 20.5]], target: { x: 626, z: 20.5, radius: 0.6 } });
  assert.equal(slot.spawns[0].reachable, true);
  assert.deepEqual(slot.spawns[0].path.narrowOpenings, [{ width: 1, centre: [615.5, 2, 20.5] }]);
});

// ------------------------------------------------------------------ 5: basins

test("spillLevels, basins and fillBasins on a grid", () => {
  // 7 x 7, height 5, a 3 x 3 dish at 2 in the middle with a notch at 4 on its east rim; the grid's edge is the outlet
  const w = 7, h = new Float32Array(49).fill(5), kind = new Uint8Array(49);
  for (let z = 0; z < 7; z++) for (let x = 0; x < 7; x++) if (!x || !z || x === 6 || z === 6) kind[z * w + x] = OUTLET;
  for (let z = 2; z <= 4; z++) for (let x = 2; x <= 4; x++) h[z * w + x] = 2;
  h[3 * w + 5] = 4;
  h[3 * w + 6] = 4;                                                // the notch runs out over the edge
  const sp = spillLevels(h, w, 7, kind);
  assert.equal(sp[3 * w + 3], 4);                                  // spills over the notch
  assert.equal(sp[1 * w + 1], 5);
  const bs = basins(h, w, 7, kind, sp, 0.5);
  assert.equal(bs.length, 1);
  assert.deepEqual([bs[0].cells, bs[0].deepest, bs[0].floor, bs[0].spill], [9, 2, 2, 4]);
  const part = Float32Array.from(h), r = fillBasins(part, w, 7, kind, 0.5);
  assert.deepEqual([r.raised, r.most], [9, 1.5]);
  assert.equal(part[3 * w + 3], 3.5);
  const full = Float32Array.from(h);
  fillBasins(full, w, 7, kind, 0);
  assert.equal(full[3 * w + 3], 4);
  assert.equal(basins(full, w, 7, kind, spillLevels(full, w, 7, kind), 0).length, 0);
  // walls all round: walled in, left alone
  const k2 = Uint8Array.from(kind);
  for (let z = 1; z <= 5; z++) for (let x = 1; x <= 5; x++) if (x === 1 || z === 1 || x === 5 || z === 5) k2[z * w + x] = WALL;
  const sp2 = spillLevels(h, w, 7, k2);
  assert.equal(sp2[3 * w + 3], Infinity);
  assert.equal(fillBasins(Float32Array.from(h), w, 7, k2, 0).raised, 0);
  assert.equal(GROUND, 0);
});

test("heightfield_drape fillBasins: a closed hollow is filled to its spill height less maxDepth; off, nothing changes", async () => {
  const dish = "10 - 8 * step(hypot(x, z), 6)";                   // a sheer dish 8 deep, 12 across, in a flat field
  const args = (extra: Record<string, unknown>) => ({ units: "map", min: [600, 0], max: [650, 50], floor: 0, expr: dish, voxelSize: 2.5, thickness: 2, core: false, material: 3, ...extra } as any);
  const a = session(), b = session(), c = session();
  const pa = planDrape(a.store, args({}));
  assert.equal((pa.report.region as any).basins, undefined);
  applyDrape(a.store, pa);
  assert.equal((checkPits(a.store, { x0: 600, z0: 0, x1: 650, z1: 50, units: "map" }) as any).pits.length, 1);
  const pb = planDrape(b.store, args({ fillBasins: true }));
  const rb = (pb.report.region as any).basins;
  assert.equal(rb.basins, 1);
  assert.ok(rb.columnsRaised >= 12, JSON.stringify(rb));
  assert.equal(rb.mostRaised, 8);
  assert.deepEqual([rb.largest[0].depth, rb.largest[0].floorY, rb.largest[0].spillY], [8, 2, 10]);
  applyDrape(b.store, pb);
  assert.equal((checkPits(b.store, { x0: 600, z0: 0, x1: 650, z1: 50, units: "map" }) as any).ok, true);
  const qb = await call(b.s, "query_ground", { x: 625, z: 25, units: "map" });
  assert.equal(qb.standOn.map, 10);
  // maxDepth keeps a shallow dish
  const pc = planDrape(c.store, args({ fillBasins: { maxDepth: 1 } }));
  assert.equal((pc.report.region as any).basins.mostRaised, 7);
  applyDrape(c.store, pc);
  const qc = await call(c.s, "query_ground", { x: 625, z: 25, units: "map" });
  assert.equal(qc.standOn.map, 9);
  // ground at or under outletBelow is an outlet: the dish drains, nothing is raised
  const pd = planDrape(session().store, args({ fillBasins: { outletBelow: 2 } }));
  assert.equal((pd.report.region as any).basins.columnsRaised, 0);
  assert.throws(() => planDrape(session().store, args({ fillBasins: { maxDepth: -1 } })), /maxDepth/);
});
