import { useEffect, useState } from "preact/hooks";
import type { Client } from "../../sdk/client";
import { errorText } from "../../sdk/hooks";
import { loadedSinceInstall, whenText, type SetupStatus } from "../api";
import { busyNotice } from "../copy";
import { StatusCard, type Tone } from "../components/StatusCard";
import { FolderIcon, PlugIcon, PuzzleIcon, SparkleIcon } from "../icons";

export interface HomeProps {
  client: Client; status: SetupStatus | undefined; onFixGame: () => void; onOpenPlugins: () => void; onOpenSettings: () => void;
}

const STORE_LABEL: Record<string, string> = { steam: "Steam", gog: "GOG", unknown: "" };

export function Home({ client, status, onFixGame, onOpenPlugins, onOpenSettings }: HomeProps) {
  const [counts, setCounts] = useState<{ on: number; updates: number } | undefined>();
  const [storeDown, setStoreDown] = useState(false);
  const [restoreBusy, setRestoreBusy] = useState(false);
  const [notice, setNotice] = useState(true);

  useEffect(() => {
    let dead = false;
    (async () => {
      try {
        const mods = await client.call<unknown[]>("mods.list");
        const on = Array.isArray(mods) ? mods.filter((m) => (m as { on?: boolean })?.on).length : 0;
        let updates = 0;
        try { updates = ((await client.call<unknown[]>("store.list", { filter: "updates" })) ?? []).length; } catch { /* store not reachable */ }
        if (!dead) { setCounts({ on, updates }); setStoreDown(false); }
      } catch {
        if (!dead) setStoreDown(true);
      }
    })();
    return () => { dead = true; };
  }, [client, status?.melange.state]);

  if (!status) return <div class="lc-grid"><SkeletonCards /></div>;

  const game = status.game;
  const store = game ? STORE_LABEL[game.store] : "";
  const gameCard: { tone: Tone; text: string; detail?: string; action?: string; onAction?: () => void } =
    status.running ? { tone: "warn", text: "Running", detail: game?.path }
    : !game || game.verdict !== "ok" ? { tone: "bad", text: game?.verdict === "wrongBuild" ? "Unsupported build" : "Not found", action: "Fix", onAction: onFixGame }
    : { tone: "ok", text: `Build #1077${store ? ` · ${store}` : ""}`, detail: game.path };

  const loader = status.loader;
  const ualOther = status.otherLoaders.find((d) => d.kind === "ual");
  const loaderCard: { tone: Tone; text: string; detail?: string; action?: string } =
    loader.state === "ual" ? { tone: "ok", text: `${loader.dll?.description || loader.dll?.product || "Ultimate ASI Loader"} ${loader.dll?.version ?? ""}`.trim() }
    : loader.state === "none" && ualOther ? { tone: "warn", text: `Using ${ualOther.file} as loader` }
    : loader.state === "none" ? { tone: "bad", text: "Missing", action: "Repair" }
    : { tone: "bad", text: `${loader.dll?.description || loader.dll?.product || "Unknown program"}'s dinput8.dll`, action: "Repair" };

  const m = status.melange;
  const melangeCard: { tone: Tone; text: string; detail?: string; action?: string } =
    m.state === "missing" ? { tone: "bad", text: "Not installed", action: "Install" }
    : m.state === "damaged" ? { tone: "bad", text: "Damaged", action: "Repair" }
    : m.state === "disabled" ? { tone: "warn", text: "Disabled", action: "Enable" }
    : m.state === "older" ? { tone: "warn", text: `${m.version} · update available`, action: "Update" }
    : m.lastLoad && loadedSinceInstall(status) ? { tone: "ok", text: `${m.version} · last loaded ${whenText(m.lastLoad.at)}` }
    : { tone: "warn", text: `${m.version} · not loaded yet` };

  const pluginsCard: { tone: Tone; text: string; detail?: string } =
    storeDown ? { tone: "warn", text: "Store unreachable" }
    : counts ? { tone: counts.updates ? "warn" : "ok", text: `${counts.on} on${counts.updates ? ` · ${counts.updates} update${counts.updates === 1 ? "" : "s"}` : ""}` }
    : { tone: "ok", text: "…" };

  const canRestore = notice && status.install?.loader === "replaced" && status.backups.length > 0;
  const busy = status.busy;
  const restore = async () => {
    const backup = status.backups[0];
    if (!backup) return;
    setRestoreBusy(true);
    try { await client.call("setup.restore", { backupId: backup.id }); } catch (e) { void errorText(e); } finally { setRestoreBusy(false); setNotice(false); }
  };

  return (
    <div data-page="home">
      {busy ? <div class="lc-notice" role="status" aria-live="polite" data-notice="busy">{busyNotice(busy)}</div> : null}
      {canRestore ? (
        <div class="lc-notice" role="status" data-notice="restore">
          <span>You replaced the previous dinput8.dll. <button class="link" disabled={restoreBusy || !!busy} title={busy ? busyNotice(busy) : undefined} onClick={restore}>Restore it</button></span>
          <button class="link" onClick={() => setNotice(false)} aria-label="Dismiss">Dismiss</button>
        </div>
      ) : null}
      {status.running ? <div class="lc-notice" role="status" data-notice="running">Melange is running in the game. Changes are paused until you close it.</div> : null}
      <div class="lc-grid">
        <StatusCard id="game" icon={<FolderIcon />} label="Game" tone={gameCard.tone} status={gameCard.text} detail={gameCard.detail}
                    actionLabel={gameCard.action} onAction={gameCard.onAction} />
        <StatusCard id="loader" icon={<PlugIcon />} label="Loader" tone={loaderCard.tone} status={loaderCard.text} detail={loaderCard.detail}
                    actionLabel={loaderCard.action} onAction={onOpenSettings} />
        <StatusCard id="melange" icon={<SparkleIcon />} label="Melange" tone={melangeCard.tone} status={melangeCard.text} detail={melangeCard.detail}
                    actionLabel={melangeCard.action} onAction={onOpenSettings} />
        <StatusCard id="plugins" icon={<PuzzleIcon />} label="Plugins" tone={pluginsCard.tone} status={pluginsCard.text} detail={pluginsCard.detail}
                    actionLabel={!storeDown ? "Manage" : undefined} onAction={onOpenPlugins} />
      </div>
    </div>
  );
}

function SkeletonCards() {
  return <>{[0, 1, 2, 3].map((i) => <div class="lc-card" key={i}><div class="lw-skel-row" style="height:64px" /></div>)}</>;
}
