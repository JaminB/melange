// Details grouped by frame, filtered by role and text. Clicking selects (Ctrl adds); the role filter also hides the
// other roles' markers in the view.
import { useEffect, useMemo, useState } from "preact/hooks";
import { ROLES, type Role } from "../../../sdk/erg";
import { ROLE_STYLE, glyphOf } from "../model/roles";
import type { EditorStore } from "../model/store";

interface Props { store: EditorStore; onVisible(roles: Set<string>): void; }

export function Outliner({ store, onVisible }: Props) {
  const [roles, setRoles] = useState<Set<Role>>(() => new Set(ROLES));
  const [text, setText] = useState("");
  const [closed, setClosed] = useState<Set<number>>(new Set());
  useEffect(() => onVisible(roles), [roles]);
  const counts = useMemo(() => {
    const c = new Map<Role, number>();
    for (const d of store.scene.details) c.set(d.role, (c.get(d.role) ?? 0) + 1);
    return c;
  }, [store.version]);
  const groups = useMemo(() => {
    const q = text.trim().toLowerCase();
    const byFrame = new Map<number, typeof store.scene.details>();
    for (const d of store.scene.details) {
      if (!roles.has(d.role)) continue;
      if (q && !`${d.name} ${d.resource} #${d.id}`.toLowerCase().includes(q)) continue;
      const list = byFrame.get(d.frame);
      if (list) list.push(d);
      else byFrame.set(d.frame, [d]);
    }
    return store.scene.frames.filter((f) => byFrame.has(f.id)).map((f) => ({ f, details: byFrame.get(f.id)! }));
  }, [store.version, roles, text]);
  const sel = new Set(store.selection);
  const toggleRole = (r: Role) => {
    const n = new Set(roles);
    if (n.has(r)) n.delete(r);
    else n.add(r);
    setRoles(n);
  };

  return (
    <nav class="erg-outliner" aria-label="Outliner" data-erg-outliner>
      <input type="search" placeholder="Filter details" value={text} onInput={(e) => setText((e.target as HTMLInputElement).value)} />
      <div class="erg-roles">
        {ROLES.filter((r) => counts.has(r) || !roles.has(r)).map((r) => (
          <button key={r} class={`erg-role${roles.has(r) ? " on" : ""}`} onClick={() => toggleRole(r)} data-role={r}
                  style={{ "--c": ROLE_STYLE[r].color }} title={ROLE_STYLE[r].label}>{ROLE_STYLE[r].label} {counts.get(r) ?? 0}</button>
        ))}
      </div>
      <div class="erg-tree">
        {groups.map(({ f, details }) => (
          <div key={f.id} class="erg-group">
            <button class="erg-frame" onClick={() => { const n = new Set(closed); if (n.has(f.id)) n.delete(f.id); else n.add(f.id); setClosed(n); }}
                    title={f.folder ? "folder frame" : `${f.size.join("x")} voxels`}>
              <span>{closed.has(f.id) ? "▸" : "▾"}</span> {f.name || "(unnamed)"} <span class="muted">#{f.id}{store.translationOnly.has(f.id) ? " · animated" : ""}</span>
            </button>
            {closed.has(f.id) ? null : details.map((d) => (
              <button key={d.id} class={`erg-item${sel.has(d.id) ? " sel" : ""}`} data-detail={d.id}
                      onClick={(e) => store.select(e.ctrlKey || e.metaKey ? (sel.has(d.id) ? store.selection.filter((x) => x !== d.id) : [...store.selection, d.id]) : [d.id])}>
                <span class="erg-glyph" style={{ background: ROLE_STYLE[d.role].color }}>{glyphOf(d)}</span>
                <span class="erg-name">{d.name || d.resource}</span>
                <span class="muted small">{d.src === null ? "new" : `#${d.src}`}</span>
              </button>
            ))}
          </div>
        ))}
        {groups.length === 0 ? <p class="muted small pad-x">Nothing matches.</p> : null}
      </div>
    </nav>
  );
}
