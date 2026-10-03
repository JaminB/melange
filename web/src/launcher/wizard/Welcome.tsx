import { StepHeading } from "../components/StepHeading";
import { WELCOME } from "../copy";
import type { WizardProps } from "./types";

export function Welcome({ dispatch }: WizardProps) {
  return (
    <main class="lw-card" aria-label="Welcome" data-step="welcome">
      <div class="lw-body">
        <StepHeading>{WELCOME.title}</StepHeading>
        {WELCOME.body.map((p, i) => <p key={i}>{p}</p>)}
        <WelcomeArt />
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

// A quiet accent filling the card below the intro text: a dawn sun over dunes, the same motif as the brand mark.
function WelcomeArt() {
  return (
    <div class="lw-welcome-art" aria-hidden="true">
      <svg viewBox="0 0 320 170" focusable="false">
        <circle cx="160" cy="78" r="34" fill="var(--accent-soft)" />
        <circle cx="160" cy="78" r="34" fill="none" stroke="var(--accent-strong)" stroke-width="2" />
        <g stroke="var(--accent-strong)" stroke-width="2" stroke-linecap="round" opacity="0.55">
          <path d="M160 26v14" /><path d="M122 40l10 10" /><path d="M198 40l-10 10" />
          <path d="M108 78h14" /><path d="M212 78h-14" />
        </g>
        <path d="M0 132q60-24 130-10t190-6v54H0Z" fill="var(--surface-2)" stroke="var(--border)" stroke-width="1.5" />
        <path d="M0 150q70-20 160-6t160 2v24H0Z" fill="var(--bg)" stroke="var(--border)" stroke-width="1.5" />
      </svg>
    </div>
  );
}
