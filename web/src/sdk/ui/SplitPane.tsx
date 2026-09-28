// Two panes with a draggable divider. The size (a fraction) is remembered per `id` in localStorage.
import type { ComponentChildren } from "preact";
import { useRef, useState } from "preact/hooks";

export interface SplitPaneProps {
  id: string;
  direction?: "row" | "column";
  initial?: number;                              // 0..1, the first pane's share
  min?: number;
  children: [ComponentChildren, ComponentChildren];
}

function load(id: string, fallback: number): number {
  try {
    const v = Number(localStorage.getItem(`oasis.split.${id}`));
    return v > 0 && v < 1 ? v : fallback;
  } catch {
    return fallback;
  }
}

export function SplitPane({ id, direction = "row", initial = 0.5, min = 0.1, children }: SplitPaneProps) {
  const [frac, setFrac] = useState(() => load(id, initial));
  const box = useRef<HTMLDivElement>(null);
  const onDown = (e: PointerEvent) => {
    const el = box.current;
    if (!el) return;
    (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
    const r = el.getBoundingClientRect();
    const move = (m: PointerEvent) => {
      const f = direction === "row" ? (m.clientX - r.left) / r.width : (m.clientY - r.top) / r.height;
      setFrac(Math.min(1 - min, Math.max(min, f)));
    };
    const up = (u: PointerEvent) => {
      window.removeEventListener("pointermove", move);
      window.removeEventListener("pointerup", up);
      const f = direction === "row" ? (u.clientX - r.left) / r.width : (u.clientY - r.top) / r.height;
      try {
        localStorage.setItem(`oasis.split.${id}`, String(Math.min(1 - min, Math.max(min, f))));
      } catch {
        /* storage unavailable */
      }
    };
    window.addEventListener("pointermove", move);
    window.addEventListener("pointerup", up);
  };
  const tracks = `${frac}fr 6px ${1 - frac}fr`;
  return (
    <div ref={box} class={`sp sp-${direction}`}
         style={direction === "row" ? { gridTemplateColumns: tracks } : { gridTemplateRows: tracks }}>
      <div class="sp-pane">{children[0]}</div>
      <div class="sp-handle" role="separator" aria-orientation={direction === "row" ? "vertical" : "horizontal"} onPointerDown={onDown} />
      <div class="sp-pane">{children[1]}</div>
    </div>
  );
}
