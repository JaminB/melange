// Drives the built bundle (dist/erg-mcp.mjs) over stdio, as an MCP client would.
//   const m = spawnMcp({ ERG_MCP_URL: url }); await m.initialize(); const r = await m.tool("list_bases", {}); m.close();
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

export const BUNDLE = process.env.ERG_MCP_BUNDLE ?? fileURLToPath(new URL("../dist/erg-mcp.mjs", import.meta.url));

export function spawnMcp(env = {}, args = []) {
  // No inherited ERG_MCP_URL or MELANGE_GAME_DIR: a test connects only where it says.
  const base = { ...process.env, ERG_MCP_URL: "", MELANGE_GAME_DIR: "", USERPROFILE: "", OneDrive: "" };
  const proc = spawn(process.execPath, [BUNDLE, ...args], { env: { ...base, ...env }, stdio: ["pipe", "pipe", "pipe"] });
  const pending = new Map();
  const stray = [];
  let buf = "", stderr = "", nextId = 1;
  proc.stdout.setEncoding("utf8");
  proc.stdout.on("data", (s) => {
    buf += s;
    let nl;
    while ((nl = buf.indexOf("\n")) >= 0) {
      const line = buf.slice(0, nl);
      buf = buf.slice(nl + 1);
      const m = JSON.parse(line);   // anything but JSON on stdout fails the test
      if (pending.has(m.id)) {
        pending.get(m.id)(m);
        pending.delete(m.id);
      } else stray.push(m);
    }
  });
  proc.stderr.setEncoding("utf8");
  proc.stderr.on("data", (s) => { stderr += s; });
  const exited = new Promise((r) => proc.on("exit", r));
  const send = (m) => proc.stdin.write(`${JSON.stringify(m)}\n`);
  const request = (method, params, timeoutMs = 60000) => new Promise((resolve, reject) => {
    const id = nextId++;
    const t = setTimeout(() => reject(new Error(`${method}: no answer in ${timeoutMs} ms; stderr: ${stderr}`)), timeoutMs);
    pending.set(id, (m) => { clearTimeout(t); resolve(m); });
    send({ jsonrpc: "2.0", id, method, ...(params ? { params } : {}) });
  });
  return {
    proc, send, request, stray, exited,
    stderr: () => stderr,
    async initialize(version = "2025-06-18") {
      const r = await request("initialize", { protocolVersion: version, capabilities: {}, clientInfo: { name: "erg-mcp-test", version: "1" } });
      send({ jsonrpc: "2.0", method: "notifications/initialized" });
      return r;
    },
    /** tools/call: { isError, text, json (parsed when the text is JSON), content }. */
    async tool(name, args = {}) {
      const m = await request("tools/call", { name, arguments: args });
      if (m.error) throw new Error(`${name}: ${m.error.message}`);
      const text = m.result.content.filter((c) => c.type === "text").map((c) => c.text).join("\n");
      let json;
      try { json = JSON.parse(text); } catch { json = undefined; }
      return { isError: !!m.result.isError, text, json, content: m.result.content };
    },
    close() { proc.stdin.end(); return exited; },
  };
}
