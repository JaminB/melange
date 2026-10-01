// Export dialog: mod id, name, version, mode, the Survivor copy and the game-files notice (install vs source).
// ErgSession has no `level.export` wrapper, so this calls the client directly, as the session's own `test()`/`save()`
// do internally.
import { useState } from "preact/hooks";
import type { Client } from "../../../sdk/client";
import { errorText } from "../../../sdk/hooks";
import { EXPORT_NOTICE, validateExportForm, type ExportForm, type ExportMode } from "./model";

export interface ExportResult { dir: string; files: string[]; restartRequired: boolean; }

export function ExportDialog({ client, project, defaultName, onDone }: {
  client: Client; project: string; defaultName: string; onDone?: (r: ExportResult) => void;
}) {
  const [form, setForm] = useState<ExportForm>({ modId: "", name: defaultName, version: "1.0.0", mode: "source", survivor: true });
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string>();
  const [result, setResult] = useState<ExportResult>();
  const errors = validateExportForm(form);

  const set = <K extends keyof ExportForm>(k: K, v: ExportForm[K]) => setForm((f) => ({ ...f, [k]: v }));

  const submit = async () => {
    if (errors.length) return;
    setBusy(true);
    setError(undefined);
    try {
      const r = await client.call<ExportResult>("level.export",
        { project, modId: form.modId, name: form.name, version: form.version, mode: form.mode, survivor: form.survivor });
      setResult(r);
      onDone?.(r);
    } catch (e) {
      setError(errorText(e));
    } finally {
      setBusy(false);
    }
  };

  return (
    <div class="erg-export" data-erg-export>
      <label>Mod id <input value={form.modId} onInput={(e) => set("modId", (e.target as HTMLInputElement).value)} /></label>
      <label>Name <input value={form.name} onInput={(e) => set("name", (e.target as HTMLInputElement).value)} /></label>
      <label>Version <input value={form.version} onInput={(e) => set("version", (e.target as HTMLInputElement).value)} /></label>
      <div class="mode-row">
        {(["source", "install"] as ExportMode[]).map((m) => (
          <label key={m}>
            <input type="radio" checked={form.mode === m} onChange={() => set("mode", m)} /> {m}
          </label>
        ))}
      </div>
      <label title="Also list the level in Survivor's Prebuilt maps (Multi.<stem>.S)">
        <input type="checkbox" data-export-survivor checked={form.survivor}
          onChange={(e) => set("survivor", (e.target as HTMLInputElement).checked)} /> Survivor copy
      </label>
      <p class="muted" data-export-notice>{EXPORT_NOTICE[form.mode]}</p>
      {errors.map((e) => <p class="error" key={e}>{e}</p>)}
      {error ? <p class="error" data-export-error>{error}</p> : null}
      <button class="btn" disabled={busy || errors.length > 0} onClick={submit}>Export</button>
      {result ? (
        <p class="status-line" data-export-result>
          Wrote {result.files.length} file{result.files.length === 1 ? "" : "s"} to {result.dir}
          {result.restartRequired ? " (restart to enable)" : ""}
        </p>
      ) : null}
    </div>
  );
}
