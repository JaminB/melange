import { useState } from "preact/hooks";
import type { Client } from "../../sdk/client";
import { errorText } from "../../sdk/hooks";
import { EXPORT_TIMEOUT_MS, exportPlaceText, exportResultOf, sizeText, type ExportResult } from "../api";

// One click: launcher.exportLogs zips the last game's logs to the Desktop and shows the zip in Explorer; then
// "Saved to Desktop · Show in folder". `compact` is the Home page's link-sized version.
export function ExportLogs({ client, compact }: { client: Client; compact?: boolean }) {
  const [busy, setBusy] = useState(false);
  const [done, setDone] = useState<ExportResult>();
  const [error, setError] = useState<string>();

  const run = async () => {
    setBusy(true);
    setError(undefined);
    setDone(undefined);
    try {
      const r = exportResultOf(await client.call("launcher.exportLogs", {}, EXPORT_TIMEOUT_MS));
      if (!r) throw new Error("The export didn't say where it saved the zip.");
      setDone(r);
    } catch (e) {
      setError(errorText(e));
    } finally {
      setBusy(false);
    }
  };
  const show = () => client.call("launcher.openPath", { what: "export" }).catch((e) => setError(errorText(e)));

  const label = busy ? <span class="btn-min"><span class="spin" />Exporting…</span> : "Export last game's logs";
  return (
    <div class="lx-export" data-export={busy ? "busy" : done ? "done" : error ? "error" : "idle"}>
      {compact
        ? <button class="link" disabled={busy} onClick={run}>{label}</button>
        : <button class="btn btn-primary" disabled={busy} onClick={run}>{label}</button>}
      {done ? (
        <span class="muted small" role="status" title={done.path}>
          Saved to {exportPlaceText(done)} ({sizeText(done.bytes)}) · <button class="link" onClick={show}>Show in folder</button>
        </span>
      ) : null}
      {error ? <span class="error small" role="alert">{error}</span> : null}
    </div>
  );
}
