import assert from "node:assert/strict";
import { test } from "node:test";
import { launchWaitsForMaps, mapsEventOf, mapsStatusOf } from "../../src/launcher/api";
import { mapPackText, mapsLine } from "../../src/launcher/copy";

test("mapsStatusOf: garbage never throws; auto-build counts as on unless the server says off", () => {
  const s = mapsStatusOf(null);
  assert.equal(s.enabled, true);
  assert.equal(s.active, false);
  assert.deepEqual(s.packs, []);
  assert.equal(mapsStatusOf({ enabled: false }).enabled, false);
});

test("mapsStatusOf: packs keep known states, an unknown one becomes none, nameless packs fall back to the id", () => {
  const s = mapsStatusOf({
    active: true, step: 1, of: 2, current: "Foldspace", waitingLaunch: true,
    packs: [{ id: "foldspace", name: "Foldspace", state: "building" }, { id: "x", state: "weird", error: "boom" }, { name: "no id" }, 7],
  });
  assert.equal(s.packs.length, 2);
  assert.equal(s.packs[0].state, "building");
  assert.equal(s.packs[1].state, "none");
  assert.equal(s.packs[1].name, "x");
  assert.equal(s.packs[1].error, "boom");
});

test("mapsEventOf: status and the waiting launch's outcome", () => {
  const ev = mapsEventOf({ launch: { ok: false, error: "Steam didn't start the game." } });
  assert.equal(ev.status, undefined);
  assert.deepEqual(ev.launch, { ok: false, error: "Steam didn't start the game." });
  assert.equal(mapsEventOf({ status: { of: 1 } }).status?.of, 1);
  assert.deepEqual(mapsEventOf("nope"), {});
});

test("launchWaitsForMaps: only how=after-build", () => {
  assert.equal(launchWaitsForMaps({ how: "after-build" }), true);
  assert.equal(launchWaitsForMaps({ how: "exe" }), false);
  assert.equal(launchWaitsForMaps(undefined), false);
});

test("mapsLine: building with progress, the waiting launch, failures, and silence when all is built", () => {
  const base = mapsStatusOf({});
  assert.equal(mapsLine(undefined), undefined);
  assert.equal(mapsLine(base), undefined);
  assert.equal(mapsLine({ ...base, active: true, step: 1, of: 2, current: "Foldspace" }), "Building map pack Foldspace (1/2)…");
  assert.match(mapsLine({ ...base, active: true, step: 2, of: 2, current: "Foldspace", waitingLaunch: true }) ?? "", /\(2\/2\)….*game starts/);
  assert.match(mapsLine({ ...base, waitingLaunch: true }) ?? "", /Checking map packs/);
  const failed = { ...base, packs: [{ id: "a", name: "A", state: "failed" as const, error: "a.ergpatch.json: bad" }] };
  assert.match(mapsLine(failed) ?? "", /Map pack A could not be built: a\.ergpatch\.json: bad/);
  const two = { ...base, packs: [failed.packs[0], { id: "b", name: "B", state: "failed" as const }] };
  assert.match(mapsLine(two) ?? "", /2 map packs could not be built \(A, B\)/);
  assert.equal(mapsLine({ ...base, packs: [{ id: "a", name: "A", state: "built" }] }), undefined);
});

test("mapPackText: every state that needs words has some; built and none have none", () => {
  for (const state of ["building", "queued", "missing", "stale", "failed"] as const)
    assert.ok(mapPackText({ id: "a", name: "A", state }, true), state);
  assert.equal(mapPackText({ id: "a", name: "A", state: "built" }, true), undefined);
  assert.equal(mapPackText(undefined, true), undefined);
  assert.match(mapPackText({ id: "a", name: "A", state: "missing" }, false) ?? "", /AutoBuildMaps is off/);
  assert.match(mapPackText({ id: "a", name: "A", state: "failed", error: "boom" }, true) ?? "", /boom/);
});
