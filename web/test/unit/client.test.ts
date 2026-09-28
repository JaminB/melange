import assert from "node:assert/strict";
import { test } from "node:test";
import { createClient, RpcError } from "../../src/sdk/client";
import { panels, registerPanel, unmetReason } from "../../src/sdk/panels";

class FakeSocket {
  static all: FakeSocket[] = [];
  readyState = 0;
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
  drop(code: number) { this.readyState = 3; this.onclose?.({ code, reason: "" } as CloseEvent); }
}

const welcome = { t: "welcome", proto: 1, build: "b1", server: "game", game: { exeBuild: 1077, melange: "0.2.0" },
  channels: ["log", "state"], methods: ["sys.ping"], panels: [] };
const tick = (ms = 0) => new Promise((r) => setTimeout(r, ms));

function make(extra: object = {}) {
  FakeSocket.all = [];
  const c = createClient({ url: "ws://x/ws", build: "b1", socket: (u) => new FakeSocket(u) as any, minBackoffMs: 5, maxBackoffMs: 20, ...extra });
  const s = FakeSocket.all[0];
  return { c, s };
}

test("hello, welcome and feature detection", () => {
  const { c, s } = make();
  assert.equal(c.state, "connecting");
  s.open();
  assert.deepEqual(s.sent[0], { t: "hello", proto: 1, build: "b1", client: "oasis-web" });
  s.recv(welcome);
  assert.equal(c.state, "open");
  assert.equal(c.welcome()?.server, "game");
  assert.ok(c.has("sys.ping") && c.has("log") && !c.has("nope"));
  c.close();
});

test("subscribe, batches with seq, drops and unsubscribe", () => {
  const { c, s } = make();
  const got: [unknown, number][] = [];
  let dropped = 0;
  const off = c.subscribe("log", { minLevel: "info" }, (m, seq) => got.push([m, seq]), (n) => (dropped += n));
  s.open();
  s.recv(welcome);
  assert.deepEqual(s.sent[1], { t: "sub", ch: "log", filter: { minLevel: "info" } });
  s.recv({ t: "ev", ch: "log", seq: 1, d: "a" });
  s.recv({ t: "drop", ch: "log", n: 3, why: "queue" });
  s.recv({ t: "ev", ch: "log", seq: 5, b: ["b", "c"] });
  s.recv({ t: "ev", ch: "other", seq: 1, d: "x" });
  assert.deepEqual(got, [["a", 1], ["b", 5], ["c", 6]]);
  assert.equal(dropped, 3);
  off();
  assert.deepEqual(s.sent.at(-1), { t: "unsub", ch: "log" });
  c.close();
});

test("calls resolve, reject with RpcError, and time out", async () => {
  const { c, s } = make();
  s.open();
  s.recv(welcome);
  const p1 = c.call<{ frame: number }>("sys.ping");
  const id1 = s.sent.at(-1).id;
  assert.equal(s.sent.at(-1).m, "sys.ping");
  s.recv({ t: "res", id: id1, r: { frame: 7, ms: 1 } });
  assert.equal((await p1).frame, 7);
  const p2 = c.call("mods.setEnabled", { id: "x", on: true });
  s.recv({ t: "err", id: s.sent.at(-1).id, code: -32003, msg: "read-only" });
  await assert.rejects(p2, (e) => e instanceof RpcError && e.code === -32003);
  await assert.rejects(c.call("sys.ping", {}, 10), (e) => e instanceof RpcError && e.code === -1);
  c.close();
});

test("reconnects with backoff and resubscribes; pending calls fail on disconnect", async () => {
  const { c, s } = make();
  c.subscribe("state", undefined, () => {});
  s.open();
  s.recv(welcome);
  const p = c.call("sys.ping");
  s.drop(1006);
  assert.equal(c.state, "closed");
  await assert.rejects(p, (e) => e instanceof RpcError && e.code === -2);
  await tick(30);
  const s2 = FakeSocket.all[1];
  assert.ok(s2, "a second socket was opened");
  s2.open();
  s2.recv(welcome);
  assert.equal(c.state, "open");
  assert.deepEqual(s2.sent[1], { t: "sub", ch: "state" });
  c.close();
});

test("protocol refusal goes offline and does not retry", async () => {
  const { c, s } = make();
  s.open();
  s.recv({ t: "bye", reason: "protocol", want: 2 });
  s.drop(4001);
  assert.equal(c.state, "offline");
  await tick(30);
  assert.equal(FakeSocket.all.length, 1);
});

test("a different server build is reported once per welcome", () => {
  let stale = "";
  const { c, s } = make({ onStale: (b: string) => (stale = b) });
  s.open();
  s.recv({ ...welcome, build: "b2" });
  assert.equal(stale, "b2");
  c.close();
});

test("panel registry orders panels and explains unmet needs", () => {
  const load = async () => ({ mount: () => () => {} });
  registerPanel({ id: "zeta", title: "Z", order: 5, needs: [], load });
  registerPanel({ id: "alpha", title: "A", order: 5, needs: ["match"], load });
  registerPanel({ id: "first", title: "F", order: 1, needs: ["game"], load });
  assert.deepEqual(panels().map((p) => p.id), ["first", "alpha", "zeta"]);
  const w = { ...welcome, server: "standalone" } as any;
  assert.equal(unmetReason(panels()[0], w, false), "needs the game running");
  assert.equal(unmetReason(panels()[1], w, false), "needs a match in progress");
  assert.equal(unmetReason(panels()[1], w, true), undefined);
});
