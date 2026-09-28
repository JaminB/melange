import { render } from "preact";
import { useEffect, useMemo, useReducer, useRef, useState } from "preact/hooks";
import type { Client } from "../../sdk/client";
import { errorText, useConnection } from "../../sdk/hooks";
import { JsonTree, SplitPane, VirtualTable, type Column } from "../../sdk/ui";
import { Counts, EventStore, covered, namesOf, prefixes, toggle, validPattern, type BusEvent, type BusName, type CountRow } from "./model";

export function mount(el: HTMLElement, c: Client): () => void {
  render(<Events client={c} />, el);
  return () => render(null, el);
}

const KEY = "oasis.events.names";

function loadPatterns(): string[] {
  try {
    const v = JSON.parse(localStorage.getItem(KEY) ?? "[]");
    return Array.isArray(v) ? v.filter((x): x is string => typeof x === "string" && validPattern(x)).slice(0, 200) : [];
  } catch {
    return [];
  }
}

function savePatterns(p: string[]) {
  try {
    localStorage.setItem(KEY, JSON.stringify(p));
  } catch {
    /* storage unavailable */
  }
}

function summary(d: unknown): string {
  if (d === undefined) return "";
  const s = JSON.stringify(d);
  return s.length > 160 ? `${s.slice(0, 160)}…` : s;
}

function Events({ client }: { client: Client }) {
  const conn = useConnection(client);
  const [names, setNames] = useState<BusName[]>([]);
  const [namesError, setNamesError] = useState<string>();
  const [patterns, setPatternsRaw] = useState<string[]>(loadPatterns);
  const [search, setSearch] = useState("");
  const [custom, setCustom] = useState("");
  const [decode, setDecode] = useState(true);
  const [paused, setPaused] = useState(false);
  const [tab, setTab] = useState<"events" | "counts">("events");
  const [selected, setSelected] = useState<number>();
  const [dropped, setDropped] = useState(0);
  const [subError, setSubError] = useState<string>();
  const store = useMemo(() => new EventStore(), []);
  const counts = useMemo(() => new Counts(), []);
  const [, bump] = useReducer((n: number) => n + 1, 0);
  const pending = useRef<unknown[]>([]);
  const frame = useRef(0);
  const pausedRef = useRef(paused);
  pausedRef.current = paused;

  const setPatterns = (p: string[]) => {
    setPatternsRaw(p);
    savePatterns(p);
  };

  const loadNames = () => {
    if (!conn.open || !client.has("bus.names")) return;
    client.call<unknown>("bus.names").then((v) => { setNames(namesOf(v)); setNamesError(undefined); }, (e) => setNamesError(errorText(e)));
  };
  useEffect(loadNames, [conn.open]);

  const hasBus = conn.open && client.has("bus");
  const key = patterns.join("\n");
  useEffect(() => {
    setSubError(undefined);
    if (!hasBus || !patterns.length) return;
    const flush = () => {
      frame.current = 0;
      const items = pending.current;
      pending.current = [];
      if (store.add(items, performance.now())) bump(0);
    };
    const off = client.subscribe<unknown>("bus", { names: patterns, decode }, (d) => {
      if (pausedRef.current) return;
      pending.current.push(d);
      if (!frame.current) frame.current = requestAnimationFrame(flush);
    }, (n) => setDropped((x) => x + n));
    return () => {
      off();
      if (frame.current) cancelAnimationFrame(frame.current);
      frame.current = 0;
      pending.current = [];
    };
  }, [hasBus, key, decode]);

  useEffect(() => {
    if (!conn.open || !client.has("bus.counts")) return;
    return client.subscribe<unknown>("bus.counts", undefined, (d) => {
      counts.apply(d, performance.now());
      bump(0);
    });
  }, [conn.open]);

  const q = search.trim().toLowerCase();
  const shown = q ? names.filter((n) => n.name.toLowerCase().includes(q)) : names;
  const groups = useMemo(() => prefixes(names), [names]);
  const rows = store.rows;
  const selIndex = selected === undefined ? -1 : rows.findIndex((r) => r.n === selected);
  const selRow = selIndex >= 0 ? rows[selIndex] : undefined;

  const columns: Column<BusEvent>[] = [
    { key: "at", title: "Received", width: "5.5rem", render: (r) => <span class="mono">{(r.at / 1000).toFixed(3)}</span> },
    { key: "frame", title: "Frame", width: "5rem", render: (r) => <span class="mono">{r.frame}</span> },
    { key: "name", title: "Message", width: "minmax(8rem, 16rem)", render: (r) => r.name },
    { key: "path", title: "Target", width: "minmax(6rem, 12rem)", render: (r) => <span title={`${r.path} ${r.handle}`}>{r.path || r.handle}</span> },
    { key: "d", title: "Payload", width: "minmax(0, 1fr)", render: (r) => <span class="mono muted">{summary(r.d)}</span> },
  ];
  const countColumns: Column<CountRow>[] = [
    { key: "name", title: "Message", width: "minmax(0, 1fr)", render: (r) => r.name },
    { key: "rate", title: "Per second", width: "6rem", render: (r) => <span class="mono">{r.rate ? r.rate.toFixed(1) : ""}</span> },
    { key: "total", title: "Since opened", width: "7rem", render: (r) => <span class="mono">{r.total.toLocaleString()}</span> },
    { key: "watch", title: "", width: "5.5rem", render: (r) => (
      <button class="link" onClick={(e) => { e.stopPropagation(); setPatterns(toggle(patterns, r.name, !patterns.includes(r.name))); }}>
        {patterns.includes(r.name) ? "Unwatch" : "Watch"}
      </button>
    ) },
  ];

  const addCustom = () => {
    const p = custom.trim();
    if (!validPattern(p)) {
      setSubError(`'${p}' is not a message name or a Prefix.* pattern`);
      return;
    }
    setPatterns(toggle(patterns, p, true));
    setCustom("");
  };

  const picker = (
    <div class="picker" data-picker>
      <div class="picker-head">
        <input class="fb-text" type="search" value={search} placeholder="Find a message" aria-label="Find a message"
               onInput={(e) => setSearch((e.currentTarget as HTMLInputElement).value)} />
        <button class="btn" onClick={loadNames} title="Reload the message names">Refresh</button>
      </div>
      {patterns.length ? (
        <div class="watched" data-watched>
          {patterns.map((p) => (
            <button key={p} class="fb-chip on" title="Stop watching" onClick={() => setPatterns(toggle(patterns, p, false))}>{p} ×</button>
          ))}
          <button class="link" onClick={() => setPatterns([])}>Clear all</button>
        </div>
      ) : <p class="muted small pad-x">Pick messages to watch. Nothing is streamed until you do.</p>}
      <form class="picker-custom" onSubmit={(e) => { e.preventDefault(); addCustom(); }}>
        <input class="fb-text" value={custom} placeholder="Name or Prefix.*" aria-label="Watch a name or pattern"
               onInput={(e) => setCustom((e.currentTarget as HTMLInputElement).value)} />
        <button class="btn" type="submit" disabled={!custom.trim()}>Watch</button>
      </form>
      {namesError ? <p class="error small pad-x">{namesError}</p> : null}
      {!client.has("bus.names") && conn.open ? <p class="muted small pad-x">This server does not list message names.</p> : null}
      <ul class="picker-list">
        {!q ? groups.map((g) => (
          <li key={g}>
            <label><input type="checkbox" checked={patterns.includes(g)} onChange={(e) => setPatterns(toggle(patterns, g, (e.currentTarget as HTMLInputElement).checked))} />
              <span>All of {g.slice(0, -2)}</span></label>
          </li>
        )) : null}
        {shown.map((n) => {
          const exact = patterns.includes(n.name);
          const viaGroup = !exact && covered(n.name, patterns);
          return (
            <li key={n.name} data-name={n.name}>
              <label title={viaGroup ? "Watched through a pattern" : undefined}>
                <input type="checkbox" checked={exact || viaGroup} disabled={viaGroup}
                       onChange={(e) => setPatterns(toggle(patterns, n.name, (e.currentTarget as HTMLInputElement).checked))} />
                <span>{n.name}</span>
                <span class="muted small count">{n.posts ? n.posts.toLocaleString() : ""}</span>
              </label>
            </li>
          );
        })}
      </ul>
    </div>
  );

  const eventsView = (
    <div class="events-main">
      <div class="fb">
        <div class="seg" role="tablist">
          <button role="tab" aria-selected={tab === "events"} class={`seg-btn${tab === "events" ? " on" : ""}`} data-tab="events" onClick={() => setTab("events")}>Events</button>
          <button role="tab" aria-selected={tab === "counts"} class={`seg-btn${tab === "counts" ? " on" : ""}`} data-tab="counts" onClick={() => setTab("counts")}>Counts</button>
        </div>
        {tab === "events" ? (
          <div class="fb-actions">
            <label class="field"><input type="checkbox" checked={decode} onChange={(e) => setDecode((e.currentTarget as HTMLInputElement).checked)} /> Decode payloads</label>
            <button class={`btn${paused ? " on" : ""}`} aria-pressed={paused} data-action="pause" onClick={() => setPaused(!paused)}>{paused ? "Resume" : "Pause"}</button>
            <button class="btn" data-action="clear" onClick={() => { store.clear(); setSelected(undefined); setDropped(0); bump(0); }}>Clear</button>
          </div>
        ) : (
          <div class="fb-actions"><button class="btn" onClick={() => { counts.clear(); bump(0); }}>Reset</button></div>
        )}
      </div>
      <div class="status-line">
        {tab === "events" ? <span data-count>{rows.length.toLocaleString()} events</span> : <span>Counts from bus.counts, once a second</span>}
        {dropped ? <span class="warn"> · {dropped.toLocaleString()} not delivered by the server</span> : null}
        {subError ? <span class="error"> · {subError}</span> : null}
      </div>
      {tab === "events" ? (
        <div class="with-detail">
          <VirtualTable<BusEvent> rows={rows} columns={columns} follow rowKey={(r) => r.n}
            selected={selIndex >= 0 ? selIndex : undefined} onRowClick={(r) => setSelected(r.n === selected ? undefined : r.n)}
            empty={!conn.open ? "Not connected." : !client.has("bus") ? "This server has no bus stream." :
              patterns.length ? "Waiting for events…" : "Pick messages to watch on the left."} />
          {selRow ? (
            <div class="detail" data-detail>
              <div class="detail-head"><strong>{selRow.name}</strong> <span class="muted">frame {selRow.frame} · {selRow.cls} {selRow.path} {selRow.handle}</span>
                <button class="link" onClick={() => setSelected(undefined)}>Close</button></div>
              {selRow.hasD ? <JsonTree key={selRow.n} value={selRow.d} open={3} /> : <p class="muted">No decoded payload for this message.</p>}
            </div>
          ) : null}
        </div>
      ) : (
        <VirtualTable<CountRow> rows={counts.rows()} columns={countColumns} rowKey={(r) => r.name}
          empty={client.has("bus.counts") ? "Waiting for counts…" : "This server has no bus counts."} />
      )}
    </div>
  );

  return <div class="events" data-events><SplitPane id="events" direction="row" initial={0.28}>{picker}{eventsView}</SplitPane></div>;
}
