// The divergence diff viewer: open a desync bundle .zip (server list via the library, a file picker, or drag and
// drop) and see both sides' detail records with the differing fields highlighted. Works standalone: nothing here
// needs a server once a bundle is loaded.
import { useEffect, useRef, useState } from "preact/hooks";
import { JsonTree } from "../../sdk/ui";
import { deepDiff } from "./model";
import { loadBundle, type LoadedBundle } from "./loadBundle";

export interface OpenRequest { name: string; buffer: ArrayBuffer }

export interface DesyncProps {
  onClose?: () => void;
  openRequest?: OpenRequest;
}

export function Desync({ onClose, openRequest }: DesyncProps) {
  const [name, setName] = useState<string>();
  const [bundle, setBundle] = useState<LoadedBundle>();
  const [error, setError] = useState<string>();
  const [busy, setBusy] = useState(false);
  const [urls, setUrls] = useState<Record<string, string>>({});
  const fileInput = useRef<HTMLInputElement>(null);
  const [dragging, setDragging] = useState(false);
  const seen = useRef<OpenRequest>();

  const openBuffer = (n: string, buf: ArrayBuffer) => {
    setBusy(true);
    setError(undefined);
    loadBundle(buf).then(
      (b) => {
        setBundle(b);
        setName(n);
        setBusy(false);
      },
      (e) => {
        setError(e instanceof Error ? e.message : String(e));
        setBusy(false);
      },
    );
  };

  useEffect(() => {
    if (openRequest && openRequest !== seen.current) {
      seen.current = openRequest;
      openBuffer(openRequest.name, openRequest.buffer);
    }
  }, [openRequest]);

  const openFile = (file: File) => file.arrayBuffer().then((b) => openBuffer(file.name, b));

  const close = () => {
    for (const u of Object.values(urls)) URL.revokeObjectURL(u);
    setUrls({});
    setBundle(undefined);
    setName(undefined);
    onClose?.();
  };

  const download = async (fileName: string) => {
    if (urls[fileName] || !bundle) return urls[fileName];
    const bytes = await bundle.zip.bytes(fileName);
    const url = URL.createObjectURL(new Blob([bytes.slice()]));
    setUrls((u) => ({ ...u, [fileName]: url }));
    return url;
  };

  if (bundle) {
    const rows = bundle.detailLocal !== undefined && bundle.detailPeer !== undefined ? deepDiff(bundle.detailLocal, bundle.detailPeer) : undefined;
    return (
      <div class="ds-viewer">
        <div class="ds-toolbar row between pad-x">
          <div class="row">
            <button class="btn" onClick={close}>&larr; Replays</button>
            <strong class="mono small">{name}</strong>
          </div>
        </div>
        <div class="ds-body pad">
          {bundle.reportJson !== undefined ? (
            <section>
              <h2 class="ds-h">Report</h2>
              <JsonTree value={bundle.reportJson} open={2} />
            </section>
          ) : null}

          {rows ? (
            <section>
              <h2 class="ds-h">Differing fields ({rows.length})</h2>
              {rows.length === 0 ? (
                <p class="muted">The two detail records match field for field.</p>
              ) : (
                <table class="ds-table">
                  <thead><tr><th>Field</th><th>Local</th><th>Peer</th></tr></thead>
                  <tbody>
                    {rows.map((r) => (
                      <tr key={r.path}>
                        <td class="mono">{r.path}</td>
                        <td class="mono ds-before">{JSON.stringify(r.before)}</td>
                        <td class="mono ds-after">{JSON.stringify(r.after)}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              )}
            </section>
          ) : (bundle.detailLocal !== undefined || bundle.detailPeer !== undefined) ? (
            <section>
              <h2 class="ds-h">Detail</h2>
              <div class="row" style={{ alignItems: "flex-start" }}>
                {bundle.detailLocal !== undefined ? <div><h3 class="ds-h3">Local</h3><JsonTree value={bundle.detailLocal} /></div> : null}
                {bundle.detailPeer !== undefined ? <div><h3 class="ds-h3">Peer</h3><JsonTree value={bundle.detailPeer} /></div> : null}
              </div>
            </section>
          ) : null}

          {bundle.diffText ? (
            <section>
              <h2 class="ds-h">diff.txt</h2>
              <pre class="ds-pre">{bundle.diffText}</pre>
            </section>
          ) : null}

          <section>
            <h2 class="ds-h">Files in this bundle</h2>
            <ul class="ds-files">
              {bundle.files.map((f) => (
                <li key={f}>
                  <button class="link" onClick={async () => { const url = await download(f); if (url) window.open(url, "_blank"); }}>{f}</button>
                </li>
              ))}
            </ul>
          </section>
        </div>
      </div>
    );
  }

  return (
    <div
      class={`ds-browse${dragging ? " dragging" : ""}`}
      onDragOver={(e) => { e.preventDefault(); setDragging(true); }}
      onDragLeave={() => setDragging(false)}
      onDrop={(e) => { e.preventDefault(); setDragging(false); const f = e.dataTransfer?.files?.[0]; if (f) openFile(f); }}
    >
      <h1>Divergence diff</h1>
      <p class="muted">Open a desync bundle (<code>.zip</code>) from a flagged replay, or drop one here. Nothing leaves this computer.</p>
      <div class="row">
        <button class="btn" disabled={busy} onClick={() => fileInput.current?.click()}>Open a bundle…</button>
        <input ref={fileInput} type="file" accept=".zip" class="rp-file-input"
               onChange={(e) => { const f = (e.currentTarget as HTMLInputElement).files?.[0]; if (f) openFile(f); (e.currentTarget as HTMLInputElement).value = ""; }} />
      </div>
      {busy ? <p class="muted">Opening…</p> : null}
      {error ? <p class="error">{error}</p> : null}
      {dragging ? <div class="rp-drop-overlay">Drop the bundle .zip to open it</div> : null}
    </div>
  );
}
