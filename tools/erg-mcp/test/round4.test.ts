// The Caldera v2 round-4 gaps, on hand-made grids and the synthetic level (no server): the run trace's off-turn moves
// and what the probe saw beside them, the perf runs file's pick, check_trail's free width and knife edges, the drape's
// refine (a fine skin along a trail cut out of the coarse one, one undo step), and the material patches and colour
// delta behind game_material_shots. Bundled for Node by test.ps1.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, type Frame, type Loaded, type Scene } from "../src/core";
import { applyDrape, planDrape, refineZone } from "../src/lib/heightfield-drape";
import { apply, plan } from "../src/lib/heightfield-plan";
import { spansAt } from "../src/lib/inspect-field";
import { Raster } from "../src/lib/inspect-png";
import { centreColour, lookDelta, materialPatches } from "../src/lib/material-look";
import { attributeMove, describeMove, mergePerfRuns, offTurnMoves, parseTrace, pickPerfBase, type PerfRun, type TraceSample } from "../src/lib/play-trace";
import { checkTrail, type TrailCheckOptions } from "../src/lib/trail-check";
import { grids, type Grid } from "../src/lib/voxel";
import { validate } from "../src/registry";
import { tools as drapeTools } from "../src/tools/drape";
import { tools as gameTools } from "../src/tools/game";
import { tools as inspectTools } from "../src/tools/inspect";

const SOLID = 3;
const word = (m: number) => (SOLID | (m << 2)) >>> 0;

/** An upright grid of `size` voxels at `scale`, its low corner at `min`, filled solid up to height(x, z) in material(x, z). */
function block(id: number, min: [number, number, number], size: [number, number, number], scale: number, height: (x: number, z: number) => number, material: (x: number, z: number) => number = () => 1): Grid {
  const [X, Y, Z] = size, words = new Uint32Array(X * Y * Z);
  for (let z = 0; z < Z; z++)
    for (let x = 0; x < X; x++) {
      const cx = min[0] + (x + 0.5) * scale, cz = min[2] + (z + 0.5) * scale, h = height(cx, cz), m = material(cx, cz);
      for (let y = 0; y < Y; y++) if ((y + 1) * scale <= h + 1e-9) words[(z * X + x) * Y + y] = word(m);
    }
  const frame = { id, name: `block_${id}`, size: [X, Y, Z], pos: [0, 0, 0], rot: [0, 0, 0], scale } as unknown as Frame;
  return { id, frame, ref: id, size: [X, Y, Z], words, base: new Uint32Array(words.length),
    toMap: [scale, 0, 0, min[0], 0, scale, 0, min[1], 0, 0, scale, min[2]], fromMap: [1 / scale, 0, 0, -min[0] / scale, 0, 1 / scale, 0, -min[1] / scale, 0, 0, 1 / scale, -min[2] / scale],
    bounds: { min: [min[0], min[1], min[2]], max: [min[0] + X * scale, min[1] + Y * scale, min[2] + Z * scale] } };
}

// ------------------------------------------------------------------ the trace

const sample = (wall: number, tick: number, turn: number, active: number, worms: [number, number, [number, number, number]][]): TraceSample =>
  ({ wall, tick, turn, active, worms: worms.map(([slot, team, pos]) => ({ slot, team, pos, alive: true })) });

test("parseTrace reads TRACE_LUA's line in map units; nil turn and worm are null", () => {
  const s = parseTrace("1234;7;3;0:0:100.00:40.00:-200.00:true;5:2:-600.00:1108.00:600.00:false", 5000, 20)!;
  assert.equal(s.tick, 1234);
  assert.equal(s.turn, 7);
  assert.equal(s.active, 3);
  assert.deepEqual(s.worms[0], { slot: 0, team: 0, pos: [5, 2, -10], alive: true });
  assert.deepEqual(s.worms[1], { slot: 5, team: 2, pos: [-30, 55.4, 30], alive: false });
  assert.equal(parseTrace("900;nil;nil", 1, 20)!.turn, null);
  assert.equal(parseTrace("garbage", 1, 20), null);
});

test("offTurnMoves: a worm that drops while another plays is one event with its samples and speed; the active worm's own walk is not", () => {
  // WORM5 stands at y 55.4 through turn 21 (WORM2 active), then falls to 44.2 over two samples in turn 22; WORM2 walks 6 units on its own turn
  const t: TraceSample[] = [
    sample(0, 1000, 21, 2, [[2, 1, [10, 60, 10]], [5, 2, [-30, 55.4, 30]]]),
    sample(1000, 1050, 21, 2, [[2, 1, [16, 60, 10]], [5, 2, [-30, 55.4, 30]]]),
    sample(2000, 1100, 22, 0, [[2, 1, [16, 60, 10]], [5, 2, [-30.5, 50, 30.5]]]),
    sample(3000, 1150, 22, 0, [[2, 1, [16, 60, 10]], [5, 2, [-31, 44.2, 31]]]),
    sample(4000, 1200, 22, 0, [[2, 1, [16, 60, 10]], [5, 2, [-31, 44.2, 31]]]),
  ];
  const m = offTurnMoves(t);
  assert.equal(m.length, 1, JSON.stringify(m));
  assert.equal(m[0].worm, "WORM5");
  assert.equal(m[0].kind, "fall");
  assert.equal(m[0].turn, 21);     // the first sample's turn (the fall began between 21's last sample and 22's first)
  assert.equal(m[0].activeWorm, 2);
  assert.deepEqual(m[0].from, [-30, 55.4, 30]);
  assert.deepEqual(m[0].to, [-31, 44.2, 31]);
  assert.equal(m[0].drop, 11.2);
  assert.equal(m[0].samples.length, 3);
  assert.equal(m[0].seconds, 2);
  assert.ok(m[0].fastestDrop >= 5.4, `fastest ${m[0].fastestDrop}`);
  // a small jitter is not a move; a worm not in the next sample ends nothing
  assert.equal(offTurnMoves([sample(0, 1, 1, 0, [[5, 2, [0, 10, 0]]]), sample(1000, 2, 1, 0, [[5, 2, [0.5, 9.5, 0.5]]])]).length, 0);
  // a move at a higher threshold is not one
  assert.equal(offTurnMoves(t, { drop: 12 }).length, 0);
});

test("attributeMove: an explosion beside the move in its ticks makes it a blast with the turn's weapon; a mine at rest and the worm's death are named", () => {
  const ev = offTurnMoves([sample(0, 1000, 21, 2, [[5, 2, [-30, 55.4, 30]]]), sample(1000, 1050, 21, 2, [[5, 2, [-31, 44.2, 31]]])])[0];
  const far = attributeMove(ev, { explosions: [{ tick: 900, turn: 21, at: [40, 50, 40], source: "weapon" }] });
  assert.equal(far.kind, "fall");
  assert.equal(far.explosions, undefined);
  const hit = attributeMove(ev, { explosions: [{ tick: 1020, turn: 21, at: [-28, 55, 31], source: "weapon", turnWeapon: "kWeaponBazooka", turnWorm: 2 }], fires: [{ tick: 980, turn: 21, mines: [{ rest: [-30, 54, 29] }, { rest: [50, 1, 50] }] }],
    deaths: [{ slot: 5, tick: 1060, turn: 21, cause: "weapon" }] });
  assert.equal(hit.kind, "blast");
  assert.equal(hit.explosions!.length, 1);
  assert.equal(hit.explosions![0].turnWeapon, "kWeaponBazooka");
  assert.equal(hit.mines!.length, 1);
  assert.equal(hit.death!.cause, "weapon");
  const words = describeMove(hit);
  assert.match(words, /WORM5 \(team 2\) knocked by weapon \(kWeaponBazooka, WORM2's turn\)/);
  assert.match(words, /it died \(weapon\)/);
  assert.match(describeMove(far), /fell 11.2 map units/);
});

// ------------------------------------------------------------------ the perf runs file

const perf = (label: string, startedAt: number, game?: string, gpu = 4): PerfRun => ({ label, scene: "match", seconds: 10, samples: 50, startedAt, fps: { p50: 36, p95: 37, n: 50 }, busyMs: null, frameMs: null, gpuFrameMs: { p50: gpu, p95: gpu + 0.5, n: 50 }, ...(game ? { game } : {}) });

test("pickPerfBase: the session's run first, else the runs file's of the same game, else any (said); without against, the file's latest baseline", () => {
  const saved = [perf("baseline15", 1000, "A", 4), perf("baseline15", 2000, "B", 5), perf("caldera", 1500, "A")];
  assert.equal(pickPerfBase([perf("baseline15", 9000, "A", 7)], saved, "baseline15", "A").base!.gpuFrameMs!.p50, 7);
  const same = pickPerfBase([], saved, "baseline15", "A");
  assert.equal(same.base!.gpuFrameMs!.p50, 4);
  assert.equal(same.base!.from, "saved");
  assert.match(same.note!, /same game/);
  const other = pickPerfBase([], saved, "baseline15", "C");
  assert.equal(other.base!.gpuFrameMs!.p50, 5);
  assert.match(other.note!, /another game build/);
  assert.equal(pickPerfBase([], saved, "nothing", "A").base, null);
  const auto = pickPerfBase([], saved, undefined, "B");
  assert.equal(auto.base!.startedAt, 2000);
  assert.match(auto.note!, /latest baseline/);
  assert.equal(pickPerfBase([perf("x", 1)], saved, undefined, "B").base!.label, "x");
  const merged = mergePerfRuns([perf("caldera", 1500, "A", 9)], saved);
  assert.equal(merged.length, 3, "one per label and start, memory's first");
  assert.equal(merged.find((r) => r.label === "caldera")!.gpuFrameMs!.p50, 9);
  assert.equal(merged.filter((r) => r.from === "saved").length, 2);
});

// ------------------------------------------------------------------ check_trail's free width

const opts: TrailCheckOptions = { halfWidth: 2.6, margin: 1, spacing: 0.25, maxDrop: 3, holeRadius: 1.5, above: 0, headroom: 1.5 };

test("check_trail: a ridge 1.5 wide is a knife edge (free width under 2) though no column drops over maxDrop; a wide bed's free width is the reach", () => {
  // a plateau at 8; along z = 10 between x 10 and 20 the ground beside a 1.5-wide ridge falls to 6 (a drop of 2: under maxDrop 3, over edgeDrop 1.5)
  const ridge = (x: number, z: number) => (x >= 10 && x < 20 && Math.abs(z - 10) > 0.75 ? 6 : 8);
  const gs = [block(-1, [-10, 0, -5], [60, 10, 25], 1, ridge)];
  const rep = checkTrail(gs, [[2, 8, 10], [38, 8, 10]], opts);
  assert.equal(rep.runs.filter((r) => r.kind === "drop").length, 0, "no drop over maxDrop");
  assert.equal(rep.narrows.length, 1, JSON.stringify(rep.narrows));
  const n = rep.narrows[0];
  assert.ok(n.from >= 7.5 && n.from <= 9 && n.to >= 17 && n.to <= 18.5, `the knife edge runs x 10..20 (${n.from}..${n.to} along)`);
  // the spine is 1.5 wide; a column read on a cell's edge counts on one side (inspect-field keeps the edge in the cell)
  assert.ok(n.width <= 1.75 + 1e-9 && n.width >= 1, `free width ${n.width} (left ${n.left}, right ${n.right})`);
  assert.equal(rep.narrowest!.width, n.width);
  // the wide plateau before it: the free width reaches the cap either side
  const wide = rep.freeWidths.find((f) => f[0] < 4)!;
  assert.equal(wide[1], 8);
  assert.equal(wide[2], 8);
  // a 1-unit drop beside the bed is not an edge at edgeDrop 1.5, and is one at edgeDrop 0.5
  const kerb = (x: number, z: number) => (Math.abs(z - 10) > 1 ? 7 : 8);
  const gk = [block(-2, [-10, 0, -5], [60, 10, 25], 1, kerb)];
  assert.equal(checkTrail(gk, [[2, 8, 10], [38, 8, 10]], opts).narrows.length, 0);
  assert.ok(checkTrail(gk, [[2, 8, 10], [38, 8, 10]], { ...opts, edgeDrop: 0.5 }).narrowest!.width <= 2);
});

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
type Session = import("../src/session").Session;
const call = async (s: Session, name: string, args: Record<string, unknown>): Promise<any> => {
  const t = [...inspectTools, ...drapeTools, ...gameTools].find((x) => x.name === name)!;
  const bad = validate(t.inputSchema, args);
  if (bad) throw new Error(bad);
  return await t.handler(s, args);
};

test("check_trail (tool): freeWidth answers the narrowest point and the knife edges, the summary names them, the rules carry the defaults", async () => {
  const { store, s } = session();
  // a ridge in a stepped field: 8 on a 2-wide spine along z = 0 between x 510 and 520, 6 beside it, 8 elsewhere (the
  // expression's x and z count from the rectangle's centre, 520, 0)
  apply(store, plan(store, { min: [500, -6], max: [540, 6], floor: 0, expr: "8 - 2*step(-10, x)*(1 - step(0, x))*step(1, abs(z))", material: 1 }));
  const r = await call(s, "check_trail", { trails: [[[502, 8, 0], [538, 8, 0]]], width: 4, margin: 1 });
  const fw = r.trails[0].freeWidth;
  assert.ok(fw && fw.knifeEdges >= 1, JSON.stringify(fw));
  assert.ok(fw.narrowest.width < 2, JSON.stringify(fw.narrowest));
  assert.match(r.trails[0].summary, /knife-edge stretch/);
  assert.deepEqual(r.rules.freeWidth, { edgeDrop: 1.5, freeReach: 8, minFreeWidth: 2 });
  const strict = await call(s, "check_trail", { trails: [[[502, 8, 0], [538, 8, 0]]], width: 4, margin: 1, minFreeWidth: 0.5 });
  assert.equal(strict.trails[0].freeWidth.knifeEdges, 0);
  assert.doesNotMatch(strict.trails[0].summary, /knife-edge/);
});

// ------------------------------------------------------------------ the drape's refine

test("heightfield_drape refine: a fine skin along the trail, the coarse skin cut out under it, one undo step; the ground on the trail is the fine skin and off it the coarse one, and the two meet", async () => {
  const { store, s } = session();
  const a = { min: [100, 100] as [number, number], max: [160, 160] as [number, number], floor: 0, expr: "max(0, 30 - r*0.8)", material: 2, voxelSize: 2.5, thickness: 2, core: false };
  const trail = [[104, 130], [156, 130]];
  const plain = planDrape(store, a);
  const refined = planDrape(store, { ...a, refine: { voxelSize: 1.25, paths: [trail], distance: 4 } });
  assert.equal(refined.report.fits, true, String(refined.report.why));
  assert.equal(refined.parts!.length, 2);
  const ref = refined.report.refine as { voxelSize: number; columns: number; skin: { blocks: number; cells: number }; coarseColumnsCut: number };
  assert.equal(ref.voxelSize, 1.25);
  assert.ok(ref.skin.blocks >= 1 && ref.columns > 0, JSON.stringify(ref));
  const plainCols = (plain.report.region as { columns: number }).columns;
  assert.ok(ref.coarseColumnsCut < plainCols, `the coarse skin lost columns to the zone (${ref.coarseColumnsCut} of ${plainCols})`);
  const budget = (q: typeof plain) => (q.report.budget as { blocks: { after: number }; cells: { after: number } });
  assert.ok(budget(refined).blocks.after > budget(plain).blocks.after, "more blocks: the fine skin's");
  // applied as one undo step through the tool
  const r = await call(s, "heightfield_drape", { ...a, refine: { voxelSize: 1.25, paths: [trail], distance: 4 } });
  assert.equal(r.applied, true);
  assert.equal(store.stack.depth, 1);
  assert.deepEqual(store.problems(), []);
  const gs = grids(store), skins = gs.filter((g) => g.frame.corners);
  assert.ok(skins.some((g) => Math.abs(g.toMap[0] - 1.25) < 1e-9) && skins.some((g) => Math.abs(g.toMap[0] - 2.5) < 1e-9), "skins of both voxel sizes");
  const cone = (x: number, z: number) => Math.max(0, 30 - Math.hypot(x - 130, z - 130) * 0.8);
  // on the trail the ground is the fine skin; 12 off it the coarse one; both within a voxel of the cone
  for (const [x, z, fine] of [[120, 130, true], [140, 131, true], [130, 118, false], [112, 112, false]] as [number, number, boolean][]) {
    const top = spansAt(gs, x, z)[0];
    assert.ok(top, `ground at ${x}, ${z}`);
    const g = gs.find((q) => q.id === top.frame)!;
    assert.equal(Math.abs(g.toMap[0] - 1.25) < 1e-9, fine, `${x}, ${z}: skin scale ${g.toMap[0]}`);
    assert.ok(Math.abs(top.top - cone(x, z)) < 2.5, `${x}, ${z}: ${top.top} vs ${cone(x, z)}`);
  }
  // at the zone's edge the two skins stand within a coarse voxel of each other (no step a worm cannot take, no gap)
  const edge = spansAt(gs, 130, 133.5), both = edge.filter((sp) => skins.some((g) => g.id === sp.frame));
  assert.ok(both.length >= 1, "ground at the seam");
  if (both.length >= 2) assert.ok(Math.abs(both[0].top - both[1].top) <= 2.5, `seam step ${Math.abs(both[0].top - both[1].top)}`);
  store.undo();
  assert.equal(grids(store).filter((g) => g.frame.new).length, 0, "one undo takes both skins out");
  // the zone, and the refusals
  const zone = refineZone({ voxelSize: 1, paths: [trail], distance: 4 }, 1);
  assert.ok(zone(120, 133) && !zone(120, 135) && zone(104, 130) && !zone(99, 130));
  assert.throws(() => planDrape(store, { ...a, refine: { voxelSize: 2.5, paths: [trail] } }), /finer than/);
  assert.throws(() => planDrape(store, { ...a, refine: { voxelSize: 1 } }), /give paths/);
  assert.ok(validate(drapeTools[0].inputSchema, { ...a, refine: { voxelSize: 1, clip: [{ x: 130, z: 130, radius: 6 }] } }) === null);
});

// ------------------------------------------------------------------ the material look

test("materialPatches finds the flat middle of each material's patch; centreColour and lookDelta measure a shot against the swatch", () => {
  // a 40 x 40 slab at 8: material 1 on the west half, 5 on the east, and a 4-wide band of 9 along z = 20 in the middle
  const g = block(-1, [0, 0, 0], [40, 10, 40], 1, () => 8, (x, z) => (Math.abs(z - 20) < 2 && x > 10 && x < 30 ? 9 : x < 20 ? 1 : 5));
  const ps = materialPatches([g], { step: 2 });
  const of = (m: number) => ps.find((p) => p.material === m)!;
  assert.ok(of(1) && of(5) && of(9), JSON.stringify(ps.map((p) => p.material)));
  assert.ok(of(1).at[0] < 20 && of(5).at[0] > 20, JSON.stringify([of(1).at, of(5).at]));
  assert.ok(Math.abs(of(9).at[2] - 20) <= 2 && of(9).at[0] > 10 && of(9).at[0] < 30, JSON.stringify(of(9).at));
  assert.equal(of(9).at[1], 8);
  assert.ok(of(1).samples > of(9).samples);
  assert.deepEqual(materialPatches([g], { step: 2, only: [5] }).map((p) => p.material), [5]);
  const img = new Raster(20, 10, [10, 10, 10]);
  img.rect(6, 3, 8, 4, [190, 230, 170]);
  const c = centreColour(img.data, 20, 10, 0.3);
  assert.deepEqual(c, [190, 230, 170]);
  const d = lookDelta([120, 115, 105], c);
  assert.ok(d.lightness > 60, `lighter by ${d.lightness}`);
  assert.equal(d.cast, "green");
  assert.match(d.words, /much lighter .* with a green cast/);
  assert.equal(lookDelta([100, 100, 100], [104, 98, 101]).words, "about as light as the swatch");
});
