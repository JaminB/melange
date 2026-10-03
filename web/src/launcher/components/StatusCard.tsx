import type { ComponentChildren } from "preact";

export type Tone = "ok" | "warn" | "bad";

export interface StatusCardProps {
  id: string; icon: ComponentChildren; label: string; tone: Tone; status: string; detail?: string;
  actionLabel?: string; onAction?: () => void; actionDisabled?: boolean; actionTitle?: string;
}

export function StatusCard({ id, icon, label, tone, status, detail, actionLabel, onAction, actionDisabled, actionTitle }: StatusCardProps) {
  return (
    <div class="lc-card" data-card={id}>
      <div class="lc-card-head">{icon}<span>{label}</span></div>
      <div class="lc-card-status"><span class={`lc-dot lc-dot-${tone}`} aria-hidden="true" />{status}</div>
      {detail ? <div class="lc-card-detail">{detail}</div> : null}
      {actionLabel ? (
        <button class="btn lc-card-action" data-card-action={id} disabled={actionDisabled} title={actionTitle} onClick={onAction}>{actionLabel}</button>
      ) : null}
    </div>
  );
}
