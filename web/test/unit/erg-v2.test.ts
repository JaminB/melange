import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import {
  CommandStack, SetTrims, applyPatch, patchUsesV2, sceneUsesV2, toPatch, upgrade, validKnot, validatePatch, validatePatchObjects, validateScene, voxelCells,
  type Patch, type Scene,
} from "../../src/sdk/erg";

const fixture = (name: string) => readFileSync(new URL(`../../../tests/fixtures/erg/${name}`, import.meta.url), "utf8");
const scene12 = (): Scene => JSON.parse(fixture("synthetic-12.json"));
const sceneV2 = (): Scene => JSON.parse(fixture("synthetic-v2.json"));
const baseV2 = (): Scene => JSON.parse(fixture("synthetic-v2-base.json"));
const patchV2 = (): Patch => JSON.parse(fixture("synthetic-v2.ergpatch.json"));

test("the v2 fixtures written by the C++ model validate", () => {
  const s = sceneV2();
  assert.equal(s.format, "erg-scene/2");
  assert.ok(validateScene(s).ok, validateScene(s).errors.join("; "));
  assert.ok(sceneUsesV2(s));
  const p = patchV2();
  assert.equal(p.format, "erg-patch/2");
  assert.ok(validatePatch(p).ok, validatePatch(p).errors.join("; "));
  assert.ok(validatePatchObjects(p, baseV2()).ok, validatePatchObjects(p, baseV2()).errors.join("; "));
  assert.ok(patchUsesV2(p));
  assert.equal(baseV2().format, "erg-scene/1");
});

test("a v2 load reply with objects and previews validates", () => {
  const s = sceneV2();
  s.previews = { CheesyGrinWorm: "detail/abc123.glb" };
  assert.ok(s.objects?.length);
  assert.ok(validateScene(s).ok, validateScene(s).errors.join("; "));
});

test("a v1 scene upgrades in memory and still saves as v1", () => {
  const s = upgrade(scene12());
  assert.equal(s.format, "erg-scene/2");
  assert.ok(validateScene(s).ok, validateScene(s).errors.join("; "));
  assert.ok(!sceneUsesV2(s));
  const p = toPatch(s, structuredClone(s));
  assert.equal(p.format, "erg-patch/1");
  assert.deepEqual(Object.keys(p).sort(), ["base", "databank", "format", "hmp", "ops", "spawns", "stem", "title", "water"]);
});

test("toPatch and applyPatch carry objects, new frames and the painted surround", () => {
  const base = upgrade(baseV2());
  const edited = applyPatch(base, patchV2());
  assert.equal(edited.objects!.length, 6);
  assert.ok(edited.kind!.survivor && edited.script!.present);
  const nf = edited.frames.find((f) => f.new)!;
  nf.voxels = 999;
  const grid = new Uint32Array(nf.size[0] * nf.size[1] * nf.size[2]);
  grid.fill(3 | (5 << 2), 0, 64);
  const heights = new Float32Array(10000), blend = new Uint8Array(10000);
  heights.fill(1, 0, 100);
  heights.fill(0.25, 9900);
  blend.fill(255);
  edited.stem = "mymaps_objects";
  const p = toPatch(base, edited, { base: new Map(), edited: new Map([[999, grid]]) }, { base: null, edited: { heights, blend } });
  assert.equal(p.format, "erg-patch/2");
  assert.ok(validatePatch(p).ok, validatePatch(p).errors.join("; "));
  assert.ok(validatePatchObjects(p, base).ok, validatePatchObjects(p, base).errors.join("; "));
  const ops = p.ops.map((o) => o.op);
  assert.deepEqual(ops.slice(-3), ["addFrame", "voxels", "hmp"]);
  const want = patchV2();
  assert.deepEqual(p.objects, want.objects);
  assert.deepEqual(p.ops.at(-1), want.ops.at(-1));
  assert.deepEqual(p.ops.at(-2), want.ops.at(-2));
});

test("v2 refusals", () => {
  const scenes: [string, (s: Scene) => void][] = [
    ["leading zero knot", (s) => { s.objects![0].knot = "CRATE_00"; s.details.find((d) => d.name === "CRATE_0")!.name = "CRATE_00"; }],
    ["target crate", (s) => ((s.objects![0] as { crate: { kind: string } }).crate.kind = "target")],
    ["knot without detail", (s) => (s.details.find((d) => d.name === "TRIG_0")!.name = "TRIG_9")],
    ["telepad detail", (s) => (s.details.find((d) => d.name === "TRIG_0")!.name = "telepad")],
    ["new frame under the root", (s) => (s.frames.find((f) => f.new)!.parent = s.frames[0].id)],
    ["new frame too big", (s) => (s.frames.find((f) => f.new)!.size[0] = 33)],
    ["v2 keys in a v1 scene", (s) => (s.format = "erg-scene/1")],
  ];
  for (const [what, mutate] of scenes) {
    const s = sceneV2();
    mutate(s);
    assert.equal(validateScene(s).ok, false, what);
  }
  const patches: [string, (p: Patch) => void][] = [
    ["v2 keys in a v1 patch", (p) => (p.format = "erg-patch/1")],
    ["hmp without paint", (p) => (p.hmp = { mode: "copy" })],
    ["voxels on an unknown frame", (p) => p.ops.push({ op: "voxels", frame: -7, runs: [[0, 1, 3]] })],
    ["tmp past 64", (p) => p.ops.push({ op: "addFrame", tmp: -65, parent: 1000, name: "x", pos: [0, 0, 0], size: [1, 1, 1] })],
    ["blend of 256", (p) => p.ops.push({ op: "hmp", blend: [[0, 1, 256]] })],
    ["height above 1", (p) => p.ops.push({ op: "hmp", heights: [[0, 1, 1.5]] })],
    ["add named telepad", (p) => p.ops.push({ op: "add", frame: 3, detail: { name: "telepad", resource: "Unit", pos: [0, 0, 0] } })],
    ["newer format", (p) => ((p as { format: string }).format = "erg-patch/3")],
  ];
  for (const [what, mutate] of patches) {
    const p = patchV2();
    mutate(p);
    assert.equal(validatePatch(p).ok, false, what);
  }
  const dup = patchV2();
  dup.objects!.push(dup.objects![0]);
  assert.equal(validatePatchObjects(dup, baseV2()).ok, false);
  assert.ok(validKnot("TP_3_12", "telepad", 3) && !validKnot("TP_3_12", "telepad", 2) && !validKnot("CRATE_256", "crate"));
});

test("mine factory settings are optional, range-checked and carried by applyPatch", () => {
  const withFactory = (factory?: unknown) => {
    const p = patchV2();
    const frame = (p.ops.find((o) => o.op === "add") as { frame: number }).frame;
    p.ops.unshift({ op: "add", frame, detail: { name: "minefactory", resource: "CheesyGrinWorm", pos: [9, 1, 2] } });
    p.objects!.push((factory === undefined ? { knot: "minefactory", type: "minefactory" } : { knot: "minefactory", type: "minefactory", factory }) as never);
    return p;
  };
  for (const f of [undefined, {}, { activation: 0 }, { activation: 255, inactiveTurns: 0, schemeMines: false }, { launchRadius: 10, launchers: [[-300, 900, 40], [260, 880, -120]] }]) {
    const p = withFactory(f);
    assert.ok(validatePatch(p).ok, `${JSON.stringify(f)}: ${validatePatch(p).errors.join("; ")}`);
    assert.ok(validatePatchObjects(p, baseV2()).ok, validatePatchObjects(p, baseV2()).errors.join("; "));
  }
  for (const f of [{ activation: 256 }, { activation: 1.5 }, { inactiveTurns: -1 }, { schemeMines: 1 }, { speed: 3 }, 7, { launchers: [] }, { launchers: [[1, 2]] },
    { launchers: [[1.5, 2, 3]] }, { launchers: [[100001, 0, 0]] }, { launchers: Array.from({ length: 9 }, () => [0, 0, 0]) }]) {
    assert.equal(validatePatch(withFactory(f)).ok, false, JSON.stringify(f));
  }
  const edited = applyPatch(upgrade(baseV2()), withFactory({ activation: 12, schemeMines: false }));
  assert.deepEqual(edited.objects!.at(-1), { knot: "minefactory", type: "minefactory", factory: { activation: 12, schemeMines: false } });
  // the throw (erg-mcp's mines round 2): every field validates, the bad ones are refused, and a project saved by the
  // panel keeps them all (applyPatch then toPatch round-trips the object as it came)
  const throwing = { activation: 255, inactiveTurns: 2, keepKnot: true, launchers: [[-300, 900, 40], [260, 880, -120]],
    throw: { minRange: 20, maxRange: 115, apex: 25, pattern: "targets", avoidWater: true, scatter: 12, targetShare: 40, targets: [[652, -1456], [1424, 816], [-1538, 634], [-1510, -624]] } };
  for (const f of [throwing, { throw: { maxRange: 30 } }, { throw: { maxRange: 30, pattern: "ring", avoidWater: false } }]) {
    const p = withFactory(f);
    assert.ok(validatePatch(p).ok, `${JSON.stringify(f)}: ${validatePatch(p).errors.join("; ")}`);
    assert.ok(validatePatchObjects(p, baseV2()).ok, validatePatchObjects(p, baseV2()).errors.join("; "));
  }
  for (const f of [{ throw: {} }, { throw: { maxRange: 0 } }, { throw: { minRange: 50, maxRange: 30 } }, { throw: { maxRange: 30, pattern: "spiral" } }, { throw: { maxRange: 30, pattern: "targets" } },
    { throw: { maxRange: 30, targets: [[1.5, 2]] } }, { throw: { maxRange: 30, apex: 0 } }, { throw: { maxRange: 30, targetShare: 101 } }, { throw: { maxRange: 30, lift: 2 } }]) {
    assert.equal(validatePatch(withFactory(f)).ok, false, JSON.stringify(f));
  }
  const thrown = applyPatch(upgrade(baseV2()), withFactory(throwing));
  assert.deepEqual(thrown.objects!.at(-1), { knot: "minefactory", type: "minefactory", factory: throwing });
  const back = toPatch(upgrade(baseV2()), thrown);
  assert.deepEqual(back.objects!.find((o) => o.knot === "minefactory"), { knot: "minefactory", type: "minefactory", factory: throwing });
  assert.ok(validatePatch(back).ok, validatePatch(back).errors.join("; "));
});

test("trims: a base frame's kept box counts in the cells, rides in the patch and the scene, and undoes", () => {
  const base = upgrade(baseV2());
  const edited = structuredClone(base);
  const before = voxelCells(base);
  const stack = new CommandStack(edited);
  stack.exec(new SetTrims(new Map([[4, { frame: 4, min: [0, 0, 0], size: [1, 1, 1] }], [5, { frame: 5, min: [0, 1, 0], size: [2, 3, 4] }]])));
  assert.equal(voxelCells(edited), before - (7 * 2 * 11 - 1) - (2 * 7 * 4 - 2 * 3 * 4));
  assert.ok(sceneUsesV2(edited));
  assert.ok(validateScene(edited).ok, validateScene(edited).errors.join("; "));
  edited.stem = "mymaps_trims";
  const p = toPatch(base, edited);
  assert.equal(p.format, "erg-patch/2");
  assert.deepEqual(p.ops.filter((o) => o.op === "trimFrame"), [
    { op: "trimFrame", frame: 4, min: [0, 0, 0], size: [1, 1, 1] }, { op: "trimFrame", frame: 5, min: [0, 1, 0], size: [2, 3, 4] }]);
  assert.ok(validatePatch(p).ok, validatePatch(p).errors.join("; "));
  assert.deepEqual(applyPatch(base, p).trims, edited.trims);
  stack.undo();
  assert.equal(edited.trims, undefined);
  assert.equal(voxelCells(edited), before);
  assert.equal(toPatch(base, edited).format, "erg-patch/1");

  const bad = (t: unknown) => validateScene({ ...structuredClone(edited), trims: [t] } as Scene).errors.join("; ");
  assert.ok(/inside the frame/.test(bad({ frame: 4, min: [1, 0, 0], size: [7, 1, 1] })));
  assert.ok(/no voxels/.test(bad({ frame: 1000, min: [0, 0, 0], size: [1, 1, 1] })));
  assert.ok(/min/.test(bad({ frame: 4, min: [0, 0], size: [1, 1, 1] })));
  const v1 = { ...p, format: "erg-patch/1" } as Patch;
  assert.ok(/unknown op 'trimFrame'/.test(validatePatch(v1).errors.join("; ")));
  const twice = structuredClone(p);
  twice.ops.push({ op: "trimFrame", frame: 4, min: [0, 0, 0], size: [1, 1, 1] });
  assert.ok(/trimmed by an earlier op/.test(validatePatch(twice).errors.join("; ")));
});
