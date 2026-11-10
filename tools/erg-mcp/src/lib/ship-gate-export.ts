// The export's ship gate (lib/ship-gate.ts) over the open project's details: the same count detail_budget answers.
import { shipRange } from "./ship-gate";
import { simFingerprint } from "./sim-fingerprint";

export function exportGate(store: Parameters<typeof simFingerprint>[0], asked?: string) {
  const c = simFingerprint(store, "").counts;
  const details = (c.simDetails ?? 0) + (c.scenery ?? 0) + (c.light ?? 0) + (c.emitter ?? 0) + (c.sound ?? 0);
  return { details, ...shipRange(details, asked) };
}
