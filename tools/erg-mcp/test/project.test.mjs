// Tool group "project" against the mock Oasis server (web/test/e2e/mock-server.mjs, synthetic levels): connect,
// list_bases, new_project, open_project, describe_level, save, undo/redo refusals, close.
import assert from "node:assert/strict";
import { test } from "node:test";
import { fileURLToPath } from "node:url";
import { startMock } from "../../../web/test/e2e/mock-server.mjs";
import { spawnMcp } from "./harness.mjs";

const root = fileURLToPath(new URL("../../../web/dist", import.meta.url));

test("project tools over the mock server", async () => {
  const mock = await startMock({ root });
  const m = spawnMcp({ ERG_MCP_URL: mock.url });
  try {
    await m.initialize();
    const c = await m.tool("connect");
    assert.ok(!c.isError, c.text);
    assert.equal(c.json.via, "ERG_MCP_URL");
    assert.ok(c.json.ergMethods.includes("level.load"));
    assert.ok(!c.text.includes(mock.token), "the token is not echoed");

    const l = await m.tool("list_bases", { search: "synthetic12" });
    assert.ok(!l.isError, l.text);
    assert.deepEqual(l.json.bases.map((b) => b.key), ["Multi.Synthetic12"]);

    const n = await m.tool("new_project", { base: "Multi.Synthetic12", slug: "mcptest1", title: "MCP Test" });
    assert.ok(!n.isError, n.text);
    assert.equal(n.json.created.id, "mcptest1");
    const again = await m.tool("new_project", { base: "Multi.Synthetic12", slug: "mcptest1", title: "MCP Test" });
    assert.ok(again.isError && /exists/.test(again.text), again.text);
    const badSlug = await m.tool("new_project", { base: "Multi.Synthetic12", slug: "Bad Slug", title: "x" });
    assert.ok(badSlug.isError, badSlug.text);

    const o = await m.tool("open_project", { id: "mcptest1" });
    assert.ok(!o.isError, o.text);
    assert.equal(o.json.voxelFrames, 12);
    assert.deepEqual(o.json.problems, []);

    const d = await m.tool("describe_level");
    assert.ok(!d.isError, d.text);
    const lv = d.json;
    assert.equal(lv.project.id, "mcptest1");
    assert.equal(lv.project.base, "Multi.Synthetic12");
    assert.equal(lv.units.worldPerMap, 20);
    assert.equal(lv.frames.length, 12);
    assert.equal(lv.voxelFrames.total, 12);
    assert.ok(lv.frames.every((f, i) => i === 0 || lv.frames[i - 1].voxels >= f.voxels), "largest first");
    assert.ok(lv.extent.map.min[0] <= Math.min(...lv.frames.map((f) => f.bounds.map.min[0])));
    const one = await m.tool("describe_level", { search: "LAND1", limit: 1 });
    assert.equal(one.json.voxelFrames.matching, 3);   // land1, land10, land11
    assert.equal(one.json.frames.length, 1);
    const f = lv.frames.find((x) => x.id === 4);
    assert.deepEqual(f.size, [7, 2, 11]);
    assert.equal(f.voxels, 7 * 2 * 11);
    assert.ok(f.solid > 0 && f.solid <= f.voxels);
    for (const fr of lv.frames)
      for (let a = 0; a < 3; a++) {
        assert.ok(Math.abs(fr.bounds.world.min[a] - fr.bounds.map.min[a] * 20) < 0.01, "world bounds are map x 20");
        assert.ok(fr.bounds.map.max[a] > fr.bounds.map.min[a]);
      }
    assert.equal(lv.limits.newFrames.max, 64);
    assert.equal(lv.limits.runsPerFrame.max, 2000);
    assert.equal(lv.limits.ops.max, 20000);
    assert.equal(lv.limits.patchBytes.max, 4 << 20);
    assert.equal(lv.limits.objects.max, 256);
    assert.equal(lv.limits.ops.used, 0);
    assert.equal(lv.limits.newFrameSide, 31);
    assert.deepEqual(lv.limits.newFrameScale, { min: 0.5, max: 4 });
    const cells = lv.frames.reduce((n, x) => n + x.voxels, 0);
    assert.deepEqual([lv.limits.voxelCells.used, lv.limits.voxelCells.max, lv.limits.voxelCells.free], [cells, 65535, 65535 - cells]);
    assert.deepEqual(lv.engineWarnings, []);
    assert.equal(lv.dirty, false);
    assert.ok(typeof lv.otherFrames.count === "number");
    const all = await m.tool("describe_level", { allFrames: true });
    assert.ok(Array.isArray(all.json.otherFrames));
    assert.equal(all.json.otherFrames.length + all.json.frames.length, 15);

    const u = await m.tool("undo");
    assert.ok(u.isError && /nothing to undo/.test(u.text), u.text);
    const r = await m.tool("redo");
    assert.ok(r.isError && /nothing to redo/.test(r.text), r.text);

    const saves = mock.state.erg.saves;
    const s = await m.tool("save");
    assert.ok(!s.isError, s.text);
    assert.equal(s.json.saved, true);
    assert.deepEqual(s.json.warnings, []);
    assert.equal(mock.state.erg.saves, saves + 1);
    const saved = mock.state.erg.projects.get("mcptest1").patch;
    assert.equal(saved.format, "erg-patch/1");
    assert.equal(saved.stem, "erg_mcptest1");
    assert.deepEqual(saved.ops, []);

    const cl = await m.tool("close");
    assert.equal(cl.json.closed, "mcptest1");
    assert.ok(mock.state.calls.some((x) => x.m === "level.close" && x.p.project === "mcptest1"));
    const none = await m.tool("describe_level");
    assert.ok(none.isError);
    const reopened = await m.tool("open_project", { id: "mcptest1" });
    assert.ok(!reopened.isError, reopened.text);
    const missing = await m.tool("open_project", { id: "nosuch" });
    assert.ok(missing.isError && /no project 'nosuch'/.test(missing.text), missing.text);
  } finally {
    await m.close();
    await mock.close();
  }
});

test("a wrong token is refused, connect {url} reconnects", async () => {
  const mock = await startMock({ root });
  const m = spawnMcp();
  try {
    await m.initialize();
    const bad = await m.tool("connect", { url: mock.url.replace(/k=.*/, "k=wrong") });
    assert.match(bad.text, /could not connect/);
    assert.ok(bad.isError, bad.text);
    const good = await m.tool("connect", { url: mock.url });
    assert.ok(!good.isError, good.text);
    assert.equal(good.json.via, "url");
    assert.ok((await m.tool("list_bases")).json.bases.length >= 2);
  } finally {
    await m.close();
    await mock.close();
  }
});

test("projects folder, new_project refuses a taken slug, delete_project", async () => {
  const mock = await startMock({ root });
  const m = spawnMcp({ ERG_MCP_URL: mock.url });
  try {
    await m.initialize();
    const c = await m.tool("connect");
    assert.match(c.json.projectsDir, /projects$/);
    assert.ok(!(await m.tool("new_project", { base: "Multi.Synthetic12", slug: "mcpdel1", title: "Del" })).isError);
    assert.match((await m.tool("list_bases")).json.projectsDir, /projects$/);
    const taken = await m.tool("new_project", { base: "Multi.Synthetic12", slug: "mcpdel1", title: "Del" });
    assert.ok(taken.isError && /exists in .*projects: open_project it, delete_project it first, or pass allowRename/.test(taken.text), taken.text);
    assert.ok(!(await m.tool("open_project", { id: "mcpdel1" })).isError);
    const open = await m.tool("delete_project", { id: "mcpdel1" });
    assert.ok(open.isError && /open here: close it first/.test(open.text), open.text);
    await m.tool("close");
    const d = await m.tool("delete_project", { id: "mcpdel1" });
    assert.ok(!d.isError, d.text);
    assert.equal(d.json.deleted, "mcpdel1");
    assert.ok(!(await m.tool("list_bases")).json.projects.some((p) => p.id === "mcpdel1"));
    const gone = await m.tool("delete_project", { id: "mcpdel1" });
    assert.ok(gone.isError && /no project/.test(gone.text), gone.text);
    assert.ok(!(await m.tool("new_project", { base: "Multi.Synthetic12", slug: "mcpdel1", title: "Again" })).isError, "the slug is free again");
    // the game tools are listed (they need the running game's console to do anything)
    const names = (await m.request("tools/list", {})).result.tools.map((t) => t.name);
    for (const n of ["game_state", "game_wait", "game_camera", "list_scenery", "remove_scenery", "list_base_scenery", "copy_base_scenery", "check_reachability", "sculpt_crater", "preview_materials", "delete_project"])
      assert.ok(names.includes(n), n);
  } finally {
    await m.close();
    await mock.close();
  }
});
