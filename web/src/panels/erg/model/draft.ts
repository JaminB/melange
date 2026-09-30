// Unsaved edits survive a page reload as a draft in this browser's localStorage, per project. The server's project
// stays the source of truth: a draft is offered only while its base and the saved state it started from still match.
import type { Patch } from "../../../sdk/erg";

export interface Draft { v: 1; base: string; saved: string; patch: Patch; at: number; }

const key = (project: string) => `oasis.erg.draft.${project}`;

function storage(): Storage | undefined {
  try {
    return window.localStorage;
  } catch {
    return undefined;
  }
}

export function writeDraft(project: string, d: Omit<Draft, "v" | "at">, store = storage()) {
  try {
    store?.setItem(key(project), JSON.stringify({ v: 1, at: Date.now(), ...d }));
  } catch {
    // storage full or blocked: drafts are a convenience only
  }
}

export function clearDraft(project: string, store = storage()) {
  try {
    store?.removeItem(key(project));
  } catch {
    // ignore
  }
}

/** The draft for a project if it applies to this base and this saved state. */
export function readDraft(project: string, baseSha: string, saved: string, store = storage()): Draft | undefined {
  let d: Draft;
  try {
    const text = store?.getItem(key(project));
    if (!text) return undefined;
    d = JSON.parse(text);
  } catch {
    return undefined;
  }
  if (!d || d.v !== 1 || d.base !== baseSha || d.saved !== saved || !d.patch || JSON.stringify(d.patch) === saved) return undefined;
  return d;
}

const LAST = "oasis.erg.last";
export function lastProject(store = storage()): string | undefined {
  try {
    return store?.getItem(LAST) ?? undefined;
  } catch {
    return undefined;
  }
}
export function setLastProject(id: string | undefined, store = storage()) {
  try {
    if (id) store?.setItem(LAST, id);
    else store?.removeItem(LAST);
  } catch {
    // ignore
  }
}
