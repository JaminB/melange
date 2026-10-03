import { useEffect, useRef, useState } from "preact/hooks";
import { errorText } from "../../sdk/hooks";
import { BROWSE_TIMEOUT_MS, gameCheckOf, setupStatusOf } from "../api";
import { StepHeading } from "../components/StepHeading";
import { CHECK_ACTION, checkCopy } from "../copy";
import type { WizardProps } from "./types";

export function CheckGame({ client, state, dispatch }: WizardProps) {
  const [pct, setPct] = useState(0);
  const [error, setError] = useState<string>();
  const [busy, setBusy] = useState(true);
  const tick = useRef(0);

  const runCheck = (path: string) => {
    setBusy(true);
    setError(undefined);
    setPct(10);
    dispatch({ type: "checking" });
    let p = 10;
    const timer = setInterval(() => { p = Math.min(92, p + 12); setPct(p); }, 90);
    client.call<unknown>("setup.validate", { path }).then((r) => {
      clearInterval(timer);
      setPct(100);
      setBusy(false);
      dispatch({ type: "check", check: gameCheckOf(r) });
    }, (e) => {
      clearInterval(timer);
      setBusy(false);
      setError(errorText(e));
      dispatch({ type: "error" });
    });
  };

  useEffect(() => { if (state.selected) runCheck(state.selected); }, [state.selected, tick.current]);

  const browseAgain = async () => {
    setBusy(true);
    try {
      const r = await client.call<{ path: string | null }>("setup.browse", {}, BROWSE_TIMEOUT_MS);
      if (r.path) { dispatch({ type: "select", path: r.path }); tick.current++; runCheck(r.path); } else setBusy(false);
    } catch (e) {
      setError(errorText(e));
      setBusy(false);
    }
  };

  const continueOn = async () => {
    if (!state.selected) return;
    try {
      const status = setupStatusOf(await client.call("setup.select", { path: state.selected, save: true }));
      dispatch({ type: "status", status });
      dispatch({ type: "toInstall" });
    } catch (e) {
      setError(errorText(e));
    }
  };

  if (busy && !state.check) {
    return (
      <main class="lw-card" aria-label="Checking the game" data-step="check" data-loading="true">
        <div class="lw-body">
          <StepHeading>Checking WormsMayhem.exe…</StepHeading>
          <progress max={100} value={pct} style="width:100%" />
        </div>
        <div class="lw-foot"><span /></div>
      </main>
    );
  }

  const check = state.check;
  if (!check) return null;
  const copy = checkCopy(check);
  const action = CHECK_ACTION[check.verdict];
  const primary = () => {
    if (check.verdict === "ok") return continueOn();
    if (check.verdict === "noExe") return browseAgain();
    if (check.verdict === "notFound") return dispatch({ type: "goto", step: "find" });
    tick.current++;
    runCheck(check.path);
  };
  return (
    <main class="lw-card" aria-label={copy.title} data-step="check" data-verdict={check.verdict}>
      <div class="lw-body" aria-live="polite">
        <StepHeading>{copy.title}</StepHeading>
        {copy.body.map((p, i) => i === copy.body.length - 1 && check.verdict === "wrongBuild"
          ? <p key={i} class="lw-mono-box mono" data-details>{p}</p> : <p key={i}>{p}</p>)}
        {error ? <p class="error" role="alert">{error}</p> : null}
      </div>
      <div class="lw-foot">
        <button class="btn btn-lg" onClick={() => dispatch({ type: "goto", step: "find" })}>Back</button>
        <div class="lw-foot-right">
          {action.secondary ? <button class="btn btn-lg" data-action="secondary" onClick={() => dispatch({ type: "goto", step: "find" })}>{action.secondary}</button> : null}
          <button class="btn btn-lg btn-primary" data-action="primary" disabled={busy} onClick={primary}>{action.primary}</button>
        </div>
      </div>
    </main>
  );
}
