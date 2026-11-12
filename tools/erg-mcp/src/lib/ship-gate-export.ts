// The export's ship gate (lib/ship-gate.ts) over the open project: the detail count detail_budget answers, and the
// features that set the pack's least Melange (spawn areas, the pack's own meshes).
import { lacking, rangeFloor, shipRange, type ShipFeatures } from "./ship-gate";
import { simFingerprint } from "./sim-fingerprint";

/** A scenery resource of a mod's own mesh bank: `<mod id>.<Name>` (the engine's own have no dot). */
const MOD_MESH = /^[a-z0-9][a-z0-9_-]*\.[A-Za-z0-9_]+$/;

/** What the open project uses that an older Melange does not know. */
export function shipFeatures(store: Parameters<typeof simFingerprint>[0]): ShipFeatures {
  const meshes = [...new Set(store.scene.details.map((d) => d.resource).filter((r): r is string => typeof r === "string" && MOD_MESH.test(r)))].sort();
  return { spawnAreas: store.scene.spawns.mode === "areas", ...(meshes.length ? { modMeshes: meshes } : {}) };
}

/** `levelLook`: the level-entry keys this export writes (timeOfDay, image), which an older Melange refuses. */
export function exportGate(store: Parameters<typeof simFingerprint>[0], asked?: string, levelLook: string[] = []) {
  const c = simFingerprint(store, "").counts;
  const details = (c.simDetails ?? 0) + (c.scenery ?? 0) + (c.light ?? 0) + (c.emitter ?? 0) + (c.sound ?? 0);
  return { details, ...shipRange(details, asked, { ...shipFeatures(store), ...(levelLook.length ? { levelLook } : {}) }) };
}

/** What the open project uses that a Melange of `build` does not know: the features, each with its release. */
export function projectLacks(store: Parameters<typeof simFingerprint>[0], build: string | undefined | null): { feature: string; melange: string }[] {
  return lacking(build, exportGate(store).needs ?? singleNeed(store));
}
/** shipRange leaves `needs` out when the only need is the detail count: say it here. */
function singleNeed(store: Parameters<typeof simFingerprint>[0]): { feature: string; melange: string }[] {
  const g = exportGate(store), v = rangeFloor(g.melangeRange);
  return g.gated && v ? [{ feature: g.why, melange: v }] : [];
}
