// The Caldera v2 round-1 gaps: check_trail (drops and holes across a trail), game_play_turns' approach route to its trail
// (planned round a pillar), check_flood with team knots (WORM0-15), list_objects' outside-frame warnings and map
// positions, the heightfield floor's lattice snap (floor report, keepFloor), build_castle's measured courtyard height, the
// older server's open_project error, the PNG decoder and the labels' projection. On the synthetic 12-frame level (its
// root renamed Scene), away from its own frames. Bundled for Node by test.ps1.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, type Loaded, type Scene } from "../src/core";
import { apply, plan } from "../src/lib/heightfield-plan";
import { applyDrape, planDrape } from "../src/lib/heightfield-drape";
import { checkTrail, densify } from "../src/lib/trail-check";
import { Raster, decodePng } from "../src/lib/inspect-png";
import { spansAt } from "../src/lib/inspect-field";
import { grids } from "../src/lib/voxel";
import { planWalk } from "../src/lib/walk-plan";
import { validate } from "../src/registry";
import { olderServer } from "../src/session";
import { approachRoute, project } from "../src/tools/game";
import { blastEye, blocked } from "../src/tools/blast";
import { tools as inspectTools } from "../src/tools/inspect";
import { tools as placeTools } from "../src/tools/place";
import { tools as structureTools } from "../src/tools/structures";
import { tools as hfTools } from "../src/tools/heightfield";
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
  const project = { store, id: "t", info: {}, erg: { script: async () => "" } };
  const s = { requireProject: () => project, get store() { return store; }, project,
    levels: async () => { throw new Error("no server"); }, client: async () => { throw new Error("no server"); }, ensure: async () => { throw new Error("no server"); } } as unknown as Session;
  return { store, s };
}
const call = async (s: Session, name: string, args: Record<string, unknown>): Promise<any> => {
  const t = [...placeTools, ...inspectTools, ...structureTools, ...hfTools].find((x) => x.name === name)!;
  const bad = validate(t.inputSchema, args);
  if (bad) throw new Error(bad);
  return await t.handler(s, args);
};

test("check_trail: a flat bed passes; a gap beside the bed is a drop on that side, a pit in it a hole and a bare centreline", async () => {
  const { store, s } = session();
  const slab = (x0: number, z0: number, x1: number, z1: number) => apply(store, plan(store, { min: [x0, z0], max: [x1, z1], floor: 0, expr: "8", material: 1 }));
  slab(500, -6, 540, 6);
  const trail = [[502, 8, 0], [538, 8, 0]];
  const ok = await call(s, "check_trail", { trails: [trail], width: 4, margin: 2 });
  assert.equal(ok.ok, true, JSON.stringify(ok.trails[0].runs));
  // again with a gap on the left (+z for a trail running +x) at x 510..516, z 3..6, and a pit across the centre at x 524..526
  const { store: st2, s: s2 } = session();
  const slab2 = (x0: number, z0: number, x1: number, z1: number) => apply(st2, plan(st2, { min: [x0, z0], max: [x1, z1], floor: 0, expr: "8", material: 1 }));
  slab2(500, -6, 524, 3); slab2(526, -6, 540, 3); slab2(524, -6, 526, -1); slab2(524, 1, 526, 3);
  slab2(500, 3, 510, 6); slab2(516, 3, 540, 6);
  const r = await call(s2, "check_trail", { trails: [trail], width: 4, margin: 2 });
  assert.equal(r.ok, false);
  const runs = r.trails[0].runs as { kind: string; side: string; from: number; to: number }[];
  const left = runs.find((x) => x.kind === "drop" && x.side === "left" && x.from < 15);
  assert.ok(left && left.from >= 7.5 && left.to <= 14.5, JSON.stringify(runs));
  assert.ok(!runs.some((x) => x.kind === "drop" && x.side === "right" && (x.to < 21 || x.from > 25)), "no drop on the right away from the pit");
  assert.ok(runs.some((x) => x.kind === "hole" && x.from >= 21.5 && x.to <= 24.5), "the pit is a hole");
  assert.ok(runs.some((x) => x.kind === "noGround" && x.side === "centre"), "the centreline over the pit has no ground");
  assert.equal(densify([[0, 0], [1, 0]], 0.25).length, 5);
  assert.equal(checkTrail(grids(st2), [[502, 0], [508, 0]], { halfWidth: 2, margin: 2, spacing: 0.25, maxDrop: 3, holeRadius: 1.5, above: 0.5, headroom: 1.5 }).runs.length, 0);
});

test("check_trail: skinCorners lists the corner where four draped skin blocks meet near the centreline", async () => {
  const { store, s } = session();
  // a 100 x 100 drape at voxel 2.5: 40 columns, tiles of 31, so four blocks meet at (min + 77.5) on both axes
  applyDrape(store, planDrape(store, { min: [400, 100], max: [500, 200], floor: 0, expr: "6 + 0.1 * x", voxelSize: 2.5, thickness: 2, core: false, material: 1 }));
  const r = await call(s, "check_trail", { trails: [[[460, 177.5], [490, 177.5]]] });
  const c = r.trails[0].skinCorners;
  assert.ok(c && c.length === 1 && c[0].at[0] === 477.5 && c[0].at[1] === 177.5 && c[0].blocks === 4, JSON.stringify(r.trails[0]));
});

test("approachRoute: a worm behind a pillar gets a route round it to its trail; on the trail, none", () => {
  const { store, s } = session();
  apply(store, plan(store, { min: [500, -10], max: [530, 10], floor: 0, expr: "4", material: 1 }));
  // a pillar 6 high at x 510..513, z -3..3 between the worm (x 505) and the trail's start (x 518)
  apply(store, plan(store, { min: [510, -3], max: [513, 3], floor: 0, expr: "12", material: 1 }));
  const W = 20, trail = [[518 * W, 4 * W, 0], [528 * W, 4 * W, 0]];
  const r = approachRoute(s, [505 * W, 4 * W, 0], trail);
  assert.ok(r && r.lead && r.lead.length > 5, JSON.stringify(r));
  assert.ok(r!.lead!.some((p) => Math.abs(p[2] / W) > 2.5), "the route goes round the pillar, not through it");
  assert.ok(r!.lead!.every((p) => !(p[0] / W > 510 && p[0] / W < 513 && Math.abs(p[2] / W) < 3)), "never inside the pillar");
  assert.equal(approachRoute(s, [518.5 * W, 4 * W, 0], trail), null, "a worm on its trail walks it directly");
  // toY: the goal is only the ground at that height
  const up = planWalk(store, [505, 4, 0], [520, 0], { radius: 1, toY: 30 });
  assert.ok("why" in up, "no ground at y 30 there");
});

test("check_flood: team knots WORM0-15 stand for the teams' worms (slot team x wormsPerTeam + place), the knots past wormsPerTeam are not played", async () => {
  const { store, s } = session();
  apply(store, plan(store, { min: [500, -20], max: [540, 20], floor: 0, expr: "10 - 0.2 * abs(x)", material: 1 }));
  const spawns = Array.from({ length: 16 }, (_, w) => ({ worm: w, pos: [505 + (w % 4) * 2 + Math.floor(w / 4) * 8, 10, -10 + (w % 4)] }));
  await call(s, "place_spawn_knots", { mode: "teams", units: "map", spawns });
  const r0 = await call(s, "check_flood", { units: "map", start: 0.5, levels: [{ turn: 1, level: 0.5 }], target: { x: 520, z: 0, radius: 30 }, wormsPerTeam: 2, minOwnTurns: 1 });
  const r = typeof r0 === "string" ? JSON.parse(r0) : r0;
  assert.equal(r.flood.teams, 4, "four teams with knots");
  assert.equal(r.flood.wormsPerTeam, 2);
  const firstTurns = r.flood.turnOrder.firstTurns.slice(0, 8).map((t: { worm: string }) => t.worm);
  assert.deepEqual(firstTurns, ["WORM0", "WORM2", "WORM4", "WORM6", "WORM1", "WORM3", "WORM5", "WORM7"], "the teams alternate, slots team x 2 + place");
  assert.ok(/flood PASS/.test(r.summary), r.summary);
  const t1 = r.spawns.find((x: { name: string }) => x.name === "WORM5");
  assert.equal(t1.flood.moves.team, 1, "knot WORM5 is team 1's second worm");
  assert.equal(r.spawns.find((x: { name: string }) => x.name === "WORM6").flood.moves, undefined, "knot WORM6 (team 1's third) is not in a match of two a team");
});

test("list_objects: outsideFrames summary and none; world positions carry mapPos", async () => {
  const { store, s } = session();
  apply(store, plan(store, { min: [500, -5], max: [510, 5], floor: 0, expr: "4", material: 1 }));
  const all = await call(s, "list_objects", { role: "camera" });
  const outside = all.issues.filter((t: string) => /outside every terrain frame/.test(t));
  const sum = await call(s, "list_objects", { role: "camera", outsideFrames: "summary" });
  const none = await call(s, "list_objects", { role: "camera", outsideFrames: "none" });
  assert.equal(none.issues.filter((t: string) => /outside every terrain/.test(t)).length, 0);
  if (outside.length) assert.equal(sum.issues.filter((t: string) => /outside every terrain/.test(t)).length, 1, "one summary line");
  const sp = await call(s, "list_objects", {});
  for (const d of [...sp.spawns, ...sp.levelObjects, ...sp.placed]) assert.deepEqual(d.mapPos, d.pos.map((v: number) => Math.round((v / 20) * 1000) / 1000));
});

test("the heightfield floor: off the lattice it snaps and the answer says where a height stands; keepFloor counts from the floor asked", async () => {
  const { store, s } = session();
  const a = await call(s, "heightfield_plan", { min: [500, -10], max: [540, 10], floor: -9, expr: "14", voxelSize: 2.5, material: 1 });
  const f = a.region.floor;
  assert.equal(f.asked, -9);
  assert.ok(Math.abs(f.shift) > 0.4 && f.warning, JSON.stringify(f));
  const dr = planDrape(store, { min: [500, -10], max: [540, 10], floor: -9, expr: "14", voxelSize: 2.5, thickness: 2, core: false, material: 1 });
  applyDrape(store, dr);
  const top = spansAt(grids(store), 520.3, 0.4)[0].top;
  assert.ok(Math.abs(top - (f.lattice + 14)) < 1e-6, `the skin stands at the lattice floor + 14: ${top}`);
  const { store: st2 } = session();
  const d2 = planDrape(st2, { min: [500, -10], max: [540, 10], floor: -9, expr: "14", voxelSize: 2.5, thickness: 2, core: false, material: 1, keepFloor: true });
  applyDrape(st2, d2);
  assert.ok(Math.abs(spansAt(grids(st2), 520.3, 0.4)[0].top - 5) < 1e-6, "keepFloor: a height of 14 over floor -9 stands at 5");
  assert.equal((d2.report.region as { floor: { keepFloor: boolean } }).floor.keepFloor, true);
});

test("build_castle: the courtyard's walk height as built, against the y asked", async () => {
  const { s } = session();
  const r = await call(s, "build_castle", { at: [600, 4, 0], size: [20, 20], wallHeight: 6, wallThickness: 2, scale: 1.5, foundation: 2, keep: false, units: "map", place: "new" });
  assert.ok(r.courtyard, JSON.stringify(Object.keys(r)));
  assert.equal(r.courtyard.askedY, 4);
  assert.equal(r.courtyard.walkY, r.courtyard.askedY + r.courtyard.snappedBy);
  assert.ok(r.courtyard.walkY >= 4 && r.courtyard.walkY <= 5.5, JSON.stringify(r.courtyard));
});

test("open_project on an older server: the error says the server is older than the project's format", () => {
  const e = olderServer(new Error('spawns.mode: must be "random" or "knots"'), "0.13.0");
  assert.match(e.message, /older than the project's format/);
  assert.match(e.message, /MELANGE_EXE/);
  assert.match(e.message, /Melange 0\.13\.0/);
  const other = new Error("timeout");
  assert.equal(olderServer(other), other, "other errors pass through");
});

test("decodePng reads back what Raster writes, and project() puts the target in the picture's middle", () => {
  const r = new Raster(7, 5, [10, 20, 30]);
  r.rect(2, 1, 3, 2, [200, 100, 50]);
  r.ring(3, 2, 1, [255, 0, 0], 1);
  const d = decodePng(r.png());
  assert.equal(d.w, 7);
  assert.equal(d.h, 5);
  assert.deepEqual([...d.rgb], [...r.data]);
  const pose = { pos: [0, 100, 300], target: [0, 0, 0], fov: 60 };
  const c = project(pose, [0, 0, 0], 960, 540)!;
  assert.ok(Math.abs(c[0] - 480) < 1e-6 && Math.abs(c[1] - 270) < 1e-6, String(c));
  const right = project(pose, [50, 0, 0], 960, 540)!;
  assert.ok(right[0] > 480, "+x is to the right seen from the south");
  assert.equal(project(pose, [0, 100, 400], 960, 540), null, "behind the camera");
});

test("test_explosion's eye: clear of the ground, not inside the hill behind the wall", () => {
  const { store } = session();
  // a hill 20 high south of x 500 (z 2..30) and a wall point at (505, 3, 0) on its north face
  apply(store, plan(store, { min: [490, 2], max: [520, 30], floor: 0, expr: "20", material: 1 }));
  const gs = grids(store), p = [505, 3, 0];
  assert.equal(blocked(gs, [505, 10, 15], p), true, "from the south the hill is in the way");
  const e = blastEye(gs, p, 10);
  assert.ok(e.clear && !blocked(gs, e.eye, p), JSON.stringify(e));
  assert.ok(e.eye[2] < 2, "it looks from the open side");
});
