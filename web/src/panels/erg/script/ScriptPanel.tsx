// The Script tab: the project's level script in a Lua editor, Save, its problems on their lines, and a short
// reference of what a level script can call.
import { useEffect, useRef, useState } from "preact/hooks";
import type { EditorView } from "@codemirror/view";
import { createScriptEditor, showProblems } from "./editor";
import { LEVEL_API, type ScriptDoc } from "./model";

function useDoc(doc: ScriptDoc) {
  const [, set] = useState(0);
  useEffect(() => doc.on(() => set((n) => n + 1)), [doc]);
}

export function ScriptPanel({ doc, readOnly }: { doc: ScriptDoc; readOnly?: boolean }) {
  useDoc(doc);
  const host = useRef<HTMLDivElement>(null);
  const view = useRef<EditorView>();

  useEffect(() => { if (!doc.loaded) void doc.load(); }, [doc]);
  useEffect(() => {
    if (!doc.loaded || !host.current || view.current) return;
    view.current = createScriptEditor(host.current, doc.text, (t) => doc.edit(t), () => { if (!readOnly) void doc.save(); });
    showProblems(view.current, doc.problems);
    return () => { view.current?.destroy(); view.current = undefined; };
  }, [doc, doc.loaded]);
  useEffect(() => { if (view.current) showProblems(view.current, doc.problems); }, [doc.problems]);

  return (
    <div class="erg-script" data-erg-script>
      <div class="erg-script-bar">
        <button class="btn" data-action="save-script" disabled={readOnly || !doc.loaded || doc.busy || !doc.dirty}
                onClick={() => void doc.save()} title="Save script.lua (Ctrl+S)">Save script</button>
        <span class="muted small" data-script-state>
          {!doc.loaded ? "Loading…" : doc.busy ? "Saving…" : doc.dirty ? "Unsaved changes" : doc.text ? "Saved" : "No script"}
        </span>
      </div>
      {doc.error ? <p class="error pad" data-script-error>{doc.error}</p> : null}
      {doc.problems.length ? (
        <ul class="erg-script-problems" data-script-problems>
          {doc.problems.map((p) => <li key={`${p.line}:${p.message}`} class="error">Line {p.line}: {p.message}</li>)}
        </ul>
      ) : null}
      {!doc.syntaxChecked && !doc.dirty ? (
        <p class="muted small pad" data-script-syntax>Syntax checked when tested in game.</p>
      ) : null}
      <div class="erg-script-editor" ref={host} />
      <details class="erg-script-ref" data-script-ref>
        <summary>Level script reference</summary>
        <p class="muted small">Runs in the match's sandbox (Lua 5.0, numbers are floats) only on this level. Test runs the saved script.</p>
        <dl>
          {LEVEL_API.map((e) => [<dt key={`${e.name}-t`}><code>{e.name}</code></dt>, <dd key={`${e.name}-d`}>{e.text}</dd>])}
        </dl>
      </details>
    </div>
  );
}
