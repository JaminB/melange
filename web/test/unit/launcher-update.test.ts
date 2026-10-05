import assert from "node:assert/strict";
import { test } from "node:test";
import type { UpdatePhase, UpdateStatus } from "../../src/launcher/api";
import { updateEventOf, updateStatusOf } from "../../src/launcher/api";
import { UPDATE_ACTION, UPDATE_GAME_RUNNING, updateAppliedLine, updateBanner, updateCheckLine } from "../../src/launcher/copy";

const PHASES: UpdatePhase[] = ["idle", "checking", "downloading", "ready", "current", "error"];
const status = (phase: UpdatePhase, extra: Partial<UpdateStatus> = {}): UpdateStatus => ({ current: "0.3.6", phase, latest: "0.3.7", ...extra });

test("updateStatusOf: garbage never throws and falls back to idle", () => {
  assert.deepEqual(updateStatusOf(null), {
    current: "", phase: "idle", latest: undefined, htmlUrl: undefined, progress: undefined, error: undefined, lastCheck: undefined, applied: undefined, auto: undefined,
  });
  assert.equal(updateStatusOf({ phase: "exploded" }).phase, "idle");
});

test("updateStatusOf: round-trips a full status", () => {
  const u = updateStatusOf({
    current: "0.3.6", phase: "downloading", latest: "0.3.7", htmlUrl: "https://github.com/JaminB/melange/releases/tag/v0.3.7",
    progress: { got: 5, total: 10 }, lastCheck: "2026-10-05T10:00:00Z",
    applied: { ok: false, version: "0.3.7", message: "Close Worms Ultimate Mayhem first.", warnings: ["w", 3] },
  });
  assert.equal(u.phase, "downloading");
  assert.deepEqual(u.progress, { got: 5, total: 10 });
  assert.equal(u.htmlUrl, "https://github.com/JaminB/melange/releases/tag/v0.3.7");
  assert.deepEqual(u.applied, { ok: false, version: "0.3.7", message: "Close Worms Ultimate Mayhem first.", warnings: ["w"] });
});

test("updateStatusOf: only a GitHub https page is ever linked", () => {
  assert.equal(updateStatusOf({ htmlUrl: "javascript:alert(1)" }).htmlUrl, undefined);
  assert.equal(updateStatusOf({ htmlUrl: "https://evil.example/github.com/" }).htmlUrl, undefined);
  assert.equal(updateStatusOf({ htmlUrl: "http://github.com/JaminB/melange" }).htmlUrl, undefined);
});

test("updateEventOf: the channel's {status} message", () => {
  assert.equal(updateEventOf({ status: { phase: "ready", latest: "1.0.0" } })?.latest, "1.0.0");
  assert.equal(updateEventOf({}), undefined);
  assert.equal(updateEventOf("x"), undefined);
});

test("updateBanner: progress while downloading, one-click restart when ready, nothing otherwise", () => {
  const d = updateBanner(status("downloading", { progress: { got: 45, total: 100 } }), false);
  assert.equal(d?.text, "Downloading Melange 0.3.7… 45%");
  assert.equal(d?.action, undefined);
  assert.equal(updateBanner(status("downloading"), false)?.text, "Downloading Melange 0.3.7…");
  const r = updateBanner(status("ready"), false);
  assert.equal(r?.text, "Melange 0.3.7 is ready");
  assert.equal(r?.action, UPDATE_ACTION);
  assert.equal(r?.blocked, undefined);
  assert.equal(updateBanner(status("ready"), true)?.blocked, UPDATE_GAME_RUNNING);
  for (const p of ["idle", "checking", "current", "error"] as UpdatePhase[]) assert.equal(updateBanner(status(p), false), undefined, p);
});

test("updateCheckLine: every phase says something", () => {
  for (const p of PHASES) assert.ok(updateCheckLine(status(p, { error: "offline" })).length > 0, p);
  assert.ok(/up to date/.test(updateCheckLine(status("current"))));
  assert.ok(/could not reach GitHub/.test(updateCheckLine(status("error", { error: "could not reach GitHub" }))));
  assert.ok(/each time it starts/.test(updateCheckLine(status("idle"))));
  assert.equal(updateCheckLine(status("idle", { auto: false })), "Automatic checks are off.");
});

test("updateStatusOf: auto is the Settings switch, absent when the launcher doesn't say", () => {
  assert.equal(updateStatusOf({ phase: "idle", auto: false }).auto, false);
  assert.equal(updateStatusOf({ phase: "idle", auto: true }).auto, true);
  assert.equal(updateStatusOf({ phase: "idle", auto: "no" }).auto, undefined);
  assert.equal(updateStatusOf({ phase: "idle" }).auto, undefined);
});

test("updateAppliedLine: success, warnings and failure", () => {
  assert.equal(updateAppliedLine({ ok: true, version: "0.3.7", message: "", warnings: [] }), "Updated to Melange 0.3.7.");
  assert.ok(/isn't installed/.test(updateAppliedLine({ ok: true, version: "0.3.7", message: "", warnings: ["Melange isn't installed in X."] })));
  const bad = updateAppliedLine({ ok: false, version: "0.3.7", message: "Close Worms Ultimate Mayhem first.", warnings: [] });
  assert.ok(/wasn't updated to 0\.3\.7/.test(bad));
  assert.ok(/Close Worms/.test(bad));
});
