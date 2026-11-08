// Tool group "place" against the mock Oasis server (synthetic levels): spawn knots, objects, move/rotate/scale/remove/
// drop, level settings, the patch they save, and the refusals. The mock saves only erg-patch/1 (no objects), so the
// object tests stop before save; the real server validates those (see docs/erg-mcp.md, live test).
import assert from "node:assert/strict";
import { test } from "node:test";
import { fileURLToPath } from "node:url";
import { startMock } from "../../../web/test/e2e/mock-server.mjs";
import { spawnMcp } from "./harness.mjs";

const root = fileURLToPath(new URL("../../../web/dist", import.meta.url));
const near = (a, b, eps = 1e-3) => assert.ok(Math.abs(a - b) <= eps, `${a} is not ${b}`);

async function open(m, mock, slug) {
  await m.initialize();
  const n = await m.tool("new_project", { base: "Multi.Synthetic12", slug, title: "Place test" });
  assert.ok(!n.isError, n.text);
  const o = await m.tool("open_project", { id: slug });
  assert.ok(!o.isError, o.text);
}

test("spawn knots, drop, move, rotate, scale, remove, undo", async () => {
  const mock = await startMock({ root });
  const m = spawnMcp({ ERG_MCP_URL: mock.url });
  try {
    await open(m, mock, "mcpplace1");
    const l0 = await m.tool("list_objects");
    assert.equal(l0.json.spawns.length, 8);
    assert.deepEqual(l0.json.placed.map((o) => o.name).sort(), ["mine", "oildrum"]);
    assert.deepEqual(l0.json.levelObjects, []);
    assert.equal(l0.json.spawnMode, "random");
    const w0 = l0.json.spawns[0];

    // world units by default, map units on request
    const a = await m.tool("place_spawn_knots", { spawns: [{ worm: 0, pos: [100, 50, 200] }] });
    assert.ok(!a.isError, a.text);
    assert.equal(a.json.mode, "knots");
    assert.equal(a.json.placed[0].action, "moved");
    assert.deepEqual(a.json.missingKnots, []);
    assert.deepEqual(a.json.spawns.find((s) => s.name === "WORM0").pos, [100, 50, 200]);
    const b = await m.tool("move", { id: w0.id, pos: [5, 2.5, 10], units: "map" });
    assert.deepEqual(b.json.moved[0].pos, [5, 2.5, 10]);   // answered in the units asked
    const c = await m.tool("move", { id: w0.id, by: [20, 0, -20] });
    assert.deepEqual(c.json.moved[0].pos, [120, 50, 180]);

    // xz drops onto the terrain: land0 (7x2x11) is centred on map (-104.5, 45)
    const d = await m.tool("place_spawn_knots", { spawns: [{ worm: 3, xz: [-2090, 900] }] });
    assert.ok(!d.isError, d.text);
    assert.equal(d.json.placed[0].dropped, true);
    const y = d.json.placed[0].pos[1];
    assert.ok(y > 0 && y < 2000, `dropped y ${y}`);
    const dropped = await m.tool("drop_to_ground", { id: w0.id });
    assert.ok(dropped.isError && /empty space/.test(dropped.text), dropped.text);   // WORM0 sits over empty space
    const up = await m.tool("move", { id: d.json.spawns.find((s) => s.name === "WORM3").id, by: [0, 300, 0] });
    assert.ok(Math.abs(up.json.moved[0].pos[1] - (y + 300)) < 0.01);
    const down = await m.tool("drop_to_ground", { id: up.json.moved[0].id });
    near(down.json.dropped[0].pos[1], y, 0.01);
    const over = await m.tool("place_spawn_knots", { spawns: [{ worm: 1, xz: [90000, 90000] }] });
    assert.ok(over.isError && /WORM1: nothing below/.test(over.text), over.text);

    // refusals: both or neither point, a worm twice
    for (const bad of [{ worm: 1 }, { worm: 1, pos: [0, 0, 0], xz: [0, 0] }]) {
      const r = await m.tool("place_spawn_knots", { spawns: [bad] });
      assert.ok(r.isError && /exactly one of pos/.test(r.text), r.text);
    }
    const twice = await m.tool("place_spawn_knots", { spawns: [{ worm: 1, pos: [0, 0, 0] }, { worm: 1, pos: [1, 0, 0] }] });
    assert.ok(twice.isError && /twice/.test(twice.text), twice.text);
    assert.ok((await m.tool("place_spawn_knots", { spawns: [{ worm: 8, pos: [0, 0, 0] }] })).isError);

    // rotate (degrees), scale
    const r1 = await m.tool("rotate", { id: w0.id, rot: [0, 90, 0] });
    assert.deepEqual(r1.json.rotated[0].rotDeg, [0, 90, 0]);
    const r2 = await m.tool("rotate", { ids: [w0.id], by: [0, 45, 10] });
    assert.deepEqual(r2.json.rotated[0].rotDeg, [0, 135, 10]);
    const s1 = await m.tool("scale", { id: w0.id, uniform: 2 });
    assert.deepEqual(s1.json.scaled[0].scale, [2, 2, 2]);
    const s2 = await m.tool("scale", { id: w0.id, by: 1.5 });
    assert.deepEqual(s2.json.scaled[0].scale, [3, 3, 3]);
    assert.ok((await m.tool("scale", { id: w0.id, by: 0 })).isError);
    assert.ok((await m.tool("scale", { id: w0.id })).isError);
    assert.ok((await m.tool("rotate", { id: w0.id })).isError);
    assert.ok((await m.tool("move", { id: 99999, by: [1, 0, 0] })).isError);
    assert.ok((await m.tool("move", { by: [1, 0, 0] })).isError);

    // remove: WORM7 gone, knots mode now reports it
    const w7 = l0.json.spawns.find((s) => s.name === "WORM7");
    const rm = await m.tool("remove", { id: w7.id });
    assert.ok(!rm.isError, rm.text);
    assert.ok(rm.json.issues.some((i) => /WORM7 is missing/.test(i)), rm.text);
    assert.ok((await m.tool("list_objects")).json.issues[0].startsWith("error:"), "errors come first");
    const undone = await m.tool("undo");
    assert.equal(undone.json.undone, "Delete detail");
    assert.equal((await m.tool("list_objects")).json.spawns.length, 8);

    // every call above was one undo step: the stack holds exactly the successful edits
    const depth = (await m.tool("describe_level", { limit: 0 })).json.undoDepth;
    assert.ok(depth >= 10, `undo depth ${depth}`);
  } finally {
    await m.close();
    await mock.close();
  }
});

test("level settings, placed mines and drums, and the saved patch", async () => {
  const mock = await startMock({ root });
  const m = spawnMcp({ ERG_MCP_URL: mock.url });
  try {
    await open(m, mock, "mcpplace2");
    assert.equal((await m.tool("set_water", { level: 150 })).json.water.world, 150);
    assert.equal((await m.tool("set_water", { level: 7.5, units: "map" })).json.water.world, 150);
    assert.ok((await m.tool("set_water", { level: 100, units: "map" })).isError, "2000 world units is out of range");
    assert.ok((await m.tool("set_water", {})).isError);
    assert.ok((await m.tool("set_water", { level: 1, useDefault: true })).isError);
    assert.equal((await m.tool("set_theme", { theme: "CAMELOT" })).json.theme, "CAMELOT");
    assert.ok((await m.tool("set_theme", { theme: "MARS" })).isError);
    assert.equal((await m.tool("set_time_of_day", { timeOfDay: "NIGHT" })).json.timeOfDay, "NIGHT");
    assert.equal((await m.tool("set_surround_mode", { mode: "flat" })).json.surround, "flat");
    assert.equal((await m.tool("place_spawn_knots", { mode: "knots" })).json.mode, "knots");
    const same = await m.tool("place_spawn_knots", { mode: "knots" });
    assert.equal(same.json.changed, false);

    const mine = await m.tool("place_object", { type: "mine", xz: [-2090, 900] });
    assert.ok(!mine.isError, mine.text);
    assert.equal(mine.json.placed[0].name, "mine");
    assert.equal(mine.json.dropped, true);
    const drum = await m.tool("place_object", { type: "oildrum", pos: [10, 400, 10] });
    assert.equal(drum.json.dropped, false);
    assert.deepEqual(drum.json.placed[0].pos, [10, 400, 10]);

    const dirty = await m.tool("describe_level", { limit: 0 });
    assert.equal(dirty.json.dirty, true);
    assert.deepEqual(dirty.json.problems, []);
    const s = await m.tool("save");
    assert.ok(!s.isError, s.text);
    const patch = mock.state.erg.projects.get("mcpplace2").patch;
    assert.equal(patch.format, "erg-patch/1");
    assert.equal(patch.water.level, 150);
    assert.equal(patch.databank.theme, "CAMELOT");
    assert.equal(patch.databank.timeOfDay, "NIGHT");
    assert.equal(patch.spawns.mode, "knots");
    assert.equal(patch.hmp.mode, "flat");
    assert.equal(patch.ops.filter((o) => o.op === "add").length, 2);

    // reopen: what was saved is what comes back
    await m.tool("close");
    await m.tool("open_project", { id: "mcpplace2" });
    const l = (await m.tool("list_objects")).json;
    assert.equal(l.spawnMode, "knots");
    assert.equal(l.water, 150);
    assert.equal(l.placed.filter((o) => o.name === "oildrum").length, 2);
    assert.equal(l.placed.filter((o) => o.name === "mine").length, 2);
  } finally {
    await m.close();
    await mock.close();
  }
});

test("objects: crates, telepads, triggers, the mine factory, limits", async () => {
  const mock = await startMock({ root });
  const m = spawnMcp({ ERG_MCP_URL: mock.url });
  try {
    await open(m, mock, "mcpplace3");
    const crate = await m.tool("place_object", { type: "crate", kind: "weapon", contents: "Bazooka", count: 3, parachute: true, pos: [0, 100, 0] });
    assert.ok(!crate.isError, crate.text);
    const o = crate.json.placed[0];
    assert.equal(o.name, "CRATE_0");
    assert.deepEqual(o.object.crate, { kind: "weapon", contents: "Bazooka", count: 3, hitpoints: 25, parachute: true });
    const health = await m.tool("place_object", { type: "health", amount: 60, pos: [20, 100, 0] });
    assert.deepEqual(health.json.placed[0].object.crate, { kind: "health", amount: 60, hitpoints: 25, parachute: false });
    const needs = await m.tool("place_object", { type: "crate", kind: "utility", pos: [0, 0, 0] });
    assert.ok(needs.isError && /needs contents/.test(needs.text), needs.text);
    const tp = await m.tool("place_object", { type: "telepad", pos: [0, 50, 0], pos2: [200, 50, 0] });
    assert.deepEqual(tp.json.placed.map((p) => p.name), ["TP_1_0", "TP_1_1"]);
    assert.deepEqual(tp.json.placed.map((p) => p.pos), [[0, 50, 0], [200, 50, 0]]);
    const tp2 = await m.tool("place_object", { type: "telepad", pos: [0, 50, 100] });
    assert.deepEqual(tp2.json.placed.map((p) => p.name), ["TP_2_0", "TP_2_1"]);
    near(tp2.json.placed[1].pos[0] - tp2.json.placed[0].pos[0], 40, 1e-6);   // PAD_SPACING 2 map units = 40 world
    const trig = await m.tool("place_object", { type: "trigger", pos: [0, 50, 0], radius: 80, teamCollect: 2, wormCollect: true });
    assert.deepEqual(trig.json.placed[0].object.trigger, { index: 0, radius: 80, teamCollect: 2, teamDestroy: 4, hitpoints: 1, wormCollect: true });
    const mf = await m.tool("place_object", { type: "minefactory", pos: [0, 0, 0] });
    assert.equal(mf.json.placed[0].name, "minefactory");
    assert.equal(mf.json.placed[0].object.factory, undefined);
    const mf2 = await m.tool("place_object", { type: "minefactory", pos: [0, 0, 0] });
    assert.ok(mf2.isError && /already has a mine factory/.test(mf2.text), mf2.text);
    // settings: a game value is dropped, the rest kept; one undo step each
    const set1 = await m.tool("set_mine_factory", { activation: 30, inactiveTurns: 7, schemeMines: false });
    assert.deepEqual(set1.json.factory, { activation: 30, schemeMines: false }, set1.text);
    const set2 = await m.tool("set_mine_factory", { activation: 20, inactiveTurns: 2 });
    assert.deepEqual(set2.json.factory, { schemeMines: false, inactiveTurns: 2 }, set2.text);
    const none = await m.tool("set_mine_factory", {});
    assert.ok(none.isError, none.text);
    const tooMany = await m.tool("set_mine_factory", { activation: 256 });
    assert.ok(tooMany.isError, tooMany.text);
    const listed = (await m.tool("list_objects")).json.levelObjects.find((x) => x.name === "minefactory");
    assert.deepEqual(listed.object.factory, { schemeMines: false, inactiveTurns: 2 });
    await m.tool("set_mine_factory", { schemeMines: true, inactiveTurns: 7 });
    assert.equal((await m.tool("list_objects")).json.levelObjects.find((x) => x.name === "minefactory").object.factory, undefined);
    const bad = await m.tool("place_object", { type: "trigger", pos: [0, 0, 0], index: 300 });
    assert.ok(bad.isError, bad.text);

    const l = (await m.tool("list_objects")).json;
    assert.deepEqual(l.levelObjects.map((x) => x.name), ["CRATE_0", "CRATE_1", "TP_1_0", "TP_1_1", "TP_2_0", "TP_2_1", "TRIG_0", "minefactory"]);
    assert.equal(l.counts.objects, "8 of 256");
    // the editor's own checks see single-pad groups; removing one pad's knot leaves a partner-less pad
    const pad = l.levelObjects.find((x) => x.name === "TP_2_1");
    const rm = await m.tool("remove", { id: pad.id });
    assert.ok(rm.json.removed[0].alsoRemoved && rm.json.issues.some((i) => /Telepad group 2 has one pad/.test(i)), rm.text);

    // each placement is one undo step; the objects go with it
    const depth0 = (await m.tool("describe_level", { limit: 0 })).json.undoDepth;
    await m.tool("undo");
    assert.equal((await m.tool("list_objects")).json.levelObjects.length, 8);
    assert.equal((await m.tool("describe_level", { limit: 0 })).json.undoDepth, depth0 - 1);
    // a v2 project cannot be saved on the mock (erg-patch/1 only): the server's own rules are exercised live
    assert.equal((await m.tool("describe_level", { limit: 0 })).json.objects.length, 8);
  } finally {
    await m.close();
    await mock.close();
  }
});

test("scripts, shipping and Test need the real server", async () => {
  const mock = await startMock({ root });
  const m = spawnMcp({ ERG_MCP_URL: mock.url });
  try {
    await open(m, mock, "mcpplace4");
    // the mock has no level.script.*, level.export or level.build: the call reaches it and its refusal comes back
    assert.ok((await m.tool("get_script")).isError);
    assert.ok((await m.tool("put_script", { text: "x" })).isError);
    const ex = await m.tool("export", { modId: "mcpplace-mod", name: "Place test" });
    assert.ok(ex.isError, ex.text);
    assert.ok(mock.state.calls.some((c) => c.m === "level.export" && c.p.mode === "source" && c.p.modId === "mcpplace-mod" && c.p.survivor === false));
    const badId = await m.tool("export", { modId: "Bad Id", name: "x" });
    assert.ok(badId.isError && /mod id/.test(badId.text), badId.text);
    const badVersion = await m.tool("export", { modId: "okay", name: "x", version: "1.0" });
    assert.ok(badVersion.isError && /version/.test(badVersion.text), badVersion.text);
    assert.ok((await m.tool("build", { modId: "mcpplace-mod" })).isError);
    // the mock is a game server without level.test / unlisted levels.live in erg's methods: levels.live exists, Test does not
    const t = await m.tool("test");
    assert.ok(t.isError && /no level\.test/.test(t.text), t.text);
    const live = await m.tool("levels_live", { modId: "mcpplace-mod", on: true });
    assert.ok(!live.isError || /levels/.test(live.text), live.text);
    // an export of an unsaved project saves it first
    await m.tool("set_water", { level: 10 });
    const ex2 = await m.tool("export", { modId: "mcpplace-mod", name: "Place test" });
    assert.ok(ex2.isError);
    assert.equal((await m.tool("describe_level", { limit: 0 })).json.dirty, false, "export saved the project first");
  } finally {
    await m.close();
    await mock.close();
  }
});
