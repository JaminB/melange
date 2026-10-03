import { useEffect, useState } from "preact/hooks";
import { errorText } from "../../sdk/hooks";
import { recommendedOf, type Val } from "../api";
import { SettingControl } from "../components/SettingControl";
import { StepHeading } from "../components/StepHeading";
import { OFFLINE_STORE, RECOMMENDED_NOTE } from "../copy";
import type { WizardProps } from "./types";

export function Recommended({ client, state, dispatch }: WizardProps) {
  const [loading, setLoading] = useState(!state.recommended);
  const [installing, setInstalling] = useState(false);
  const [error, setError] = useState<string>();

  useEffect(() => {
    if (state.recommended) { setLoading(false); return; }
    client.call<unknown>("recommended.get").then((r) => {
      const d = recommendedOf(r);
      if (!d.items.length) dispatch({ type: "recommendedUnreachable" });
      else dispatch({ type: "recommended", source: d.source, items: d.items });
      setLoading(false);
    }, () => { dispatch({ type: "recommendedUnreachable" }); setLoading(false); });
  }, []);

  const skip = () => dispatch({ type: "toReady" });
  const install = async () => {
    const items = (state.recommended?.items ?? []).filter((it) => state.choices[it.id]?.enabled)
      .map((it) => ({ id: it.id, settings: state.choices[it.id]?.settings ?? it.settings }));
    if (!items.length) { skip(); return; }
    setInstalling(true);
    setError(undefined);
    try {
      await client.call("recommended.apply", { items, saveAsDefaults: true });
      dispatch({ type: "toReady", applied: true });
    } catch (e) {
      setError(errorText(e));
    } finally {
      setInstalling(false);
    }
  };

  if (loading) {
    return (
      <main class="lw-card" aria-label="Recommended plugins" data-step="recommended" data-loading="true">
        <div class="lw-body"><StepHeading>Recommended plugins</StepHeading><div class="lw-skel"><div class="lw-skel-row" /></div></div>
        <div class="lw-foot"><span /></div>
      </main>
    );
  }

  if (state.recommendedUnreachable) {
    return (
      <main class="lw-card" aria-label={OFFLINE_STORE.title} data-step="recommended" data-offline="true">
        <div class="lw-body"><StepHeading>{OFFLINE_STORE.title}</StepHeading>{OFFLINE_STORE.body.map((p, i) => <p key={i}>{p}</p>)}</div>
        <div class="lw-foot"><button class="btn btn-lg" onClick={() => dispatch({ type: "back" })}>Back</button>
          <div class="lw-foot-right"><button class="btn btn-lg btn-primary" onClick={skip}>Continue</button></div></div>
      </main>
    );
  }

  const items = state.recommended?.items ?? [];
  return (
    <main class="lw-card" aria-label="Recommended plugins" data-step="recommended">
      <div class="lw-body">
        <StepHeading>Recommended plugins</StepHeading>
        <p>Plugins add features to the game. You can change these any time.</p>
        {items.map((it) => {
          const choice = state.choices[it.id] ?? { enabled: false, settings: it.settings };
          const decl = it.decl?.[0];
          return (
            <div class="lw-plugin" key={it.id} data-plugin={it.id}>
              <div class="lw-plugin-head">
                <input type="checkbox" checked={choice.enabled} disabled={!it.compatible} aria-label={`Install ${it.name}`}
                       onChange={(e) => dispatch({ type: "toggleChoice", id: it.id, enabled: (e.currentTarget as HTMLInputElement).checked })} />
                <div style="flex:1">
                  <div><span class="lw-plugin-name">{it.name}</span> <span class="lw-plugin-tag">Graphics · client-only</span></div>
                  <p class="muted small" style="margin:2px 0 0">{it.why || it.description}</p>
                  {!it.compatible ? <p class="small error">{it.reason || "Not compatible right now."}</p> : null}
                  {decl && choice.enabled ? (
                    <div style="margin-top:8px">
                      <div class="small muted">{decl.label}</div>
                      <SettingControl decl={decl} value={choice.settings[decl.key] ?? decl.default}
                                      onChange={(v: Val) => dispatch({ type: "setChoiceSetting", id: it.id, key: decl.key, value: v })} />
                    </div>
                  ) : null}
                </div>
              </div>
            </div>
          );
        })}
        <p class="muted small">{RECOMMENDED_NOTE}</p>
        {error ? <p class="error" role="alert">{error}</p> : null}
      </div>
      <div class="lw-foot">
        <button class="btn btn-lg" disabled={installing} onClick={() => dispatch({ type: "back" })}>Back</button>
        <div class="lw-foot-right">
          <button class="btn btn-lg" disabled={installing} onClick={skip}>Skip</button>
          <button class="btn btn-lg btn-primary btn-min" data-action="install-selected" disabled={installing} onClick={install}>
            {installing ? <span class="btn-min"><span class="spin" />Installing…</span> : "Install selected"}
          </button>
        </div>
      </div>
    </main>
  );
}
