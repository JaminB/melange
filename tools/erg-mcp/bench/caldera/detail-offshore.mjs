// Detail layer round 8: OFFSHORE consolidation (the round-7 judge: "in the oblique, the SW offshore stacks read at distance as
// a scatter of black debris drifting away from the island. Seven or eight small dark dots around the coast is noise; two or
// three bigger masses would compose better"). After the rock and coast passes, the rock copies standing in the sea (ground
// under them below the water) are grouped (pieces within 2.5 of each other); a group is kept only when it is a MASS (3 or
// more pieces with one at least 3 across, or one piece at least 5 across) or it touches the shore (dry land within 1.5 of a
// piece). Every other group, a small lone block or a pair drifting off the coast, is removed; and of two masses within 10
// of each other only the bigger stays (unless the smaller touches the shore): what is left offshore is a few big stacks with
// their talus, not a trail of dots.
const WATER = 1;
const SIZE = 2.05;   // the rock mesh (Horror5) is about 2.05 map units at scale 1

export async function offshorePass(ctx) {
  const { call } = ctx;
  const d = (await call("list_scenery", { search: "_dtl", units: "map", limit: 2000 })).json.details;
  const sample = async (pts) => {
    const res = [];
    for (let i = 0; i < pts.length; i += 3500) res.push(...(await call("surface_sample", { points: pts.slice(i, i + 3500), units: "map", step: 0.8 })).json.samples);
    return res;
  };
  const g = await sample(d.map((x) => [x.at[0], x.at[2]]));
  const sea = d.map((x, i) => ({ x, g: g[i], size: SIZE * Math.max(...(x.scale ?? [1, 1, 1]).map(Math.abs)) })).filter((p) => p.g && p.g.y < WATER - 0.3);
  // groups: union-find over pieces within 2.5 (xz)
  const parent = sea.map((_, i) => i), find = (i) => (parent[i] === i ? i : (parent[i] = find(parent[i])));
  for (let i = 0; i < sea.length; i++) for (let j = i + 1; j < sea.length; j++)
    if (Math.hypot(sea[i].x.at[0] - sea[j].x.at[0], sea[i].x.at[2] - sea[j].x.at[2]) < 2.5 + 0.3 * (sea[i].size + sea[j].size)) parent[find(i)] = find(j);
  const groups = new Map();
  sea.forEach((p, i) => { const r = find(i); if (!groups.has(r)) groups.set(r, []); groups.get(r).push(p); });
  // dry land within 3.5 of any piece: 8 points round each
  const ring = [];
  for (const p of sea) for (let k = 0; k < 8; k++) { const a = (k / 8) * Math.PI * 2; ring.push([p.x.at[0] + Math.cos(a) * 1.5, p.x.at[2] + Math.sin(a) * 1.5]); }
  const rs = await sample(ring);
  const nearShore = new Set();
  sea.forEach((p, i) => { if (rs.slice(i * 8, i * 8 + 8).some((s) => s && s.y > WATER)) nearShore.add(p); });
  const remove = [], kept = [];
  const info = [...groups.values()].map((grp) => {
    const big = Math.max(...grp.map((p) => p.size));
    const cx = grp.reduce((a, p) => a + p.x.at[0], 0) / grp.length, cz = grp.reduce((a, p) => a + p.x.at[2], 0) / grp.length;
    return { grp, cx, cz, weight: grp.reduce((a, p) => a + p.size * p.size, 0), mass: (grp.length >= 3 && big >= 3) || big >= 5, shore: grp.some((p) => nearShore.has(p)) };
  }).sort((a, b) => b.weight - a.weight);
  const masses = [];
  for (const g of info) {
    const near = masses.find((m) => Math.hypot(m.cx - g.cx, m.cz - g.cz) < 10);
    // round 9: a small group touching the shore is a speck too unless it has 3 pieces (a stepped causeway)
    // round 10 (the round-9 judge: "delete the offshore black specks visible at the oblique"): a mass off the shore stays
    // only when it is a big stack (a piece 8 or more across: the headland the judge liked), not a mid-size lump
    const big = Math.max(...g.grp.map((p) => p.size));
    if ((g.mass && !near && (g.shore || big >= 8)) || (g.shore && g.grp.length >= 3)) { if (g.mass) masses.push(g); kept.push({ n: g.grp.length, at: [+g.cx.toFixed(1), +g.cz.toFixed(1)], mass: g.mass, shore: g.shore }); }
    else remove.push(...g.grp);
  }
  if (remove.length) await call("remove_scenery", { ids: remove.map((p) => p.x.id) });
  ctx.say?.("offshore", JSON.stringify({ seaPieces: sea.length, groups: groups.size, kept: kept.length, removedPieces: remove.length }));
  return { seaPieces: sea.length, groups: groups.size, keptGroups: kept.length, removedPieces: remove.length, removed: remove.map((p) => [p.x.id, p.x.at.map((v) => +v.toFixed(1)), +p.size.toFixed(1)]), kept };
}
