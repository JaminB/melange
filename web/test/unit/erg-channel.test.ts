import assert from "node:assert/strict";
import { test } from "node:test";
import { createClient } from "../../src/sdk/client";
import { createErgSession, describeTest, onErgEvents, parseErgEvent, TEST_STATES, type ErgEvent } from "../../src/sdk/erg";

// The exact payloads the game publishes (levels_selftest checks the same strings on the C++ side).
const ARMED = JSON.parse(`{"state":"armed","key":"Multi.ergtest_p1","detail":""}`);
const FAILED = JSON.parse(`{"state":"failed","key":"k","detail":"a \\"b\\"\\u000a"}`);
const START = JSON.parse(`{"level":"Multi.mymaps_a","stem":"mymaps_a","source":"pack","online":false,"water":40.0000}`);
const VANILLA = JSON.parse(`{"level":"Multi.DinerMight","stem":"","source":"vanilla","online":true,"water":null}`);

class FakeSocket {
  static all: FakeSocket[] = [];
  readyState = 0;
  binaryType = "blob";
  sent: any[] = [];
  onopen: ((ev: Event) => void) | null = null;
  onclose: ((ev: CloseEvent) => void) | null = null;
  onmessage: ((ev: MessageEvent) => void) | null = null;
  onerror: ((ev: Event) => void) | null = null;
  constructor(readonly url: string) { FakeSocket.all.push(this); }
  send(s: string) { this.sent.push(JSON.parse(s)); }
  close(code = 1000, reason = "") { this.readyState = 3; this.onclose?.({ code, reason } as CloseEvent); }
  open() { this.readyState = 1; this.onopen?.({} as Event); }
  recv(m: object) { this.onmessage?.({ data: JSON.stringify(m) } as MessageEvent); }
}

const welcome = { t: "welcome", proto: 1, build: "b1", server: "game", channels: ["erg"], methods: [], panels: [] };

test("the game's erg payloads parse", () => {
  assert.deepEqual(parseErgEvent(ARMED), { kind: "test", state: "armed", key: "Multi.ergtest_p1", detail: "" });
  assert.deepEqual(parseErgEvent(FAILED), { kind: "test", state: "failed", key: "k", detail: 'a "b"\n' });
  assert.deepEqual(parseErgEvent(START),
    { kind: "level", level: "Multi.mymaps_a", stem: "mymaps_a", source: "pack", online: false, water: 40 });
  assert.deepEqual(parseErgEvent(VANILLA),
    { kind: "level", level: "Multi.DinerMight", stem: "", source: "vanilla", online: true, water: null });
});

test("malformed erg payloads are ignored", () => {
  for (const m of [null, 3, "x", [], {}, { state: "exploded" }, { state: 1 }, { level: 7 }])
    assert.equal(parseErgEvent(m), null, JSON.stringify(m));
  assert.deepEqual(parseErgEvent({ level: "L", source: "mars", water: "deep", online: "yes" }),
    { kind: "level", level: "L", stem: "", source: "vanilla", online: false, water: null });
  assert.deepEqual(parseErgEvent({ state: "idle" }), { kind: "test", state: "idle", key: "", detail: "" });
});

test("every Test state has a description", () => {
  for (const state of TEST_STATES) {
    const text = describeTest({ kind: "test", state, key: "Multi.ergtest_p1", detail: "why" }, "Harbour");
    assert.ok(state === "idle" ? text.includes("why") : text.length > 0, state);
  }
  assert.equal(describeTest({ kind: "test", state: "idle", key: "", detail: "" }), "");
  assert.ok(describeTest({ kind: "test", state: "armed", key: "Multi.x", detail: "" }).includes("Multi.x"));
});

test("onErgEvents and the session's onTest share the channel", () => {
  FakeSocket.all = [];
  const c = createClient({ url: "ws://x/ws", build: "b1", socket: (u) => new FakeSocket(u) as any });
  const s = FakeSocket.all[0];
  s.open();
  s.recv(welcome);
  const all: ErgEvent[] = [];
  const tests: string[] = [];
  const off = onErgEvents(c, (e) => all.push(e));
  const offTest = createErgSession(c).onTest((t) => tests.push(`${t.state}:${t.key}`));
  assert.ok(s.sent.some((m) => m.t === "sub" && m.ch === "erg"));
  s.recv({ t: "ev", ch: "erg", seq: 1, d: ARMED });
  s.recv({ t: "ev", ch: "erg", seq: 2, d: START });
  s.recv({ t: "ev", ch: "erg", seq: 3, d: { nonsense: true } });
  s.recv({ t: "ev", ch: "lobby", seq: 4, d: ARMED });
  assert.deepEqual(all.map((e) => e.kind), ["test", "level"]);
  assert.deepEqual(tests, ["armed:Multi.ergtest_p1"], "onTest ignores level starts");
  off();
  offTest();
  s.recv({ t: "ev", ch: "erg", seq: 5, d: VANILLA });
  assert.equal(all.length, 2);
  c.close();
});
