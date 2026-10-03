import { useState } from "preact/hooks";
import type { Client } from "../../sdk/client";
import { errorText } from "../../sdk/hooks";
import { Logs } from "../../panels/logs";
import { ExternalLinkIcon } from "../icons";

export function Help({ client, version }: { client: Client; version: string }) {
  const [error, setError] = useState<string>();
  const saveLogs = () => client.call("launcher.openPath", { what: "logs" }).catch((e) => setError(errorText(e)));
  return (
    <div data-page="help">
      <h1 style="margin:0 0 16px;font-size:18px">Help</h1>
      {error ? <p class="error" role="alert">{error}</p> : null}
      <div class="row" style="margin-bottom:16px">
        <button class="btn" onClick={saveLogs}>Save logs as zip</button>
        <a class="btn" href="https://github.com/JaminB/melange#readme" target="_blank" rel="noreferrer">README <ExternalLinkIcon size={14} /></a>
        <a class="btn" href="https://github.com/JaminB/melange/issues" target="_blank" rel="noreferrer">Report an issue <ExternalLinkIcon size={14} /></a>
      </div>
      <p class="muted small">Melange {version}</p>
      <div style="height:420px;border:1px solid var(--border);border-radius:8px;overflow:hidden;margin-top:12px">
        <Logs client={client} />
      </div>
    </div>
  );
}
