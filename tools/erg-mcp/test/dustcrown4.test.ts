// Dustcrown round 4's builder gaps on the synthetic 12-frame level: build_mesa's ledgeAt, avoidBand and floorLattice.
// Every new setting is off by default: the old calls give the old voxels.
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, isSolid, material, maskOf, secondOf, type Loaded, type Scene } from "../src/core";
import { applyDrape, planDrape } from "../src/lib/heightfield-drape";
import { apply, planFit } from "../src/lib/heightfield-plan";
import { grids } from "../src/lib/voxel";
import { validate } from "../src/registry";
import { doorway, flatTunnel, tunnel } from "../src/lib/structures-shapes";
import { tools as heightTools } from "../src/tools/heightline";
import { tools as mesaTools } from "../src/tools/mesa";
import { tools as placeTools } from "../src/tools/place";
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
/** The distinct tops (map y) of the added blocks' columns, lowest first. */
function newTops(store: EditorStore): number[] {
  const tops = new Set<number>();
  for (const g of grids(store)) {
    if (!g.frame.new) continue;
    const [X, Y, Z] = g.size, m = g.toMap;
    for (let z = 0; z < Z; z++) for (let x = 0; x < X; x++) {
      let top = -1;
      for (let y = 0; y < Y; y++) if (isSolid(g.words[(z * X + x) * Y + y])) top = y;
      if (top >= 0) tops.add(Math.round((m[5] * (top + 1) + m[7]) * 1000) / 1000);
    }
  }
  return [...tops].sort((p, q) => p - q);
}

// ------------------------------------------------------------------ build_mesa

const MESA = { at: [520, 0], floor: 0, top: 12, shape: { kind: "circle", radius: 20 }, tread: { width: 3, jitter: 0 }, strata: { step: 3 } };

test("build_mesa ledgeAt: the steps' tops are the ys given, one step each", async () => {
  const store = newStore();
  const r = await callTool(mesaTools, "build_mesa", sessionOf(store), { ...MESA, ledgeAt: [2, 7] });
  assert.equal(r.mesa.steps, 3);
  assert.deepEqual(r.mesa.ledgeAt, { asked: [2, 7], built: [2, 7] });
  assert.deepEqual(r.mesa.ledges.map((l: any) => l.y.map), [2, 7, 12]);
  assert.deepEqual(newTops(store), [2, 7, 12]);
  // refused: out of order, or closer than a voxel
  await assert.rejects(callTool(mesaTools, "build_mesa", sessionOf(newStore()), { ...MESA, ledgeAt: [7, 2] }), /less than a voxel/);
  await assert.rejects(callTool(mesaTools, "build_mesa", sessionOf(newStore()), { ...MESA, ledgeAt: [2, 11.5] }), /under top/);
});

test("build_mesa avoidBand: no step's top inside the band; moved to the nearest level outside, or left out", async () => {
  // steps at 3, 6, 9 and the plateau at 12; the band 5-7 holds the step at 6
  const store = newStore();
  const r = await callTool(mesaTools, "build_mesa", sessionOf(store), { ...MESA, avoidBand: { y: 6, band: 1 } });
  assert.deepEqual(r.mesa.avoidBand.moved, [{ from: 6, to: 5 }]);
  assert.deepEqual(newTops(store), [3, 5, 9, 12]);
  // a band that leaves no level between the neighbours: the step is left out
  const st2 = newStore();
  const r2 = await callTool(mesaTools, "build_mesa", sessionOf(st2), { ...MESA, avoidBand: { y: 6, band: 2.5 } });
  assert.deepEqual(r2.mesa.avoidBand.moved, [{ from: 6, to: null }]);
  assert.deepEqual(newTops(st2), [3, 9, 12]);
  // nothing inside: nothing moves, and without the setting the mesa is the old one
  const st3 = newStore(), st4 = newStore();
  const r3 = await callTool(mesaTools, "build_mesa", sessionOf(st3), { ...MESA, avoidBand: { y: 7.5, band: 1 } });
  assert.deepEqual(r3.mesa.avoidBand.moved, []);
  const r4 = await callTool(mesaTools, "build_mesa", sessionOf(st4), MESA);
  assert.equal(r4.mesa.avoidBand, undefined);
  assert.equal(r4.mesa.ledgeAt, undefined);
  assert.deepEqual(newTops(st3), newTops(st4));
  assert.deepEqual(newTops(st4), [3, 6, 9, 12]);
  // the plateau inside the band is the caller's: a warning
  const r5 = await callTool(mesaTools, "build_mesa", sessionOf(newStore()), { ...MESA, avoidBand: { y: 12.5, band: 1 }, costOnly: true });
  assert.match(r5.mesa.avoidBand.warning, /plateau/);
});

test("build_mesa floorLattice: ledges are floor + whole voxels, off the Scene's 2.5 lattice", async () => {
  const base = { at: [520, 0], top: 14.2, shape: { kind: "circle", radius: 30 }, voxelSize: 2.5, tread: { width: 6, jitter: 0 }, strata: { step: 1 } };
  // on the Scene's lattice (the default) a floor of 1.7 puts every top on a multiple of 2.5
  const st0 = newStore();
  const r0 = await callTool(mesaTools, "build_mesa", sessionOf(st0), { ...base, floor: 1.7 });
  assert.ok(newTops(st0).every((y) => Math.abs(y / 2.5 - Math.round(y / 2.5)) < 1e-6), JSON.stringify(newTops(st0)));
  assert.equal(r0.mesa.lattice, undefined);
  // with floorLattice they are 1.7 + whole voxels, and top (1.7 + 5 x 2.5) is built exactly
  const st1 = newStore();
  const r1 = await callTool(mesaTools, "build_mesa", sessionOf(st1), { ...base, floor: 1.7, floorLattice: true });
  assert.equal(r1.mesa.lattice.y, "floor");
  assert.deepEqual(newTops(st1), [4.2, 6.7, 9.2, 11.7, 14.2]);
  assert.deepEqual(r1.mesa.ledges.map((l: any) => l.y.map), [4.2, 6.7, 9.2, 11.7, 14.2]);
  assert.deepEqual(st1.problems(), []);
  // ledgeAt on the floor's lattice is built as asked
  const st2 = newStore();
  const r2 = await callTool(mesaTools, "build_mesa", sessionOf(st2), { ...base, floor: 1.7, floorLattice: true, ledgeAt: [4.2, 11.7] });
  assert.deepEqual(r2.mesa.ledgeAt.built, [4.2, 11.7]);
  assert.deepEqual(newTops(st2), [4.2, 11.7, 14.2]);
});

// ------------------------------------------------------------------ maskLets

test("maskLets: a mask names the height expression's lets and calls its fns; off by default", () => {
  const expr = "let ridge = 6 * exp(-(x / 6)^2); fn lift(k) = k * ridge; 4 + lift(1)";
  const args = { min: [500, -20] as [number, number], max: [540, 20] as [number, number], floor: 0, expr, material: 1 };
  const mats = (store: EditorStore) => {
    const n = new Map<number, number>();
    for (const g of grids(store)) if (g.frame.new) for (const w of g.words) if (isSolid(w)) n.set(material(w), (n.get(material(w)) ?? 0) + 1);
    return n;
  };
  // the crest (ridge over 4) in material 7, by the let; and the same by a fn call
  const st = newStore();
  apply(st, planFit(st, { ...args, maskLets: true, materials: [{ material: 7, mask: "ridge - 4" }, { material: 9, mask: "lift(2) - 6" }] }));
  const m = mats(st);
  assert.ok((m.get(7) ?? 0) > 0 && (m.get(9) ?? 0) > 0 && (m.get(1) ?? 0) > 0, JSON.stringify([...m]));
  // the same crest written out in full, without maskLets, is the same voxels
  const st2 = newStore();
  apply(st2, planFit(st2, { ...args, materials: [{ material: 7, mask: "6 * exp(-(x / 6)^2) - 4" }, { material: 9, mask: "2 * 6 * exp(-(x / 6)^2) - 6" }] }));
  assert.deepEqual([...mats(st2)].sort(), [...m].sort());
  // without maskLets a mask does not know the name (as before); a mask binding a name of expr's is refused
  assert.throws(() => planFit(newStore(), { ...args, materials: [{ material: 7, mask: "ridge - 4" }] }), /materials\[0\]\.mask: .*ridge/);
  assert.throws(() => planFit(newStore(), { ...args, maskLets: true, materials: [{ material: 7, mask: "let ridge = 1; ridge" }] }), /materials\[0\]\.mask: .*already a name.*at character 5/);
  assert.throws(() => planFit(newStore(), { ...args, expr: undefined, grid: [[1, 1], [1, 1]], maskLets: true, materials: [] } as never), /maskLets/);
});

// ------------------------------------------------------------------ build_castle gates[].bevel, dig_tunnel caps

test("doorway bevel: a canted head, the two top corners cut at 45 degrees; none by default", () => {
  const plain = doorway(10, 10, 2, "s", 6, 6, 0, 0, 0, true), cant = doorway(10, 10, 2, "s", 6, 6, 0, 0, 0, true, 2);
  // inside the wall (z 9.5), along the opening's top row (y 5.5) and one under it
  const row = (sh: typeof plain, y: number) => [-2.5, -1.5, -0.5, 0.5, 1.5, 2.5].map((x) => (sh.hit(x, y, 9.5) ? "#" : ".")).join("");
  assert.equal(row(plain, 5.5), "######");
  assert.equal(row(cant, 5.5), ".####.");
  assert.equal(row(cant, 4.5), "######");
  assert.equal(row(cant, 0.5), "######");
  // a round head is the default for a wide gate; the bevel replaces it
  const round = doorway(10, 10, 2, "s", 6, 6);
  assert.equal(row(round, 5.5), ".####.".replace(/./g, (c, i) => (round.hit([-2.5, -1.5, -0.5, 0.5, 1.5, 2.5][i], 5.5, 9.5) ? "#" : ".")));
  assert.notEqual(row(doorway(10, 10, 2, "s", 6, 6, 0, 0, 0, false, 1), 5.5), row(doorway(10, 10, 2, "s", 6, 6, 0, 0, 0, false, 3), 5.5));
});

test("build_castle gates[].bevel reaches the gate and is answered; without it the castle is the old one", async () => {
  const args = { at: [500, 12, 500], size: [24, 24], wallHeight: 8, towerRadius: 3, keep: false, material: 6 };
  const words = (st: EditorStore) => createHash("sha1").update(grids(st).map((g) => `${g.id}:${Array.from(g.words).join(",")}`).join("|")).digest("hex");
  const a = newStore(), b = newStore(), c = newStore();
  const ra = await callTool(structureTools, "build_castle", sessionOf(a), { ...args, gates: [{ side: "s", width: 6, height: 5 }] });
  const rb = await callTool(structureTools, "build_castle", sessionOf(b), { ...args, gates: [{ side: "s", width: 6, height: 5, bevel: 3 }] });
  await callTool(structureTools, "build_castle", sessionOf(c), { ...args, gates: [{ side: "s", width: 6, height: 5, bevel: 0 }] });
  assert.equal(rb.structureInfo.gates[0].bevel, 3);
  assert.equal(ra.structureInfo.gates[0].bevel, undefined);
  assert.notEqual(words(a), words(b));
  assert.equal(words(a), words(c));
});

test("tunnel caps flat: the bore stops at the plane through its ends; round (default) runs a radius past", () => {
  const pts: [number, number, number][] = [[0, 5, 0], [10, 5, 0]];
  const hitAny = (shapes: ReturnType<typeof tunnel>, x: number) => shapes.some((sh) => sh.op === "carve" && sh.hit(x, 5, 0));
  const round = tunnel(pts, 2, "round", null), flat = tunnel(pts, 2, "round", null, "flat"), sq = tunnel(pts, 2, "square", null, "flat");
  for (const [x, r, f] of [[-1.5, true, false], [-0.5, true, false], [0.5, true, true], [9.5, true, true], [10.5, true, false], [12.5, false, false]] as const) {
    assert.equal(hitAny(round, x), r, `round at ${x}`);
    assert.equal(hitAny(flat, x), f, `flat at ${x}`);
    assert.equal(hitAny(sq, x), f, `square flat at ${x}`);
  }
  // a bend in the middle stays joined: only the first and last ends are cut
  const bent = tunnel([[0, 5, 0], [10, 5, 0], [10, 5, 10]], 2, "round", null, "flat");
  assert.ok(bent.some((sh) => sh.hit(11, 5, -1)), "the outside of the bend is bored");
  assert.ok(!bent.some((sh) => sh.hit(-1, 5, 0)) && !bent.some((sh) => sh.hit(10, 5, 11)));
  // the flat-floored tunnel too
  const ft = (caps?: "flat") => flatTunnel({ points: pts, radius: 2, shape: "square", headroom: 3, floor: 0, material: 1, lining: null, caps });
  assert.ok(ft().some((sh) => sh.hit(-1, 6, 0)));
  assert.ok(!ft("flat").some((sh) => sh.hit(-1, 6, 0)) && ft("flat").some((sh) => sh.hit(1, 6, 0)));
});

test("dig_tunnel: a bore that fills nothing names no material; a lined one does", async () => {
  const plateau = async () => {
    const st = newStore();
    await callTool(structureTools, "build_tower", sessionOf(st), { at: [500, 0, 500], shape: "square", radius: 14, height: 8, crenellations: false, material: 6 });
    return st;
  };
  const bare = await callTool(structureTools, "dig_tunnel", sessionOf(await plateau()), { points: [[490, 3, 490], [510, 3, 510]], radius: 1.5, caps: "flat" });
  assert.equal(bare.material, undefined);
  assert.equal(bare.materialNote, undefined);
  assert.equal(bare.structureInfo.caps, "flat");
  assert.ok(bare.voxels.removed > 0);
  const lined = await callTool(structureTools, "dig_tunnel", sessionOf(await plateau()), { points: [[490, 3, 490], [510, 3, 510]], radius: 1.5, liningMaterial: 9 });
  assert.equal(typeof lined.material, "number");
  // a structure that fills keeps its material and the note
  const wall = await callTool(structureTools, "build_wall", sessionOf(newStore()), { points: [[490, 12, 490], [510, 12, 490]], height: 4 });
  assert.equal(typeof wall.material, "number");
  assert.match(wall.materialNote, /chosen/);
});

// ------------------------------------------------------------------ check_height_line design, spawn positions, walks

test("check_height_line design: the distance to the design line beside the land-box line", async () => {
  const store = newStore(), s = sessionOf(store, { project: { store } });
  const base = await callTool(heightTools, "check_height_line", s, { preset: "sandtide" });
  assert.equal(base.design, undefined);
  assert.equal(base.spawns.list[0]?.designMargin, undefined);
  const line = base.line.map;
  const r = await callTool(heightTools, "check_height_line", s, { preset: "sandtide", design: line - 3 });
  assert.equal(r.line.map, line);
  assert.equal(r.design.line.map, Math.round((line - 3) * 100) / 100);
  assert.equal(r.design.apart.map, 3);
  assert.equal(r.design.withinBand, false);
  for (const sp of r.spawns.list) assert.ok(Math.abs(sp.designMargin.map - sp.margin.map - 3) < 0.011, JSON.stringify(sp));
  for (const g of r.surfaces.nearGroups) assert.ok(g.toLine && g.toDesign, JSON.stringify(g));
  // a surface within the band of the design line fails ok even when it is clear of the land-box line
  const lo = await callTool(heightTools, "check_height_line", s, { y: 1000, design: base.surfaces.lowest.map });
  assert.equal(lo.ok, false);
  assert.ok(lo.design.surfacesNear.columns > 0);
  assert.ok(lo.surfaces.nearGroups.every((g: any) => g.toDesign.map <= 2 && g.toLine.map < 0));
  // in world units the design is world units
  const w = await callTool(heightTools, "check_height_line", s, { preset: "sandtide", design: (line - 3) * 20, units: "world" });
  assert.equal(w.design.apart.world, 60);
});

test("place_spawn_knots answers positions by knot name, changed or not", async () => {
  const store = newStore(), s = sessionOf(store, { ensure: async () => ({ methods: [] }), client: async () => ({ call: async () => { throw new Error("no game"); } }) });
  const args = { mode: "knots", units: "map", spawns: [{ worm: 0, pos: [500, 30, 500] }, { worm: 1, pos: [504, 30, 500] }] };
  const r = await callTool(placeTools, "place_spawn_knots", s, args);
  assert.deepEqual(Object.keys(r.positions).filter((k) => k === "WORM0" || k === "WORM1").sort(), ["WORM0", "WORM1"]);
  assert.deepEqual(r.positions.WORM0, r.placed.find((p: any) => p.worm === 0).pos);
  // a call that only reads the mode back still answers them
  const again = await callTool(placeTools, "place_spawn_knots", s, { mode: "knots", units: "map" });
  assert.equal(again.placed, undefined);
  assert.deepEqual(again.positions.WORM1, r.positions.WORM1);
});

// ------------------------------------------------------------------ heightfield_drape coast

test("drape coast: the columns that dip to the sea take rimMaterial, before the other rules; blendToSea lends corners", () => {
  // a cone that falls under y 2 at its edge: the sea at 2
  const args = { min: [500, -20] as [number, number], max: [540, 20] as [number, number], floor: 0, expr: "max(0, 14 - 0.8 * r)", material: 1, thickness: 2, core: false,
    steepMaterial: { material: 5, minSlope: 30 } };
  const words = (store: EditorStore) => grids(store).filter((g) => g.frame.new).flatMap((g) => Array.from(g.words).filter(isSolid));
  const st0 = newStore(), p0 = planDrape(st0, args);
  applyDrape(st0, p0);
  assert.equal((p0.report as any).coast, undefined);
  assert.ok(!words(st0).some((w) => material(w) === 7));
  const st1 = newStore(), p1 = planDrape(st1, { ...args, coast: { seaLevel: 2, rimMaterial: 7, band: 1 } });
  applyDrape(st1, p1);
  const c1 = (p1.report as any).coast;
  assert.ok(c1.columns > 20, JSON.stringify(c1));
  assert.equal(c1.blendedEdge, undefined);
  const w1 = words(st1);
  assert.equal(w1.filter((w) => material(w) === 7).length, c1.columns * 2, "every skin voxel of a rim column");
  assert.ok(w1.every((w) => secondOf(w) !== 7));
  // the rim wins over the steep rule there; inland the steep rule still holds
  assert.ok(w1.some((w) => material(w) === 5) && w1.filter((w) => material(w) === 5).length < words(st0).filter((w) => material(w) === 5).length);
  // blendToSea: the same rim, and its inland neighbours carry it as a second material on some corners
  const st2 = newStore(), p2 = planDrape(st2, { ...args, coast: { seaLevel: 2, rimMaterial: 7, band: 1, blendToSea: true } });
  applyDrape(st2, p2);
  const c2 = (p2.report as any).coast, w2 = words(st2);
  assert.equal(c2.columns, c1.columns);
  assert.ok(c2.blendedEdge > 8, JSON.stringify(c2));
  const lent = w2.filter((w) => material(w) !== 7 && secondOf(w) === 7);
  assert.equal(lent.length, c2.blendedEdge * 2);
  assert.ok(lent.every((w) => maskOf(w) !== 0 && maskOf(w) !== 0xff));
  // a sea under all the skin: no rim, and the voxels are the plain drape's
  const st3 = newStore(), p3 = planDrape(st3, { ...args, coast: { seaLevel: -50, rimMaterial: 7 } });
  applyDrape(st3, p3);
  assert.equal((p3.report as any).coast.columns, 0);
  assert.deepEqual(words(st3), words(st0));
  assert.throws(() => planDrape(newStore(), { ...args, coast: { seaLevel: 2, rimMaterial: 99 } }), /rimMaterial/);
});
