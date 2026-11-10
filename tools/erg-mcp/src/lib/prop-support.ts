// Ground support of a placed prop (detail layer round 8; the round-7 judge: "a group of 4-5 boulders sits mid-face on a
// near-vertical lava cliff, with no ledge beneath, no talus below and no contact shadow. It reads as floating", and the tool
// gap "no ground-contact/support query for a placed prop footprint"). Pure: a clump's drawn vertices and a ground function
// in, a verdict out. Map units.
//
// The clump's footprint (its xz box pulled in by `inset` of its size) is sampled on an n x n grid. At each sample the
// clump's UNDERSIDE is the lowest drawn vertex within `reach` of it in xz (a sample with no vertex near it is outside the
// drawn shape and does not count); the sample is in CONTACT when the ground there comes up to that underside (ground y >=
// underside - `tol`): the prop rests on or sinks into the ground at that point. `contact` is the share of counted samples
// in contact. A clump on ground steeper than `minSlope` (the steepest ground under its footprint) must have `minContact`
// of its footprint in contact, or another clump (talus, a ledge piece) under its downhill edge holding it up: one whose
// box reaches up to the clump's underside within its downhill half. Flat-ground clumps are not judged (supported: "flat").

export interface Ground { y: number; slope: number; normal?: number[] }
export interface SupportClump { ids: number[]; verts: number[]; min: [number, number, number]; max: [number, number, number] }
export interface SupportOpts { grid?: number; inset?: number; reach?: number; tol?: number; minSlope?: number; minContact?: number }
export interface SupportVerdict {
  ids: number[]; at: [number, number, number]; size: number; slope: number; samples: number; contact: number;
  supported: boolean; by: "flat" | "ground" | "talus" | null; holder?: number[];
}

/** The lowest vertex y within `reach` (xz) of (x, z), or null when no vertex is near. verts: [x, y, z, x, y, z, ...]. */
export function undersideAt(verts: number[], x: number, z: number, reach: number): number | null {
  let lo = Infinity;
  const r2 = reach * reach;
  for (let i = 0; i < verts.length; i += 3) {
    const dx = verts[i] - x, dz = verts[i + 2] - z;
    if (dx * dx + dz * dz <= r2 && verts[i + 1] < lo) lo = verts[i + 1];
  }
  return lo === Infinity ? null : lo;
}

/** Each clump's support verdict. `others` are every clump of the layer (a clump never holds itself up). */
export function supportOf(c: SupportClump, ground: (x: number, z: number) => Ground | null, others: SupportClump[], o: SupportOpts = {}): SupportVerdict {
  const n = Math.max(2, o.grid ?? 5), inset = o.inset ?? 0.12, tol = o.tol ?? 0.15, minSlope = o.minSlope ?? 55, minContact = o.minContact ?? 0.6;
  const sx = c.max[0] - c.min[0], sz = c.max[2] - c.min[2], size = Math.max(sx, sz);
  const reach = o.reach ?? Math.max(0.15, (0.6 * size) / n);
  const x0 = c.min[0] + sx * inset, x1 = c.max[0] - sx * inset, z0 = c.min[2] + sz * inset, z1 = c.max[2] - sz * inset;
  let counted = 0, touching = 0, steepest = 0, under = Infinity;
  for (let i = 0; i < n; i++) for (let k = 0; k < n; k++) {
    const x = x0 + ((x1 - x0) * i) / (n - 1), z = z0 + ((z1 - z0) * k) / (n - 1);
    const g = ground(x, z);
    if (!g) continue;
    steepest = Math.max(steepest, g.slope);
    const u = undersideAt(c.verts, x, z, reach);
    if (u === null) continue;
    counted++;
    under = Math.min(under, u);
    if (g.y >= u - tol) touching++;
  }
  const cx = (c.min[0] + c.max[0]) / 2, cz = (c.min[2] + c.max[2]) / 2;
  const contact = counted ? touching / counted : 0;
  const base = { ids: c.ids, at: [cx, c.min[1], cz] as [number, number, number], size, slope: steepest, samples: counted, contact };
  if (steepest < minSlope) return { ...base, supported: true, by: "flat" };
  if (counted && contact >= minContact) return { ...base, supported: true, by: "ground" };
  // talus: another clump whose box overlaps this one's downhill half in xz and whose top reaches the underside
  const g0 = ground(cx, cz), nx = g0?.normal?.[0] ?? 0, nz = g0?.normal?.[2] ?? 0, nl = Math.hypot(nx, nz);
  const dx = nl > 1e-6 ? nx / nl : 0, dz = nl > 1e-6 ? nz / nl : 0;   // downhill (the normal leans down the slope)
  const lowY = under === Infinity ? c.min[1] : under;
  for (const t of others) {
    if (t === c || t.ids.some((id) => c.ids.includes(id))) continue;
    if (t.max[0] < c.min[0] || t.min[0] > c.max[0] || t.max[2] < c.min[2] || t.min[2] > c.max[2]) continue;
    const tx = (t.min[0] + t.max[0]) / 2 - cx, tz = (t.min[2] + t.max[2]) / 2 - cz;
    if (nl > 1e-6 && tx * dx + tz * dz < 0) continue;   // not on the downhill side
    if (t.max[1] >= lowY - tol && t.min[1] < lowY) return { ...base, supported: true, by: "talus", holder: t.ids };
  }
  return { ...base, supported: false, by: null };
}
