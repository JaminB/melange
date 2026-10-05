import { render } from "preact";
import { useEffect, useState } from "preact/hooks";
import type { Client } from "../../sdk/client";
import { errorText, useConnection } from "../../sdk/hooks";
import { deepDesertAction, hiddenText, modsOf, sourceText, stateText, stateTone, viewOf, visibleMods, withMod, type ModInfo, type ModsView } from "./model";

export function mount(el: HTMLElement, c: Client): () => void {
  render(<Mods client={c} />, el);
  return () => render(null, el);
}

export function Mods({ client }: { client: Client }) {
  const conn = useConnection(client);
  const [list, setList] = useState<ModInfo[]>();
  const [view, setView] = useState<ModsView>({ showLocal: false, notices: [] });
  const [error, setError] = useState<string>();
  const [rowError, setRowError] = useState<Record<string, string>>({});
  const [busy, setBusy] = useState<string>();
  const readOnly = !!conn.welcome?.limits?.readOnly;
  const canSet = conn.open && client.has("mods.setEnabled") && !readOnly;
  const canRevoke = conn.open && client.has("mods.revokeDeepDesert") && !readOnly;
  const canLive = conn.open && client.has("levels.live") && !readOnly;
  const canDismiss = conn.open && client.has("mods.dismissNotice") && !readOnly;

  const loadView = () => {
    if (!conn.open || !client.has("mods.view")) return;
    client.call<unknown>("mods.view").then((v) => setView(viewOf(v)), () => {});
  };
  const load = () => {
    if (!conn.open || !client.has("mods.list")) return;
    client.call<unknown>("mods.list").then((v) => { setList(modsOf(v)); setError(undefined); }, (e) => setError(errorText(e)));
    loadView();
  };
  useEffect(() => {
    if (!conn.open) return;
    if (client.has("mods")) {
      loadView();
      return client.subscribe<unknown>("mods", undefined, (v) => setList(modsOf(v)));
    }
    load();
  }, [conn.open]);

  // Display only: a hidden local plugin keeps its switch and still loads. Kept for this page alone when the server
  // cannot store the choice (read-only Oasis).
  const setShowLocal = async (on: boolean) => {
    setView((v) => ({ ...v, showLocal: on }));
    if (!client.has("mods.setShowLocal") || readOnly) return;
    try {
      setView(viewOf(await client.call<unknown>("mods.setShowLocal", { on })));
    } catch {
      // the page keeps the choice for itself
    }
  };

  const dismiss = async (key?: string) => {
    try {
      setView(viewOf(await client.call<unknown>("mods.dismissNotice", key ? { key } : {})));
    } catch (e) {
      setError(errorText(e));
    }
  };

  // As in the game's overlay: a map pack changes at once when it can (offline, at the menu), otherwise after a restart.
  const live = async (m: ModInfo, on: boolean) => {
    if (!canLive || m.kind !== "content") return false;
    try {
      const r = await client.call<{ ok?: boolean }>("levels.live", { modId: m.id, on });
      if (r?.ok !== true) return false;
    } catch {
      return false;
    }
    const all = modsOf(await client.call<unknown>("mods.list"));
    setList(all);
    return true;
  };

  const act = async (m: ModInfo, method: string, params: object, liveOn?: boolean) => {
    setBusy(m.id);
    setRowError(({ [m.id]: _, ...rest }) => rest);
    try {
      if (liveOn !== undefined && (await live(m, liveOn))) return;
      const r = modsOf([await client.call<unknown>(method, params)]);
      if (r[0]) setList((l) => withMod(l ?? [], r[0]));
    } catch (e) {
      setRowError((x) => ({ ...x, [m.id]: errorText(e) }));
    } finally {
      setBusy(undefined);
    }
  };

  if (conn.open && !client.has("mods.list") && !client.has("mods"))
    return <div class="pad"><p class="muted">This server has no mod list.</p></div>;
  const restart = list?.some((m) => m.restartRequired);
  const { shown, hidden, hiddenOn } = visibleMods(list ?? [], view.showLocal);
  const anyLocal = !!list?.some((m) => m.source === "local");

  return (
    <div class="mods pad" data-mods>
      <div class="row between">
        <h1>Mods</h1>
        <button class="btn" onClick={load} disabled={!conn.open}>Refresh</button>
      </div>
      {readOnly ? <p class="hint warn">Oasis is read-only ([Oasis] ReadOnly=1): mods cannot be changed from here.</p> : null}
      {restart ? <p class="hint warn" data-restart>A restart is needed before some changes take effect.</p> : null}
      {view.notices.length ? (
        <div class="hint warn" data-notices role="status">
          <div>Plugins that cannot load on this version of Melange were set aside:</div>
          <ul>
            {view.notices.map((n) => (
              <li key={n.key} data-notice={n.id}>
                {n.text}{" "}
                {canDismiss ? <button class="link" data-dismiss={n.key} onClick={() => dismiss(n.key)}>Dismiss</button> : null}
              </li>
            ))}
          </ul>
          {canDismiss && view.notices.length > 1 ? <button class="link" data-dismiss-all onClick={() => dismiss()}>Dismiss all</button> : null}
        </div>
      ) : null}
      {anyLocal || view.showLocal ? (
        <p class="muted small">
          <label>
            <input type="checkbox" checked={view.showLocal} data-show-local
                   onChange={(e) => setShowLocal((e.currentTarget as HTMLInputElement).checked)} /> Show local plugins
          </label>
          {!view.showLocal && hidden ? <span data-hidden-hint> · {hiddenText(hidden, hiddenOn)}</span> : null}
        </p>
      ) : null}
      {error ? <p class="error">{error}</p> : null}
      {!list ? <p class="muted">{conn.open ? "Loading…" : "Not connected."}</p>
        : list.length === 0 ? <p class="muted">No mods found in the Mods folder.</p>
        : shown.length === 0 ? <p class="muted">No mods from the Store. Local plugins are hidden.</p> : (
        <table class="table">
          <thead>
            <tr><th>On</th><th>Mod</th><th>State</th><th>Kind</th><th>Deep Desert</th></tr>
          </thead>
          <tbody>
            {shown.map((m) => {
              const dd = deepDesertAction(m);
              const src = view.showLocal ? sourceText(m) : undefined;
              return (
                <tr key={m.id} data-mod={m.id} class={busy === m.id ? "busy" : undefined}>
                  <td>
                    <input type="checkbox" checked={m.on} disabled={!canSet || busy === m.id} data-toggle={m.id}
                           aria-label={`${m.on ? "Disable" : "Enable"} ${m.name || m.id}`}
                           onChange={(e) => {
                             const on = (e.currentTarget as HTMLInputElement).checked;
                             act(m, "mods.setEnabled", { id: m.id, on }, on);
                           }} />
                  </td>
                  <td>
                    <div>
                      <strong>{m.name || m.id}</strong> <span class="muted small">{m.version}</span>
                      {src ? <span class="tag" data-source={m.source}>{src}</span> : null}
                      {m.implicitManifest ? <span class="tag">no spice.json</span> : null}
                    </div>
                    <div class="muted small">{m.id}{m.authors ? ` · ${m.authors}` : ""}</div>
                    {m.sandbox?.error ? <div class="error small">{m.sandbox.error}</div> : null}
                    {m.sandbox?.disabledCallbacks ? <div class="warn small">{m.sandbox.disabledCallbacks} callback(s) disabled after faults</div> : null}
                    {rowError[m.id] ? <div class="error small" data-row-error>{rowError[m.id]}</div> : null}
                  </td>
                  <td>
                    <span class={`state tone-${stateTone(m.state)}`} data-state={m.state}>{stateText(m.state)}</span>
                    {m.reason ? <div class="muted small">{m.reason}</div> : null}
                  </td>
                  <td>{m.kind === "content" ? "Content" : "Client only"}{m.hasSim ? <span class="tag">sim</span> : null}</td>
                  <td data-dd={dd}>
                    {dd === "revoke" ? (
                      <>
                        <span class="state tone-warn">Granted</span>{" "}
                        <button class="btn danger" data-revoke={m.id} disabled={!canRevoke || busy === m.id}
                                onClick={() => act(m, "mods.revokeDeepDesert", { id: m.id })}>Revoke</button>
                      </>
                    ) : dd === "grant-in-game" ? (
                      <span class="muted small" title="Granting Deep Desert is possible only in the game's overlay (Thumper/Deep Desert).">
                        Not granted · grant in the game
                      </span>
                    ) : <span class="muted">—</span>}
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      )}
      <p class="muted small">Deep Desert gives a mod raw memory access. It can be revoked here, but granted only from the in-game overlay.</p>
    </div>
  );
}
