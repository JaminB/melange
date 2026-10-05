import { useState } from "preact/hooks";
import type { Client } from "../../sdk/client";
import { errorText } from "../../sdk/hooks";
import type { VanillaResult } from "../api";
import { VANILLA_DONE, vanillaVerifyText } from "../copy";
import { StepHeading } from "./StepHeading";

// The last screen after Restore vanilla: what happened, how the overwritten game files come back, and Close. Melange
// has already forgotten the folder; closing also deletes its own Melange.exe if that was in the game folder.
export function VanillaDone({ client, result }: { client: Client; result: VanillaResult }) {
  const [closing, setClosing] = useState(false);
  const [error, setError] = useState<string>();
  const close = async () => {
    setClosing(true);
    try { await client.call("launcher.quit"); } catch (e) { setError(errorText(e)); setClosing(false); }
  };
  const changed = result.modifiedCount + result.missingCount;   // the lists themselves stop at 500
  return (
    <div class="lw">
      <div class="lw-col">
        <main class="lw-card" aria-label="Back to stock" data-vanilla-done>
          <div class="lw-body">
            <StepHeading>{VANILLA_DONE}</StepHeading>
            <p data-vanilla-deleted>Deleted {result.deleted} file{result.deleted === 1 ? "" : "s"}{result.dirsRemoved ? ` and ${result.dirsRemoved} empty folder${result.dirsRemoved === 1 ? "" : "s"}` : ""}.</p>
            {result.moved.length ? (
              <p data-vanilla-moved>Your {result.moved.length} replay{result.moved.length === 1 ? " is" : "s are"} in <span class="mono small">{result.replaysDir}</span>.</p>
            ) : null}
            {result.verify ? (
              <p data-vanilla-verify>{changed ? `${changed} of the game's own file${changed === 1 ? " was" : "s were"} changed or missing. ` : "A mod had overwritten some of the game's own files. "}
                {vanillaVerifyText(result.store, result.store === "steam" ? result.verifyStarted : undefined)}</p>
            ) : null}
            {changed ? (
              <details class="small" style="margin:0 0 12px">
                <summary>Changed or missing game files</summary>
                <ul class="lh-list mono">{[...result.modified.map((f) => `changed: ${f}`), ...result.missing.map((f) => `missing: ${f}`)].map((l) => <li key={l}>{l}</li>)}</ul>
              </details>
            ) : null}
            <p class="muted">Your saves and settings were kept. To use Melange again, open Melange.exe from the release zip: setup starts from the beginning.</p>
            {error ? <p class="error" role="alert">{error}</p> : null}
          </div>
          <div class="lw-foot">
            <span />
            <div class="lw-foot-right">
              <button class="btn btn-lg btn-primary" data-action="close-melange" disabled={closing} onClick={close}>{closing ? "Closing…" : "Close Melange"}</button>
            </div>
          </div>
        </main>
      </div>
    </div>
  );
}
