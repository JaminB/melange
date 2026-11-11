// Detail layer round 4: per-copy scenery tints (src/lib/tint.ts, src/tools/tint.ts). A tint is a "_tint<rrggbb>" name tag
// Melange's MirageTints reads and Sunstone's model shaders draw; it must leave the sim fingerprint alone and only touch
// scenery copies. Bundled for Node by test.ps1.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { AddDetail, EditorStore, type Loaded, type Scene } from "../src/core";
import { commit } from "../src/lib/place-geom";
import { classifyDetail, simFingerprint } from "../src/lib/sim-fingerprint";
import { mixHex, tintHex, tintOf, withTint } from "../src/lib/tint";
import { validate } from "../src/registry";
import { tools as tintTools } from "../src/tools/tint";

test("tint tags: set, replace, read and strip; malformed colours are refused", () => {
  assert.equal(withTint("VISIBLE_Horror5_dtl", "#3A302B"), "VISIBLE_Horror5_dtl_tint3a302b");
  assert.equal(withTint("VISIBLE_Horror5_dtl_tint3a302b", "201a18"), "VISIBLE_Horror5_dtl_tint201a18", "replaced, not stacked");
  assert.equal(withTint("VISIBLE_Horror5_dtl_tint3a302b", null), "VISIBLE_Horror5_dtl");
  assert.equal(withTint("VISIBLE_Rock_tint3a302b_dtl", null), "VISIBLE_Rock_dtl", "a tag in the middle");
  assert.equal(tintOf("VISIBLE_Horror5_dtl_tint3a302b"), "#3a302b");
  assert.equal(tintOf("VISIBLE_Horror5_dtl_tint3a302"), null);
  assert.equal(tintOf("VISIBLE_Horror5_dtl"), null);
  assert.throws(() => tintHex("#3a302"), /#rrggbb/);
  assert.throws(() => tintHex("red"), /#rrggbb/);
  assert.equal(mixHex("#000000", "#ffffff", 0.5), "#808080");
  assert.equal(mixHex("#2a2522", "#3a302b", 0), "#2a2522");
  // a tinted name is still a visual scenery detail (no engine tag)
  for (const c of ["#3a302b", "#ffffff", "#deadbe", "#abcdef", "#000000"]) {
    const cl = classifyDetail({ name: withTint("VISIBLE_Horror5_dtl", c), resource: "Horror5" });
    assert.equal(cl.cls, "visual", c);
    assert.equal(cl.role, "scenery", c);
  }
});

function loaded(): Loaded {
  const s: Scene = JSON.parse(readFileSync(new URL("../../../../tests/fixtures/erg/synthetic-12.json", import.meta.url), "utf8"));
  s.frames.find((f) => f.id === 1)!.name = "Scene";
  const blobs = new Map<number, ArrayBuffer>();
  for (const [ref, buf] of syntheticBlobs(s) as Map<number, Uint8Array>) blobs.set(ref, buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength) as ArrayBuffer);
  return { scene: s, blobs };
}
type Session = import("../src/session").Session;
function session() {
  const store = new EditorStore("t", loaded(), loaded());
  const project = { store, id: "t", info: {}, erg: { script: async () => "" } };
  const s = { requireProject: () => project, get store() { return store; }, project } as unknown as Session;
  return { store, s };
}
const call = async (s: Session, name: string, args: Record<string, unknown>): Promise<any> => {
  const t = tintTools.find((x) => x.name === name)!;
  const bad = validate(t.inputSchema, args);
  if (bad) throw new Error(bad);
  return await t.handler(s, args);
};

test("tint_scenery: renames scenery copies only, by search, ids or a height ramp; the sim fingerprint stays", async () => {
  const { store, s } = session();
  const frame = store.scene.details.find((d) => d.role === "scenery")?.frame ?? store.scene.frames[0].id;
  for (let i = 0; i < 3; i++) commit(store, new AddDetail(frame, { name: "VISIBLE_Horror5_dtl", resource: "Horror5", pos: [i * 4, i * 5, 0], rot: [0, 0, 0], scale: [1, 1, 1] }));
  const copies = () => store.scene.details.filter((d) => d.resource === "Horror5");
  assert.equal(copies().length, 3);
  const before = simFingerprint(store, "");
  const dry = await call(s, "tint_scenery", { search: "Horror5", color: "#3a302b", dryRun: true });
  assert.equal(dry.changed, 3);
  assert.ok(copies().every((d) => d.name === "VISIBLE_Horror5_dtl"), "dryRun changes nothing");
  const r = await call(s, "tint_scenery", { search: "Horror5", color: "#3a302b" });
  assert.equal(r.changed, 3);
  assert.ok(copies().every((d) => d.name === "VISIBLE_Horror5_dtl_tint3a302b"));
  const after = simFingerprint(store, "");
  assert.equal(after.sim, before.sim, "a tint is visual only");
  assert.notEqual(after.visual, before.visual, "the visual hash sees it");
  // a height ramp (map units, given unsorted): the lowest copy dark, the highest red-brown, the middle between
  const ys = copies().map((d) => store.frames.detailWorld(d)[1]);
  await call(s, "tint_scenery", { ids: copies().map((d) => d.id), ramp: [[ys[2], "#402010"], [ys[0], "#202020"]], units: "map" });
  const byY = copies().map((d) => tintOf(d.name));
  assert.deepEqual(byY, ["#202020", "#302018", "#402010"]);
  await call(s, "tint_scenery", { search: "Horror5", color: "none" });
  assert.ok(copies().every((d) => d.name === "VISIBLE_Horror5_dtl"));
  assert.equal(simFingerprint(store, "").visual, before.visual, "removing the tint restores the visual hash");
  // a base detail is never renamed
  const base = store.scene.details.find((d) => d.src !== null && classifyDetail(d).role === "scenery");
  if (base) {
    const r2 = await call(s, "tint_scenery", { ids: [base.id, copies()[0].id], color: "#111111" }).catch((e) => e);
    assert.ok(!(r2 instanceof Error), String(r2));
    assert.equal(store.detail(base.id)!.name, base.name);
    assert.equal(r2.skipped?.[0]?.why, "a base detail");
  }
  await assert.rejects(call(s, "tint_scenery", { search: "Horror5", color: "blue" }), /#rrggbb/);
  await assert.rejects(call(s, "tint_scenery", { color: "#111111" }), /ids, search or near/);
});
