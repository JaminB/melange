// Detail layer round 5: landscape tints (src/lib/landtint.ts, src/tools/landtint.ts). A level recolours a landscape
// texture through a carrier scenery copy named "..._landtint_ltint<digest><rrggbb>"; it must leave the sim fingerprint
// alone, replace rather than stack, and never read as a per-copy tint. Bundled for Node by test.ps1.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, type Loaded, type Scene } from "../src/core";
import { classifyDetail, simFingerprint } from "../src/lib/sim-fingerprint";
import { carrierName, landTintsOf, materialTiles, textureDigest } from "../src/lib/landtint";
import { tintOf } from "../src/lib/tint";
import { validate } from "../src/registry";
import { tools } from "../src/tools/landtint";

test("land tint tags: digests, materials' tiles, carrier names", () => {
  assert.deepEqual(textureDigest("p05"), { digest: "bfb9ab5911ad8e29", texture: "P05" });
  assert.deepEqual(textureDigest("049E2021928CA5E7"), { digest: "049e2021928ca5e7", texture: "P07" });
  assert.throws(() => textureDigest("P99"), /P01-P64/);
  assert.deepEqual(materialTiles("path02").tiles, ["P05"]);
  assert.deepEqual(materialTiles(19, "all").tiles, ["P23", "P07"]);
  assert.deepEqual(materialTiles(51, "side").tiles, ["P54"]);
  const n = carrierName("VISIBLE_Horror5", "bfb9ab5911ad8e29", "#3D3632");
  assert.equal(n, "VISIBLE_Horror5_landtint_ltintbfb9ab5911ad8e293d3632");
  assert.deepEqual(landTintsOf(n), [{ digest: "bfb9ab5911ad8e29", color: "#3d3632", texture: "P05" }]);
  assert.equal(tintOf(n), null, "not a per-copy tint");
  assert.deepEqual(landTintsOf("VISIBLE_X_ltintbfb9ab5911ad8e293d363"), [], "too short");
  const cl = classifyDetail({ name: n, resource: "Horror5" });
  assert.equal(cl.cls, "visual");
  assert.equal(cl.role, "scenery");
});

function loaded(): Loaded {
  const s: Scene = JSON.parse(readFileSync(new URL("../../../../tests/fixtures/erg/synthetic-12.json", import.meta.url), "utf8"));
  s.frames.find((f) => f.id === 1)!.name = "Scene";
  const blobs = new Map<number, ArrayBuffer>();
  for (const [ref, buf] of syntheticBlobs(s) as Map<number, Uint8Array>) blobs.set(ref, buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength) as ArrayBuffer);
  return { scene: s, blobs };
}
type Session = import("../src/session").Session;

test("tint_landscape: one carrier per texture, replaced and removed; the sim fingerprint stays", async () => {
  const store = new EditorStore("t", loaded(), loaded());
  const project = { store, id: "t", info: {}, erg: { script: async () => "" } };
  const base = loaded().scene;
  base.details.push({ id: 90001, frame: base.frames[0].id, name: "VISIBLE_Rock", resource: "Rock", pos: [0, 0, 0], rot: [0, 0, 0], scale: [1, 1, 1], src: null } as never);
  const client = {};
  const s = { requireProject: () => project, get store() { return store; }, project, client: async () => client, levels: async () => ({ loadBase: async () => ({ scene: base }) }) } as unknown as Session;
  const t = tools.find((x) => x.name === "tint_landscape")!;
  const call = async (args: Record<string, unknown>): Promise<any> => { const bad = validate(t.inputSchema, args); if (bad) throw new Error(bad); return await t.handler(s, args); };
  const before = simFingerprint(store, "");
  const r = await call({ tints: [{ material: "path02", color: "#3d3632" }, { texture: "P07", color: "#443c36" }], id: 90001 });
  assert.equal(r.tints.length, 2);
  const carriers = () => store.scene.details.filter((d) => /_landtint_/.test(d.name));
  assert.equal(carriers().length, 2);
  assert.ok(carriers().every((d) => d.resource === "Rock" && d.scale[0] === 0.05));
  const after = simFingerprint(store, "");
  assert.equal(after.sim, before.sim, "a landscape tint is visual only");
  assert.notEqual(after.visual, before.visual);
  await call({ tints: [{ texture: "P05", color: "#202020" }], id: 90001 });
  assert.equal(carriers().length, 2, "replaced, not stacked");
  assert.ok(carriers().some((d) => d.name.endsWith("_ltintbfb9ab5911ad8e29202020")));
  const l = await call({ list: true });
  assert.equal(l.tints.length, 2);
  await call({ tints: [{ texture: "P05", color: "none" }, { texture: "P07", color: "none" }], id: 90001 });
  assert.equal(carriers().length, 0);
  assert.equal(simFingerprint(store, "").visual, before.visual, "removing the tints restores the visual hash");
  await assert.rejects(call({ tints: [{ texture: "P05", material: 6, color: "#000000" }] }), /texture or material/);
});
