// The level script's CodeMirror editor: Lua highlighting, line numbers, and each problem shown on its line.
import { defaultKeymap, history, historyKeymap, indentWithTab } from "@codemirror/commands";
import { StreamLanguage, bracketMatching, indentOnInput, syntaxHighlighting } from "@codemirror/language";
import { lua } from "@codemirror/legacy-modes/mode/lua";
import { highlightSelectionMatches, searchKeymap } from "@codemirror/search";
import { EditorState, StateEffect, StateField, type Range } from "@codemirror/state";
import { Decoration, EditorView, WidgetType, drawSelection, highlightActiveLine, keymap, lineNumbers, type DecorationSet } from "@codemirror/view";
import type { ScriptProblem } from "../../../sdk/erg/session";
import { editorTheme, luaHighlight } from "../../console/editor";

class ProblemWidget extends WidgetType {
  constructor(readonly message: string) { super(); }
  eq(o: ProblemWidget) { return o.message === this.message; }
  toDOM() {
    const el = document.createElement("span");
    el.className = "erg-script-problem";
    el.setAttribute("data-script-problem", "");
    el.textContent = this.message;
    return el;
  }
}

const setProblems = StateEffect.define<ScriptProblem[]>();
const badLine = Decoration.line({ class: "erg-script-bad" });

export function problemDecorations(state: EditorState, problems: ScriptProblem[]): DecorationSet {
  const out: Range<Decoration>[] = [];
  for (const p of [...problems].sort((a, b) => a.line - b.line)) {
    const line = state.doc.line(Math.min(Math.max(1, p.line), state.doc.lines));
    out.push(badLine.range(line.from));
    out.push(Decoration.widget({ widget: new ProblemWidget(p.message), side: 1 }).range(line.to));
  }
  return Decoration.set(out, true);
}

const problemField = StateField.define<DecorationSet>({
  create: () => Decoration.none,
  update(deco, tr) {
    for (const e of tr.effects) if (e.is(setProblems)) return problemDecorations(tr.state, e.value);
    return deco.map(tr.changes);
  },
  provide: (f) => EditorView.decorations.from(f),
});

export function createScriptEditor(parent: HTMLElement, text: string, onChange: (text: string) => void, onSave: () => void): EditorView {
  return new EditorView({
    parent,
    state: EditorState.create({
      doc: text,
      extensions: [
        lineNumbers(),
        history(),
        drawSelection(),
        highlightActiveLine(),
        indentOnInput(),
        bracketMatching(),
        highlightSelectionMatches(),
        StreamLanguage.define(lua),
        syntaxHighlighting(luaHighlight),
        problemField,
        keymap.of([{ key: "Mod-s", run: () => { onSave(); return true; }, preventDefault: true },
          indentWithTab, ...searchKeymap, ...historyKeymap, ...defaultKeymap]),
        EditorView.updateListener.of((u) => { if (u.docChanged) onChange(u.state.doc.toString()); }),
        EditorView.contentAttributes.of({ "aria-label": "Level script (Lua)", "data-editor": "level-script" }),
        editorTheme,
      ],
    }),
  });
}

export function showProblems(v: EditorView, problems: ScriptProblem[]) {
  v.dispatch({ effects: setProblems.of(problems) });
}
