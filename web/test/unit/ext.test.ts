import assert from "node:assert/strict";
import { test } from "node:test";
import { extAllowed } from "../../src/shell/ext/host";

test("a panel may use its own mod.<id>.* channels and methods", () => {
  assert.ok(extAllowed("hello-spice", "mod.hello-spice.ticks", "channel"));
  assert.ok(extAllowed("hello-spice", "mod.hello-spice.doThing", "method"));
});

test("a panel may only read state and log, never call them", () => {
  assert.ok(extAllowed("hello-spice", "state", "channel"));
  assert.ok(extAllowed("hello-spice", "log", "channel"));
  assert.ok(!extAllowed("hello-spice", "state", "method"));
  assert.ok(!extAllowed("hello-spice", "log", "method"));
});

test("a panel cannot reach another mod's names, or core methods", () => {
  assert.ok(!extAllowed("hello-spice", "mod.other.ticks", "channel"));
  assert.ok(!extAllowed("hello-spice", "mod.other.doThing", "method"));
  assert.ok(!extAllowed("hello-spice", "lua.eval", "method"));
  assert.ok(!extAllowed("hello-spice", "mods.setEnabled", "method"));
  assert.ok(!extAllowed("hello-spice", "bus", "channel"));
});

test("a prefix match must be a real name boundary", () => {
  assert.ok(!extAllowed("hello", "mod.hello-spice.ticks", "channel"));
});
