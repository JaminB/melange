import type { JSX } from "preact";
import { useEffect, useReducer, useState } from "preact/hooks";
import type { Client, ClientState } from "../sdk/client";
import { errorText, useConnection } from "../sdk/hooks";
import { Store } from "../panels/store";
import { launcherStateOf, setupEventOf, setupStatusOf, type LauncherState, type SetupStatus, type Theme } from "./api";
import { ExternalLinkIcon, GearIcon, LifeBuoyIcon, PlayIcon, PuzzleIcon, SparkleIcon, StoreIcon } from "./icons";
import { Help } from "./pages/Help";
import { Home } from "./pages/Home";
import { Plugins } from "./pages/Plugins";
import { Settings } from "./pages/Settings";
import { initialWizard, wizardReducer } from "./state";
import { CheckGame } from "./wizard/CheckGame";
import { FindGame } from "./wizard/FindGame";
import { Install } from "./wizard/Install";
import { Ready } from "./wizard/Ready";
import { Recommended } from "./wizard/Recommended";
import { Welcome } from "./wizard/Welcome";
import { StepRail } from "./components/StepRail";

type Page = "home" | "plugins" | "store" | "settings" | "help";
const PAGES: { id: Page; label: string; icon: (s: number) => JSX.Element }[] = [
  { id: "home", label: "Home", icon: (s) => <SparkleIcon size={s} /> },
  { id: "plugins", label: "Plugins", icon: (s) => <PuzzleIcon size={s} /> },
  { id: "store", label: "Store", icon: (s) => <StoreIcon size={s} /> },
  { id: "settings", label: "Settings", icon: (s) => <GearIcon size={s} /> },
  { id: "help", label: "Help", icon: (s) => <LifeBuoyIcon size={s} /> },
];

function applyTheme(t: Theme) {
  const root = document.documentElement;
  if (t === "system") delete root.dataset.theme;
  else root.dataset.theme = t;
}

function Brand() {
  return (
    <span class="la-brand">
      <svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="10" fill="var(--accent-strong)" /><path d="M3 15c3-2 6-2 9 0s6 2 9 0" fill="none" stroke="var(--surface)" stroke-width="2.2" stroke-linecap="round" /></svg>
      <span>Melange</span>
    </span>
  );
}

const STATE_TEXT: Record<ClientState, string> = { connecting: "Connecting", open: "Connected", closed: "Reconnecting", offline: "Offline" };

export function LauncherApp({ client }: { client: Client }) {
  const conn = useConnection(client);
  const [launcher, setLauncher] = useState<LauncherState>();
  const [status, setStatus] = useState<SetupStatus>();
  const [inWizard, setInWizard] = useState<boolean>();
  const [page, setPage] = useState<Page>("home");
  const [wizard, dispatch] = useReducer(wizardReducer, undefined, initialWizard);
  const [launchError, setLaunchError] = useState<string>();
  const [launching, setLaunching] = useState(false);

  useEffect(() => {
    if (!conn.open) return;
    client.call<unknown>("launcher.state").then((r) => {
      const s = launcherStateOf(r);
      setLauncher(s);
      setInWizard((prev) => prev ?? s.firstRun);
    }).catch(() => {});
  }, [conn.open, client]);

  const loadStatus = () => client.call<unknown>("setup.status").then((r) => setStatus(setupStatusOf(r))).catch(() => {});
  useEffect(() => {
    if (!conn.open || inWizard !== false) return;
    loadStatus();
    if (!client.has("setup")) return;
    return client.subscribe<unknown>("setup", undefined, (m) => {
      const ev = setupEventOf(m);
      if (ev.status) setStatus(ev.status);
    });
  }, [conn.open, inWizard, client]);

  const setTheme = (t: Theme) => {
    setLauncher((l) => (l ? { ...l, theme: t } : l));
    applyTheme(t);
    client.call("launcher.setTheme", { theme: t }).catch(() => {});
  };
  useEffect(() => { if (launcher) applyTheme(launcher.theme); }, [launcher?.theme]);

  const launch = async () => {
    setLaunching(true);
    setLaunchError(undefined);
    try { await client.call("launcher.launch"); } catch (e) { setLaunchError(errorText(e)); } finally { setLaunching(false); }
  };

  if (inWizard === undefined || !conn.open) {
    return <div class="lw" aria-label="Starting Melange"><div class="lw-col"><div class="lw-skel" style="margin-top:80px"><div class="lw-skel-row" /></div></div></div>;
  }

  if (inWizard) {
    return (
      <div class="lw">
        <div class="lw-col">
          {wizard.step === "welcome" ? (
            <div class="lw-brand" aria-hidden="true">
              <svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="10" fill="var(--accent-strong)" /><path d="M3 15c3-2 6-2 9 0s6 2 9 0" fill="none" stroke="var(--bg)" stroke-width="2.2" stroke-linecap="round" /></svg>
              <span class="lw-brand-name">Melange</span>
            </div>
          ) : <StepRail step={wizard.step} />}
          {wizard.step === "welcome" ? <Welcome client={client} state={wizard} dispatch={dispatch} />
            : wizard.step === "find" ? <FindGame client={client} state={wizard} dispatch={dispatch} />
            : wizard.step === "check" ? <CheckGame client={client} state={wizard} dispatch={dispatch} />
            : wizard.step === "install" ? <Install client={client} state={wizard} dispatch={dispatch} />
            : wizard.step === "recommended" ? <Recommended client={client} state={wizard} dispatch={dispatch} />
            : <Ready client={client} state={wizard} dispatch={dispatch} onFinish={() => { setInWizard(false); setLauncher((l) => (l ? { ...l, firstRun: false } : l)); }} />}
        </div>
      </div>
    );
  }

  const runLabel = status?.running ? "Running" : !status || status.melange.state === "missing" ? "Launch without Melange" : "Launch game";

  return (
    <div class="la">
      <header class="la-top">
        <Brand />
        <span class="la-version muted">{launcher?.version}</span>
        <div class={`la-status-badge badge-${conn.state}`} role="status" aria-live="polite"><span class="dot" />{STATE_TEXT[conn.state]}</div>
        <button class="btn btn-primary la-launch" disabled={status?.running || launching} title={launchError} onClick={launch}>
          <PlayIcon size={14} />{launching ? "Launching…" : runLabel}
        </button>
      </header>
      <div class="la-body">
        <nav class="la-side" aria-label="Sections">
          {PAGES.map((p) => (
            <button key={p.id} class="la-tab" aria-current={page === p.id ? "page" : undefined} data-page-tab={p.id} onClick={() => setPage(p.id)}>
              {p.icon(16)}<span class="la-tab-label">{p.label}</span>
            </button>
          ))}
          <a class="la-tab" href="/#oasis" target="_blank" rel="noreferrer"><ExternalLinkIcon size={16} /><span class="la-tab-label">Oasis tools</span></a>
        </nav>
        <main class="la-content" aria-label={PAGES.find((p) => p.id === page)?.label}>
          <div class="la-content-inner">
            {page === "home" ? <Home client={client} status={status} onFixGame={() => { dispatch({ type: "goto", step: "find" }); setInWizard(true); }}
                                      onOpenPlugins={() => setPage("plugins")} onOpenSettings={() => setPage("settings")} />
              : page === "plugins" ? <Plugins client={client} status={status} onOpenStore={() => setPage("store")} />
              : page === "store" ? <Store client={client} />
              : page === "settings" ? <Settings client={client} status={status} theme={launcher?.theme ?? "system"} onTheme={setTheme}
                                                 onChangeFolder={() => { dispatch({ type: "goto", step: "find" }); setInWizard(true); }} />
              : <Help client={client} version={launcher?.version ?? ""} />}
          </div>
        </main>
      </div>
    </div>
  );
}
