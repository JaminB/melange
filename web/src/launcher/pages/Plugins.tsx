import { useEffect, useRef, useState } from "preact/hooks";
import type { Client } from "../../sdk/client";
import { errorText } from "../../sdk/hooks";
import { modsOf, stateText, stateTone, type ModInfo } from "../../panels/mods/model";
import type { Setting, Val } from "../api";
import { settingOf, valuesOf } from "../api";
import { Drawer } from "../components/Drawer";
import { SettingControl } from "../components/SettingControl";
import { StoreIcon } from "../icons";

export function Plugins({ client, onOpenStore }: { client: Client; onOpenStore: () => void }) {
  const [list, setList] = useState<ModInfo[]>();
  const [error, setError] = useState<string>();
  const [busy, setBusy] = useState<string>();
  const [drawer, setDrawer] = useState<string>();

  const load = () => {
    client.call<unknown>("mods.list").then((v) => { setList(modsOf(v)); setError(undefined); }, (e) => setError(errorText(e)));
  };
  useEffect(load, [client]);

  const toggle = async (m: ModInfo) => {
    setBusy(m.id);
    try {
      const r = modsOf([await client.call("mods.setEnabled", { id: m.id, on: !m.on })]);
      if (r[0]) setList((l) => (l ?? []).map((x) => (x.id === r[0].id ? r[0] : x)));
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
      {!list ? (
        <div class="lw-skel"><div class="lw-skel-row" /><div class="lw-skel-row" /></div>
      ) : list.length === 0 ? (
        <p class="muted">No plugins yet. Browse the Store to add some. <button class="link" onClick={onOpenStore}>Open Store</button></p>
      ) : (
        <ul class="lp-list" data-plugins>
          {list.map((m) => (
            <li key={m.id} class="lp-row" data-plugin={m.id}>
              <button class="lp-switch" role="switch" aria-checked={m.on} disabled={busy === m.id} aria-label={`${m.on ? "Disable" : "Enable"} ${m.name}`}
                      data-toggle={m.id} onClick={() => toggle(m)} />
              <div class="lp-row-main">
                <div class="lp-row-name">{m.name || m.id} <span class="muted small">{m.version}</span></div>
                <div class="lp-row-desc">{m.authors} · <span class={`tone-${stateTone(m.state)}`}>{stateText(m.state)}</span></div>
              </div>
              <button class="btn" data-settings={m.id} onClick={() => setDrawer(m.id)}>Settings</button>
            </li>
          ))}
        </ul>
      )}
      {drawer ? <PluginSettings client={client} id={drawer} name={list?.find((x) => x.id === drawer)?.name ?? drawer} onClose={() => setDrawer(undefined)} /> : null}
    </div>
  );
}

function PluginSettings({ client, id, name, onClose }: { client: Client; id: string; name: string; onClose: () => void }) {
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
      <button class="link" onClick={reset}>Reset to default</button>
    </>}>
      {!decl ? <div class="lw-skel"><div class="lw-skel-row" /></div>
        : decl.length === 0 ? <p class="muted">This plugin has no settings.</p>
        : decl.map((d) => (
          <div class="lx-field" key={d.key}>
            <span class="lx-label">{d.label}{values[d.key] !== defaults[d.key] ? <span class="tag">changed</span> : null}</span>
            <SettingControl decl={d} value={values[d.key] ?? d.default} onChange={(v) => save({ ...values, [d.key]: v })} />
            {d.help ? <p class="lx-help">{d.help}</p> : null}
          </div>
        ))}
      {error ? <p class="error" role="alert">{error}</p> : null}
    </Drawer>
  );
}
