// Added terrain blocks: new frames under the Scene frame, identity rotation and scale, each side 1-32 voxels, at most 64.
// The engine centres a frame's voxels on its position, so a block's position is its centre; the corner is snapped to the
// Scene frame's whole units so its voxels line up with a 1-unit grid.
import { AddFrame, LIMITS, apply, frameWorld, type Frame, type Mat3x4, type Scene, type Vec3 } from "../../../sdk/erg";
import { filled, invert, transformBox, type Box } from "./voxel";

export const blockFrames = (s: Scene) => s.frames.filter((f) => f.new);

/** Changes whenever a block is added or removed (the view remeshes all frames, the tools rebuild their grids). */
export function blockKey(s: Scene): string {
  let key = "";
  for (let i = s.frames.length - 1; i >= 0 && s.frames[i].new; i--) key += `${s.frames[i].id}:${s.frames[i].voxels};`;
  return key;
}

export const sceneFrame = (s: Scene) => s.frames.find((f) => f.name === "Scene" && !f.new);

function sceneWorld(s: Scene): Mat3x4 | null {
  const sf = sceneFrame(s);
  return sf ? frameWorld(new Map(s.frames.map((f) => [f.id, f])), sf.id) : null;
}

/** ergblock_<n>, the lowest n no frame uses (names compare without case). */
export function blockName(s: Scene): string {
  const used = new Set(s.frames.map((f) => f.name.toLowerCase()));
  for (let n = 0; ; n++) if (!used.has(`ergblock_${n}`)) return `ergblock_${n}`;
}

/** The block's centre in the Scene frame's space for a point in the level: resting on the point, corner snapped. */
export function blockCentre(s: Scene, at: Vec3, size: Vec3): Vec3 | null {
  const w = sceneWorld(s), inv = w && invert(w);
  if (!inv) return null;
  const p = apply(inv, at);
  return [Math.round(p[0] - size[0] / 2) + size[0] / 2, Math.round(p[1]) + size[1] / 2, Math.round(p[2] - size[2] / 2) + size[2] / 2];
}

/** The level box a block centred at `centre` covers, for the cursor. */
export function blockBox(s: Scene, centre: Vec3, size: Vec3): Box | null {
  const w = sceneWorld(s);
  return w && transformBox(w, { min: centre.map((c, i) => c - size[i] / 2) as Vec3, max: centre.map((c, i) => c + size[i] / 2) as Vec3 });
}

/** A block of `size` filled with `material`, centred at `centre` (Scene frame space), or why it cannot be added. */
export function planBlock(s: Scene, voxels: Map<number, Uint32Array>, ref: number, centre: Vec3, size: Vec3, material: number): AddFrame | string {
  const sf = sceneFrame(s);
  if (!sf) return "This level has no Scene frame to add blocks under";
  const blocks = blockFrames(s);
  if (blocks.length >= LIMITS.newFrames) return `A level holds at most ${LIMITS.newFrames} added blocks`;
  if (!size.every((v) => Number.isInteger(v) && v >= 1 && v <= LIMITS.newFrameSide)) return `Each side of a block is 1-${LIMITS.newFrameSide} voxels`;
  let tmp = -1;
  while (blocks.some((f) => f.id === tmp)) tmp--;
  const frame: Frame = { id: tmp, new: true, parent: sf.id, name: blockName(s), pos: [centre[0], centre[1], centre[2]], rot: [0, 0, 0],
    scale: [1, 1, 1], size: [size[0], size[1], size[2]], voxels: ref, heightMap: null, folder: false };
  return new AddFrame(voxels, frame, new Uint32Array(size[0] * size[1] * size[2]).fill(filled(material)));
}
