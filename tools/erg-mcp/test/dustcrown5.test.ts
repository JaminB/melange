// Dustcrown round 5's builder gaps on the synthetic 12-frame level: a gate's splayed jambs, a rule edge that softens a
// slope bound, the structures' coreMaterial and check_exposed_core, build_mesa's cap (dome, erosion, treads) and
// build_outcrop, build_boulder_field and lint_scatter_stairs, check_pits {fix}, the version checks at connect, a rock
// bank's palette preview, the grade's shade fit and tint_landscape's preview, and set_water_look's haze. Every new
// setting is off by default: the old calls give the old voxels.
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, isSolid, material, maskOf, secondOf, type Loaded, type Scene } from "../src/core";
import { lintStairs, planField, riserNear, type FieldSpec } from "../src/lib/boulders";
import { colourReads, exposedCore, pixelReads, withCore } from "../src/lib/exposed-core";
import { peVersion } from "../src/lib/game-build";
import { fitGrade, gradeShade, parseGrade, shadeDrift } from "../src/lib/grade";
import { apply, planFit } from "../src/lib/heightfield-plan";
import type { MaterialSet } from "../src/lib/materials";
import { facePatches } from "../src/lib/material-look";
import { decodePng } from "../src/lib/inspect-png";
import { rockSheet } from "../src/lib/rock-preview";
import { buildRocks } from "../src/lib/rockmesh";
import { ERG_FEATURES, lackWarning, lacking, versionOf } from "../src/lib/ship-gate";
import { box, doorway } from "../src/lib/structures-shapes";
import { hazeLook, readHaze, readSea, waterLookBlock, withWaterLook } from "../src/lib/water-look";
import { grids } from "../src/lib/voxel";
import { validate } from "../src/registry";
import { tools as boulderTools } from "../src/tools/boulders";
import { checkExposedCore } from "../src/tools/exposedcore";
import { checkPits, fixPits } from "../src/tools/inspect";
import { tools as mesaTools } from "../src/tools/mesa";
import { tools as rockTools } from "../src/tools/rockbank";
import { tools as structureTools } from "../src/tools/structures";
import type { Session } from "../src/session";

function loaded(): Loaded {
  const s: Scene = JSON.parse(readFileSync(new URL("../../../../tests/fixtures/erg/synthetic-12.json", import.meta.url), "utf8"));
  s.frames.find((f) => f.id === 1)!.name = "Scene";
  const blobs = new Map<number, ArrayBuffer>();
  for (const [ref, buf] of syntheticBlobs(s) as Map<number, Uint8Array>) blobs.set(ref, buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength) as ArrayBuffer);
  return { scene: s, blobs };
}
const newStore = () => new EditorStore("t", loaded(), loaded());
const sessionOf = (store: EditorStore, extra: Record<string, unknown> = {}) =>
  ({ requireProject: () => ({ store, id: "t", info: {}, erg: { script: async () => "" } }), get store() { return store; }, ...extra }) as unknown as Session;
const callTool = async (list: { name: string; inputSchema: unknown; handler: (s: Session, a: never) => unknown }[], name: string, s: Session, args: Record<string, unknown>): Promise<any> => {
  const t = list.find((x) => x.name === name)!;
  const bad = validate(t.inputSchema as never, args);
  if (bad) throw new Error(bad);
  const r: any = await t.handler(s, args as never);
  return r?.content ? JSON.parse(r.content.find((c: { type: string }) => c.type === "text").text) : typeof r === "string" ? JSON.parse(r) : r;
};
const words = (st: EditorStore) => createHash("sha1").update(grids(st).map((g) => `${g.id}:${Array.from(g.words).join(",")}`).join("|")).digest("hex");
/** The materials of the added blocks' solid voxels, counted. */
function mats(store: EditorStore): Map<number, number> {
  const n = new Map<number, number>();
  for (const g of grids(store)) if (g.frame.new) for (const w of g.words) if (isSolid(w)) n.set(material(w), (n.get(material(w)) ?? 0) + 1);
  return n;
}
/** The tops (map y) of the added blocks' columns. */
function tops(store: EditorStore): number[] {
  const out: number[] = [];
  for (const g of grids(store)) {
    if (!g.frame.new) continue;
    const [X, Y, Z] = g.size, m = g.toMap;
    for (let z = 0; z < Z; z++) for (let x = 0; x < X; x++) {
      let top = -1;
      for (let y = 0; y < Y; y++) if (isSolid(g.words[(z * X + x) * Y + y])) top = y;
      if (top >= 0) out.push(Math.round((m[5] * (top + 1) + m[7]) * 1000) / 1000);
    }
  }
  return out;
}

// ------------------------------------------------------------------ gates[].jambBevel

test("doorway jambBevel: the jambs splay at both faces, under the head only; none by default", () => {
  // a wall 4 thick on the south side of a 20 x 20 rectangle: faces at z 6 and z 10; a square opening 4 wide, 6 high
  const plain = doorway(10, 10, 4, "s", 4, 6, 0, 0, 0, true), splay = doorway(10, 10, 4, "s", 4, 6, 0, 0, 0, true, 0, 2);
  const row = (sh: typeof plain, z: number, y = 0.5) => [-3.5, -2.5, -1.5, -0.5, 0.5, 1.5, 2.5, 3.5].map((x) => (sh.hit(x, y, z) ? "#" : ".")).join("");
  for (const z of [6.5, 7.5, 8.5, 9.5]) assert.equal(row(plain, z), "..####..");
  // two wider at each face, one wider a voxel in, the opening's own width in the middle of the wall
  assert.equal(row(splay, 9.5), "########");
  assert.equal(row(splay, 8.5), ".######.");
  assert.equal(row(splay, 7.5), ".######.");
  assert.equal(row(splay, 6.5), "########");
  // a canted head: the splay stops where the head starts
  const both = doorway(10, 10, 4, "s", 4, 6, 0, 0, 0, true, 2, 2);
  assert.equal(row(both, 9.5, 3.5), "########");
  assert.equal(row(both, 9.5, 5.5), "...##...");
  // at most half the wall
  assert.equal(row(doorway(10, 10, 4, "s", 4, 6, 0, 0, 0, true, 0, 9), 9.5), "########");
});

test("build_castle gates[].jambBevel reaches the gate and is answered; without it the castle is the old one", async () => {
  const args = { at: [500, 12, 500], size: [24, 24], wallHeight: 8, towerRadius: 3, keep: false, material: 6, wallThickness: 4 };
  const a = newStore(), b = newStore(), c = newStore();
  const ra = await callTool(structureTools, "build_castle", sessionOf(a), { ...args, gates: [{ side: "s", width: 6, height: 5 }] });
  const rb = await callTool(structureTools, "build_castle", sessionOf(b), { ...args, gates: [{ side: "s", width: 6, height: 5, jambBevel: 2 }] });
  await callTool(structureTools, "build_castle", sessionOf(c), { ...args, gates: [{ side: "s", width: 6, height: 5, jambBevel: 0 }] });
  assert.equal(rb.structureInfo.gates[0].jambBevel, 2);
  assert.equal(ra.structureInfo.gates[0].jambBevel, undefined);
  assert.ok(rb.voxels.added < ra.voxels.added, "the splay carves more of the wall");
  assert.equal(words(a), words(c));
});

// ------------------------------------------------------------------ materials[].edge.soft

test("edge.soft: a column a slope bound leaves out takes the material on the corners it shares; off by default", () => {
  // a ramp that steepens along x: gentle (rule 7 holds) then steep (it does not)
  const args = { min: [500, -8] as [number, number], max: [540, 8] as [number, number], floor: 0, expr: "2 + 0.02 * (x + 20)^2", material: 1 };
  const second = (store: EditorStore) => {
    let n = 0;
    for (const g of grids(store)) if (g.frame.new) for (const w of g.words) if (isSolid(w) && material(w) !== 7 && secondOf(w) === 7 && maskOf(w)) n++;
    return n;
  };
  const hard = newStore(), soft = newStore(), plain = newStore();
  apply(hard, planFit(hard, { ...args, materials: [{ material: 7, maxSlope: 20, mask: "1", edge: { depth: 1 } }] }));
  apply(soft, planFit(soft, { ...args, materials: [{ material: 7, maxSlope: 20, mask: "1", edge: { depth: 1, soft: true } }] }));
  apply(plain, planFit(plain, { ...args, materials: [{ material: 7, maxSlope: 20, mask: "1" }] }));
  assert.ok((mats(hard).get(7) ?? 0) > 0 && (mats(hard).get(1) ?? 0) > 0);
  // the mask holds everywhere and the slope is the column's: the plain edge has nothing to draw
  assert.equal(second(hard), 0);
  assert.ok(second(soft) > 0, "the first steep columns carry material 7 on the corners they share with the gentle ones");
  // the rule's own columns are the same either way
  assert.equal(mats(soft).get(7), mats(hard).get(7));
  assert.equal(second(plain), 0);
  // a slope bound alone is an area for a soft edge, not for a plain one
  assert.doesNotThrow(() => planFit(newStore(), { ...args, materials: [{ material: 7, maxSlope: 20, edge: { soft: true } }] }));
  assert.throws(() => planFit(newStore(), { ...args, materials: [{ material: 7, maxSlope: 20, edge: {} }] }), /needs an area/);
});

// ------------------------------------------------------------------ coreMaterial, check_exposed_core

test("withCore: cells deeper than the depth under the plan's surface are the core; carves keep their faces", () => {
  const slab = [box("fill", 3, [0, 0, 0], [10, 6, 10]), box("carve", 0, [4, 2, 0], [6, 6, 3])];
  const cored = withCore(slab, 9, 1), at = (x: number, y: number, z: number) => cored[0].matAt!(x + 0.5, y + 0.5, z + 0.5);
  assert.equal(at(5, 3, 6), 9);                      // the middle
  assert.equal(at(0, 3, 6), 3);                      // a side face
  assert.equal(at(5, 5, 6), 3);                      // the top
  assert.equal(at(5, 0, 6), 9);                      // the base stands on the ground: core
  assert.equal(at(5, 3, 3), 3);                      // the niche's back wall
  assert.equal(at(5, 1, 1), 3);                      // under the niche's floor
  assert.equal(cored[1], slab[1]);
  // no fill: as it was
  assert.deepEqual(withCore([slab[1]], 9, 1), [slab[1]]);
});

const SET: MaterialSet = { file: "t", names: Array.from({ length: 16 }, (_, i) => `m${i}`), resolved: null,
  colors: Array.from({ length: 16 }, (_, i) => (i === 5 ? [226, 222, 244] : i === 9 ? [150, 110, 70] : [210, 180, 110])) as never,
  side: Array.from({ length: 16 }, (_, i) => (i === 5 ? [180, 120, 70] : i === 9 ? [140, 100, 64] : [200, 170, 100])) as never, sideResolved: null };

test("colourReads: lilac, sky and ice are blue or white; sand, ochre and rock are earth", () => {
  for (const c of [[226, 222, 244], [112, 128, 200], [150, 200, 230]] as const) assert.equal(colourReads([...c]), "blue", String(c));
  assert.equal(colourReads([245, 245, 240]), "white");
  for (const c of [[214, 190, 140], [242, 227, 189], [150, 110, 70], [120, 120, 112], [218, 213, 178]] as const) assert.equal(colourReads([...c]), "earth", String(c));
});

test("check_exposed_core: a crater through a banded plinth opens onto the band's top; coreMaterial fixes it; nothing is edited", async () => {
  const plinth = { at: [600, 0, 0], place: "new", material: 3, shapes: [{ kind: "box", size: [16, 16], height: 12, bands: [{ fromY: 0, toY: 6, material: 5 }] }] };
  const bad = newStore();
  await callTool(structureTools, "build_compound", sessionOf(bad), plinth);
  const before = words(bad);
  const r = checkExposedCore(grids(bad), { points: [[600, 0]], radius: 3, passes: 3, frames: "all" }, SET) as any;
  assert.equal(words(bad), before);
  assert.equal(r.ok, false);
  // the hole's floor is the top of the band of material 5 (a pale lilac tile); its walls are the band's sides, earth
  const top5 = r.uncovered.find((u: any) => u.material === 5 && u.face === "top");
  assert.ok(top5 && top5.reads === "blue" && top5.voxels > 0, JSON.stringify(r.uncovered));
  assert.equal(r.uncovered.find((u: any) => u.material === 3 && u.face === "side")?.reads, "earth");
  assert.deepEqual(r.flagged.map((f: any) => `${f.material} ${f.face}`), ["5 top"]);
  assert.match(r.summary, /m5 \(top, #e2def4, blue\)/);
  // allowed on purpose
  assert.equal((checkExposedCore(grids(bad), { points: [[600, 0]], radius: 3, frames: "all", allow: [5] }, SET) as any).ok, true);
  // the same plinth with a core: the hole opens onto material 9
  const good = newStore();
  const g = await callTool(structureTools, "build_compound", sessionOf(good), { ...plinth, coreMaterial: 9 });
  assert.deepEqual(g.structureInfo.core, { material: 9, depth: 1, note: g.structureInfo.core.note });
  const rg = checkExposedCore(grids(good), { points: [[600, 0]], radius: 3, passes: 3, frames: "all" }, SET) as any;
  assert.equal(rg.ok, true, JSON.stringify(rg.uncovered));
  assert.ok(rg.uncovered.some((u: any) => u.material === 9 && u.face === "top"));
  // the faces are as they were asked: the same voxel count, and material 5 still on the outside
  assert.equal(g.voxels.added, 16 * 16 * 12);
  assert.ok((mats(good).get(5) ?? 0) > 0 && (mats(good).get(9) ?? 0) > 0);
  // no colours: nothing can be judged
  assert.equal((checkExposedCore(grids(bad), { points: [[600, 0]], frames: "all" }, null) as any).ok, null);
  // coreDepth without a core is refused; without either the build is the old one
  await assert.rejects(callTool(structureTools, "build_compound", sessionOf(newStore()), { ...plinth, coreDepth: 2 }), /give coreMaterial/);
  const plain = newStore();
  await callTool(structureTools, "build_compound", sessionOf(plain), plinth);
  assert.equal(words(plain), before);
});

test("exposedCore: a second and third hit dig deeper than one", () => {
  const st = newStore();
  return callTool(structureTools, "build_compound", sessionOf(st), { at: [600, 0, 0], place: "new", material: 3, shapes: [{ kind: "box", size: [16, 16], height: 12 }] }).then(() => {
    const gs = grids(st).filter((g) => g.frame.new);
    const one = exposedCore(gs, [[600, 12, 0]], 3, 1), three = exposedCore(gs, [[600, 12, 0]], 3, 3);
    assert.ok(three.carved[0] > one.carved[0] && one.carved[0] > 0);
  });
});

// ------------------------------------------------------------------ build_mesa cap, build_outcrop

const MESA = { at: [520, 0], floor: 0, top: 12, shape: { kind: "circle", radius: 14 }, tread: { width: 3, jitter: 0 }, strata: { step: 3 }, material: 3 };

test("build_mesa cap: dome swells the plateau, erosion pits it, treads caps every step; the plain cap is the old one", async () => {
  const plain = newStore(), dome = newStore(), ero = newStore(), treads = newStore(), old = newStore();
  await callTool(mesaTools, "build_mesa", sessionOf(old), { ...MESA, cap: { material: 7 } });
  await callTool(mesaTools, "build_mesa", sessionOf(plain), { ...MESA, cap: { material: 7, dome: 0, treads: false } });
  assert.equal(words(plain), words(old));
  const rd = await callTool(mesaTools, "build_mesa", sessionOf(dome), { ...MESA, cap: { material: 7, dome: 4 } });
  assert.equal(Math.max(...tops(old)), 12);
  assert.equal(Math.max(...tops(dome)), 16);
  assert.equal(rd.mesa.cap.dome, 4);
  assert.equal(rd.mesa.cap.crownY.map, 16);
  assert.equal(rd.mesa.topY.map, 12);
  // the dome's columns are all capped
  assert.ok((mats(dome).get(7) ?? 0) >= (mats(old).get(7) ?? 0));
  const re = await callTool(mesaTools, "build_mesa", sessionOf(ero), { ...MESA, cap: { material: 7, erosion: { amount: 2, scale: 4, seed: 3 } } });
  const plateauTops = tops(ero).filter((y) => y > 9);
  assert.ok(Math.min(...plateauTops) < 12 && Math.max(...plateauTops) === 12, JSON.stringify([...new Set(plateauTops)]));
  assert.ok(Math.min(...plateauTops) >= 10, "never down to the step under it");
  assert.equal(re.mesa.cap.erosion, 2);
  // the pitted columns keep the cap
  assert.equal(mats(ero).get(7), mats(old).get(7));
  await callTool(mesaTools, "build_mesa", sessionOf(treads), { ...MESA, cap: { material: 7, treads: true } });
  assert.ok((mats(treads).get(7) ?? 0) > 2 * (mats(old).get(7) ?? 0), "every step's top is capped, not only the plateau");
  await assert.rejects(callTool(mesaTools, "build_mesa", sessionOf(newStore()), { ...MESA, cap: { material: 7, dome: -1 } }), /dome/);
});

test("build_outcrop: a seated rock with a rock cap on every ledge, in blocks of its own", async () => {
  const st = newStore(), base = newStore();
  // ground to stand on: a slab at y 0..4
  for (const s of [st, base]) await callTool(structureTools, "build_compound", sessionOf(s), { at: [600, 0, 0], place: "new", material: 1, shapes: [{ kind: "box", size: [40, 40], height: 4 }] });
  const r = await callTool(mesaTools, "build_outcrop", sessionOf(st), { at: [600, 0], radius: 6, height: 5, material: 3, capMaterial: 7, seed: 5 });
  assert.equal(r.applied, true);
  assert.equal(r.outcrop.seat.ground, 4);
  assert.equal(r.outcrop.seat.sink, 1);
  assert.equal(r.mesa.topY.map, 9);
  assert.ok(r.mesa.cap.crownY.map > 9 && r.mesa.cap.treads);
  assert.ok(r.added.every((b: any) => String(b.name).startsWith("ergonskin")), JSON.stringify(r.added));
  const m = mats(st), m0 = mats(base);
  assert.ok((m.get(7) ?? 0) > 0 && (m.get(3) ?? 0) > (m0.get(3) ?? 0));
  // every column of the outcrop that shows is capped: no top voxel of the rock's own material
  let bare = 0;
  for (const g of grids(st)) {
    if (!g.frame.name.startsWith("ergonskin")) continue;
    const [X, Y, Z] = g.size;
    for (let z = 0; z < Z; z++) for (let x = 0; x < X; x++) {
      let top = -1;
      for (let y = 0; y < Y; y++) if (isSolid(g.words[(z * X + x) * Y + y])) top = y;
      if (top >= 0 && g.toMap[5] * (top + 1) + g.toMap[7] > 4.5 && material(g.words[(z * X + x) * Y + top]) !== 7) bare++;
    }
  }
  assert.equal(bare, 0);
  // the same call is the same rock; mesa settings win
  const again = newStore();
  await callTool(structureTools, "build_compound", sessionOf(again), { at: [600, 0, 0], place: "new", material: 1, shapes: [{ kind: "box", size: [40, 40], height: 4 }] });
  await callTool(mesaTools, "build_outcrop", sessionOf(again), { at: [600, 0], radius: 6, height: 5, material: 3, capMaterial: 7, seed: 5 });
  assert.equal(words(again), words(st));
  const flat = await callTool(mesaTools, "build_outcrop", sessionOf(base), { at: [600, 0], radius: 6, height: 5, material: 3, dryRun: true, mesa: { cap: { material: 3 } } });
  assert.equal(flat.applied, false);
  assert.equal(flat.mesa.cap, undefined);
  await assert.rejects(callTool(mesaTools, "build_outcrop", sessionOf(newStore()), { at: [600, 0], height: 5, material: 3 }), /radius, or radii/);
});

// ------------------------------------------------------------------ build_boulder_field, lint_scatter_stairs

const FIELD: FieldSpec = { at: [0, 0], radius: 12, count: 8, seed: 3, size: [1.5, 3], height: [1, 4], maxTiers: 2, voxel: 1, jitter: 0.6, gap: 0.5, keepOut: 6, riserMin: 2.5, materials: [3, 4], avoid: [] };

test("planField: seeded, apart by the gap, at most maxTiers, clear of risers and avoid circles", () => {
  const flat = () => 5;
  const a = planField(FIELD, flat), b = planField(FIELD, flat);
  assert.deepEqual(a, b);
  assert.equal(a.boulders.length, 8);
  for (const [i, p] of a.boulders.entries()) {
    assert.ok(p.tiers <= 2 && p.height <= 2);
    for (const q of a.boulders.slice(i + 1)) assert.ok(Math.hypot(p.x - q.x, p.z - q.z) >= p.radius + q.radius + 0.5 - 1e-9);
  }
  assert.notDeepEqual(planField({ ...FIELD, seed: 4 }, flat).boulders, a.boulders);
  // a wall along x = 8: nothing within keepOut of it
  const wall = (x: number) => (x >= 8 ? 14 : 5);
  const w = planField(FIELD, wall);
  assert.ok(w.boulders.length > 0 && w.rejected.riser > 0);
  for (const p of w.boulders) assert.ok(p.x + p.radius <= 8 - 6 + 0.5, `${p.x} + ${p.radius}`);
  assert.equal(riserNear(wall, 0, 0, 6, 2.5), null);
  assert.ok(Math.abs(riserNear(wall, 5, 0, 6, 2.5)!.distance - 2.5) < 1e-9);
  // an avoid circle over the middle
  for (const p of planField({ ...FIELD, avoid: [{ x: 0, z: 0, radius: 5 }] }, flat).boulders) assert.ok(Math.hypot(p.x, p.z) >= 5 + p.radius);
  // no ground: none
  assert.equal(planField(FIELD, () => null).boulders.length, 0);
});

test("lintStairs: a pile of three discs is a stack; a block that brings a wall within a jump is a foothold", () => {
  const ground = (x: number) => (x >= 10 ? 9 : 4);   // a wall 5 high along x = 10
  const cols = (x0: number, levels: number[]) => levels.flatMap((top, i) => [0, 1].map((j) => ({ x: x0 + i + 0.5, z: j + 0.5, top, ground: 4, i, j })));
  const o = { near: 6, riserMin: 2.5, jump: 2.5, maxTiers: 2 };
  const [low, pile, far] = lintStairs([
    { id: 1, name: "ergonskin_0", voxel: 1, columns: cols(4, [5, 6, 6]) },
    { id: 2, name: "ergonskin_1", voxel: 1, columns: cols(5, [5, 6, 7]) },
    { id: 3, name: "ergonskin_2", voxel: 1, columns: cols(-20, [5, 5]) }], ground, o);
  assert.equal(low.tiers, 2);
  assert.equal(low.stack, false);
  assert.equal(low.riser!.left, 3);
  assert.equal(low.foothold, false);                 // 3 left of the wall: more than a jump
  assert.equal(pile.tiers, 3);
  assert.equal(pile.stack, true);
  assert.equal(pile.riser!.left, 2);
  assert.equal(pile.foothold, true);                 // 2 left: a jump from the pile's top clears the wall
  assert.equal(far.riser, null);
  assert.equal(far.tiers, 1);
  assert.equal(far.stack || far.foothold, false);
});

test("build_boulder_field and lint_scatter_stairs: boulders in one on-skin block, none stacked; a hand-made pile is flagged", async () => {
  const st = newStore(), s = sessionOf(st);
  await callTool(structureTools, "build_compound", s, { at: [600, 0, 0], place: "new", material: 1, shapes: [{ kind: "box", size: [50, 50], height: 4 }, { kind: "box", size: [6, 50], height: 12, at: [22, 0, 0] }] });
  const r = await callTool(boulderTools, "build_boulder_field", s, { at: [596, 0], radius: 12, count: 6, seed: 2, materials: [3, 4] });
  assert.ok(r.built >= 4, JSON.stringify(r));
  assert.equal(r.compound.newBlocks.length, 1);
  assert.match(r.compound.newBlocks[0].name, /^ergonskin_scatter_/);
  for (const b of r.boulders) { assert.ok(b.tiers <= 2); assert.equal(b.at[1], 4); assert.ok(b.at[0] + b.radius <= 619 - 6 + 0.5); }
  const lint = await callTool(boulderTools, "lint_scatter_stairs", s, {});
  assert.equal(lint.ok, true, JSON.stringify(lint.flagged));
  assert.equal(lint.blocks, 1);
  // a hand-made pile of three discs against the wall
  await callTool(structureTools, "build_compound", s, { at: [617, 4, 10], place: "new", onSkin: true, material: 3, shapes: [
    { kind: "cylinder", radius: 3, height: 2 }, { kind: "cylinder", radius: 2, height: 4, at: [1, 0, 0] }, { kind: "cylinder", radius: 1, height: 6, at: [1.5, 0, 0] }] });
  const bad = await callTool(boulderTools, "lint_scatter_stairs", s, {});
  assert.equal(bad.ok, false);
  assert.equal(bad.flagged.length, 1);
  assert.ok(bad.flagged[0].stack && bad.flagged[0].foothold, JSON.stringify(bad.flagged[0]));
  assert.equal(bad.flagged[0].riser.left, 2);
  // refused without a rock, and with no room
  await assert.rejects(callTool(boulderTools, "build_boulder_field", s, { at: [596, 0], radius: 12 }), /materials/);
  await assert.rejects(callTool(boulderTools, "build_boulder_field", s, { at: [900, 900], radius: 4, material: 3 }), /no boulder fits/);
});

// ------------------------------------------------------------------ check_pits {fix}

test("check_pits fix: a pit a later structure closed is filled to its rim, in the floor's material; a deep court is left", async () => {
  const st = newStore(), s = sessionOf(st);
  await callTool(structureTools, "build_compound", s, { at: [615, 0, 6], place: "new", material: 3, shapes: [
    { kind: "box", size: [30, 12], height: 6 }, { kind: "box", carve: true, size: [5, 5], height: 3, at: [0, 3, 0] }] });
  const REGION = { x0: 598, z0: -2, x1: 632, z1: 14, units: "map" as const };
  assert.equal((checkPits(st, REGION) as any).pits.length, 1);
  const before = words(st);
  const dry = fixPits(st, { ...REGION, fix: true, dryRun: true }) as any;
  assert.equal(words(st), before);
  assert.equal(dry.fix.passes[0].voxelsAdded, 75);
  const r = fixPits(st, { ...REGION, fix: true }) as any;
  assert.equal(r.ok, true);
  assert.deepEqual([r.fix.before, r.fix.after, r.fix.voxelsAdded], [1, 0, 75]);
  assert.equal(mats(st).get(3), 30 * 12 * 6);
  assert.equal((checkPits(st, REGION) as any).pits.length, 0);
  // deeper than fixMaxDepth: left, and said
  const deep = newStore();
  await callTool(structureTools, "build_compound", sessionOf(deep), { at: [615, 0, 6], place: "new", material: 3, shapes: [
    { kind: "box", size: [30, 12], height: 6 }, { kind: "box", carve: true, size: [5, 5], height: 5, at: [0, 1, 0] }] });
  const d = fixPits(deep, { ...REGION, fix: true }) as any;
  assert.equal(d.fix.voxelsAdded, 0);
  assert.equal(d.pits.length, 1);
  assert.match(d.fix.skipped[0].why, /over fixMaxDepth/);
  assert.equal((fixPits(deep, { ...REGION, fix: true, fixMaxDepth: 6 }) as any).ok, true);
});

// ------------------------------------------------------------------ versions

test("lacking and lackWarning: an old server is named with what it does not know; an unknown build says nothing", () => {
  assert.equal(versionOf("0.15.0-private+abc"), "0.15.0");
  assert.equal(versionOf(undefined), null);
  assert.equal(lacking("0.15.0", ERG_FEATURES).length, 0);
  assert.equal(lacking(undefined, ERG_FEATURES).length, 0);
  assert.ok(lacking("0.14.1", ERG_FEATURES).some((f) => /spawn areas/.test(f.feature)));
  assert.ok(!lacking("0.14.1", ERG_FEATURES).some((f) => /mesh banks/.test(f.feature)));
  assert.equal(lacking("0.13.0", ERG_FEATURES).length, ERG_FEATURES.length);
  const w = lackWarning("the server", "0.14.1", ERG_FEATURES)!;
  assert.match(w, /^the server is Melange 0\.14\.1, older than this erg-mcp needs \(0\.15\.0\+\): it does not know spawn areas/);
  assert.equal(lackWarning("the server", "0.15.0", ERG_FEATURES), null);
});

test("peVersion: the fixed file info's version, from its signature", () => {
  const b = new Uint8Array(64);
  b.set([0xbd, 0x04, 0xef, 0xfe, 0, 0, 1, 0, 15, 0, 0, 0, 0, 0, 2, 0], 20);
  assert.equal(peVersion(b), "0.15.2");
  assert.equal(peVersion(new Uint8Array(64)), null);
});

// ------------------------------------------------------------------ rock palette preview

test("rockSheet and build_rock_bank preview: a picture, the colours as graded, and how far each rock stands from the ground", async () => {
  const built = buildRocks([{ name: "Boulder", kind: "boulder" }, { name: "Heap", kind: "scree" }], { palette: "sandstone" });
  const plain = rockSheet(built.rocks, { ground: [220, 205, 150] });
  const img = decodePng(plain.png);
  assert.deepEqual([img.w, img.h], [plain.width, plain.height]);
  assert.equal(plain.swatches.length, 2);
  assert.equal(plain.swatches[0].drawn.base, "#a47c52");
  // a grade that halves the light: the drawn colours follow it, and the picture changes
  const dark = rockSheet(built.rocks, { ground: [220, 205, 150], grade: { m: [[0.5, 0, 0, 0], [0, 0.5, 0, 0], [0, 0, 0.5, 0]], pairs: 2, residual: 0 } });
  assert.notEqual(dark.swatches[0].drawn.base, plain.swatches[0].drawn.base);
  assert.notEqual(createHash("sha1").update(dark.png).digest("hex"), createHash("sha1").update(plain.png).digest("hex"));
  // a rock of the ground's own colour melts into it
  const same = rockSheet(buildRocks([{ name: "B", kind: "boulder" }], { palette: { base: [220, 205, 150], dust: [220, 205, 150] } }).rocks, { ground: [220, 205, 150] });
  assert.ok(same.swatches[0].contrast!.sun < 12);
  const tool = rockTools.find((t) => t.name === "build_rock_bank")!;
  const r: any = await tool.handler({} as Session, { id: "t", rocks: [{ name: "B", kind: "boulder" }], palette: { base: [220, 205, 150], dust: [222, 206, 152], dustShare: 1 }, preview: { ground: "#dccd96" }, register: false } as never);
  assert.equal(r.content[0].type, "image");
  const j = JSON.parse(r.content[1].text);
  assert.equal(j.written, false);
  assert.match(j.preview.warning, /B has lit faces less than 12 of 255 from the ground/);
  // without preview the answer is the old one
  const o: any = await tool.handler({} as Session, { id: "t", rocks: [{ name: "B", kind: "boulder" }], register: false } as never);
  assert.equal(o.content, undefined);
  assert.equal(o.preview, undefined);
});

// ------------------------------------------------------------------ shade

test("the grade's shade fit: parsed, applied by gradeShade, and its drift toward red is said", () => {
  const sun = fitGrade([{ from: [200, 180, 120], to: [210, 190, 130] }, { from: [100, 90, 60], to: [110, 100, 70] }]);
  assert.equal(gradeShade(sun, 3, [200, 180, 120]), null);
  const sh = fitGrade([{ from: [200, 180, 120], to: [150, 100, 70] }, { from: [100, 90, 60], to: [80, 50, 35] }]);
  const g = parseGrade(JSON.stringify({ grade: { ...sun, shade: { m: sh.m, pairs: 2, residual: sh.residual }, shadeMeasured: [{ material: 9, from: [216, 173, 106], to: [196, 120, 96], kind: "steep e" }] } }));
  assert.ok(g.shade && g.shadeMeasured?.length === 1);
  // measured for this material in this colour: as measured; another colour: the fit
  assert.deepEqual(gradeShade(g, 9, [216, 173, 106]), [196, 120, 96]);
  const fit = gradeShade(g, 3, [200, 180, 120])!;
  assert.ok(Math.abs(fit[0] - 150) < 25 && Math.abs(fit[1] - 100) < 25 && fit[0] - fit[1] > 30, String(fit));
  assert.match(shadeDrift([217, 180, 110], [196, 120, 96])!, /toward red/);
  assert.equal(shadeDrift([217, 180, 110], [150, 124, 76]), null);
  // an old grade file (no shade) parses as before
  assert.equal(parseGrade(JSON.stringify({ grade: sun })).shade, undefined);
});

test("facePatches: a material's steep faces by the way they fall, and its flat ground under a wall", async () => {
  const st = newStore(), s = sessionOf(st);
  // a slab at 4, a wall 6 higher along its east edge, and a wedge falling to the west
  await callTool(structureTools, "build_compound", s, { at: [600, 0, 0], place: "new", material: 3, shapes: [
    { kind: "box", size: [40, 40], height: 4 }, { kind: "box", size: [6, 40], height: 10, at: [17, 0, 0], material: 4 },
    ...[0, 1, 2, 3, 4].map((i) => ({ kind: "box", size: [2, 12], height: 5 + 2 * i, at: [-16 + 2 * i, 0, 0], material: 3 }))] });
  const gs = grids(st).filter((g) => g.frame.new);
  const f = facePatches(gs, { step: 1, only: [3] });
  assert.ok(f.some((p) => p.kind === "shade" && p.facing === "e"), JSON.stringify(f.map((p) => [p.kind, p.facing])));
  assert.ok(f.some((p) => p.kind === "steep" && p.facing === "w" && p.slope! >= 30), JSON.stringify(f));
  assert.ok(f.every((p) => p.material === 3));
  assert.equal(facePatches(gs, { step: 1, only: [3], kinds: ["shade"] }).some((p) => p.kind === "steep"), false);
});

// ------------------------------------------------------------------ haze

test("set_water_look haze: its own call in the block, read back; without it the block is the old text", () => {
  const sea = { albedo: [255, 250, 180], opacity: 1, reflection: 0, foam: 0 };
  const old = waterLookBlock({ NearOpacity: 1 }, sea);
  assert.equal(waterLookBlock({ NearOpacity: 1 }, sea, null), old);
  const h = hazeLook({ color: "#e8d9a0", amount: 0.8 });
  assert.deepEqual(h.haze, { color: [232, 217, 160], amount: 0.8, start: 0, distance: 0 });
  const text = withWaterLook("-- script\n", { NearOpacity: 1 }, sea, h.haze);
  assert.match(text, /wum\.level\.water\{Haze = \{232, 217, 160, 0\.8\}, HazeRange = \{0, 0\}\}/);
  assert.deepEqual(readHaze(text), h.haze);
  assert.deepEqual(readSea(text), sea);
  assert.ok(text.startsWith(old.replace(/-- erg-mcp set_water_look: end$/, "")));
  // a haze alone is a block; taken off, the block is the old one again
  assert.match(withWaterLook("", null, null, h.haze), /Haze = /);
  assert.equal(withWaterLook(text, { NearOpacity: 1 }, sea, null), old + "\n-- script\n");
  assert.deepEqual(hazeLook({ color: [1, 2] }).problems, ["haze.color: [r, g, b] 0-255 or \"#rrggbb\""]);
  assert.match(hazeLook({ color: [1, 2, 3], amount: 2 }).problems[0], /amount/);
});

test("test's mesh banks: the files are copied into the project's meshes folder and a mod with no bank there is named", async () => {
  const { testMeshBanks } = await import("../src/tools/place");
  const dir = mkdtempSync(join(tmpdir(), "ergmcp-banks-"));
  try {
    const st = newStore();
    const proj = join(dir, "projects", "t");
    writeFileSync(join(dir, "dc.Rocks.xom"), "bank");
    const s = { conn: { build: "0.15.0" }, levels: async () => ({ list: async () => ({ projectsDir: join(dir, "projects") }) }) } as unknown as Session;
    // no mod mesh placed, none given: nothing to say
    assert.equal(await testMeshBanks(s, "t", st), undefined);
    // the project's folder is not there: a file cannot be copied
    await assert.rejects(testMeshBanks(s, "t", st, [join(dir, "dc.Rocks.xom")]), /is not on this machine/);
    const { mkdirSync } = await import("node:fs");
    mkdirSync(proj, { recursive: true });
    const r: any = await testMeshBanks(s, "t", st, [join(dir, "dc.Rocks.xom")]);
    assert.deepEqual([r.banks, r.copied, r.warning], [["dc.Rocks.xom"], ["dc.Rocks.xom"], undefined]);
    assert.equal(readFileSync(join(proj, "meshes", "dc.Rocks.xom"), "utf8"), "bank");
    // the same file again is not copied twice
    assert.equal(((await testMeshBanks(s, "t", st, [join(dir, "dc.Rocks.xom")])) as any).copied, undefined);
    await assert.rejects(testMeshBanks(s, "t", st, [join(dir, "nope.xom")]), /no \.xom file/);
  } finally { rmSync(dir, { recursive: true, force: true }); }
});

// ------------------------------------------------------------------ the harness: crater looks, the rope's plan

test("pixelReads: the middle of a picture, pixel by pixel; a lilac floor in a sand frame is told", () => {
  const w = 20, h = 20, rgb = new Uint8Array(w * h * 3);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const inside = x >= 8 && x < 12 && y >= 7 && y < 13, c = inside ? [226, 222, 244] : [214, 190, 140];
    rgb.set(c, (y * w + x) * 3);
  }
  const r = pixelReads(rgb, w, h, 0.3);
  assert.equal(r.pixels, 36);
  assert.ok(r.share.blue > 0.6 && r.share.earth < 0.4 && r.share.white === 0, JSON.stringify(r.share));
  assert.deepEqual(pixelReads(new Uint8Array(w * h * 3).fill(120), w, h).share, { earth: 1, blue: 0, white: 0 });
});

test("game_use_tool takes a rope plan and game_crater_look its points; both refuse without a game", async () => {
  const { tools: moveTools } = await import("../src/tools/moves");
  const use = moveTools.find((t) => t.name === "game_use_tool")!, look = moveTools.find((t) => t.name === "game_crater_look")!;
  assert.equal(validate(use.inputSchema as never, { weapon: "rope", target: [0, 18, 0], rope: { aimSec: 1, refire: 1, steps: [{ do: "shorten", sec: 3, until: "still" }, { do: "swingForward", sec: 0.8 }, { do: "jump" }] } }), null);
  assert.match(validate(use.inputSchema as never, { weapon: "rope", target: [0, 18, 0], rope: { steps: [{ do: "climb" }] } })!, /must be one of shorten/);
  assert.equal(validate(look.inputSchema as never, { points: [[1, 2, 3]], height: 9, flag: ["blue"] }), null);
  // a server with no input.drive: the rope says what it needs
  const s = { ensure: async () => ({ methods: [] }) } as unknown as Session;
  await assert.rejects(Promise.resolve().then(() => use.handler(s, { weapon: "rope", target: [0, 18, 0] } as never)), /needs the game's input\.drive/);
  // teleport and girder are still refused, and say why
  await assert.rejects(Promise.resolve().then(() => use.handler(s, { weapon: "teleport", target: [0, 18, 0] } as never)), /cannot drive the teleport yet/);
});
