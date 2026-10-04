// Pure helpers for the map browser (spec §12.2 "Map browser"): filtering, the derived filter option lists (built
// from the maps themselves, since the RPC `Importer` type carries no separate category/group list — §10.2) and the
// bulk show/hide targets. No RPC calls here; Import.tsx and MapBrowser.tsx own those.
import type { ImportMap } from "../api";

export type ShownFilter = "all" | "shown" | "hidden";
export interface MapFilters { query: string; group: string; category: string; shown: ShownFilter; }
export const DEFAULT_FILTERS: MapFilters = { query: "", group: "", category: "", shown: "all" };

export interface Option { value: string; label: string; }

// First-seen order (the order the server listed the maps in, itself category-then-name per the recipe) rather than
// alphabetical, so "All" is followed by the groups/categories in a stable, recipe-driven order.
function options(maps: ImportMap[], value: (m: ImportMap) => string, label: (m: ImportMap) => string): Option[] {
  const seen = new Map<string, string>();
  for (const m of maps) if (!seen.has(value(m))) seen.set(value(m), label(m));
  return [...seen.entries()].map(([v, l]) => ({ value: v, label: l }));
}
export const groupOptions = (maps: ImportMap[]): Option[] => options(maps, (m) => m.group, (m) => m.groupLabel || m.group);
export const categoryOptions = (maps: ImportMap[]): Option[] => options(maps, (m) => m.category, (m) => m.categoryLabel || m.category);

export function filterMaps(maps: ImportMap[], f: MapFilters): ImportMap[] {
  const q = f.query.trim().toLowerCase();
  return maps.filter((m) => {
    if (q && !(m.title.toLowerCase().includes(q) || m.file.toLowerCase().includes(q) || (m.author ?? "").toLowerCase().includes(q))) return false;
    if (f.group && m.group !== f.group) return false;
    if (f.category && m.category !== f.category) return false;
    if (f.shown === "shown" && m.hidden) return false;
    if (f.shown === "hidden" && !m.hidden) return false;
    return true;
  });
}

export function sortedByTitle(maps: ImportMap[]): ImportMap[] {
  return [...maps].sort((a, b) => a.title.localeCompare(b.title));
}

// The `file` names (not stems: §6.3 keeps hidden choices by original file name so they survive a re-import that
// renames stems) of every filtered map not already in the target state — what `import.setHidden` should touch.
export function bulkHideTargets(filtered: ImportMap[], hidden: boolean): string[] {
  return filtered.filter((m) => m.hidden !== hidden).map((m) => m.file);
}
export function countToShow(filtered: ImportMap[]): number {
  return filtered.filter((m) => m.hidden).length;
}
export function countToHide(filtered: ImportMap[]): number {
  return filtered.filter((m) => !m.hidden).length;
}

export function timeOfDayLabel(tod: ImportMap["timeOfDay"]): string {
  return tod === "DAY" ? "Day" : tod === "EVENING" ? "Evening" : "Night";
}

// The badge on a row: a dropped mode names it, otherwise deathmatch-only maps are flagged, play-as-designed maps
// get no badge at all.
export function playBadge(m: ImportMap): string | undefined {
  if (m.category === "mode") return `${m.mode ? `${m.mode}: ` : ""}mode not supported`;
  if (m.category === "dm") return "Deathmatch only";
  return undefined;
}
