import assert from "node:assert/strict";
import { test } from "node:test";
import type { Candidate, GameCheck, Recommended } from "../../src/launcher/api";
import { canGoBack, findActions, initialWizard, wizardReducer, type WizardState } from "../../src/launcher/state";

const check = (path: string, verdict: GameCheck["verdict"] = "ok"): GameCheck => ({ path, verdict, store: "steam", running: false, writable: true });
const candidate = (path: string, verdict: GameCheck["verdict"] = "ok"): Candidate => ({ path, source: "steam", check: check(path, verdict) });

test("initial state starts at welcome", () => {
  const s = initialWizard();
  assert.equal(s.step, "welcome");
  assert.equal(s.candidates.length, 0);
});

test("start moves to find", () => {
  const s = wizardReducer(initialWizard(), { type: "start" });
  assert.equal(s.step, "find");
});

test("candidates preselects the first ok verdict, not just the first row", () => {
  const s = wizardReducer(initialWizard(), { type: "candidates", candidates: [candidate("a", "wrongBuild"), candidate("b", "ok")] });
  assert.equal(s.selected, "b");
});

test("candidates falls back to the first row when none are ok", () => {
  const s = wizardReducer(initialWizard(), { type: "candidates", candidates: [candidate("a", "wrongBuild")] });
  assert.equal(s.selected, "a");
});

test("select and childHint/useChild", () => {
  let s = wizardReducer(initialWizard(), { type: "select", path: "x" });
  assert.equal(s.selected, "x");
  s = wizardReducer(s, { type: "childHint", parent: "C:\\common", child: "C:\\common\\WormsXHD" });
  assert.equal(s.childHint?.child, "C:\\common\\WormsXHD");
  s = wizardReducer(s, { type: "useChild" });
  assert.equal(s.selected, "C:\\common\\WormsXHD");
  assert.equal(s.childHint, undefined);
});

test("check sets the step to check and stores the result", () => {
  const s = wizardReducer(initialWizard(), { type: "check", check: check("a", "wrongBuild") });
  assert.equal(s.step, "check");
  assert.equal(s.check?.verdict, "wrongBuild");
});

test("toInstall only proceeds when the check verdict is ok", () => {
  const base: WizardState = { ...initialWizard(), step: "check", check: check("a", "wrongBuild") };
  assert.equal(wizardReducer(base, { type: "toInstall" }).step, "check");
  const ok = { ...base, check: check("a", "ok") };
  assert.equal(wizardReducer(ok, { type: "toInstall" }).step, "install");
});

test("back walks the fixed chain and stops at welcome", () => {
  let s: WizardState = { ...initialWizard(), step: "ready" };
  for (const expect of ["recommended", "install", "check", "find"]) {
    s = wizardReducer(s, { type: "back" });
    assert.equal(s.step, expect);
  }
  assert.equal(wizardReducer(s, { type: "back" }).step, "welcome"); // find -> welcome
  assert.equal(canGoBack("welcome"), false);
  assert.equal(canGoBack("ready"), true);
});

test("applying/applyError/applyDone", () => {
  let s = wizardReducer(initialWizard(), { type: "applying" });
  assert.equal(s.applying, true);
  s = wizardReducer(s, { type: "applyError", code: -32010 });
  assert.equal(s.applying, false);
  assert.equal(s.applyError?.code, -32010);
  s = wizardReducer(s, { type: "applying" });
  const status = { game: null, running: false, melangeLoaded: false, loader: { state: "none" as const }, otherLoaders: [],
    melange: { state: "installed" as const, duplicates: [] }, ini: { present: true, missingKeys: 0 }, legacy: [],
    payload: { ok: true, version: "0.4.0", missing: [], fromGameFolder: false }, backups: [] };
  s = wizardReducer(s, { type: "applyDone", status });
  assert.equal(s.step, "recommended");
  assert.equal(s.applyError, undefined);
  assert.equal(s.status, status);
});

test("recommended seeds choices for compatible, not-yet-installed items only", () => {
  const items: Recommended[] = [
    { id: "sunstone", name: "Sunstone", description: "", why: "", settings: { quality: "bold" }, installed: false, compatible: true },
    { id: "installed-one", name: "x", description: "", why: "", settings: {}, installed: true, compatible: true },
    { id: "incompatible-one", name: "y", description: "", why: "", settings: {}, installed: false, compatible: false },
  ];
  const s = wizardReducer(initialWizard(), { type: "recommended", source: "index", items });
  assert.deepEqual(Object.keys(s.choices), ["sunstone"]);
  assert.equal(s.choices.sunstone.enabled, true);
});

test("toggleChoice and setChoiceSetting", () => {
  let s = wizardReducer(initialWizard(), { type: "toggleChoice", id: "sunstone", enabled: true });
  assert.equal(s.choices.sunstone.enabled, true);
  s = wizardReducer(s, { type: "setChoiceSetting", id: "sunstone", key: "quality", value: "ultra" });
  assert.equal(s.choices.sunstone.settings.quality, "ultra");
  s = wizardReducer(s, { type: "toggleChoice", id: "sunstone", enabled: false });
  assert.equal(s.choices.sunstone.enabled, false);
  assert.equal(s.choices.sunstone.settings.quality, "ultra", "turning a plugin off keeps its chosen settings");
});

test("findActions: browse is primary only with no candidates", () => {
  assert.deepEqual(findActions([]), { primary: "browse" });
  assert.deepEqual(findActions([candidate("a")]), { primary: "continue", secondary: "browse" });
});

test("shortcut and error", () => {
  let s = wizardReducer(initialWizard(), { type: "shortcut", which: "desktop", value: true });
  assert.equal(s.shortcuts.desktop, true);
  assert.equal(s.shortcuts.startMenu, true, "default stays unless the action names it");
  s = wizardReducer({ ...s, checking: true, applying: true }, { type: "error", message: "oops" });
  assert.equal(s.checking, false);
  assert.equal(s.applying, false);
  assert.equal(s.error, "oops");
});
