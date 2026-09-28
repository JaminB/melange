import { render } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import type { Client } from "../../sdk/client";
import { RpcError } from "../../sdk/client";
import { CallList } from "./CallList";
import type { LoadedCapture } from "./loadCapture";
import { loadCapture } from "./loadCapture";
import { Programs } from "./Programs";
import { StateDiffView } from "./StateDiffView";
import { Summary } from "./Summary";
import { Textures } from "./Textures";

export function mount(el: HTMLElement, c: Client): () => void {
  render(<CapturePanel client={c} />, el);
  return () => render(null, el);
}

interface ServerEntry { name: string; bytes: number; time: number }
interface ArmState { state: string; path: string; error?: string }
type Tab = "summary" | "calls" | "state" | "textures" | "programs";

function CapturePanel({ client }: { client: Client }) {
  const [entries, setEntries] = useState<ServerEntry[]>();
  const [listError, setListError] = useState<string>();
  const [arm, setArm] = useState<ArmState>();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string>();
  const [capture, setCapture] = useState<LoadedCapture>();
  const [tab, setTab] = useState<Tab>("summary");
  const [dragging, setDragging] = useState(false);
  const fileInput = useRef<HTMLInputElement>(null);
  const captureRef = useRef<LoadedCapture>();
  captureRef.current = capture;

  const canList = client.has("capture.list");
  const canRequest = client.has("capture.request");

  const refreshList = () => {
    if (!canList) return;
    client.call<ServerEntry[]>("capture.list").then(
      (list) => { setEntries([...list].sort((a, b) => b.time - a.time)); setListError(undefined); },
      (e) => setListError(e instanceof RpcError ? e.message : String(e)),
    );
  };

  useEffect(() => {
    if (client.state === "open") refreshList();
    return client.onState((s) => { if (s === "open") refreshList(); });
  }, []);

  useEffect(() => {
    if (!client.has("capture")) return;
    return client.subscribe<ArmState>("capture", undefined, (msg) => {
      setArm(msg);
      if (msg.state === "done") refreshList();
    });
  }, []);

  useEffect(() => () => captureRef.current?.close(), []);

  const openBuffer = async (buf: ArrayBuffer) => {
    setBusy(true);
    setError(undefined);
    try {
      const next = await loadCapture(buf);
      captureRef.current?.close();
      setCapture(next);
      setTab("summary");
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setBusy(false);
    }
  };

  const openFromServer = (name: string) => {
    setBusy(true);
    setError(undefined);
    fetch(`/captures/${encodeURIComponent(name)}`)
      .then((r) => { if (!r.ok) throw new Error(`${r.status} ${r.statusText}`); return r.arrayBuffer(); })
      .then(openBuffer)
      .catch((e) => { setError(String(e?.message ?? e)); setBusy(false); });
  };

  const openFromFile = (file: File) => { file.arrayBuffer().then(openBuffer); };

  const requestCapture = () => {
    client.call<{ armed: boolean }>("capture.request", {}).catch((e) => setListError(e instanceof RpcError ? e.message : String(e)));
  };

  const close = () => {
    captureRef.current?.close();
    setCapture(undefined);
  };

  if (capture) {
    const tabs: [Tab, string][] = [
      ["summary", "Summary"], ["calls", "Calls"], ["state", "State diff"], ["textures", "Textures"], ["programs", "Programs"],
    ];
    return (
      <div class="cap-viewer">
        <div class="cap-toolbar row">
          <button class="btn" onClick={close}>&larr; Captures</button>
          <nav class="cap-tabs">
            {tabs.map(([id, title]) => (
              <button key={id} class={`cap-tab${tab === id ? " active" : ""}`} onClick={() => setTab(id)}>{title}</button>
            ))}
          </nav>
        </div>
        <div class="cap-tabpage">
          {tab === "summary" && <Summary capture={capture} />}
          {tab === "calls" && <CallList capture={capture} />}
          {tab === "state" && <StateDiffView capture={capture} />}
          {tab === "textures" && <Textures capture={capture} />}
          {tab === "programs" && <Programs capture={capture} />}
        </div>
      </div>
    );
  }

  return (
    <div
      class={`cap-browse${dragging ? " dragging" : ""}`}
      onDragOver={(e) => { e.preventDefault(); setDragging(true); }}
      onDragLeave={() => setDragging(false)}
      onDrop={(e) => {
        e.preventDefault();
        setDragging(false);
        const file = e.dataTransfer?.files?.[0];
        if (file) openFromFile(file);
      }}
    >
      <h1>Capture viewer</h1>
      <p class="muted">Open a Mirage GL capture (<code>.mcap</code>) from the game, or drop one here. It never leaves this computer.</p>
      <div class="row">
        <button class="btn" disabled={!canRequest || busy} title={canRequest ? undefined : "needs the game running"} onClick={requestCapture}>
          Capture frame
        </button>
        <button class="btn" disabled={busy} onClick={() => fileInput.current?.click()}>Open a .mcap file…</button>
        <input ref={fileInput} type="file" accept=".mcap" class="cap-file-input" onChange={(e) => {
          const file = (e.currentTarget as HTMLInputElement).files?.[0];
          if (file) openFromFile(file);
          (e.currentTarget as HTMLInputElement).value = "";
        }} />
        {arm && arm.state !== "idle" ? <span class="cap-arm-state">{arm.state}{arm.error ? `: ${arm.error}` : ""}</span> : null}
      </div>
      {busy ? <p class="muted">Opening…</p> : null}
      {error ? <p class="error">{error}</p> : null}
      {canList ? (
        <>
          <h2>Captures on this computer</h2>
          {listError ? <p class="error">{listError}</p> : null}
          {entries === undefined ? <p class="muted">Loading…</p> : entries.length === 0 ? (
            <p class="muted">No captures yet.</p>
          ) : (
            <ul class="cap-list">
              {entries.map((e) => (
                <li key={e.name}>
                  <button class="cap-list-item" onClick={() => openFromServer(e.name)}>
                    <span>{e.name}</span>
                    <span class="muted small">{(e.bytes / (1024 * 1024)).toFixed(1)} MB · {new Date(e.time).toLocaleString()}</span>
                  </button>
                </li>
              ))}
            </ul>
          )}
        </>
      ) : null}
      {dragging ? <div class="cap-drop-overlay">Drop the .mcap file to open it</div> : null}
    </div>
  );
}
