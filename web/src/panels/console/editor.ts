// The console's CodeMirror 6 editor: Lua highlighting (legacy mode), Enter runs, Shift+Enter breaks the line,
// Up/Down on the first/last line walk the history, Tab or typing a '.' completes through lua.complete.
import { acceptCompletion, autocompletion, closeBrackets, closeBracketsKeymap, completionKeymap, completionStatus, startCompletion,
  type CompletionContext, type CompletionResult } from "@codemirror/autocomplete";
import { defaultKeymap, history, historyKeymap, insertNewlineAndIndent } from "@codemirror/commands";
import { HighlightStyle, StreamLanguage, bracketMatching, indentOnInput, syntaxHighlighting } from "@codemirror/language";
import { lua } from "@codemirror/legacy-modes/mode/lua";
import { highlightSelectionMatches, searchKeymap } from "@codemirror/search";
import { EditorState, Prec } from "@codemirror/state";
import { EditorView, drawSelection, keymap, placeholder } from "@codemirror/view";
import { tags } from "@lezer/highlight";
import { completionWord } from "./history";

export interface EditorHooks {
  run: () => void;
  historyStep: (dir: -1 | 1, current: string) => string | undefined;
  complete: (prefix: string) => Promise<string[]>;
}

const style = HighlightStyle.define([
  { tag: tags.keyword, color: "var(--hl-keyword)", fontWeight: "600" },
  { tag: [tags.string, tags.special(tags.string)], color: "var(--hl-string)" },
  { tag: [tags.number, tags.bool, tags.null, tags.atom], color: "var(--hl-number)" },
  { tag: [tags.comment, tags.lineComment, tags.blockComment], color: "var(--hl-comment)", fontStyle: "italic" },
  { tag: [tags.function(tags.variableName), tags.standard(tags.variableName), tags.standard(tags.name)], color: "var(--hl-builtin)" },
  { tag: tags.operator, color: "var(--hl-operator)" },
]);

const theme = EditorView.theme({
  "&": { backgroundColor: "var(--surface)", color: "var(--text)", fontSize: "13px" },
  ".cm-content": { fontFamily: "var(--mono)", caretColor: "var(--text)", padding: "6px 0" },
  ".cm-scroller": { fontFamily: "var(--mono)", lineHeight: "1.5" },
  "&.cm-focused": { outline: "none" },
  ".cm-cursor": { borderLeftColor: "var(--text)" },
  "&.cm-focused .cm-selectionBackground, .cm-selectionBackground, ::selection": { backgroundColor: "var(--accent-soft) !important" },
  ".cm-placeholder": { color: "var(--muted)" },
  ".cm-tooltip": { backgroundColor: "var(--surface)", border: "1px solid var(--border)", color: "var(--text)" },
  ".cm-tooltip-autocomplete ul li[aria-selected]": { backgroundColor: "var(--accent-soft)", color: "var(--text)" },
  ".cm-matchingBracket": { backgroundColor: "var(--surface-2)", outline: "1px solid var(--border)" },
});

export function createEditor(parent: HTMLElement, hooks: EditorHooks): EditorView {
  const source = async (ctx: CompletionContext): Promise<CompletionResult | null> => {
    const line = ctx.state.doc.lineAt(ctx.pos);
    const w = completionWord(line.text.slice(0, ctx.pos - line.from));
    if (!w || (!ctx.explicit && !/[.:]$/.test(w.word) && w.word.length < 2)) return null;
    let cands: string[];
    try {
      cands = await hooks.complete(w.word);
    } catch {
      return null;
    }
    if (ctx.aborted || !cands.length) return null;
    return { from: line.from + w.from, options: cands.map((label) => ({ label, type: "variable" })), validFor: /^[\w]*$/ };
  };

  const walk = (dir: -1 | 1) => (v: EditorView) => {
    const head = v.state.selection.main.head;
    const line = v.state.doc.lineAt(head).number;
    if (dir < 0 ? line !== 1 : line !== v.state.doc.lines) return false;
    if (completionStatus(v.state)) return false;
    const next = hooks.historyStep(dir, v.state.doc.toString());
    if (next === undefined) return false;
    v.dispatch({ changes: { from: 0, to: v.state.doc.length, insert: next }, selection: { anchor: next.length } });
    return true;
  };

  const runKey = (v: EditorView) => {
    if (completionStatus(v.state) === "active") return false;
    if (!v.state.doc.toString().trim()) return true;
    hooks.run();
    return true;
  };

  return new EditorView({
    parent,
    state: EditorState.create({
      doc: "",
      extensions: [
        Prec.highest(keymap.of([
          { key: "Enter", run: runKey },
          { key: "Mod-Enter", run: (v) => { hooks.run(); return !!v; } },
          { key: "Shift-Enter", run: insertNewlineAndIndent },
          { key: "ArrowUp", run: walk(-1) },
          { key: "ArrowDown", run: walk(1) },
          { key: "Tab", run: (v) => (completionStatus(v.state) === "active" ? acceptCompletion(v) : startCompletion(v)) || true },
        ])),
        history(),
        drawSelection(),
        indentOnInput(),
        bracketMatching(),
        closeBrackets(),
        highlightSelectionMatches(),
        autocompletion({ override: [source], activateOnTyping: true, defaultKeymap: true }),
        StreamLanguage.define(lua),
        syntaxHighlighting(style),
        placeholder("Lua. Enter runs, Shift+Enter adds a line, Tab completes, Up/Down recall history. =expr prints a value."),
        keymap.of([...closeBracketsKeymap, ...completionKeymap, ...searchKeymap, ...historyKeymap, ...defaultKeymap]),
        EditorView.lineWrapping,
        EditorView.contentAttributes.of({ "aria-label": "Lua code", "data-editor": "lua" }),
        theme,
      ],
    }),
  });
}

export function setText(v: EditorView, text: string) {
  v.dispatch({ changes: { from: 0, to: v.state.doc.length, insert: text }, selection: { anchor: text.length } });
}
