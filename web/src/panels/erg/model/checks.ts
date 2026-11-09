// The validation list: missing or doubled spawn knots, objects under water, details outside every frame, and anything
// the patch validator refuses.
import { WORLD_PER_XAN, type Scene } from "../../../sdk/erg";
import { overFrame, type Frames } from "./geometry";

export interface Issue { level: "error" | "warn"; text: string; detail?: number; }

export const KNOTS = Array.from({ length: 8 }, (_, i) => `WORM${i}`);
/** spawns.mode teams: team t's worm p stands on WORM<4t + p>. */
export const TEAM_KNOTS = Array.from({ length: 16 }, (_, i) => `WORM${i}`);
export const PLACED_OBJECTS = ["mine", "oildrum"];

export function checkScene(scene: Scene, frames: Frames, patchErrors: string[] = []): Issue[] {
  const out: Issue[] = [];
  const knots = new Map<string, number[]>();
  const named = scene.spawns.mode === "teams" ? TEAM_KNOTS : KNOTS;
  for (const d of scene.details) if (named.includes(d.name)) knots.set(d.name, [...(knots.get(d.name) ?? []), d.id]);
  if (scene.spawns.mode === "knots") {
    const missing = KNOTS.filter((k) => !knots.has(k));
    if (missing.length) out.push({ level: "error", text: `Spawn mode is knots but ${missing.join(", ")} ${missing.length > 1 ? "are" : "is"} missing` });
  }
  if (scene.spawns.mode === "teams") {
    // a 2-team game uses WORM0-7, four teams WORM0-15: the first eight are needed, the rest warned of
    const missing = TEAM_KNOTS.filter((k) => !knots.has(k));
    const needed = missing.filter((k) => KNOTS.includes(k)), more = missing.filter((k) => !KNOTS.includes(k));
    if (needed.length) out.push({ level: "error", text: `Spawn mode is teams but ${needed.join(", ")} ${needed.length > 1 ? "are" : "is"} missing` });
    if (more.length) out.push({ level: "warn", text: `Spawn mode is teams but ${more.join(", ")} ${more.length > 1 ? "are" : "is"} missing: a third or fourth team's worms have no knot` });
  }
  for (const [name, ids] of knots)
    if (ids.length > 1) out.push({ level: "warn", text: `${ids.length} details are named ${name}; the game uses one of them`, detail: ids[1] });

  const objects = scene.objects ?? [];
  const added = new Map(scene.details.filter((d) => d.src === null).map((d) => [d.name, d.id]));
  const pads = new Map<number, string[]>();
  for (const o of objects) if (o.type === "telepad") pads.set(o.group, [...(pads.get(o.group) ?? []), o.knot]);
  for (const [g, knots] of pads)
    if (knots.length === 1) out.push({ level: "warn", text: `Telepad group ${g} has one pad (${knots[0]}); it needs a partner`, detail: added.get(knots[0]) });
  const crates = new Set(objects.filter((o) => o.type === "crate").map((o) => o.knot));

  const water = scene.water.level;
  const terrain = scene.frames.filter((f) => !f.folder && f.parent !== null && f.size[0] && f.size[2]);
  for (const d of scene.details) {
    const p = frames.detailWorld(d);
    const placed = PLACED_OBJECTS.includes(d.name) || (d.src === null && crates.has(d.name));
    if (water !== null && placed && p[1] * WORLD_PER_XAN < water)
      out.push({ level: "warn", text: `${d.name} #${d.id} is under water (${(p[1] * WORLD_PER_XAN).toFixed(1)} < ${water})`, detail: d.id });
    if (terrain.length && !terrain.some((f) => overFrame(frames, f, p)))
      out.push({ level: "warn", text: `${d.name || d.resource} #${d.id} is outside every terrain frame`, detail: d.id });
  }
  for (const e of patchErrors) out.push({ level: "error", text: e });
  return out;
}
