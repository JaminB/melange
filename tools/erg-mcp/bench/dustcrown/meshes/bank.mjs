// Dustcrown's ONE mesh bank (dustcrown.Rocks.xom), shared by build.mjs (the undressed map's boulder fields) and the detail
// layer (detail.mjs places, ship.mjs ships). Six meshes, the most a bank can hold on section 514 (build_rock_bank converts mesh
// i on section 514 + i and xomtool takes 476..519; the default 486 moves Kindjal's BlastKeg, Shiv and StoneDonkey banks, and a
// pack lists the bank as its only one: lint_mesh_banks). Boulder and Heap keep the names, kinds and order the bank had before
// the detail layer, so their seeds (the kind's own + the map seed + 101 per earlier rock of the kind) and shapes did not move.
//   Boulder, Heap  hull rocks and a heap of them, in a burnt-ochre sandstone: the fields, the mesas' feet, the palace's pilasters
//   Spike          a thin shard in dead-scrub grey-brown: thorn bushes and dry grass (round 1's straw read as the sand itself)
//   Dome           a SMOOTH onion dome (build_rock_bank's `lathe` kind: analytic normals, no stair-step, no seam) in weathered
//                  copper green: it caps the four corner towers and the Crown; the voxel domes under it are hidden, not removed
//   Slit           a dark block squashed flat: arrow slits, windows and the gate's shadow
//   Bone           a long bone, a lathe with knobbed ends (4.2 map units, bleached): laid on its side, a skeleton's litter
// Round 2 (2026-11-12) dropped round 1's gold Obelisk (the judge read the corner spikes as tan traffic cones, the gold as yellow
// plastic) and the straw palette. Palettes are the dunes' own family, desaturated: in the game the baked albedo draws cooler and
// darker than its bytes (round 4's [214, 192, 128] played an olive grey beside the dunes), so they sit warm and a little light.
export const MOD = "dustcrown";
export const SECTION = 514;
const SAND = { base: [232, 184, 126], dust: [248, 222, 172], dustShare: 0.35 };
export const ROCKS = [
  { name: "Boulder", kind: "boulder" },                                  // a chamfered block: the fields, the mesas' feet, the pilasters
  { name: "Heap", kind: "scree", scree: { rocks: 9 } },                  // a heap of small hulls: scree, a field's litter
  { name: "Spike", kind: "shard", palette: { base: [104, 82, 58], dust: [150, 126, 92], dustShare: 0.4 } },         // dead thorn scrub, dry grass
  { name: "Dome", kind: "lathe", lathe: { profile: [[0.9, 0], [1, 0.1], [1, 0.3], [0.96, 0.45], [0.84, 0.58], [0.64, 0.7], [0.4, 0.82], [0.18, 0.92], [0.06, 0.97], [0, 1]], radius: 112, height: 166, segments: 28 }, palette: { base: [112, 150, 128], dust: [166, 192, 164], dustShare: 0.45 } },
  { name: "Slit", kind: "boulder", palette: { base: [52, 38, 28], dust: [66, 50, 38], dustShare: 0.2 } },          // squashed flat: an arrow slit or a window
  { name: "Bone", kind: "lathe", lathe: { profile: [[0, 0], [0.55, 0.02], [0.78, 0.07], [0.6, 0.13], [0.34, 0.22], [0.27, 0.5], [0.34, 0.78], [0.6, 0.87], [0.78, 0.93], [0.55, 0.98], [0, 1]], radius: 16, height: 84, segments: 10 }, palette: { base: [255, 238, 200], dust: [255, 246, 218], dustShare: 0.5 } },   // a long bone, knobbed ends: it lies on its side
];
export const BANK = { id: MOD, palette: SAND, seed: 5, rocks: ROCKS, section: SECTION };
