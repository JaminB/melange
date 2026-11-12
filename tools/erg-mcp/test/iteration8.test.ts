// The Caldera iteration-8 gaps, on the synthetic 12-frame level (its root renamed Scene so blocks can be added), away
// from its own frames (x 480-700): spawn treads (a knot in solid, with no ground near under it, on an edge) and their
// snap, build_castle's spawnSpots off the wall-walk's edge, check_flood's default dry margin and its capReport,
// set_mine_factory's everyNTurns, the materialFile "theme" keyword, set_water_look's script block, test's simMods,
// game_probe and game_mines {fire} against a stand-in game, and game_shots' onAnnounce with the shown frame.
// Bundled for Node by test.ps1.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, type Loaded, type Scene } from "../src/core";
import { apply, plan } from "../src/lib/heightfield-plan";
import { nearestTread, treadAt } from "../src/lib/spawn-check";
import { grids } from "../src/lib/voxel";
import { expandWaterPreset, readWaterLook, waterLookProblems, withWaterLook, WATER_LOOK_BEGIN } from "../src/lib/water-look";
import { validate } from "../src/registry";
import { tools as gameTools } from "../src/tools/game";
import { themeKeyword, tools as inspectTools } from "../src/tools/inspect";
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
  const saved: string[] = [];
  let text = script;
  const project = { store, id: "t", info: {}, erg: { script: async () => text, saveScript: async (t: string) => { text = t; saved.push(t); return { saved: true, problems: [], syntaxChecked: true }; } } };
  const s = { requireProject: () => project, get store() { return store; }, project,
    levels: async () => { throw new Error("no server"); }, client: async () => { throw new Error("no server"); }, ...extra } as unknown as Session;
  return { store, s, saved, script: () => text };
}
const all = [...inspectTools, ...structureTools, ...placeTools, ...gameTools];
const call = async (s: Session, name: string, args: Record<string, unknown>): Promise<any> => {
  const t = all.find((x) => x.name === name)!;
  const bad = validate(t.inputSchema, args);
  if (bad) throw new Error(bad);
  const r = await t.handler(s, args);
  if (r && typeof r === "object" && "content" in (r as object)) {
    const parts = (r as { content: { type: string; text?: string }[] }).content;
    return { ...JSON.parse(parts.find((p) => p.type === "text")!.text!), images: parts.filter((p) => p.type === "image").length };
  }
  return typeof r === "string" ? JSON.parse(r) : r;
};
// a slab 2 map units high over x 480-520, z -12-12, on the floor at 0
const SLAB = { min: [480, -12] as [number, number], max: [520, 12] as [number, number], floor: 0, expr: "2", material: 1 };

test("spawn treads: a knot in solid, with no ground near under it, on an edge, and the nearest point that passes", () => {
  const { store } = session();
  apply(store, plan(store, SLAB));
  const gs = grids(store);
  assert.deepEqual(treadAt(gs, [500, 2, 0]), { ok: true, problems: [], kinds: [], standOn: 2 });
  const buried = treadAt(gs, [500, 1, 0]);
  assert.deepEqual(buried.kinds, ["inSolid"]);
  assert.match(buried.problems[0], /inside solid ground/);
  assert.deepEqual(treadAt(gs, [470, 8, 0]).kinds, ["noGround"]);
  assert.deepEqual(treadAt(gs, [500, 7, 0]).kinds, ["noGround"], "ground 5 under the knot is a drop");
  const edge = treadAt(gs, [480.1, 2, 0]);
  assert.deepEqual(edge.kinds, ["edge"]);
  assert.match(edge.problems[0], /narrower than a worm/);
  // the nearest points that pass: on top of the slab over the buried knot, half a worm in from the edge
  assert.deepEqual(nearestTread(gs, [500, 1, 0]), [500, 2, 0]);
  const off = nearestTread(gs, [480.1, 2, 0])!;
  assert.ok(off[0] >= 480.4 && off[0] <= 480.7 && off[1] === 2 && treadAt(gs, off).ok, JSON.stringify(off));
  assert.equal(nearestTread(gs, [440, 2, 0]), null, "no ground within 3 map units");
});

test("place_spawn_knots warns of a knot a worm cannot stand on, and snap: tread moves it; save says so too", async () => {
  const { store, s } = session();
  apply(store, plan(store, SLAB));
  const r = await call(s, "place_spawn_knots", { units: "map", spawns: [{ worm: 0, pos: [500, 1, 0] }, { worm: 1, pos: [480.1, 2, 0] }, { worm: 2, pos: [505, 2, 5] }] });
  const w = r.warnings.filter((l: string) => /WORM[01] at/.test(l));
  assert.equal(w.length, 2, JSON.stringify(r.warnings));
  assert.match(w[0], /WORM0 at \[500, 1, 0\] map: the knot is inside solid ground.*the nearest point a worm stands on is \[500, 2, 0\] map/);
  assert.match(w[1], /WORM1 .*on an edge narrower than a worm/);
  assert.ok(!r.warnings.some((l: string) => /WORM2 at/.test(l)));
  const snapped = await call(s, "place_spawn_knots", { units: "map", snap: "tread", spawns: [{ worm: 0, pos: [500, 1, 0] }, { worm: 1, pos: [480.1, 2, 0] }] });
  assert.equal(snapped.snapped.length, 2);
  assert.deepEqual(snapped.snapped[0].to, [500, 2, 0]);
  assert.ok(!snapped.warnings.some((l: string) => /WORM[01] at/.test(l)), JSON.stringify(snapped.warnings));
  await assert.rejects(() => call(s, "place_spawn_knots", { units: "map", snap: "tread", spawns: [{ worm: 3, pos: [440, 2, 0] }] }), /no point within 3 map units passes/);
  // save warns of the knots left in solid
  await call(s, "place_spawn_knots", { units: "map", spawns: [{ worm: 0, pos: [500, 1, 0] }] });
  const { spawnTreadWarnings } = await import("../src/tools/place");
  assert.ok(spawnTreadWarnings(store).some((l) => /WORM0 at \[10000, 20, 0\] world: the knot is inside solid ground/.test(l)), JSON.stringify(spawnTreadWarnings(store)));
});

test("build_castle's spawnSpots: a spot is moved off a wall-walk's edge to where a worm stands", async () => {
  const { s } = session();
  const r = await call(s, "build_castle", { at: [560, 0, 0], size: [24, 24], wallHeight: 6, wallThickness: 2, keep: false, place: "new" });
  for (const sp of r.spawnSpots.spots as { ok: boolean; standOn: number[]; tread?: { snapped: boolean } }[]) {
    if (!sp.ok) continue;
    const t = treadAt(grids(s.store), sp.standOn as [number, number, number]);
    assert.ok(t.ok, `${JSON.stringify(sp)}: ${t.problems.join("; ")}`);
  }
  assert.ok(r.spawnSpots.spots.some((sp: { ok: boolean }) => sp.ok), JSON.stringify(r.spawnSpots.spots));
});

test("check_flood: the verdict's dry margin is 1.5 map units by default, and capReport lists the spawns near the cap", async () => {
  const { store, s } = session();
  apply(store, plan(store, { min: [480, -12], max: [600, 12], floor: 0, expr: "2 + max(0, x + 20)*0.4", material: 1 }));
  await call(s, "place_spawn_knots", { units: "map", spawns: [{ worm: 0, xz: [482, 0] }, { worm: 1, xz: [595, 0] }] });
  const base = { units: "map", start: 1, x0: 480, z0: -12, x1: 600, z1: 12, wormsPerTeam: 1, teams: 2 };
  const r = await call(s, "check_flood", { ...base, schedule: { startTurn: 2, step: 0.34, cap: 3, units: "map" } });
  assert.equal(r.floodVerdict.minDryMargin, 1.5);
  assert.equal(r.floodVerdict.pass, false);
  assert.match(r.floodVerdict.failures[0], /^WORM0: only 1 over the water \(minDryMargin 1\.5\) at its own turn 1/);
  const cap = r.flood.capReport;
  assert.deepEqual(cap.cap, { map: 3, world: 60 });
  assert.equal(cap.capMargin, 3);
  assert.deepEqual(cap.within, ["WORM0"]);
  assert.equal(cap.spawns[0].name, "WORM0");
  assert.equal(cap.spawns[0].overCap.map, -1);
  assert.match(r.summary, /within 3 map of the water's cap: WORM0/);
  const wide = await call(s, "check_flood", { ...base, schedule: { startTurn: 2, step: 0.34, cap: 3, units: "map" }, capMargin: 100 });
  assert.deepEqual(wide.flood.capReport.within, ["WORM0", "WORM1"]);
});

test("set_mine_factory everyNTurns: activation 255 and inactiveTurns N", async () => {
  const { store, s } = session();
  apply(store, plan(store, SLAB));
  await call(s, "place_object", { type: "minefactory", units: "map", pos: [500, 2, 0] });
  const r = await call(s, "set_mine_factory", { everyNTurns: 3 });
  assert.deepEqual(r.factory, { activation: 255, inactiveTurns: 3 });
  assert.deepEqual(r.everyNTurns.mapsTo, { activation: 255, inactiveTurns: 3 });
  assert.deepEqual(r.everyNTurns.firesAtGameTurns, [3, 6, 9, 12]);
  assert.match(r.rule, /min\(activation - mines, 10\)/);
  await assert.rejects(() => call(s, "set_mine_factory", { everyNTurns: 2, activation: 30 }), /everyNTurns sets activation and inactiveTurns/);
});

test("materialFile \"theme\": the theme's own file, from theme, base or the open project, else an error naming the path", async () => {
  const levels = async () => ({ list: async () => ({ bases: [{ key: "Multi.DinerMight", theme: "BUILDING" }] }) });
  const none = { project: undefined, levels } as unknown as Session;
  assert.deepEqual(await themeKeyword(none, { materialFile: "theme", theme: "PREHISTORIC" }),
    { materialFile: "ThemePrehistoric\\ThemePrehistoric.txt", theme: "PREHISTORIC", note: "materialFile \"theme\": the PREHISTORIC theme's own file, ThemePrehistoric\\ThemePrehistoric.txt" });
  assert.equal((await themeKeyword(none, { materialFile: "THEME", base: "Multi.DinerMight" })).materialFile, "ThemeBuilding\\ThemeBuilding.txt");
  await assert.rejects(() => themeKeyword(none, { materialFile: "theme" }), /Theme<Name>\\Theme<Name>\.txt \(for PREHISTORIC: ThemePrehistoric\\ThemePrehistoric\.txt\): pass theme, or base/);
  await assert.rejects(() => themeKeyword(none, { materialFile: "theme", base: "Multi.Nowhere" }), /no base level 'Multi.Nowhere'/);
  // other files pass through
  assert.deepEqual(await themeKeyword(none, { materialFile: "Maps\\dinermight.txt" }), { materialFile: "Maps\\dinermight.txt", theme: undefined });
  const { s } = session();
  const own = await themeKeyword(s, { materialFile: "theme" });
  assert.equal(own.materialFile, `Theme${own.theme!.charAt(0)}${own.theme!.slice(1).toLowerCase()}\\Theme${own.theme!.charAt(0)}${own.theme!.slice(1).toLowerCase()}.txt`);
});

test("set_water_look: one wum.level.water block at the top of the script, rewritten in place and cleared", async () => {
  assert.deepEqual(waterLookProblems({ NearOpacity: 2, LightAmbient: [1, 2], Fog: 1 }), [
    "NearOpacity: a number from 0 to 1", "LightAmbient: {r, g, b} 0-255: the land's ambient light (the landscape shader's globalAmbient)", "Fog: not a water field (NearOpacity, ReflectionStrength, ReflectionContrast, SubtractColourScale, SpecularContrast, SpecularPower, SpecularFadeScale, TextureScale, LandSpecularPower, LandFresnelPower, SkyBlendColor, LightAmbient, LightDiffuse, LightSpecular, LightFresnel, LandFresnelColour, NormalIntensity, NormalIntensity1, LightDirection)"]);
  const user = "-- my level\nwum.sim.waterSchedule{startTurn = 5, step = 20, cap = 200}\n";
  const one = withWaterLook(user, { NearOpacity: 0.95, LightAmbient: [40, 80, 120] });
  assert.ok(one.startsWith(WATER_LOOK_BEGIN) && one.endsWith(user), one);
  assert.match(one, /wum\.level\.water\{LightAmbient = \{40, 80, 120\}, NearOpacity = 0\.95\}/);
  assert.deepEqual(readWaterLook(one), { LightAmbient: [40, 80, 120], NearOpacity: 0.95 });
  const two = withWaterLook(one, { ReflectionStrength: 0.2 });
  assert.equal(two.split(WATER_LOOK_BEGIN).length, 2, "one block");
  assert.deepEqual(readWaterLook(two), { ReflectionStrength: 0.2 });
  assert.equal(withWaterLook(two, null), user);

  const { s, script } = session({}, user);
  const r = await call(s, "set_water_look", { opacity: 1, reflection: 0.3, fields: { SubtractColourScale: 0.5 } });
  assert.equal(r.changed, true);
  assert.deepEqual(readWaterLook(script()), { NearOpacity: 1, ReflectionStrength: 0.3, SubtractColourScale: 0.5 });
  // the named ones: shade and the land's light
  const land = await call(s, "set_water_look", { opacity: 1, reflection: 0.3, shade: 0.5, landAmbient: [90, 60, 40], landDiffuse: [255, 240, 220] });
  assert.deepEqual(readWaterLook(script()), { LightAmbient: [90, 60, 40], LightDiffuse: [255, 240, 220], NearOpacity: 1, ReflectionStrength: 0.3, SubtractColourScale: 0.5 });
  assert.deepEqual(land.was, { NearOpacity: 1, ReflectionStrength: 0.3, SubtractColourScale: 0.5 });
  await call(s, "set_water_look", { opacity: 1, reflection: 0.3, fields: { SubtractColourScale: 0.5 } });
  assert.ok(script().endsWith(user));
  assert.equal((await call(s, "set_water_look", { opacity: 1, reflection: 0.3, fields: { SubtractColourScale: 0.5 } })).changed, false);
  await assert.rejects(() => call(s, "set_water_look", { fields: { Haze: 1 } }), /Haze: not a water field/);
  await assert.rejects(() => call(s, "set_water_look", {}), /give a preset, opacity, reflection, shade, landAmbient, landDiffuse or fields/);
  assert.equal((await call(s, "set_water_look", { clear: true })).look, null);
  assert.equal(script(), user);
});

test("test simMods: level-only is the sim's clean room; contradictions are refused before anything is sent", async () => {
  const { s } = session();
  await assert.rejects(() => call(s, "test", { simMods: "level-only", cleanMods: false }), /leave out cleanMods: false/);
  await assert.rejects(() => call(s, "test", { simMods: "all", cleanMods: true }), /give one of them/);
});

// a stand-in game for the probe and the factory's fire on demand
const PROBE = {
  probe: { key: "Multi.ErgTest_t", playing: true, turn: 7, tick: 9000,
    spawns: [{ slot: 0, team: 0, posInTeam: 0, name: "A", pos: [500, 2, 0], posWorld: [10000, 40, 0], health: 100, state: 0 },
      { slot: 1, team: 1, posInTeam: 0, name: "B", pos: [505, 2.1, 5], posWorld: [10100, 42, 100], health: 100, state: 0 }],
    deaths: [{ slot: 1, team: 1, posInTeam: 0, name: "B", tick: 8000, turn: 6, pos: [505, -1, 5], posWorld: [10100, -20, 100], lastAlivePos: [505, 1.7, 5], health: 100, state: 8,
      cause: "drown", evidence: ["the engine's DrownFloat state"], waterMap: 2, waterWorld: 40,
      hits: [{ explosion: { tick: 7000, turn: 5, at: [504, 2, 5], wormDamage: 30, wormDamageRadiusMap: 3, source: "mine", entity: { handle: 77, kind: "mine", type: "?", label: "Landmine", at: [504, 2, 5] } }, distanceMap: 1.2 }],
      falls: [], healthSteps: [{ tick: 7001, from: 100, to: 70 }] }],
    explosionsSeen: 1, explosions: [] },
  factory: { fires: [{ stem: "ergtest_t", tick: 8500, turn: 6, asked: 3, launch: true, factory: [500, 2, 0],
    mines: [{ handle: 90, spawn: [500, 22, 0], rest: [502, 2, 1], settled: true, restFromFactoryMap: 2.24 }] }], following: 0,
    trigger: { tick: 8400, count: 3, started: true, why: "", detail: "mines in the level 0, target 255" } },
};
function gameSession(methods: string[], calls: { m: string; p: unknown }[]) {
  const client = { call: async (m: string, p: unknown) => { calls.push({ m, p }); if (m === "level.test.probe") return PROBE; if (m === "level.test.fireFactory") return { queued: true }; if (m === "entities.list") return []; if (m === "bus.names") return { names: ["MineFactory.FireEnd"] }; return {}; },
    has: () => false, subscribe: () => () => undefined };
  return session({ ensure: async () => ({ methods }), client: async () => client });
}

test("game_probe: the worms at turn 1 against their knots, and each death with its cause", async () => {
  const calls: { m: string; p: unknown }[] = [];
  const { store, s } = gameSession(["level.test.probe"], calls);
  apply(store, plan(store, SLAB));
  await call(s, "place_spawn_knots", { units: "map", spawns: [{ worm: 0, pos: [500, 2, 0] }, { worm: 1, pos: [505, 2, 5] }] });
  const r = await call(s, "game_probe", {});
  assert.deepEqual(r.spawns.map((x: { worm: string }) => x.worm), ["WORM0", "WORM1"]);
  assert.equal(r.spawns[1].fromKnot, 0.1);
  assert.equal(r.deaths[0].worm, "WORM1");
  assert.equal(r.deaths[0].cause, "drown");
  assert.deepEqual(r.deaths[0].water, { map: 2, world: 40, feetOverWater: -0.3 });
  assert.equal(r.deaths[0].hits[0].source, "mine");
  assert.equal(r.factory.fires[0].each[0].restFromFactory, 2.24);
  const w = await call(s, "game_probe", { units: "world" });
  assert.deepEqual(w.spawns[0].at, [10000, 40, 0]);
  await assert.rejects(() => call(gameSession([], []).s, "game_probe", {}), /no level\.test\.probe/);
});

test("set_water_look presets: sand expands to the verified fields, explicit fields win, unverified themes are reported", async () => {
  const e = expandWaterPreset("sand", undefined, {}, "ARABIAN", "DAY");
  assert.equal(e.verified, true);
  assert.deepEqual([e.look.NearOpacity, e.look.ReflectionStrength], [1, 0]);
  assert.equal(expandWaterPreset("sand", undefined, {}, "ARABIAN", "EVENING").verified, false);
  assert.equal(expandWaterPreset("lava", undefined, {}, "ARABIAN", "DAY").verified, false);
  assert.throws(() => expandWaterPreset("ice", undefined, {}, "ARABIAN", "DAY"), /preset 'ice' is not one of sand, lava, murk/);
  const user = "-- mine\n";
  const { s, script } = session({}, user);
  await call(s, "set_theme", { theme: "ARABIAN" }); await call(s, "set_time_of_day", { timeOfDay: "DAY" });
  const r = await call(s, "set_water_look", { preset: "sand", tint: [200, 170, 120], reflection: 0.5 });
  assert.equal(r.preset.verified, true);
  assert.deepEqual(r.preset.overridden, ["ReflectionStrength"]);
  const look = readWaterLook(script())!;
  assert.deepEqual([look.NearOpacity, look.ReflectionStrength], [1, 0.5]);
  assert.deepEqual([look.LightDiffuse, look.LightAmbient], [[200, 170, 120], [100, 85, 60]]);
  assert.ok(script().endsWith(user));
  await call(s, "set_theme", { theme: "PREHISTORIC" }); await call(s, "set_time_of_day", { timeOfDay: "EVENING" });
  const u = await call(s, "set_water_look", { preset: "sand" });
  assert.equal(u.preset.verified, false);
  assert.match(u.preset.note, /UNVERIFIED on PREHISTORIC EVENING \(tried on ARABIAN DAY\)/);
  await assert.rejects(() => call(s, "set_water_look", { tint: [1, 2, 3] }), /tint goes with a preset/);
  await assert.rejects(() => call(s, "set_water_look", { preset: "sand", clear: true }), /give no preset/);
  await assert.rejects(() => call(s, "set_water_look", { preset: "ice" }), /must be one of sand, lava, murk/);
});
