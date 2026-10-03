import { useEffect, useState } from "preact/hooks";
import type { Client } from "../../sdk/client";
import { errorText } from "../../sdk/hooks";
import { Ini } from "../../panels/ini";
import type { Defaults, SetupStatus, Theme } from "../api";
import { defaultsOf } from "../api";
import { UndoIcon } from "../icons";

export interface SettingsProps { client: Client; status: SetupStatus | undefined; theme: Theme; onTheme: (t: Theme) => void; onChangeFolder: () => void; }

export function Settings({ client, status, theme, onTheme, onChangeFolder }: SettingsProps) {
  const [busy, setBusy] = useState<string>();
  const [error, setError] = useState<string>();
  const [confirmUninstall, setConfirmUninstall] = useState(false);
  const [removeData, setRemoveData] = useState(false);
  const [defaults, setDefaults] = useState<Defaults>();
  const [indexUrl, setIndexUrl] = useState<{ url: string; custom: boolean }>();

  useEffect(() => {
    client.call<unknown>("defaults.get").then((r) => setDefaults(defaultsOf(r))).catch(() => {});
    client.call<{ indexUrl?: string; customIndex?: boolean }>("store.status").then((r) => setIndexUrl({ url: r.indexUrl ?? "", custom: !!r.customIndex })).catch(() => {});
  }, [client]);

  const run = async (action: "repair" | "enable" | "disable" | "uninstall") => {
    setBusy(action);
    setError(undefined);
    try {
      if (action === "enable" || action === "disable") await client.call("setup.setMelangeEnabled", { on: action === "enable" });
      else if (action === "repair") {
        const plan = await client.call<{ planId: string }>("setup.plan", { action: "repair" });
        await client.call("setup.apply", { action: "repair", planId: plan.planId });
      } else {
        const plan = await client.call<{ planId: string }>("setup.plan", { action: "uninstall", removeData });
        await client.call("setup.apply", { action: "uninstall", removeData, planId: plan.planId });
        setConfirmUninstall(false);
      }
    } catch (e) {
      setError(errorText(e));
    } finally {
      setBusy(undefined);
    }
  };

  const restore = async (backupId: string) => {
    setBusy(backupId);
    try { await client.call("setup.restore", { backupId }); } catch (e) { setError(errorText(e)); } finally { setBusy(undefined); }
  };
  const deleteBackup = async (backupId: string) => {
    setBusy(backupId);
    try { await client.call("setup.deleteBackup", { backupId }); } catch (e) { setError(errorText(e)); } finally { setBusy(undefined); }
  };

  const resetIndex = async () => {
    try { await client.call("ini.set", { section: "Store", key: "IndexUrl", value: "" }); setIndexUrl((s) => (s ? { ...s, custom: false } : s)); } catch (e) { setError(errorText(e)); }
  };

  return (
    <div data-page="settings">
      <h1 style="margin:0 0 16px;font-size:18px">Settings</h1>
      {error ? <p class="error" role="alert">{error}</p> : null}

      <section class="ls-section" data-section="game-folder">
        <h2>Game folder</h2>
        <div class="ls-row">
          <span class="ls-row-label mono small">{status?.game?.path ?? "Not set"}</span>
          <span class={status?.game?.verdict === "ok" ? "tone-ok" : "tone-bad"}>{status?.game?.verdict === "ok" ? "Ready" : status?.game?.verdict ?? ""}</span>
          <button class="btn" onClick={onChangeFolder}>Change…</button>
          <button class="btn" disabled={!status?.game} onClick={() => client.call("launcher.openPath", { what: "game" }).catch((e) => setError(errorText(e)))}>Open folder</button>
        </div>
      </section>

      <section class="ls-section" data-section="melange">
        <h2>Melange</h2>
        <div class="ls-row">
          <span class="ls-row-label">Version {status?.melange.version ?? "—"} · Loader {status?.loader.state === "ual" ? "Ultimate ASI Loader" : status?.loader.state ?? "—"}</span>
          <button class="btn" disabled={busy === "repair"} onClick={() => run("repair")}>Repair</button>
          {status?.melange.state === "disabled"
            ? <button class="btn" disabled={busy === "enable"} onClick={() => run("enable")}>Enable</button>
            : <button class="btn" disabled={busy === "disable"} onClick={() => run("disable")}>Disable</button>}
          <button class="btn danger" onClick={() => setConfirmUninstall(true)}>Uninstall…</button>
        </div>
        {confirmUninstall ? (
          <div class="lw-warn-box" role="alertdialog" data-confirm="uninstall">
            <p>This removes Melange from the game folder.</p>
            <label class="small"><input type="checkbox" checked={removeData} onChange={(e) => setRemoveData((e.currentTarget as HTMLInputElement).checked)} /> Also remove settings, logs and Store plugins</label>
            <div class="row" style="margin-top:10px">
              <button class="btn danger" disabled={busy === "uninstall"} onClick={() => run("uninstall")}>Uninstall</button>
              <button class="btn" onClick={() => setConfirmUninstall(false)}>Cancel</button>
            </div>
          </div>
        ) : null}
        <h2 style="margin-top:16px">Backups</h2>
        {!status?.backups.length ? <p class="muted small">No backups yet.</p> : status.backups.map((b) => (
          <div class="ls-backup" key={b.id} data-backup={b.id}>
            <span class="mono">{b.created}</span><span class="muted">{b.action}</span>
            <button class="btn" disabled={busy === b.id} onClick={() => restore(b.id)}><UndoIcon size={14} /> Restore</button>
            <button class="link" disabled={busy === b.id} onClick={() => deleteBackup(b.id)}>Delete</button>
          </div>
        ))}
      </section>

      <section class="ls-section" data-section="defaults">
        <h2>Defaults</h2>
        {!defaults?.plugins.length ? <p class="muted small">No default plugins set. Choose some on first run or from the Store.</p> : (
          <ul class="lh-list">
            {defaults.plugins.map((p) => <li key={p.id}>{p.id} — {p.enabled ? "enabled" : "disabled"}</li>)}
          </ul>
        )}
      </section>

      <section class="ls-section" data-section="ini">
        <h2>Melange.ini</h2>
        <div style="height:360px;border:1px solid var(--border);border-radius:8px;overflow:hidden"><Ini client={client} /></div>
      </section>

      <section class="ls-section" data-section="appearance">
        <h2>Appearance</h2>
        <div class="lw-seg" role="group" aria-label="Theme">
          {(["system", "light", "dark"] as Theme[]).map((t) => (
            <button key={t} aria-pressed={theme === t} onClick={() => onTheme(t)}>{t === "system" ? "System" : t === "light" ? "Light" : "Dark"}</button>
          ))}
        </div>
      </section>

      <section class="ls-section" data-section="store-source">
        <h2>Store source</h2>
        <div class="ls-row">
          <span class="ls-row-label mono small">{indexUrl?.url || "default"}</span>
          {indexUrl?.custom ? <button class="link" onClick={resetIndex}>Reset</button> : null}
        </div>
      </section>
    </div>
  );
}
