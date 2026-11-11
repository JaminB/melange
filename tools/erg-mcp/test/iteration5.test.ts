// The Caldera iteration-5 gaps, on the synthetic 12-frame level (its root renamed Scene so blocks can be added), away
// from its own frames (x 480-600): the mine factory's placement checks and the game probe of its entity, check_flood's
// units, own moves and hard warnings, the base backdrop's meshes and the water that covers them, feather patterns with a
// cell size and path masks, spawn balancing, castle gates and seat, query_ground's openings, several targets and the
// skin slope mode of check_reachability, and the drape's slope limiter and skirt estimates. Bundled for Node by test.ps1.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, type Loaded, type Scene } from "../src/core";
import { coverLevel, glbBounds, type BackdropPiece } from "../src/lib/backdrop";
import { checkFactory } from "../src/lib/factory-check";
import { applyDrape, planDrape } from "../src/lib/heightfield-drape";
import { apply, limitSlopes, pathPoints, plan, sampleField } from "../src/lib/heightfield-plan";
import { spansAt } from "../src/lib/inspect-field";
import { grids } from "../src/lib/voxel";
import { validate } from "../src/registry";
import { tools as drapeTools } from "../src/tools/drape";
import { tools as gameTools } from "../src/tools/game";
import { openings, tools as inspectTools } from "../src/tools/inspect";
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
function session(extra: Record<string, unknown> = {}, script = "") {
  const store = new EditorStore("t", loaded(), loaded());
  const project = { store, id: "t", info: {}, erg: { script: async () => script } };
  const s = { requireProject: () => project, get store() { return store; }, project,
    levels: async () => { throw new Error("no server"); }, client: async () => { throw new Error("no server"); }, ...extra } as unknown as Session;
  return { store, s, project };
}
const all = [...inspectTools, ...structureTools, ...placeTools, ...gameTools, ...drapeTools];
const call = async (s: Session, name: string, args: Record<string, unknown>): Promise<any> => {
  const t = all.find((x) => x.name === name)!;
  const bad = validate(t.inputSchema, args);
  if (bad) throw new Error(bad);
  const r = await t.handler(s, args);
  return typeof r === "string" ? JSON.parse(r) : r;
};
const CONE = { min: [480, -30] as [number, number], max: [560, 30] as [number, number], floor: 0, expr: "max(0, 30 - abs(x)*1.2)", voxelSize: 2, thickness: 2, material: 1 };

test("the mine factory: a knot over a draped skin, out of reach or under the water is an error; on a block a warning", async () => {
  const { store, s } = session();
  apply(store, plan(store, { min: [480, -10], max: [520, 10], floor: 0, expr: "4", material: 1 }));
  applyDrape(store, planDrape(store, { ...CONE, min: [530, -10], max: [590, 10], expr: "max(2, 12 - abs(x)*0.3)", core: false }));
  const len = (v: number) => `${v} map`;
  const placed = await call(s, "place_object", { type: "minefactory", units: "map", xz: [500, 0] });
  assert.ok(!placed.factoryWillBeLost, JSON.stringify(placed.issues));
  assert.ok(placed.issues.some((l: string) => /warning: .*game_mines \{alive: true\}/.test(l)));
  const c = checkFactory(store, len)!;
  assert.equal(c.lands?.draped, false);
  assert.ok(Math.abs(c.lands!.y - 4) < 1e-6);
  // on the skin
  const id = placed.placed[0].id;
  const moved = await call(s, "move", { id, pos: [560, 30, 0], units: "map", drop: true });
  assert.ok(moved.factory.some((l: string) => /^error: .*draped skin/.test(l)), JSON.stringify(moved.factory));
  // under the script's water: the cap is over the block it lands on
  await call(s, "move", { id, pos: [500, 4, 0], units: "map" });
  assert.ok(checkFactory(store, len, 6)!.problems.some((p) => /explodes when the water gets there/.test(p)));
  assert.deepEqual(checkFactory(store, len, 4.4)!.problems, [], "within half a map unit of its ground it survives");
  // nothing under it within the drop's 50 map units
  await call(s, "move", { id, pos: [500, 60, 0], units: "map" });
  assert.ok(checkFactory(store, len)!.problems.some((p) => /no voxel ground within/.test(p)));
});

/** A session whose game answers `answer(method, params)`. */
function gameSession(answer: (m: string, p: any) => unknown, methods: string[]) {
  const client = { call: async (m: string, p: any) => answer(m, p), subscribe: () => () => undefined };
  return session({ ensure: async () => ({ methods }), client: async () => client });
}

test("game_mines alive: the factory entity, where it settled (raw inspect) and how far from its knot; gone, why", async () => {
  const hex = (x: number, y: number, z: number) => {
    const b = new Uint8Array(0x90), v = new DataView(b.buffer);
    v.setFloat32(0x2c, x, true); v.setFloat32(0x30, y, true); v.setFloat32(0x34, z, true);
    return Array.from(b, (n) => n.toString(16).padStart(2, "0")).join("");
  };
  let alive = true;
  const g = gameSession((m) => {
    if (m === "entities.list") return alive ? [{ handle: 7, kind: "Other", type: "MineFactoryLogicEntity", pos: null }, { handle: 8, kind: "Other", type: "MineFactoryGraphicEntity", pos: null }] : [];
    if (m === "entity.inspect") return { hex: hex(211, -154, 2319.7) };
    return null;
  }, ["entities.list", "entity.inspect"]);
  apply(g.store, plan(g.store, { min: [480, -10], max: [520, 10], floor: 0, expr: "4", material: 1 }));
  await call(g.s, "place_object", { type: "minefactory", units: "map", xz: [500, 0] });
  const r = await call(g.s, "game_mines", { alive: true, units: "world" });
  assert.equal(r.factoryEntity.alive, true);
  assert.deepEqual(r.factoryEntity.at, [211, -154, 2319.7]);
  assert.ok(r.factoryEntity.fromKnot > 1000);
  assert.match(r.factoryEntity.warning, /far from its knot/);
  alive = false;
  const gone = await call(g.s, "game_mines", { alive: true });
  assert.equal(gone.factoryEntity.alive, false);
  assert.match(gone.factoryEntity.why, /MineFactory\.Deleted/);
});

test("check_flood: each spawn's flood in map and world units, its own moves, and hard warnings for the cap and early floods", async () => {
  const { store, s } = session();
  apply(store, plan(store, { min: [480, -12], max: [600, 12], floor: 0, expr: "2 + max(0, x + 20)*0.4", material: 1 }));
  // WORM0 low (ground 2), WORM1 high (ground about 30); the rest far up
  await call(s, "place_spawn_knots", { units: "map", spawns: [{ worm: 0, xz: [482, 0] }, { worm: 1, xz: [595, 0] }] });
  const r = await call(s, "check_flood", { units: "map", from: undefined, schedule: { startTurn: 2, step: 20, cap: 100, units: "world" }, start: 1, x0: 480, z0: -12, x1: 600, z1: 12,
    wormsPerTeam: 1, teams: 2 });
  const w0 = r.spawns.find((x: { name: string }) => x.name === "WORM0");
  assert.equal(w0.flood.floods.turn, 2);
  assert.deepEqual(w0.flood.floods.water, { map: 2, world: 40 });
  assert.deepEqual(w0.flood.floods.ground, { map: 2, world: 40 });
  assert.equal(w0.flood.moves.team, 0);
  assert.equal(w0.flood.moves.firstMove, 1);
  assert.equal(w0.flood.moves.every, 2);
  assert.equal(w0.flood.moves.ownMovesBeforeFlood, 1);
  assert.ok(r.flood.warnings.some((l: string) => /WORM0: the water's top 5 map \(100 world\) reaches over/.test(l)), JSON.stringify(r.flood.warnings));
  assert.ok(r.flood.warnings.some((l: string) => /WORM0: its ground floods in game turn 2, before its worm's 2nd move/.test(l)));
  assert.ok(!r.flood.warnings.some((l: string) => /^WORM1:/.test(l)), "the high spawn is safe");
  assert.match(r.flood.scheduleUnits, /^world/);
  assert.deepEqual(r.flood.highestWater, { map: 5, world: 100 });
});

test("backdrop: a GLB's bounds through its nodes, and the water that covers a ground plane", () => {
  const json = JSON.stringify({ scene: 0, scenes: [{ nodes: [0] }], nodes: [{ children: [1], translation: [0, 10, 0] }, { mesh: 0, scale: [2, 1, 2] }],
    meshes: [{ primitives: [{ attributes: { POSITION: 0 } }] }], accessors: [{ min: [-1, 0, -1], max: [1, 0.5, 1] }] });
  const pad = (4 - (json.length % 4)) % 4, body = json + " ".repeat(pad), b = new Uint8Array(20 + body.length), v = new DataView(b.buffer);
  v.setUint32(0, 0x46546c67, true); v.setUint32(4, 2, true); v.setUint32(8, b.length, true); v.setUint32(12, body.length, true); v.setUint32(16, 0x4e4f534a, true);
  b.set(new TextEncoder().encode(body), 20);
  assert.deepEqual(glbBounds(b), { min: [-2, 10, -2], max: [2, 10.5, 2] });
  assert.equal(glbBounds(new Uint8Array(30)), null);
  const piece = (top: number, half: number, kind: BackdropPiece["kind"]) => ({ detail: { id: 1, name: "table", resource: "T" }, kind,
    bounds: { min: [-half, top - 0.2, -half], max: [half, top, half], mesh: true } }) as unknown as BackdropPiece;
  const ext = { x0: -50, z0: -50, x1: 50, z1: 50 };
  assert.equal(coverLevel([piece(3, 200, "plane"), piece(9, 30, "plane"), piece(50, 300, "distant")], ext)!.level, 3, "a plane inside the level is not the sea's");
  assert.equal(coverLevel([piece(3, 20, "plane")], ext), null);
});

test("material edges: blue and perlin feathers in cells, and path masks along a polyline and a switchback", () => {
  const { store } = session();
  const F = { min: [480, -20] as [number, number], max: [520, 20] as [number, number], floor: 0, expr: "4", voxelSize: 1, material: 1 };
  const band = (pattern: string, cell?: number) => {
    const f = sampleField(store, { ...F, materials: [{ material: 2, mask: "x", feather: { width: 8, pattern: pattern as "blue", ...(cell ? { cell } : {}) } }] });
    const row: number[] = [];
    for (let a = 0; a < f.nx; a++) row.push(f.materialAt(10 * f.nx + a, f.hi[10 * f.nx + a] - 1));
    return row;
  };
  // in a 4-voxel cell the blue pattern changes at most every 4 columns
  const blue = band("blue", 4);
  for (let a = 1; a < blue.length; a++) if (blue[a] !== blue[a - 1]) assert.equal((a + Math.round((F.min[0] - 0) / 1)) % 4, 0, `a change inside a cell at column ${a}`);
  assert.ok(blue.includes(1) && blue.includes(2));
  assert.ok(band("perlin", 6).includes(2));
  assert.throws(() => sampleField(store, { ...F, materials: [{ material: 2, feather: { width: 1, cell: 0 } }] }), /cell is above 0/);
  // a path: material 3 within 2 of the line z = 0 from x 485 to 515, narrowing to 1
  const p = sampleField(store, { ...F, materials: [{ material: 3, paths: [{ points: [[485, 0], [515, 0]], width: 4, taper: 0.5 }] }] });
  const at = (x: number, z: number) => { const a = Math.floor(x - p.i0 * p.s), b = Math.floor(z - p.j0 * p.s), c = b * p.nx + a; return p.materialAt(c, p.hi[c] - 1); };
  assert.equal(at(486.5, 0.5), 3);
  assert.equal(at(486.5, 1.5), 3, "2 wide each side at the start");
  assert.equal(at(513.5, 1.5), 1, "narrowed at the end");
  assert.equal(at(500.5, 8.5), 1);
  assert.deepEqual(pathPoints({ width: 1, switchback: { from: [0, 0], to: [0, 30], legs: 3, spread: 10 } }), [[0, 0], [-5, 10], [5, 20], [0, 30]]);
  assert.throws(() => sampleField(store, { ...F, materials: [{ material: 3, paths: [{ width: 2 }] }] }), /points or switchback/);
});

test("relax and maxGradient: the heights are slope-limited, the volume kept, and plan and drape say so", () => {
  const h = new Float32Array([0, 0, 10, 10, 0, 0]), live = new Uint8Array(6).fill(1);
  const sum = h.reduce((a, b) => a + b, 0);
  const r = limitSlopes(h, 6, 1, live, 0, 45);
  assert.ok(r.steepest <= 45.1, String(r.steepest));
  assert.ok(Math.abs(h.reduce((a, b) => a + b, 0) - sum) < 1e-3, "the volume stays");
  const { store } = session();
  const steep = { ...CONE, expr: "max(0, min(20, (40 - abs(x))*3))" };
  const p = plan(store, { ...steep, maxGradient: 40 });
  const lim = (p.report.region as { slopeLimit?: { steepestDegrees: number } }).slopeLimit!;
  assert.ok(lim.steepestDegrees <= 40.1, JSON.stringify(lim));
  const d = planDrape(store, { ...steep, core: false, relax: 1, maxGradient: 40 });
  assert.ok((d.report.region as { slopeLimit?: unknown }).slopeLimit);
  assert.throws(() => plan(store, { ...steep, maxGradient: 95 }), /1-89/);
});

test("heightfield_drape estimateSkirts: the dry run answers each skirt mode's blocks and cells", async () => {
  const { s } = session();
  const r = await call(s, "heightfield_drape", { ...CONE, expr: "max(0, 20 - abs(x)*0.5)", core: false, skirtTo: -4, skirtTop: 6, dryRun: true, estimateSkirts: true });
  const m = new Map(r.skirtEstimates.map((e: { mode: string; cells: number }) => [e.mode.split(" ")[0] + (e.mode.includes("skirtTop") ? "+top" : ""), e.cells]));
  assert.equal(r.skirtEstimates.length, 4);
  const hollow = m.get("hollow") as number, core = m.get("core") as number, skirt = m.get("skirtTo") as number, top = m.get("skirtTo+top") as number;
  assert.ok(hollow < top && top < skirt, JSON.stringify(r.skirtEstimates));
  assert.ok(core > hollow);
  assert.equal(r.applied, false);
});

test("place_spawn_knots balance: the heights are snaked over the teams, a team's spawns spread over groups, and each team reported", async () => {
  const { store, s } = session();
  apply(store, plan(store, { min: [480, -12], max: [600, 12], floor: 0, expr: "1 + floor(max(0, x + 60)/15)*2", material: 1 }));
  const xs = [482, 497, 512, 527, 542, 557, 572, 587];
  const r = await call(s, "place_spawn_knots", { units: "map", balance: true, spawns: xs.map((x, i) => ({ worm: i, xz: [x, 0], group: `c${Math.floor(i / 2)}` })) });
  assert.equal(r.balanced.teams, 2);
  const [a, b] = r.teams.teams;
  assert.ok(Math.abs(a.meanHeight - b.meanHeight) <= 1, JSON.stringify(r.teams));
  // the lowest of each team is its worm 0
  const ground = (w: string) => { const sp = r.spawns.find((x: { name: string }) => x.name === w); return sp.pos[1]; };
  assert.ok(ground("WORM0") <= Math.min(ground("WORM1"), ground("WORM2"), ground("WORM3")));
  // no team has both spawns of a group (four groups of two over two teams of four)
  for (const t of [0, 1]) {
    const groups = r.placed.filter((p: { worm: number }) => Math.floor(p.worm / 4) === t).map((p: { group: string }) => p.group);
    assert.equal(new Set(groups).size, 4, JSON.stringify(r.placed));
  }
  await assert.rejects(call(s, "place_spawn_knots", { units: "map", balance: { teams: 3, wormsPerTeam: 2 }, spawns: xs.slice(0, 7).map((x, i) => ({ worm: i, xz: [x, 0] })) }), /fewer than the 7 spawns/);
});

test("build_castle gates: several gates, carved to the ground outside, and seat fills under the walls only", async () => {
  const { store, s } = session();
  // a slope down to +z: 8 at z -12, about 2 at z 12
  apply(store, plan(store, { min: [500, -16], max: [540, 16], floor: 0, expr: "max(1, 5 - z/4)", material: 1 }));
  const A = { at: [520, 0, 0], size: [16, 14], wallHeight: 5, towerRadius: 2.5, keep: false, units: "map", place: "new", ground: "max" };
  const r = await call(s, "build_castle", { ...A, gates: [{ side: "s" }, { side: "e", width: 3, offset: 1 }], dryRun: true });
  const gates = r.structureInfo.gates;
  assert.equal(gates.length, 2);
  assert.ok(gates[0].below > 0, "the ground south of the castle is lower than its floor: the gate is carved down to it");
  await assert.rejects(call(s, "build_castle", { ...A, gate: "s", gates: [{ side: "n" }], dryRun: true }), /gate or gates/);
  await assert.rejects(call(s, "build_castle", { ...A, gates: [{ side: "n", below: -1 }], dryRun: true }), /0 or more/);
  const found = await call(s, "build_castle", { ...A, foundation: 6, dryRun: true });
  const seat = await call(s, "build_castle", { ...A, seat: true, dryRun: true });
  assert.ok(seat.voxels.added < found.voxels.added, `seat ${seat.voxels.added} vs foundation ${found.voxels.added}`);
  // built: the south gate is an opening a worm fits through, the wall beside it is not
  const built = await call(s, "build_castle", { ...A, gates: [{ side: "s" }], seat: true });
  const floor = built.structureInfo.courtyardFloorY;
  const gate = await call(s, "query_ground", { x: 520.5, z: 7.5, y: floor + 1, units: "map" });
  assert.equal(gate.passableAt.passable, true, JSON.stringify(gate.passableAt));
  const wall = await call(s, "query_ground", { x: 525.5, z: 7.5, y: floor + 1, units: "map" });
  assert.equal(wall.passableAt.passable, false);
});

test("query_ground openings: a span with air under a lintel is an opening, passable when headroom high", () => {
  const o = openings([{ top: 10, bottom: 8 }, { top: 2, bottom: 0 }, { top: 9, bottom: 8.5 }]);
  assert.equal(o.length, 2);
  assert.equal(o[0].ceiling, null);
  assert.deepEqual([o[1].floor, o[1].ceiling, o[1].height], [2, 8, 6]);
});

test("check_reachability targets and slopeOver skin: the nearest of several targets, and a skin's cliff is a barrier", async () => {
  const { store, s } = session();
  apply(store, plan(store, { min: [480, -6], max: [600, 6], floor: 0, expr: "2", material: 1 }));
  const R = { x0: 480, z0: -6, x1: 600, z1: 6, units: "map", from: [[530.5, 3, 0.5]] };
  const r = await call(s, "check_reachability", { ...R, targets: [{ x: 485, z: 0, name: "west" }, { x: 590, z: 0, name: "east" }], targetMode: "each" });
  assert.equal(r.spawns[0].reachedTarget, "west");
  assert.equal(r.spawns[0].perTarget.length, 2);
  assert.ok(r.spawns[0].perTarget.every((t: { reachable: boolean }) => t.reachable));
  const near = await call(s, "check_reachability", { ...R, target: "nearest", targets: [{ x: 595, z: 0 }, { x: 530, z: 0 }] });
  assert.equal(near.spawns[0].reachedTarget, "targets[1]");
  await assert.rejects(call(s, "check_reachability", { ...R, target: "nearest" }), /give targets/);
  // a smooth skin rising 1.9 a column (62 degrees) from x 560: one voxel of 2 each step, so tread mode walks it; skin mode does not
  const { store: st2, s: s2 } = session();
  applyDrape(st2, planDrape(st2, { min: [530, -6], max: [600, 6], floor: 0, voxelSize: 2, thickness: 2, material: 1, expr: "2 + max(0, x - 10)*1.9", core: true }));
  const R2 = { x0: 530, z0: -6, x1: 600, z1: 6, units: "map", from: [[532, 4, 0.5]], target: { x: 598, z: 0, radius: 2 } };
  const skin = await call(s2, "check_reachability", { ...R2, slopeOver: "skin" });
  assert.equal(skin.spawns[0].reachable, false, "a 62-degree skin face is a barrier");
  const seg = await call(s2, "check_reachability", { ...R2, slopeOver: "segment" });
  assert.equal(seg.spawns[0].reachable, false);
  assert.match(skin.rules.note, /draped skin/);
  assert.ok(spansAt(grids(st2), 598, 0).length);
});
