import type { Plan } from "../api";
import { planLine } from "../copy";

export function PlanList({ plan }: { plan: Plan | undefined }) {
  if (!plan || !plan.steps.length) return null;
  return (
    <details class="lw-disclosure" data-plan>
      <summary>What will change</summary>
      <ul class="lw-plan">
        {plan.steps.map((s, i) => <li key={i} data-op={s.op}>{planLine(s)}</li>)}
      </ul>
    </details>
  );
}
