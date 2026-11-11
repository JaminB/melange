// The ship gate for a heavily dressed level (detail layer round 8; the round-7 judge: "on any Melange build without [Fixes]
// MeshInstances the map crashes the client at load ... needs a guard: a minimum-Melange-version declaration"). Pure.
// The engine's mesh-instance pool holds 550 (lib/budget.ts); round 6 measured a Caldera of 520 details loading and 546
// crashing, so a level over VANILLA_SAFE_DETAILS needs a Melange that raises the pool. Such a level's pack declares it in
// spice.json (melange.range), which every Melange since 0.2 checks before it offers the level: an older Melange refuses
// the pack instead of crashing in it.
//
// The same holds for every feature a level uses that an older Melange does not know (Dustcrown round 1: the export's
// default range said >=0.2.0 for a level with spawn areas and a mod mesh bank, and the bench script patched spice.json by
// hand): the range is the newest release any used feature needs.

/** The detail count a level may hold and still load on the vanilla 550-instance pool with the match's own meshes. */
export const VANILLA_SAFE_DETAILS = 450;
/** The first Melange release that raises the pool ([Fixes] MeshInstances): 0.14.0. */
export const MESH_POOL_MELANGE = "0.14.0";
/** The first Melange release that loads a pack's own mesh banks (spice.json `meshes`): 0.14.0. */
export const MESH_BANK_MELANGE = "0.14.0";
/** The first Melange release with spawns.mode areas (an older one refuses the patch: `spawns: unknown key 'areas'`). */
export const SPAWN_AREAS_MELANGE = "0.15.0";
export const DEFAULT_RANGE = ">=0.2.0";

/** What a level uses that sets its pack's least Melange. */
export interface ShipFeatures { spawnAreas?: boolean; modMeshes?: string[]; }

const parts = (v: string) => v.split(".").map((n) => parseInt(n, 10) || 0);
/** x.y.z compared: below 0 when a is older. */
export const compareVersions = (a: string, b: string) => { const p = parts(a), q = parts(b); return p[0] - q[0] || p[1] - q[1] || p[2] - q[2]; };
/** The least version a simple range's lower bound names (">=0.14.0", ">0.14.1 <2", "^0.15.0", "0.15.0"); null when it has none. */
export function rangeFloor(range: string): string | null {
  const m = /(?:>=|>|\^|~|=)?\s*(\d+\.\d+\.\d+)/.exec(range.trim().split(/\s+/).find((t) => !t.startsWith("<")) ?? "");
  return m ? m[1] : null;
}

/** The melange.range a pack of a level with `details` details and these features should declare, and why. `asked` (a
 * caller's own range) wins when given; `warning` then says when its floor is under what the level needs. */
export function shipRange(details: number, asked?: string, features: ShipFeatures = {}): { melangeRange: string; gated: boolean; why: string; needs?: { feature: string; melange: string }[]; warning?: string } {
  const needs: { feature: string; melange: string }[] = [];
  if (details > VANILLA_SAFE_DETAILS) needs.push({ feature: `${details} details: over the ${VANILLA_SAFE_DETAILS} the vanilla 550-instance mesh pool loads ([Fixes] MeshInstances)`, melange: MESH_POOL_MELANGE });
  if (features.modMeshes?.length) needs.push({ feature: `the pack's own meshes (${features.modMeshes.slice(0, 4).join(", ")}${features.modMeshes.length > 4 ? ", ..." : ""}: a mod mesh bank, spice.json meshes)`, melange: MESH_BANK_MELANGE });
  if (features.spawnAreas) needs.push({ feature: "spawns.mode areas (the seeded team-to-area deal)", melange: SPAWN_AREAS_MELANGE });
  const least = needs.reduce<string | null>((v, n) => (v === null || compareVersions(n.melange, v) > 0 ? n.melange : v), null);
  // the old wording when the mesh pool alone gates it (Caldera's export)
  const why = !needs.length ? `${details} details: within the vanilla mesh pool`
    : needs.length === 1 && details > VANILLA_SAFE_DETAILS ? `${details} details: over the ${VANILLA_SAFE_DETAILS} the vanilla 550-instance mesh pool loads, so the pack needs Melange ${MESH_POOL_MELANGE}+ ([Fixes] MeshInstances)`
    : `${needs.filter((n) => n.melange === least).map((n) => n.feature).join("; ")}, so the pack needs Melange ${least}+`;
  if (asked) {
    const floor = rangeFloor(asked);
    const low = least !== null && floor !== null && compareVersions(floor, least) < 0;
    return { melangeRange: asked, gated: asked !== DEFAULT_RANGE, why: "asked", ...(needs.length ? { needs } : {}),
      ...(low ? { warning: `melangeRange ${asked} admits a Melange older than ${least}, which the level needs (${why}): an older Melange would offer a level it cannot load` } : {}) };
  }
  if (least !== null) return { melangeRange: `>=${least}`, gated: true, why, ...(needs.length > 1 || details <= VANILLA_SAFE_DETAILS ? { needs } : {}) };
  return { melangeRange: DEFAULT_RANGE, gated: false, why };
}
