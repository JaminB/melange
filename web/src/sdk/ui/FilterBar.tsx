// A text filter with optional toggle chips (levels, categories) and trailing actions.
import type { ComponentChildren } from "preact";

export interface Chip { id: string; label: string; on: boolean; }

export interface FilterBarProps {
  text: string;
  onText: (s: string) => void;
  placeholder?: string;
  chips?: Chip[];
  onChip?: (id: string, on: boolean) => void;
  children?: ComponentChildren;
}

export function FilterBar(p: FilterBarProps) {
  return (
    <div class="fb" role="search">
      <input
        class="fb-text"
        type="search"
        value={p.text}
        placeholder={p.placeholder ?? "Filter"}
        aria-label={p.placeholder ?? "Filter"}
        onInput={(e) => p.onText((e.currentTarget as HTMLInputElement).value)}
      />
      {p.chips?.map((c) => (
        <button key={c.id} class={`fb-chip${c.on ? " on" : ""}`} aria-pressed={c.on} onClick={() => p.onChip?.(c.id, !c.on)}>
          {c.label}
        </button>
      ))}
      <div class="fb-actions">{p.children}</div>
    </div>
  );
}
