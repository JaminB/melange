// Every panel registers itself; the shell lazy-loads it on first open.
import type { Client, Welcome } from "./client";

export type Need = "game" | "match";

export interface PanelModule { mount(el: HTMLElement, c: Client): () => void; }

export interface PanelDef {
  id: string; title: string; order: number;
  needs: Need[];                                 // greyed out with a reason when unmet
  load: () => Promise<PanelModule>;
}

const registry = new Map<string, PanelDef>();
const listeners = new Set<() => void>();

export function registerPanel(p: PanelDef): void {
  if (!/^[a-z0-9][a-z0-9._-]{0,47}$/.test(p.id)) throw new Error(`bad panel id '${p.id}'`);
  registry.set(p.id, p);
  for (const fn of [...listeners]) fn();
}

export function panels(): PanelDef[] {
  return [...registry.values()].sort((a, b) => a.order - b.order || (a.id < b.id ? -1 : 1));
}

export function onPanelsChanged(fn: () => void): () => void {
  listeners.add(fn);
  return () => listeners.delete(fn);
}

// Why a panel cannot be used right now, or undefined when it can.
export function unmetReason(p: PanelDef, welcome: Welcome | undefined, inMatch: boolean | undefined): string | undefined {
  for (const n of p.needs) {
    if (n === "game" && welcome && welcome.server !== "game") return "needs the game running";
    if (n === "match" && inMatch !== true) return "needs a match in progress";
  }
  return undefined;
}
