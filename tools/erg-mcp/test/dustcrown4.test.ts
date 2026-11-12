// Dustcrown round 4's builder gaps on the synthetic 12-frame level: build_mesa's ledgeAt, avoidBand and floorLattice.
// Every new setting is off by default: the old calls give the old voxels.
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, isSolid, material, maskOf, secondOf, type Loaded, type Scene } from "../src/core";
import { applyDrape, planDrape } from "../src/lib/heightfield-drape";
import { apply, planFit } from "../src/lib/heightfield-plan";
import { IDENTITY, applyGrade, fitGrade, gradeMaterial, parseGrade } from "../src/lib/grade";
import { colourDistance, facesOf, nearestMaterials } from "../src/lib/materials";
import { assignSections, compareSets, freeBuiltSections, loadOrder, type BankIn, type ModIn } from "../src/lib/meshbank-lint";
import { grids } from "../src/lib/voxel";
import { validate } from "../src/registry";
import { doorway, flatTunnel, tunnel } from "../src/lib/structures-shapes";
import { tools as heightTools } from "../src/tools/heightline";
import { tools as inspectTools } from "../src/tools/inspect";
import { tools as mesaTools } from "../src/tools/mesa";
import { tools as placeTools } from "../src/tools/place";
import { tools as renderTools } from "../src/tools/render";
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

// ------------------------------------------------------------------ materials: the sides' colours

/** A session whose server answers level.materials with tops and sides: 0 grass (rock sides), 1 rock, 2 sandstone, 3 sand. */
function matSession(store: EditorStore, sides = true) {
  const names = ["grass01", "rock01", "sandstone", "sand01"];
  const colors = ["#4a8a30", "#808080", "#d8b070", "#e8d090"], sideColors = ["#808080", "#808080", "#c8a060", "#e0c888"];
  const client = { call: async (m: string) => {
    if (m !== "level.materials") throw new Error(`unexpected ${m}`);
    return { file: "Maps\\t.txt", names, colors, resolved: [true, true, true, true], ...(sides ? { sideColors, sideResolved: [true, true, true, true] } : {}) };
  } };
  store.scene.databank.materialFile = "Maps\\t.txt";
  return sessionOf(store, { project: { store }, client: async () => client, levels: async () => ({ themes: async () => ({ themes: [], materialFiles: ["Maps\\t.txt"], timesOfDay: [] }) }) });
}

test("colourDistance, facesOf and nearestMaterials: a wall of grass is rock; the sandstones come first for a sand colour", () => {
  assert.equal(colourDistance([10, 20, 30], [10, 20, 30]), 0);
  assert.ok(colourDistance([216, 176, 112], [200, 160, 96]) < 60 && colourDistance([216, 176, 112], [128, 128, 128]) > 100);
  const rgb = (h: string) => [1, 3, 5].map((i) => parseInt(h.slice(i, i + 2), 16)) as [number, number, number];
  const set = { file: "f", names: ["grass01", "rock01", "sandstone", "sand01"], colors: ["#4a8a30", "#808080", "#d8b070", "#e8d090"].map(rgb), resolved: [true, true, true, true],
    side: ["#808080", "#808080", "#c8a060", "#e0c888"].map(rgb), sideResolved: [true, true, true, false] };
  assert.equal(facesOf(set, 0).sideDiffers, true);
  assert.equal(facesOf(set, 2).sideDiffers, false);
  assert.equal(facesOf({ ...set, side: null }, 0).sideDiffers, false);
  assert.deepEqual(nearestMaterials(set, rgb("#d0a868"), "side", 2).map((x) => x.name), ["sandstone", "grass01"], "sand01's side texture was not found: left out");
  assert.deepEqual(nearestMaterials(set, rgb("#d0a868"), "top", 2).map((x) => x.name), ["sandstone", "sand01"]);
  assert.deepEqual(nearestMaterials({ ...set, side: null }, rgb("#d0a868"), "side"), []);
});

test("list_materials: sideColour and sideDiffers per material, `like` finds a family; a server without sides answers as before", async () => {
  const r = await callTool(inspectTools, "list_materials", matSession(newStore()), { only: "materials", like: "#d0a868" });
  const list = r.materials.list;
  assert.equal(list[0].sideColour, "#808080");
  assert.equal(list[0].sideDiffers, true);
  assert.equal(list[2].sideDiffers, undefined);
  assert.equal(r.materials.nearest.face, "side");
  assert.deepEqual(r.materials.nearest.materials.slice(0, 2).map((x: any) => x.index), [2, 3]);
  const old = await callTool(inspectTools, "list_materials", matSession(newStore(), false), { only: "materials" });
  assert.equal(old.materials.list[0].sideColour, undefined);
  assert.equal(old.materials.sides, undefined);
  assert.equal(old.materials.nearest, undefined);
  await assert.rejects(callTool(inspectTools, "list_materials", matSession(newStore()), { only: "materials", like: "sand" }), /#rrggbb/);
});

test("build_mesa cliffLook: the bands' side colours, and a warning for a band whose sides are another material", async () => {
  const bands = { strata: { step: 3, bands: [{ material: 0, thickness: 3 }, { material: 2, thickness: 3 }] } };
  const r = await callTool(mesaTools, "build_mesa", matSession(newStore()), { ...MESA, ...bands, costOnly: true });
  assert.deepEqual(r.cliffLook.materials.map((m: any) => [m.material, m.side, !!m.sideDiffers]), [[0, "#808080", true], [2, "#c8a060", false]]);
  assert.match(r.cliffLook.warning, /0 \(grass01\) has sides/);
  const ok = await callTool(mesaTools, "build_mesa", matSession(newStore()), { ...MESA, strata: { step: 3, bands: [{ material: 2, thickness: 3 }] }, costOnly: true });
  assert.equal(ok.cliffLook.warning, undefined);
  // no side colours from the server, or no bands: no cliffLook
  assert.equal((await callTool(mesaTools, "build_mesa", matSession(newStore(), false), { ...MESA, ...bands, costOnly: true })).cliffLook, undefined);
  assert.equal((await callTool(mesaTools, "build_mesa", sessionOf(newStore()), { ...MESA, costOnly: true })).cliffLook, undefined);
});

// ------------------------------------------------------------------ look grade, side textures in render_view

test("fitGrade: recovers a gain and a saturation change from pairs; few pairs stay near the identity; parseGrade", () => {
  // the "game": an affine map in linear light
  const lin = (v: number) => { const c = v / 255; return c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4; };
  const srgb = (v: number) => { const c = Math.max(0, Math.min(1, v)); return Math.round(255 * (c <= 0.0031308 ? c * 12.92 : 1.055 * c ** (1 / 2.4) - 0.055)); };
  const game = (c: number[]) => [srgb(0.8 * lin(c[0]) + 0.02), srgb(1.1 * lin(c[1])), srgb(0.7 * lin(c[2]))] as [number, number, number];
  const from: [number, number, number][] = [[216, 173, 106], [232, 208, 144], [128, 128, 128], [74, 138, 48], [60, 60, 90], [200, 80, 60], [150, 170, 160], [30, 30, 30]];
  const g = fitGrade(from.map((c) => ({ from: c, to: game(c) })), { ridge: 1e-6, source: "test" });
  assert.ok(g.residual < 2, `residual ${g.residual}`);
  assert.equal(g.saturation, 1);
  assert.deepEqual(g.gain!.map((v) => Math.round(v * 10) / 10), [0.8, 1.1, 0.7]);
  // a saturation boost is found as one: grey stays grey, an ochre loses blue
  const satUp = (c: number[]) => { const l = c.map(lin), L = 0.2126 * l[0] + 0.7152 * l[1] + 0.0722 * l[2]; return l.map((v) => srgb(L + 1.3 * (v - L))) as [number, number, number]; };
  const gs = fitGrade(from.map((c) => ({ from: c, to: satUp(c) })), { ridge: 1e-6 });
  assert.ok(Math.abs(gs.saturation! - 1.3) < 0.06 && gs.residual < 3, JSON.stringify(gs));
  assert.deepEqual(applyGrade(gs, [128, 128, 128]).map((v) => Math.abs(v - 128) <= 2), [true, true, true]);
  assert.ok(applyGrade(gs, [216, 173, 106])[2] < 100);
  // a weightless pair does not pull the fit
  const odd = fitGrade([...from.map((c) => ({ from: c, to: satUp(c) })), { from: [10, 200, 30] as [number, number, number], to: [255, 0, 255] as [number, number, number], weight: 0 }], { ridge: 1e-6 });
  assert.ok(Math.abs(odd.saturation! - 1.3) < 0.06);
  // gradeMaterial: a measured material still in its colour takes the game's colour; a changed one the fit (the tinted fit)
  const gm = { ...g, measured: [{ material: 9, from: [216, 173, 106] as [number, number, number], to: [212, 176, 62] as [number, number, number], tinted: true }], tint: { m: [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 0.5, 0]], pairs: 2, residual: 1 } };
  assert.deepEqual(gradeMaterial(gm, 9, [217, 172, 106]), [212, 176, 62]);
  assert.deepEqual(gradeMaterial(gm, 9, [150, 120, 90]), applyGrade(g, [150, 120, 90]));
  assert.deepEqual(gradeMaterial(gm, 9, [150, 120, 90], true), applyGrade({ ...g, m: gm.tint.m }, [150, 120, 90]));
  assert.deepEqual(gradeMaterial(gm, 4, [216, 173, 106]), applyGrade(g, [216, 173, 106]));
  assert.equal(parseGrade(JSON.stringify({ grade: gm })).measured!.length, 1);
  const q = applyGrade(g, [180, 150, 90]), want = game([180, 150, 90]);
  assert.ok(q.every((v, i) => Math.abs(v - want[i]) <= 3), `${q} vs ${want}`);
  assert.deepEqual(applyGrade(IDENTITY, [12, 200, 77]), [12, 200, 77]);
  // two near-identical pairs cannot make a wild map: a far colour moves less than the pairs did
  const few = fitGrade([{ from: [200, 160, 100], to: [230, 150, 60] }, { from: [205, 165, 100], to: [234, 155, 62] }]);
  const far = applyGrade(few, [40, 60, 200]);
  assert.ok(Math.abs(far[2] - 200) < 60 && far.every((v) => v >= 0 && v <= 255), String(far));
  assert.throws(() => fitGrade([{ from: [1, 2, 3], to: [1, 2, 3] }]), /two/);
  assert.deepEqual(parseGrade(JSON.stringify({ grade: g })).m, g.m);
  assert.throws(() => parseGrade("{}"), /three rows/);
});

test("render_view grade and sideTextures: the picture changes only when asked; a missing grade is refused", async () => {
  const dir = mkdtempSync(join(tmpdir(), "erg-grade-"));
  try {
    const file = join(dir, "g.json");
    writeFileSync(file, JSON.stringify({ grade: { m: [[1.4, 0, 0, 0], [0, 0.8, 0, 0], [0, 0, 0.5, 0]], pairs: 3, residual: 1, source: "test" } }));
    const pic = async (s: Session, extra: Record<string, unknown>) => {
      const t = renderTools.find((x) => x.name === "render_view")!;
      assert.equal(validate(t.inputSchema as never, { width: 160, height: 90, markers: false, ...extra }), null);
      const r: any = await t.handler(s, { width: 160, height: 90, markers: false, ...extra } as never);
      return { png: createHash("sha1").update(Buffer.from(r.content[0].data, "base64")).digest("hex"), info: JSON.parse(r.content[1].text) };
    };
    const store = newStore();
    const plain = await pic(matSession(store), {}), again = await pic(matSession(store), {});
    assert.equal(plain.png, again.png);
    assert.equal(plain.info.grade, undefined);
    assert.equal(plain.info.sideTextures, undefined);
    const graded = await pic(matSession(store), { grade: file });
    assert.notEqual(graded.png, plain.png);
    assert.equal(graded.info.grade.pairs, 3);
    const sided = await pic(matSession(store), { sideTextures: true });
    assert.notEqual(sided.png, plain.png);
    assert.match(sided.info.sideTextures, /side texture/);
    // a server without side colours draws as before, and says so
    const old = await pic(matSession(store, false), { sideTextures: true }), oldPlain = await pic(matSession(store, false), {});
    assert.equal(old.png, oldPlain.png);
    assert.match(old.info.sideTextures, /no side colours/);
    await assert.rejects(pic(matSession(store), { grade: join(dir, "none.json") }), /neither a built-in grade/);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

// ------------------------------------------------------------------ lint_mesh_banks

test("assignSections: a bank keeps its built section while it is free, else the first free; past 44 nothing loads", () => {
  const bank = (mod: string, n: number, section: number | null): BankIn => ({ mod, file: `b${n}.xom`, section, meshes: [`${mod}.M${n}`] });
  // kindjal's 38 banks on 476..513, then a map's bank built on 486
  const kindjal = Array.from({ length: 38 }, (_, i) => bank("kindjal", i, 476 + i));
  const a = assignSections([...kindjal, bank("dustcrown", 0, 486)]);
  assert.equal(a.relocated, 1);
  const moved = a.banks.find((b) => b.mod === "dustcrown")!;
  assert.equal(moved.resolved, 514);
  assert.match(moved.why!, /section 486 is taken by kindjal\/b10\.xom/);
  assert.equal(a.used, 39);
  assert.equal(a.free, 5);
  // the other way round the map keeps 486, kindjal's eleventh bank takes the next free one (487), which is the
  // twelfth's, and so on: every bank from there moves up one
  const b = assignSections([bank("dustcrown", 0, 486), ...kindjal]);
  assert.equal(b.banks[0].resolved, 486);
  const shifted = b.banks.filter((x) => x.relocated);
  assert.equal(shifted.length, 28);
  assert.deepEqual([shifted[0].file, shifted[0].resolved, shifted[27].file, shifted[27].resolved], ["b10.xom", 487, "b37.xom", 514]);
  // built on a section nobody was built on: nothing moves either way
  assert.equal(assignSections([...kindjal, bank("dustcrown", 0, 515)]).relocated, 0);
  assert.deepEqual(freeBuiltSections([...kindjal, bank("dustcrown", 0, 486)]), [514, 515, 516, 517, 518, 519]);
  // the budget: the 45th bank does not load; an unreadable bank burns nothing; a section out of range is moved
  const many = assignSections(Array.from({ length: 46 }, (_, i) => bank("big", i, 476 + (i % 44))));
  assert.equal(many.notLoaded, 2);
  assert.equal(many.used, 44);
  const odd = assignSections([{ ...bank("m", 0, null), error: "the file is missing" }, bank("m", 1, 30), { ...bank("m", 2, 500), meshes: ["other.X"] }]);
  assert.deepEqual(odd.banks.map((x) => x.resolved), [null, 476, null]);
  assert.match(odd.banks[2].why!, /outside the mod's own names/);
  assert.deepEqual(assignSections([bank("a", 0, 480), { ...bank("b", 0, 481), meshes: ["b.M0"] }, { ...bank("b", 1, 482), meshes: ["b.M0"] }]).duplicateNames.map((d) => d.name), ["b.M0"]);
});

test("loadOrder: a mod after its dependencies, else by id; compareSets finds a bank that shifts with the plugin set", () => {
  const mod = (id: string, deps: string[], sections: number[]): ModIn => ({ id, dependencies: deps, banks: sections.map((s, i) => ({ mod: id, file: `${i}.xom`, section: s, meshes: [`${id}.M${i}`] })) });
  const kindjal = mod("kindjal", [], [476, 486]), sand = mod("sandtide", ["kindjal"], []), map = mod("foldspace", [], [486]), zed = mod("aaa", ["sandtide"], [476]);
  assert.deepEqual(loadOrder([sand, kindjal, map]).map((m) => m.id), ["foldspace", "kindjal", "sandtide"]);
  assert.deepEqual(loadOrder([zed, sand, kindjal]).map((m) => m.id), ["kindjal", "sandtide", "aaa"]);
  const sets = [[map], [map, kindjal, sand]].map((mods, i) => ({ name: `s${i}`, assignment: assignSections(loadOrder(mods).flatMap((m) => m.banks)) }));
  // foldspace loads first and keeps 486 in both; kindjal's second bank is the one that moves
  assert.deepEqual(compareSets(sets, "foldspace").map((c) => [c.sections, c.shifts]), [[{ s0: 486, s1: 486 }, false]]);
  assert.deepEqual(compareSets(sets, "kindjal").map((c) => [c.sections.s1, c.shifts]), [[476, false], [477, true]]);
  // a map whose id sorts after the other mod is the one that moves
  const late = mod("zmap", [], [486]);
  const sets2 = [[late], [late, kindjal]].map((mods, i) => ({ name: `s${i}`, assignment: assignSections(loadOrder(mods).flatMap((m) => m.banks)) }));
  assert.deepEqual(compareSets(sets2, "zmap").map((c) => [c.sections, c.shifts]), [[{ s0: 486, s1: 477 }, true]]);
});
