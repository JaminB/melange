// A minimal MCP stdio client for the Caldera bench: spawns the erg-mcp bundle on the pinned Node and calls its tools
// over JSON-RPC (one message per line). Shared by build.mjs and the experiments.
import { spawn, spawnSync } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
export const ROOT = path.resolve(HERE, "../../../..");

export async function startServer() {
  const node = path.join(ROOT, "tools/node/node.exe");
  // the bundle is built from this checkout's sources whenever they are newer (never a stale dist)
  const b = spawnSync("powershell", ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File", path.join(ROOT, "tools/erg-mcp/build.ps1"), "-IfStale"], { stdio: ["ignore", 2, 2] });
  if (b.status !== 0) throw new Error("tools/erg-mcp/build.ps1 failed: fix the build before running the bench");
  const p = spawn(node, [path.join(ROOT, "tools/erg-mcp/dist/erg-mcp.mjs")], { stdio: ["pipe", "pipe", "inherit"] });
  let buf = "", id = 0;
  const pend = new Map();
  p.stdout.on("data", (d) => {
    buf += d;
    let i;
    while ((i = buf.indexOf("\n")) >= 0) {
      const l = buf.slice(0, i); buf = buf.slice(i + 1);
      if (!l.trim()) continue;
      const m = JSON.parse(l);
      if (m.id != null && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); }
    }
  });
  const rpc = (method, params) => new Promise((r) => { const i = ++id; pend.set(i, r); p.stdin.write(JSON.stringify({ jsonrpc: "2.0", id: i, method, params }) + "\n"); });
  await rpc("initialize", { protocolVersion: "2025-06-18", capabilities: {}, clientInfo: { name: "caldera-bench", version: "1" } });
  p.stdin.write(JSON.stringify({ jsonrpc: "2.0", method: "notifications/initialized" }) + "\n");
  const log = [];
  const stats = { calls: 0 };
  const call = async (name, args = {}, { soft = false } = {}) => {
    stats.calls++;
    const r = await rpc("tools/call", { name, arguments: args });
    const res = r.result || { isError: true, content: [{ type: "text", text: JSON.stringify(r.error) }] };
    const text = (res.content || []).filter((c) => c.type === "text").map((c) => c.text).join("\n");
    let json = null;
    try { json = JSON.parse(text); } catch { /* text answer */ }
    const out = { err: !!res.isError, text, json, images: (res.content || []).filter((c) => c.type === "image") };
    const brief = { ...args };
    for (const k of ["grid", "text", "expr", "mask"]) if (typeof brief[k] === "string" && brief[k].length > 200) brief[k] = `<${brief[k].length} chars>`; else if (Array.isArray(brief[k])) brief[k] = "<array>";
    log.push({ name, args: brief, err: out.err, text: text.slice(0, 2000) });
    if (out.err && !soft) throw new Error(`${name}: ${text.slice(0, 1500)}`);
    return out;
  };
  const stop = () => new Promise((r) => { p.on("exit", r); p.stdin.end(); setTimeout(() => { p.kill(); r(); }, 5000); });
  return { call, stop, log, stats };
}
