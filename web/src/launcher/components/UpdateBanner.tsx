import { useState } from "preact/hooks";
import type { Client } from "../../sdk/client";
import { errorText } from "../../sdk/hooks";
import type { UpdateStatus } from "../api";
import { WHATS_NEW, updateAppliedLine, updateBanner } from "../copy";

// Above every page: "Melange X is ready — Restart to update" once a downloaded update is verified, a quiet progress
// line while it downloads, and once, after a restart, what the update did.
export function UpdateBanner({ client, update, gameRunning }: { client: Client; update: UpdateStatus | undefined; gameRunning: boolean }) {
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string>();
  const [appliedSeen, setAppliedSeen] = useState(false);
  if (!update) return null;
  const banner = updateBanner(update, gameRunning);
  const applied = update.applied && !appliedSeen ? update.applied : undefined;

  const apply = async () => {
    setBusy(true);
    setError(undefined);
    try { await client.call("update.apply"); } catch (e) { setError(errorText(e)); setBusy(false); }
  };

  return (
    <>
      {applied ? (
        <div class="lc-notice" role="status" data-notice="update-applied" data-ok={applied.ok ? "1" : "0"}>
          <span class={applied.ok ? undefined : "tone-bad"}>{updateAppliedLine(applied)}</span>
          <button class="link" onClick={() => setAppliedSeen(true)} aria-label="Dismiss">Dismiss</button>
        </div>
      ) : null}
      {banner && !banner.action ? (
        <div class="lc-update-progress muted small" role="status" aria-live="polite" data-notice="update-progress">
          <span>{banner.text}</span>
          {update.progress && update.progress.total > 0 ? <progress max={update.progress.total} value={update.progress.got} /> : null}
        </div>
      ) : null}
      {banner?.action ? (
        <div class="lc-notice" role="status" data-notice="update">
          <span>{banner.text} — <button class="btn btn-primary" data-update-apply disabled={busy || !!banner.blocked}
                                          title={banner.blocked} onClick={apply}>{busy ? "Restarting…" : banner.action}</button>
            {banner.blocked ? <span class="muted small"> {banner.blocked}</span> : null}</span>
          {update.htmlUrl ? <a class="link" href={update.htmlUrl} target="_blank" rel="noreferrer">{WHATS_NEW}</a> : null}
          {error ? <span class="error small" role="alert">{error}</span> : null}
        </div>
      ) : null}
    </>
  );
}
