// The place group's logic without a server, bundled for Node by test.ps1 into test/out: placing is the panel's own
// (model/placing.ts) command, a refused edit leaves no trace on the undo stack, and runTest follows the 'erg' channel.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { AddDetail, BUILTIN, EditorStore, SetDetail, place, type ErgSession, type Loaded, type Scene } from "../src/core";
import { commit, detailInfo, resolvePoint } from "../src/lib/place-geom";
import { runTest } from "../src/lib/place-wait";
import { tools } from "../src/tools/place";
import type { Session } from "../src/session";

function loaded(): Loaded {
  const s: Scene = JSON.parse(readFileSync(new URL("../../../../tests/fixtures/erg/synthetic-12.json", import.meta.url), "utf8"));
  const blobs = new Map<number, ArrayBuffer>();
  for (const [ref, buf] of syntheticBlobs(s) as Map<number, Uint8Array>) blobs.set(ref, buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength) as ArrayBuffer);
  return { scene: s, blobs };
}
const store = () => new EditorStore("t", loaded(), loaded());
const fake = (st: EditorStore) => ({ store: st, requireProject: () => ({ store: st }), client: async () => ({ call: async () => ({}) }) }) as unknown as Session;
const rounded = (v: unknown) => JSON.parse(JSON.stringify(v), (_k, x) => (typeof x === "number" ? Math.round(x * 1e6) / 1e6 : x));
const tool = (name: string) => tools.find((t) => t.name === name)!;

test("placed objects are the ones the palette places", async () => {
  for (const [type, id, args] of [
    ["telepad", "telepads", {}], ["trigger", "trigger", {}], ["minefactory", "minefactory", {}], ["crate", "crate", {}],
    ["mine", "mine", {}], ["oildrum", "oildrum", {}],
  ] as const) {
    const a = store(), b = store();
    const world: [number, number, number] = [12, 34, 56];
    const cmd = place(a.scene, a.frames, BUILTIN.find((e) => e.id === id)!, [world[0] / 20, world[1] / 20, world[2] / 20], null);
    assert.ok(typeof cmd !== "string", String(cmd));
    a.exec(cmd);
    await tool("place_object").handler(fake(b), { type, pos: world, ...args });
    // positions agree up to float noise (the tool rounds to 6 places; the panel keeps what the matrix gives)
    assert.deepEqual(rounded(b.scene.details), rounded(a.scene.details), type);
    assert.deepEqual(b.scene.objects, a.scene.objects, type);
    assert.equal(b.stack.depth, 1, `${type} is one undo step`);
  }
});

test("an edit that would break the patch is refused and leaves the stack alone", () => {
  const st = store();
  const d = st.scene.details[0];
  assert.throws(() => commit(st, new SetDetail(d.id, { pos: [NaN, 0, 0] })), /would be invalid/);
  assert.equal(st.stack.depth, 0);
  assert.equal(st.stack.canRedo, false);
  assert.deepEqual(st.scene.details[0].pos, d.pos);
  commit(st, new SetDetail(d.id, { pos: [1, 2, 3] }));
  assert.equal(st.stack.depth, 1);
  // problems the patch already had are not blamed on the edit
  commit(st, new AddDetail(d.frame, { name: "x", resource: "y", pos: [0, 0, 0] }));
  assert.equal(st.stack.depth, 2);
});

test("points: pos, xz drops, units", () => {
  const st = store();
  assert.deepEqual(resolvePoint(st, { pos: [20, 40, 60] }).map, [1, 2, 3]);
  assert.deepEqual(resolvePoint(st, { pos: [1, 2, 3] }, "map").map, [1, 2, 3]);
  const hit = resolvePoint(st, { xz: [-104.5, 45] }, "map");
  assert.equal(hit.dropped, true);
  assert.ok(hit.map[1] > 0 && hit.map[1] < 30, String(hit.map));
  assert.throws(() => resolvePoint(st, { xz: [1e6, 0] }, "map", "p"), /p: nothing below/);
  assert.throws(() => resolvePoint(st, {}), /exactly one/);
  const worm0 = st.scene.details.find((d) => d.name === "WORM0")!;
  assert.equal(detailInfo(st, worm0).id, 16);
  assert.deepEqual(detailInfo(st, worm0, "map").pos, [52, 10, -58]);
  assert.deepEqual(detailInfo(st, worm0).pos, [1040, 200, -1160]);
});

function fakeErg(script: { delay: number; state: string; detail?: string }[], first = "registering"): Pick<ErgSession, "test" | "onTest"> {
  let fn: ((s: { state: string; key: string; detail: string }) => void) | undefined;
  return {
    onTest(f) { fn = f; return () => { fn = undefined; }; },
    async test() {
      for (const s of script) setTimeout(() => fn?.({ state: s.state, key: "Multi.k", detail: s.detail ?? "" }), s.delay);
      return { key: "Multi.k", state: first };
    },
  };
}

test("runTest waits for playing, failed or the timeout", async () => {
  const ok = await runTest(fakeErg([{ delay: 5, state: "registered" }, { delay: 10, state: "armed" }, { delay: 15, state: "starting" }, { delay: 25, state: "playing" }]), { timeoutMs: 2000 });
  assert.deepEqual(ok.states.map((s) => s.state), ["registering", "registered", "armed", "starting", "playing"]);
  assert.equal(ok.reached, true);
  assert.equal(ok.final, "playing");
  assert.equal(ok.key, "Multi.k");
  assert.equal(ok.timedOut, false);

  const armed = await runTest(fakeErg([{ delay: 5, state: "armed" }, { delay: 5000, state: "playing" }]), { goal: "armed", timeoutMs: 2000 });
  assert.equal(armed.final, "armed");
  assert.equal(armed.reached, true);

  const failed = await runTest(fakeErg([{ delay: 5, state: "failed", detail: "the game is in a match" }]), { timeoutMs: 2000 });
  assert.equal(failed.reached, false);
  assert.equal(failed.failed, "the game is in a match");
  assert.equal(failed.timedOut, false);

  const slow = await runTest(fakeErg([{ delay: 5, state: "armed" }]), { timeoutMs: 80 });
  assert.equal(slow.timedOut, true);
  assert.equal(slow.reached, false);
  assert.equal(slow.final, "armed");

  // the call's own answer can be the end state; repeated events are folded
  const instant = await runTest(fakeErg([{ delay: 1, state: "playing" }, { delay: 2, state: "playing" }], "playing"), { timeoutMs: 500 });
  assert.equal(instant.reached, true);
  assert.equal(instant.states.length, 1);
  assert.equal(instant.cleanRoom, undefined);
});

test("runTest passes the clean-room options and reports what the game applied", async () => {
  const seen: unknown[] = [];
  const erg: Pick<ErgSession, "test" | "onTest"> = {
    onTest() { return () => {}; },
    async test(opts) {
      seen.push(opts);
      return { key: "Multi.k", state: "playing", ...(opts?.cleanMods || opts?.postfx === false ? { cleanRoom: { simMods: !opts?.cleanMods, postfx: opts?.postfx ?? !opts?.cleanMods } } : {}) };
    },
  };
  const clean = await runTest(erg, { timeoutMs: 100, cleanMods: true });
  assert.deepEqual(clean.cleanRoom, { simMods: false, postfx: false });
  const dark = await runTest(erg, { timeoutMs: 100, tod: "NIGHT", postfx: false });
  assert.deepEqual(dark.cleanRoom, { simMods: true, postfx: false });
  await runTest(erg, { timeoutMs: 100 });
  assert.deepEqual(seen, [{ cleanMods: true }, { tod: "NIGHT", postfx: false }, undefined]);
});
