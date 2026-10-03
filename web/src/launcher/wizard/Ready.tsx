import { useState } from "preact/hooks";
import { errorText } from "../../sdk/hooks";
import { StepHeading } from "../components/StepHeading";
import type { WizardProps } from "./types";

export function Ready({ client, state, dispatch, onFinish }: WizardProps) {
  const [busy, setBusy] = useState<"launch" | "open">();
  const [error, setError] = useState<string>();
  const path = state.status?.game?.path ?? state.selected ?? "";
  const sunstone = state.recommended?.items.find((i) => i.id === "sunstone" && state.choices[i.id]?.enabled);
  const line = sunstone ? `${sunstone.name} (${String(sunstone.settings.quality ?? state.choices[sunstone.id]?.settings.quality ?? "Bold")}) is on.` : "";

  const finish = async (how: "launch" | "open") => {
    setBusy(how);
    setError(undefined);
    try {
      await client.call("launcher.shortcuts", { startMenu: state.shortcuts.startMenu, desktop: state.shortcuts.desktop });
      if (how === "launch") await client.call("launcher.launch");
      onFinish?.();
    } catch (e) {
      setError(errorText(e));
    } finally {
      setBusy(undefined);
    }
  };

  return (
    <main class="lw-card" aria-label="You're all set" data-step="ready">
      <div class="lw-body">
        <StepHeading>You're all set</StepHeading>
        <p>Melange is installed in <span class="mono">{path}</span>.{line ? ` ${line}` : ""}</p>
        <div class="lw-ready-check">
          <label><input type="checkbox" checked={state.shortcuts.startMenu}
                         onChange={(e) => dispatch({ type: "shortcut", which: "startMenu", value: (e.currentTarget as HTMLInputElement).checked })} /> Add Melange to the Start menu</label>
          <label><input type="checkbox" checked={state.shortcuts.desktop}
                         onChange={(e) => dispatch({ type: "shortcut", which: "desktop", value: (e.currentTarget as HTMLInputElement).checked })} /> Add a desktop shortcut</label>
        </div>
        {error ? <p class="error" role="alert">{error}</p> : null}
      </div>
      <div class="lw-foot">
        <span />
        <div class="lw-foot-right">
          <button class="btn btn-lg" data-action="open-melange" disabled={!!busy} onClick={() => finish("open")}>Open Melange</button>
          <button class="btn btn-lg btn-primary btn-min" data-action="launch-game" disabled={!!busy} onClick={() => finish("launch")}>
            {busy === "launch" ? <span class="btn-min"><span class="spin" />Launching…</span> : "Launch game"}
          </button>
        </div>
      </div>
    </main>
  );
}
