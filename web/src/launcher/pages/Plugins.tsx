import { useEffect, useRef, useState } from "preact/hooks";
import type { Client } from "../../sdk/client";
import { errorText } from "../../sdk/hooks";
import { modsOf, stateText, stateTone, type ModInfo } from "../../panels/mods/model";
import type { Importer, Setting, SetupStatus, Val } from "../api";
import { importEventOf, importersOf, settingOf, valuesOf } from "../api";
import { Drawer } from "../components/Drawer";
import { SettingControl } from "../components/SettingControl";
import { busyNotice, importButtonLabel } from "../copy";
import { StoreIcon } from "../icons";

export function Plugins({ client, status, onOpenStore, onOpenImport }: {
  client: Client; status: SetupStatus | undefined; onOpenStore: () => void; onOpenImport: (plugin: string) => void;
}) {
  const batch = status?.busy;
  const batchWhy = batch ? busyNotice(batch) : undefined;
  const [list, setList] = useState<ModInfo[]>();
  const [importers, setImporters] = useState<Map<string, Importer>>(new Map());
  const [error, setError] = useState<string>();
  const [busy, setBusy] = useState<string>();
  const [drawer, setDrawer] = useState<string>();

  const load = () => {
    client.call<unknown>("mods.list").then((v) => { setList(modsOf(v)); setError(undefined); }, (e) => setError(errorText(e)));
  };
  useEffect(load, [client]);

  useEffect(() => {
    if (!client.has("import.list")) return;
    client.call<unknown>("import.list").then((v) => setImporters(new Map(importersOf(v).map((i) => [i.plugin, i]))), () => {});
    if (!client.has("import")) return;
    return client.subscribe<unknown>("import", undefined, (m) => {
      const ev = importEventOf(m);
      if (ev.importers) setImporters(new Map(ev.importers.map((i) => [i.plugin, i])));
      if (ev.job) setImporters((prev) => { const i = prev.get(ev.job!.plugin); if (!i) return prev; const next = new Map(prev); next.set(i.plugin, { ...i, job: ev.job }); return next; });
    });
  }, [client]);

  const toggle = async (m: ModInfo) => {
    setBusy(m.id);
    try {
      await client.call("mods.setEnabled", { id: m.id, on: !m.on });
      setList(modsOf(await client.call<unknown>("mods.list")));
    } catch (e) {
      setError(errorText(e));
    } finally {
      setBusy(undefined);
    }
  };

  return (
    <div data-page="plugins">
      <div class="row between" style="margin-bottom:16px">
        <h1 style="margin:0;font-size:18px">Plugins</h1>
        <button class="btn" onClick={onOpenStore}><StoreIcon size={16} /> Open Store</button>
      </div>
      {error ? <p class="error" role="alert">{error}</p> : null}
      {batch ? <p class="hint" role="status" aria-live="polite" data-busy>{batchWhy}</p> : null}
      {!list ? (
        <div class="lw-skel"><div class="lw-skel-row" /><div class="lw-skel-row" /></div>
      ) : list.length === 0 ? (
        <p class="muted">No plugins yet. Browse the Store to add some. <button class="link" onClick={onOpenStore}>Open Store</button></p>
      ) : (
        <ul class="lp-list" data-plugins>
          {list.map((m) => {
            const importer = importers.get(m.id);
            const job = importer?.job;
            const pct = job && job.total > 0 ? Math.round((job.bytes / job.total) * 100) : undefined;
            const importing = job && !["idle", "done", "error", "cancelled"].includes(job.phase);
            return (
              <li key={m.id} class="lp-row" data-plugin={m.id}>
                <button class="lp-switch" role="switch" aria-checked={m.on} disabled={busy === m.id} aria-label={`${m.on ? "Disable" : "Enable"} ${m.name}`}
                        data-toggle={m.id} onClick={() => toggle(m)} />
                <div class="lp-row-main">
                  <div class="lp-row-name">{m.name || m.id} <span class="muted small">{m.version}</span></div>
                  <div class="lp-row-desc">
                    {m.authors} · <span class={`tone-${stateTone(m.state)}`}>{stateText(m.state)}</span>
                    {importing ? <span class="muted"> · Importing…{pct !== undefined ? ` ${pct}%` : ""}</span> : null}
                  </div>
                </div>
                {importer ? (
                  <button class="btn" data-import-open={m.id} disabled={importer.status === "unsupported"}
                          title={importer.status === "unsupported" ? importer.statusReason : undefined} onClick={() => onOpenImport(m.id)}>
                    {importButtonLabel(importer.status)}
                  </button>
                ) : null}
                <button class="btn" data-settings={m.id} onClick={() => setDrawer(m.id)}>Settings</button>
              </li>
            );
          })}
        </ul>
      )}
      {drawer ? <PluginSettings client={client} id={drawer} name={list?.find((x) => x.id === drawer)?.name ?? drawer} busyWhy={batchWhy} onClose={() => setDrawer(undefined)} /> : null}
    </div>
  );
}

function PluginSettings({ client, id, name, busyWhy, onClose }: { client: Client; id: string; name: string; busyWhy?: string; onClose: () => void }) {
  const [decl, setDecl] = useState<Setting[]>();
  const [values, setValues] = useState<Record<string, Val>>({});
  const [defaults, setDefaults] = useState<Record<string, Val>>({});
  const [error, setError] = useState<string>();
  const [saved, setSaved] = useState(false);
  const timer = useRef<ReturnType<typeof setTimeout>>();

  useEffect(() => {
    client.call<unknown>("plugins.settings", { id }).then((r) => {
      const o = (r && typeof r === "object" ? r : {}) as Record<string, unknown>;
      setDecl((Array.isArray(o.decl) ? o.decl : []).map(settingOf).filter((x): x is Setting => !!x));
      setValues(valuesOf(o.values));
      setDefaults(valuesOf(o.defaults));
    }, (e) => setError(errorText(e)));
  }, [client, id]);

  const save = (next: Record<string, Val>) => {
    setValues(next);
    setSaved(false);
    if (timer.current) clearTimeout(timer.current);
    timer.current = setTimeout(async () => {
      try {
        await client.call("plugins.setSettings", { id, values: next });
        setSaved(true);
        setError(undefined);
      } catch (e) {
        setError(errorText(e));
      }
    }, 300);
  };

  const reset = async () => {
    try {
      const r = await client.call<{ values: unknown }>("plugins.resetSettings", { id });
      setValues(valuesOf(r.values));
      setSaved(true);
    } catch (e) {
      setError(errorText(e));
    }
  };

  return (
    <Drawer title={name} onClose={onClose} foot={<>
      {saved ? <span class="lx-saved" aria-live="polite">Saved</span> : <span />}
      <button class="link" disabled={!!busyWhy} title={busyWhy} onClick={reset}>Reset to default</button>
    </>}>
      {busyWhy ? <p class="hint" role="status" aria-live="polite">{busyWhy}</p> : null}
      {!decl ? <div class="lw-skel"><div class="lw-skel-row" /></div>
        : decl.length === 0 ? <p class="muted">This plugin has no settings.</p>
        : decl.map((d) => (
          <div class="lx-field" key={d.key}>
            <span class="lx-label">{d.label}{values[d.key] !== defaults[d.key] ? <span class="tag">changed</span> : null}</span>
            <SettingControl decl={d} value={values[d.key] ?? d.default} disabled={!!busyWhy} onChange={(v) => save({ ...values, [d.key]: v })} />
            {d.help ? <p class="lx-help">{d.help}</p> : null}
          </div>
        ))}
      {error ? <p class="error" role="alert">{error}</p> : null}
    </Drawer>
  );
}
