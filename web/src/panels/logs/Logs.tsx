import { render } from "preact";
import { useEffect, useMemo, useReducer, useRef, useState } from "preact/hooks";
import type { Client } from "../../sdk/client";
import { errorText, useConnection } from "../../sdk/hooks";
import { FilterBar, JsonTree, VirtualTable, type Column } from "../../sdk/ui";
import {
  DEFAULT_CAP, LEVELS, LogStore, fromEvent, formatBytes, message, parseJsonl, record, sessionUrl, sessionsOf, shortTime,
  type LogFilter, type LogRow, type SessionInfo,
} from "./model";

export function mount(el: HTMLElement, c: Client): () => void {
  render(<Logs client={c} />, el);
  return () => render(null, el);
}

const LIVE = "live";

function indexOfN(view: LogRow[], n: number): number {
  let lo = 0, hi = view.length - 1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (view[mid].n === n) return mid;
    if (view[mid].n < n) lo = mid + 1;
    else hi = mid - 1;
  }
  return -1;
}

function Logs({ client }: { client: Client }) {
  const conn = useConnection(client);
  const live = useMemo(() => new LogStore(DEFAULT_CAP), []);
  const [past, setPast] = useState<LogStore>();
  const [source, setSource] = useState(LIVE);
  const [sessions, setSessions] = useState<SessionInfo[]>([]);
  const [filter, setFilter] = useState<LogFilter>({ minLevel: 0, cats: [], text: "" });
  const [paused, setPaused] = useState(false);
  const [follow, setFollow] = useState(true);
  const [selected, setSelected] = useState<number>();
  const [dropped, setDropped] = useState(0);
  const [note, setNote] = useState<string>();
  const [, bump] = useReducer((n: number) => n + 1, 0);
  const pending = useRef<ReturnType<typeof fromEvent>[]>([]);
  const frame = useRef(0);
  const pausedRef = useRef(false);
  pausedRef.current = paused;

  const hasLog = conn.open && client.has("log");
  useEffect(() => {
    if (!hasLog) return;
    const flush = () => {
      frame.current = 0;
      if (pausedRef.current) return;
      const items = pending.current.filter((x): x is NonNullable<typeof x> => !!x);
      pending.current = [];
      if (items.length && live.addLive(items)) bump(0);
    };
    const off = client.subscribe<unknown>("log", {}, (d) => {
      pending.current.push(fromEvent(d));
      if (pending.current.length > DEFAULT_CAP) pending.current.splice(0, pending.current.length - DEFAULT_CAP);
      if (!frame.current) frame.current = requestAnimationFrame(flush);
    }, (n) => setDropped((x) => x + n));
    return () => {
      off();
      if (frame.current) cancelAnimationFrame(frame.current);
      frame.current = 0;
    };
  }, [hasLog]);

  useEffect(() => {
    if (!paused && pending.current.length) {
      live.addLive(pending.current.filter((x): x is NonNullable<typeof x> => !!x));
      pending.current = [];
      bump(0);
    }
  }, [paused]);

  const loadSessions = () => {
    if (!conn.open || !client.has("log.sessions")) return;
    client.call<unknown>("log.sessions").then((v) => setSessions(sessionsOf(v)), (e) => setNote(errorText(e)));
  };
  useEffect(loadSessions, [conn.open]);

  const store = source === LIVE ? live : past;
  useEffect(() => {
    live.setFilter(filter);
    past?.setFilter(filter);
    bump(0);
  }, [filter, past]);

  const openSession = async (value: string) => {
    setSource(value);
    setSelected(undefined);
    setNote(undefined);
    if (value === LIVE) return;
    const [id, file] = value.split("\n");
    setPast(undefined);
    try {
      const r = await fetch(sessionUrl(id, file), { credentials: "same-origin" });
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      const { rows, bad, skipped } = parseJsonl(await r.text());
      const s = new LogStore(DEFAULT_CAP);
      s.setFilter(filter);
      s.add(rows);
      setPast(s);
      const parts = [];
      if (skipped) parts.push(`showing the last ${DEFAULT_CAP.toLocaleString()} records (${skipped.toLocaleString()} older ones not shown)`);
      if (bad) parts.push(`${bad} unreadable line${bad === 1 ? "" : "s"} skipped`);
      setNote(parts.join("; ") || undefined);
    } catch (e) {
      setNote(`Could not load ${id}/${file}: ${errorText(e)}`);
    }
  };

  const cats = useMemo(() => [...(store?.cats.keys() ?? [])].sort(), [store, store?.cats.size]);
  const chips = cats.slice(0, 24).map((c) => ({ id: c, label: c, on: filter.cats.includes(c) }));
  const view = store?.view ?? [];
  const selIndex = selected === undefined ? -1 : indexOfN(view, selected);
  const selRow = selIndex >= 0 ? view[selIndex] : undefined;

  const columns: Column<LogRow>[] = [
    { key: "ts", title: "Time", width: "6.5rem", render: (r) => <span class="mono">{shortTime(r.ts)}</span> },
    { key: "lvl", title: "Level", width: "3.8rem", render: (r) => <span class={`lvl lvl-${LEVELS[r.lvl]}`}>{LEVELS[r.lvl]}</span> },
    { key: "cat", title: "Category", width: "7rem", render: (r) => r.cat },
    { key: "msg", title: "Message", width: "minmax(0, 1fr)", render: (r) => <span title={message(r)}>{message(r)}</span> },
  ];

  const unavailable = conn.open && source === LIVE && !client.has("log");
  const table = (
    <VirtualTable<LogRow>
      rows={view}
      columns={columns}
      follow={follow && source === LIVE}
      rowKey={(r) => r.n}
      selected={selIndex >= 0 ? selIndex : undefined}
      onRowClick={(r) => setSelected(r.n === selected ? undefined : r.n)}
      empty={unavailable ? "This server has no log stream." : !conn.open && source === LIVE ? "Not connected." :
        store && store.all.length ? "No record matches the filter." : source === LIVE ? "Waiting for log records…" : past ? "The file is empty." : "Loading…"}
    />
  );

  return (
    <div class="logs" data-logs>
      <FilterBar text={filter.text} onText={(text) => setFilter({ ...filter, text })} placeholder="Filter messages"
                 chips={chips} onChip={(id, on) => setFilter({ ...filter, cats: on ? [...filter.cats, id] : filter.cats.filter((x) => x !== id) })}>
        <label class="field">
          <span>Level</span>
          <select value={String(filter.minLevel)} data-control="level"
                  onChange={(e) => setFilter({ ...filter, minLevel: Number((e.currentTarget as HTMLSelectElement).value) })}>
            {LEVELS.map((l, i) => <option key={l} value={String(i)}>{i ? `${l} and up` : "all"}</option>)}
          </select>
        </label>
        <select value={source} aria-label="Source" data-control="source" onChange={(e) => openSession((e.currentTarget as HTMLSelectElement).value)}
                onFocus={loadSessions}>
          <option value={LIVE}>Live</option>
          {sessions.flatMap((s) => s.files.map((f) => (
            <option key={`${s.id}/${f}`} value={`${s.id}\n${f}`}>{s.id}{s.files.length > 1 ? ` / ${f}` : ""} ({formatBytes(s.bytes)})</option>
          )))}
        </select>
        {source === LIVE ? (
          <>
            <button class={`btn${paused ? " on" : ""}`} data-action="pause" aria-pressed={paused} onClick={() => setPaused(!paused)}>{paused ? "Resume" : "Pause"}</button>
            <button class={`btn${follow ? " on" : ""}`} data-action="follow" aria-pressed={follow} onClick={() => setFollow(!follow)}>Follow</button>
            <button class="btn" data-action="clear" onClick={() => { live.clear(); setSelected(undefined); setDropped(0); bump(0); }}>Clear</button>
          </>
        ) : null}
      </FilterBar>
      <div class="status-line" data-status>
        <span data-count>{view.length.toLocaleString()} shown of {(store?.all.length ?? 0).toLocaleString()}</span>
        {source === LIVE && paused ? <span class="warn"> · paused ({pending.current.length.toLocaleString()} waiting)</span> : null}
        {source === LIVE && dropped ? <span class="warn" data-dropped> · {dropped.toLocaleString()} not delivered by the server</span> : null}
        {source === LIVE && live.trimmed ? <span class="muted"> · {live.trimmed.toLocaleString()} oldest removed (cap {DEFAULT_CAP.toLocaleString()})</span> : null}
        {note ? <span class="muted"> · {note}</span> : null}
      </div>
      <div class="with-detail">
        {table}
        {selRow ? (
          <div class="detail" data-detail>
            <div class="detail-head"><strong>#{selRow.seq}</strong> <span class="muted">{selRow.cat} · {LEVELS[selRow.lvl]} · {selRow.ts}</span>
              <button class="link" onClick={() => setSelected(undefined)}>Close</button></div>
            <JsonTree key={selRow.n} value={record(selRow) ?? selRow.j} open={2} />
          </div>
        ) : null}
      </div>
    </div>
  );
}
