// The tool contract and the registry. A tool group is one file in src/tools/ that exports `tools: Tool[]`; build.ps1
// lists every such file in src/tools/groups.gen.ts, so a new group needs no other edit. Names are unique across groups.
import type { Session } from "./session";

export type Content = { type: "text"; text: string } | { type: "image"; data: string; mimeType: string };
/** What tools/call answers: content, and isError for a failure the model should see (not a protocol error). */
export interface ToolOutput { content: Content[]; isError?: boolean; }

/** A JSON Schema subset: what validate() below checks. Anything else is passed through to the client unchecked. */
export interface Schema {
  type?: "object" | "string" | "number" | "integer" | "boolean" | "array";
  description?: string;
  properties?: Record<string, Schema>;
  required?: string[];
  additionalProperties?: boolean;
  items?: Schema;
  enum?: readonly (string | number)[];
  minimum?: number;
  maximum?: number;
  minItems?: number;
  maxItems?: number;
  default?: unknown;
}

export interface Tool<A = any> {
  name: string;                 // snake_case, unique across every group
  description: string;
  inputSchema: Schema & { type: "object" };
  /** Return a string (text), a ToolOutput (images, several parts), or any other value (sent as indented JSON).
   * Throw for a failure: the message comes back as an isError result. Args are checked against inputSchema first. */
  handler(session: Session, args: A): unknown | Promise<unknown>;
}

/** A failure meant for the model: the message is the whole answer (no stack). */
export class ToolError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "ToolError";
  }
}

export const text = (t: string): ToolOutput => ({ content: [{ type: "text", text: t }] });
/** An image part; data is the raw bytes (PNG unless mimeType says otherwise). */
export const image = (data: Uint8Array | ArrayBuffer, mimeType = "image/png"): Content =>
  ({ type: "image", data: Buffer.from(data instanceof Uint8Array ? data : new Uint8Array(data)).toString("base64"), mimeType });

export function toOutput(v: unknown): ToolOutput {
  if (typeof v === "string") return text(v);
  if (v && typeof v === "object" && Array.isArray((v as ToolOutput).content)) return v as ToolOutput;
  return text(v === undefined ? "ok" : JSON.stringify(v, null, 1));
}

const typeOk = (t: Schema["type"], v: unknown) =>
  t === undefined ? true :
  t === "integer" ? Number.isInteger(v) :
  t === "number" ? typeof v === "number" && Number.isFinite(v) :
  t === "array" ? Array.isArray(v) :
  t === "object" ? !!v && typeof v === "object" && !Array.isArray(v) :
  typeof v === t;

/** The first way `v` breaks the schema, as "path: why", or null. */
export function validate(s: Schema, v: unknown, path = "args"): string | null {
  if (!typeOk(s.type, v)) return `${path}: must be ${s.type === "integer" ? "an integer" : `a${/^[aeiou]/.test(s.type!) ? "n" : ""} ${s.type}`}`;
  if (s.enum && !s.enum.includes(v as string | number)) return `${path}: must be one of ${s.enum.join(", ")}`;
  if (typeof v === "number") {
    if (s.minimum !== undefined && v < s.minimum) return `${path}: must be at least ${s.minimum}`;
    if (s.maximum !== undefined && v > s.maximum) return `${path}: must be at most ${s.maximum}`;
  }
  if (Array.isArray(v)) {
    if (s.minItems !== undefined && v.length < s.minItems) return `${path}: needs at least ${s.minItems} items`;
    if (s.maxItems !== undefined && v.length > s.maxItems) return `${path}: takes at most ${s.maxItems} items`;
    if (s.items) for (let i = 0; i < v.length; i++) {
      const e = validate(s.items, v[i], `${path}[${i}]`);
      if (e) return e;
    }
  }
  if (s.type === "object" && v && typeof v === "object") {
    const o = v as Record<string, unknown>;
    for (const k of s.required ?? []) if (o[k] === undefined) return `${path}.${k}: is required`;
    for (const [k, val] of Object.entries(o)) {
      const p = s.properties?.[k];
      if (!p) {
        if (s.additionalProperties === false) return `${path}.${k}: is not a known argument`;
        continue;
      }
      if (val === undefined) continue;
      const e = validate(p, val, `${path}.${k}`);
      if (e) return e;
    }
  }
  return null;
}

export class Registry {
  private byName = new Map<string, { tool: Tool; group: string }>();

  add(group: string, tools: Tool[]) {
    for (const t of tools) {
      if (!/^[a-z][a-z0-9_]{0,63}$/.test(t.name)) throw new Error(`tool group ${group}: '${t.name}' is not a snake_case name`);
      const had = this.byName.get(t.name);
      if (had) throw new Error(`tool group ${group}: '${t.name}' is already defined by group ${had.group}`);
      this.byName.set(t.name, { tool: t, group });
    }
  }

  list() { return [...this.byName.values()].map(({ tool }) => ({ name: tool.name, description: tool.description, inputSchema: tool.inputSchema })); }
  has(name: string) { return this.byName.has(name); }
  groupOf(name: string) { return this.byName.get(name)?.group; }

  async call(session: Session, name: string, args: unknown): Promise<ToolOutput> {
    const t = this.byName.get(name)!.tool;
    const a = args ?? {};
    const bad = validate(t.inputSchema, a);
    if (bad) return { ...text(bad), isError: true };
    try {
      return toOutput(await t.handler(session, a));
    } catch (e) {
      const err = e as Error & { details?: string[]; code?: number };
      const msg = err instanceof ToolError ? err.message : `${err?.name ?? "Error"}: ${err?.message ?? String(e)}`;
      return { ...text(err?.details?.length ? `${msg}\n- ${err.details.join("\n- ")}` : msg), isError: true };
    }
  }
}
