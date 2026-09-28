// A collapsible JSON viewer. Objects and arrays deeper than `open` levels start collapsed.
import { useState } from "preact/hooks";

export interface JsonTreeProps { value: unknown; open?: number; name?: string; }

export function JsonTree({ value, open = 1, name }: JsonTreeProps) {
  return <div class="jt"><Node value={value} depth={0} open={open} name={name} /></div>;
}

function Node({ value, depth, open, name }: { value: unknown; depth: number; open: number; name?: string }) {
  const [expanded, setExpanded] = useState(depth < open);
  const label = name !== undefined ? <span class="jt-key">{name}: </span> : null;
  if (value === null || typeof value !== "object") {
    return <div class="jt-line">{label}<Scalar value={value} /></div>;
  }
  const isArray = Array.isArray(value);
  const entries = isArray ? (value as unknown[]).map((v, i) => [String(i), v] as const) : Object.entries(value as object);
  const summary = isArray ? `[${entries.length}]` : `{${entries.length}}`;
  return (
    <div class="jt-node">
      <button class="jt-toggle" aria-expanded={expanded} onClick={() => setExpanded(!expanded)}>
        <span class="jt-caret">{expanded ? "▾" : "▸"}</span>
        {label}
        <span class="jt-summary">{summary}</span>
      </button>
      {expanded ? (
        <div class="jt-children">
          {entries.map(([k, v]) => <Node key={k} value={v} depth={depth + 1} open={open} name={k} />)}
        </div>
      ) : null}
    </div>
  );
}

function Scalar({ value }: { value: unknown }) {
  if (typeof value === "string") return <span class="jt-str">{JSON.stringify(value)}</span>;
  if (typeof value === "number") return <span class="jt-num">{String(value)}</span>;
  if (typeof value === "boolean") return <span class="jt-bool">{String(value)}</span>;
  return <span class="jt-null">{value === undefined ? "undefined" : "null"}</span>;
}
