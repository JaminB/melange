import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import {
  AddDetail, AddObjects, KNOT_RESOURCE, apply, type CommandStack, PATCH_FORMAT_2, RemoveDetail, SetDetail, SetLevel, SetObject, THEMES, applyPatch, deriveRole,
  frameWorld, isEmptyPatch, validatePatch, validatePatchObjects, type Command, type Scene, type Vec3,
} from "../../src/sdk/erg";
import { catalogOf, objectOf } from "../../src/panels/erg/model/placing";
import { checkScene } from "../../src/panels/erg/model/checks";
import { clearDraft, readDraft, writeDraft } from "../../src/panels/erg/model/draft";
import { Duplicate, Group, setMany } from "../../src/panels/erg/model/edits";
import { Frames, invert, snap, translationOnlyFrames } from "../../src/panels/erg/model/geometry";
import { dropPoint, rayVoxels } from "../../src/panels/erg/model/ground";
import { withPatch } from "../../src/panels/erg/model/loader";
import { BUILTIN, SCENERY_COPIES, nextKnot, paletteFrom, place, targetFrame } from "../../src/panels/erg/model/placing";
import { EditorStore, type Loaded } from "../../src/panels/erg/model/store";
import { Sculptor } from "../../src/panels/erg/terrain";
import { frameQuads, meshFrames } from "../../src/panels/erg/terrain/mesher";
import { themePalette } from "../../src/panels/erg/terrain/materials";
import { buckets, meshInput } from "../../src/panels/erg/terrain/pool";
import { listOf, projectOf, validSlug } from "../../src/panels/erg/ui/Home";
import { themesOf } from "../../src/panels/erg/ui/LevelSettings";

const fixture = (name: string) => readFileSync(new URL(`../../../tests/fixtures/erg/${name}`, import.meta.url), "utf8");
const scene12 = (): Scene => JSON.parse(fixture("synthetic-12.json"));
const scene400 = (): Scene => JSON.parse(fixture("synthetic-400.json"));

function rng(seed: number) {
  let s = seed >>> 0;
  return () => ((s = (Math.imul(s, 1664525) + 1013904223) >>> 0) >>> 8) / 16777216;
}

/** Terrain for a synthetic scene: a height field per frame (the same generator as the e2e mock server). */
export function syntheticBlobs(s: Scene): Map<number, ArrayBuffer> {
  const out = new Map<number, ArrayBuffer>();
  for (const f of s.frames) {
    const [X, Y, Z] = f.size;
    if (f.voxels !== null) {
      const v = new Uint32Array(X * Y * Z);
      for (let z = 0; z < Z; z++)
        for (let x = 0; x < X; x++) {
          const h = Math.max(1, Math.min(Y, 1 + Math.floor((Y - 1) * (0.5 + 0.5 * Math.sin(x * 0.7 + f.id) * Math.cos(z * 0.5 + f.id * 0.3)))));
          for (let y = 0; y < h; y++) v[(z * X + x) * Y + y] = 3 | (((f.id + y) % 16) << 2);
        }
      out.set(f.voxels, v.buffer);
    }
    if (f.heightMap !== null) out.set(f.heightMap, new Float32Array((X + 1) * (Z + 1)).buffer);
  }
  return out;
}

const loaded = (s: Scene): Loaded => ({ scene: s, blobs: syntheticBlobs(s) });
const newStore = (s = scene12(), limit?: number) => new EditorStore("p1", loaded(s), loaded(structuredClone(s)), limit);

// ------------------------------------------------------------------ mesher
const vox = (size: number[], solid: [number, number, number, number][]) => {
  const v = new Uint32Array(size[0] * size[1] * size[2]);
  for (const [x, y, z, m] of solid) v[(z * size[0] + x) * size[1] + y] = 3 | (m << 2);
  return v;
};

test("mesher: face counts with greedy merging", () => {
  assert.equal(frameQuads([1, 1, 1], vox([1, 1, 1], [[0, 0, 0, 1]])).length / 8, 6);
  assert.equal(frameQuads([2, 1, 1], vox([2, 1, 1], [[0, 0, 0, 1], [1, 0, 0, 1]])).length / 8, 6, "same material merges");
  assert.equal(frameQuads([2, 1, 1], vox([2, 1, 1], [[0, 0, 0, 1], [1, 0, 0, 2]])).length / 8, 10, "materials stay apart");
  const full: [number, number, number, number][] = [];
  for (let x = 0; x < 3; x++) for (let y = 0; y < 3; y++) for (let z = 0; z < 3; z++) full.push([x, y, z, 5]);
  assert.equal(frameQuads([3, 3, 3], vox([3, 3, 3], full)).length / 8, 6, "a full block is six quads");
  assert.equal(frameQuads([3, 3, 3], new Uint32Array(27)).length, 0);
  const odd = vox([2, 1, 1], [[0, 0, 0, 1]]);
  odd[1] = 1 | (3 << 2);
  assert.equal(frameQuads([2, 1, 1], odd).length / 8, 6, "solid bits other than 3 count as empty");
  assert.equal(frameQuads([2, 2, 2], new Uint32Array(3)).length, 0, "a short array is ignored");
});

test("mesher: the quad budget bounds a checkerboard", () => {
  const n = 16, cells: [number, number, number, number][] = [];
  for (let x = 0; x < n; x++) for (let y = 0; y < n; y++) for (let z = 0; z < n; z++) if ((x + y + z) % 2 === 0) cells.push([x, y, z, 1]);
  const v = vox([n, n, n], cells);
  const all = frameQuads([n, n, n], v).length / 8;
  assert.ok(all > 10000, `${all} quads`);
  assert.equal(frameQuads([n, n, n], v, 100).length / 8, 100, "frameQuads stops at its budget");
  const frame = { size: [n, n, n] as [number, number, number], world: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0], voxels: v };
  const pal = new Uint8Array(192);
  const m = meshFrames([{ id: 1, ...frame }, { id: 2, ...frame }], pal, all + 10);
  assert.ok(m.truncated && m.quads === all && m.positions.length === all * 12, "a frame past the budget is left out");
  const whole = meshFrames([{ id: 1, ...frame }], pal, all);
  assert.ok(!whole.truncated && whole.quads === all, "a frame that fits exactly is kept");
});

function checkWinding(m: ReturnType<typeof meshFrames>) {
  for (let q = 0; q < m.quads; q++) {
    const i = [m.index[q * 6], m.index[q * 6 + 1], m.index[q * 6 + 2]];
    const p = i.map((k) => [m.positions[k * 3], m.positions[k * 3 + 1], m.positions[k * 3 + 2]]);
    const a = [p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]], b = [p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]];
    const n = [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
    const vn = [m.normals[i[0] * 3], m.normals[i[0] * 3 + 1], m.normals[i[0] * 3 + 2]];
    assert.ok(n[0] * vn[0] + n[1] * vn[1] + n[2] * vn[2] > 0, `quad ${q} faces its normal`);
  }
}

test("mesher: world transform, outward winding, mirrored frames, colours and frame ids", () => {
  const pal = themePalette("BUILDING");
  const v = vox([1, 1, 1], [[0, 0, 0, 7]]);
  const moved = meshFrames([{ id: 9, size: [1, 1, 1], world: [1, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 30], voxels: v }], pal);
  const xs = [], ys = [];
  for (let i = 0; i < moved.positions.length; i += 3) { xs.push(moved.positions[i]); ys.push(moved.positions[i + 1]); }
  assert.deepEqual([Math.min(...xs), Math.max(...xs), Math.min(...ys), Math.max(...ys)], [10, 11, 20, 21]);
  assert.deepEqual([...moved.quadFrame], [9, 9, 9, 9, 9, 9]);
  assert.deepEqual([...moved.colors.slice(0, 3)], [...pal.slice(21, 24)]);
  checkWinding(moved);
  const c = Math.cos(1), s = Math.sin(1);
  checkWinding(meshFrames([{ id: 1, size: [1, 1, 1], world: [c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0], voxels: v }], pal));
  checkWinding(meshFrames([{ id: 1, size: [1, 1, 1], world: [-2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0], voxels: v }], pal));
});

test("mesher: the synthetic 400-frame level meshes in buckets", () => {
  const s = scene400();
  const blobs = syntheticBlobs(s);
  const bs = buckets(s);
  assert.equal(bs.reduce((n, b) => n + b.frames.length, 0), 400);
  const t0 = performance.now();
  let quads = 0;
  for (const b of bs) {
    const m = meshFrames(meshInput(s, b.frames, (f) => (f.voxels !== null ? new Uint32Array(blobs.get(f.voxels)!) : undefined)), themePalette("CAMELOT"));
    quads += m.quads;
    checkWinding(m);
  }
  const ms = performance.now() - t0;
  assert.ok(quads > 5000, `${quads} quads`);
  assert.ok(ms < 1000, `meshing took ${ms.toFixed(0)} ms`);
});

// ------------------------------------------------------------------ geometry and ground
test("geometry: inverse matrices and local/world round trips", () => {
  const s = scene400();
  const frames = new Frames(s);
  const r = rng(3);
  for (const f of s.frames.slice(0, 60)) {
    const w = frameWorld(frames.byId, f.id)!;
    const inv = invert(w)!;
    const p: Vec3 = [r() * 100, r() * 10, r() * 100];
    const back = frames.toLocal(f.id, [
      w[0] * p[0] + w[1] * p[1] + w[2] * p[2] + w[3], w[4] * p[0] + w[5] * p[1] + w[6] * p[2] + w[7], w[8] * p[0] + w[9] * p[1] + w[10] * p[2] + w[11]]);
    for (let i = 0; i < 3; i++) assert.ok(Math.abs(back[i] - p[i]) < 1e-9);
    assert.ok(inv.every(Number.isFinite));
  }
  assert.equal(invert([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1]), null);
  assert.equal(snap(1.26, 0.5), 1.5);
  assert.equal(snap(-0.24, 0.5), 0);
  assert.equal(snap(0.12345678, null), 0.1235);
});

test("ground: voxel rays and drop to ground", () => {
  const slab = vox([4, 4, 4], [...Array(16)].map((_, i) => [i % 4, 1, Math.floor(i / 4), 0] as [number, number, number, number]));
  assert.equal(rayVoxels([4, 4, 4], slab, [1.5, 10, 1.5], [0, -1, 0]), 8);
  assert.equal(rayVoxels([4, 4, 4], slab, [5.5, 10, 1.5], [0, -1, 0]), null, "outside the box");
  assert.equal(rayVoxels([4, 4, 4], slab, [1.5, 0.5, 1.5], [0, -1, 0]), null, "below the slab");
  assert.ok(Math.abs(rayVoxels([4, 4, 4], slab, [-2, 3, 1.5], [1, -0.5, 0])! - 2) < 1e-9, "a slanted ray meets the top face");

  const s = scene12();
  const store = newStore(s);
  const f = s.frames.find((x) => x.name === "land1")!;
  const w = store.frames.worldOf(f.id)!;
  const top: Vec3 = [w[0] * 0.5 + w[2] * 0.5 + w[3], 40, w[8] * 0.5 + w[10] * 0.5 + w[11]];
  const hit = dropPoint(store.scene, store.frames, (id) => store.voxelsOf({ id }), top);
  assert.ok(hit && hit[1] < 40 && hit[1] > w[7], `landed at ${hit}`);
  assert.equal(dropPoint(store.scene, store.frames, (id) => store.voxelsOf({ id }), [5000, 40, 5000]), null);
});

test("translation-only frames: the recorded lamp bowls and anything under a HangingLamp", () => {
  const s = scene12();
  s.base.file = "Deathmatch1";
  s.frames.push({ ...s.frames[1], id: 900, parent: s.frames[0].id, name: "HangingLamp01", folder: false },
    { ...s.frames[1], id: 901, parent: 900, name: "Bowl" });
  const t = translationOnlyFrames(s);
  assert.ok(t.has(325) && t.has(900) && t.has(901));
  assert.ok(!t.has(s.frames[3].id));
  s.base.file = "Other";
  assert.ok(!translationOnlyFrames(s).has(325));
});

// ------------------------------------------------------------------ placing and palette
test("placing: knots, objects, target frames and refusals", () => {
  const s = scene12();
  const store = newStore(s);
  const worms = s.frames.find((f) => f.name === "Worms")!.id, objects = s.frames.find((f) => f.name === "Objects")!.id;
  assert.equal(targetFrame(store.scene, "spawn"), worms);
  assert.equal(targetFrame(store.scene, "object"), objects);
  assert.equal(nextKnot(store.scene), null);
  const knot = BUILTIN.find((e) => e.id === "knot")!, drum = BUILTIN.find((e) => e.id === "oildrum")!;
  assert.ok(/already exist/.test(String(place(store.scene, store.frames, knot, [0, 0, 0], null))));
  const cmd = place(store.scene, store.frames, drum, [10.26, 3, 7], 0.5);
  if (!(cmd instanceof AddDetail)) throw new Error(String(cmd));
  store.exec(cmd);
  const added = store.detail(cmd.detailId)!;
  assert.equal(added.frame, objects);
  assert.deepEqual(added.pos, [8.5, 3, 9], "local to the Objects folder at (2, 0, -2), snapped");
  assert.equal(added.role, "object");
  store.exec(new RemoveDetail(store.scene.details.find((d) => d.name === "WORM3")!.id));
  assert.equal(nextKnot(store.scene), "WORM3");
  const k = place(store.scene, store.frames, knot, [1, 2, 3], null) as AddDetail;
  store.exec(k);
  assert.equal(store.detail(k.detailId)!.name, "WORM3");
  assert.equal(store.detail(k.detailId)!.frame, worms);
  const scenery = place(store.scene, store.frames, { id: "x", label: "Box", name: "VISIBLE_x", resource: "BUILDING1", role: "scenery" }, [0, 0, 0], null);
  assert.equal(typeof scenery === "string", !SCENERY_COPIES);
});

test("objects: crates, a telepad pair, triggers and the mine factory place, edit, delete and undo", () => {
  const store = newStore();
  const entry = (id: string) => BUILTIN.find((e) => e.id === id)!;
  const put = (id: string, at: Vec3) => {
    const cmd = place(store.scene, store.frames, entry(id), at, null);
    if (!(cmd instanceof AddObjects)) throw new Error(String(cmd));
    store.exec(cmd);
    return cmd;
  };
  const empty = JSON.stringify(store.patch());
  put("crate", [1, 2, 3]);
  put("crate", [2, 2, 3]);
  const pads = put("telepads", [3, 2, 3]);
  put("telepads", [4, 2, 3]);
  put("trigger", [5, 2, 3]);
  put("minefactory", [6, 2, 3]);
  assert.ok(/already has a mine factory/.test(String(place(store.scene, store.frames, entry("minefactory"), [0, 0, 0], null))));
  assert.deepEqual(store.scene.objects!.map((o) => o.knot), ["CRATE_0", "CRATE_1", "TP_1_0", "TP_1_1", "TP_2_0", "TP_2_1", "TRIG_0", "minefactory"]);
  assert.equal(pads.ids.length, 2);
  for (const o of store.scene.objects!) {
    const d = store.scene.details.find((x) => x.name === o.knot)!;
    assert.equal(d.resource, KNOT_RESOURCE, "every knot is the non-visual marker");
    assert.equal(d.role, "object");
    assert.equal(objectOf(store.scene, d), o);
  }
  assert.equal(deriveRole("CRATE_0", KNOT_RESOURCE), "object");
  assert.equal(deriveRole("WORM0", KNOT_RESOURCE), "spawn");
  assert.ok(!store.scene.details.some((d) => d.name === "telepad"));
  store.exec(new SetObject("CRATE_1", { knot: "CRATE_1", type: "crate", crate: { kind: "weapon", contents: "kWeaponBazooka", count: 3, hitpoints: 25, parachute: true } }));

  const p = store.patch();
  assert.equal(p.format, PATCH_FORMAT_2);
  assert.deepEqual(validatePatch(p).errors, []);
  assert.deepEqual(validatePatchObjects(p, store.base).errors, []);
  assert.deepEqual(store.problems(), []);
  const replay = applyPatch(store.base, p);
  assert.deepEqual(replay.objects, store.scene.objects);

  const tp = store.scene.details.find((d) => d.name === "TP_2_1")!;
  store.exec(new RemoveDetail(tp.id));
  assert.ok(!store.scene.objects!.some((o) => o.knot === "TP_2_1"), "deleting a knot deletes its object");
  const issues = checkScene(store.scene, store.frames, store.problems());
  assert.ok(issues.some((i) => i.level === "warn" && /Telepad group 2 has one pad/.test(i.text) && i.detail !== undefined));
  store.undo();
  assert.ok(store.scene.objects!.some((o) => o.knot === "TP_2_1"));
  assert.ok(!checkScene(store.scene, store.frames).some((i) => /one pad/.test(i.text)));

  store.exec(new SetLevel({ water: 1000 }));
  assert.ok(checkScene(store.scene, store.frames).some((i) => /^CRATE_0 #\d+ is under water/.test(i.text)));
  store.undo();

  const before = JSON.stringify(store.patch());
  store.undo();
  const back = store.scene.objects!.find((o) => o.knot === "CRATE_1")!;
  assert.ok(back.type === "crate" && back.crate.kind === "health");
  store.redo();
  assert.equal(JSON.stringify(store.patch()), before, "undo and redo of an object edit are byte-identical");
  while (store.undo());
  assert.equal(JSON.stringify(store.patch()), empty, "undoing every placement gives the empty patch");
  assert.ok(!store.scene.details.some((d) => /^(CRATE|TP|TRIG)_|^minefactory$/.test(d.name)));
});

test("objects: the install's catalog is read defensively", () => {
  assert.deepEqual(catalogOf({ weapons: ["kWeaponBazooka", 3, "bad name"], utilities: ["kUtilityJetpack"], error: null }),
    { weapons: ["kWeaponBazooka"], utilities: ["kUtilityJetpack"], error: undefined });
  assert.deepEqual(catalogOf(null), { weapons: [], utilities: [], error: undefined });
});

test("palette: server entries merge over the built-ins; scenery waits for the in-game check", () => {
  const p = paletteFrom([
    { name: "oildrum", resource: "OilDrum", role: "object", preview: "abc" },
    { name: "VISIBLE_tree", resource: "CAMELOT3", role: "scenery", preview: "def" },
    { name: "Camera1", resource: "Camera", role: "camera" },
    { bad: true }, "x",
  ]);
  assert.equal(p.find((e) => e.id === "oildrum")!.preview, "abc");
  assert.equal(p.some((e) => e.role === "scenery"), SCENERY_COPIES);
  assert.ok(p.some((e) => e.name === "Camera1"));
  assert.deepEqual(paletteFrom(undefined).map((e) => e.id), ["knot", "oildrum", "mine", "crate", "telepads", "trigger", "minefactory"]);
  assert.equal(paletteFrom([{ name: "minefactory", resource: "MineFactory", role: "object", preview: "mf" }]).find((e) => e.id === "minefactory")!.preview,
    undefined, "a server entry never takes over an object knot");
});

// ------------------------------------------------------------------ store, commands and patches
test("store: undo and redo return byte-identical patches", () => {
  const store = newStore();
  const p0 = store.patchText();
  const d = store.scene.details[0];
  store.exec(new SetDetail(d.id, { pos: [1.25, 2, 3] }));
  store.exec(new SetDetail(d.id, { rot: [0, 0.7853981633974483, 0] }));
  const p2 = store.patchText();
  assert.ok(store.dirty);
  store.undo();
  store.undo();
  assert.equal(store.patchText(), p0);
  assert.ok(!store.dirty);
  store.redo();
  store.redo();
  assert.equal(store.patchText(), p2);
});

test("store: drags merge into one step; a new drag is a new step", () => {
  const store = newStore();
  const [a, b] = store.scene.details;
  store.exec(setMany([[a.id, { pos: [1, 1, 1] }], [b.id, { pos: [2, 2, 2] }]], "Move"));
  store.exec(setMany([[a.id, { pos: [1, 1, 2] }], [b.id, { pos: [2, 2, 3] }]], "Move"), true);
  assert.equal(store.stack.depth, 1);
  store.exec(setMany([[a.id, { pos: [5, 5, 5] }], [b.id, { pos: [6, 6, 6] }]], "Move"));
  assert.equal(store.stack.depth, 2);
  store.undo();
  assert.deepEqual(store.detail(a.id)!.pos, [1, 1, 2]);
  store.undo();
  assert.equal(store.patchText(), store.savedText());
});

test("store: 500 random commands then 500 undos give an empty patch", () => {
  const store = newStore(scene12());
  const r = rng(42);
  const pick = () => store.scene.details[Math.floor(r() * store.scene.details.length)];
  const vec = (k: number): Vec3 => [Math.round(r() * k * 100) / 100, Math.round(r() * k * 100) / 100, Math.round(r() * k * 100) / 100];
  const cmds: (() => Command | null)[] = [
    () => new SetDetail(pick().id, { pos: vec(50) }),
    () => new SetDetail(pick().id, { rot: vec(3) }),
    () => new SetDetail(pick().id, { scale: [1 + r(), 1 + r(), 1 + r()] }),
    () => new SetDetail(pick().id, { name: `n${Math.floor(r() * 99)}` }),
    () => place(store.scene, store.frames, BUILTIN[1 + Math.floor(r() * 2)], vec(40), 0.5) as AddDetail,
    () => (store.scene.details.length > 4 ? new RemoveDetail(pick().id) : null),
    () => new Duplicate([pick()], [1, 0, 1]),
    () => new SetLevel({ water: Math.round(r() * 100) }),
    () => new SetLevel({ databank: { theme: THEMES[Math.floor(r() * THEMES.length)] } }),
    () => new Group("Delete", [new RemoveDetail(pick().id)]),
  ];
  let n = 0;
  while (n < 500) {
    const c = cmds[Math.floor(r() * cmds.length)]();
    if (!c) continue;
    store.exec(c);
    n++;
  }
  assert.equal(store.stack.depth, 500);
  assert.ok(store.dirty);
  const edited = store.patch();
  assert.ok(validatePatch(edited).ok, validatePatch(edited).errors.join("; "));
  for (let i = 0; i < 500; i++) assert.ok(store.undo());
  assert.ok(!store.stack.canUndo);
  const p = store.patch();
  assert.ok(isEmptyPatch(p, store.base), JSON.stringify(p).slice(0, 300));
  for (let i = 0; i < 500; i++) store.redo();
  assert.equal(JSON.stringify(store.patch()), JSON.stringify(edited));
});

test("store: a saved patch applied to the base reopens as the same scene", () => {
  const store = newStore();
  const d = store.scene.details[2];
  store.exec(new SetDetail(d.id, { pos: [3, 4, 5] }));
  store.exec(place(store.scene, store.frames, BUILTIN[2], [4, 1, 4], 0.5) as AddDetail);
  store.exec(new RemoveDetail(store.scene.details[0].id));
  store.exec(new SetLevel({ water: 40, databank: { theme: "CAMELOT" }, spawns: "knots" }));
  const patch = store.patch();
  const base = loaded(scene12());
  const again = new EditorStore("p1", base, withPatch(base, patch));
  assert.equal(again.patchText(), JSON.stringify(patch));
  assert.ok(!again.dirty);
  const s = applyPatch(scene12(), patch);
  assert.equal(s.water.level, 40);
  assert.equal(s.databank.theme, "CAMELOT");
});

// level.load numbers blob refs on from earlier loads, so the project's refs differ from the base's.
function shiftRefs(s: Scene, by: number): Scene {
  const o = structuredClone(s);
  for (const b of o.blobs) b.ref += by;
  for (const f of o.frames) {
    if (f.voxels !== null) f.voxels += by;
    if (f.heightMap !== null) f.heightMap += by;
  }
  if (o.hmp.ref !== undefined) o.hmp.ref += by;
  return o;
}

test("store: sculpting works when the project's blob refs differ from the base's", () => {
  const s = scene12();
  const store = new EditorStore("p1", loaded(s), loaded(shiftRefs(s, 1000)));
  const sc = new Sculptor({
    scene: store.scene, voxels: store.voxels, base: store.baseVoxels, refOf: (id) => store.voxelRef(id),
    stack: { exec: (c: Command, m?: boolean) => store.exec(c, m) } as unknown as CommandStack, remesh: () => {},
  });
  assert.equal(sc.frames.length, s.frames.filter((f) => f.voxels !== null).length);
  const g = sc.frames.find((x) => x.frame.size[0] >= 6 && x.frame.size[2] >= 6)!;
  const top = apply(g.toWorld, [2.5, g.frame.size[1] + 5, 2.5]), below = apply(g.toWorld, [2.5, 0, 2.5]);
  const hit = sc.pick(top, [below[0] - top[0], below[1] - top[1], below[2] - top[2]])!;
  assert.ok(hit, "pick meets the terrain");
  const r = sc.step(sc.anchor(hit, { mode: "carve", shape: "box", size: [3, 3, 3], material: 0 }),
    { mode: "carve", shape: "box", size: [3, 3, 3], material: 0 });
  assert.ok(r.changed > 0 && r.frames.includes(g.frame.id));
  const voxelOps = () => store.patch().ops.filter((o) => o.op === "voxels");
  assert.deepEqual(voxelOps().map((o) => o.op === "voxels" && o.frame), [g.frame.id]);
  store.undo();
  assert.equal(voxelOps().length, 0);
  store.redo();
  assert.equal(voxelOps().length, 1);
});

test("store: selection follows deletes and undo", () => {
  const store = newStore();
  const id = store.scene.details[1].id;
  store.select([id, 999999]);
  assert.deepEqual(store.selection, [id]);
  store.exec(new RemoveDetail(id));
  assert.deepEqual(store.selection, []);
  const dup = new Duplicate([store.scene.details[0], store.scene.details[1]], [1, 0, 1]);
  store.exec(dup);
  assert.equal(new Set(dup.ids).size, 2);
  assert.ok(dup.ids.every((x) => store.detail(x)?.src === null));
  store.undo();
  assert.ok(dup.ids.every((x) => !store.detail(x)));
});

// ------------------------------------------------------------------ checks
test("checks: knots, water and frame bounds", () => {
  const store = newStore();
  assert.deepEqual(checkScene(store.scene, store.frames).filter((i) => i.level === "error"), []);
  store.exec(new SetLevel({ spawns: "knots" }));
  store.exec(new RemoveDetail(store.scene.details.find((d) => d.name === "WORM5")!.id));
  const issues = checkScene(store.scene, store.frames);
  assert.ok(issues.some((i) => i.level === "error" && /WORM5/.test(i.text)));
  const drum = store.scene.details.find((d) => d.name === "oildrum")!;
  store.exec(new SetLevel({ water: 1000 }));
  assert.ok(checkScene(store.scene, store.frames).some((i) => i.detail === drum.id && /under water/.test(i.text)));
  store.exec(new SetDetail(drum.id, { pos: [9000, 0, 9000] }));
  assert.ok(checkScene(store.scene, store.frames).some((i) => i.detail === drum.id && /outside every terrain frame/.test(i.text)));
  store.exec(new AddDetail(drum.frame, { name: "WORM0", resource: "CheesyGrinWorm", pos: [0, 0, 0] }));
  assert.ok(checkScene(store.scene, store.frames).some((i) => /2 details are named WORM0/.test(i.text)));
  assert.ok(checkScene(store.scene, store.frames, ["ops[1]: bad"]).some((i) => i.text === "ops[1]: bad"));
});

// ------------------------------------------------------------------ drafts and parsing
test("drafts: written per project, offered only for the same base and saved state", () => {
  const mem = new Map<string, string>();
  const fake = { getItem: (k: string) => mem.get(k) ?? null, setItem: (k: string, v: string) => void mem.set(k, v),
    removeItem: (k: string) => void mem.delete(k) } as unknown as Storage;
  const store = newStore();
  store.exec(new SetLevel({ water: 12 }));
  writeDraft("p1", { base: "a".repeat(64), saved: store.savedText(), patch: store.patch() }, fake);
  assert.ok(readDraft("p1", "a".repeat(64), store.savedText(), fake));
  assert.equal(readDraft("p1", "b".repeat(64), store.savedText(), fake), undefined, "another base");
  assert.equal(readDraft("p1", "a".repeat(64), "{}", fake), undefined, "saved since");
  assert.equal(readDraft("p2", "a".repeat(64), store.savedText(), fake), undefined);
  mem.set("oasis.erg.draft.p1", "{not json");
  assert.equal(readDraft("p1", "a".repeat(64), store.savedText(), fake), undefined);
  clearDraft("p1", fake);
  assert.equal(mem.size, 0);
  const broken = { getItem: () => { throw new Error("denied"); }, setItem: () => { throw new Error("full"); }, removeItem: () => {} } as unknown as Storage;
  writeDraft("p1", { base: "", saved: "", patch: store.patch() }, broken);
  assert.equal(readDraft("p1", "", "", broken), undefined);
});

test("server shapes: level.list, level.new and level.themes are read defensively", () => {
  const l = listOf({ bases: [{ key: "Multi.A", stem: "a", title: "A", source: "game" }, { title: "no key" }],
    projects: [{ id: "one", title: "One", stem: "x_one", base: "Multi.A" }, { id: "nobase" }, null] });
  assert.equal(l.bases.length, 1);
  assert.deepEqual(l.projects.map((p) => p.id), ["one"]);
  assert.equal(projectOf({ id: "p", base: { key: "Multi.B" } })?.base, "Multi.B");
  assert.deepEqual(listOf(null), { bases: [], projects: [] });
  assert.ok(validSlug("abc123") && !validSlug("Abc") && !validSlug("a".repeat(25)) && !validSlug(""));
  const t = themesOf({ themes: ["CAMELOT", { name: "WAR" }, "MOON"], timesOfDay: ["DAY"], materialFiles: ["a.txt"] });
  assert.deepEqual(t, { themes: ["CAMELOT", "WAR"], times: ["DAY"], materialFiles: ["a.txt"] });
  assert.equal(themesOf(undefined).themes.length, 11);
});
