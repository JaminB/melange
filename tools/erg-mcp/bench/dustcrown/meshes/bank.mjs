// Dustcrown's ONE mesh bank (dustcrown.Rocks.xom), shared by build.mjs (the undressed map's boulder fields) and the detail
// layer (detail.mjs places, ship.mjs ships). Every mesh is a build_rock_bank rock (src/lib/rockmesh.ts): a faceted hull with
// its albedo baked into a 128 x 128 texture, so it draws the same with or without a tint shader. The first two keep the
// names, kinds and order the bank had before the detail layer, so their seeds (the kind's own + the map seed + 101 per
// earlier rock of the kind) and shapes did not move. The bank is built on section 514 (lint_mesh_banks: the default 486
// moves Kindjal's BlastKeg, Shiv and StoneDonkey banks), and a pack lists it as its only bank.
//
// The palettes are the dunes' own family, a little dusted. In the game the baked albedo draws cooler and darker than its
// bytes: round 4's [214, 192, 128] played an olive grey beside the dunes, a green-over-red ratio of 0.8 played a pink brown
// (the first Hoodoo, the first Spike), so the stones sit near 0.8 with a yellower blue. The gold Obelisk finials of the palace and the dark Slit
// of its arrow slits are the two accents. Hoodoos are built from two Boulders (a stem stretched tall and a flat cap) and the
// Heap, not from a stretched shard: a shard's hull is lopsided and stretched it reads as a plank. At most SIX meshes:
// build_rock_bank converts mesh i on section 514 + i and xomtool takes 476..519 (a seventh fails the build). Round 1 of the
// detail layer left out Slab, Wedge and Strata: the Caldera notes found slabs read as plinth blocks, wedges as pink slabs and
// strata as ledges to stand on.
export const MOD = "dustcrown";
export const SECTION = 514;
const SAND = { base: [228, 184, 116], dust: [242, 218, 164], dustShare: 0.3 };
export const ROCKS = [
  { name: "Boulder", kind: "boulder" },                                  // a chamfered block: the fields, the mesas' feet
  { name: "Heap", kind: "scree", scree: { rocks: 9 } },                  // a heap of small hulls: scree, a field's litter
  { name: "Obelisk", kind: "shard", palette: { base: [206, 138, 28], dust: [226, 172, 56], dustShare: 0.1 } },       // stretched thin: the palace's gold finials
  { name: "Spike", kind: "shard", palette: { base: [214, 178, 90], dust: [236, 208, 128], dustShare: 0.5 } },         // stretched thin, a handful to a clump: dry grass and yucca
  { name: "Slit", kind: "boulder", palette: { base: [58, 42, 30], dust: [74, 56, 42], dustShare: 0.2 } },          // squashed flat: an arrow slit or a window
];
export const BANK = { id: MOD, palette: SAND, seed: 5, rocks: ROCKS, section: SECTION };
