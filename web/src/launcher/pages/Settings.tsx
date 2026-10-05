import { useEffect, useState } from "preact/hooks";
import type { Client } from "../../sdk/client";
import { errorText } from "../../sdk/hooks";
import { Ini } from "../../panels/ini";
import type { Defaults, SetupStatus, Theme, UpdateStatus } from "../api";
import { defaultsOf, updateStatusOf, whenText } from "../api";
import { busyNotice, updateCheckLine } from "../copy";
import { UndoIcon } from "../icons";

export interface SettingsProps { client: Client; status: SetupStatus | undefined; update?: UpdateStatus; theme: Theme; onTheme: (t: Theme) => void; onChangeFolder: () => void; }

export function Settings({ client, status, update, theme, onTheme, onChangeFolder }: SettingsProps) {
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

  useEffect(() => { if (!status?.running) setError(undefined); }, [status?.running]);

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

  // The switch follows the server's answer at once; the "update" channel brings the same value to the rest of the app.
  const [autoSet, setAutoSet] = useState<boolean>();
  useEffect(() => setAutoSet(undefined), [update?.auto]);
  const autoOn = autoSet ?? update?.auto ?? true;
  const setAuto = async (on: boolean) => {
    setBusy("auto");
    setError(undefined);
    try { setAutoSet(updateStatusOf(await client.call<unknown>("update.setAuto", { on })).auto ?? on); } catch (e) { setError(errorText(e)); } finally { setBusy(undefined); }
  };

  const batch = status?.busy;
  const batchWhy = batch ? busyNotice(batch) : undefined;

  return (
    <div data-page="settings">
      <h1 style="margin:0 0 16px;font-size:18px">Settings</h1>
      {error ? <p class="error" role="alert">{error}</p> : null}
      {batch ? <p class="hint" role="status" aria-live="polite" data-busy>{batchWhy}</p> : null}

      <section class="ls-section" data-section="game-folder">
        <h2>Game folder</h2>
        <div class="ls-row">
          <span class="ls-row-label mono small">{status?.game?.path ?? "Not set"}</span>
          <span class={status?.game?.verdict === "ok" ? "tone-ok" : "tone-bad"}>{status?.game?.verdict === "ok" ? "Ready" : status?.game?.verdict ?? ""}</span>
          <button class="btn" onClick={onChangeFolder}>Change…</button>
          <button class="btn" disabled={!status?.game} onClick={() => client.call("launcher.openPath", { what: "game" }).catch((e) => setError(errorText(e)))}>Open folder</button>
        </div>
      </section>

      <section class="ls-section" data-section="updates">
        <h2>Updates</h2>
        <div class="ls-row">
          <span class="ls-row-label" role="status" aria-live="polite" data-update-line>{update ? updateCheckLine(update) : "—"}</span>
          <button class="btn" data-update-check disabled={!update || update.phase === "checking" || update.phase === "downloading"}
                  onClick={() => client.call("update.check").catch((e) => setError(errorText(e)))}>Check for updates</button>
        </div>
        <div class="row" style="margin-top:8px;gap:8px;align-items:center">
          <button class="lp-switch" role="switch" aria-checked={autoOn} aria-labelledby="ls-auto-update" data-update-auto
                  disabled={!update || busy === "auto"} onClick={() => setAuto(!autoOn)} />
          <span id="ls-auto-update">Check for updates automatically</span>
        </div>
        <p class="muted small">When this is on, Melange looks for a newer release on GitHub each time it starts, and the game
          looks at most once a day and shows a notice when one is out. Each look is a plain HTTPS request that sends nothing about you or
          your game. A newer release is downloaded and checked in the background, and installed when you choose Restart to
          update. Turn it off and Melange only looks when you choose Check for updates.</p>
      </section>

      <section class="ls-section" data-section="melange">
        <h2>Melange</h2>
        <div class="ls-row">
          <span class="ls-row-label">{melangeText(status)} · Loader {loaderText(status)}</span>
          <button class="btn" disabled={busy === "repair" || !!batch} title={batchWhy} onClick={() => run("repair")}>{status?.melange.state === "missing" ? "Install" : "Repair"}</button>
          {status?.melange.state === "missing" ? null : status?.melange.state === "disabled"
            ? <button class="btn" disabled={busy === "enable" || !!batch} title={batchWhy} onClick={() => run("enable")}>Enable</button>
            : <button class="btn" disabled={busy === "disable" || !!batch} title={batchWhy} onClick={() => run("disable")}>Disable</button>}
          {status?.melange.state === "missing" ? null : <button class="btn danger" disabled={!!batch} title={batchWhy} onClick={() => setConfirmUninstall(true)}>Uninstall…</button>}
        </div>
        {confirmUninstall ? (
          <div class="lw-warn-box" role="alertdialog" data-confirm="uninstall">
            <p>This removes Melange from the game folder.</p>
            <label class="small"><input type="checkbox" checked={removeData} onChange={(e) => setRemoveData((e.currentTarget as HTMLInputElement).checked)} /> Also remove settings, logs and Store plugins</label>
            <div class="row" style="margin-top:10px">
              <button class="btn danger" disabled={busy === "uninstall" || !!batch} title={batchWhy} onClick={() => run("uninstall")}>Uninstall</button>
              <button class="btn" onClick={() => setConfirmUninstall(false)}>Cancel</button>
            </div>
          </div>
        ) : null}
        <h2 style="margin-top:16px">Backups</h2>
        {!status?.backups.length ? <p class="muted small">No backups yet.</p> : status.backups.map((b) => (
          <div class="ls-backup" key={b.id} data-backup={b.id}>
            <span class="mono">{whenText(b.created)}</span><span class="muted">{b.action}</span>
            <button class="btn" disabled={busy === b.id || !!batch} title={batchWhy} onClick={() => restore(b.id)}><UndoIcon size={14} /> Restore</button>
            <button class="link" disabled={busy === b.id || !!batch} title={batchWhy} onClick={() => deleteBackup(b.id)}>Delete</button>
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

function melangeText(s: SetupStatus | undefined): string {
  const m = s?.melange;
  if (!m) return "—";
  if (m.state === "missing") return "Not installed";
  return `Version ${m.version ?? "—"}${m.state === "disabled" ? " (disabled)" : m.state === "damaged" ? " (needs repair)" : ""}`;
}

function loaderText(s: SetupStatus | undefined): string {
  const l = s?.loader;
  if (!l) return "—";
  if (l.state === "ual") return "Ultimate ASI Loader";
  if (l.state === "none") return "not installed";
  return l.dll?.description || l.dll?.product || "another program's dinput8.dll";
}
