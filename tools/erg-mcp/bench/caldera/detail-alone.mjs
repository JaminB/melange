// Detail layer round 10: NO LONE SPRITES (the round-9 judge: "on the Seawall beach, single fern sprites sprinkled on open sand
// read as debris and no longer as planting. Either clump them or delete them"). After the flora and the lints (which remove
// members and can leave a cluster's satellite on its own), every plant below ALONE_Y with no other plant within ALONE_R of
// it is removed: a plant reads as planting only in a clump. Up on the dry slopes a lone agave or succulent is the hardy
// look the focus asks for, so they stay.
export const ALONE_R = 1.6, ALONE_Y = 14;

export async function alonePass(ctx) {
  const { call } = ctx;
  const all = (await call("list_scenery", { search: "_flo", units: "map", limit: 2000 })).json.details;
  const lone = all.filter((d) => d.at[1] < ALONE_Y && !all.some((o) => o !== d && Math.hypot(o.at[0] - d.at[0], o.at[2] - d.at[2]) < ALONE_R));
  if (lone.length) await call("remove_scenery", { ids: lone.map((d) => d.id) });
  const shore = lone.filter((d) => d.at[1] < 4).length;
  return { plants: all.length, removed: lone.length, onTheShore: shore, first: lone.slice(0, 12).map((d) => [d.id, d.resource, d.at.map((v) => +v.toFixed(1))]) };
}
