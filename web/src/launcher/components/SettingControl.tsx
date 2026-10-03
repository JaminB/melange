import type { Setting, Val } from "../api";

export interface SettingControlProps {
  decl: Setting; value: Val; disabled?: boolean; onChange: (v: Val) => void;
}

function optionLabel(decl: Setting, opt: string): string {
  return decl.optionLabels?.[opt]?.label ?? opt;
}

export function SettingControl({ decl, value, disabled, onChange }: SettingControlProps) {
  const id = `lset-${decl.key}`;
  if (decl.type === "bool") {
    const on = value === true;
    return (
      <button type="button" class="lp-switch" role="switch" aria-checked={on} aria-label={decl.label} data-setting={decl.key}
              disabled={disabled} onClick={() => onChange(!on)} />
    );
  }
  if (decl.type === "enum" && (decl.options?.length ?? 0) <= 5) {
    return (
      <div class="lw-seg" role="group" aria-label={decl.label} data-setting={decl.key}>
        {(decl.options ?? []).map((o) => (
          <button key={o} type="button" aria-pressed={value === o} disabled={disabled} data-option={o} onClick={() => onChange(o)}
                  title={decl.optionLabels?.[o]?.help}>
            {optionLabel(decl, o)}
          </button>
        ))}
      </div>
    );
  }
  if (decl.type === "enum") {
    return (
      <select id={id} value={String(value)} disabled={disabled} data-setting={decl.key} aria-label={decl.label}
              onChange={(e) => onChange((e.currentTarget as HTMLSelectElement).value)}>
        {(decl.options ?? []).map((o) => <option key={o} value={o}>{optionLabel(decl, o)}</option>)}
      </select>
    );
  }
  if (decl.type === "int" || decl.type === "float") {
    const n = typeof value === "number" ? value : Number(value) || 0;
    const step = decl.type === "int" ? 1 : (((decl.max ?? 1) - (decl.min ?? 0)) / 100 || 0.01);
    return (
      <div class="row" data-setting={decl.key}>
        {decl.min !== undefined && decl.max !== undefined ? (
          <input type="range" min={decl.min} max={decl.max} step={step} value={n} disabled={disabled} aria-label={decl.label}
                 onInput={(e) => onChange(Number((e.currentTarget as HTMLInputElement).value))} />
        ) : null}
        <input type="number" min={decl.min} max={decl.max} step={step} value={n} disabled={disabled} aria-label={`${decl.label} value`}
               onInput={(e) => onChange(Number((e.currentTarget as HTMLInputElement).value))} style="width: 5.5rem" />
      </div>
    );
  }
  return (
    <input class="fb-text" id={id} type="text" value={String(value)} disabled={disabled} data-setting={decl.key} aria-label={decl.label}
           onInput={(e) => onChange((e.currentTarget as HTMLInputElement).value)} />
  );
}
