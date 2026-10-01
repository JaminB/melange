import assert from "node:assert/strict";
import { test } from "node:test";
import {
  actionLabel, compareVersions, confirmLines, detailsOf, eventOf, fetchLine, itemsOf, permissionsText, progress, shotUrl, sizeText,
  stateLine, statusOf, DEEP_DESERT,
} from "../../src/panels/store/model";

const raw = {
  id: "hd-water", name: "HD Water", authors: ["someone", 5], description: "d", categories: ["graphics"], kind: "client-only",
  unsafe: false, licence: "MIT", latest: "1.1.0", compatible: "1.1.0", size: 2 * 1048576,
  installed: { version: "1.0.0", managed: true, state: "enabled", enabled: true }, action: "update", canRemove: true,
  state: "update", reason: "", error: "",
};

test("store: items are read defensively", () => {
  const items = itemsOf([raw, { name: "no id" }, null, { id: "x", action: "explode", kind: "weird", size: "big" }]);
  assert.equal(items.length, 2);
  assert.deepEqual(items[0].authors, ["someone"]);
  assert.equal(items[0].installed?.version, "1.0.0");
  assert.equal(items[1].action, "none");
  assert.equal(items[1].kind, "client-only");
  assert.equal(items[1].size, 0);
  assert.equal(items[1].name, "x");
  assert.equal(items[1].installed, null);
  assert.deepEqual(itemsOf("nope"), []);
});

test("store: labels and state lines", () => {
  const [it] = itemsOf([raw]);
  assert.equal(actionLabel(it), "Update");
  assert.equal(stateLine(it), "Update to 1.1.0");
  assert.equal(actionLabel({ ...it, action: "install", installed: null }), "Install");
  assert.equal(actionLabel({ ...it, action: "install" }), "Replace");
  assert.equal(actionLabel({ ...it, action: "none" }), undefined);
  assert.equal(stateLine({ ...it, state: "pending" }), "Applies at the next launch");
  assert.equal(stateLine({ ...it, state: "installed", installed: { ...it.installed!, version: "1.1.0" } }), "Installed 1.1.0");
  assert.equal(stateLine({ ...it, state: "incompatible", installed: null, reason: "Incompatible: needs Melange >=9.0.0" }),
    "Incompatible: needs Melange >=9.0.0");
});

test("store: status, fetch line and progress", () => {
  const s = statusOf({ fetchedAt: "2026-10-01 14:02", haveIndex: true, plugins: 23, job: { phase: "downloading", id: "a", version: "1.0.0", bytes: 512 * 1024, total: 1048576 } });
  assert.equal(fetchLine(s), "Fetched 2026-10-01 14:02, 23 plugins");
  assert.equal(fetchLine({ ...s, fetching: true }), "Fetching the list…");
  assert.equal(fetchLine({ ...s, offline: true, error: "timed out" }), "Offline: showing the list from 2026-10-01 14:02");
  assert.equal(fetchLine({ ...s, haveIndex: false, error: "could not connect" }), "could not connect");
  const p = progress(s.job);
  assert.equal(p.active, true);
  assert.equal(p.pct, 50);
  assert.equal(p.text, "downloading a 1.0.0 512 KiB / 1.0 MiB");
  assert.equal(progress({ ...s.job, phase: "done" }).active, false);
  assert.equal(statusOf(null).job.phase, "idle");
  assert.deepEqual(eventOf({ phase: "done", pending: ["x", 1], shots: 2 }).pending, ["x"]);
  assert.equal(sizeText(100), "1 KiB");
  assert.equal(shotUrl("a b", 2, 7), "/store/shots/a%20b/2?v=7");
});

test("store: confirms say what will happen before anything is downloaded", () => {
  const d = detailsOf({
    ...raw, unsafe: true, kind: "content", content: true, installed: null, homepage: "https://x",
    permissions: { unsafe: true, filesystem: "own-folder" }, plan: [{ id: "lib", version: "0.3.1" }, { id: "hd-water", version: "1.1.0" }],
    conflictsEnabled: ["other"], dependants: ["b", "c"], screenshots: [{ n: 1, caption: "c", ready: true }, { n: 0 }],
    versions: [{ version: "1.1.0", compatible: true }, { nope: 1 }],
  })!;
  assert.equal(d.screenshots.length, 1);
  assert.equal(d.versions.length, 1);
  assert.equal(permissionsText(d), "Deep Desert (raw access to the game's memory), files: own-folder");
  const lines = confirmLines(d, { kind: "install", version: "1.1.0", older: false });
  assert.ok(lines.includes(DEEP_DESERT));
  assert.ok(lines.includes("Content: everyone in an online match needs the same version."));
  assert.ok(lines.includes("Also installs: lib 0.3.1"));
  assert.ok(lines.some((l) => l.startsWith("Conflicts with other")));
  assert.ok(!lines.includes("Updates ask for Deep Desert again."));
  const upd = confirmLines({ ...d, installed: { version: "1.0.0", managed: true, state: "enabled", enabled: true } }, { kind: "install", version: "1.0.0", older: true });
  assert.ok(upd.includes("Updates ask for Deep Desert again."));
  assert.ok(upd.includes("This is older than what you have."));
  const manual = confirmLines({ ...d, installed: { version: "0.9.0", managed: false, state: "enabled", enabled: true } }, { kind: "install", version: "1.1.0", older: false });
  assert.ok(manual.some((l) => l.startsWith("Replace the copy in Mods\\hd-water")));
  assert.deepEqual(confirmLines(d, { kind: "remove" }), ["b, c need this; they will be blocked."]);
});

test("store: version order", () => {
  assert.equal(compareVersions("1.2.0", "1.10.0"), -1);
  assert.equal(compareVersions("1.0.0", "1.0.0-rc.1"), 1);
  assert.equal(compareVersions("1.0.0-beta", "1.0.0-rc.1"), -1);
  assert.equal(compareVersions("2.0.0", "2.0.0"), 0);
});
