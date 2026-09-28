import { useEffect, useState } from "preact/hooks";
import { SplitPane } from "../../sdk/ui";
import { loadProgramAsm, type LoadedCapture, type LoadedProgram } from "./loadCapture";

export function Programs({ capture }: { capture: LoadedCapture }) {
  const [selected, setSelected] = useState<LoadedProgram>();
  const [asm, setAsm] = useState("");

  useEffect(() => {
    if (!selected) return;
    let dead = false;
    loadProgramAsm(capture.zip, selected).then((text) => { if (!dead) setAsm(text); });
    return () => { dead = true; };
  }, [selected]);

  if (!capture.programs.length) return <p class="muted">No shader programs in this capture.</p>;
  return (
    <SplitPane id="capture.programs" initial={0.34}>
      <ul class="cap-prog-list">
        {capture.programs.map((p) => (
          <li key={p.n}>
            <button class={`cap-prog-item${selected?.n === p.n ? " active" : ""}`} onClick={() => setSelected(p)}>
              <div>{p.file}<span class="muted">:{p.entry}</span></div>
              <div class="muted small">
                {p.stage} · binds {p.binds ?? 0}
                {p.failed ? " · failed" : ""}
                {p.overridden ? ` · overridden (${p.owner ?? "builtin"})` : ""}
              </div>
            </button>
          </li>
        ))}
      </ul>
      {selected ? (
        <pre class="cap-asm">{asm || (selected.asm ? "Loading…" : "No compiled text for this program.")}</pre>
      ) : (
        <p class="muted">Select a program to see its compiled text.</p>
      )}
    </SplitPane>
  );
}
