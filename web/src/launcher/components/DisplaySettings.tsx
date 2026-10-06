import { useEffect, useState } from "preact/hooks";
import type { Client } from "../../sdk/client";
import { errorText } from "../../sdk/hooks";
import type { DisplaySize, DisplayState } from "../api";
import { defaultWindowed, displayOf, displaySizeOptions, sizeFromKey, sizeKey } from "../api";
import { DISPLAY_RUNNING, displayFullscreenHelp, displayNote, displaySizeText, windowSizeHelp } from "../copy";

// Settings › Display: Melange's borderless fullscreen and the window size the game opens at. Each change is written
// at once (display.set); the server refuses while the game runs, since the game reads both when it starts.
export function DisplaySettings({ client, running }: { client: Client; running: boolean }) {
  const [d, setD] = useState<DisplayState>();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string>();

  // Again when the game closes: switching fullscreen in game changes Melange.ini.
  useEffect(() => {
    client.call<unknown>("display.get").then((r) => setD(displayOf(r))).catch((e) => setError(errorText(e)));
  }, [client, running]);

  const save = async (fullscreen: boolean, size: DisplaySize) => {
    setBusy(true);
    setError(undefined);
    try {
      setD(displayOf(await client.call<unknown>("display.set", { fullscreen, width: size.w, height: size.h })));
    } catch (e) {
      setError(errorText(e));
    } finally {
      setBusy(false);
    }
  };

  if (!d) return <p class="muted small">{error ?? "Reading the game's display settings…"}</p>;
  const size = defaultWindowed(d);
  const blocked = d.refused ?? (running ? DISPLAY_RUNNING : undefined);
  const note = displayNote(d);
  const options = displaySizeOptions(d);
  const fsDisabled = busy || !!blocked || !size || (!d.fullscreen && !d.melangeIni);
  return (
    <div class="ls-display">
      {error ? <p class="error" role="alert">{error}</p> : null}
      {blocked ? <p class="hint" role="status" data-display-refused>{blocked}</p> : null}
      <div class="row" style="gap:8px;align-items:center">
        <button class="lp-switch" role="switch" aria-checked={d.fullscreen} aria-labelledby="ls-display-fs" data-display-fullscreen
                disabled={fsDisabled} title={blocked} onClick={() => size && save(!d.fullscreen, size)} />
        <span id="ls-display-fs">Fullscreen</span>
      </div>
      <p class="muted small">{displayFullscreenHelp(d)}</p>
      {note ? <p class="hint small" data-display-note>{note}</p> : null}
      <div class="ls-row" style="margin-top:4px">
        <label class="ls-row-label" for="ls-display-size">Window size</label>
        <select id="ls-display-size" data-display-size disabled={busy || !!blocked || !options.length} title={blocked}
                value={size ? sizeKey(size) : ""}
                onChange={(e) => { const s = sizeFromKey((e.currentTarget as HTMLSelectElement).value); if (s) save(d.fullscreen, s); }}>
          {options.map((o) => <option key={o.key} value={o.key}>{displaySizeText(o.size)}{o.native ? " (your monitor)" : ""}</option>)}
        </select>
      </div>
      <p class="muted small">{windowSizeHelp(d)}</p>
    </div>
  );
}
