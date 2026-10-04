// The local content importer's own page (spec §12: Melange.exe, `#/plugins/<id>/import`). One page serves any
// plugin with a recipe — every word of the disclosure, errors and results comes from the RPC `Importer` (its
// `content`/`sources`), via web/src/launcher/copy.ts, so nothing here names Caravan or Renewation directly.
import type { ComponentChildren } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import type { Client } from "../../sdk/client";
import { errorText } from "../../sdk/hooks";
import {
  BROWSE_TIMEOUT_MS, importEventOf, importMapsResultOf, importerOf, type ImportMap, type ImportPack, type Importer,
} from "../api";
import {
  CHOOSE_FILE_LABEL, DAMAGED_BANNER, IMPORT_BUTTON_LABEL, SOURCE_LOCAL_LABEL, deleteZipActionLabel, deleteZipCheckboxLabel,
  disclosureCheckboxLabel, disclosureCopy, importErrorText, importGateText, importProgressLine, importedHeaderLine,
  keepZipLabel, localFileChosenLine, localFileHint, reimportConfirmLine, removeConfirmLine, resultCopy, sourceDownloadLabel,
  staleBanner, termsLinkLabel,
} from "../copy";
import { MapBrowser } from "../import/MapBrowser";
import { Packs } from "../import/Packs";

type ChosenFile = { path: string; name: string; size: number };

export function Import({ client, plugin, onBack }: { client: Client; plugin: string; onBack: () => void }) {
  const [importer, setImporter] = useState<Importer>();
  const [mapsData, setMapsData] = useState<{ maps: ImportMap[]; packs: ImportPack[] }>();
  const [loadError, setLoadError] = useState<string>();

  const [accepted, setAccepted] = useState(false);
  const [sourceChoice, setSourceChoice] = useState<string>("");
  const [chosenFile, setChosenFile] = useState<ChosenFile>();
  const [keepZip, setKeepZip] = useState(true);
  const [browsing, setBrowsing] = useState(false);
  const [starting, setStarting] = useState(false);
  const [startError, setStartError] = useState<string>();
  const [dismissed, setDismissed] = useState(false);
  const [cancelling, setCancelling] = useState(false);
  const prevPhase = useRef<string>();

  const [showReimport, setShowReimport] = useState(false);
  const [showRemove, setShowRemove] = useState(false);
  const [deleteZipOnRemove, setDeleteZipOnRemove] = useState(false);
  const [actionBusy, setActionBusy] = useState(false);
  const [actionError, setActionError] = useState<string>();

  const load = () => {
    client.call<unknown>("import.status", { plugin }).then((r) => { setImporter(importerOf(r)); setLoadError(undefined); }, (e) => setLoadError(errorText(e)));
  };
  useEffect(load, [client, plugin]);

  useEffect(() => {
    if (!client.has("import")) return;
    return client.subscribe<unknown>("import", undefined, (m) => {
      const ev = importEventOf(m);
      if (ev.importers) {
        const found = ev.importers.find((i) => i.plugin === plugin);
        if (found) setImporter(found);
      }
      if (ev.job && ev.job.plugin === plugin) setImporter((prev) => (prev ? { ...prev, job: ev.job } : prev));
    });
  }, [client, plugin]);

  const job = importer?.job;
  useEffect(() => {
    const phase = job?.phase;
    if (phase && phase !== prevPhase.current && (phase === "downloading" || phase === "copying")) { setDismissed(false); setCancelling(false); }
    prevPhase.current = phase;
  }, [job?.phase]);

  const loadMaps = () => {
    client.call<unknown>("import.maps", { plugin }).then((r) => setMapsData(importMapsResultOf(r)), (e) => setActionError(errorText(e)));
  };
  const imported = !!importer?.imported;
  useEffect(() => { if (imported) loadMaps(); }, [imported, importer?.imported?.fingerprint]);

  const source0 = importer?.sources[0];
  useEffect(() => { if (source0 && !sourceChoice) setSourceChoice(source0.id); }, [source0?.id]);

  if (!importer) return <div data-page="import"><div class="lw-skel"><div class="lw-skel-row" /><div class="lw-skel-row" /></div>{loadError ? <p class="error">{loadError}</p> : null}</div>;

  const back = <button class="link" onClick={onBack}>← Plugins</button>;
  const header = (
    <div style="margin-bottom:16px">
      {back}
      <h1 style="margin:4px 0 0;font-size:18px">{importer.name}</h1>
      <p class="muted" style="margin:0">Import maps</p>
    </div>
  );

  if (importer.status === "unsupported") {
    return (
      <div data-page="import" data-plugin={plugin} data-import-step="unsupported">
        {header}
        <p class="error">This plugin's recipe needs a newer version of Melange.{importer.statusReason ? ` ${importer.statusReason}` : ""}</p>
      </div>
    );
  }

  const activeJob = job && job.phase !== "idle" && !(dismissed && (job.phase === "done" || job.phase === "cancelled" || job.phase === "error")) ? job : undefined;
  const building = activeJob && !["done", "error", "cancelled"].includes(activeJob.phase);

  const browse = async () => {
    setBrowsing(true);
    setStartError(undefined);
    try {
      const r = await client.call<{ path?: string; name?: string; size?: number; cancelled?: boolean }>("import.browse", { plugin }, BROWSE_TIMEOUT_MS);
      if (r?.path) { setChosenFile({ path: r.path, name: r.name || r.path, size: r.size || 0 }); setSourceChoice("file"); }
    } catch (e) {
      setStartError(errorText(e));
    } finally {
      setBrowsing(false);
    }
  };

  const start = async () => {
    const source = sourceChoice === "file" ? (chosenFile ? { kind: "file" as const, path: chosenFile.path } : undefined)
      : { kind: "download" as const, id: sourceChoice };
    if (!source) return;
    setStarting(true);
    setStartError(undefined);
    setDismissed(false);
    try {
      await client.call("import.start", { plugin, source, keepZip, accepted: true });
    } catch (e) {
      setStartError(errorText(e));
    } finally {
      setStarting(false);
    }
  };

  const cancel = () => { setCancelling(true); client.call("import.cancel", { plugin }).catch((e) => setStartError(errorText(e))); };

  const dismissTerminal = () => { setDismissed(true); load(); };

  const runAction = async (fn: () => Promise<unknown>) => {
    setActionBusy(true);
    setActionError(undefined);
    try { await fn(); load(); } catch (e) { setActionError(errorText(e)); } finally { setActionBusy(false); }
  };

  // Re-import reuses the primary (download) source: the engine re-verifies a cached zip before every use (§4.1), so
  // this reuses it when present and otherwise re-downloads, without asking the disclosure question again.
  const doReimport = () => {
    setShowReimport(false);
    setDismissed(false);
    if (source0) runAction(() => client.call("import.start", { plugin, source: { kind: "download", id: source0.id }, keepZip: true, accepted: true }));
  };
  const doRemove = () => { setShowRemove(false); runAction(() => client.call("import.uninstall", { plugin, deleteZip: deleteZipOnRemove })); };
  const doDeleteZip = () => runAction(() => client.call("import.deleteZip", { plugin }));

  // -- Active job: progress / error / cancelled / result --------------------------------------------------------
  if (activeJob) {
    return (
      <div data-page="import" data-plugin={plugin} data-import-step={building ? "progress" : activeJob.phase === "done" ? "result" : activeJob.phase}>
        {header}
        {building ? (
          <div class="lw-progress" aria-live="polite">
            <div class="lw-progress-row"><span class="spin" />{importProgressLine(activeJob)}</div>
            {activeJob.phase === "downloading" || activeJob.phase === "copying" ? (
              <progress max={activeJob.total || 100} value={activeJob.bytes} />
            ) : activeJob.phase === "building" && activeJob.of > 0 ? (
              <progress max={activeJob.of} value={activeJob.step} />
            ) : null}
            <div class="row"><button class="btn" data-cancel disabled={cancelling} onClick={cancel}>{cancelling ? "Cancelling…" : "Cancel"}</button></div>
          </div>
        ) : activeJob.phase === "error" ? (
          <div role="alert" data-error={activeJob.reason}>
            <p class="error">{importErrorText(activeJob, importer)}</p>
            <button class="btn" onClick={dismissTerminal}>Try again</button>
          </div>
        ) : activeJob.phase === "cancelled" ? (
          <div data-cancelled>
            <p class="hint">{importProgressLine(activeJob)}</p>
            <button class="btn" onClick={dismissTerminal}>Continue</button>
          </div>
        ) : activeJob.result ? (
          <ResultView result={activeJob.result} content={importer.content} onBrowse={() => { dismissTerminal(); }} onDone={dismissTerminal} />
        ) : null}
      </div>
    );
  }

  // -- Imported (or stale/damaged): header, banners, packs, map browser -----------------------------------------
  if (imported && importer.imported) {
    const im = importer.imported;
    return (
      <div data-page="import" data-plugin={plugin} data-import-step="imported" data-status={importer.status}>
        {header}
        <p class="muted small">{importedHeaderLine(importer)}</p>
        {importer.status === "stale" ? (
          <div class="lc-notice" role="status" data-notice="stale">
            <span>{staleBanner(importer.name)}</span>
            <button class="btn" onClick={() => setShowReimport(true)}>Re-import</button>
          </div>
        ) : null}
        {importer.status === "damaged" ? (
          <div class="lc-notice" role="status" data-notice="damaged">
            <span>{DAMAGED_BANNER}</span>
            <button class="btn" onClick={() => setShowReimport(true)}>Repair</button>
          </div>
        ) : null}
        <div class="row" style="margin:12px 0;gap:8px">
          {importer.status !== "stale" && importer.status !== "damaged" ? <button class="btn" onClick={() => setShowReimport(true)}>Re-import</button> : null}
          {importer.zip?.bytes ? <button class="btn" disabled={actionBusy} onClick={doDeleteZip}>{deleteZipActionLabel(importer.zip.bytes)}</button> : null}
          <button class="btn danger" onClick={() => setShowRemove(true)}>Remove imported maps</button>
        </div>
        {actionError ? <p class="error" role="alert">{actionError}</p> : null}
        {mapsData ? <Packs client={client} plugin={plugin} packs={mapsData.packs} onChanged={(packs) => setMapsData((d) => (d ? { ...d, packs } : d))} /> : null}
        {mapsData ? <MapBrowser client={client} plugin={plugin} maps={mapsData.maps} onChanged={(maps) => setMapsData((d) => (d ? { ...d, maps } : d))} /> : null}
        {showReimport ? (
          <Dialog title="Re-import?" onClose={() => setShowReimport(false)}>
            <p class="small">{reimportConfirmLine(importer.content)}</p>
            <div class="row">
              <button class="btn primary" onClick={doReimport}>Re-import</button>
              <button class="btn" onClick={() => setShowReimport(false)}>Cancel</button>
            </div>
          </Dialog>
        ) : null}
        {showRemove ? (
          <Dialog title="Remove maps?" onClose={() => setShowRemove(false)}>
            <p class="small">{removeConfirmLine(importer.name, im.packs.length)}</p>
            {importer.zip?.bytes ? (
              <label class="small"><input type="checkbox" checked={deleteZipOnRemove} onChange={(e) => setDeleteZipOnRemove((e.currentTarget as HTMLInputElement).checked)} />
                {" "}{deleteZipCheckboxLabel(importer.zip.bytes)}</label>
            ) : null}
            <div class="row">
              <button class="btn danger" disabled={actionBusy} onClick={doRemove}>Remove</button>
              <button class="btn" onClick={() => setShowRemove(false)}>Cancel</button>
            </div>
          </Dialog>
        ) : null}
      </div>
    );
  }

  // -- Not imported: disclosure, source choice, import button ---------------------------------------------------
  const disclosure = disclosureCopy(importer, source0);
  const gateText = importer.gate ? importGateText(importer.gate) : undefined;
  const ready = sourceChoice === "file" ? !!chosenFile : !!sourceChoice;
  const canImport = accepted && ready && !gateText && !starting;

  return (
    <div data-page="import" data-plugin={plugin} data-import-step="form">
      {header}
      <div class="li-card">
        <h2 style="margin-top:0">{disclosure.heading}</h2>
        <p>{disclosure.intro}</p>
        <ul>{disclosure.bullets.map((b, i) => <li key={i}>{b}</li>)}</ul>
        {importer.content.termsUrl ? (
          <p><a class="link" href={importer.content.termsUrl} target="_blank" rel="noreferrer">{termsLinkLabel(importer.content.publisher)}</a></p>
        ) : null}
        <label class="small" style="display:block;margin:10px 0">
          <input type="checkbox" data-accept checked={accepted} onChange={(e) => setAccepted((e.currentTarget as HTMLInputElement).checked)} />
          {" "}{disclosureCheckboxLabel(importer.content.publisher)}
        </label>

        <h3>Get the file</h3>
        {importer.sources.map((s) => (
          <label class="lw-choice" key={s.id}>
            <input type="radio" name="import-source" data-source={s.id} checked={sourceChoice === s.id} onChange={() => setSourceChoice(s.id)} />
            <span>{sourceDownloadLabel(s)}</span>
          </label>
        ))}
        <label class="lw-choice">
          <input type="radio" name="import-source" data-source="file" checked={sourceChoice === "file"} onChange={() => setSourceChoice("file")} />
          <span>
            {SOURCE_LOCAL_LABEL}{" "}
            <button class="btn" type="button" disabled={browsing} onClick={browse} data-browse>{browsing ? "Choosing…" : CHOOSE_FILE_LABEL}</button>
            <div class="muted small">{chosenFile ? localFileChosenLine(chosenFile.name, chosenFile.size) : source0 ? localFileHint(source0) : ""}</div>
          </span>
        </label>

        {source0 ? (
          <label class="small" style="display:block;margin:10px 0">
            <input type="checkbox" checked={keepZip} onChange={(e) => setKeepZip((e.currentTarget as HTMLInputElement).checked)} /> {keepZipLabel(source0.size)}
          </label>
        ) : null}

        {startError ? <p class="error" role="alert">{startError}</p> : null}
        {gateText ? <p class="hint" data-gate>{gateText}</p> : (
          <button class="btn primary" data-import disabled={!canImport} onClick={start}>{starting ? "Starting…" : IMPORT_BUTTON_LABEL}</button>
        )}
      </div>
    </div>
  );
}

function ResultView({ result, content, onBrowse, onDone }: { result: NonNullable<Importer["job"]>["result"]; content: Importer["content"]; onBrowse: () => void; onDone: () => void }) {
  if (!result) return null;
  const copy = resultCopy(result, content);
  return (
    <div data-result>
      <h2>{copy.heading}</h2>
      {copy.lines.map((l, i) => <p key={i} class="small">{l}</p>)}
      <div class="row">
        <button class="btn primary" onClick={onBrowse}>Browse maps</button>
        <button class="btn" onClick={onDone}>Done</button>
      </div>
    </div>
  );
}

function Dialog({ title, onClose, children }: { title: string; onClose: () => void; children: ComponentChildren }) {
  const h2 = useRef<HTMLHeadingElement>(null);
  useEffect(() => { h2.current?.focus(); }, []);
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => { if (e.key === "Escape") onClose(); };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [onClose]);
  return (
    <div class="li-dialog-backdrop" onClick={onClose}>
      <div class="li-dialog" role="dialog" aria-modal="true" aria-labelledby="li-dialog-title" onClick={(e) => e.stopPropagation()}>
        <h2 id="li-dialog-title" tabIndex={-1} ref={h2}>{title}</h2>
        {children}
      </div>
    </div>
  );
}
