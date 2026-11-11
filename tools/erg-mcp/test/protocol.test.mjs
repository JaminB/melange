// The MCP side of the bundle, with no server: initialize and version negotiation, tools/list, ping, errors.
import assert from "node:assert/strict";
import { test } from "node:test";
import { spawnMcp } from "./harness.mjs";

const PROJECT_TOOLS = ["connect", "list_bases", "new_project", "open_project", "describe_level", "save", "undo", "redo", "close"];

test("initialize, tools/list, ping, errors", async () => {
  const m = spawnMcp();
  try {
    const init = await m.initialize("2025-06-18");
    assert.equal(init.result.protocolVersion, "2025-06-18");
    assert.equal(init.result.serverInfo.name, "erg-mcp");
    assert.ok(init.result.capabilities.tools);

    const list = await m.request("tools/list", {});
    const names = list.result.tools.map((t) => t.name);
    for (const n of PROJECT_TOOLS) assert.ok(names.includes(n), `tools/list has ${n}`);
    assert.equal(new Set(names).size, names.length, "tool names are unique");
    for (const t of list.result.tools) {
      assert.ok(t.description.length > 10, `${t.name} has a description`);
      assert.equal(t.inputSchema.type, "object", `${t.name} takes an object`);
    }

    assert.deepEqual((await m.request("ping")).result, {});
    assert.equal((await m.request("no/such")).error.code, -32601);
    assert.equal((await m.request("tools/call", { name: "no_such_tool", arguments: {} })).error.code, -32602);

    const noProject = await m.tool("describe_level");
    assert.ok(noProject.isError && /open_project/.test(noProject.text), noProject.text);
    const badArgs = await m.tool("new_project", { base: "x" });
    assert.ok(badArgs.isError && /slug: is required/.test(badArgs.text), badArgs.text);
    const extra = await m.tool("undo", { nope: 1 });
    assert.ok(extra.isError && /not a known argument/.test(extra.text), extra.text);
    const noServer = await m.tool("list_bases");
    assert.ok(noServer.isError && /no Oasis server/.test(noServer.text), noServer.text);

    m.send({ jsonrpc: "2.0", id: 99, method: "tools/list" });
    m.proc.stdin.write("not json\n");
    await new Promise((r) => setTimeout(r, 200));
    assert.ok(m.stray.some((x) => x.error?.code === -32700), "a parse error is answered");

    const exe = await m.tool("connect", { gameDir: "C:\nowhere", exe: "C:\Windows\System32\calc.exe" });
    assert.ok(exe.isError && /exe: is not a known argument/.test(exe.text), "connect names no program to run");

    // A batch is answered with one array, notifications left out; an empty batch is an invalid request.
    m.stray.length = 0;
    m.proc.stdin.write(`${JSON.stringify([{ jsonrpc: "2.0", id: "b1", method: "ping" }, { jsonrpc: "2.0", method: "notifications/initialized" },
      { jsonrpc: "2.0", id: "b2", method: "no/such" }])}
[]
`);
    await new Promise((r) => setTimeout(r, 300));
    const batch = m.stray.find(Array.isArray);
    assert.ok(batch, JSON.stringify(m.stray));
    assert.deepEqual(batch.map((x) => x.id), ["b1", "b2"]);
    assert.equal(batch[1].error.code, -32601);
    assert.ok(m.stray.some((x) => !Array.isArray(x) && x.error?.code === -32600), "an empty batch is refused");
  } finally {
    await m.close();
  }
});

test("an unknown protocol version gets the newest", async () => {
  const m = spawnMcp();
  try {
    assert.equal((await m.initialize("2024-11-05")).result.protocolVersion, "2024-11-05");
    const n = spawnMcp();
    try {
      assert.equal((await n.initialize("1999-01-01")).result.protocolVersion, "2025-06-18");
    } finally {
      await n.close();
    }
  } finally {
    await m.close();
  }
});

test("the process ends with stdin", async () => {
  const m = spawnMcp();
  await m.initialize();
  assert.equal(await m.close(), 0);
});
