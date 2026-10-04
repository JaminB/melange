// The "Map packs" section of the Import page (spec §12.2 "Packs"): one row per generated pack with the same
// Thumper enable toggle the Plugins page uses, through `import.setPacks`.
import { useState } from "preact/hooks";
import type { Client } from "../../sdk/client";
import { errorText } from "../../sdk/hooks";
import { importPacksOf, type ImportPack } from "../api";
import { PACKS_NOTE } from "../copy";

export function Packs({ client, plugin, packs, onChanged }: {
  client: Client; plugin: string; packs: ImportPack[]; onChanged: (packs: ImportPack[]) => void;
}) {
  const [busy, setBusy] = useState<string>();
  const [error, setError] = useState<string>();

  const toggle = async (p: ImportPack) => {
    setBusy(p.id);
    setError(undefined);
    try {
      const r = await client.call<{ packs?: unknown }>("import.setPacks", { plugin, packs: [{ id: p.id, enabled: !p.enabled }] });
      onChanged(importPacksOf(r?.packs));
    } catch (e) {
      setError(errorText(e));
    } finally {
      setBusy(undefined);
    }
  };

  return (
    <section data-import-packs>
      <h2>Map packs</h2>
      <ul class="lp-list" data-packs>
        {packs.map((p) => (
          <li key={p.id} class="lp-row" data-pack={p.id}>
            <button class="lp-switch" role="switch" aria-checked={p.enabled} disabled={busy === p.id} aria-label={`${p.enabled ? "Turn off" : "Turn on"} ${p.name}`}
                    data-toggle={p.id} onClick={() => toggle(p)} />
            <div class="lp-row-main">
              <div class="lp-row-name">{p.name}</div>
              <div class="lp-row-desc">{p.levels} map{p.levels === 1 ? "" : "s"}</div>
            </div>
          </li>
        ))}
      </ul>
      <p class="hint">{PACKS_NOTE}</p>
      {error ? <p class="error" role="alert">{error}</p> : null}
    </section>
  );
}
