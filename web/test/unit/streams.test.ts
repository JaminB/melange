import assert from "node:assert/strict";
import { test } from "node:test";
import {
  anyBusNameMatches, busNameMatches, busPrefixOf, isBusPrefixPattern, levelAtLeast, matchesLog,
} from "../../src/sdk/streams";

test("levelAtLeast", () => {
  assert.equal(levelAtLeast("info"), true, "no floor: everything passes");
  assert.equal(levelAtLeast("info", "warn"), false);
  assert.equal(levelAtLeast("warn", "warn"), true, "at the floor passes");
  assert.equal(levelAtLeast("fatal", "warn"), true);
  assert.equal(levelAtLeast("trace", "debug"), false);
});

test("matchesLog", () => {
  assert.equal(matchesLog({}, { lvl: "trace", cat: "x", j: {} }), true, "an empty filter matches everything");
  assert.equal(matchesLog({ minLevel: "warn" }, { lvl: "info", cat: "x", j: {} }), false);
  assert.equal(matchesLog({ cats: ["net", "handshake"] }, { lvl: "info", cat: "net", j: {} }), true);
  assert.equal(matchesLog({ cats: ["net"] }, { lvl: "info", cat: "shader", j: {} }), false);
  assert.equal(matchesLog({ text: "boom" }, { lvl: "info", cat: "x", j: { msg: "boom" } }), true);
  assert.equal(matchesLog({ text: "boom" }, { lvl: "info", cat: "x", j: { msg: "quiet" } }), false);
});

test("isBusPrefixPattern / busPrefixOf", () => {
  assert.equal(isBusPrefixPattern("GameLogic.Turn.Started"), false);
  assert.equal(isBusPrefixPattern("GameLogic.Turn.*"), true);
  assert.equal(isBusPrefixPattern(".*"), false, "nothing before the dot is not a prefix pattern");
  assert.equal(busPrefixOf("GameLogic.Turn.*"), "GameLogic.Turn.");
  assert.equal(busPrefixOf("GameLogic.Turn.Started"), "GameLogic.Turn.Started", "an exact name is returned unchanged");
});

test("busNameMatches", () => {
  assert.equal(busNameMatches("GameLogic.Turn.Started", "GameLogic.Turn.Started"), true);
  assert.equal(busNameMatches("GameLogic.Turn.Started", "GameLogic.Turn.Ended"), false);
  assert.equal(busNameMatches("GameLogic.Turn.Started", "GameLogic.Turn.*"), true);
  assert.equal(busNameMatches("GameLogic.Turn.Started", "GameLogic.*"), true);
  assert.equal(busNameMatches("GameLogic2.Turn.Started", "GameLogic.*"), false, "the dot after the prefix is required");
  assert.equal(busNameMatches("GameLogicX", "GameLogic.*"), false);
});

test("anyBusNameMatches", () => {
  assert.equal(anyBusNameMatches("Camera.HasUpdated", ["GameLogic.*", "Camera.HasUpdated"]), true);
  assert.equal(anyBusNameMatches("Camera.HasUpdated", ["GameLogic.*"]), false);
  assert.equal(anyBusNameMatches("Camera.HasUpdated", []), false);
});
