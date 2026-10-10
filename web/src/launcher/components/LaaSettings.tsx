import { useEffect, useState } from "preact/hooks";
import type { Client } from "../../sdk/client";
import { errorText } from "../../sdk/hooks";
import type { LaaState } from "../api";
import { laaOf } from "../api";
import { LAA_NO_MELANGE, LAA_NOTE, LAA_RUNNING, laaStatus } from "../copy";

// Settings › Memory: the opt-in 4 GB (large-address-aware) mode. The choice is written at once (launcher.laa.set),
// which also patches WormsMayhem.exe; the server refuses while the game runs.
export function LaaSettings({ client, running }: { client: Client; running: boolean }) {
  const [s, setS] = useState<LaaState>();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string>();

  useEffect(() => {
    client.call<unknown>("launcher.laa.get").then((r) => setS(laaOf(r))).catch((e) => setError(errorText(e)));
  }, [client, running]);

  const save = async (enabled: boolean) => {
    setBusy(true);
    setError(undefined);
    try {
      setS(laaOf(await client.call<unknown>("launcher.laa.set", { enabled })));
    } catch (e) {
      setError(errorText(e));
    } finally {
      setBusy(false);
    }
  };

  if (!s) return <p class="muted small">{error ?? "Reading the game's memory setting…"}</p>;
  const blocked = s.refused ?? (running ? LAA_RUNNING : undefined) ?? (!s.enabled && !s.melangeIni ? LAA_NO_MELANGE : undefined);
  const disabled = busy || !!blocked;
  return (
    <div class="ls-laa">
      {error ? <p class="error" role="alert">{error}</p> : null}
      {blocked ? <p class="hint" role="status" data-laa-refused>{blocked}</p> : null}
      <div class="row" style="gap:8px;align-items:center">
        <button class="lp-switch" role="switch" aria-checked={s.enabled} aria-labelledby="ls-laa-label" data-laa-toggle
                disabled={disabled} title={blocked} onClick={() => save(!s.enabled)} />
        <span id="ls-laa-label">Use up to 4 GB of memory (large-address-aware)</span>
      </div>
      <p class="muted small">{LAA_NOTE}</p>
      <p class="muted small" data-laa-status>{laaStatus(s)}</p>
    </div>
  );
}
