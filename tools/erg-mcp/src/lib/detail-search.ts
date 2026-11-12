// One matcher for the lints that pick a decoration layer's props by a name fragment (lint_support, lint_contact). They
// had two: lint_support took a list and read the copy's name only, lint_contact one string and read the name and the
// resource, so "Rock" found a bank's boulders in one and nothing in the other (Dustcrown round 5). Here a search is a
// fragment or a list of them, and a copy matches when its name OR its resource contains any of them, whatever the case.
import type { Schema } from "../registry";

export type Search = string | string[];

/** The fragments of a search (default `fallback`), lower case; an empty one is dropped. */
export function searchTerms(search: Search | undefined, fallback: string[] = ["_dtl"]): string[] {
  const list = search === undefined ? fallback : Array.isArray(search) ? search : [search];
  return list.map((q) => String(q).toLowerCase()).filter((q) => q.length > 0);
}

/** Whether a scenery copy is one the search names: by its name or by its resource (the mesh it draws). */
export function matchesSearch(d: { name: string; resource?: string | null }, terms: string[]): boolean {
  const name = d.name.toLowerCase(), res = (d.resource ?? "").toLowerCase();
  return terms.some((q) => name.includes(q) || res.includes(q));
}

/** The schema both lints share for `search`. */
export const searchSchema: Schema = { anyOf: [{ type: "string" }, { type: "array", items: { type: "string" }, minItems: 1, maxItems: 8 }],
  description: "A fragment, or a list of them: the scenery copies whose NAME or RESOURCE (the mesh drawn) contains any of them, whatever the case (default \"_dtl\"). lint_support and lint_contact read it the same way." };
