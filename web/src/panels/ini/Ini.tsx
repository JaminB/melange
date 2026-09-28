import { render } from "preact";
import { useEffect, useState } from "preact/hooks";
import type { Client } from "../../sdk/client";
import { errorText, useConnection } from "../../sdk/hooks";
import { FilterBar } from "../../sdk/ui";
import { docOf, effective, isBoolish, isDefault, protectedReason, sections, valueProblem, type IniDoc, type IniKey } from "./model";

export function mount(el: HTMLElement, c: Client): () => void {
  render(<Ini client={c} />, el);
  return () => render(null, el);
}

const id = (k: { section: string; key: string }) => `${k.section.toLowerCase()}\n${k.key.toLowerCase()}`;

interface Edit { key: IniKey; value: string; error?: string; saving?: boolean; }

function Ini({ client }: { client: Client }) {
  const conn = useConnection(client);
  const [doc, setDoc] = useState<IniDoc>();
  const [error, setError] = useState<string>();
  const [filter, setFilter] = useState("");
  const [raw, setRaw] = useState(false);
  const [edit, setEdit] = useState<Edit>();
  const [changed, setChanged] = useState<Record<string, "live" | "restart">>({});
  const readOnly = !!conn.welcome?.limits?.readOnly;
  const canSet = conn.open && client.has("ini.set") && !readOnly;

  const load = () => {
    if (!conn.open || !client.has("ini.get")) return;
    client.call<unknown>("ini.get").then((v) => { setDoc(docOf(v)); setError(undefined); }, (e) => setError(errorText(e)));
  };
  useEffect(load, [conn.open]);

  const save = async (e: Edit) => {
    const problem = valueProblem(e.value) ?? protectedReason(e.key.section, e.key.key, e.value);
    if (problem) {
      setEdit({ ...e, error: problem });
      return;
    }
    setEdit({ ...e, saving: true, error: undefined });
    try {
      const r = await client.call<{ live: boolean; restart: boolean; changed: boolean }>("ini.set",
        { section: e.key.section, key: e.key.key, value: e.value });
      if (r.changed) setChanged((c) => ({ ...c, [id(e.key)]: r.restart ? "restart" : "live" }));
      setEdit(undefined);
      load();
    } catch (err) {
      setEdit({ ...e, saving: false, error: errorText(err) });
    }
  };

  if (conn.open && !client.has("ini.get")) return <div class="pad"><p class="muted">This server has no settings editor.</p></div>;
  const groups = doc ? sections(doc.keys, filter) : [];
  const restartCount = Object.values(changed).filter((x) => x === "restart").length;

  return (
    <div class="ini" data-ini>
      <FilterBar text={filter} onText={setFilter} placeholder="Filter settings">
        <button class={`btn${raw ? " on" : ""}`} aria-pressed={raw} data-action="raw" onClick={() => setRaw(!raw)}>Raw text</button>
        <button class="btn" onClick={load} disabled={!conn.open}>Reload</button>
      </FilterBar>
      <div class="status-line">
        {doc ? <span class="mono" title={doc.encoding}>{doc.path}</span> : <span class="muted">{conn.open ? "Loading…" : "Not connected."}</span>}
        {readOnly ? <span class="warn"> · read-only</span> : null}
        {restartCount ? <span class="warn" data-restart> · {restartCount} change{restartCount === 1 ? "" : "s"} take effect after a restart</span> : null}
        {error ? <span class="error"> · {error}</span> : null}
      </div>
      <div class="ini-body">
        {raw && doc ? <pre class="raw" data-raw>{doc.text}</pre> : groups.map((s) => (
          <section key={s.name} class="ini-section" data-section={s.name}>
            <h2>[{s.name}]</h2>
            <table class="table ini-table">
              <tbody>
                {s.keys.map((k) => {
                  const kid = id(k);
                  const editing = edit && id(edit.key) === kid ? edit : undefined;
                  const locked = protectedReason(k.section, k.key);
                  const mark = changed[kid];
                  return (
                    <tr key={kid} data-key={`${k.section}.${k.key}`}>
                      <td class="ini-key">
                        <code>{k.key}</code>
                        {!k.declared ? <span class="tag" title="Not declared by any module">unknown</span> : null}
                        <span class={`tag ${k.live ? "tag-live" : ""}`} title={k.live ? "Applies without a restart" : "Needs a restart"}>{k.live ? "live" : "restart"}</span>
                      </td>
                      <td class="ini-value">
                        {editing ? (
                          <form class="ini-edit" onSubmit={(ev) => { ev.preventDefault(); save(editing); }}>
                            {isBoolish(k) ? (
                              <select value={editing.value} aria-label={k.key} onChange={(ev) => setEdit({ ...editing, value: (ev.currentTarget as HTMLSelectElement).value })}>
                                <option value="1">1 (on)</option>
                                <option value="0">0 (off)</option>
                              </select>
                            ) : (
                              <input class="fb-text" value={editing.value} aria-label={k.key} autoFocus
                                     onInput={(ev) => setEdit({ ...editing, value: (ev.currentTarget as HTMLInputElement).value, error: undefined })}
                                     onKeyDown={(ev) => { if (ev.key === "Escape") setEdit(undefined); }} />
                            )}
                            <button class="btn primary" type="submit" disabled={editing.saving}>Save</button>
                            <button class="btn" type="button" onClick={() => setEdit(undefined)}>Cancel</button>
                            {k.def !== null && editing.value !== k.def ? (
                              <button class="link" type="button" onClick={() => setEdit({ ...editing, value: k.def ?? "" })}>Default</button>
                            ) : null}
                            {editing.error ? <div class="error small" data-edit-error>{editing.error}</div> : null}
                          </form>
                        ) : (
                          <>
                            <code class={isDefault(k) ? "muted" : ""}>{effective(k) || <em class="muted">empty</em>}</code>
                            {k.current === null ? <span class="muted small"> (default, not in the file)</span> : null}
                            {mark ? <span class={`tag ${mark === "live" ? "tag-live" : "tag-warn"}`} data-mark={mark}>{mark === "live" ? "applied" : "restart to apply"}</span> : null}
                          </>
                        )}
                      </td>
                      <td class="ini-def muted small">{k.def !== null ? <>default <code>{k.def || "(empty)"}</code></> : null}</td>
                      <td class="ini-act">
                        {!editing ? (
                          <button class="link" data-edit={`${k.section}.${k.key}`} disabled={!canSet || !!locked} title={locked}
                                  onClick={() => setEdit({ key: k, value: effective(k) })}>Edit</button>
                        ) : null}
                      </td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </section>
        ))}
        {doc && !raw && groups.length === 0 ? <p class="muted pad">No setting matches the filter.</p> : null}
      </div>
    </div>
  );
}
