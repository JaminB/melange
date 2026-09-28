// The command palette: type to filter, arrows to move, Enter to run, Escape to close.
import { useEffect, useLayoutEffect, useRef, useState } from "preact/hooks";
import { matchScore } from "./layout";

export interface Command { id: string; label: string; hint?: string; disabled?: boolean; run: () => void; }

export function filterCommands(all: Command[], query: string): Command[] {
  if (!query.trim()) return all;
  return all
    .map((c, i) => ({ c, i, s: matchScore(c.label, query) }))
    .filter((x) => x.s >= 0)
    .sort((a, b) => a.s - b.s || a.i - b.i)
    .map((x) => x.c);
}

export function Palette({ commands, onClose }: { commands: Command[]; onClose: () => void }) {
  const [query, setQuery] = useState("");
  const [sel, setSel] = useState(0);
  const input = useRef<HTMLInputElement>(null);
  const list = filterCommands(commands, query);
  useLayoutEffect(() => input.current?.focus(), []);
  useEffect(() => setSel(0), [query]);

  const run = (c?: Command) => {
    if (!c || c.disabled) return;
    onClose();
    c.run();
  };
  const onKey = (e: KeyboardEvent) => {
    if (e.key === "Escape") {
      e.preventDefault();
      onClose();
    } else if (e.key === "ArrowDown") {
      e.preventDefault();
      setSel((s) => Math.min(list.length - 1, s + 1));
    } else if (e.key === "ArrowUp") {
      e.preventDefault();
      setSel((s) => Math.max(0, s - 1));
    } else if (e.key === "Enter") {
      e.preventDefault();
      run(list[sel]);
    }
  };
  return (
    <div class="palette-back" onClick={onClose}>
      <div class="palette" role="dialog" aria-modal="true" aria-label="Commands" onClick={(e) => e.stopPropagation()}>
        <input ref={input} class="palette-input" type="text" value={query} placeholder="Type a command"
               aria-label="Command" aria-controls="palette-list" onKeyDown={onKey}
               onInput={(e) => setQuery((e.currentTarget as HTMLInputElement).value)} />
        <ul id="palette-list" class="palette-list" role="listbox">
          {list.length === 0 ? <li class="muted palette-empty">No matching command</li> : null}
          {list.map((c, i) => (
            <li key={c.id} role="option" aria-selected={i === sel} aria-disabled={c.disabled}
                class={`palette-item${i === sel ? " sel" : ""}${c.disabled ? " disabled" : ""}`} data-command={c.id}
                onMouseMove={() => setSel(i)} onClick={() => run(c)}>
              <span>{c.label}</span>
              {c.hint ? <span class="palette-hint">{c.hint}</span> : null}
            </li>
          ))}
        </ul>
      </div>
    </div>
  );
}
