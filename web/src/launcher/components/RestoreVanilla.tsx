import { useEffect, useState } from "preact/hooks";
import { RpcError, type Client } from "../../sdk/client";
import { errorText } from "../../sdk/hooks";
import { EXPORT_TIMEOUT_MS, LauncherErrorCode, sizeText, vanillaPlanOf, vanillaResultOf, type VanillaPlan, type VanillaResult } from "../api";
import { vanillaFoundText, vanillaOthers, vanillaVerifyText } from "../copy";

export interface RestoreVanillaProps {
  client: Client; gamePath: string; running: boolean; blocked?: string;
  onCancel: () => void; onDone: (r: VanillaResult) => void;
}

// Settings › Restore vanilla: the plan, a warning that names every mod framework it found, an "I understand" box,
// then setup.vanillaApply. Deleting can't be undone, so the confirm button stays off until the box is ticked.
export function RestoreVanilla({ client, gamePath, running, blocked, onCancel, onDone }: RestoreVanillaProps) {
  const [plan, setPlan] = useState<VanillaPlan>();
  const [ack, setAck] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<{ text: string; elevate?: boolean }>();

  const load = () => client.call<unknown>("setup.vanillaPlan").then((r) => setPlan(vanillaPlanOf(r))).catch((e) => setError({ text: errorText(e) }));
  useEffect(() => { load(); }, [client, running]);

  const apply = async () => {
    if (!plan) return;
    setBusy(true);
    setError(undefined);
    try {
      onDone(vanillaResultOf(await client.call("setup.vanillaApply", { planId: plan.planId }, EXPORT_TIMEOUT_MS)));
    } catch (e) {
      const code = e instanceof RpcError ? e.code : 0;
      if (code === LauncherErrorCode.PlanChanged) { setAck(false); load(); }
      setError({ text: errorText(e), elevate: code === LauncherErrorCode.AccessDenied });
    } finally {
      setBusy(false);
    }
  };
  const elevate = () => client.call("setup.elevate", { resume: "vanilla" }).catch((e) => setError({ text: errorText(e) }));

  const others = plan ? vanillaOthers(plan.groups) : [];
  const nothing = !!plan && !plan.refused && plan.files === 0 && plan.replays.length === 0;
  const why = running ? "Close Worms Ultimate Mayhem first." : blocked;

  return (
    <div class="lw-warn-box" role="alertdialog" aria-labelledby="vanilla-title" data-confirm="vanilla">
      <h2 id="vanilla-title">Restore vanilla — this can't be undone</h2>
      {!plan && !error ? <p class="muted small" role="status">Looking through the game folder…</p> : null}
      {plan?.refused ? <p class="error" role="alert" data-vanilla-refused>{plan.refused}</p> : null}
      {plan && !plan.refused ? (
        <div data-vanilla-plan>
          {nothing
            ? <p>Nothing to delete: <span class="mono small">{gamePath}</span> only holds the game's own files.</p>
            : <p>This permanently deletes <strong data-vanilla-count>{plan.files} file{plan.files === 1 ? "" : "s"}</strong> ({sizeText(plan.bytes)}) from <span class="mono small">{gamePath}</span>. No backup is kept.</p>}
          {plan.groups.length ? (
            <>
              <p data-vanilla-found>Found: {vanillaFoundText(plan.groups)}.</p>
              {others.length ? <p data-vanilla-others><strong>This also removes other mods, not just Melange:</strong> {others.map((g) => g.label).join(", ")}.</p> : null}
              <ul class="lh-list small" data-vanilla-groups>
                {plan.groups.map((g) => <li key={`${g.id}-${g.label}`} data-group={g.id}>{g.id === "other" ? "Other files not part of the game" : g.label} — {g.files} file{g.files === 1 ? "" : "s"}</li>)}
              </ul>
            </>
          ) : null}
          {plan.replays.length ? <p class="small" data-vanilla-replays>{plan.replays.length} replay{plan.replays.length === 1 ? " is" : "s are"} moved to <span class="mono">{plan.replaysDir}</span> first.</p> : null}
          <p class="small">Kept: the game itself, your saves (they live in Steam, not in this folder), local.cfg and the game's shader caches and logs.</p>
          {plan.verify ? (
            <p class="small" data-vanilla-verify>
              {plan.modifiedCount || plan.missingCount
                ? `${plan.modifiedCount} game file${plan.modifiedCount === 1 ? " was" : "s were"} changed and ${plan.missingCount} ${plan.missingCount === 1 ? "is" : "are"} missing. `
                : "A mod here overwrote some of the game's own files. "}
              {vanillaVerifyText(plan.store)}
            </p>
          ) : null}
          {plan.modified.length || plan.missing.length ? (
            <details class="small">
              <summary>Changed or missing game files</summary>
              <ul class="lh-list mono">{[...plan.modified.map((f) => `changed: ${f}`), ...plan.missing.map((f) => `missing: ${f}`)].map((l) => <li key={l}>{l}</li>)}</ul>
            </details>
          ) : null}
          {plan.selfInGame ? <p class="small">Melange.exe in the game folder is deleted when Melange closes.</p> : null}
          <p class="small">Melange then forgets this folder: the next time you open Melange.exe, setup starts from the beginning.</p>
          {nothing ? null : (
            <label class="small"><input type="checkbox" data-vanilla-ack checked={ack} onChange={(e) => setAck((e.currentTarget as HTMLInputElement).checked)} /> I understand that these files are deleted permanently</label>
          )}
        </div>
      ) : null}
      {why && why !== plan?.refused ? <p class="hint" role="status">{why}</p> : null}
      {error ? <p class="error" role="alert" data-vanilla-error>{error.text}</p> : null}
      <div class="row" style="margin-top:10px">
        {error?.elevate ? <button class="btn" data-vanilla-elevate onClick={elevate}>Restart as administrator</button> : null}
        <button class="btn danger" data-vanilla-apply disabled={!plan || !!plan.refused || (!ack && !nothing) || busy || !!why} title={why} onClick={apply}>
          {busy ? "Restoring…" : nothing ? "Forget this folder" : `Delete ${plan?.files ?? 0} files and restore vanilla`}
        </button>
        <button class="btn" disabled={busy} onClick={onCancel}>Cancel</button>
      </div>
    </div>
  );
}
