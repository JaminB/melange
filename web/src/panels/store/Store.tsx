import { render } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import type { Client } from "../../sdk/client";
import { errorText, useConnection } from "../../sdk/hooks";
import {
  CATEGORIES, PRIVACY, actionLabel, compareVersions, confirmLines, detailsOf, eventOf, fetchLine, itemsOf, permissionsText,
  progress, shotUrl, sizeText, stateLine, statusOf, type Ask, type Details, type Item, type Status,
} from "./model";

export function mount(el: HTMLElement, c: Client): () => void {
  render(<Store client={c} />, el);
  return () => render(null, el);
}

type Filter = "all" | "installed" | "updates";

function Store({ client }: { client: Client }) {
  const conn = useConnection(client);
  const [status, setStatus] = useState<Status>();
  const [items, setItems] = useState<Item[]>();
  const [query, setQuery] = useState("");
  const [category, setCategory] = useState("");
  const [filter, setFilter] = useState<Filter>("all");
  const [sel, setSel] = useState<string>();
  const [details, setDetails] = useState<Details>();
  const [ask, setAsk] = useState<{ id: string; ask: Ask }>();
  const [enableAfter, setEnableAfter] = useState(true);
  const [deleteData, setDeleteData] = useState(false);
  const [error, setError] = useState<string>();
  const [shots, setShots] = useState(0);
  const params = useRef({ query, category, filter, sel });
  params.current = { query, category, filter, sel };
  const timer = useRef<ReturnType<typeof setTimeout>>();

  const readOnly = !!conn.welcome?.limits?.readOnly;
  const can = (m: string) => conn.open && client.has(m) && !readOnly;
  const available = client.has("store.status");

  const loadList = () => {
    const p = params.current;
    client.call<unknown>("store.list", { query: p.query, category: p.category || undefined, filter: p.filter })
      .then((v) => setItems(itemsOf(v)), (e) => setError(errorText(e)));
  };
  const loadDetails = (id: string | undefined) => {
    if (!id) { setDetails(undefined); return; }
    client.call<unknown>("store.details", { id }).then((v) => {
      if (params.current.sel === id) setDetails(detailsOf(v));
    }, (e) => setError(errorText(e)));
  };
  const reload = () => {
    client.call<unknown>("store.status").then((v) => setStatus(statusOf(v)), (e) => setError(errorText(e)));
    loadList();
    loadDetails(params.current.sel);
  };
  const soon = () => {
    if (timer.current) clearTimeout(timer.current);
    timer.current = setTimeout(reload, 120);
  };

  useEffect(() => {
    if (!conn.open || !available) return;
    client.call("store.refresh").catch((e) => setError(errorText(e)));
    reload();
    const off = client.has("store")
      ? client.subscribe<unknown>("store", undefined, (m) => {
        const e = eventOf(m);
        setShots(e.shots);
        setStatus((s) => (s ? { ...s, job: { ...e }, busy: e.busy, gate: e.gate, fetching: e.fetching, pending: e.pending } : s));
        soon();
      })
      : undefined;
    return () => {
      off?.();
      if (timer.current) clearTimeout(timer.current);
    };
  }, [conn.open]);
  useEffect(() => { if (conn.open && available) loadList(); }, [query, category, filter]);
  useEffect(() => { if (conn.open && available) loadDetails(sel); }, [sel]);

  const call = async (method: string, p: object) => {
    setError(undefined);
    try {
      await client.call(method, p);
    } catch (e) {
      setError(errorText(e));
    }
    soon();
  };

  const startAsk = (it: Item, a: Ask) => {
    setSel(it.id);
    setEnableAfter(true);
    setDeleteData(false);
    setAsk({ id: it.id, ask: a });
  };
  const confirm = () => {
    if (!ask) return;
    const d = details?.id === ask.id ? details : undefined;
    if (ask.ask.kind === "remove") call("store.remove", { id: ask.id, deleteData });
    else if (d?.installed?.managed && ask.ask.version === d.compatible && d.action === "update") call("store.update", { id: ask.id });
    else call("store.install", { id: ask.id, version: ask.ask.version || undefined, enable: enableAfter, replaceManual: !!d?.installed && !d.installed.managed });
    setAsk(undefined);
  };

  if (conn.open && !available)
    return <div class="pad"><p class="muted" data-unavailable-store>The Store needs the game ([Store] Enabled=1).</p></div>;

  const gate = status?.gate ?? "";
  const blocked = !can("store.install") || !!gate || !!status?.busy;
  const blockedWhy = readOnly ? "Oasis is read-only" : gate || (status?.busy ? "another install, update or remove is running" : "");
  const job = status ? progress(status.job) : undefined;

  const actionButton = (it: Item) => {
    const label = actionLabel(it);
    if (!label) return null;
    return (
      <span class="store-actions">
        <button class={`btn${it.action === "remove" ? " danger" : " primary"}`} data-store-action={it.id} disabled={blocked} title={blockedWhy}
                onClick={(e) => { e.stopPropagation(); startAsk(it, it.action === "remove" ? { kind: "remove" } : { kind: "install", version: it.compatible, older: false }); }}>
          {label}
        </button>
        {it.action === "update" && it.canRemove ? (
          <button class="btn danger" data-store-remove={it.id} disabled={blocked} title={blockedWhy}
                  onClick={(e) => { e.stopPropagation(); startAsk(it, { kind: "remove" }); }}>Remove</button>
        ) : null}
      </span>
    );
  };

  const d = details && details.id === sel ? details : undefined;
  const askDetails = ask && details?.id === ask.id ? details : undefined;

  return (
    <div class="store pad" data-store>
      <div class="row between">
        <h1>Store</h1>
        <button class="btn" data-store-refresh disabled={!conn.open || status?.fetching} onClick={() => call("store.refresh", {})}>Refresh</button>
      </div>
      <p class="hint" data-privacy>{PRIVACY}</p>
      {status?.customIndex ? <p class="hint warn" data-custom-index>Custom index: {status.indexUrl}</p> : null}
      {status ? <p class={`small ${status.error && !status.offline ? "error" : status.offline ? "warn" : "muted"}`} data-fetch>{fetchLine(status)}</p> : null}
      {readOnly ? <p class="hint warn" data-readonly>Oasis is read-only ([Oasis] ReadOnly=1): plugins cannot be installed or removed from here.</p> : null}
      {status?.rollback ? <p class="hint warn">This list is older than one seen before; updates are disabled.</p> : null}
      {gate ? <p class="hint warn" data-gate>Install, update and remove are paused: {gate}</p> : null}
      {status?.notices.map((n) => <p class="hint" key={n}>{n}</p>)}
      {error ? <p class="error" data-store-error>{error}</p> : null}
      {job?.active ? (
        <div class="row store-progress" data-progress={status!.job.phase}>
          <progress max={100} value={job.pct} />
          <span class="small">{job.text}</span>
          {status!.job.phase === "downloading" ? (
            <button class="btn" data-store-cancel disabled={!can("store.cancel")} onClick={() => call("store.cancel", {})}>Cancel</button>
          ) : null}
        </div>
      ) : status?.job.phase === "done" ? <p class="small tone-ok" data-job="done">{status.job.message}</p>
        : status?.job.phase === "error" ? <p class="small error" data-job="error">{status.job.id} {status.job.version}: {status.job.message}</p> : null}

      <div class="fb store-filters">
        <input class="fb-text" type="search" placeholder="Search name, author, description" value={query} data-store-search
               onInput={(e) => setQuery((e.currentTarget as HTMLInputElement).value)} />
        {(["all", "installed", "updates"] as Filter[]).map((f) => (
          <button key={f} class={`fb-chip${filter === f ? " on" : ""}`} data-filter={f} onClick={() => setFilter(f)}>
            {f === "all" ? "All" : f === "installed" ? "Installed" : "Updates"}
          </button>
        ))}
      </div>
      <div class="fb store-filters">
        {CATEGORIES.map((c) => (
          <button key={c} class={`fb-chip${category === c ? " on" : ""}`} data-category={c} onClick={() => setCategory(category === c ? "" : c)}>{c}</button>
        ))}
      </div>

      <div class="store-body">
        <ul class="store-list" data-plugins>
          {!items ? <li class="muted">{conn.open ? "Loading…" : "Not connected."}</li>
            : items.length === 0 ? <li class="muted" data-empty>{status?.haveIndex ? "No plugin matches." : "No list yet."}</li>
              : items.map((it) => (
                <li key={it.id} data-plugin={it.id} class={sel === it.id ? "sel" : undefined} onClick={() => setSel(it.id)}>
                  <div class="row between">
                    <div>
                      <strong>{it.name}</strong> <span class="muted small">{it.installed ? it.installed.version : it.latest}</span>
                      <span class="tag">{it.kind}</span>
                      {it.unsafe ? <span class="tag tag-warn" data-dd>Deep Desert</span> : null}
                    </div>
                    {actionButton(it)}
                  </div>
                  <div class="muted small">{it.authors.join(", ")} · {sizeText(it.size)}</div>
                  {stateLine(it) ? <div class={`small ${it.state === "incompatible" ? "error" : it.state === "update" ? "tone-ok" : "muted"}`} data-state={it.state}>{stateLine(it)}</div> : null}
                  {it.error ? <div class="small error" data-row-error>{it.error}</div> : null}
                </li>
              ))}
        </ul>
        <div class="store-details" data-details={d?.id ?? ""}>
          {!d ? <p class="muted">Select a plugin to see its details.</p> : (
            <>
              <h2>{d.name} <span class="muted small">{d.latest}</span></h2>
              <div class="muted small">{d.id} · {d.authors.join(", ")} · {d.licence}</div>
              <p class="store-desc">{d.description}</p>
              <dl class="facts">
                <dt>Categories</dt><dd>{d.categories.join(", ")}</dd>
                <dt>Permissions</dt><dd data-permissions>{permissionsText(d)}</dd>
                {d.homepage ? (<><dt>Homepage</dt><dd>
                  <button class="link" data-homepage disabled={!conn.open} onClick={() => call("store.openHomepage", { id: d.id })}>{d.homepage}</button>
                </dd></>) : null}
                {d.dependencies.length ? (<><dt>Needs</dt><dd>{d.dependencies.map((x) => `${x.id}${x.range ? ` ${x.range}` : ""}`).join(", ")}</dd></>) : null}
                {d.conflicts.length ? (<><dt>Conflicts</dt><dd>{d.conflicts.map((x) => x.id).join(", ")}</dd></>) : null}
              </dl>
              {d.content ? <p class="hint" data-content-note>Content: everyone in an online match needs the same version.</p> : null}
              {d.screenshots.length ? (
                <div class="store-shots">
                  {d.screenshots.map((s) => (
                    <figure key={s.n}>
                      {s.ready ? <img src={shotUrl(d.id, s.n, shots)} alt={s.caption} data-shot={s.n} /> : <div class="muted small">Loading screenshot…</div>}
                      {s.caption ? <figcaption class="muted small">{s.caption}</figcaption> : null}
                    </figure>
                  ))}
                </div>
              ) : null}
              <h3>Versions</h3>
              <ul class="store-versions" data-versions>
                {d.versions.map((v) => (
                  <li key={v.version} data-version={v.version}>
                    <div class="row">
                      <strong>{v.version}</strong>
                      <span class="muted small">{v.released} · {sizeText(v.size)} · Melange {v.melange || "any"}</span>
                      {v.yanked ? <span class="tag tag-warn">withdrawn</span> : !v.compatible ? <span class="tag">incompatible</span> : null}
                      {v.compatible && v.version !== d.installed?.version && v.version !== d.compatible ? (
                        <button class="btn" data-install-version={v.version} disabled={blocked} title={blockedWhy}
                                onClick={() => startAsk(d, { kind: "install", version: v.version, older: !!d.installed && compareVersions(v.version, d.installed.version) < 0 })}>
                          Install this version
                        </button>
                      ) : null}
                    </div>
                    {v.changelog ? <div class="small muted store-changelog">{v.changelog}</div> : null}
                  </li>
                ))}
              </ul>
            </>
          )}
        </div>
      </div>

      {ask ? (
        <div class="store-dialog-backdrop">
          <div class="store-dialog" role="dialog" aria-modal="true" data-confirm={ask.ask.kind}>
            <h2>{ask.ask.kind === "remove" ? `Remove ${askDetails?.name ?? ask.id}?` : `${askDetails?.installed?.managed ? "Update" : "Install"} ${askDetails?.name ?? ask.id} ${ask.ask.version}`}</h2>
            {askDetails ? confirmLines(askDetails, ask.ask).map((l) => <p key={l} class="small">{l}</p>) : <p class="muted">Loading…</p>}
            {ask.ask.kind === "install" && !askDetails?.installed ? (
              <label class="small"><input type="checkbox" checked={enableAfter} data-enable-after
                                          onChange={(e) => setEnableAfter((e.currentTarget as HTMLInputElement).checked)} /> Enable after install</label>
            ) : null}
            {ask.ask.kind === "remove" ? (
              <label class="small"><input type="checkbox" checked={deleteData} data-delete-data
                                          onChange={(e) => setDeleteData((e.currentTarget as HTMLInputElement).checked)} /> Also delete its settings and saved data</label>
            ) : null}
            <div class="row">
              <button class={`btn ${ask.ask.kind === "remove" ? "danger" : "primary"}`} data-confirm-ok disabled={!askDetails || blocked} onClick={confirm}>
                {ask.ask.kind === "remove" ? "Remove" : askDetails?.installed?.managed ? "Update" : "Install"}
              </button>
              <button class="btn" data-confirm-cancel onClick={() => setAsk(undefined)}>Cancel</button>
            </div>
          </div>
        </div>
      ) : null}
    </div>
  );
}
