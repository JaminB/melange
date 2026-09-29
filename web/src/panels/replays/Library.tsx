// The replay library: wormsign.library (game or the standalone file-backed version), plus opening a local .wsr
// or a desync bundle .zip directly, which needs no server at all.
import { useEffect, useRef, useState } from "preact/hooks";
import type { Client } from "../../sdk/client";
import { RpcError } from "../../sdk/client";
import { VirtualTable, type Column } from "../../sdk/ui";
import { entriesOf, formatBytes, formatDuration, formatWhen, sortNewestFirst, withEntry, type ReplayEntry } from "./model";

export interface LibraryProps {
  client: Client;
  onOpenServerFile: (name: string) => void;
  onOpenLocalFile: (file: File) => void;
  onOpenBundleFile: (file: File) => void;
}

export function Library({ client, onOpenServerFile, onOpenLocalFile, onOpenBundleFile }: LibraryProps) {
  const [entries, setEntries] = useState<ReplayEntry[]>();
  const [error, setError] = useState<string>();
  const [selected, setSelected] = useState<string>();
  const [dragging, setDragging] = useState(false);
  const fileInput = useRef<HTMLInputElement>(null);
  const bundleInput = useRef<HTMLInputElement>(null);

  const canList = client.has("wormsign.library");
  const canPin = client.has("wormsign.pin");
  const canArm = client.has("wormsign.arm");

  const refresh = () => {
    if (!canList) return;
    client.call<unknown>("wormsign.library").then(
      (v) => setEntries(sortNewestFirst(entriesOf(v))),
      (e) => setError(e instanceof RpcError ? e.message : String(e)),
    );
  };
  useEffect(() => {
    if (client.state === "open") refresh();
    return client.onState((s) => { if (s === "open") refresh(); });
  }, []);
  useEffect(() => {
    if (!client.has("wormsign.library")) return; // the channel and the method share a name
    return client.subscribe<unknown>("wormsign.library", undefined, (v) => setEntries(sortNewestFirst(entriesOf(v))));
  }, []);

  const pin = (e: ReplayEntry) => {
    client.call<boolean>("wormsign.pin", { name: e.name, on: !e.pinned }).then(
      () => setEntries((list) => (list ? withEntry(list, { ...e, pinned: !e.pinned }) : list)),
      (err) => setError(err instanceof RpcError ? err.message : String(err)),
    );
  };

  const arm = (e: ReplayEntry) => {
    setError(undefined);
    client.call<unknown>("wormsign.arm", { name: e.name }).catch((err) => setError(err instanceof RpcError ? err.message : String(err)));
  };

  const columns: Column<ReplayEntry>[] = [
    { key: "name", title: "Name", width: "1.4fr", render: (e) => <span class="mono small">{e.name}</span> },
    { key: "when", title: "Recorded", width: "1fr", render: (e) => formatWhen(e.startUnix) || <span class="muted">unknown</span> },
    { key: "dur", title: "Duration", width: "5.5rem", render: (e) => formatDuration(e.ticks) },
    { key: "size", title: "Size", width: "5.5rem", render: (e) => formatBytes(e.bytes) },
    { key: "land", title: "Land", width: "1fr", render: (e) => e.land || <span class="muted">—</span> },
    {
      key: "flags", title: "", width: "9rem",
      render: (e) => (
        <span class="row" style={{ gap: "4px" }}>
          {e.online ? <span class="rp-badge">online</span> : null}
          {!e.complete ? <span class="rp-badge warn">incomplete</span> : null}
          {e.flagged ? <span class="rp-badge bad">desync</span> : null}
          {e.pinned ? <span class="rp-badge accent">pinned</span> : null}
        </span>
      ),
    },
  ];

  const sel = entries?.find((e) => e.name === selected);

  return (
    <div
      class={`rp-browse${dragging ? " dragging" : ""}`}
      onDragOver={(ev) => { ev.preventDefault(); setDragging(true); }}
      onDragLeave={() => setDragging(false)}
      onDrop={(ev) => {
        ev.preventDefault();
        setDragging(false);
        const file = ev.dataTransfer?.files?.[0];
        if (!file) return;
        if (file.name.endsWith(".zip")) onOpenBundleFile(file);
        else onOpenLocalFile(file);
      }}
    >
      <div class="rp-toolbar row between pad-x">
        <div class="row">
          <button class="btn" onClick={() => fileInput.current?.click()}>Open a .wsr file…</button>
          <button class="btn" onClick={() => bundleInput.current?.click()}>Open a desync bundle (.zip)…</button>
          <input ref={fileInput} type="file" accept=".wsr" class="rp-file-input"
                 onChange={(ev) => { const f = (ev.currentTarget as HTMLInputElement).files?.[0]; if (f) onOpenLocalFile(f); (ev.currentTarget as HTMLInputElement).value = ""; }} />
          <input ref={bundleInput} type="file" accept=".zip" class="rp-file-input"
                 onChange={(ev) => { const f = (ev.currentTarget as HTMLInputElement).files?.[0]; if (f) onOpenBundleFile(f); (ev.currentTarget as HTMLInputElement).value = ""; }} />
        </div>
        {error ? <p class="error small">{error}</p> : null}
      </div>
      {canList ? (
        <VirtualTable
          rows={entries ?? []}
          columns={columns}
          rowHeight={26}
          rowKey={(e) => e.name}
          selected={sel ? entries!.indexOf(sel) : undefined}
          onRowClick={(e) => setSelected(e.name === selected ? undefined : e.name)}
          empty={entries === undefined ? "Loading…" : "No replays yet."}
        />
      ) : (
        <p class="muted pad-x">The server has no recording library yet. Open a file directly instead.</p>
      )}
      {sel ? (
        <div class="rp-detail row between pad-x">
          <div class="row small muted">
            <span>{sel.exeBuild ? `build ${sel.exeBuild}` : null}</span>
            <span>{sel.melange ? `Melange ${sel.melange}` : null}</span>
            <span>{sel.contentHash ? `content ${sel.contentHash}` : "vanilla"}</span>
          </div>
          <div class="row">
            {canPin ? <button class="btn" onClick={() => pin(sel)}>{sel.pinned ? "Unpin" : "Pin"}</button> : null}
            <a class="btn" href={`/replays/${encodeURIComponent(sel.name)}`} download>Download</a>
            <button class="btn" onClick={() => onOpenServerFile(sel.name)}>Open timeline</button>
            {canArm ? <button class="btn primary" onClick={() => arm(sel)} title="Arms the replay player in-game">Arm</button> : null}
          </div>
        </div>
      ) : null}
      {dragging ? <div class="rp-drop-overlay">Drop a .wsr recording or a desync .zip bundle to open it</div> : null}
    </div>
  );
}
