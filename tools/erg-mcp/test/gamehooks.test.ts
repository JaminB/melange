// The game tools' new paths against a stub of the game's Oasis (render.camera, render.shot, level.test.explode,
// lua.eval): game_camera without a mod and its presets, game_shots in one call, test_explosion's predicted diff, apply
// and game run, place_blast_points, and runTest ignoring an earlier Test's end. Bundled for Node by test.ps1.
import assert from "node:assert/strict";
import { mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, isSolid, type Loaded, type Scene } from "../src/core";
import { runTest } from "../src/lib/place-wait";
import { grid } from "../src/lib/voxel";
import { tools as blastTools } from "../src/tools/blast";
import { tools as gameTools } from "../src/tools/game";
import { tools as terrainTools } from "../src/tools/terrain";
import type { Session } from "../src/session";

function loaded(): Loaded {
  const s: Scene = JSON.parse(readFileSync(new URL("../../../../tests/fixtures/erg/synthetic-12.json", import.meta.url), "utf8"));
  s.frames.find((f) => f.id === 1)!.name = "Scene";
  const blobs = new Map<number, ArrayBuffer>();
  for (const [ref, buf] of syntheticBlobs(s) as Map<number, Uint8Array>) blobs.set(ref, buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength) as ArrayBuffer);
  return { scene: s, blobs };
}

/** A game: frames advance 3 per call; render.shot writes a tiny file; it records what was called. */
function game(methods = ["lua.eval", "mods.list", "render.camera", "render.shot", "level.test.explode"]) {
  const dir = mkdtempSync(join(tmpdir(), "ergmcp-shots-"));
  let frame = 100;
  const calls: { m: string; p: any }[] = [];
  const client = {
    async call(m: string, p: any) {
      calls.push({ m, p });
      frame += 3;
      if (m === "render.camera") return p.clear ? { cleared: true, frame } : { set: true, frame, frames: p.frames };
      if (m === "render.shot") {
        if (p.minFrame && frame < p.minFrame) return { ready: false, frame, readyAt: p.minFrame };
        const path = join(dir, `${p.name}.png`);
        writeFileSync(path, new Uint8Array([137, 80, 78, 71]));
        return { ready: true, path, width: p.width ?? 1280, height: 720, frame, clean: !!p.clean };
      }
      if (m === "level.test.explode") return { queued: true, tick: 7 };
      if (m === "mods.list") return [];
      if (m === "lua.eval") return { ok: true, text: "true;nil" };
      throw new Error(`unexpected ${m}`);
    },
  };
  const store = new EditorStore("t", loaded(), loaded());
  const project = { store, id: "t", info: {} };
  const s = { requireProject: () => project, get store() { return store; }, project, ensure: async () => ({ methods }), client: async () => client } as unknown as Session;
  return { s, store, calls };
}
const all = [...gameTools, ...blastTools, ...terrainTools];
const call = async (s: Session, name: string, args: Record<string, unknown>): Promise<any> => all.find((t) => t.name === name)!.handler(s, args);
const textOf = (r: { content: { type: string; text?: string }[] }) => JSON.parse(r.content.find((c) => c.type === "text")!.text!);

test("game_camera: render.camera with no mod, presets default and overview, a shot", async () => {
  const { s, calls } = game();
  const set = await call(s, "game_camera", { pos: [0, 10, 20], target: [0, 0, 0] });
  assert.equal(set.owner, "oasis");
  assert.deepEqual(calls.at(-1)!.p.pos, [0, 200, 400]);
  assert.ok(!calls.some((c) => c.m === "mods.list"), "no mod is looked for");
  const d = await call(s, "game_camera", { preset: "default" });
  assert.equal(d.cleared, true);
  const o = await call(s, "game_camera", { preset: "overview", shot: true, name: "ov" });
  const t = textOf(o);
  assert.equal(t.preset, "overview");
  assert.equal(o.content[0].type, "image");
  const shot = calls.filter((c) => c.m === "render.shot").at(-1)!;
  assert.equal(shot.p.clean, true);
  assert.ok(shot.p.minFrame > 0);
  // an older game: the override goes through a client mod, and with none enabled it is refused
  const old = game(["lua.eval", "mods.list"]);
  await assert.rejects(() => call(old.s, "game_camera", { pos: [0, 10, 20], target: [0, 0, 0] }), /enabled mod/);
});

test("game_shots: several poses in one call, the camera given back", async () => {
  const { s, calls } = game();
  const r = await call(s, "game_shots", { cams: [{ pos: [0, 5, 9], target: [0, 0, 0], name: "a" }, { preset: "overview" }, { preset: "default" }], width: 640 });
  const t = textOf(r);
  assert.equal(t.shots.length, 3);
  assert.equal(r.content.filter((c: { type: string }) => c.type === "image").length, 3);
  // every name is prefixed (default erg_<yyyymmdd_hhmmss>), so runs never overwrite each other's in the shared folder
  assert.match(t.shots[0].name, /^erg_\d{8}_\d{6}_a$/);
  assert.deepEqual(t.shots.map((x: { name: string }) => x.name.replace(/^erg_\d{8}_\d{6}_/, "")), ["a", "2", "3"]);
  assert.ok(calls.at(-1)!.m === "render.camera" && calls.at(-1)!.p.clear, "the camera goes back at the end");
  await assert.rejects(() => call(game(["lua.eval"]).s, "game_shots", { cams: [{ preset: "default" }] }), /render.camera and render.shot/);
});

test("test_explosion: the predicted voxel diff, apply, and the game run at a blast knot", async () => {
  const { s, store, calls } = game();
  const b = await call(s, "add_block", { size: [8, 8, 8], material: 2, at: [520, 0, 0], units: "map" });
  const p = await call(s, "test_explosion", { at: [520, 8, 0], radius: 3, units: "map" });
  assert.ok(p.removed > 20 && p.frames[0].id === b.frameId, JSON.stringify(p));
  const before = grid(store, b.frameId).words.filter(isSolid).length;
  const a = await call(s, "test_explosion", { at: [520, 8, 0], radius: 3, units: "map", apply: true });
  assert.equal(grid(store, b.frameId).words.filter(isSolid).length, before - a.removed);
  store.undo();
  assert.equal(grid(store, b.frameId).words.filter(isSolid).length, before);
  await assert.rejects(() => call(s, "test_explosion", { at: [520, 8, 0], radius: 3, units: "map", game: true }), /ERGBLAST_/);
  const k = await call(s, "place_blast_points", { points: [[520, 8, 0]], units: "map" });
  assert.equal(k.placed[0].name, "ERGBLAST_0");
  assert.ok(store.scene.details.some((d) => d.name === "ERGBLAST_0"));
  const g = await call(s, "test_explosion", { at: [520, 8, 0], radius: 3, units: "map", game: true, settleMs: 0 });
  const t = textOf(g);
  assert.equal(t.game.detail, "ERGBLAST_0");
  const ex = calls.find((c) => c.m === "level.test.explode")!;
  assert.equal(ex.p.detail, "ERGBLAST_0");
  assert.equal(ex.p.landRadius, 60);
  assert.equal(g.content.filter((c: { type: string }) => c.type === "image").length, 2);
});

test("runTest: an earlier Test's end on the channel is not this run's", async () => {
  let listener: ((s: { state: string; key: string; detail: string }) => void) | undefined;
  const erg = {
    onTest(fn: typeof listener) { listener = fn; fn!({ state: "ended", key: "Multi.ergtest_t", detail: "" }); return () => { listener = undefined; }; },
    async test() {
      setTimeout(() => listener?.({ state: "playing", key: "Multi.ergtest_t", detail: "" }), 20);
      return { key: "Multi.ergtest_t", state: "starting" };
    },
  };
  const r = await runTest(erg as never, { timeoutMs: 2000 });
  assert.equal(r.final, "playing");
  assert.ok(!r.states.some((x) => x.state === "ended"));
});
