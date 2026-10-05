import assert from "node:assert/strict";
import { test } from "node:test";
import { History, completionWord, parseTarget, targetParams, targetValue } from "../../src/panels/console/history";
import { Counts, EventStore, covered, namesOf, prefixes, toggle, validPattern } from "../../src/panels/events/model";
import { docOf, effective, isDefault, protectedReason, sections, valueProblem } from "../../src/panels/ini/model";
import { LogStore, fromEvent, fromLine, levelOf, message, parseJsonl, sessionUrl, sessionsOf, shortTime } from "../../src/panels/logs/model";
import { deepDesertAction, hiddenText, modsOf, sourceText, stateText, viewOf, visibleMods, withMod } from "../../src/panels/mods/model";
import { filterCommands } from "../../src/shell/Palette";
import { closeSecondary, loadLayout, matchScore, openPanel, resolve, sanitize, saveLayout } from "../../src/shell/layout";

const rec = (seq: number, lvl: string, cat: string, msg: string) =>
  fromEvent({ seq, lvl, cat, ts: "2026-09-28T10:00:00.123-04:00", j: JSON.stringify({ seq, lvl, cat, msg }) })!;

test("logs: levels, payloads and messages", () => {
  assert.equal(levelOf("warn"), 3);
  assert.equal(levelOf("ERROR"), 4);
  assert.equal(levelOf(5), 5);
  assert.equal(levelOf("nonsense"), 2);
  const r = { ...rec(7, "info", "core", "hello"), n: 1 };
  assert.equal(message(r), "hello");
  const withData = { ...fromLine('{"seq":3,"lvl":"warn","cat":"net","msg":"peer","data":{"id":5},"wall":"2026-09-28T10:00:01.000-04:00"}')!, n: 2 };
  assert.equal(message(withData), 'peer {"id":5}');
  assert.equal(shortTime(withData.ts), "10:00:01.000");
  assert.equal(fromEvent({ lvl: "info" }), undefined);
  assert.equal(fromLine("not json"), undefined);
  const obj = fromEvent({ seq: 1, lvl: 2, cat: "c", ts: 5, j: { msg: "as object", wall: "x" } })!;
  assert.equal(message({ ...obj, n: 3 }), "as object");
  assert.equal(obj.ts, "x");
});

test("logs: store caps, filters and skips replayed backlog", () => {
  const s = new LogStore(5);
  s.addLive([rec(1, "info", "a", "one"), rec(2, "warn", "b", "two"), rec(3, "debug", "a", "three")]);
  assert.equal(s.view.length, 3);
  s.addLive([rec(2, "warn", "b", "two"), rec(3, "debug", "a", "three"), rec(4, "error", "b", "four")]);
  assert.deepEqual(s.all.map((r) => r.seq), [1, 2, 3, 4], "backlog after a reconnect is not duplicated");
  s.setFilter({ minLevel: 3, cats: [], text: "" });
  assert.deepEqual(s.view.map((r) => r.seq), [2, 4]);
  s.setFilter({ minLevel: 0, cats: ["a"], text: "" });
  assert.deepEqual(s.view.map((r) => r.seq), [1, 3]);
  s.setFilter({ minLevel: 0, cats: [], text: "THR" });
  assert.deepEqual(s.view.map((r) => r.seq), [3]);
  s.setFilter({ minLevel: 0, cats: [], text: "" });
  s.addLive([5, 6, 7].map((n) => rec(n, "info", "a", `m${n}`)));
  assert.deepEqual(s.all.map((r) => r.seq), [3, 4, 5, 6, 7], "oldest removed at the cap");
  assert.deepEqual(s.view.map((r) => r.seq), [3, 4, 5, 6, 7], "the view follows the cap");
  assert.equal(s.trimmed, 2);
  assert.equal(s.cats.get("a"), 5);
  s.clear();
  assert.equal(s.all.length + s.view.length, 0);
});

test("logs: 2000 records a second for 30 s stays capped and fast", () => {
  const s = new LogStore(50000);
  s.setFilter({ minLevel: 2, cats: [], text: "" });
  const t0 = Date.now();
  let seq = 0;
  for (let batch = 0; batch < 60 * 30; batch++) {
    const items = [];
    for (let i = 0; i < 34; i++) { seq++; items.push(rec(seq, i % 7 ? "info" : "debug", "load", `line ${seq}`)); }
    s.addLive(items);
  }
  assert.equal(s.all.length, 50000);
  assert.equal(s.all[s.all.length - 1].seq, seq);
  assert.ok(s.view.every((r, i) => i === 0 || r.n > s.view[i - 1].n));
  assert.ok(Date.now() - t0 < 5000, `took ${Date.now() - t0} ms`);
});

test("logs: sessions and jsonl files", () => {
  const ss = sessionsOf([{ id: "2026-09-28_17-52-13_pid1", files: ["events.jsonl"], bytes: 10 }, { id: "x", files: 2, bytes: 1 }, { nope: 1 }]);
  assert.equal(ss.length, 2);
  assert.deepEqual(ss[1].files, ["events.jsonl"]);
  assert.equal(sessionUrl("a b", "events.jsonl"), "/logs/a%20b/events.jsonl");
  const { rows, bad, skipped } = parseJsonl('{"seq":1,"lvl":"info","cat":"a","msg":"x"}\n\nbroken\n{"seq":2,"lvl":"warn","cat":"a","msg":"y"}\n', 1);
  assert.equal(bad, 1);
  assert.equal(skipped, 1);
  assert.equal(rows[0].seq, 2);
});

test("events: names, patterns and counts", () => {
  const names = namesOf([{ id: 2, name: "GameLogic.Turn.Started", posts: 4, deliveries: 9 }, { id: 1, name: "Camera.HasUpdated", posts: 9 }, { x: 1 }]);
  assert.deepEqual(names.map((n) => n.name), ["Camera.HasUpdated", "GameLogic.Turn.Started"]);
  assert.deepEqual(prefixes(names), ["Camera.*", "GameLogic.*"]);
  assert.ok(validPattern("GameLogic.Turn.*") && validPattern("Camera.HasUpdated"));
  assert.ok(!validPattern("*") && !validPattern("a..b") && !validPattern("a b") && !validPattern(".x"));
  assert.ok(covered("GameLogic.Turn.Started", ["GameLogic.*"]));
  assert.ok(!covered("GameLogicX.Turn", ["GameLogic.*"]));
  assert.ok(!covered("GameLogic.Turn.Started", ["GameLogic.Turn"]));
  assert.deepEqual(toggle(["b"], "a", true), ["a", "b"]);
  assert.deepEqual(toggle(["a", "b"], "a", false), ["b"]);
  const s = new EventStore(3);
  s.add([{ name: "A", seq: 1, frame: 10, d: { x: 1 } }, { bad: true }, { name: "B", seq: 2 }], 5);
  assert.equal(s.rows.length, 2);
  assert.ok(s.rows[0].hasD && !s.rows[1].hasD);
  s.add([{ name: "C" }, { name: "D" }], 6);
  assert.deepEqual(s.rows.map((r) => r.name), ["B", "C", "D"]);
  assert.equal(s.trimmed, 1);
  const c = new Counts();
  c.apply({ A: 10, B: 1 }, 1000);
  c.apply({ A: 4 }, 3000);
  const rows = c.rows();
  assert.equal(rows[0].name, "A");
  assert.equal(rows[0].total, 14);
  assert.equal(rows[0].rate, 2);
  assert.equal(rows[1].rate, 0);
});

test("console: history browsing keeps the draft", () => {
  const h = new History(["a", "", "b"], 3);
  assert.deepEqual(h.list(), ["a", "b"]);
  assert.equal(h.newer(), undefined);
  assert.equal(h.older("draft"), "b");
  assert.equal(h.older("b"), "a");
  assert.equal(h.older("a"), undefined);
  assert.equal(h.newer(), "b");
  assert.equal(h.newer(), "draft");
  assert.equal(h.newer(), undefined);
  h.add("b");
  assert.deepEqual(h.list(), ["a", "b"], "a repeat of the last entry is not added");
  h.add("c");
  h.add("d");
  assert.deepEqual(h.list(), ["b", "c", "d"], "capped");
});

test("console: completion words and targets", () => {
  assert.deepEqual(completionWord("return wum.game.sc"), { word: "wum.game.sc", from: 16 });
  assert.deepEqual(completionWord("x = obj:me"), { word: "obj:me", from: 8 });
  assert.deepEqual(completionWord("print(pri"), { word: "pri", from: 6 });
  assert.equal(completionWord("1 + "), undefined);
  assert.deepEqual(targetParams(parseTarget("mod:hello-spice")), { target: "mod", mod: "hello-spice" });
  assert.deepEqual(targetParams(parseTarget("match")), { target: "match" });
  assert.deepEqual(targetParams(parseTarget("mod:")), { target: "client" });
  assert.equal(targetValue(parseTarget("mod:x")), "mod:x");
});

test("mods: info, states and Deep Desert actions", () => {
  const list = modsOf([
    { id: "a", name: "A", state: "enabled", kind: "client", on: true, deepDesert: { declared: true, granted: true } },
    { id: "b", state: "pending-consent", deepDesert: { declared: true, granted: false } },
    { id: "c", state: "restart-required", kind: "content", deepDesert: { declared: false, granted: true } },
    { name: "no id" },
  ]);
  assert.equal(list.length, 3);
  assert.equal(deepDesertAction(list[0]), "revoke");
  assert.equal(deepDesertAction(list[1]), "grant-in-game");
  assert.equal(deepDesertAction(list[2]), "none", "granted without declared is not shown as granted");
  assert.ok(list[2].restartRequired && list[2].kind === "content");
  assert.equal(stateText("pending-consent"), "Waiting for Deep Desert consent");
  const next = withMod(list, { ...list[0], on: false, state: "disabled" });
  assert.equal(next[0].state, "disabled");
  assert.equal(list[0].state, "enabled");
});

test("mods: Store and local plugins, the show-local filter and sweep notices", () => {
  const list = modsOf([
    { id: "s", state: "enabled", on: true, source: "store" },
    { id: "l1", state: "enabled", on: true, source: "local" },
    { id: "l2", state: "disabled", on: false, source: "local" },
    { id: "old", state: "enabled", on: true },
    { id: "odd", state: "enabled", on: true, source: "elsewhere" },
  ]);
  assert.equal(list[0].source, "store");
  assert.equal(list[3].source, undefined, "an older server says nothing");
  assert.equal(list[4].source, undefined);
  const hiddenView = visibleMods(list, false);
  assert.deepEqual(hiddenView.shown.map((m) => m.id), ["s", "old", "odd"], "only plugins known to be local are hidden");
  assert.equal(hiddenView.hidden, 2);
  assert.equal(hiddenView.hiddenOn, 1);
  assert.equal(hiddenText(2, 1), "2 local plugins hidden (1 on)");
  assert.equal(hiddenText(1, 0), "1 local plugin hidden (0 on)");
  assert.equal(visibleMods(list, true).shown.length, 5);
  assert.equal(sourceText(list[0]), "Store");
  assert.equal(sourceText(list[1]), "Local");
  assert.equal(sourceText(list[3]), undefined);

  const view = viewOf({ showLocal: true, notices: [
    { key: "k1", id: "a", name: "A", action: "quarantined", reason: "needs Melange >=0.4.0, you have 0.3.6", folder: ".incompatible\\a", text: "Moved A" },
    { key: "k2", id: "b", action: "removed", reason: "r" },
    { key: "", id: "c" },
    "junk",
  ] });
  assert.equal(view.showLocal, true);
  assert.deepEqual(view.notices.map((n) => n.key), ["k2", "k1"], "newest first; a notice without a key is dropped");
  assert.equal(view.notices[1].folder, ".incompatible\\a");
  assert.equal(view.notices[0].text, "b: r", "a missing text is made from the id and reason");
  assert.deepEqual(viewOf(undefined), { showLocal: false, notices: [] });
});

test("ini: keys by section, values and protected keys", () => {
  const doc = docOf({ path: "C:\\g\\Melange.ini", encoding: "ansi", text: "[Oasis]\n", keys: [
    { section: "Oasis", key: "Port", def: "8765", live: false, declared: true, current: null },
    { section: "oasis", key: "Enabled", def: "1", live: false, declared: true, current: "1", line: 2 },
    { section: "Extra", key: "X", def: null, live: false, declared: false, current: "y" },
    { section: 1, key: "bad" },
  ] })!;
  assert.equal(doc.keys.length, 3);
  const s = sections(doc.keys);
  assert.deepEqual(s.map((x) => x.name), ["Oasis", "Extra"]);
  assert.deepEqual(s[0].keys.map((k) => k.key), ["Enabled", "Port"]);
  assert.equal(effective(doc.keys[0]), "8765");
  assert.ok(isDefault(doc.keys[0]) && isDefault(doc.keys[1]) && !doc.keys[2].declared);
  assert.deepEqual(sections(doc.keys, "extra").map((x) => x.name), ["Extra"]);
  assert.equal(valueProblem("Ctrl+Shift+O"), undefined);
  assert.ok(valueProblem("a;b") && valueProblem("a\nb") && valueProblem(" a") && valueProblem("x".repeat(1001)));
  assert.ok(protectedReason("Thumper", "AutoGrantDeepDesert", "1"));
  assert.equal(protectedReason("thumper", "autograntdeepdesert", "0"), undefined);
  assert.ok(protectedReason("THUMPER", "GrantSalt", "0"));
  assert.equal(protectedReason("Oasis", "GrantSalt", "1"), undefined);
});

test("shell: layout, storage and palette", () => {
  assert.deepEqual(sanitize({ primary: "logs", secondary: "logs", direction: "diagonal", theme: "neon" }),
    { primary: "logs", secondary: undefined, direction: "row", theme: "system" });
  assert.equal(sanitize({ primary: "../x" }).primary, undefined);
  let l = openPanel(sanitize({ primary: "logs" }), "console", true);
  assert.equal(l.secondary, "console");
  l = openPanel(l, "console");
  assert.deepEqual([l.primary, l.secondary], ["console", "logs"], "opening the side panel swaps");
  assert.equal(openPanel(l, "console", true), l);
  assert.equal(closeSecondary(l).secondary, undefined);
  assert.deepEqual(resolve({ ...l, primary: "gone", secondary: "logs" }, ["logs", "mods"]), { ...l, primary: "logs", secondary: undefined });
  const mem = new Map<string, string>();
  const store = { getItem: (k: string) => mem.get(k) ?? null, setItem: (k: string, v: string) => void mem.set(k, v) };
  saveLayout(store, { primary: "mods", direction: "column", theme: "dark" });
  assert.deepEqual(loadLayout(store), { primary: "mods", secondary: undefined, direction: "column", theme: "dark" });
  const broken = { getItem: () => { throw new Error("denied"); }, setItem: () => { throw new Error("denied"); } };
  assert.equal(loadLayout(broken).theme, "system");
  saveLayout(broken, l);
  mem.set("oasis.layout", "{not json");
  assert.equal(loadLayout(store).direction, "row");
  assert.ok(matchScore("Open Logs beside", "logs bes") >= 0);
  assert.equal(matchScore("Open Logs", "mods"), -1);
  const cmds = ["Open Logs", "Open Mods", "Theme: dark", "Open Logs beside"].map((label, i) => ({ id: String(i), label, run: () => {} }));
  assert.deepEqual(filterCommands(cmds, "logs").map((c) => c.label), ["Open Logs", "Open Logs beside"]);
  assert.deepEqual(filterCommands(cmds, "dark").map((c) => c.label), ["Theme: dark"]);
  assert.equal(filterCommands(cmds, "  ").length, 4);
});
