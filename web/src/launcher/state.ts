// The first-run wizard as a pure reducer: every RPC result becomes an action, the reducer only computes what step
// shows and what the step needs, with no side effects. Components dispatch, then make the next RPC call themselves.
import type { Candidate, GameCheck, Plan, Recommended, SetupStatus, Val, Verdict } from "./api";

export type WizardStep = "welcome" | "find" | "check" | "install" | "recommended" | "ready";
export const STEPS: WizardStep[] = ["welcome", "find", "check", "install", "recommended", "ready"];
export const STEP_LABEL: Record<WizardStep, string> = {
  welcome: "Welcome", find: "Find your game", check: "Check the game", install: "Install Melange",
  recommended: "Recommended plugins", ready: "Ready",
};

export interface PluginChoice { enabled: boolean; settings: Record<string, Val>; }

export interface WizardState {
  step: WizardStep;
  candidates: Candidate[];
  selected?: string;
  childHint?: { parent: string; child: string };
  checking: boolean;
  check?: GameCheck;
  status?: SetupStatus;
  plan?: Plan;
  replaceLoader: boolean;
  applying: boolean;
  applyError?: { code: number; data?: unknown };
  recommended?: { source: "index" | "builtin"; items: Recommended[] };
  recommendedUnreachable: boolean;
  choices: Record<string, PluginChoice>;
  shortcuts: { startMenu: boolean; desktop: boolean };
  error?: string;
}

export function initialWizard(): WizardState {
  return { step: "welcome", candidates: [], checking: false, replaceLoader: false, applying: false,
    recommendedUnreachable: false, choices: {}, shortcuts: { startMenu: true, desktop: false } };
}

export type WizardAction =
  | { type: "start" }
  | { type: "candidates"; candidates: Candidate[] }
  | { type: "select"; path: string }
  | { type: "childHint"; parent: string; child: string }
  | { type: "useChild" }
  | { type: "browseAgain" }
  | { type: "checking" }
  | { type: "check"; check: GameCheck }
  | { type: "toInstall" }
  | { type: "back" }
  | { type: "goto"; step: WizardStep }
  | { type: "status"; status: SetupStatus }
  | { type: "plan"; plan: Plan }
  | { type: "replaceLoader"; value: boolean }
  | { type: "applying" }
  | { type: "applyError"; code: number; data?: unknown }
  | { type: "applyDone"; status: SetupStatus }
  | { type: "recommended"; source: "index" | "builtin"; items: Recommended[] }
  | { type: "recommendedUnreachable" }
  | { type: "toggleChoice"; id: string; enabled: boolean }
  | { type: "setChoiceSetting"; id: string; key: string; value: Val }
  | { type: "toReady" }
  | { type: "shortcut"; which: "startMenu" | "desktop"; value: boolean }
  | { type: "error"; message?: string };

const BACK: Partial<Record<WizardStep, WizardStep>> = { find: "welcome", check: "find", install: "check", recommended: "install", ready: "recommended" };

export function wizardReducer(s: WizardState, a: WizardAction): WizardState {
  switch (a.type) {
    case "start":
      return { ...s, step: "find", error: undefined };
    case "candidates": {
      const ok = a.candidates.find((c) => c.check.verdict === "ok");
      return { ...s, candidates: a.candidates, selected: ok?.path ?? a.candidates[0]?.path, childHint: undefined, error: undefined };
    }
    case "select":
      return { ...s, selected: a.path, childHint: undefined };
    case "childHint":
      return { ...s, childHint: { parent: a.parent, child: a.child } };
    case "useChild":
      return s.childHint ? { ...s, selected: s.childHint.child, childHint: undefined } : s;
    case "browseAgain":
      return { ...s, childHint: undefined };
    case "checking":
      return { ...s, checking: true, error: undefined };
    case "check":
      return { ...s, checking: false, check: a.check, step: "check" };
    case "toInstall":
      return s.check?.verdict === "ok" ? { ...s, step: "install" } : s;
    case "back": {
      const prev = BACK[s.step];
      return prev ? { ...s, step: prev, error: undefined, applyError: undefined } : s;
    }
    case "goto":
      return { ...s, step: a.step, error: undefined, applyError: undefined };
    case "status":
      return { ...s, status: a.status, replaceLoader: s.replaceLoader };
    case "plan":
      return { ...s, plan: a.plan };
    case "replaceLoader":
      return { ...s, replaceLoader: a.value };
    case "applying":
      return { ...s, applying: true, applyError: undefined };
    case "applyError":
      return { ...s, applying: false, applyError: { code: a.code, data: a.data } };
    case "applyDone":
      return { ...s, applying: false, applyError: undefined, status: a.status, step: "recommended" };
    case "recommended": {
      const choices: Record<string, PluginChoice> = {};
      for (const it of a.items) if (it.compatible && !it.installed) choices[it.id] = { enabled: true, settings: { ...it.settings } };
      return { ...s, recommended: { source: a.source, items: a.items }, recommendedUnreachable: false, choices };
    }
    case "recommendedUnreachable":
      return { ...s, recommendedUnreachable: true, recommended: { source: "builtin", items: [] } };
    case "toggleChoice":
      return { ...s, choices: { ...s.choices, [a.id]: { ...(s.choices[a.id] ?? { enabled: a.enabled, settings: {} }), enabled: a.enabled } } };
    case "setChoiceSetting": {
      const cur = s.choices[a.id] ?? { enabled: true, settings: {} };
      return { ...s, choices: { ...s.choices, [a.id]: { ...cur, settings: { ...cur.settings, [a.key]: a.value } } } };
    }
    case "toReady":
      return { ...s, step: "ready" };
    case "shortcut":
      return { ...s, shortcuts: { ...s.shortcuts, [a.which]: a.value } };
    case "error":
      return { ...s, checking: false, applying: false, error: a.message };
    default:
      return s;
  }
}

// Which primary/secondary actions the Find-your-game step offers, from the candidate list alone.
export function findActions(candidates: Candidate[]): { primary: "continue" | "browse"; secondary?: "browse" } {
  return candidates.length ? { primary: "continue", secondary: "browse" } : { primary: "browse" };
}

export function canGoBack(step: WizardStep): boolean {
  return step in BACK;
}

export const VERDICT_BLOCKS_INSTALL: Record<Verdict, boolean> = { ok: false, wrongBuild: true, noExe: true, notFound: true, unreadable: true };
