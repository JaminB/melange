import type { ComponentChildren } from "preact";
import { useEffect, useRef } from "preact/hooks";

export interface DrawerProps { title: string; onClose: () => void; children: ComponentChildren; foot?: ComponentChildren; }

export function Drawer({ title, onClose, children, foot }: DrawerProps) {
  const h2 = useRef<HTMLHeadingElement>(null);
  useEffect(() => { h2.current?.focus(); }, []);
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => { if (e.key === "Escape") onClose(); };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [onClose]);
  return (
    <>
      <div class="lx-backdrop" onClick={onClose} />
      <div class="lx-drawer" role="dialog" aria-modal="true" aria-labelledby="lx-title">
        <div class="lx-head">
          <h2 id="lx-title" tabIndex={-1} ref={h2}>{title}</h2>
          <button class="link" onClick={onClose} aria-label="Close">Close</button>
        </div>
        <div class="lx-body">{children}</div>
        {foot ? <div class="lx-foot">{foot}</div> : null}
      </div>
    </>
  );
}
