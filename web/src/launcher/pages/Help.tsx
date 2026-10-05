import { useState } from "preact/hooks";
import type { Client } from "../../sdk/client";
import { errorText } from "../../sdk/hooks";
import { Logs } from "../../panels/logs";
import { ExportLogs } from "../components/ExportLogs";
import { ExternalLinkIcon } from "../icons";

export function Help({ client, version }: { client: Client; version: string }) {
  const [error, setError] = useState<string>();
  const openLogs = () => client.call("launcher.openPath", { what: "logs" }).catch((e) => setError(errorText(e)));
  return (
    <div data-page="help">
      <h1 style="margin:0 0 16px;font-size:18px">Help</h1>
      <p class="muted" style="margin:0 0 12px">
        Reporting a bug? Export the last game's logs and attach the zip. User names are removed, and Steam IDs and IP addresses are hashed.
      </p>
      <div class="row" style="margin-bottom:8px">
        <ExportLogs client={client} />
        <a class="btn" href="https://github.com/JaminB/melange/issues" target="_blank" rel="noreferrer">Report an issue <ExternalLinkIcon size={14} /></a>
        <a class="btn" href="https://github.com/JaminB/melange#readme" target="_blank" rel="noreferrer">README <ExternalLinkIcon size={14} /></a>
      </div>
      {error ? <p class="error" role="alert">{error}</p> : null}
      <p class="muted small" style="margin:0 0 16px">
        Melange {version} · <button class="link" data-action="open-logs" onClick={openLogs}>Open the logs folder</button> (the raw session logs, not zipped)
      </p>
      <div style="height:420px;border:1px solid var(--border);border-radius:8px;overflow:hidden;margin-top:12px">
        <Logs client={client} />
      </div>
    </div>
  );
}
