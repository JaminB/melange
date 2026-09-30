import assert from "node:assert/strict";
import { test } from "node:test";
import {
  IDLE_STATUS, initialTod, prefixOf, reduceTestEvent, statusLine, testAvailability, validateExportForm, validModId, validSlug,
  validVersion,
} from "../../src/panels/erg/test/model";

test("testAvailability", () => {
  assert.equal(testAvailability({ connected: false, inMatch: false, inLobby: false }).ok, false);
  assert.equal(testAvailability({ connected: true, inMatch: false, inLobby: true }).ok, false);
  assert.equal(testAvailability({ connected: true, inMatch: true, inLobby: false }).ok, false);
  assert.equal(testAvailability({ connected: true, inMatch: true, inLobby: false, attract: true }).ok, true, "the attract demo is not a match");
  assert.equal(testAvailability({ connected: true, inMatch: false, inLobby: false, attract: true }).ok, true);
  const ok = testAvailability({ connected: true, inMatch: false, inLobby: false });
  assert.equal(ok.ok, true);
  assert.equal(ok.reason, undefined);
});

test("reduceTestEvent follows the TestState machine and tracks busy", () => {
  let s = IDLE_STATUS;
  s = reduceTestEvent(s, { state: "registering", key: "", detail: "" });
  assert.equal(s.phase, "registering");
  assert.equal(s.busy, true);
  s = reduceTestEvent(s, { state: "armed", key: "Multi.ergtest_p1", detail: "" });
  assert.equal(s.key, "Multi.ergtest_p1");
  assert.equal(s.busy, true);
  s = reduceTestEvent(s, { state: "playing", key: "Multi.ergtest_p1", detail: "" });
  assert.equal(s.busy, false);
  assert.ok(/Playing/.test(statusLine(s)));
  s = reduceTestEvent(s, { state: "failed", key: "", detail: "boom" });
  assert.equal(statusLine(s), "Test failed: boom");
});

test("reduceTestEvent ignores an unknown state", () => {
  const s = reduceTestEvent(IDLE_STATUS, { state: "bogus", key: "x", detail: "" });
  assert.equal(s.phase, "idle");
  assert.equal(s.key, "x");
});

test("prefixOf and slug/mod-id/version validation", () => {
  assert.equal(prefixOf("my-maps"), "my_maps");
  assert.equal(validSlug("harbour1"), true);
  assert.equal(validSlug("Harbour"), false);
  assert.equal(validSlug(""), false);
  assert.equal(validSlug("x".repeat(25)), false);
  assert.equal(validModId("my-maps"), true);
  assert.equal(validModId("ergtest"), false);
  assert.equal(validModId("My Maps"), false);
  assert.equal(validVersion("1.0.0"), true);
  assert.equal(validVersion("1.0"), false);
});

test("validateExportForm", () => {
  assert.deepEqual(validateExportForm({ modId: "my-maps", name: "Harbour Brawl", version: "1.0.0", mode: "source" }), []);
  const errs = validateExportForm({ modId: "", name: "", version: "x", mode: "install" });
  assert.equal(errs.length, 3);
});

test("initialTod follows the project and falls back to DAY", () => {
  assert.equal(initialTod("NIGHT"), "NIGHT");
  assert.equal(initialTod("EVENING"), "EVENING");
  assert.equal(initialTod(""), "DAY");
  assert.equal(initialTod(undefined), "DAY");
  assert.equal(initialTod("night"), "DAY");
});

test("an attract refusal surfaces as a failed Test with its detail", () => {
  const s = reduceTestEvent(IDLE_STATUS, { state: "failed", key: "Multi.ergtest_p1", detail: "the attract demo started first; press Test again" });
  assert.equal(s.busy, false);
  assert.equal(statusLine(s), "Test failed: the attract demo started first; press Test again");
});
