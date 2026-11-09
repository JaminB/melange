// surface_sample, and copy_base_details' tag and seat, against the mock Oasis server (synthetic level).
import assert from "node:assert/strict";
import { test } from "node:test";
import { fileURLToPath } from "node:url";
import { startMock } from "../../../web/test/e2e/mock-server.mjs";
import { spawnMcp } from "./harness.mjs";

const root = fileURLToPath(new URL("../../../web/dist", import.meta.url));

test("surface_sample and tagged copies", async () => {
  const mock = await startMock({ root });
  const m = spawnMcp({ ERG_MCP_URL: mock.url });
  try {
    await m.initialize();
    assert.ok(!(await m.tool("new_project", { base: "Multi.Synthetic12", slug: "mcpdress1", title: "Dress" })).isError);
    assert.ok(!(await m.tool("open_project", { id: "mcpdress1" })).isError);
    const d = (await m.tool("describe_level", { search: "land2" })).json.frames[0];
    const mid = d.bounds.world.min.map((v, a) => (v + d.bounds.world.max[a]) / 2);
    const s = await m.tool("surface_sample", { points: [[mid[0], mid[2]], [1e6, 1e6]] });
    assert.ok(!s.isError, s.text);
    assert.ok(Number.isFinite(s.json.samples[0].y) && s.json.samples[0].slope >= 0, s.text);
    assert.equal(s.json.samples[1], null, "over empty space");
    const fp0 = (await m.tool("sim_fingerprint")).json;
    const bs = (await m.tool("scenery_catalog", { limit: 5, sizes: false })).json.list.find((r) => r.role === "scenery");
    if (bs) {
      const r = await m.tool("copy_base_details", { base: bs.base, allowOtherTheme: true, tag: "dtl", copies: [{ id: bs.example.id, xz: [mid[0], mid[2]], seat: 0.2 }] });
      assert.ok(!r.isError, r.text);
      assert.ok(r.json.placed[0].name.endsWith("_dtl"), r.text);
      assert.equal((await m.tool("sim_fingerprint", { against: fp0 })).json.sim, fp0.sim);
      assert.ok((await m.tool("copy_base_details", { base: bs.base, allowOtherTheme: true, tag: "bad tag", copies: [{ id: bs.example.id, xz: [mid[0], mid[2]] }] })).isError);
    }
  } finally { await m.close?.(); await mock.close?.(); }
});
