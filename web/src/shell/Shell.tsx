// The app shell: connection badge, the panel list, and the selected panel (lazy-loaded on first open).
import { render } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import { createClient, type Client, type ClientState, type OasisClient, type Welcome } from "../sdk/client";
import { onPanelsChanged, panels, unmetReason, type PanelDef } from "../sdk/panels";
import type { PanelInfo } from "../sdk/protocol";
import { ExtPanel } from "./ext/host";

const STORE_KEY = "oasis.panel";

function remembered(): string | undefined {
  const h = location.hash.replace(/^#\/?/, "");
  if (h) return h;
  try {
    return localStorage.getItem(STORE_KEY) ?? undefined;
  } catch {
    return undefined;
  }
}

function remember(id: string) {
  try {
    localStorage.setItem(STORE_KEY, id);
  } catch {
    /* storage unavailable */
  }
  if (location.hash !== `#/${id}`) history.replaceState(null, "", `#/${id}`);
}

function reloadOnce() {
  try {
    if (sessionStorage.getItem("oasis.reloaded") === "1") return;
    sessionStorage.setItem("oasis.reloaded", "1");
  } catch {
    return;
  }
  location.reload();
}

export function startShell(root: HTMLElement) {
  const client = createClient({ build: typeof __OASIS_BUILD__ === "string" ? __OASIS_BUILD__ : "dev", onStale: reloadOnce });
  render(<App client={client} />, root);
}

const STATE_TEXT: Record<ClientState, string> = {
  connecting: "Connecting",
  open: "Connected",
  closed: "Reconnecting",
  offline: "Offline",
};

function App({ client }: { client: OasisClient }) {
  const [state, setState] = useState<ClientState>(client.state);
  const [welcome, setWelcome] = useState<Welcome | undefined>(client.welcome());
  const [list, setList] = useState<PanelDef[]>(panels());
  const [inMatch, setInMatch] = useState<boolean | undefined>(undefined);
  const [selected, setSelected] = useState<string | undefined>(remembered());

  useEffect(() => {
    const sync = (s: ClientState) => {
      setState(s);
      setWelcome(client.welcome());
      if (s === "open") try { sessionStorage.removeItem("oasis.reloaded"); } catch { /* storage unavailable */ }
    };
    sync(client.state);  // the state may have changed before this effect subscribed
    return client.onState(sync);
  }, []);
  useEffect(() => onPanelsChanged(() => setList(panels())), []);
  useEffect(() => {
    if (state !== "open" || !client.has("state")) return;
    return client.subscribe<{ match?: { inMatch?: boolean } }>("state", { hz: 1 }, (s) => setInMatch(!!s.match?.inMatch));
  }, [state]);
  useEffect(() => {
    const onHash = () => setSelected(remembered());
    window.addEventListener("hashchange", onHash);
    return () => window.removeEventListener("hashchange", onHash);
  }, []);

  const localIds = new Set(list.map((p) => p.id));
  const extPanels: PanelInfo[] = (welcome?.panels ?? []).filter((p) => !localIds.has(p.id));
  const tabs: Array<{ id: string; title: string }> = [...list, ...extPanels];
  const current = tabs.find((p) => p.id === selected) ?? tabs[0];
  const currentExt = current ? extPanels.find((p) => p.id === current.id) : undefined;
  const select = (id: string) => {
    setSelected(id);
    remember(id);
  };

  return (
    <div class="shell">
      <header class="top">
        <div class="brand" aria-label="Oasis">
          <svg viewBox="0 0 24 24" width="20" height="20" aria-hidden="true"><circle cx="12" cy="12" r="10" class="brand-sun" /><path d="M3 15c3-2 6-2 9 0s6 2 9 0" class="brand-wave" /></svg>
          <span>Oasis</span>
        </div>
        <div class="top-info">
          {welcome ? <span>{welcome.server === "game" ? "Game" : "Standalone"}{welcome.game ? ` · Melange ${welcome.game.melange}` : ""}</span> : null}
        </div>
        <div class={`badge badge-${state}`} role="status" aria-live="polite">
          <span class="dot" />
          {STATE_TEXT[state]}
        </div>
      </header>
      {state !== "open" ? <ConnectionNotice state={state} client={client} /> : null}
      <div class="body">
        <nav class="tabs" aria-label="Panels">
          {list.map((p) => {
            const why = unmetReason(p, welcome, inMatch);
            return (
              <button
                key={p.id}
                class={`tab${current?.id === p.id ? " active" : ""}`}
                aria-current={current?.id === p.id ? "page" : undefined}
                disabled={!!why}
                title={why}
                data-panel={p.id}
                onClick={() => select(p.id)}
              >
                {p.title}
                {why ? <span class="tab-why">{why}</span> : null}
              </button>
            );
          })}
          {extPanels.map((p) => (
            <button
              key={p.id}
              class={`tab${current?.id === p.id ? " active" : ""}`}
              aria-current={current?.id === p.id ? "page" : undefined}
              data-panel={p.id}
              onClick={() => select(p.id)}
            >
              {p.title}
            </button>
          ))}
        </nav>
        <main class="panel" aria-label={current?.title}>
          {currentExt ? (
            <ExtPanel key={currentExt.id} info={currentExt} client={client} />
          ) : current ? (
            <PanelHost key={current.id} def={current as PanelDef} client={client} />
          ) : (
            <p class="muted">No panels.</p>
          )}
        </main>
      </div>
    </div>
  );
}

function ConnectionNotice({ state, client }: { state: ClientState; client: OasisClient }) {
  const last = client.lastClose();
  let text = "Connecting to the game…";
  if (state === "closed") text = "The connection to the game was lost. Reconnecting…";
  if (state === "offline")
    text = last?.code === 4001
      ? "This page and the game speak different protocol versions. Reload the page."
      : "Not connected. If the game restarted, open Oasis again from the in-game overlay to get a new link.";
  return <div class={`notice notice-${state}`} role="alert">{text}</div>;
}

function PanelHost({ def, client }: { def: PanelDef; client: Client }) {
  const el = useRef<HTMLDivElement>(null);
  const [error, setError] = useState<string>();
  useEffect(() => {
    let cleanup: (() => void) | undefined;
    let dead = false;
    def.load().then(
      (m) => {
        if (!dead && el.current) cleanup = m.mount(el.current, client);
      },
      (e) => setError(String(e?.message ?? e)),
    );
    return () => {
      dead = true;
      cleanup?.();
    };
  }, [def.id]);
  return error ? <p class="error">Could not load {def.title}: {error}</p> : <div ref={el} class="panel-host" data-panel-host={def.id} />;
}
