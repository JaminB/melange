// check_reachability's path figures, utility classification and route picture,
// and the spawn checks (a wall at the turn camera, ground under the water) on the synthetic 12-frame level (its root
// renamed Scene so blocks can be added), bundled for Node by test.ps1.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, type Loaded, type Scene } from "../src/core";
import { apply, plan } from "../src/lib/heightfield-plan";
import { classify, pathStats, pathTo, relaxed, search, standing, surfaces, walks, type ReachOptions } from "../src/lib/reach";
import { checkSpawns, solidNear } from "../src/lib/spawn-check";
import { grids } from "../src/lib/voxel";
import { tools as inspectTools } from "../src/tools/inspect";
import { tools as placeTools } from "../src/tools/place";
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
  const s = { requireProject: () => ({ store, id: "t", info: {} }), get store() { return store; }, project: { store },
    levels: async () => { throw new Error("no server"); }, client: async () => { throw new Error("no server"); } } as unknown as Session;
  return { store, s };
}
const all = [...inspectTools, ...placeTools];
const call = async (s: Session, name: string, args: Record<string, unknown>): Promise<any> => all.find((t) => t.name === name)!.handler(s, args);


function strip(expr: string) {
  const { store, s } = session();
  apply(store, plan(store, { min: [200, -10], max: [260, 10], floor: 0, expr, material: 1 }));
  const o: ReachOptions = { step: 1, maxStep: 0.5, maxSlope: 45, maxDrop: 8, maxGap: 0, headroom: 1.5, water: null };
  const S = surfaces(grids(store), { x0: 200, z0: -10, x1: 260, z1: 10 }, 1, 1.5);
  return { store, s, o, S };
}
const reach = (s: Session, from: number[], tx: number) =>
  call(s, "check_reachability", { from: [from], x0: 200, z0: -10, x1: 260, z1: 10, target: { x: tx, z: 0, radius: 2 }, units: "map" });


test("check_reachability: the path's length, steepest slope, highest step and deepest drop", async () => {
  const { s, o, S } = strip("1 + max(0, (x + 20)*0.5)");
  const q = search(S, standing(S, [205, 2, 0], o), o);
  const top = (() => { let b = -1; for (let n = 0; n < S.nodes; n++) if (q.reached[n] && (b < 0 || S.y[n] > S.y[b])) b = n; return b; })();
  const st = pathStats(S, pathTo(q, top));
  assert.ok(st.maxSlope > 0 && st.maxSlope <= 45 + 1e-6 && st.maxStepUp <= 1 && st.jumps === 0, JSON.stringify(st));
  assert.ok(st.length >= q.dist[top] - 1e-6, "along the ground is at least the horizontal walk");
  const r = JSON.parse(await reach(s, [205, 2, 0], 258));
  const sp = r.spawns[0];
  assert.equal(sp.reachable, true, JSON.stringify(sp));
  assert.ok(sp.path.length >= sp.walk && sp.path.maxSlope <= 45 && sp.path.maxStepUp <= 1 && sp.path.maxDrop >= 0, JSON.stringify(sp.path));
  assert.ok(r.rules.utilities.note.includes("heuristic"));
});

test("check_reachability: the utility a start needs, cheapest first, and where it is used", async () => {
  const cases: [string, number[], number, string | null][] = [
    ["4 - 4*step(-1, x)*(1 - step(1, x))", [212, 4, 0], 248, "jump"],       // a gap 2 wide
    ["4 - 4*step(-3, x)*(1 - step(3, x))", [212, 4, 0], 248, "girder"],     // 6 wide
    ["2 + 18*step(0, x)", [205, 2, 0], 250, "rope"],                       // a cliff 18 high
    ["2 + 48*step(0, x)", [205, 2, 0], 250, "jetpack"],                    // 48 high
    ["2 + 18*step(0, x) + 2*step(-15, x)*(1 - step(-12, x))", [205, 2, 0], 250, "rope"],   // a girder step on the way: the rope is at the cliff
    ["4 - 4*step(-18, x)*(1 - step(18, x))", [205, 4, 0], 255, null],      // 36 wide
  ];
  for (const [expr, from, tx, want] of cases) {
    const { s, o, S } = strip(expr);
    const r = JSON.parse(await reach(s, from, tx));
    const sp = r.spawns[0];
    assert.equal(sp.reachable, false, `${expr}: ${JSON.stringify(sp)}`);
    assert.equal(sp.needs.utility, want, `${expr}: ${JSON.stringify(sp.needs)}`);
    if (!want) continue;
    assert.ok(sp.needs.usedAt && sp.needs.path.length > 0 && sp.needs.waypoints.length >= 2, JSON.stringify(sp.needs));
    if (want === "rope") assert.ok(Math.abs(sp.needs.usedAt.rise - 18) < 1.01, JSON.stringify(sp.needs.usedAt));
    if (want === "jump" || want === "girder") assert.ok(sp.needs.usedAt.across > 1.5, JSON.stringify(sp.needs.usedAt));
    // the lib directly: the first refused move of the widened route is the one reported
    const n0 = standing(S, from as [number, number, number], o), goal = new Uint8Array(S.nodes);
    for (let n = 0; n < S.nodes; n++) if (S.y[n] >= S.y.reduce((m, y) => Math.max(m, y), -Infinity) - 1.5 && Math.abs((S.cell[n] % S.w) + 200.5 - tx) <= 2 && Math.abs(Math.floor(S.cell[n] / S.w) - 9.5) <= 2) goal[n] = 1;
    const c = classify(S, n0, goal, o);
    assert.equal(c.utility, want);
    assert.ok(c.at && !walks(S, c.at[0], c.at[1], o));
    const tiers = relaxed(o), i = tiers.findIndex(([u]) => u === want);
    assert.ok(!walks(S, c.at[0], c.at[1], i ? tiers[i - 1][1] : o), "used where the cheaper rules refuse");
    const tier = relaxed(o).find(([u]) => u === want)![1];
    for (let k = 1; k < c.path.length; k++) assert.ok(walks(S, c.path[k - 1], c.path[k], tier), `move ${k} breaks the ${want} rules`);
  }
});

test("check_reachability render: a PNG with each start's route", async () => {
  const { s } = strip("4 - 4*step(-1, x)*(1 - step(1, x))");
  const r = await call(s, "check_reachability", { from: [[212, 4, 0], [250, 4, 0]], x0: 200, z0: -10, x1: 260, z1: 10, target: { x: 248, z: 0, radius: 2 }, units: "map", render: true });
  assert.equal(r.content[0].type, "image");
  assert.equal(r.content[0].mimeType, "image/png");
  const info = JSON.parse(r.content[1].text);
  assert.equal(info.image.routes.length, 2);
  assert.notEqual(info.image.routes[0].colour, info.image.routes[1].colour);
  assert.equal(info.spawns[1].reachable, true);
  assert.equal(info.spawns[0].needs.utility, "jump");
  // without render the answer is the text alone, as before
  assert.equal(typeof (await reach(s, [212, 4, 0], 248)), "string");
});

test("spawn checks: a wall at the turn camera, ground under the water, in list_objects and place_spawn_knots", async () => {
  const { store, s } = strip("2 + 18*step(6, x)");      // ground 2 high, a plateau 20 high from x 236
  const gs = grids(store);
  assert.ok(solidNear(gs, 234.5, 0, 8, 2.5), "the wall is within 2.5 at the camera's height");
  assert.equal(solidNear(gs, 220, 0, 8, 2.5), null);
  const placed = await call(s, "place_spawn_knots", { units: "map", spawns: [{ worm: 0, pos: [234.5, 3, 0], drop: true }, { worm: 1, pos: [220, 3, 0], drop: true }] });
  const w0 = placed.warnings.filter((t: string) => /WORM0/.test(t)), w1 = placed.warnings.filter((t: string) => /WORM1/.test(t));
  assert.equal(w0.length, 1, JSON.stringify(placed.warnings));
  assert.match(w0[0], /^warning: WORM0's turn camera may start inside a wall: ground 1(\.\d+)? map away rises to 18 map above/);
  assert.equal(w1.length, 0);
  const c = checkSpawns(store).find((x) => x.name === "WORM0")!;
  assert.equal(c.standOn, 2);
  assert.ok(c.wall && c.wall.distance <= 2.5 && c.wall.top === 20);
  // a smaller radius (in world units) clears it; list_objects carries the same checks
  assert.equal((await call(s, "list_objects", { wallRadius: 20 })).issues.filter((t: string) => /WORM0's turn camera/.test(t)).length, 0);
  const lo = await call(s, "list_objects", {});
  assert.ok(lo.issues.some((t: string) => /WORM0's turn camera may start inside a wall: ground [\d.]+ world away/.test(t)), JSON.stringify(lo.issues));
  // water over the ground they stand on
  store.scene.water.level = 3 * 20;
  const wet = await call(s, "list_objects", { units: "map" });
  assert.ok(wet.issues.some((t: string) => /WORM1 stands on ground under the water \(ground y 2 map, water y 3 map\)/.test(t)), JSON.stringify(wet.issues));
});
