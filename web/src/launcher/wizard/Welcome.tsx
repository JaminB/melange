import { StepHeading } from "../components/StepHeading";
import { WELCOME } from "../copy";
import type { WizardProps } from "./types";

export function Welcome({ dispatch }: WizardProps) {
  return (
    <main class="lw-card" aria-label="Welcome" data-step="welcome">
      <div class="lw-body">
        <StepHeading>{WELCOME.title}</StepHeading>
        {WELCOME.body.map((p, i) => <p key={i}>{p}</p>)}
      </div>
      <div class="lw-foot">
        <span />
        <div class="lw-foot-right">
          <button class="btn btn-lg btn-primary" data-action="get-started" onClick={() => dispatch({ type: "start" })}>Get started</button>
        </div>
      </div>
    </main>
  );
}
