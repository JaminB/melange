import { useEffect, useState } from "preact/hooks";
import { candidatesOf, gameCheckOf, type Candidate } from "../api";
import { errorText } from "../../sdk/hooks";
import { StepHeading } from "../components/StepHeading";
import { CHILD_HINT, findCopy } from "../copy";
import { findActions } from "../state";
import type { WizardProps } from "./types";

const SOURCE_LABEL: Record<Candidate["source"], string> = { saved: "Saved", self: "This folder", steam: "Steam", gog: "GOG", manual: "Chosen folder" };

export function FindGame({ client, state, dispatch }: WizardProps) {
  const [loading, setLoading] = useState(true);
  const [browsing, setBrowsing] = useState(false);
  const [error, setError] = useState<string>();

  const detect = async () => {
    setLoading(true);
    setError(undefined);
    try {
      const r = await client.call<unknown>("setup.detect");
      const o = (r && typeof r === "object" ? r : {}) as Record<string, unknown>;
      dispatch({ type: "candidates", candidates: candidatesOf(o.candidates) });
    } catch (e) {
      setError(errorText(e));
    } finally {
      setLoading(false);
    }
  };
  useEffect(() => { detect(); }, []);

  const browse = async () => {
    setBrowsing(true);
    setError(undefined);
    try {
      const r = await client.call<{ path: string | null; hint?: "child"; child?: string }>("setup.browse", {});
      if (!r.path) return;
      if (r.hint === "child" && r.child) { dispatch({ type: "childHint", parent: r.path, child: r.child }); return; }
      const check = gameCheckOf(await client.call("setup.validate", { path: r.path }));
      const next = [...state.candidates.filter((c) => c.path !== r.path), { path: r.path, source: "manual" as const, check }];
      dispatch({ type: "candidates", candidates: next });
      dispatch({ type: "select", path: r.path });
    } catch (e) {
      setError(errorText(e));
    } finally {
      setBrowsing(false);
    }
  };

  const useChild = async () => {
    if (!state.childHint) return;
    const path = state.childHint.child;
    setBrowsing(true);
    try {
      const check = gameCheckOf(await client.call("setup.validate", { path }));
      dispatch({ type: "candidates", candidates: [...state.candidates.filter((c) => c.path !== path), { path, source: "manual" as const, check }] });
      dispatch({ type: "useChild" });
    } catch (e) {
      setError(errorText(e));
    } finally {
      setBrowsing(false);
    }
  };

  if (state.childHint) {
    const folder = state.childHint.child.split(/[\\/]/).filter(Boolean).pop() ?? state.childHint.child;
    const copy = CHILD_HINT(folder);
    return (
      <main class="lw-card" aria-label={copy.title} data-step="find" data-hint="child">
        <div class="lw-body">
          <StepHeading>{copy.title}</StepHeading>
          <p>That folder contains the game in <strong>{folder}</strong>. Use that one?</p>
        </div>
        <div class="lw-foot">
          <button class="btn btn-lg" onClick={() => dispatch({ type: "back" })}>Back</button>
          <div class="lw-foot-right">
            <button class="btn btn-lg" data-action="browse-again" onClick={() => dispatch({ type: "browseAgain" })}>Choose again</button>
            <button class="btn btn-lg btn-primary" data-action="use-child" onClick={useChild} disabled={browsing}>Use {folder}</button>
          </div>
        </div>
      </main>
    );
  }

  if (loading) {
    return (
      <main class="lw-card" aria-label="Looking for your game" data-step="find" data-loading="true">
        <div class="lw-body">
          <StepHeading>Looking for your game…</StepHeading>
          <div class="lw-skel" aria-live="polite"><div class="lw-skel-row" /><div class="lw-skel-row" /></div>
        </div>
        <div class="lw-foot"><span />
        </div>
      </main>
    );
  }

  const copy = findCopy(state.candidates);
  const actions = findActions(state.candidates);
  return (
    <main class="lw-card" aria-label={copy.title} data-step="find">
      <div class="lw-body">
        <StepHeading>{copy.title}</StepHeading>
        {copy.body.map((p, i) => <p key={i}>{p}</p>)}
        {error ? <p class="error" role="alert">{error}</p> : null}
        {state.candidates.length ? (
          <ul class="lw-candidates" role="radiogroup" aria-label="Game folders found">
            {state.candidates.map((c) => (
              <li key={c.path} class="lw-candidate" role="radio" aria-checked={state.selected === c.path} tabIndex={0} data-candidate={c.path}
                  onClick={() => dispatch({ type: "select", path: c.path })}
                  onKeyDown={(e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); dispatch({ type: "select", path: c.path }); } }}>
                <input type="radio" name="candidate" checked={state.selected === c.path} readOnly tabIndex={-1} />
                <div style="flex:1;min-width:0">
                  <div><span class="lw-candidate-path">{c.path}</span><span class="badge-src">{SOURCE_LABEL[c.source]}</span></div>
                </div>
                <span class={`verdict-chip ${c.check.verdict === "ok" ? "verdict-ok" : "verdict-bad"}`} data-verdict={c.check.verdict}>
                  {c.check.verdict === "ok" ? "Ready" : c.check.verdict === "wrongBuild" ? "Wrong version" : c.check.verdict === "noExe" ? "No game here" : c.check.verdict === "notFound" ? "Not found" : "Unreadable"}
                </span>
              </li>
            ))}
          </ul>
        ) : null}
      </div>
      <div class="lw-foot">
        <button class="btn btn-lg" onClick={() => dispatch({ type: "goto", step: "welcome" })}>Back</button>
        <div class="lw-foot-right">
          {actions.secondary || !state.candidates.length ? (
            <button class={`btn btn-lg${state.candidates.length ? "" : " btn-primary"}`} data-action="browse" disabled={browsing} onClick={browse}>
              {browsing ? <span class="btn-min"><span class="spin" />Waiting for the folder picker…</span> : state.candidates.length ? "Choose a different folder…" : "Choose folder…"}
            </button>
          ) : null}
          {actions.primary === "continue" ? (
            <button class="btn btn-lg btn-primary" data-action="continue" disabled={!state.selected}
                    onClick={() => dispatch({ type: "goto", step: "check" })}>Continue</button>
          ) : null}
        </div>
      </div>
    </main>
  );
}
