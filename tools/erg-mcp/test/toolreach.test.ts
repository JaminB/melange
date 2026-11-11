// check_tool_reach on the synthetic 12-frame level with a small test field built beside it (build_compound boxes): walking
// first, then a jetpack, a rope and a girder each getting a start to ground the others cannot. Bundled for Node by test.ps1.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, type Loaded, type Scene } from "../src/core";
import { CALIBRATION, TOOL_LIMITS } from "../src/lib/tool-reach";
import { tools as structureTools } from "../src/tools/structures";
import { tools as reachTools } from "../src/tools/toolreach";
import type { Session } from "../src/session";

function loaded(): Loaded {
  const s: Scene = JSON.parse(readFileSync(new URL("../../../../tests/fixtures/erg/synthetic-12.json", import.meta.url), "utf8"));
  s.frames.find((f) => f.id === 1)!.name = "Scene";
  const blobs = new Map<number, ArrayBuffer>();
  for (const [ref, buf] of syntheticBlobs(s) as Map<number, Uint8Array>) blobs.set(ref, buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength) as ArrayBuffer);
  return { scene: s, blobs };
}
const session = () => {
  const store = new EditorStore("t", loaded(), loaded());
  const s = { requireProject: () => ({ store, id: "t", info: {} }), get store() { return store; } } as unknown as Session;
  return { store, s };
};
const compound = structureTools.find((t) => t.name === "build_compound")!;
const tool = reachTools.find((t) => t.name === "check_tool_reach")!;
const box = (s: Session, x: number, y: number, z: number, w: number, d: number, h: number) =>
  compound.handler(s, { at: [x, y, z], material: 5, shapes: [{ kind: "box", size: [w, d], height: h }] });
const call = async (s: Session, a: Record<string, unknown>): Promise<any> => tool.handler(s, { x0: 470, z0: 490, x1: 580, z1: 550, water: -50, ...a });

// a 100 x 40 floor (top 2), a tower (top 16) at x 530, a platform (top 8) at x 548 and a gap-separated pad (top 8) at x 560
async function field(s: Session) {
  await box(s, 520, 0, 520, 100, 40, 2);
  await box(s, 530, 2, 520, 10, 10, 14);
  await box(s, 505, 2, 520, 8, 8, 6);          // a step-up stub the start walks onto? no: a pad of its own, 6 high, off the path
  await box(s, 556, 2, 520, 8, 8, 6);
  await box(s, 570, 2, 520, 8, 8, 6);          // 6 units east of the previous pad's edge (a 6 gap)
}

test("check_tool_reach is registered with a strict schema and names the tool group", () => {
  assert.ok(tool);
  assert.equal(tool.inputSchema.additionalProperties, false);
  assert.deepEqual((tool.inputSchema as any).properties.tools.items.enum, ["jetpack", "rope", "girder"]);
  assert.ok((tool.inputSchema as any).properties.limits.properties.rope);
});

test("walking: a target on the floor is reached with no tool; a target on the tower is not walked", async () => {
  const { s } = session();
  await field(s);
  const r = await call(s, { from: [[480, 2, 520]], targets: [{ name: "floor", x: 495, z: 520, radius: 2 }, { name: "tower", x: 530, z: 520, radius: 2 }], tools: ["jetpack"], limits: { jetpack: { rise: 4, run: 6 } } });
  const [floor, tower] = r.groups[0].targets;
  assert.equal(floor.walk, true);
  assert.equal(floor.anyTool.uses, 0);
  assert.equal(tower.walk, false);
  assert.equal(tower.anyTool.reachable, false, "a jetpack of rise 4 does not climb 14");
  assert.equal(r.ok, false);
  assert.match(r.summary.join("\n"), /tower: NOT reachable/);
});

test("a jetpack climbs the tower at the limits that allow it, a rope hangs on its flank, a girder cannot", async () => {
  const { s } = session();
  await field(s);
  const base = { from: [[515, 2, 520]], targets: [{ name: "tower", x: 530, z: 520, radius: 2 }] };
  const jp = await call(s, { ...base, tools: ["jetpack"], limits: { jetpack: { rise: 20, run: 30 } } });
  const hop = jp.groups[0].targets[0].anyTool.plan[0];
  assert.equal(jp.groups[0].targets[0].anyTool.uses, 1);
  assert.equal(hop.tool, "jetpack");
  assert.ok(hop.rise >= 13.5 && hop.rise <= 14.5, `rise ${hop.rise}`);
  assert.ok(hop.usesOfLimit <= 1);
  const rp = await call(s, { ...base, tools: ["rope"], limits: { rope: { length: 30 } } });
  const rh = rp.groups[0].targets[0].anyTool.plan[0];
  assert.equal(rh.tool, "rope");
  assert.ok(rh.anchor[1] >= 15, `anchor ${rh.anchor}`);
  assert.ok(rh.ropeOut <= 30 && rh.ropeIn <= 30);
  const gd = await call(s, { ...base, tools: ["girder"], limits: { girder: { length: 8, maxSlope: 45 } } });
  assert.equal(gd.groups[0].targets[0].anyTool.reachable, false, "14 high is no girder span of 8 at 45 degrees");
  // the same rope too short does not get there
  const short = await call(s, { ...base, tools: ["rope"], limits: { rope: { length: 5 } } });
  assert.equal(short.groups[0].targets[0].anyTool.reachable, false);
});

test("a girder spans a gap its length covers and not one it does not; a jetpack's run limit decides the same gap", async () => {
  const { s } = session();
  await field(s);
  const base = { from: [[556, 8, 520]], targets: [{ name: "pad", x: 570, z: 520, radius: 2 }] };
  const ok = await call(s, { ...base, tools: ["girder"], limits: { girder: { length: 10, maxSlope: 45 } } });
  assert.equal(ok.groups[0].targets[0].anyTool.plan[0].tool, "girder");
  assert.equal(ok.groups[0].targets[0].anyTool.uses, 1);
  const no = await call(s, { ...base, tools: ["girder"], limits: { girder: { length: 3, maxSlope: 45 } } });
  assert.equal(no.groups[0].targets[0].anyTool.reachable, false);
  const jp = await call(s, { ...base, tools: ["jetpack"], limits: { jetpack: { rise: 6, run: 12 } } });
  assert.equal(jp.groups[0].targets[0].anyTool.reachable, true);
  const jpNo = await call(s, { ...base, tools: ["jetpack"], limits: { jetpack: { rise: 6, run: 5 } } });
  assert.equal(jpNo.groups[0].targets[0].anyTool.reachable, false);
});

test("perTool answers each tool alone and needs names the one a target takes; limits and calibration are reported", async () => {
  const { s } = session();
  await field(s);
  const r = await call(s, { from: [[515, 2, 520]], targets: [{ name: "tower", x: 530, z: 520, radius: 2 }], limits: { jetpack: { rise: 20, run: 30 }, rope: { length: 30 }, girder: { length: 4, maxSlope: 45 } } });
  const t = r.groups[0].targets[0];
  assert.equal(t.only.jetpack.reachable, true);
  assert.equal(t.only.rope.reachable, true);
  assert.equal(t.only.girder.reachable, false);
  assert.match(t.anyTool.needs, /jetpack|rope/);
  assert.equal(t.anyTool.turns, 1);
  assert.equal(r.usesPerTurn.rope, 5, "the game lets the rope be shot 5 times a turn (Ninja.NumShots)");
  assert.deepEqual(r.limits.jetpack, { rise: 20, run: 30 });
  assert.ok(r.calibration.overridden.includes("jetpack"));
  assert.equal(r.calibration.status, CALIBRATION.status);
  const d = await call(s, { from: [[515, 2, 520]], targets: [{ x: 530, z: 520, radius: 2 }] });
  assert.equal(d.limits.rope.length, TOOL_LIMITS.rope.length);
  assert.equal(TOOL_LIMITS.rope.length, 22.5, "Ninja.MaxLength 450 world units");
  assert.ok(CALIBRATION.estimates.includes("jetpack.rise") && !CALIBRATION.estimates.includes("rope.length"));
  assert.equal(d.limits.girder.maxSlope, TOOL_LIMITS.girder.maxSlope);
});

test("world units, errors, and the project is not changed", async () => {
  const { store, s } = session();
  await field(s);
  const before = JSON.stringify(store.scene);
  const w = await call(s, { units: "world", x0: 470 * 20, z0: 490 * 20, x1: 580 * 20, z1: 550 * 20, water: -1000, from: [[515 * 20, 40, 520 * 20]], targets: [{ x: 530 * 20, z: 520 * 20, radius: 40 }], tools: ["jetpack"], limits: { jetpack: { rise: 400, run: 600 } } });
  assert.equal(w.groups[0].targets[0].anyTool.uses, 1);
  assert.ok(w.groups[0].targets[0].anyTool.plan[0].rise > 250, "the rise is in world units");
  await assert.rejects(() => call(s, { from: [[515, 2, 520]] }), /give targets/);
  await assert.rejects(() => call(s, { from: [[515, 2, 520]], targets: [{ x: 0, z: 0 }] }), /no ground/);
  await assert.rejects(() => call(s, { from: [[515, 2]] as any, targets: [{ x: 530, z: 520 }] }), /from\[0\]/);
  assert.equal(JSON.stringify(store.scene), before);
});

test("render: true answers a PNG with the plans drawn", async () => {
  const { s } = session();
  await field(s);
  const r = await call(s, { from: [[515, 2, 520]], targets: [{ x: 530, z: 520, radius: 2 }], render: true, limits: { jetpack: { rise: 20, run: 30 } } });
  assert.equal(r.content[0].type, "image");
  assert.equal(r.content[0].mimeType, "image/png");
  assert.ok(JSON.parse(r.content[1].text).groups[0].targets[0].anyTool.reachable);
});
