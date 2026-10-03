import { useEffect, useRef, useState } from "preact/hooks";
import { errorText } from "../../sdk/hooks";
import { RpcError } from "../../sdk/client";
import { applyResultOf, planOf, setupEventOf, setupStatusOf, type SetupProgress } from "../api";
import { CheckCircleIcon, CircleDashedIcon } from "../icons";
import { PlanList } from "../components/PlanList";
import { StepHeading } from "../components/StepHeading";
import { errorCopy, loaderChecklistLine, loaderCopy, melangeCopy } from "../copy";
import type { WizardProps } from "./types";

export function Install({ client, state, dispatch }: WizardProps) {
  const [loading, setLoading] = useState(!state.status);
  const [progress, setProgress] = useState<SetupProgress[]>([]);
  const [done, setDone] = useState(false);
  const [localError, setLocalError] = useState<string>();
  const applyingRef = useRef(false);

  const loadPlan = async (replaceLoader: boolean) => {
    setLoading(true);
    setLocalError(undefined);
    try {
      let status = state.status;
      if (!status) status = setupStatusOf(await client.call("setup.status"));
      dispatch({ type: "status", status });
      const plan = planOf(await client.call("setup.plan", { action: "install", replaceLoader }));
      dispatch({ type: "plan", plan });
    } catch (e) {
      setLocalError(errorText(e));
    } finally {
      setLoading(false);
    }
  };
  useEffect(() => { loadPlan(state.replaceLoader); }, []);

  useEffect(() => {
    if (!client.has("setup")) return;
    return client.subscribe<unknown>("setup", undefined, (m) => {
      const ev = setupEventOf(m);
      if (ev.progress) setProgress((p) => [...p.filter((x) => x.step !== ev.progress!.step), ev.progress!]);
      if (ev.status && !applyingRef.current) dispatch({ type: "status", status: ev.status });
    });
  }, [client]);

  const chooseReplace = (replace: boolean) => { dispatch({ type: "replaceLoader", value: replace }); loadPlan(replace); };

  const install = async () => {
    if (!state.plan) return;
    applyingRef.current = true;
    setProgress([]);
    dispatch({ type: "applying" });
    try {
      const r = applyResultOf(await client.call("setup.apply", { action: "install", replaceLoader: state.replaceLoader, planId: state.plan.planId }));
      setDone(true);
      setTimeout(() => dispatch({ type: "applyDone", status: r.status }), 600);
    } catch (e) {
      if (e instanceof RpcError) dispatch({ type: "applyError", code: e.code, data: e.data });
      else dispatch({ type: "error", message: errorText(e) });
    } finally {
      applyingRef.current = false;
    }
  };

  if (loading || !state.status || !state.plan) {
    return (
      <main class="lw-card" aria-label="Preparing to install" data-step="install" data-loading="true">
        <div class="lw-body">
          <StepHeading>Install Melange</StepHeading>
          <div class="lw-skel"><div class="lw-skel-row" /><div class="lw-skel-row" /><div class="lw-skel-row" /></div>
        </div>
        <div class="lw-foot"><span /></div>
      </main>
    );
  }

  const status = state.status, plan = state.plan;
  const target = status.payload.version;
  const loader = loaderCopy(status.loader, status.otherLoaders);
  const melange = melangeCopy(status, target);
  const needsChoice = plan.needsChoice === "loader" && !state.replaceLoader;
  const blockedByChoice = needsChoice;

  return (
    <main class="lw-card" aria-label={`Install Melange ${target}`} data-step="install">
      <div class="lw-body">
        <StepHeading>Install Melange {target}</StepHeading>
        {state.applying ? (
          <div class="lw-progress" aria-live="polite" data-installing="true">
            {done ? <p class="lw-check-row done"><CheckCircleIcon />Installed.</p> : (
              <>
                <div class="lw-progress-row"><span class="spin" />{progress.length ? progress[progress.length - 1].label : "Starting…"}</div>
                <ul class="lw-checklist">
                  {progress.map((p) => <li key={p.step} class="lw-check-row done"><CheckCircleIcon size={16} />{p.label}</li>)}
                </ul>
              </>
            )}
          </div>
        ) : state.applyError ? (
          <ErrorBlock code={state.applyError.code} data={state.applyError.data} onRetry={install} />
        ) : (
          <>
            <ul class="lw-checklist" data-checklist>
              <li class={`lw-check-row ${needsChoice ? "undecided" : "pending"}`} data-item="loader">
                {needsChoice ? <CircleDashedIcon size={16} /> : <CheckCircleIcon size={16} />}
                <span>{loaderChecklistLine(status.loader, status.otherLoaders, state.replaceLoader)}</span>
              </li>
              <li class="lw-check-row pending" data-item="melange"><CheckCircleIcon size={16} /><span>{melange.title}</span></li>
              <li class="lw-check-row pending" data-item="ini"><CheckCircleIcon size={16} /><span>{!status.ini.present ? "Create Melange.ini with the default settings" : status.ini.missingKeys ? `Keep your Melange.ini and add ${status.ini.missingKeys} new setting${status.ini.missingKeys === 1 ? "" : "s"}` : "Keep your Melange.ini"}</span></li>
              {status.legacy.length ? <li class="lw-check-row pending" data-item="legacy"><CheckCircleIcon size={16} /><span>Remove the old {status.legacy.join(", ")} (backed up)</span></li> : null}
            </ul>
            {needsChoice ? (
              <div class="lw-warn-box" role="alert" data-needs-choice="loader">
                <h2>{loader.title}</h2>
                {loader.body.map((p, i) => <p key={i}>{p}</p>)}
                <div class="lw-choice">
                  <label><input type="radio" name="loader-choice" onChange={() => chooseReplace(true)} /> Replace it (a backup is kept)</label>
                </div>
                <div class="lw-choice">
                  <label><input type="radio" name="loader-choice" defaultChecked onChange={() => chooseReplace(false)} /> Don't install Melange now</label>
                </div>
              </div>
            ) : null}
            <PlanList plan={plan} />
            {localError ? <p class="error" role="alert">{localError}</p> : null}
          </>
        )}
      </div>
      <div class="lw-foot">
        <button class="btn btn-lg" disabled={state.applying} onClick={() => dispatch({ type: "back" })}>Back</button>
        <div class="lw-foot-right">
          {!state.applying && !state.applyError ? (
            <button class="btn btn-lg btn-primary btn-min" data-action="install" disabled={blockedByChoice} onClick={install}>Install</button>
          ) : null}
        </div>
      </div>
    </main>
  );
}

function ErrorBlock({ code, data, onRetry }: { code: number; data?: unknown; onRetry: () => void }) {
  const copy = errorCopy(code, data);
  return (
    <div role="alert" data-apply-error={code}>
      <p><strong>{copy.title}.</strong> {copy.body.join(" ")}</p>
      <button class="btn" onClick={onRetry}>Try again</button>
    </div>
  );
}
