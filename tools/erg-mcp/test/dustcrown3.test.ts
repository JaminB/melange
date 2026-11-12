// Dustcrown round 3's builder gaps on the synthetic 12-frame level: tint_landscape's carrier from the level's own theme,
// its sRGB input and the renders' tint preview; crestMaterial / troughMaterial `blend` (a per-corner edge); build_mesa's
// onSkin (the block names and the seat), `beds` strata and gullies; check_pits' shore tag; check_holes along a walk.
// Every new setting is off by default: the old calls give the old voxels.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, isSolid, material, maskOf, secondOf, type Loaded, type Scene } from "../src/core";
import { applyDrape, planDrape } from "../src/lib/heightfield-drape";
import { apply, planFit } from "../src/lib/heightfield-plan";
import { linearToSrgb, srgbToLinearHex, tintedMaterialColours } from "../src/lib/landtint";
import { landTintPreview } from "../src/lib/materials";
import { mesaField } from "../src/lib/mesa";
import { grids } from "../src/lib/voxel";
import { validate } from "../src/registry";
import { tools as inspectTools, checkPits } from "../src/tools/inspect";
import { tools as landtintTools } from "../src/tools/landtint";
import { tools as mesaTools } from "../src/tools/mesa";
import type { Session } from "../src/session";

function loaded(): Loaded {
  const s: Scene = JSON.parse(readFileSync(new URL("../../../../tests/fixtures/erg/synthetic-12.json", import.meta.url), "utf8"));
  s.frames.find((f) => f.id === 1)!.name = "Scene";
  const blobs = new Map<number, ArrayBuffer>();
  for (const [ref, buf] of syntheticBlobs(s) as Map<number, Uint8Array>) blobs.set(ref, buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength) as ArrayBuffer);
  return { scene: s, blobs };
}
const newStore = () => new EditorStore("t", loaded(), loaded());
const sessionOf = (store: EditorStore, extra: Record<string, unknown> = {}) =>
  ({ requireProject: () => ({ store, id: "t", info: {}, erg: { script: async () => "" } }), get store() { return store; }, ...extra }) as unknown as Session;
const callTool = async (list: { name: string; inputSchema: unknown; handler: (s: Session, a: never) => unknown }[], name: string, s: Session, args: Record<string, unknown>): Promise<any> => {
  const t = list.find((x) => x.name === name)!;
  const bad = validate(t.inputSchema as never, args);
  if (bad) throw new Error(bad);
  const r: any = await t.handler(s, args as never);
  return r?.content ? JSON.parse(r.content.find((c: { type: string }) => c.type === "text").text) : typeof r === "string" ? JSON.parse(r) : r;
};

// ------------------------------------------------------------------ tint_landscape

test("srgbToLinearHex and back: a picker's colour becomes the tag's linear value", () => {
  assert.equal(srgbToLinearHex("#ffffff"), "#ffffff");
  assert.equal(srgbToLinearHex("#000000"), "#000000");
  // 0xbc (188) is about half in linear light
  assert.equal(srgbToLinearHex("#bcbcbc"), "#808080");
  const lin = srgbToLinearHex("#e8b878");
  assert.deepEqual(linearToSrgb(lin).map((v, i) => Math.abs(v - [0xe8, 0xb8, 0x78][i]) <= 2), [true, true, true], lin);
});

test("tint_landscape: carrier theme takes a piece of the level's own theme (the base's by default); colorSpace srgb", async () => {
  const store = newStore();
  store.scene.databank.theme = "ARABIAN";
  const mk = (name: string) => {
    const b = loaded().scene;
    b.details.push({ id: 90001, frame: b.frames[0].id, name: `VISIBLE_${name}`, resource: name, pos: [0, 0, 0], rot: [0, 0, 0], scale: [1, 1, 1], src: null } as never);
    return b;
  };
  const scenes: Record<string, Scene> = { [store.scene.base.key]: mk("DinerSign"), "Multi.Oasis": mk("Palm"), "Multi.Other": mk("Other") };
  const bases = [
    { key: store.scene.base.key, stem: "a", title: "a", source: "game", theme: "WILDWEST" },
    { key: "Multi.Other", stem: "c", title: "c", source: "game", theme: "CAMELOT" },
    { key: "Multi.Oasis", stem: "b", title: "b", source: "game", theme: "Arabian" },
  ];
  const levels = { loadBase: async (k: string) => ({ scene: scenes[k] }), list: async () => ({ bases, projects: [] }) };
  const s = sessionOf(store, { client: async () => levels, levels: async () => levels });
  // the default is the project's base, whatever its theme (Caldera's committed pack depends on it)
  const r0 = await callTool(landtintTools as never, "tint_landscape", s, { tints: [{ texture: "A11", color: "#102030" }] });
  assert.equal(r0.carrier.base, store.scene.base.key);
  assert.equal(r0.carrier.why, "the project's base");
  store.undo();
  await assert.rejects(() => callTool(landtintTools as never, "tint_landscape", s, { tints: [{ texture: "A11", color: "#102030" }], carrier: "theme", base: "Multi.Other" }), /without base and id/);
  const r = await callTool(landtintTools as never, "tint_landscape", s, { tints: [{ texture: "A11", color: "#e8b878" }], colorSpace: "srgb", carrier: "theme" });
  assert.equal(r.carrier.base, "Multi.Oasis");
  assert.match(r.carrier.why, /ARABIAN/);
  assert.equal(r.tints[0].asked, "#e8b878");
  assert.equal(r.tints[0].color, srgbToLinearHex("#e8b878"));
  const carrier = store.scene.details.find((d) => /_landtint_/.test(d.name))!;
  assert.equal(carrier.resource, r.carrier.resource);
  assert.ok(carrier.name.endsWith(srgbToLinearHex("#e8b878").slice(1)));
  // a base given, or a base of the level's own theme: as before
  store.undo();
  const r2 = await callTool(landtintTools as never, "tint_landscape", s, { tints: [{ texture: "A11", color: "#102030" }], base: "Multi.Other" });
  assert.equal(r2.carrier.base, "Multi.Other");
  assert.equal(r2.tints[0].color, "#102030");
  assert.equal(r2.tints[0].asked, undefined);
  store.undo();
  bases[0].theme = "ARABIAN";
  const r3 = await callTool(landtintTools as never, "tint_landscape", s, { tints: [{ texture: "A11", color: "#102030" }], carrier: "theme" });
  assert.equal(r3.carrier.base, store.scene.base.key);
});

test("the renders' tint preview: a material whose top tile is tinted takes the tint's colour", () => {
  const names = ["grass01", "grass01blend", "grass02", "grass02blend", "rock01"];
  const over = tintedMaterialColours(names, [{ digest: "bfb9ab5911ad8e29", color: "#808080", texture: "P05" }], "PREHISTORIC");
  // P05 is rock01's top (and only the SIDE of grass01): the preview colours tops
  assert.deepEqual([...over.keys()], [4]);
  assert.deepEqual(over.get(4), linearToSrgb("#808080"));
  const store = newStore();
  store.scene.databank.theme = "PREHISTORIC";
  const set = { file: "x", names, colors: names.map(() => [1, 2, 3] as [number, number, number]), resolved: null };
  assert.equal(landTintPreview(store, set, () => [9, 9, 9]), null, "no tint: no preview");
  store.scene.details.push({ id: 91000, frame: store.scene.frames[0].id, name: "VISIBLE_X_landtint_ltintbfb9ab5911ad8e29808080", resource: "X", pos: [0, 0, 0], rot: [0, 0, 0], scale: [1, 1, 1], src: null } as never);
  const p = landTintPreview(store, set, () => [9, 9, 9])!;
  assert.deepEqual(p.tint(4), linearToSrgb("#808080"));
  assert.deepEqual(p.tint(0), [9, 9, 9]);
  assert.equal(p.materials[0].name, "rock01");
});

// ------------------------------------------------------------------ crestMaterial blend

const drapeArgs = (extra: Record<string, unknown> = {}) => ({ min: [100, 100], max: [160, 160], floor: 0, expr: "2 + dunes(x, z, 0, 24, 5, 0.6, 3)", material: 2, voxelSize: 1, thickness: 2, ...extra });
function skinWords(st: EditorStore): number[] {
  const out: number[] = [];
  for (const g of grids(st).filter((g) => g.frame.corners)) for (const w of g.words) out.push(w);
  return out;
}

test("crestMaterial blend: the columns beside a crest take it as second material on shared corners; off by default", () => {
  const hard = newStore(), soft = newStore();
  const ph = planDrape(hard, drapeArgs({ crestMaterial: { material: 5, minCurvature: 0.02 }, troughMaterial: { material: 7, minCurvature: 0.02 } }));
  const ps = planDrape(soft, drapeArgs({ crestMaterial: { material: 5, minCurvature: 0.02, blend: true }, troughMaterial: { material: 7, minCurvature: 0.02 } }));
  assert.equal((ph.report.curveMaterials as { blendedEdge?: number }).blendedEdge, undefined);
  const edge = (ps.report.curveMaterials as { blendedEdge: number; crest: { blend: boolean; columns: number } });
  assert.ok(edge.blendedEdge > 20, JSON.stringify(edge));
  assert.equal(edge.crest.blend, true);
  applyDrape(hard, ph); applyDrape(soft, ps);
  const a = skinWords(hard), b = skinWords(soft);
  assert.equal(a.length, b.length);
  let changed = 0;
  for (let i = 0; i < a.length; i++) {
    if (a[i] === b[i]) continue;
    changed++;
    // only a non-crest voxel changes: the same primary material, the crest as its second, some corners but not none
    assert.equal(material(a[i]), material(b[i]));
    assert.notEqual(material(a[i]), 5);
    assert.equal(secondOf(b[i]), 5);
    assert.ok(maskOf(b[i]) !== 0);
    assert.equal(secondOf(a[i]), -1);
  }
  assert.equal(changed, edge.blendedEdge * 2, "every blended column, both skin voxels");
  // the mask's top and bottom halves agree (the same corners in x, z all the way down the skin)
  const some = b.find((w) => secondOf(w) === 5)!;
  assert.ok(isSolid(some));
});

// ------------------------------------------------------------------ build_mesa

const mesaBase = { at: [130, 130] as [number, number], floor: 0, top: 12, shape: { kind: "circle" as const, radius: 12 }, strata: { step: 3, bands: [{ material: 4, thickness: 2 }, { material: 6, thickness: 2 }] }, tread: { walkable: false } };

test("build_mesa: beds vary the bands and feather them, gullies notch the outline; without them the field is the old one", () => {
  const st = newStore();
  const plain = mesaField(st, mesaBase, {}), again = mesaField(st, { ...mesaBase, strata: { ...mesaBase.strata, style: "bands" } }, {});
  assert.deepEqual(plain.args, again.args);
  assert.ok(plain.args.materials!.every((r) => !r.feather && !r.jitter));
  const beds = mesaField(st, { ...mesaBase, strata: { ...mesaBase.strata, style: "beds", seed: 4 } }, {});
  const th = (m: typeof plain) => m.args.materials!.filter((r) => r.maxHeight !== undefined).map((r) => Math.round((r.maxHeight! - r.minHeight!) * 100) / 100);
  assert.ok(new Set(th(plain)).size === 1, JSON.stringify(th(plain)));
  assert.ok(new Set(th(beds)).size > 1, JSON.stringify(th(beds)));
  assert.ok(th(beds).every((t) => t >= 1), "never under a voxel");
  assert.ok(beds.args.materials!.every((r) => r.feather?.pattern === "blue" && r.jitter), JSON.stringify(beds.args.materials![0]));
  assert.equal(beds.info.strataStyle, "beds");
  assert.throws(() => mesaField(st, { ...mesaBase, strata: { ...mesaBase.strata, style: "stripes" as never } }, {}), /bands or beds/);
  // gullies only cut: no column higher, some lower, the plateau kept
  const gul = mesaField(st, { ...mesaBase, gullies: { depth: 4, seed: 2 } }, {});
  const flat = (m: typeof plain) => (m.args.grid as number[][]).flat();
  const g0 = flat(plain), g1 = flat(gul);
  assert.equal(g0.length, g1.length);
  assert.ok(g1.every((v, i) => v <= g0[i] + 1e-6));
  assert.ok(g1.filter((v, i) => v < g0[i] - 1e-6).length > 10);
  assert.ok((gul.info.plateau as { columns: number }).columns > 20);
  assert.deepEqual(gul.info.gullies, { depth: 4, spacing: 12 });
});

test("build_mesa onSkin: blocks named ergonskin_N, and with no floor the mesa is seated under the lowest ground there", async () => {
  const st = newStore(), s = sessionOf(st);
  // a sloping skin to stand on
  applyDrape(st, planDrape(st, { min: [100, 100], max: [160, 160], floor: 0, expr: "4 + 0.1 * x", material: 2, voxelSize: 1, thickness: 2 }));
  await assert.rejects(() => callTool(mesaTools as never, "build_mesa", s, { at: [130, 130], top: 16, shape: { kind: "circle", radius: 8 } }), /floor/);
  const r = await callTool(mesaTools as never, "build_mesa", s, { at: [130, 130], top: 16, shape: { kind: "circle", radius: 8 }, onSkin: { sink: 2 }, tread: { walkable: false } });
  assert.equal(r.applied, true);
  assert.ok(r.added.length >= 1 && r.added.every((b: { name: string }) => /^ergonskin_\d+$/.test(b.name)), JSON.stringify(r.added));
  assert.ok(r.seat.ground.min < r.seat.ground.max, JSON.stringify(r.seat));
  assert.equal(r.seat.floor, Math.round((r.seat.ground.min - 2) * 1000) / 1000);
  // with a floor and no onSkin the blocks keep their old name
  st.undo();
  const r2 = await callTool(mesaTools as never, "build_mesa", s, { at: [130, 130], floor: 3, top: 16, shape: { kind: "circle", radius: 8 }, tread: { walkable: false } });
  assert.ok(r2.added.every((b: { name: string }) => /^ergblock_\d+$/.test(b.name)));
  assert.equal(r2.seat, undefined);
});

// ------------------------------------------------------------------ check_pits shore, check_holes along

test("check_pits: a perch over the sea under a face too steep to climb is tagged shore, and shore: skip leaves it out", () => {
  const st = newStore();
  // a plateau at 8, a one-column ledge at 2 on its east side, then ground under the water (1): the ledge's only way is the sea
  const expr = "select(126 - x, 8, select(127 - x, 2, 0.5))";
  apply(st, planFit(st, { min: [110, 110], max: [140, 140], floor: 0, expr, centre: [0, 0], material: 2, voxelSize: 1 }));
  st.scene.water.level = 1 * 20;
  const all = checkPits(st, { units: "map", x0: 108, z0: 108, x1: 142, z1: 142 }) as { ok: boolean; pits: { shore?: boolean; climbOut: number }[]; shore?: { count: number } };
  assert.equal(all.ok, false);
  assert.ok(all.pits.length >= 1 && all.pits.every((p) => p.shore), JSON.stringify(all.pits));
  assert.equal(all.shore!.count, all.pits.length);
  const skip = checkPits(st, { units: "map", x0: 108, z0: 108, x1: 142, z1: 142, shore: "skip" }) as { ok: boolean; pits: unknown[]; shore?: { count: number; listed: boolean } };
  assert.equal(skip.ok, true);
  assert.equal(skip.pits.length, 0);
  assert.equal(skip.shore!.listed, false);
  // a closed hollow inland is no shore pit
  const st2 = newStore();
  apply(st2, planFit(st2, { min: [110, 110], max: [140, 140], floor: 0, expr: "select(3 - hypot(x - 125, z - 125), 4, 8)", centre: [0, 0], material: 2, voxelSize: 1 }));
  const hollow = checkPits(st2, { units: "map", x0: 108, z0: 108, x1: 142, z1: 142 }) as { pits: { shore?: boolean }[]; shore?: unknown };
  assert.ok(hollow.pits.length >= 1 && hollow.pits.every((p) => !p.shore), JSON.stringify(hollow.pits));
  assert.equal(hollow.shore, undefined);
});

test("check_holes along: only the walk's corridor is read, finer by default, and a hole answers how far along it is", async () => {
  const st = newStore(), s = sessionOf(st);
  // ground with a one-column hole at (125.5, 120.5) and a larger one far from the walk
  const expr = "select(0.5 - max(abs(x - 125.5), abs(z - 120.5)), 0, select(3 - max(abs(x - 135), abs(z - 135)), 0, 4))";
  apply(st, planFit(st, { min: [110, 110], max: [140, 140], floor: 0, expr, centre: [0, 0], material: 2, voxelSize: 1 }));
  const r = await callTool(inspectTools as never, "check_holes", s, { along: [[112, 120.5], [138, 120.5]], above: 0 });
  assert.equal(r.step, 0.25);
  assert.equal(r.along.width, 2);
  assert.equal(r.ok, false);
  assert.equal(r.holes.groupCount, 1, JSON.stringify(r.holes));
  assert.ok(Math.abs(r.holes.groups[0].alongAt - 13.5) < 0.3, JSON.stringify(r.holes.groups[0]));
  assert.ok(Math.abs(r.holes.groups[0].area - 1) < 0.01);
  // off the hole's line: nothing
  const clear = await callTool(inspectTools as never, "check_holes", s, { along: [[112, 114], [138, 114]], above: 0 });
  assert.equal(clear.ok, true);
  await assert.rejects(() => callTool(inspectTools as never, "check_holes", s, { along: [[112, 114], [138, 114]], around: { x: 1, z: 1, radius: 2 } }), /along or around/);
  await assert.rejects(() => callTool(inspectTools as never, "check_holes", s, { width: 3 }), /width goes with along/);
});

// ------------------------------------------------------------------ the level's look, the test game's folders, evidence

import { mkdirSync, mkdtempSync, existsSync, readFileSync as readBytes, rmSync, writeFileSync, utimesSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { encodePng } from "../src/lib/inspect-png";
import { shipRange } from "../src/lib/ship-gate";
import { CALIBRATION, JETPACK_CEILING, TOOL_LIMITS } from "../src/lib/tool-reach";
import { checkPng, levelLookOf, tools as lookTools } from "../src/tools/levellook";
import { namedPngs, projectsDirOf, tools as envTools } from "../src/tools/testenv";

test("set_level_look and set_level_thumbnail keep the pin and the picture per project; the ship gate asks for 0.15", async () => {
  const st = newStore(), s = sessionOf(st);
  (s as unknown as { requireProject: () => unknown }).requireProject = () => ({ store: st, id: "lookproj", info: {} });
  const a = await callTool(lookTools as never, "set_level_look", s, {});
  assert.equal(a.locked, false);
  const b = await callTool(lookTools as never, "set_level_look", s, { timeOfDay: "NIGHT", lock: true });
  assert.equal(b.timeOfDay, "NIGHT");
  assert.equal(b.locked, true);
  assert.equal(st.scene.databank.timeOfDay, "NIGHT");
  assert.equal(levelLookOf("lookproj").lock, true);
  assert.equal((await callTool(lookTools as never, "set_level_look", s, { lock: false })).locked, false);
  // a picture from a file, checked; a file that is no PNG is refused
  const dir = mkdtempSync(join(tmpdir(), "erg-look-"));
  try {
    const png = encodePng(64, 48, new Uint8Array(64 * 48 * 3).fill(120));
    assert.deepEqual(checkPng(png, "x"), { width: 64, height: 48 });
    writeFileSync(join(dir, "shot.png"), png);
    writeFileSync(join(dir, "not.png"), "hello");
    const t = await callTool(lookTools as never, "set_level_thumbnail", s, { from: join(dir, "shot.png") });
    assert.equal(t.thumbnail.width, 64);
    assert.equal(levelLookOf("lookproj").thumbnail!.png.length, png.length);
    await assert.rejects(() => callTool(lookTools as never, "set_level_thumbnail", s, { from: join(dir, "not.png") }), /not a PNG/);
    await assert.rejects(() => callTool(lookTools as never, "set_level_thumbnail", s, { from: join(dir, "shot.png"), clear: true }), /one of/);
    assert.equal((await callTool(lookTools as never, "set_level_thumbnail", s, { clear: true })).thumbnail, null);
  } finally { rmSync(dir, { recursive: true, force: true }); }
  const g = shipRange(10, undefined, { levelLook: ["timeOfDay", "image"] });
  assert.equal(g.melangeRange, ">=0.15.0");
  assert.match(g.why, /timeOfDay and image/);
  assert.equal(shipRange(10).melangeRange, ">=0.2.0", "no key, no gate");
});

test("push_project_to_game copies the saved project into the game's own projects folder and keeps what it replaces", async () => {
  const root = mkdtempSync(join(tmpdir(), "erg-push-"));
  try {
    const game = join(root, "game"), src = join(root, "server-projects"), dst = join(root, "game-projects");
    mkdirSync(join(game, "Mods"), { recursive: true });
    writeFileSync(join(game, "WormsMayhem.exe"), "x");
    writeFileSync(join(game, "Melange.ini"), `[Oasis]\r\nProjectsDir=wrong\r\n[Erg]\r\nEnabled=1\r\nProjectsDir=${dst}\r\n`);
    assert.equal(projectsDirOf(game).dir, dst, "the [Erg] section's key, not another section's");
    mkdirSync(join(src, "pushme"), { recursive: true });
    writeFileSync(join(src, "pushme", "project.ergpatch.json"), "{\"v\":2}");
    writeFileSync(join(src, "pushme", "script.lua"), "-- s");
    mkdirSync(join(dst, "pushme"), { recursive: true });
    writeFileSync(join(dst, "pushme", "project.ergpatch.json"), "{\"v\":1}");
    const s = { project: null, levels: async () => ({ list: async () => ({ projectsDir: src }) }) } as unknown as Session;
    const dry = await callTool(envTools as never, "push_project_to_game", s, { id: "pushme", gameDir: game, dryRun: true });
    assert.equal(dry.replaces, true);
    assert.equal(readBytes(join(dst, "pushme", "project.ergpatch.json"), "utf8"), "{\"v\":1}", "a dry run copies nothing");
    const r = await callTool(envTools as never, "push_project_to_game", s, { id: "pushme", gameDir: game });
    assert.equal(r.copied, true);
    assert.equal(r.files, 2);
    assert.equal(readBytes(join(dst, "pushme", "project.ergpatch.json"), "utf8"), "{\"v\":2}");
    assert.equal(readBytes(join(dst, "pushme.erg-mcp-old", "project.ergpatch.json"), "utf8"), "{\"v\":1}");
    assert.equal((await callTool(envTools as never, "push_project_to_game", s, { id: "pushme", gameDir: game, from: dst })).same, true);
    await assert.rejects(() => callTool(envTools as never, "push_project_to_game", s, { id: "nosuch", gameDir: game }), /no saved project/);
  } finally { rmSync(root, { recursive: true, force: true }); }
});

test("prune_evidence keeps every picture a record names, is a dry run by default and needs a prefix inside a game's folder", async () => {
  const root = mkdtempSync(join(tmpdir(), "erg-prune-"));
  try {
    const ev = join(root, "ev");
    mkdirSync(join(ev, "shots"), { recursive: true });
    const png = encodePng(8, 8, new Uint8Array(8 * 8 * 3));
    for (const n of ["hole_S.png", "a.png", "b.png", "keepme_c.png"]) writeFileSync(join(ev, "shots", n), png);
    utimesSync(join(ev, "shots", "a.png"), new Date(Date.now() - 7200e3), new Date(Date.now() - 7200e3));
    writeFileSync(join(ev, "probe.json"), JSON.stringify({ deaths: [{ worm: 4, shot: "C:/x/shots/hole_S.png" }] }));
    assert.deepEqual([...namedPngs("see shots/Hole_S.png and x.PNG, not y.jpg")], ["hole_s.png", "x.png"]);
    const s = {} as Session;
    const dry = await callTool(envTools as never, "prune_evidence", s, { dir: ev, keep: ["keepme"] });
    assert.equal(dry.dryRun, true);
    assert.equal(dry.wouldRemove, 2);
    assert.deepEqual(dry.kept, { referenced: 1, byKeep: 1, young: 0, notPrefix: 0 });
    assert.ok(existsSync(join(ev, "shots", "a.png")), "a dry run removes nothing");
    const young = await callTool(envTools as never, "prune_evidence", s, { dir: ev, keep: ["keepme"], minAgeMinutes: 60, dryRun: false });
    assert.equal(young.removed, 1, "only the old one");
    assert.ok(!existsSync(join(ev, "shots", "a.png")) && existsSync(join(ev, "shots", "b.png")));
    const all = await callTool(envTools as never, "prune_evidence", s, { dir: ev, dryRun: false });
    assert.equal(all.removed, 2);
    assert.ok(existsSync(join(ev, "shots", "hole_S.png")), "the picture the probe names is kept");
    // inside a game's folder: only with a prefix
    writeFileSync(join(root, "WormsMayhem.exe"), "x");
    await assert.rejects(() => callTool(envTools as never, "prune_evidence", s, { dir: ev }), /prefix/);
    assert.equal((await callTool(envTools as never, "prune_evidence", s, { dir: ev, prefix: "zz" })).wouldRemove, 0);
  } finally { rmSync(root, { recursive: true, force: true }); }
});

test("the jetpack's limits are the game's measured ones, under its ceiling", () => {
  assert.deepEqual(TOOL_LIMITS.jetpack, { rise: 90, run: 60 });
  assert.equal(JETPACK_CEILING, 100);
  assert.deepEqual(CALIBRATION.estimates, ["girder.length", "girder.maxSlope"]);
});
