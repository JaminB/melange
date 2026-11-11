// A Model Context Protocol server over stdio: JSON-RPC 2.0, one message per line on stdin and stdout (stderr is for
// logs). It offers tools only. Calls run one at a time in arrival order, so edits never interleave; ping is answered
// at once.
import type { Registry } from "./registry";
import type { Session } from "./session";

/** Newest first; the client's version is echoed when known, else the newest is offered. */
export const PROTOCOL_VERSIONS = ["2025-06-18", "2025-03-26", "2024-11-05"];

type Id = string | number | null;
interface Request { jsonrpc: "2.0"; id?: Id; method: string; params?: Record<string, unknown>; }

const PARSE = -32700, INVALID = -32600, NO_METHOD = -32601, BAD_PARAMS = -32602, INTERNAL = -32603;

export interface ServerInfo { name: string; version: string; instructions?: string; }

export class McpServer {
  private queue: Promise<unknown> = Promise.resolve();
  private initialized = false;

  constructor(private registry: Registry, private session: Session, private info: ServerInfo,
    private write: (line: string) => void = (l) => void process.stdout.write(l)) {}

  /** Reads stdin until it ends. */
  listen(onEnd: () => void) {
    let buf = "";
    process.stdin.setEncoding("utf8");
    process.stdin.on("data", (chunk) => {
      buf += chunk;
      let nl: number;
      while ((nl = buf.indexOf("\n")) >= 0) {
        const line = buf.slice(0, nl).trim();
        buf = buf.slice(nl + 1);
        if (line) void this.line(line);
      }
    });
    process.stdin.on("end", () => void this.queue.then(onEnd, onEnd));
  }

  private send(m: object) { this.write(`${JSON.stringify(m)}\n`); }
  private ok = (id: Id, result: unknown) => ({ jsonrpc: "2.0", id, result });
  private err = (id: Id, code: number, message: string) => ({ jsonrpc: "2.0", id, error: { code, message } });

  async line(line: string) {
    let msg: unknown;
    try {
      msg = JSON.parse(line);
    } catch {
      return this.send(this.err(null, PARSE, "parse error"));
    }
    if (Array.isArray(msg)) {
      // A batch (protocol 2025-03-26): answered with one array of the responses, none when it held only notifications.
      if (!msg.length) return this.send(this.err(null, INVALID, "invalid request: empty batch"));
      const out: object[] = [];
      for (const m of msg) {
        const r = await this.message(m);
        if (r) out.push(r);
      }
      if (out.length) this.send(out);
      return;
    }
    const r = await this.message(msg);
    if (r) this.send(r);
  }

  /** The response to one message, or undefined for a notification (or a response from the client). */
  private async message(m: unknown): Promise<object | undefined> {
    const r = m as Request;
    if (!r || typeof r !== "object" || r.jsonrpc !== "2.0" || typeof r.method !== "string") {
      // A response from the client (we send no requests) or garbage.
      if (r && typeof r === "object" && "id" in r && !("result" in r || "error" in r)) return this.err(r.id ?? null, INVALID, "invalid request");
      if (!r || typeof r !== "object") return this.err(null, INVALID, "invalid request");
      return;
    }
    const isCall = r.id !== undefined && r.id !== null;
    const p = r.params ?? {};
    switch (r.method) {
      case "initialize": {
        const want = typeof p.protocolVersion === "string" ? p.protocolVersion : "";
        this.initialized = true;
        return this.ok(r.id ?? null, {
          protocolVersion: PROTOCOL_VERSIONS.includes(want) ? want : PROTOCOL_VERSIONS[0],
          capabilities: { tools: { listChanged: false } },
          serverInfo: { name: this.info.name, version: this.info.version },
          ...(this.info.instructions ? { instructions: this.info.instructions } : {}),
        });
      }
      case "notifications/initialized":
      case "notifications/cancelled":
        return;
      case "ping":
        return isCall ? this.ok(r.id!, {}) : undefined;
      case "tools/list":
        return isCall ? this.ok(r.id!, { tools: this.registry.list() }) : undefined;
      case "tools/call": {
        if (!isCall) return;
        const name = p.name;
        if (typeof name !== "string" || !this.registry.has(name)) return this.err(r.id!, BAD_PARAMS, `unknown tool: ${String(name)}`);
        const run = () => this.registry.call(this.session, name, p.arguments);
        const job = this.queue.then(run, run);
        this.queue = job.catch(() => {});
        try {
          return this.ok(r.id!, await job);
        } catch (e) {
          return this.err(r.id!, INTERNAL, (e as Error).message);
        }
      }
      default:
        if (r.method.startsWith("notifications/")) return;
        if (isCall) return this.err(r.id!, NO_METHOD, `method not found: ${r.method}${this.initialized ? "" : " (initialize first)"}`);
    }
  }
}
