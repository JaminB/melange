import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import { createClient, RpcError } from "../../src/sdk/client";
import { createLevelService, ErgError, receiveBlobs, type Scene } from "../../src/sdk/erg";

const scene12 = (): Scene => JSON.parse(readFileSync(new URL("../../../tests/fixtures/erg/synthetic-12.json", import.meta.url), "utf8"));
const tick = (ms = 0) => new Promise((r) => setTimeout(r, ms));

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
  bin(ref: number, payload: Uint8Array, meta: object = { ref }) {
    this.recv({ t: "bin", ref, ch: "erg", len: payload.length, meta });
    const buf = new Uint8Array(payload.length + 4);
    new DataView(buf.buffer).setUint32(0, ref, true);
    buf.set(payload, 4);
    this.onmessage?.({ data: buf.buffer } as MessageEvent);
  }
  last() { return this.sent.at(-1); }
  reply(r: unknown) { this.recv({ t: "res", id: this.last().id, r }); }
}

const LEVEL_METHODS = ["level.list", "level.new", "level.load", "level.save", "level.export", "level.build", "level.themes", "level.palette", "level.close"];

function connect() {
  FakeSocket.all = [];
  const c = createClient({ url: "ws://x/ws", build: "b1", socket: (u) => new FakeSocket(u) as any });
  const s = FakeSocket.all[0];
  s.open();
  s.recv({ t: "welcome", proto: 1, build: "b1", server: "standalone", channels: ["erg"], methods: LEVEL_METHODS, panels: [] });
  return { c, s };
}

test("level service calls carry the documented params", async () => {
  const { c, s } = connect();
  const svc = createLevelService(c);
  for (const m of LEVEL_METHODS) assert.ok(c.has(m), m);

  const listing = svc.list();
  assert.deepEqual([s.last().m, s.last().p], ["level.list", {}]);
  s.reply({ bases: [{ key: "Multi.Synth", stem: "Multi_Synth", title: "Synthetic", source: "game", theme: "BUILDING" }], projects: [] });
  assert.equal((await listing).bases[0].theme, "BUILDING");

  const creating = svc.create("Multi.Synth", "harbour", "Harbour Brawl");
  assert.deepEqual([s.last().m, s.last().p], ["level.new", { base: "Multi.Synth", slug: "harbour", title: "Harbour Brawl" }]);
  s.reply({ id: "harbour", title: "Harbour Brawl", stem: "ergtest_harbour", base: "Multi.Synth", modified: "2026-09-29T00:00:00Z", built: false,
    patch: { format: "erg-patch/1", stem: "ergtest_harbour", title: "Harbour Brawl", base: { key: "Multi.Synth", source: "game",
      sha256: { xan: "a".repeat(64), xom: "b".repeat(64), hmp: null } }, ops: [] } });
  const p = await creating;
  assert.equal(p.patch.stem, "ergtest_harbour");

  svc.create("Multi.my_maps_x", "x", "X", "pack");
  assert.deepEqual(s.last().p, { base: "Multi.my_maps_x", slug: "x", title: "X", source: "pack" });
  s.reply({});

  const themes = svc.themes();
  assert.equal(s.last().m, "level.themes");
  s.reply({ themes: ["ARABIAN"], timesOfDay: ["DAY"], materialFiles: [] });
  assert.deepEqual((await themes).timesOfDay, ["DAY"]);

  const pal = svc.palette("CAMELOT");
  assert.deepEqual([s.last().m, s.last().p], ["level.palette", { theme: "CAMELOT" }]);
  s.reply({ theme: "CAMELOT", entries: [{ name: "oildrum", resource: "OilDrum", role: "object", preview: null }] });
  assert.equal((await pal)[0].resource, "OilDrum");

  const exp = svc.exportProject("harbour", "my-maps", "My maps", "1.0.0", "source");
  assert.deepEqual([s.last().m, s.last().p], ["level.export", { project: "harbour", modId: "my-maps", name: "My maps", version: "1.0.0", mode: "source" }]);
  s.recv({ t: "err", id: s.last().id, code: -32003, msg: "the game is running; Mods is read-only here" });
  await assert.rejects(exp, (e: unknown) => e instanceof RpcError && e.code === -32003);

  const built = svc.buildMod("my-maps");
  assert.deepEqual([s.last().m, s.last().p], ["level.build", { modId: "my-maps" }]);
  s.reply({ modId: "my-maps", dir: "x", levels: [{ slug: "harbour", stem: "my_maps_harbour", files: ["assets/levels/Maps/my_maps_harbour.xan"] }], skipped: [] });
  assert.equal((await built).levels[0].stem, "my_maps_harbour");

  const closing = svc.close("harbour");
  assert.deepEqual([s.last().m, s.last().p], ["level.close", { project: "harbour" }]);
  s.reply({ closed: true });
  await closing;
  c.close();
});

test("a base loads read-only with its blobs, and wrong blob sizes are refused", async () => {
  const { c, s } = connect();
  const svc = createLevelService(c, { blobTimeoutMs: 300 });
  const scene = scene12();
  const loading = svc.loadBase("Multi.Synth");
  assert.deepEqual([s.last().m, s.last().p], ["level.load", { base: "Multi.Synth" }]);
  s.reply(scene);
  await tick();
  for (const b of scene.blobs) s.bin(b.ref, new Uint8Array(b.bytes).fill(1), { ref: b.ref, kind: b.kind, frame: b.frame });
  const { blobs } = await loading;
  assert.equal(blobs.size, scene.blobs.length);

  const again = receiveBlobs(c, scene, 300);
  s.bin(scene.blobs[0].ref, new Uint8Array(scene.blobs[0].bytes + 4));
  await assert.rejects(again, (e: unknown) => e instanceof ErgError && /expected/.test((e as Error).message));

  const bad = svc.loadBase("Multi.Synth");
  s.reply({ ...scene, format: "erg-scene/2" });
  await assert.rejects(bad, (e: unknown) => e instanceof ErgError);

  const late = receiveBlobs(c, scene, 50);
  await assert.rejects(late, (e: unknown) => e instanceof ErgError && /did not arrive/.test((e as Error).message));
  c.close();
});

test("blobs sent before the handlers register are still delivered", async () => {
  const { c, s } = connect();
  const scene = scene12();
  for (const b of scene.blobs) s.bin(b.ref, new Uint8Array(b.bytes).fill(2));
  const blobs = await receiveBlobs(c, scene, 300);
  assert.equal(blobs.size, scene.blobs.length);
  assert.equal(new Uint8Array(blobs.get(scene.blobs[0].ref)!)[0], 2);
  c.close();
});
