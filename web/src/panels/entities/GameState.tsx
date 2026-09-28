// The game-state browser: worms and teams, the entity list, a 2-D map, the data variables and an inspector.
import { render } from "preact";
import { useEffect, useState } from "preact/hooks";
import type { Client, ClientState } from "../../sdk/client";
import { RpcError } from "../../sdk/client";
import { SplitPane } from "../../sdk/ui";
import { Inspector, type Target } from "./Inspector";
import { EntityList, MapView, VarsView } from "./Views";
import {
  fmtClock, fmtNum, fmtVec, physicsName, speed, teamColour, weaponName, windDegrees,
  type Entity, type Snapshot, type Worm,
} from "./model";

export function mount(el: HTMLElement, c: Client): () => void {
  render(<GameState client={c} />, el);
  return () => render(null, el);
}

type Tab = "worms" | "entities" | "map" | "vars";
const TABS: { id: Tab; title: string }[] = [
  { id: "worms", title: "Worms & teams" }, { id: "entities", title: "Entities" },
  { id: "map", title: "Map" }, { id: "vars", title: "Variables" },
];

function storedTab(): Tab {
  try {
    const t = localStorage.getItem("oasis.gamestate.tab");
    if (TABS.some((x) => x.id === t)) return t as Tab;
  } catch { /* storage unavailable */ }
  return "worms";
}

// Below 720 px the inspector goes under the views instead of beside them.
function useNarrow(): boolean {
  const q = typeof matchMedia === "function" ? matchMedia("(max-width: 720px)") : undefined;
  const [narrow, setNarrow] = useState(!!q?.matches);
  useEffect(() => {
    if (!q) return;
    const on = () => setNarrow(q.matches);
    q.addEventListener("change", on);
    return () => q.removeEventListener("change", on);
  }, []);
  return narrow;
}

function GameState({ client }: { client: Client }) {
  const [conn, setConn] = useState<ClientState>(client.state);
  const [snap, setSnap] = useState<Snapshot>();
  const [ents, setEnts] = useState<Entity[]>([]);
  const [tab, setTab] = useState<Tab>(storedTab);
  const [target, setTarget] = useState<Target>();
  const [note, setNote] = useState<string>();
  const narrow = useNarrow();

  useEffect(() => {
    setConn(client.state);
    return client.onState(setConn);
  }, []);
  useEffect(() => {
    if (conn !== "open" || !client.has("state")) return;
    return client.subscribe<Snapshot>("state", { hz: 5 }, setSnap);
  }, [conn]);
  const wantEntities = tab === "entities" || tab === "map";
  useEffect(() => {
    if (conn !== "open" || !wantEntities || !client.has("entities")) return;
    return client.subscribe<Entity[]>("entities", { hz: 2 }, setEnts);
  }, [conn, wantEntities]);

  const choose = (t: Tab) => {
    setTab(t);
    try { localStorage.setItem("oasis.gamestate.tab", t); } catch { /* storage unavailable */ }
  };

  // Worm rows carry a slot, not a handle: find the worm's logic entity by name.
  const inspectWorm = async (w: Worm) => {
    setNote(undefined);
    try {
      const list = await client.call<Entity[]>("entities.list", { kinds: ["Worm"] });
      const e = list.find((x) => x.label === w.name);
      if (e) setTarget({ handle: e.handle });
      else setNote(`No live entity for ${w.name}.`);
    } catch (err) {
      setNote(err instanceof RpcError ? err.message : String(err));
    }
  };

  if (conn === "open" && !client.has("state"))
    return <div class="gs"><p class="gs-notice">This server has no game-state provider.</p></div>;

  let view;
  if (tab === "worms") view = <WormsView snap={snap} onWorm={inspectWorm} />;
  else if (tab === "entities") view = <EntityList entities={ents} inMatch={!!snap?.match.inMatch} target={target} onPick={(e) => setTarget({ handle: e.handle })} />;
  else if (tab === "map") view = <MapView snap={snap} entities={ents} onEntity={(e) => setTarget({ handle: e.handle })} onWorm={inspectWorm} />;
  else view = <VarsView client={client} onContainer={(addr) => setTarget({ addr })} />;

  return (
    <div class="gs">
      <MatchBar snap={snap} />
      <SplitPane key={narrow ? "v" : "h"} id={narrow ? "gamestate-v" : "gamestate"} direction={narrow ? "column" : "row"}
                 initial={0.6} min={0.2}>
        <div class="gs-main">
          <nav class="gs-tabs" role="tablist">
            {TABS.map((t) => (
              <button key={t.id} role="tab" aria-selected={tab === t.id} class={`gs-tab${tab === t.id ? " on" : ""}`}
                      data-gs-tab={t.id} onClick={() => choose(t.id)}>{t.title}</button>
            ))}
          </nav>
          {note ? <p class="gs-note" role="status">{note}</p> : null}
          <div class="gs-view">{view}</div>
        </div>
        <Inspector client={client} target={target} onTarget={setTarget} />
      </SplitPane>
    </div>
  );
}

function MatchBar({ snap }: { snap?: Snapshot }) {
  if (!snap) return <div class="gs-bar"><span class="muted">Waiting for the game…</span></div>;
  if (!snap.available)
    return <div class="gs-bar"><span class="error">The game-state readers are off (unrecognised game build, or [GameState] Enabled=0).</span></div>;
  const m = snap.match;
  if (!m.inMatch) return <div class="gs-bar" data-gs="menu"><span class="gs-pill">Not in a match</span><span class="muted small">frame {snap.frame}</span></div>;
  return (
    <div class="gs-bar" data-gs="match">
      <span class="gs-pill on">In a match</span>
      {m.online ? <span class="gs-pill">Online</span> : null}
      {m.suddenDeath ? <span class="gs-pill warn">Sudden death</span> : null}
      <Fact k="Theme" v={m.theme || "—"} />
      <Fact k="Turn" v={`${m.turnsStarted} · ${fmtClock(m.turnMsLeft)} / ${fmtClock(m.turnMs)}`} />
      <Fact k="Round" v={`${fmtClock(m.roundMsLeft)} / ${fmtClock(m.roundMs)}`} />
      <span class="gs-fact"><span class="k">Wind</span>
        <span class="gs-wind" style={{ transform: `rotate(${windDegrees(m.windDir)}deg)` }} aria-hidden="true">➜</span>
        <span class="v">{typeof m.windSpeed === "number" ? m.windSpeed.toExponential(2) : "—"}</span></span>
      <Fact k="Water" v={fmtNum(m.waterLevel, 1)} />
    </div>
  );
}

function Fact({ k, v }: { k: string; v: string }) {
  return <span class="gs-fact"><span class="k">{k}</span><span class="v">{v}</span></span>;
}

function WormsView({ snap, onWorm }: { snap?: Snapshot; onWorm: (w: Worm) => void }) {
  if (!snap || !snap.match.inMatch || (!snap.worms.length && !snap.teams.length))
    return <p class="muted gs-empty">Worms and teams appear here during a match.</p>;
  const m = snap.match;
  const teamName = (i: number) => snap.teams.find((t) => t.slot === i)?.name ?? `Team ${i}`;
  return (
    <div class="gs-scroll">
      <table class="gs-table" data-gs="teams">
        <thead><tr><th>Team</th><th>Player</th><th>Alliance</th><th>Rounds</th><th>Score</th></tr></thead>
        <tbody>
          {snap.teams.map((t) => (
            <tr key={t.slot} class={t.slot === m.currentTeam ? "cur" : ""}>
              <td><span class="gs-swatch" style={{ background: teamColour(t.slot) }} />{t.name || `Team ${t.slot}`}</td>
              <td>{t.ai ? "CPU" : "Human"}{t.local ? " · local" : ""}{t.active ? "" : " · out"}</td>
              <td>{t.alliance}</td><td>{t.roundsWon}</td><td>{t.score}</td>
            </tr>
          ))}
        </tbody>
      </table>
      <table class="gs-table" data-gs="worms">
        <thead><tr><th>#</th><th>Worm</th><th>Team</th><th>Health</th><th>State</th><th>Weapon</th><th>Position</th><th>Speed</th></tr></thead>
        <tbody>
          {snap.worms.map((w) => (
            <tr key={w.slot} class={`${w.slot === m.activeWorm ? "cur" : ""}${w.alive ? "" : " dead"}`} data-slot={w.slot}
                onClick={() => onWorm(w)} title="Inspect this worm">
              <td>{w.slot}</td>
              <td>{w.name || "—"}</td>
              <td><span class="gs-swatch" style={{ background: teamColour(w.team) }} />{teamName(w.team)}</td>
              <td><span class="gs-health"><span style={{ width: `${Math.min(100, w.health)}%` }} /></span>{w.health}</td>
              <td>{w.alive ? physicsName(w.physicsState) : `dead (${physicsName(w.physicsState)})`}</td>
              <td>{weaponName(w.weapon)}</td>
              <td class="mono">{fmtVec(w.pos)}</td>
              <td class="mono">{fmtNum(speed(w.vel), 2)}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}
