// sim_fingerprint must FAIL when the server connection is gone (round 6: a dead connection answered the scene's script
// metadata hash, a different `script` part, which a detail pass read as a sim change). Only a server without
// level.script.get (the mock: unknown method) may use the metadata.
import assert from "node:assert/strict";
import { test } from "node:test";
import { fileURLToPath } from "node:url";
import { startMock } from "../../../web/test/e2e/mock-server.mjs";
import { spawnMcp } from "./harness.mjs";

const root = fileURLToPath(new URL("../../../web/dist", import.meta.url));

test("sim_fingerprint fails hard on a lost connection", async () => {
  const mock = await startMock({ root });
  const m = spawnMcp({ ERG_MCP_URL: mock.url });
  try {
    await m.initialize();
    assert.ok(!(await m.tool("new_project", { base: "Multi.Synthetic12", slug: "mcpfp2", title: "Fingerprint dead" })).isError);
    assert.ok(!(await m.tool("open_project", { id: "mcpfp2" })).isError);
    const ok = await m.tool("sim_fingerprint");
    assert.ok(!ok.isError, ok.text);
    assert.equal(ok.json.scriptFrom, "metadata", "the mock has no level.script.get: the metadata fallback still works");
    await mock.close();
    await new Promise((r) => setTimeout(r, 300));
    const dead = await m.tool("sim_fingerprint", { against: ok.json });
    assert.ok(dead.isError, `a dead connection must fail, got ${dead.text.slice(0, 200)}`);
    assert.match(dead.text, /could not be read|not connected|disconnected|closed/i);
  } finally {
    await m.close();
    await mock.close().catch(() => {});
  }
});
