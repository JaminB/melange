import { useMemo, useState } from "preact/hooks";
import { FilterBar, JsonTree, SplitPane, VirtualTable, type Column } from "../../sdk/ui";
import { activeProgramAt, isDraw, type CallRecord } from "./mcap";
import type { LoadedCapture } from "./loadCapture";

export function CallList({ capture }: { capture: LoadedCapture }) {
  const [text, setText] = useState("");
  const [drawsOnly, setDrawsOnly] = useState(false);
  const [program, setProgram] = useState<number | "">("");
  const [selected, setSelected] = useState<number>();

  const programOptions = useMemo(() => {
    const seen = new Map<number, string>();
    for (const b of capture.bindings) {
      if (seen.has(b.arb)) continue;
      const p = capture.programs.find((x) => x.arbName === b.arb);
      seen.set(b.arb, p ? `${p.file}:${p.entry}` : `arb ${b.arb}`);
    }
    return [...seen.entries()];
  }, [capture]);

  const rows = useMemo(() => {
    const needle = text.trim().toLowerCase();
    return capture.calls.filter((c) => {
      if (drawsOnly && !isDraw(c.fn)) return false;
      if (program !== "" && activeProgramAt(capture.bindings, c.i) !== program) return false;
      if (needle && !c.fn.toLowerCase().includes(needle) && !(c.text ?? "").toLowerCase().includes(needle)) return false;
      return true;
    });
  }, [capture, text, drawsOnly, program]);

  const columns: Column<CallRecord>[] = [
    { key: "i", title: "#", width: "5rem", render: (c) => c.i },
    { key: "frame", title: "Frame", width: "5rem", render: (c) => (capture.marks.frame[c.i] >= 0 ? capture.marks.frame[c.i] : "") },
    { key: "pass", title: "Pass", width: "4rem", render: (c) => (capture.marks.pass[c.i] >= 0 ? capture.marks.pass[c.i] : "") },
    { key: "stage", title: "Stage", width: "6rem", render: (c) => capture.marks.stage[c.i] ?? "" },
    { key: "fn", title: "Function", width: "14rem", render: (c) => <code>{c.fn}</code> },
    { key: "text", title: "Arguments", width: "1fr", render: (c) => c.text ?? "" },
  ];

  const detail = selected !== undefined ? rows.find((r) => r.i === selected) : undefined;

  return (
    <div class="cap-calls">
      <FilterBar
        text={text}
        onText={setText}
        placeholder="Filter by function or arguments"
        chips={[{ id: "draws", label: "Draws only", on: drawsOnly }]}
        onChip={(id) => { if (id === "draws") setDrawsOnly((v) => !v); }}
      >
        {programOptions.length ? (
          <select class="cap-program-select" aria-label="Filter by program" value={program}
                  onChange={(e) => { const v = (e.currentTarget as HTMLSelectElement).value; setProgram(v === "" ? "" : Number(v)); }}>
            <option value="">All programs</option>
            {programOptions.map(([arb, label]) => <option key={arb} value={arb}>{label}</option>)}
          </select>
        ) : null}
        <span class="muted small">{rows.length} / {capture.calls.length} calls</span>
      </FilterBar>
      <SplitPane id="capture.calls" initial={0.68}>
        <VirtualTable
          rows={rows}
          columns={columns}
          rowKey={(c) => c.i}
          selected={detail ? rows.indexOf(detail) : undefined}
          onRowClick={(c) => setSelected(c.i)}
          empty={<span>No calls match this filter.</span>}
        />
        {detail ? <div class="cap-detail"><JsonTree value={detail} open={2} /></div> : <div class="cap-detail muted">Select a call to see its full record.</div>}
      </SplitPane>
    </div>
  );
}
