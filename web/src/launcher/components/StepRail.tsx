import { CheckCircleIcon } from "../icons";
import { STEPS, STEP_LABEL, type WizardStep } from "../state";

export function StepRail({ step }: { step: WizardStep }) {
  const i = STEPS.indexOf(step);
  return (
    <nav class="lw-rail" aria-label="Setup steps">
      <ol>
        {STEPS.map((s, idx) => (
          <li key={s} aria-current={s === step ? "step" : undefined} data-done={idx < i}>
            <span class="lw-dot">{idx < i ? <CheckCircleIcon size={14} /> : idx + 1}</span>
            <span class="lw-rail-label">{STEP_LABEL[s]}</span>
          </li>
        ))}
      </ol>
    </nav>
  );
}
