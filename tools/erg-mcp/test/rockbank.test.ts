// build_rock_bank / lib/rockmesh.ts / lib/rockbank.ts: the hull rocks and strata are Caldera's gen.mjs byte for byte at
// Caldera's seeds and palettes (so Caldera's bank is untouched: its scripts are not changed and the library reproduces
// them), determinism, palettes and seeds, the kinds' limits, the tool's checks, session registration for render_view, and
// (when xomtool and a game folder with Bundl31.xom are there) a real merged bank.
import assert from "node:assert/strict";
import { existsSync, mkdtempSync, readFileSync, readdirSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { execFileSync } from "node:child_process";
import { test } from "node:test";
// @ts-ignore: plain ESM bench script
import { generate } from "../bench/caldera/meshes/gen.mjs";
import { PALETTES, buildRocks, type Palette, type RockSpec } from "../src/lib/rockmesh";
import { buildBank } from "../src/lib/rockbank";
import { sceneryMesh } from "../src/lib/scenery-mesh";
import { tools } from "../src/tools/rockbank";
import type { Session } from "../src/session";

const tool = tools.find((t) => t.name === "build_rock_bank")!;
const call = async (a: Record<string, unknown>): Promise<any> => tool.handler({} as Session, a);
const B5: Palette = { base: [0x5b, 0x46, 0x33], dust: [0x98, 0x82, 0x64], dustShare: 1, textureSeed: [45, 5505] };
const GEN = "caldera meshes/gen.mjs";
const bytes = (a: Uint8Array) => Buffer.from(a).toString("hex");

test("hull rocks and strata are Caldera's gen.mjs, byte for byte", () => {
  const dir = mkdtempSync(join(tmpdir(), "rockbank-test-"));
  try {
    generate(dir);
    const rocks: RockSpec[] = [
      { name: "Boulder", kind: "boulder" }, { name: "Slab", kind: "slab" },
      { name: "Wedge", kind: "wedge", palette: B5 }, { name: "Shard", kind: "shard", palette: B5 }, { name: "Ledge", kind: "strata" },
    ];
    const b = buildRocks(rocks, { palette: "basalt", generator: GEN });
    for (const r of b.rocks) {
      assert.equal(r.mesh.gltf, readFileSync(join(dir, `${r.name}.gltf`), "utf8"), `${r.name}.gltf`);
      assert.equal(bytes(r.mesh.bin), bytes(readFileSync(join(dir, `${r.name}.bin`))), `${r.name}.bin`);
    }
    // the baked albedo: basalt7 for the first two, basalt5 for the wedge and shard
    const tex = (n: string) => b.textures.get(b.rocks.find((r) => r.name === n)!.textureKey)!;
    assert.equal(bytes(tex("Boulder")), bytes(readFileSync(join(dir, "basalt7.png"))));
    assert.equal(bytes(tex("Wedge")), bytes(readFileSync(join(dir, "basalt5.png"))));
    for (const n of ["Boulder", "Slab", "Wedge", "Shard"]) { const v = b.rocks.find((r) => r.name === n)!.mesh.vertices; assert.ok(v >= 40 && v <= 450, `${n} ${v}`); }
  } finally { rmSync(dir, { recursive: true, force: true }); }
});

test("deterministic; seed, palette and scale change the output", () => {
  const rocks: RockSpec[] = [{ name: "A", kind: "boulder" }, { name: "B", kind: "boulder" }, { name: "C", kind: "scree" }, { name: "D", kind: "strata" }];
  const x = buildRocks(rocks, { palette: "sandstone", seed: 7 }), y = buildRocks(rocks, { palette: "sandstone", seed: 7 });
  assert.deepEqual(x.rocks.map((r) => r.mesh.gltf), y.rocks.map((r) => r.mesh.gltf));
  assert.notEqual(x.rocks[0].mesh.gltf, x.rocks[1].mesh.gltf, "the second boulder gets its own seed");
  assert.equal(x.rocks[1].seed, 1101 + 7 + 101);
  const z = buildRocks(rocks, { palette: "sandstone", seed: 8 });
  assert.notEqual(x.rocks[0].mesh.gltf, z.rocks[0].mesh.gltf);
  assert.notEqual(bytes([...x.textures.values()][0]), bytes([...buildRocks(rocks, { palette: "redrock", seed: 7 }).textures.values()][0]));
  // the boulders and the scree share a texture, the strata have their own
  assert.equal(x.textures.size, 2);
  const big = buildRocks([{ name: "A", kind: "boulder", scale: 2 }], { seed: 7 }), one = buildRocks([{ name: "A", kind: "boulder" }], { seed: 7 });
  assert.ok(Math.abs(big.rocks[0].box.max[0] - 2 * one.rocks[0].box.max[0]) < 1e-3);
  assert.equal(big.rocks[0].mesh.vertices, one.rocks[0].mesh.vertices);
  assert.throws(() => buildRocks([{ name: "A", kind: "slab" }], { palette: "granite" }), /unknown palette/);
  assert.throws(() => buildRocks([{ name: "A", kind: "slab" }, { name: "A", kind: "shard" }]), /two rocks/);
});

test("strata layers and scree rocks size the mesh; both stay inside the bank's limits", () => {
  const b = buildRocks([{ name: "S1", kind: "strata", strata: { layers: 1 } }, { name: "S5", kind: "strata", strata: { layers: 5 } },
    { name: "R3", kind: "scree", scree: { rocks: 3 } }, { name: "R24", kind: "scree", scree: { rocks: 24, radius: 120 } }]);
  const [s1, s5, r3, r24] = b.rocks;
  assert.ok(s5.box.max[1] > s1.box.max[1] * 2 && s5.mesh.vertices > s1.mesh.vertices * 4);
  assert.ok(r24.mesh.vertices > r3.mesh.vertices * 4 && r24.box.max[0] > r3.box.max[0]);
  for (const r of b.rocks) { assert.ok(r.mesh.vertices < 65535); assert.equal(r.tris.length, r.mesh.triangles * 9); }
  assert.ok(r24.box.min[1] > -1, "a scree heap does not hang under the ground plane");
});

test("palette textures are 128 x 128 PNGs", () => {
  for (const name of Object.keys(PALETTES)) {
    const t = buildRocks([{ name: "A", kind: "boulder" }], { palette: name }).textures.values().next().value as Uint8Array;
    assert.deepEqual([...t.slice(0, 8)], [137, 80, 78, 71, 13, 10, 26, 10]);
    const dv = new DataView(t.buffer, t.byteOffset); assert.equal(dv.getUint32(16), 128); assert.equal(dv.getUint32(20), 128);
  }
});

test("the tool checks its arguments and plans without writing", async () => {
  await assert.rejects(call({ id: "Bad Id" }), /lowercase/);
  await assert.rejects(call({ id: "m", rocks: [{ name: "a b", kind: "slab" }] }), /rock name/);
  await assert.rejects(call({ id: "m", rocks: [{ name: "A", kind: "slab", strata: { layers: 2 } }] }), /strata options on a slab/);
  await assert.rejects(call({ id: "m", palette: "granite" }), /unknown palette/);
  const r = await call({ id: "tstrocks", palette: "sandstone", rocks: [{ name: "Block", kind: "boulder" }, { name: "Bed", kind: "strata", strata: { layers: 4 } }] });
  assert.equal(r.written, false);
  assert.deepEqual(r.meshes.map((m: any) => m.name), ["tstrocks.Block", "tstrocks.Bed"]);
  assert.equal(r.spice.meshes[0].file, "assets/meshes/tstrocks.Rocks.xom");
  assert.equal(r.textures, 2);
  assert.ok(r.meshes[0].box.max[1] > 0);
  const d = mkdtempSync(join(tmpdir(), "rockbank-dry-"));
  try { await call({ id: "tstrocks", out: join(d, "x"), dryRun: true }); assert.equal(existsSync(join(d, "x")), false); } finally { rmSync(d, { recursive: true, force: true }); }
});

test("registered rocks draw through render_view's scenery path", async () => {
  await call({ id: "regtest", rocks: [{ name: "Rock", kind: "slab" }] });
  const m = await sceneryMesh({} as Session, "regtest.Rock");
  assert.ok(m && m.tris.length > 90 && m.max[1] > 0.5);
  assert.equal(m.albedo.length, 3);
  await call({ id: "noreg", register: false, rocks: [{ name: "Rock", kind: "slab" }] });
  // not registered: the lookup falls through to the server, which this session does not have
  await assert.rejects(sceneryMesh({ ensure: async () => { throw new Error("no server"); } } as unknown as Session, "noreg.Rock"), /no server/);
});

const GAME = process.env.MELANGE_GAME_DIR ?? "C:/Users/Jamin/Desktop/WUMFix/testenv/C";
const XOMTOOL = join(new URL(".", import.meta.url).pathname.replace(/^\/(\w:)/, "$1"), "../../../../dist/tools/xomtool.exe");
const haveTools = existsSync(XOMTOOL) && existsSync(join(GAME, "Data/Bundles/Bundl31.xom"));
test("a real bank: one XMeshDescriptor per rock, one section, matte", { skip: !haveTools && "no xomtool or game folder" }, () => {
  const out = mkdtempSync(join(tmpdir(), "rockbank-bank-"));
  const rocks: RockSpec[] = [{ name: "Boulder", kind: "boulder" }, { name: "Bed", kind: "strata" }, { name: "Heap", kind: "scree" }];
  try {
    const r = buildBank(out, { id: "tbank", rocks, palette: "sandstone", gameDir: GAME, xomtool: XOMTOOL });
    assert.equal(r.file, join(out, "tbank.Rocks.xom"));
    assert.deepEqual(readdirSync(out), ["tbank.Rocks.xom"], "no sources left behind");
    assert.ok(r.bytes > 20000);
    const j = join(out, "b.json");
    execFileSync(XOMTOOL, ["unpack", r.file, "-o", j], { encoding: "utf8" });
    const doc = JSON.parse(readFileSync(j, "utf8"));
    const descs = doc.objects.filter((o: any) => o.type === "XMeshDescriptor");
    assert.deepEqual(descs.map((o: any) => o.fields.ResourceId).sort(), ["tbank.Bed", "tbank.Boulder", "tbank.Heap"]);
    assert.equal(new Set(descs.map((o: any) => o.fields.SectionId)).size, 1);
    assert.equal(descs[0].fields.SectionId, 486);
    for (const m of doc.objects.filter((o: any) => o.type === "XMaterial")) assert.deepEqual(m.fields.Specular, [0, 0, 0, 1]);
    const r2 = buildBank(join(out, "again"), { id: "tbank", rocks, palette: "sandstone", gameDir: GAME, xomtool: XOMTOOL });
    assert.equal(bytes(readFileSync(r2.file)), bytes(readFileSync(r.file)), "the same inputs build the same bank");
  } finally { rmSync(out, { recursive: true, force: true }); }
});
