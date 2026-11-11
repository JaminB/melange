// Tool group "fingerprint" against the mock Oasis server (synthetic level): sim_fingerprint is stable, ignores a scenery
// copy and a primary-material repaint (both visual parts change), changes with a voxel carve (the voxels part and the
// frame named), and comes back after undo.
import assert from "node:assert/strict";
import { test } from "node:test";
import { fileURLToPath } from "node:url";
import { startMock } from "../../../web/test/e2e/mock-server.mjs";
import { spawnMcp } from "./harness.mjs";

const root = fileURLToPath(new URL("../../../web/dist", import.meta.url));

test("sim_fingerprint over the mock server", async () => {
  const mock = await startMock({ root });
  const m = spawnMcp({ ERG_MCP_URL: mock.url });
  try {
    await m.initialize();
    assert.ok((await m.request("tools/list")).result.tools.some((t) => t.name === "sim_fingerprint"));
    assert.ok((await m.tool("sim_fingerprint")).isError, "no project open");
    assert.ok(!(await m.tool("new_project", { base: "Multi.Synthetic12", slug: "mcpfp1", title: "Fingerprint" })).isError);
    assert.ok(!(await m.tool("open_project", { id: "mcpfp1" })).isError);

    const r0 = await m.tool("sim_fingerprint", { detail: true });
    assert.ok(!r0.isError, r0.text);
    const fp0 = r0.json;
    assert.match(fp0.sim, /^[0-9a-f]{64}$/);
    for (const k of ["voxels", "frames", "spawnKnots", "objectKnots", "objects", "water", "script", "databank", "surround"]) assert.ok(fp0.parts[k], k);
    for (const k of ["scenery", "light", "emitter", "sound", "paint"]) assert.ok(fp0.visualParts[k], k);
    const again = (await m.tool("sim_fingerprint", { against: fp0 })).json;
    assert.equal(again.sim, fp0.sim, "stable");
    assert.equal(again.against.same, true);

    // a scenery copy: sim the same, the scenery part changes
    const bs = (await m.tool("list_base_scenery", {})).json;
    const res = JSON.stringify(bs).match(/"resource":"([^"]+)"/)?.[1];
    assert.ok(res, `a base scenery resource: ${JSON.stringify(bs).slice(0, 300)}`);
    const d = (await m.tool("describe_level", { search: "land2" })).json.frames[0];
    const mid = d.bounds.world.min.map((v, a) => (v + d.bounds.world.max[a]) / 2);
    const cp = await m.tool("copy_base_scenery", { copies: [{ resource: res, pos: mid }] });
    assert.ok(!cp.isError, cp.text);
    const fp1 = (await m.tool("sim_fingerprint", { against: fp0 })).json;
    assert.equal(fp1.sim, fp0.sim, `a scenery copy changed the sim: ${JSON.stringify(fp1.against)}`);
    assert.notEqual(fp1.visual, fp0.visual);
    assert.ok(fp1.against.visualParts.includes("scenery") || fp1.against.visualParts.length, JSON.stringify(fp1.against));

    // a primary-material repaint: sim the same, paint changes; strict counts it
    const pt = await m.tool("sculpt", { mode: "paint", shape: "box", size: [4, 4, 4], center: mid, material: 5 });
    assert.ok(!pt.isError, pt.text);
    const fp2 = (await m.tool("sim_fingerprint", { against: fp1 })).json;
    assert.equal(fp2.sim, fp0.sim, `a repaint changed the sim: ${JSON.stringify(fp2.against)}`);
    assert.ok(fp2.against.visualParts.includes("paint"), JSON.stringify(fp2.against));

    // a carve: the sim changes, in the voxels part, naming the frame
    const c = await m.tool("sculpt", { mode: "carve", shape: "sphere", size: [6, 6, 6], center: mid });
    assert.ok(!c.isError && c.json.changed > 0, c.text);
    const fp3 = (await m.tool("sim_fingerprint", { against: fp0, detail: true })).json;
    assert.notEqual(fp3.sim, fp0.sim, "a carve changes the sim");
    assert.deepEqual(fp3.against.simParts, ["voxels"]);
    assert.ok(fp3.against.voxelFrames.includes(d.id), JSON.stringify(fp3.against));

    // undo the carve: back to the sim of before
    assert.ok(!(await m.tool("undo")).isError);
    assert.equal((await m.tool("sim_fingerprint")).json.sim, fp0.sim);
    const strict = (await m.tool("sim_fingerprint", { strict: true })).json;
    assert.ok(strict.parts.paint && !strict.visualParts.paint);
    await m.tool("close", { discard: true });
  } finally {
    await m.close();
    await mock.close();
  }
});
