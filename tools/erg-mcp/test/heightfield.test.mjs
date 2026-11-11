// Tool group "heightfield" over MCP against the mock Oasis server: listed, arguments checked, refusals reach the model.
// (The synthetic levels have no Scene frame, so blocks are refused here: they are tested on the planner in heightfield.test.ts
// and live.)
import assert from "node:assert/strict";
import { test } from "node:test";
import { fileURLToPath } from "node:url";
import { startMock } from "../../../web/test/e2e/mock-server.mjs";
import { spawnMcp } from "./harness.mjs";

const root = fileURLToPath(new URL("../../../web/dist", import.meta.url));

test("heightfield tools over the mock server", async () => {
  const mock = await startMock({ root });
  const m = spawnMcp({ ERG_MCP_URL: mock.url });
  try {
    await m.initialize();
    const names = (await m.request("tools/list", {})).result.tools.map((t) => t.name);
    assert.ok(names.includes("heightfield_plan") && names.includes("heightfield_region"));

    const none = await m.tool("heightfield_plan", { min: [0, 0], max: [8, 8], floor: 0, expr: "4" });
    assert.ok(none.isError && /open_project/.test(none.text), none.text);

    assert.ok(!(await m.tool("new_project", { base: "Multi.Synthetic12", slug: "mcpheightfield1", title: "HF" })).isError);
    assert.ok(!(await m.tool("open_project", { id: "mcpheightfield1" })).isError);
    const missing = await m.tool("heightfield_plan", { min: [0, 0], max: [8, 8] });
    assert.ok(missing.isError && /floor: is required/.test(missing.text), missing.text);
    const badMat = await m.tool("heightfield_plan", { min: [0, 0], max: [8, 8], floor: 0, expr: "4", material: 64 });
    assert.ok(badMat.isError && /at most 63/.test(badMat.text), badMat.text);
    const noScene = await m.tool("heightfield_region", { min: [0, 0], max: [8, 8], floor: 0, expr: "4" });
    assert.ok(noScene.isError && /no Scene frame/.test(noScene.text), noScene.text);
    const d = await m.tool("describe_level", { limit: 0 });
    assert.equal(d.json.dirty, false, "a refused heightfield changes nothing");
  } finally {
    await m.close();
    await mock.close();
  }
});
