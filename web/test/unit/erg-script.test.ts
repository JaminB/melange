import assert from "node:assert/strict";
import { test } from "node:test";
import { EditorState } from "@codemirror/state";
import type { ScriptProblem } from "../../src/sdk/erg/session";
import { LEVEL_API, MAX_SCRIPT_BYTES, ScriptDoc, localProblems, type ScriptSession } from "../../src/panels/erg/script";
import { problemDecorations } from "../../src/panels/erg/script/editor";

function fakeSession(initial = "", reply?: (text: string) => { saved: boolean; problems: ScriptProblem[] }) {
  const puts: string[] = [];
  const s: ScriptSession = {
    script: async () => initial,
    saveScript: async (text: string) => { puts.push(text); return reply ? reply(text) : { saved: true, problems: [] }; },
  };
  return { s, puts };
}

test("localProblems mirrors the server's byte rules with a line", () => {
  assert.deepEqual(localProblems("wum.log.info('hi')\n"), []);
  assert.deepEqual(localProblems(""), []);
  assert.equal(localProblems("﻿x = 1")[0].line, 1);
  const esc = localProblems("-- a\nlocal x = 1\n\u001bLua");
  assert.equal(esc.length, 1);
  assert.equal(esc[0].line, 3);
  assert.ok(/ESC/.test(esc[0].message));
  assert.equal(localProblems("a\n\u0000")[0].line, 2);
  assert.ok(/256 KB/.test(localProblems("x".repeat(MAX_SCRIPT_BYTES + 1))[0].message));
  assert.deepEqual(localProblems("x".repeat(MAX_SCRIPT_BYTES)), []);
});

test("ScriptDoc loads, tracks unsaved edits and saves", async () => {
  const { s, puts } = fakeSession("-- old\n");
  const doc = new ScriptDoc(s);
  let events = 0;
  doc.on(() => events++);
  await doc.load();
  assert.equal(doc.loaded, true);
  assert.equal(doc.text, "-- old\n");
  assert.equal(doc.dirty, false);
  assert.equal(await doc.save(), true);
  assert.equal(puts.length, 0, "nothing to save");
  doc.edit("wum.log.info(wum.level.stem)\n");
  assert.equal(doc.dirty, true);
  assert.equal(await doc.save(), true);
  assert.deepEqual(puts, ["wum.log.info(wum.level.stem)\n"]);
  assert.equal(doc.dirty, false);
  assert.ok(events >= 3);
});

test("ScriptDoc shows the server's problems and stays unsaved", async () => {
  const { s } = fakeSession("", () => ({ saved: false, problems: [{ line: 2, message: "the level script is not valid UTF-8" }] }));
  const doc = new ScriptDoc(s);
  await doc.load();
  doc.edit("a\nb\n");
  assert.equal(await doc.save(), false);
  assert.equal(doc.dirty, true);
  assert.deepEqual(doc.problems, [{ line: 2, message: "the level script is not valid UTF-8" }]);
});

test("ScriptDoc refuses locally without a round trip, and keeps an edit made while saving", async () => {
  let release!: () => void;
  const gate = new Promise<void>((r) => { release = r; });
  const puts: string[] = [];
  const doc = new ScriptDoc({
    script: async () => "",
    saveScript: async (t: string) => { puts.push(t); await gate; return { saved: true, problems: [] }; },
  });
  await doc.load();
  doc.edit("\u001b");
  assert.equal(doc.problems.length, 1);
  assert.equal(await doc.save(), false);
  assert.equal(puts.length, 0);
  doc.edit("x = 1");
  const p = doc.save();
  assert.equal(doc.busy, true);
  doc.edit("x = 2");
  release();
  assert.equal(await p, true);
  assert.equal(doc.saved, "x = 1");
  assert.equal(doc.dirty, true, "the newer edit is still unsaved");
});

test("ScriptDoc reports a failed call", async () => {
  const doc = new ScriptDoc({ script: async () => { throw new Error("no project is open"); }, saveScript: async () => ({ saved: true, problems: [] }) });
  await doc.load();
  assert.equal(doc.loaded, false);
  assert.equal(doc.error, "no project is open");
});

test("problems decorate their lines, clamped to the document", () => {
  const state = EditorState.create({ doc: "a\nb\nc" });
  const set = problemDecorations(state, [{ line: 2, message: "bad" }, { line: 99, message: "past the end" }]);
  const at: number[] = [];
  set.between(0, state.doc.length, (from) => { at.push(from); });
  assert.deepEqual(at, [2, 3, 4, 5]);
});

test("the reference names wum.level and sim.turnStarted", () => {
  const names = LEVEL_API.map((e) => `${e.name} ${e.text}`).join("\n");
  for (const n of ["wum.level.stem", "wum.level.knots", "wum.level.trigger", "wum.level.crate", "sim.turnStarted"]) assert.ok(names.includes(n), n);
});
