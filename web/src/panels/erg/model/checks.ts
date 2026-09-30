// The validation list: missing or doubled spawn knots, objects under water, details outside every frame, and anything
// the patch validator refuses.
import { WORLD_PER_XAN, type Scene } from "../../../sdk/erg";
import { overFrame, type Frames } from "./geometry";

export interface Issue { level: "error" | "warn"; text: string; detail?: number; }

export const KNOTS = Array.from({ length: 8 }, (_, i) => `WORM${i}`);
export const PLACED_OBJECTS = ["mine", "oildrum"];

export function checkScene(scene: Scene, frames: Frames, patchErrors: string[] = []): Issue[] {
  const out: Issue[] = [];
  const knots = new Map<string, number[]>();
  for (const d of scene.details) if (KNOTS.includes(d.name)) knots.set(d.name, [...(knots.get(d.name) ?? []), d.id]);
  if (scene.spawns.mode === "knots") {
    const missing = KNOTS.filter((k) => !knots.has(k));
    if (missing.length) out.push({ level: "error", text: `Spawn mode is knots but ${missing.join(", ")} ${missing.length > 1 ? "are" : "is"} missing` });
  }
  for (const [name, ids] of knots)
    if (ids.length > 1) out.push({ level: "warn", text: `${ids.length} details are named ${name}; the game uses one of them`, detail: ids[1] });

  const water = scene.water.level;
  const terrain = scene.frames.filter((f) => !f.folder && f.parent !== null && f.size[0] && f.size[2]);
  for (const d of scene.details) {
    const p = frames.detailWorld(d);
    if (water !== null && PLACED_OBJECTS.includes(d.name) && p[1] * WORLD_PER_XAN < water)
      out.push({ level: "warn", text: `${d.name} #${d.id} is under water (${(p[1] * WORLD_PER_XAN).toFixed(1)} < ${water})`, detail: d.id });
    if (terrain.length && !terrain.some((f) => overFrame(frames, f, p)))
      out.push({ level: "warn", text: `${d.name || d.resource} #${d.id} is outside every terrain frame`, detail: d.id });
  }
  for (const e of patchErrors) out.push({ level: "error", text: e });
  return out;
}
