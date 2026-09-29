// The "Replays" panel: the library by default, switching to the timeline or the divergence diff viewer for one
// file. Kept as a single panel (rather than three separate nav tabs) so opening a replay's timeline or a flagged
// entry's bundle is just a click, with no cross-panel routing needed; the library, timeline and diff view still
// each live in their own module (replays/, timeline/, desync/) for the same reason any large panel is split up.
import { render } from "preact";
import { useEffect, useState } from "preact/hooks";
import type { Client } from "../../sdk/client";
import { openWsr, type WsrFile } from "../../sdk/wsr";
import { Desync, type OpenRequest } from "../desync/Desync";
import { Timeline } from "../timeline/Timeline";
import { Library } from "./Library";

export function mount(el: HTMLElement, c: Client): () => void {
  render(<Replays client={c} />, el);
  return () => render(null, el);
}

type Mode = { kind: "library" } | { kind: "timeline"; name: string; file: WsrFile } | { kind: "diff"; request: OpenRequest };

interface DivergenceNotice { key: string; tick: number; source: string; bundle: string }

function Replays({ client }: { client: Client }) {
  const [mode, setMode] = useState<Mode>({ kind: "library" });
  const [error, setError] = useState<string>();
  const [notices, setNotices] = useState<DivergenceNotice[]>([]);

  useEffect(() => {
    if (!client.has("wormsign.divergence")) return;
    return client.subscribe<unknown>("wormsign.divergence", undefined, (v) => {
      if (!v || typeof v !== "object") return;
      const o = v as Record<string, unknown>;
      const bundle = typeof o.bundle === "string" ? o.bundle : "";
      if (!bundle) return; // no bundle written yet for this divergence
      const tick = typeof o.tick === "number" ? o.tick : 0;
      const source = typeof o.source === "string" ? o.source : "";
      setNotices((list) => [{ key: `${bundle}-${Date.now()}`, tick, source, bundle }, ...list].slice(0, 5));
    });
  }, []);

  const fetchArrayBuffer = (name: string): Promise<ArrayBuffer> =>
    fetch(`/replays/${encodeURIComponent(name)}`, { credentials: "same-origin" }).then((r) => {
      if (!r.ok) throw new Error(`HTTP ${r.status} fetching ${name}`);
      return r.arrayBuffer();
    });

  const openServerFile = (name: string) => {
    setError(undefined);
    fetchArrayBuffer(name).then(openWsr).then((file) => setMode({ kind: "timeline", name, file })).catch((e) => setError(e instanceof Error ? e.message : String(e)));
  };

  const openLocalFile = (file: File) => {
    setError(undefined);
    file.arrayBuffer().then(openWsr).then((f) => setMode({ kind: "timeline", name: file.name, file: f })).catch((e) => setError(e instanceof Error ? e.message : String(e)));
  };

  const openBundleFile = (file: File) => {
    setError(undefined);
    file.arrayBuffer().then((buf) => setMode({ kind: "diff", request: { name: file.name, buffer: buf } }));
  };

  const openServerBundle = (bundleName: string) => {
    setError(undefined);
    fetchArrayBuffer(bundleName).then((buf) => setMode({ kind: "diff", request: { name: bundleName, buffer: buf } })).catch((e) => setError(e instanceof Error ? e.message : String(e)));
  };

  return (
    <div class="rp-root">
      {mode.kind === "library" && notices.length ? (
        <div class="rp-notices">
          {notices.map((n) => (
            <div class="rp-notice row between" key={n.key}>
              <span>Desync at tick {n.tick}{n.source ? ` (${n.source})` : ""}; bundle saved.</span>
              <button class="link" onClick={() => openServerBundle(n.bundle)}>View</button>
            </div>
          ))}
        </div>
      ) : null}
      {error ? <p class="error pad-x">{error}</p> : null}
      {mode.kind === "library" ? (
        <Library client={client} onOpenServerFile={openServerFile} onOpenLocalFile={openLocalFile} onOpenBundleFile={openBundleFile} />
      ) : mode.kind === "timeline" ? (
        <Timeline file={mode.file} name={mode.name} onClose={() => setMode({ kind: "library" })} />
      ) : (
        <Desync openRequest={mode.request} onClose={() => setMode({ kind: "library" })} />
      )}
    </div>
  );
}
