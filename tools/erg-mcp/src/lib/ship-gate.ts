// The ship gate for a heavily dressed level (detail layer round 8; the round-7 judge: "on any Melange build without [Fixes]
// MeshInstances the map crashes the client at load ... needs a guard: a minimum-Melange-version declaration"). Pure.
// The engine's mesh-instance pool holds 550 (lib/budget.ts); round 6 measured a Caldera of 520 details loading and 546
// crashing, so a level over VANILLA_SAFE_DETAILS needs a Melange that raises the pool. Such a level's pack declares it in
// spice.json (melange.range), which every Melange since 0.2 checks before it offers the level: an older Melange refuses
// the pack instead of crashing in it.

/** The detail count a level may hold and still load on the vanilla 550-instance pool with the match's own meshes. */
export const VANILLA_SAFE_DETAILS = 450;
/** The first Melange release that raises the pool ([Fixes] MeshInstances). The fix is unreleased when this was written
 * (the tree is 0.13.0, tagged without it): set this to the version it ships in. */
export const MESH_POOL_MELANGE = "0.14.0";
export const DEFAULT_RANGE = ">=0.2.0";

/** The melange.range a pack of a level with `details` details should declare, and why. `asked` (a caller's own range)
 * wins when given. */
export function shipRange(details: number, asked?: string): { melangeRange: string; gated: boolean; why: string } {
  if (asked) return { melangeRange: asked, gated: asked !== DEFAULT_RANGE, why: "asked" };
  if (details > VANILLA_SAFE_DETAILS)
    return { melangeRange: `>=${MESH_POOL_MELANGE}`, gated: true,
      why: `${details} details: over the ${VANILLA_SAFE_DETAILS} the vanilla 550-instance mesh pool loads, so the pack needs Melange ${MESH_POOL_MELANGE}+ ([Fixes] MeshInstances)` };
  return { melangeRange: DEFAULT_RANGE, gated: false, why: `${details} details: within the vanilla mesh pool` };
}
