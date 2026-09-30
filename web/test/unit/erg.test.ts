import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import { createClient } from "../../src/sdk/client";
import {
  AddDetail, CommandStack, RemoveDetail, SetDetail, SetLevel, SetVoxels, apply, applyPatch, createErgSession, deriveRole, frameLocal,
  frameWorld, isEmptyPatch,
  toPatch, validatePatch, validateScene, voxelRuns, type Command, type Patch, type Scene, type Vec3,
} from "../../src/sdk/erg";

const fixture = (name: string) => readFileSync(new URL(`../../../tests/fixtures/erg/${name}`, import.meta.url), "utf8");
const scene12 = (): Scene => JSON.parse(fixture("synthetic-12.json"));
const scene400 = (): Scene => JSON.parse(fixture("synthetic-400.json"));

// Same generator as the C++ test (an LCG), so the command sequences are reproducible.
function rng(seed: number) {
  let s = seed >>> 0;
  return () => ((s = (Math.imul(s, 1664525) + 1013904223) >>> 0) >>> 8) / 16777216;
}

test("the synthetic fixtures validate", () => {
  for (const s of [scene12(), scene400()]) {
    const v = validateScene(s);
    assert.ok(v.ok, v.errors.join("; "));
  }
  assert.equal(scene400().frames.length, 403);
});

test("level.load's previews are optional and merge-friendly", () => {
  const s = scene12() as Scene & { previews?: Record<string, string> };
  s.previews = { CheesyGrinWorm: "detail/abc123.glb" };
  const v = validateScene(s);
  assert.ok(v.ok, v.errors.join("; "));
  delete s.previews;
  assert.ok(validateScene(s).ok);
});

test("scene refusals", () => {
  const cases: [string, (s: Scene) => void][] = [
    ["previews value", (s) => ((s as unknown as { previews: unknown }).previews = { x: 5 })],
    ["format", (s) => ((s as { format: string }).format = "erg-scene/3")],
    ["unknown key", (s) => ((s as unknown as Record<string, unknown>).extra = 1)],
    ["long title", (s) => (s.title = "x".repeat(41))],
    ["blob size", (s) => (s.blobs[0].bytes += 4)],
    ["missing parent", (s) => (s.frames[3].parent = 999999)],
    ["cycle", (s) => { s.frames[3].parent = s.frames[4].id; s.frames[4].parent = s.frames[3].id; }],
    ["duplicate detail id", (s) => (s.details[1].id = s.details[0].id)],
    ["detail frame", (s) => (s.details[0].frame = 777777)],
    ["role", (s) => ((s.details[0] as { role: string }).role = "weapon")],
    ["hmp paint", (s) => ((s.hmp as { mode: string }).mode = "paint")],
    ["water", (s) => (s.water.level = 5000)],
    ["theme", (s) => (s.databank.theme = "MOON")],
    ["two roots", (s) => (s.frames[1].parent = null)],
  ];
  for (const [what, mutate] of cases) {
    const s = scene12();
    mutate(s);
    assert.equal(validateScene(s).ok, false, what);
  }
});

test("the sample patch validates and applies", () => {
  const p: Patch = JSON.parse(fixture("synthetic-12.ergpatch.json"));
  const v = validatePatch(p);
  assert.ok(v.ok, v.errors.join("; "));
  const out = applyPatch(scene12(), p);
  const worm = out.details.find((d) => d.name === "WORM0")!;
  assert.deepEqual(worm.pos, [12.5, 3, -4]);
  assert.equal(out.databank.theme, "CAMELOT");
  assert.equal(out.water.level, 40);
  assert.equal(out.details.at(-1)!.role, "object");
  assert.ok(validateScene(out).ok);
});

test("patch refusals name the op index", () => {
  const base: Patch = JSON.parse(fixture("synthetic-12.ergpatch.json"));
  const cases: [string, (p: Patch) => void, string][] = [
    ["unknown op", (p) => ((p.ops[2] as { op: string }).op = "explode"), "ops[2]"],
    ["short vector", (p) => ((p.ops[0] as { pos: number[] }).pos = [1, 2]), "ops[0]"],
    ["empty name", (p) => ((p.ops[1] as { detail: { name: string } }).detail.name = ""), "ops[1]"],
    ["set without fields", (p) => (p.ops[0] = { op: "set", src: 5 }), "ops[0]"],
    ["high voxel bits", (p) => p.ops.push({ op: "voxels", frame: 4, runs: [[0, 1, 0x1000003]] }), "ops[3]"],
    ["solid bits 1", (p) => p.ops.push({ op: "voxels", frame: 4, runs: [[0, 1, 1]] }), "ops[3]"],
    ["stem with dot", (p) => (p.stem = "my.maps"), "stem"],
    ["short hash", (p) => (p.base.sha256.xan = "abc"), "sha256"],
  ];
  for (const [what, mutate, needle] of cases) {
    const p: Patch = structuredClone(base);
    mutate(p);
    const v = validatePatch(p);
    assert.equal(v.ok, false, what);
    assert.ok(v.errors.some((e) => e.includes(needle)), `${what}: ${v.errors.join("; ")}`);
  }
  const many: Patch = structuredClone(base);
  many.ops = Array.from({ length: 20001 }, () => ({ op: "remove" as const, src: 5 }));
  assert.equal(validatePatch(many).ok, false);
  const runs: Patch = structuredClone(base);
  runs.ops = [{ op: "voxels", frame: 4, runs: Array.from({ length: 2001 }, (_, i) => [i, 1, 0] as [number, number, number]) }];
  assert.ok(validatePatch(runs).errors.some((e) => e.includes("2000")));
});

test("toPatch of an unedited scene is empty", () => {
  const s = scene12();
  const p = toPatch(s, structuredClone(s));
  assert.ok(isEmptyPatch(p, s));
  assert.ok(validatePatch({ ...p, stem: "mymaps_x" }).ok);
});

test("toPatch then applyPatch reproduces the edit", () => {
  const base = scene12();
  const edited = structuredClone(base);
  const stack = new CommandStack(edited);
  stack.exec(new SetDetail(edited.details[0].id, { pos: [1, 2, 3] }));
  stack.exec(new SetDetail(edited.details[1].id, { name: "WORM7x", rot: [0, 1, 0] }));
  stack.exec(new RemoveDetail(edited.details[9].id));
  stack.exec(new AddDetail(edited.details[8].frame, { name: "mine", resource: "Mine", pos: [4, 5, 6] }));
  stack.exec(new SetLevel({ water: 40, spawns: "knots", databank: { theme: "CAMELOT" } }));
  edited.stem = "mymaps_x";
  const p = toPatch(base, edited);
  assert.ok(validatePatch(p).ok, validatePatch(p).errors.join("; "));
  assert.deepEqual(p.ops.map((o) => o.op), ["set", "set", "remove", "add"]);
  const again = applyPatch(base, p);
  const norm = (s: Scene) => s.details.map((d) => ({ ...d, id: d.src === null ? 0 : d.id }));
  assert.deepEqual(norm(again), norm(edited));
  assert.equal(again.water.level, 40);
  assert.equal(again.spawns.mode, "knots");
  assert.equal(again.databank.theme, "CAMELOT");
});

test("undo and redo return byte-identical patches; drags merge", () => {
  const base = scene12();
  const s = structuredClone(base);
  s.stem = "mymaps_x";
  const stack = new CommandStack(s);
  const id = s.details[0].id;
  for (let i = 1; i <= 10; i++) stack.exec(new SetDetail(id, { pos: [i, 0, 0] }), true);
  assert.equal(stack.depth, 1, "a drag is one undo step");
  stack.exec(new AddDetail(s.details[8].frame, { name: "oildrum", resource: "OilDrum", pos: [1, 1, 1] }));
  stack.exec(new SetLevel({ water: 25 }));
  const a = JSON.stringify(toPatch(base, s));
  while (stack.undo());
  assert.ok(isEmptyPatch({ ...toPatch(base, s), stem: base.stem }, base));
  while (stack.redo());
  assert.equal(JSON.stringify(toPatch(base, s)), a);
});

test("500 random commands then 500 undos give an empty patch", () => {
  const base = scene12();
  const s = structuredClone(base);
  const stack = new CommandStack(s);
  const r = rng(42);
  const vec = (): Vec3 => [Math.round(r() * 100), Math.round(r() * 10), Math.round(r() * 100)];
  for (let i = 0; i < 500; i++) {
    const k = r();
    let cmd: Command;
    if (k < 0.5 || s.details.length < 4) cmd = new SetDetail(s.details[Math.floor(r() * s.details.length)].id, { pos: vec() });
    else if (k < 0.7) cmd = new AddDetail(s.details[8].frame, { name: "oildrum", resource: "OilDrum", pos: vec() });
    else if (k < 0.85) cmd = new RemoveDetail(s.details[Math.floor(r() * s.details.length)].id);
    else cmd = new SetLevel({ water: Math.round(r() * 100) });
    stack.exec(cmd);
  }
  assert.equal(stack.depth, 500);
  assert.equal(isEmptyPatch(toPatch(base, s), base), false);
  let undone = 0;
  while (stack.undo()) undone++;
  assert.equal(undone, 500);
  assert.ok(isEmptyPatch(toPatch(base, s), base));
  assert.deepEqual(s, base);
});

test("the stack keeps at most 500 steps", () => {
  const s = scene12();
  const stack = new CommandStack(s);
  for (let i = 0; i < 520; i++) stack.exec(new SetLevel({ water: i }));
  assert.equal(stack.depth, 500);
});

test("voxel runs and SetVoxels", () => {
  const before = new Uint32Array([3, 3, 3, 3, 0, 0, 3, 3]);
  const after = before.slice();
  after.fill(0, 1, 4);
  after[6] = 3 | (7 << 2);
  assert.deepEqual(voxelRuns(before, after), [[1, 3, 0], [6, 1, 31]]);
  const base = scene12();
  const f = base.frames[3];
  const n = f.size[0] * f.size[1] * f.size[2];
  const v0 = new Uint32Array(n).fill(3), v1 = v0.slice();
  const blobs = new Map([[f.voxels!, v1]]);
  const cmd = new SetVoxels(blobs, f.voxels!, new Map([[0, [3, 0]], [2, [3, 0]]]));
  const stack = new CommandStack(structuredClone(base));
  stack.exec(cmd);
  const p = toPatch(base, base, { base: new Map([[f.voxels!, v0]]), edited: blobs });
  assert.deepEqual(p.ops, [{ op: "voxels", frame: f.id, runs: [[0, 1, 0], [2, 1, 0]] }]);
  stack.undo();
  assert.deepEqual([...v1], [...v0]);
  const target = v0.slice();
  applyPatch(base, p, new Map([[f.voxels!, target]]));
  assert.equal(target[0], 0);
  assert.equal(target[1], 3);
});

test("frame transforms match the engine's composition", () => {
  const near = (a: number[], b: number[]) => a.every((v, i) => Math.abs(v - b[i]) < 1e-9);
  assert.ok(near(apply(frameLocal({ pos: [0, 0, 0], rot: [0, Math.PI / 2, 0], scale: [1, 1, 1] }), [1, 0, 0]), [0, 0, -1]));
  assert.ok(near(apply(frameLocal({ pos: [0, 0, 0], rot: [Math.PI / 2, 0, Math.PI / 2], scale: [1, 1, 1] }), [0, 1, 0]), [0, 0, 1]));
  const s = scene12();
  s.frames[0].scale = [0, 0, 0];
  const frames = new Map(s.frames.map((f) => [f.id, f]));
  const worms = s.frames[1];
  const w = apply(frameWorld(frames, worms.id)!, s.details[0].pos);
  assert.ok(near(w, s.details[0].pos), "a folder frame at the origin under the root keeps positions");
  assert.equal(frameWorld(frames, 999999), null);
});

test("roles match the server's", () => {
  assert.equal(deriveRole("WORM3", "CheesyGrinWorm"), "spawn");
  assert.equal(deriveRole("oildrum", "OilDrum"), "object");
  assert.equal(deriveRole("Camera1", "Camera"), "camera");
  assert.equal(deriveRole("LIGHT PNTLGHT 1 1 1 30", "LIGHT"), "light");
  assert.equal(deriveRole("VISIBLE_x", "BUILDING15"), "scenery");
  assert.equal(deriveRole("thing", "Unknown"), "other");
});

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
  bin(ref: number, payload: Uint8Array) {
    this.recv({ t: "bin", ref, ch: "erg", len: payload.length, meta: { ref } });
    const buf = new Uint8Array(payload.length + 4);
    new DataView(buf.buffer).setUint32(0, ref, true);
    buf.set(payload, 4);
    this.onmessage?.({ data: buf.buffer } as MessageEvent);
  }
}

const welcome = { t: "welcome", proto: 1, build: "b1", server: "game", channels: ["erg"], methods: ["level.load"], panels: [] };
const tick = (ms = 0) => new Promise((r) => setTimeout(r, ms));

test("onBinary delivers frames that arrive before and after the handler", async () => {
  FakeSocket.all = [];
  const c = createClient({ url: "ws://x/ws", build: "b1", socket: (u) => new FakeSocket(u) as any });
  const s = FakeSocket.all[0];
  s.open();
  s.recv(welcome);
  assert.equal(s.binaryType, "arraybuffer");
  s.bin(7, new Uint8Array([1, 2, 3]));
  const got: number[][] = [];
  const off = c.onBinary(7, (d, meta) => got.push([...new Uint8Array(d), (meta as { ref: number }).ref]));
  assert.equal(got.length, 0, "a buffered frame is delivered asynchronously");
  await tick();
  s.bin(7, new Uint8Array([4]));
  off();
  s.bin(7, new Uint8Array([5]));
  assert.deepEqual(got, [[1, 2, 3, 7], [4, 7]]);
  c.close();
});

test("an Erg session loads a scene and its blobs", async () => {
  FakeSocket.all = [];
  const c = createClient({ url: "ws://x/ws", build: "b1", socket: (u) => new FakeSocket(u) as any });
  const s = FakeSocket.all[0];
  s.open();
  s.recv(welcome);
  const session = createErgSession(c, { blobTimeoutMs: 500 });
  const scene = scene12();
  const loading = session.load("p1");
  const call = s.sent.at(-1);
  assert.equal(call.m, "level.load");
  assert.deepEqual(call.p, { project: "p1" });
  s.recv({ t: "res", id: call.id, r: scene });
  await tick();
  for (const b of scene.blobs) s.bin(b.ref, new Uint8Array(b.bytes).fill(b.ref & 0xff));
  const { blobs } = await loading;
  assert.equal(blobs.size, scene.blobs.length);
  assert.equal(new Uint8Array(blobs.get(scene.blobs[0].ref)!)[0], scene.blobs[0].ref & 0xff);
  const saving = session.save({ ...toPatch(scene, scene), stem: "mymaps_x" });
  const save = s.sent.at(-1);
  assert.equal(save.m, "level.save");
  s.recv({ t: "res", id: save.id, r: { saved: true, warnings: [] } });
  assert.deepEqual(await saving, { saved: true, warnings: [] });
  c.close();
});
