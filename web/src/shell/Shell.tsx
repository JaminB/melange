// The app shell: header with the connection badge, the panel tabs, one panel or two side by side (split), the
// command palette (Ctrl+K) and the theme. Panels are lazy-loaded on first open; the layout is remembered.
import { render } from "preact";
import { useEffect, useMemo, useRef, useState } from "preact/hooks";
import { createClient, type Client, type ClientState, type OasisClient, type Welcome } from "../sdk/client";
import { onPanelsChanged, panels, unmetReason, type PanelDef } from "../sdk/panels";
import { SplitPane } from "../sdk/ui";
import type { PanelInfo } from "../sdk/protocol";
import { ExtPanel } from "./ext/host";
import { Palette, type Command } from "./Palette";
import { closeSecondary, loadLayout, openPanel, resolve, saveLayout, type Layout, type Theme } from "./layout";

function storage(): Storage | undefined {
  try {
    return window.localStorage;
  } catch {
    return undefined;
  }
}

function hashPanel(): string | undefined {
  const h = location.hash.replace(/^#\/?/, "");
  return h || undefined;
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

function applyTheme(t: Theme) {
  const root = document.documentElement;
  if (t === "system") delete root.dataset.theme;
  else root.dataset.theme = t;
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

type Tab = PanelDef & { ext?: PanelInfo };

function extTab(p: PanelInfo): Tab {
  return { id: p.id, title: p.title, order: 1000, needs: [], load: () => Promise.reject(new Error("web panel")), ext: p };
}

const THEME_TEXT: Record<Theme, string> = { system: "Theme: system", light: "Theme: light", dark: "Theme: dark" };
const NEXT_THEME: Record<Theme, Theme> = { system: "light", light: "dark", dark: "system" };

export function App({ client }: { client: OasisClient }) {
  const [state, setState] = useState<ClientState>(client.state);
  const [welcome, setWelcome] = useState<Welcome | undefined>(client.welcome());
  const [list, setList] = useState<PanelDef[]>(panels());
  const [inMatch, setInMatch] = useState<boolean | undefined>(undefined);
  const [layout, setLayoutRaw] = useState<Layout>(() => {
    const l = loadLayout(storage());
    const h = hashPanel();
    return h ? openPanel(l, h) : l;
  });
  const [palette, setPalette] = useState(false);

  const setLayout = (l: Layout) => {
    setLayoutRaw(l);
    saveLayout(storage(), l);
    if (l.primary && location.hash !== `#/${l.primary}`) history.replaceState(null, "", `#/${l.primary}`);
  };

  useEffect(() => {
    const sync = (s: ClientState) => {
      setState(s);
      setWelcome(client.welcome());
      if (s === "open") try { sessionStorage.removeItem("oasis.reloaded"); } catch { /* storage unavailable */ }
    };
    sync(client.state);
    return client.onState(sync);
  }, []);
  useEffect(() => onPanelsChanged(() => setList(panels())), []);
  useEffect(() => {
    if (state !== "open" || !client.has("state")) return;
    return client.subscribe<{ match?: { inMatch?: boolean } }>("state", { hz: 1 }, (s) => setInMatch(!!s.match?.inMatch));
  }, [state]);
  useEffect(() => {
    const onHash = () => {
      const h = hashPanel();
      if (h) setLayoutRaw((l) => openPanel(l, h));
    };
    window.addEventListener("hashchange", onHash);
    return () => window.removeEventListener("hashchange", onHash);
  }, []);
  useEffect(() => applyTheme(layout.theme), [layout.theme]);
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if ((e.ctrlKey || e.metaKey) && !e.altKey && (e.key === "k" || e.key === "K" || (e.shiftKey && (e.key === "p" || e.key === "P")))) {
        e.preventDefault();
        setPalette((p) => !p);
      }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, []);

  const localIds = new Set(list.map((p) => p.id));
  const all: Tab[] = [...list, ...(welcome?.panels ?? []).filter((p) => !localIds.has(p.id)).map(extTab)];
  const view = resolve(layout, all.map((p) => p.id));
  const byId = (id?: string) => all.find((p) => p.id === id);
  const primary = byId(view.primary);
  const secondary = byId(view.secondary);

  const commands = useMemo<Command[]>(() => {
    const out: Command[] = [];
    for (const p of all) {
      const why = unmetReason(p, welcome, inMatch);
      out.push({ id: `open:${p.id}`, label: `Open ${p.title}`, hint: why, disabled: !!why, run: () => setLayout(openPanel(layout, p.id)) });
      out.push({ id: `beside:${p.id}`, label: `Open ${p.title} beside`, hint: why, disabled: !!why || p.id === view.primary,
        run: () => setLayout(openPanel(layout, p.id, true)) });
    }
    if (view.secondary) {
      out.push({ id: "close-side", label: "Close the side panel", run: () => setLayout(closeSecondary(layout)) });
      out.push({ id: "swap", label: "Swap the two panels", run: () => setLayout({ ...layout, primary: view.secondary, secondary: view.primary }) });
      out.push({ id: "direction", label: layout.direction === "row" ? "Stack the panels vertically" : "Put the panels side by side",
        run: () => setLayout({ ...layout, direction: layout.direction === "row" ? "column" : "row" }) });
    }
    for (const t of ["system", "light", "dark"] as Theme[])
      out.push({ id: `theme:${t}`, label: t === "system" ? "Theme: follow the system" : `Theme: ${t}`, disabled: layout.theme === t,
        run: () => setLayout({ ...layout, theme: t }) });
    out.push({ id: "reload", label: "Reload the page", run: () => location.reload() });
    return out;
  }, [list, welcome, inMatch, layout, view.primary, view.secondary]);

  return (
    <div class="shell">
      <header class="top">
        <div class="brand" aria-label="Oasis">
          <svg viewBox="0 0 24 24" width="20" height="20" aria-hidden="true"><circle cx="12" cy="12" r="10" class="brand-sun" /><path d="M3 15c3-2 6-2 9 0s6 2 9 0" class="brand-wave" /></svg>
          <span>Oasis</span>
        </div>
        <div class="top-info">
          {welcome ? <span>{welcome.server === "game" ? "Game" : "Standalone"}{welcome.game ? ` · Melange ${welcome.game.melange}` : ""}{welcome.limits?.readOnly ? " · read-only" : ""}</span> : null}
        </div>
        <button class="top-btn" data-action="palette" onClick={() => setPalette(true)} title="Commands (Ctrl+K)">
          Commands <kbd>Ctrl K</kbd>
        </button>
        <button class="top-btn" data-action="theme" onClick={() => setLayout({ ...layout, theme: NEXT_THEME[layout.theme] })}
                title="Switch the colour theme">{THEME_TEXT[layout.theme]}</button>
        <div class={`badge badge-${state}`} role="status" aria-live="polite">
          <span class="dot" />
          {STATE_TEXT[state]}
        </div>
      </header>
      {state !== "open" ? <ConnectionNotice state={state} client={client} /> : null}
      <div class="body">
        <nav class="tabs" aria-label="Panels">
          {all.map((p) => {
            const why = unmetReason(p, welcome, inMatch);
            const where = view.primary === p.id ? "main" : view.secondary === p.id ? "side" : undefined;
            return (
              <div class={`tab-row${where ? ` tab-${where}` : ""}`} key={p.id}>
                <button
                  class={`tab${where === "main" ? " active" : where === "side" ? " beside" : ""}`}
                  aria-current={where === "main" ? "page" : undefined}
                  disabled={!!why}
                  title={why}
                  data-panel={p.id}
                  onClick={() => setLayout(openPanel(layout, p.id))}
                >
                  {p.title}
                  {why ? <span class="tab-why">{why}</span> : null}
                </button>
                {!why && where !== "main" ? (
                  <button class="tab-side" data-beside={p.id} title={`Open ${p.title} beside`} aria-label={`Open ${p.title} beside`}
                          onClick={() => setLayout(openPanel(layout, p.id, true))}>⧉</button>
                ) : null}
              </div>
            );
          })}
        </nav>
        <main class="panel" aria-label={primary?.title}>
          {!primary ? <p class="muted pad">No panels.</p> : !secondary ? (
            <PanelFrame key={primary.id} def={primary} client={client} welcome={welcome} inMatch={inMatch} />
          ) : (
            <SplitPane id={`shell-${layout.direction}`} direction={layout.direction} initial={0.55}>
              <PanelFrame key={primary.id} def={primary} client={client} welcome={welcome} inMatch={inMatch} titled
                          onClose={() => setLayout({ ...layout, primary: view.secondary, secondary: undefined })} />
              <PanelFrame key={secondary.id} def={secondary} client={client} welcome={welcome} inMatch={inMatch} titled
                          onClose={() => setLayout(closeSecondary(layout))}
                          onSwap={() => setLayout({ ...layout, primary: view.secondary, secondary: view.primary })} />
            </SplitPane>
          )}
        </main>
      </div>
      {palette ? <Palette commands={commands} onClose={() => setPalette(false)} /> : null}
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

interface FrameProps {
  def: Tab; client: Client; welcome?: Welcome; inMatch?: boolean;
  titled?: boolean; onClose?: () => void; onSwap?: () => void;
}

function PanelFrame({ def, client, welcome, inMatch, titled, onClose, onSwap }: FrameProps) {
  const why = unmetReason(def, welcome, inMatch);
  return (
    <section class="frame" data-frame={def.id} aria-label={def.title}>
      {titled ? (
        <div class="frame-head">
          <span class="frame-title">{def.title}</span>
          {onSwap ? <button class="link" onClick={onSwap} title="Make this the main panel">Swap</button> : null}
          {onClose ? <button class="link" data-close={def.id} onClick={onClose} aria-label={`Close ${def.title}`}>Close</button> : null}
        </div>
      ) : null}
      {why && def.needs.includes("game") && welcome && welcome.server !== "game" ? (
        <div class="unavailable" data-unavailable={def.id}><p><strong>Game not running.</strong> {def.title} {why}.</p></div>
      ) : why ? (
        <div class="unavailable" data-unavailable={def.id}><p>{def.title} {why}.</p></div>
      ) : def.ext ? (
        <ExtPanel info={def.ext} client={client} />
      ) : (
        <PanelHost def={def} client={client} />
      )}
    </section>
  );
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
  return error ? <p class="error pad">Could not load {def.title}: {error}</p> : <div ref={el} class="panel-host" data-panel-host={def.id} />;
}
