import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import { SetLevel, validatePatch, validateScene, type Scene } from "../../src/sdk/erg";
import { withPatch } from "../../src/panels/erg/model/loader";
import { EditorStore, type Loaded } from "../../src/panels/erg/model/store";
import {
  SURROUND_SIDE, SurroundPainter, dabChanges, quantizeHeight, surroundBytes, surroundOf, type SurroundBrush,
} from "../../src/panels/erg/terrain/surround";

const scene12 = (): Scene => JSON.parse(readFileSync(new URL("../../../tests/fixtures/erg/synthetic-12.json", import.meta.url), "utf8"));
const REF = 25;

function baseHeights() {
  const h = new Float32Array(10000), b = new Uint8Array(10000);
  for (let i = 0; i < h.length; i++) { h[i] = (i % 100) / 100; b[i] = i & 0xff; }
  return { heights: h, blend: b };
}

/** The base as level.load {base, surround: true} sends it. */
function baseLoaded(): Loaded {
  const s = scene12();
  s.format = "erg-scene/2";
  s.kind = { survivor: false };
  s.objects = [];
  s.script = { present: false, sha256: null };
  s.hmp = { mode: "copy", ref: REF };
  s.blobs.push({ ref: REF, kind: "hmp", frame: 0, bytes: 50000 });
  return { scene: s, blobs: new Map([[REF, surroundBytes(baseHeights())]]) };
}
const current = (): Loaded => ({ scene: scene12(), blobs: new Map() });

test("the base scene with its surround is valid, and a painted surround needs paint or copy", () => {
  const b = baseLoaded();
  assert.ok(validateScene(b.scene).ok, validateScene(b.scene).errors.join("; "));
  const flat = structuredClone(b.scene);
  flat.hmp.mode = "flat";
  assert.ok(!validateScene(flat).ok);
  const s = surroundOf(b)!;
  assert.equal(s.heights[3], Math.fround(0.03));
  assert.equal(s.blend[7], 7);
  assert.equal(surroundOf(current()), null);
});

test("dabs raise, lower, flatten and smooth on the 1/256 grid", () => {
  const h = new Float32Array(10000);
  const br = (mode: SurroundBrush["mode"], more: Partial<SurroundBrush> = {}): SurroundBrush => ({ mode, radius: 3, strength: 1, level: 0.5, ...more });
  const up = dabChanges(h, 50, 50, br("raise"));
  assert.ok(up.size > 0 && up.size <= 29);
  for (const [i, [before, after]] of up) {
    assert.equal(before, 0);
    assert.equal(after * 256, Math.round(after * 256), `cell ${i} on the grid`);
  }
  assert.equal(up.get(50 * SURROUND_SIDE + 50)![1], quantizeHeight(0.1));
  assert.equal(dabChanges(h, 50, 50, br("lower")).size, 0, "nothing below 0");
  const fl = dabChanges(h, 0, 0, br("flatten"));
  assert.equal(fl.get(0)![1], 0.5, "the centre reaches the level");
  assert.ok([...fl.keys()].every((i) => i % SURROUND_SIDE <= 3 && Math.floor(i / SURROUND_SIDE) <= 3), "clipped to the grid");
  h[5050] = 1;
  const sm = dabChanges(h, 50, 50, br("smooth", { radius: 1 }));
  assert.ok(sm.get(5050)![1] < 1 && sm.get(5050)![1] > 0);
});

test("painting makes an hmp op against the base's surround, undoes as one stroke, and survives a draft", () => {
  const base = baseLoaded();
  const store = new EditorStore("p1", base, current());
  assert.ok(store.canPaintSurround);
  const painter = new SurroundPainter({ surround: store.surround, exec: (c, m) => store.exec(c, m) });
  assert.equal(store.patch().ops.length, 0, "copy mode: no hmp op");
  store.exec(new SetLevel({ hmp: "paint" }));
  let p = store.patch();
  assert.equal(p.hmp?.mode, "paint");
  assert.equal(p.format, "erg-patch/2");
  assert.ok(!p.ops.some((o) => o.op === "hmp"), "an unpainted surround has no runs");

  painter.setBrush({ mode: "flatten", radius: 2, strength: 1, level: 1 });
  painter.begin();
  assert.ok(painter.step(10, 10) > 0);
  painter.step(12, 10);
  painter.end();
  p = store.patch();
  assert.ok(validatePatch(p).ok, validatePatch(p).errors.join("; "));
  const op = p.ops.find((o) => o.op === "hmp");
  assert.ok(op && op.op === "hmp" && op.heights?.length && !op.blend);
  const depth = store.stack.depth;
  store.undo();
  assert.equal(store.stack.depth, depth - 1, "the drag is one undo step");
  assert.equal(store.surround.heights[10 * SURROUND_SIDE + 10], Math.fround(0.1));
  store.redo();
  assert.equal(store.surround.heights[10 * SURROUND_SIDE + 10], 1);

  const again = new EditorStore("p1", base, withPatch(base, store.patch()));
  assert.deepEqual(again.surround.heights, store.surround.heights, "the patch re-applies to the same surround");
  assert.equal(again.patchText(), store.patchText());
});

test("a base without a .hmp paints from zeros; a base whose surround did not arrive cannot be painted", () => {
  const s = scene12();
  s.base.sha256.hmp = null;
  const none = new EditorStore("p1", { scene: s, blobs: new Map() }, current());
  assert.ok(none.canPaintSurround);
  assert.equal(none.surround.heights[0], 0);
  const missing = new EditorStore("p1", { scene: scene12(), blobs: new Map() }, current());
  assert.ok(!missing.canPaintSurround);
});
