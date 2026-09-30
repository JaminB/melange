// What the palette can place, and where a new detail goes: its frame and its position in that frame.
import { AddDetail, type Role, type Scene, type Vec3 } from "../../../sdk/erg";
import { snapVec, type Frames } from "./geometry";
import { KNOTS } from "./checks";

export interface PaletteEntry { id: string; label: string; name: string; resource: string; role: Role; preview?: string; }

// Duplicating or placing scenery ships only once a copied scenery detail is shown to render and collide in the game;
// until then the palette lists markers and objects only, and duplicate refuses scenery.
export const SCENERY_COPIES = false;

export const BUILTIN: PaletteEntry[] = [
  { id: "knot", label: "Spawn knot", name: "WORM", resource: "CheesyGrinWorm", role: "spawn" },
  { id: "oildrum", label: "Oil drum", name: "oildrum", resource: "OilDrum", role: "object" },
  { id: "mine", label: "Mine", name: "mine", resource: "Mine", role: "object" },
];

export function canCopy(role: Role): boolean {
  return SCENERY_COPIES || role !== "scenery";
}

/** Server palette entries (level.palette) merged over the built-ins; scenery only when copies are allowed. */
export function paletteFrom(server: unknown): PaletteEntry[] {
  const out = BUILTIN.map((e) => ({ ...e }));
  if (!Array.isArray(server)) return out;
  for (const x of server) {
    if (!x || typeof x !== "object") continue;
    const { name, resource, role, preview } = x as Record<string, unknown>;
    if (typeof name !== "string" || typeof resource !== "string" || typeof role !== "string") continue;
    const known = out.find((e) => e.name.toLowerCase() === name.toLowerCase() || (e.id === "knot" && role === "spawn"));
    if (known) {
      if (typeof preview === "string" && preview) known.preview = preview;
      continue;
    }
    if (!canCopy(role as Role) || role === "spawn" || role === "object") continue;
    out.push({ id: `p:${name}:${resource}`, label: resource || name, name, resource, role: role as Role,
      preview: typeof preview === "string" && preview ? preview : undefined });
  }
  return out;
}

export function nextKnot(scene: Scene): string | null {
  const have = new Set(scene.details.map((d) => d.name));
  return KNOTS.find((k) => !have.has(k)) ?? null;
}

const FOLDER_HINTS: Partial<Record<Role, RegExp>> = { spawn: /worm|spawn/i, object: /barrel|mine|drum|object|crate/i };

/** The frame a new detail of this role goes into: a folder already holding that role, a folder named for it, any
 * folder, then the root. */
export function targetFrame(scene: Scene, role: Role): number {
  const folders = scene.frames.filter((f) => f.folder);
  const holding = new Map<number, number>();
  for (const d of scene.details) if (d.role === role) holding.set(d.frame, (holding.get(d.frame) ?? 0) + 1);
  const best = folders.filter((f) => holding.has(f.id)).sort((a, b) => holding.get(b.id)! - holding.get(a.id)!)[0];
  if (best) return best.id;
  const hint = FOLDER_HINTS[role];
  const named = hint && folders.find((f) => hint.test(f.name));
  if (named) return named.id;
  if (folders[0]) return folders[0].id;
  return (scene.frames.find((f) => f.parent === null) ?? scene.frames[0]).id;
}

/** The add command for a palette entry at a level position, or a reason it cannot be placed. */
export function place(scene: Scene, frames: Frames, e: PaletteEntry, world: Vec3, step: number | null): AddDetail | string {
  let name = e.name;
  if (e.id === "knot") {
    const k = nextKnot(scene);
    if (!k) return "all eight spawn knots (WORM0-WORM7) already exist";
    name = k;
  }
  if (!canCopy(e.role)) return "scenery cannot be placed in this version";
  const frame = targetFrame(scene, e.role);
  const pos = snapVec(frames.toLocal(frame, world), step);
  return new AddDetail(frame, { name, resource: e.resource, pos });
}
