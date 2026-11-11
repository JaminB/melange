// Tool group "terrain" through the MCP bundle against the mock Oasis server: the tools are listed with their schemas,
// arguments are checked, and a sculpt, a path and an undo run end to end. (The mock's level.save takes only erg-patch/1,
// so saving voxel edits is covered by the live run against the real server.)
import assert from "node:assert/strict";
import { test } from "node:test";
import { fileURLToPath } from "node:url";
import { startMock } from "../../../web/test/e2e/mock-server.mjs";
import { spawnMcp } from "./harness.mjs";

const root = fileURLToPath(new URL("../../../web/dist", import.meta.url));

test("terrain tools over the mock server", async () => {
  const mock = await startMock({ root });
  const m = spawnMcp({ ERG_MCP_URL: mock.url });
  try {
    await m.initialize();
    const list = await m.request("tools/list");
    const names = list.result.tools.map((t) => t.name);
    for (const n of ["sculpt", "sculpt_path", "add_block", "remove_block", "clear_base_terrain", "fill_box_raw", "paint_surround", "read_surround"])
      assert.ok(names.includes(n), n);
    const noProject = await m.tool("sculpt", { mode: "carve", size: [3, 3, 3], center: [0, 0, 0] });
    assert.ok(noProject.isError && /open/i.test(noProject.text), noProject.text);
    const bad = await m.tool("sculpt", { mode: "smash", size: [3, 3, 3], center: [0, 0, 0] });
    assert.ok(bad.isError && /args.mode/.test(bad.text), bad.text);

    assert.ok(!(await m.tool("new_project", { base: "Multi.Synthetic12", slug: "mcpterrain1", title: "Terrain" })).isError);
    assert.ok(!(await m.tool("open_project", { id: "mcpterrain1" })).isError);
    const d = (await m.tool("describe_level", { search: "land2" })).json;
    const f = d.frames[0];
    const mid = f.bounds.world.min.map((v, a) => (v + f.bounds.world.max[a]) / 2);

    const dry = await m.tool("sculpt", { mode: "carve", shape: "sphere", size: [6, 6, 6], center: mid, dryRun: true });
    assert.ok(!dry.isError, dry.text);
    assert.equal(dry.json.dryRun, true);
    const c = await m.tool("sculpt", { mode: "carve", shape: "sphere", size: [6, 6, 6], center: mid });
    assert.ok(!c.isError, c.text);
    assert.equal(c.json.changed, dry.json.changed);
    assert.ok(c.json.changed > 0);
    const after = (await m.tool("describe_level", { search: "land2" })).json.frames[0];
    assert.equal(after.solid, f.solid - c.json.changed);

    const p = await m.tool("sculpt_path", { mode: "carve", size: [2, 2, 2], points: [mid, mid.map((v, a) => v + (a === 1 ? 0 : 60))] });
    assert.ok(!p.isError, p.text);
    assert.ok(p.json.dabs >= 2);
    const u = await m.tool("undo");
    assert.ok(!u.isError && /terrain/.test(u.json.undone), u.text);

    const sg = await m.tool("paint_surround", { op: "raise", points: [[50, 50]], radius: 5, strength: 1 });
    assert.ok(sg.isError && /did not load/.test(sg.text), sg.text);   // the mock serves no .hmp for its base (the live run paints)
    const rs = await m.tool("read_surround", { step: 25 });
    assert.equal(rs.json.grid.length, 4);
    assert.equal(rs.json.canPaint, false);
    const nb = await m.tool("add_block", { size: [4, 4, 4], material: 3, at: [0, 0, 0] });
    assert.ok(nb.isError && /Scene frame/.test(nb.text), nb.text);   // the synthetic level has no Scene frame
    assert.ok(!(await m.tool("close", { discard: true })).isError);
  } finally {
    await m.close();
    await mock.close();
  }
});
