// Dustcrown round 6's builder gaps on the synthetic 12-frame level: set_level_thumbnail's own camera, set_level_camera,
// the renders' water look and the haze check, check_sea_opacity and set_water_look {opaque}, check_crater_traps and
// heightfield_drape's craterSlope, check_pits' filters and enclosed rooms, build_outcrop's steps from mesa.top and
// mesa.floor, the lints' shared search and lint_support's remove, the grade preview that interpolates, and
// describe_dunes. Every new setting is off by default: the old calls give the old answers.
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { test } from "node:test";
// @ts-ignore: plain ESM test helper
import { syntheticBlobs } from "../../../web/test/e2e/erg-mock.mjs";
import { EditorStore, type Loaded, type Scene } from "../src/core";
import { JUMP_RISE, carveCrater, craterVerdict, trapAdvice } from "../src/lib/crater-traps";
import { matchesSearch, searchTerms } from "../src/lib/detail-search";
import { DUNE_RULES, crestFaults, findCrests, wormView, type ProfileSample } from "../src/lib/dune-profile";
import { GRADE_REACH, fitGrade, gradeMaterial, gradePreview, type Grade } from "../src/lib/grade";
import { applyDrape, planDrape } from "../src/lib/heightfield-drape";
import { decodePng } from "../src/lib/inspect-png";
import { spansAt } from "../src/lib/inspect-field";
import { fittedPose, landPoints, levelCameraOf } from "../src/lib/level-camera";
import { LOOKS, renderView, type ViewInput } from "../src/lib/render-3d";
import { HAZE_MODEL, hazeAt, hazeReach, hazeVerdict, levelLooks, seaOpacity, seaShowsThrough, shallows } from "../src/lib/sea-look";
import { waterLookBlock } from "../src/lib/water-look";
import { grids } from "../src/lib/voxel";
import { validate } from "../src/registry";
import { checkCraterTraps, tools as craterTools } from "../src/tools/cratertraps";
import { tools as drapeTools } from "../src/tools/drape";
import { describeDunes } from "../src/tools/dunes";
import { checkPits, fixPits } from "../src/tools/inspect";
import { tools as lookTools } from "../src/tools/levellook";
import { tools as mesaTools } from "../src/tools/mesa";
import { checkSeaOpacity, tools as placeTools } from "../src/tools/place";
import { tools as renderTools } from "../src/tools/render";
import { tools as structureTools } from "../src/tools/structures";
import type { Session } from "../src/session";

function loaded(): Loaded {
  const s: Scene = JSON.parse(readFileSync(new URL("../../../../tests/fixtures/erg/synthetic-12.json", import.meta.url), "utf8"));
  s.frames.find((f) => f.id === 1)!.name = "Scene";
  const blobs = new Map<number, ArrayBuffer>();
  for (const [ref, buf] of syntheticBlobs(s) as Map<number, Uint8Array>) blobs.set(ref, buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength) as ArrayBuffer);
  return { scene: s, blobs };
}
const newStore = () => new EditorStore("t", loaded(), loaded());
/** A session over a store whose project keeps `script` (the level script the looks are read from). */
const sessionOf = (store: EditorStore, id = "t", script = { text: "" }) =>
  ({ project: { id }, requireProject: () => ({ store, id, info: {}, erg: { script: async () => script.text, saveScript: async (t: string) => { script.text = t; return { saved: true, problems: [] }; } } }), get store() { return store; } }) as unknown as Session;
const callRaw = async (list: { name: string; inputSchema: unknown; handler: (s: Session, a: never) => unknown }[], name: string, s: Session, args: Record<string, unknown>): Promise<any> => {
  const t = list.find((x) => x.name === name)!;
  const bad = validate(t.inputSchema as never, args);
  if (bad) throw new Error(bad);
  return t.handler(s, args as never);
};
const callTool = async (list: { name: string; inputSchema: unknown; handler: (s: Session, a: never) => unknown }[], name: string, s: Session, args: Record<string, unknown>): Promise<any> => {
  const r: any = await callRaw(list, name, s, args);
  return r?.content ? JSON.parse(r.content.find((c: { type: string }) => c.type === "text").text) : typeof r === "string" ? JSON.parse(r) : r;
};
const words = (st: EditorStore) => createHash("sha1").update(grids(st).map((g) => `${g.id}:${Array.from(g.words).join(",")}`).join("|")).digest("hex");
/** A slab of ground far from the synthetic frames: x 580..620, z -20..20, y 0..`height`. */
const slab = (st: EditorStore, height = 4, scale = 1) => callTool(structureTools as never, "build_compound", sessionOf(st), { at: [600, 0, 0], place: "new", scale, material: 1, shapes: [{ kind: "box", size: [40, 40], height }] });
/** A draped skin of `expr` (x and z from its middle, 730, 0) over x 700..760, z -30..30. */
const skin = (st: EditorStore, expr: string, voxelSize = 2.5, extra: Record<string, unknown> = {}) =>
  applyDrape(st, planDrape(st, { min: [700, -30], max: [760, 30], centre: [730, 0], floor: 0, keepFloor: true, voxelSize, thickness: 3, core: false, expr, material: 2, maxBlocks: 8, ...extra } as never));

// ------------------------------------------------------------------ set_level_thumbnail, set_level_camera

test("set_level_thumbnail {render: {pos, target}} draws from that camera (it was refused: the se preset was added to it)", async () => {
  const st = newStore(), s = sessionOf(st, "thumb");
  await slab(st);
  const own = await callRaw(lookTools as never, "set_level_thumbnail", s, { render: { pos: [600, 60, 80], target: [600, 2, 0] }, size: 128, show: true });
  const png = decodePng(new Uint8Array(Buffer.from(own.content[0].data, "base64")));
  assert.deepEqual([png.w, png.h], [128, 128]);
  // a view preset alone, and no camera at all, still work, and the three pictures differ
  const se = await callRaw(lookTools as never, "set_level_thumbnail", s, { render: {}, size: 128, show: true });
  const nw = await callRaw(lookTools as never, "set_level_thumbnail", s, { render: { view: "nw" }, size: 128, show: true });
  assert.notEqual(own.content[0].data, se.content[0].data);
  assert.notEqual(nw.content[0].data, se.content[0].data);
  // pos with a view is still refused, by render_view's own rule
  await assert.rejects(callRaw(lookTools as never, "set_level_thumbnail", s, { render: { view: "se", pos: [600, 60, 80], target: [600, 2, 0] } }), /not both/);
});

test("set_level_camera: a pose or a fitted side, kept per project; render_view {view: level} and the thumbnail use it; nothing is edited", async () => {
  const st = newStore(), s = sessionOf(st, "cam");
  await slab(st);
  st.scene.water.level = 40;                       // 2 map units: the slab's top (4) stands over it
  const before = words(st), dirty = st.dirty;
  assert.equal((await callTool(lookTools as never, "set_level_camera", s, {})).camera, null);
  await assert.rejects(callTool(renderTools as never, "render_view", s, { view: "level" }), /no level camera/);
  const r = await callTool(lookTools as never, "set_level_camera", s, { view: "s", elevation: 45 });
  assert.match(r.camera.how, /^s, 45 degrees up, fitted to the land over the water/);
  // from the south: the eye is at a greater z than the target, over it
  assert.ok(r.camera.pos.map[2] > r.camera.target.map[2] && r.camera.pos.map[1] > r.camera.target.map[1]);
  assert.equal(levelCameraOf("cam")!.fov, 60);
  assert.equal(levelCameraOf("other"), null);
  // every land point is in the view: the fitted distance shows the slab's corners
  const pts = landPoints(grids(st).filter((g) => g.words.some((w) => w !== 0)), 2, "island");
  assert.ok(pts.length > 100 && pts.every((p) => p[1] >= 2));
  assert.ok(landPoints(grids(st).filter((g) => g.frame.new), 2, "island").every((p) => p[1] === 4 || p[1] === 2), "the slab's top and the waterline under it");
  const pose = fittedPose(pts, { azimuth: 180, elevation: 45 });
  assert.ok(Math.abs(pose.distance - r.camera.distance.map) < 0.05, `${pose.distance} vs ${r.camera.distance.map}`);
  const v = await callTool(renderTools as never, "render_view", s, { view: "level", width: 160, height: 90 });
  assert.match(v.cameras[0].how, /set_level_camera/);
  assert.deepEqual(v.cameras[0].pos.map, r.camera.pos.map);
  // an explicit pose in world units; a view with a pose is refused; clear drops it
  const p = await callTool(lookTools as never, "set_level_camera", s, { pos: [12000, 1200, 1600], target: [12000, 40, 0], units: "world", fov: 50 });
  assert.deepEqual([p.camera.pos.map, p.camera.fov, p.camera.how], [[600, 60, 80], 50, "pos and target"]);
  await assert.rejects(callTool(lookTools as never, "set_level_camera", s, { pos: [1, 2, 3], target: [0, 0, 0], view: "n" }), /not both/);
  await assert.rejects(callTool(lookTools as never, "set_level_camera", s, { pos: [1, 2, 3] }), /needs a target/);
  // the thumbnail with no camera of its own takes the level camera
  const withCam = await callRaw(lookTools as never, "set_level_thumbnail", s, { render: {}, size: 128, show: true });
  assert.equal((await callTool(lookTools as never, "set_level_camera", s, { clear: true })).camera, null);
  const without = await callRaw(lookTools as never, "set_level_thumbnail", s, { render: {}, size: 128, show: true });
  assert.notEqual(withCam.content[0].data, without.content[0].data);
  assert.equal(words(st), before);
  assert.equal(st.dirty, dirty);
});

// ------------------------------------------------------------------ the water look in the renders, the haze check

const HAZE = { color: [240, 228, 176], amount: 1, start: 0, distance: 0 };

test("hazeAt: Sunstone's model: nothing before the start, the land's share tops out at landHaze, a level's own range wins", () => {
  assert.deepEqual(hazeAt(HAZE_MODEL.fogStart, HAZE), { land: 0, sea: 0 });
  const far = hazeAt(1e7, HAZE);
  assert.ok(Math.abs(far.land - HAZE_MODEL.landHaze) < 1e-6 && Math.abs(far.sea - HAZE_MODEL.maxHaze) < 1e-6);
  // one fog distance past the start: 1 - 1/e of the top
  assert.ok(Math.abs(hazeAt(HAZE_MODEL.fogStart + HAZE_MODEL.fogDistance, HAZE).land - HAZE_MODEL.landHaze * (1 - Math.exp(-1))) < 1e-9);
  const own = { ...HAZE, start: 3000, distance: 9000 };
  assert.equal(hazeAt(3000, own).land, 0);
  assert.ok(hazeAt(4000, own).land < hazeAt(4000, HAZE).land);
  assert.equal(hazeAt(5000, { ...HAZE, amount: 0 }).land, 0);
  // the distance a share is reached at, and none past the top
  const d = hazeReach(0.33, HAZE)!;
  assert.ok(Math.abs(hazeAt(d, HAZE).land - 0.33) < 1e-6);
  assert.equal(hazeReach(0.7, HAZE), null);
});

test("hazeVerdict: a far overview is hidden and says how close the camera must come; a near one is not", () => {
  const far = hazeVerdict(4600, HAZE, { land: [200, 150, 90], what: "the default overview" });
  assert.equal(far.hidden, true);
  assert.match(far.warning!, /takes \d+ % of the land's colour from the default overview .*set_level_camera/);
  // the land reads toward the haze colour
  assert.ok(far.landReads![0] > 200 && far.landReads![2] > 90);
  const near = hazeVerdict(1200, HAZE);
  assert.deepEqual([near.hidden, near.warning], [false, undefined]);
  assert.equal(hazeVerdict(4600, { ...HAZE, start: 3000, distance: 9000 }).hidden, false);
});

test("renderView: a sea material is a matte plane at its opacity; a haze fades the far land and tints the horizon; without them the picture is the old one", () => {
  const st = newStore();
  const gs = grids(st), cam = { pos: [5, 40, 90] as [number, number, number], target: [5, 0, 0] as [number, number, number], fov: 60 };
  const base: ViewInput = { grids: gs, tint: () => [120, 160, 90], look: LOOKS.DAY, water: 1.5, edges: false };
  const plain = renderView(base, cam, 160, 90), again = renderView({ ...base }, cam, 160, 90);
  assert.deepEqual(plain.rgb, again.rgb);
  const px = (v: { rgb: Uint8Array }, x: number, y: number) => [v.rgb[(y * 160 + x) * 3], v.rgb[(y * 160 + x) * 3 + 1], v.rgb[(y * 160 + x) * 3 + 2]];
  // open sea in the bottom corner: the sea material's own colour, exactly, and no blue
  const sea = renderView({ ...base, sea: { rgb: [214, 190, 140], opacity: 1 } }, cam, 160, 90);
  assert.deepEqual(px(sea, 2, 88), [214, 190, 140]);
  assert.ok(px(plain, 2, 88)[2] > px(plain, 2, 88)[0], "the plain plane is blue");
  // an opaque sea hides ground under the water; a translucent one lets it through
  const under = { ...base, water: 30 };            // the whole level is under the sea, the eye over it
  const opaque = renderView({ ...under, sea: { rgb: [214, 190, 140], opacity: 1 } }, cam, 160, 90), thin = renderView({ ...under, sea: { rgb: [214, 190, 140], opacity: 0.5 } }, cam, 160, 90);
  let same = 0, differ = 0;
  for (let i = 0; i < opaque.rgb.length; i += 3) { if (opaque.rgb[i] === 214 && opaque.rgb[i + 1] === 190 && opaque.rgb[i + 2] === 140) same++; if (thin.rgb[i] !== opaque.rgb[i]) differ++; }
  assert.ok(same > 160 * 90 * 0.4, `the opaque sea covers its pixels (${same})`);
  assert.ok(differ > 50, `the translucent sea shows the land under it (${differ} pixels)`);
  // a haze: everything fades to it with distance, and the band under the horizon is its colour
  const hz = renderView({ ...base, haze: { rgb: [240, 228, 176], amount: 1, at: () => ({ land: 1, sea: 1 }) } }, cam, 160, 90);
  for (let i = 0; i < hz.rgb.length; i += 3) assert.ok(Math.abs(hz.rgb[i] - 240) <= 60 && hz.rgb[i + 2] <= 235, "every pixel is hazed toward the colour");
  const none = renderView({ ...base, haze: { rgb: [240, 228, 176], amount: 0, at: () => ({ land: 0, sea: 0 }) } }, cam, 160, 90);
  assert.deepEqual(none.rgb, plain.rgb);
});

test("render_view waterLook and gameMatch: the level's looks are read from its script; a hazed far camera is warned of; off by default", async () => {
  const st = newStore(), script = { text: "" }, s = sessionOf(st, "gm", script);
  st.scene.water.level = 30;
  const plain = await callRaw(renderTools as never, "render_view", s, { width: 160, height: 90 });
  script.text = waterLookBlock({ NearOpacity: 1 }, { albedo: [214, 190, 140], opacity: 1, reflection: 0, foam: 0 }, { color: [240, 228, 176], amount: 1, start: 0, distance: 0 }) + "\n-- the level's own script\n";
  // without the flag the script's looks change nothing
  const still = await callRaw(renderTools as never, "render_view", s, { width: 160, height: 90 });
  assert.equal(still.content[0].data, plain.content[0].data);
  assert.equal(JSON.parse(still.content[1].text).waterLook, undefined);
  const wl = await callRaw(renderTools as never, "render_view", s, { width: 160, height: 90, waterLook: true });
  assert.notEqual(wl.content[0].data, plain.content[0].data);
  const info = JSON.parse(wl.content[1].text);
  assert.deepEqual([info.waterLook.sea.albedo, info.waterLook.opacity, info.waterLook.haze.color], [[214, 190, 140], 1, [240, 228, 176]]);
  assert.equal(info.gameMatch, undefined);
  // gameMatch: the default overview of this wide level stands far back, in the haze
  const gm = JSON.parse((await callRaw(renderTools as never, "render_view", s, { width: 160, height: 90, gameMatch: true })).content[1].text).gameMatch;
  const overview = gm.haze.find((h: { from: string }) => /default overview/.test(h.from)), turn = gm.haze.find((h: { from: string }) => /turn camera/.test(h.from));
  assert.equal(turn.hidden, false);
  assert.equal(overview.hidden, gm.warnings?.some((w: string) => /default overview/.test(w)) ?? false);
  assert.equal(gm.ok, !gm.warnings);
  // a level camera stands in for the default overview
  await callTool(lookTools as never, "set_level_camera", s, { pos: [0, 30, 60], target: [0, 0, 0] });
  const gm2 = JSON.parse((await callRaw(renderTools as never, "render_view", s, { width: 160, height: 90, gameMatch: { hidden: 0.05 } })).content[1].text).gameMatch;
  assert.ok(gm2.haze.some((h: { from: string }) => /level camera/.test(h.from)) && !gm2.haze.some((h: { from: string }) => /default overview/.test(h.from)));
  // a sea that is not opaque is warned of
  script.text = waterLookBlock({ NearOpacity: 0.7 }, null, null);
  const gm3 = JSON.parse((await callRaw(renderTools as never, "render_view", s, { width: 160, height: 90, gameMatch: true })).content[1].text).gameMatch;
  assert.ok(gm3.warnings.some((w: string) => /opacity 0.7/.test(w)));
  await callTool(lookTools as never, "set_level_camera", s, { clear: true });
});

// ------------------------------------------------------------------ check_sea_opacity, set_water_look {opaque}

test("seaShowsThrough: an opaque sea shows nothing; a translucent one shows the beds that differ from it", () => {
  const cells = [{ x: 0, z: 0, depth: 0.5, material: 1 }, { x: 1, z: 0, depth: 1, material: 2 }];
  const tint = (m: number): [number, number, number] => (m === 1 ? [40, 40, 40] : [210, 188, 142]);
  assert.deepEqual(seaShowsThrough(cells, 1, [214, 190, 140], tint), { opaque: true, opacity: 1, visible: [], worst: null });
  const half = seaShowsThrough(cells, 0.5, [214, 190, 140], tint);
  // the dark bed shifts the sea's colour by (214 - 40) / 2; the sand-coloured bed by 2: not seen
  assert.deepEqual([half.opaque, half.visible.length, half.worst!.at, half.worst!.shift], [false, 1, [0, 0], 87]);
  // no opacity set at all: the theme's translucent water, every bed without a colour counts
  assert.equal(seaShowsThrough(cells, null, null, null).visible.length, 2);
  assert.equal(seaOpacity(levelLooks(waterLookBlock({ NearOpacity: 0.8 }, { albedo: [1, 2, 3], opacity: 0.4, reflection: 0, foam: 0 }, null)), "mod"), 0.4);
  assert.equal(seaOpacity(levelLooks(waterLookBlock({ NearOpacity: 0.8 }, { albedo: [1, 2, 3], opacity: 0.4, reflection: 0, foam: 0 }, null)), "game"), 0.8);
  assert.equal(seaOpacity(levelLooks(""), "game"), null);
});

test("check_sea_opacity and set_water_look {opaque}: shallows under a translucent sea are listed; opaque sets the plane and the sea material to 1", async () => {
  const st = newStore(), script = { text: "" }, s = sessionOf(st, "sea", script);
  await slab(st, 4);
  st.scene.water.level = 100;                      // 5 map units: the slab's top is 1 under the water
  const sh = shallows(grids(st).filter((g) => g.frame.new), 5, 2, 1);
  assert.equal(sh.cells.length, 1600);
  assert.ok(sh.cells.every((c) => Math.abs(c.depth - 1) < 1e-9));
  assert.equal(shallows(grids(st).filter((g) => g.frame.new), 5, 0.5, 1).cells.length, 0);
  // no look at all: the theme's water is not opaque
  const none = await checkSeaOpacity(s, "", { gameDir: "" }) as any;
  assert.equal(none.ok, false);
  assert.match(none.summary, /shallow columns show through/);
  assert.ok(none.drawnBy[0].shallowsShowing >= 1600);
  assert.match(none.fix, /opaque: true/);
  // a translucent sea material and plane
  await callTool(placeTools as never, "set_water_look", s, { opacity: 0.6, sea: { albedo: [214, 190, 140], opacity: 0.5 } });
  const tr = await callTool(placeTools as never, "check_sea_opacity", s, {});
  assert.deepEqual([tr.ok, tr.drawnBy.map((d: { by: string; opacity: number }) => [d.by, d.opacity])], [false, [["mod", 0.5], ["game", 0.6]]]);
  // opaque: both go to 1, the albedo and the rest stay; the answer carries the verdict
  await assert.rejects(callTool(placeTools as never, "set_water_look", s, { opaque: true, opacity: 0.5 }), /contradict/);
  const o = await callTool(placeTools as never, "set_water_look", s, { opaque: true });
  assert.deepEqual([o.changed, o.look.NearOpacity, o.sea.opacity, o.sea.albedo, o.seaOpacity.ok], [true, 1, 1, [214, 190, 140], true]);
  const after = await callTool(placeTools as never, "check_sea_opacity", s, {});
  assert.equal(after.ok, true);
  assert.ok(after.shallows.count >= 1600);
});

// ------------------------------------------------------------------ check_crater_traps, craterSlope

test("carveCrater: the cells whose centres lie in the sphere go, in a copy; the grids given are untouched", () => {
  const st = newStore();
  return slab(st, 6).then(() => {
    const gs = grids(st).filter((g) => g.frame.new), before = words(st);
    const c = carveCrater(gs, [600, 6, 0], { radius: 3 });
    assert.ok(c.carved > 20 && c.layers === 3 && c.voxel === 1, JSON.stringify([c.carved, c.layers, c.voxel]));
    assert.equal(words(st), before);
    assert.notEqual(c.grids[0], gs[0]);
    assert.equal(spansAt(c.grids, 600.5, 0.5)[0].top, 3);
    assert.equal(spansAt(c.grids, 610.5, 0.5)[0].top, 6);
    // touch takes more than centre
    assert.ok(carveCrater(gs, [600, 6, 0], { radius: 3, rule: "touch" }).carved > c.carved);
    assert.equal(carveCrater(gs, [900, 6, 0], { radius: 3 }).carved, 0);
  });
});

const RULES = { jump: JUMP_RISE, maxSlope: 45, step: 1, headroom: 1.5, water: null, margin: 5 };

test("craterVerdict: a hole in level ground of 2.5-unit voxels is a trap; in 2-unit voxels a jump clears it; a hole on a slope lets out downhill", async () => {
  // coarse flat ground: one layer goes, the walls are 2.5 all round
  const coarse = newStore();
  await slab(coarse, 10, 2.5);
  const g25 = grids(coarse).filter((g) => g.frame.new);
  const v = craterVerdict(g25, [600, 10, 0], { radius: 3 }, RULES);
  assert.deepEqual([v.trap, v.wasOpen, v.layers, v.voxel, v.depth, v.wall], [true, true, 1, 2.5, 2.5, 2.5]);
  // the same hole in 2-unit voxels
  const fine = newStore();
  await slab(fine, 10, 2);
  const w = craterVerdict(grids(fine).filter((g) => g.frame.new), [600, 10, 0], { radius: 3 }, RULES);
  assert.deepEqual([w.trap, w.layers, w.voxel], [false, 1, 2]);
  // a worm that jumps 3 gets out of the coarse hole
  assert.equal(craterVerdict(g25, [600, 10, 0], { radius: 3 }, { ...RULES, jump: 3 }).trap, false);
  // a draped slope of the same voxels: the wall on the downhill side is lower than a voxel
  const dune = newStore();
  skin(dune, "8 + x * 0.4");
  const gd = grids(dune).filter((g) => g.frame.new), top = spansAt(gd, 730, 0)[0].top;
  const d = craterVerdict(gd, [730, top, 0], { radius: 3 }, RULES);
  assert.equal(d.trap, false);
  assert.ok(d.carved > 0);
  // and the level stretch of a draped skin traps, as the slab does
  const pad = newStore();
  skin(pad, "8");
  const gp = grids(pad).filter((g) => g.frame.new);
  assert.equal(craterVerdict(gp, [730, spansAt(gp, 730, 0)[0].top, 0], { radius: 3 }, RULES).trap, true);
  // the advice: the voxel a jump clears with a quarter unit to spare
  assert.deepEqual([trapAdvice([v], JUMP_RISE)!.voxelSize, trapAdvice([v], JUMP_RISE)!.layers, trapAdvice([], JUMP_RISE)], [2, 1, null]);
});

test("check_crater_traps: the traps of a flat coarse skin, the fix it names, and nothing edited; a fine skin passes", async () => {
  const st = newStore();
  skin(st, "8");
  const before = words(st);
  const r = checkCraterTraps(grids(st), { x0: 705, z0: -25, x1: 755, z1: 25, spacing: 10 }, [], null) as any;
  assert.equal(r.ok, false);
  assert.equal(r.traps.length, r.craters.tried);
  assert.deepEqual([r.traps[0].wall, r.traps[0].layers, r.traps[0].voxel, r.fix.voxelSize, r.craters.frames], [2.5, 1, 2.5, 2, "skin"]);
  assert.equal(words(st), before);
  const ok = newStore();
  skin(ok, "8", 2);
  assert.equal((checkCraterTraps(grids(ok), { x0: 705, z0: -25, x1: 755, z1: 25, spacing: 10 }, [], null) as any).ok, true);
  // points and a route; a weapon's radius; a radius with a weapon is refused
  const p = checkCraterTraps(grids(st), { points: [[730, 0]], weapon: "bazooka" }, [], null) as any;
  assert.deepEqual([p.craters.tried, p.craters.weapon, p.traps.length], [1, "bazooka", 1]);
  assert.equal((checkCraterTraps(grids(st), { along: [[710, 0], [750, 0]], spacing: 10 }, [], null) as any).craters.tried, 5);
  assert.throws(() => checkCraterTraps(grids(st), { radius: 2, weapon: "mine" }, [], null), /not both/);
  // only ground a spawn's walk reaches: a spawn on the skin keeps the lattice, one far off the level drops it
  const on = checkCraterTraps(grids(st), { x0: 705, z0: -25, x1: 755, z1: 25, spacing: 10 }, [[730, 8, 0]], null) as any;
  assert.equal(on.craters.tried, r.craters.tried);
  assert.match(on.craters.how, /a spawn's walk reaches/);
  // the tool answers through the registry
  const t = await callTool(craterTools as never, "check_crater_traps", sessionOf(st), { points: [[730, 0]], units: "world", radius: 60 }).catch((e: Error) => e);
  assert.ok(t instanceof Error, "points in world units land off the skin");
  const tw = await callTool(craterTools as never, "check_crater_traps", sessionOf(st), { points: [[14600, 0]], units: "world", radius: 60 });
  assert.deepEqual([tw.traps.length, tw.traps[0].wall, tw.rules.jump], [1, 50, 49.6]);
});

test("heightfield_drape craterSlope: the voxel is held to what a jump clears, and says what it costs; off by default and at a fine size it changes nothing", async () => {
  const base = { min: [700, -30], max: [760, 30], floor: 0, keepFloor: true, thickness: 3, core: false, expr: "8", material: 2, maxBlocks: 8 };
  const plainS = newStore(), offS = newStore(), onS = newStore(), fineS = newStore();
  const plain = await callTool(drapeTools as never, "heightfield_drape", sessionOf(plainS), { ...base, voxelSize: 2.5 });
  await callTool(drapeTools as never, "heightfield_drape", sessionOf(offS), { ...base, voxelSize: 2.5, craterSlope: false });
  assert.equal(words(offS), words(plainS));
  assert.equal(plain.craterSlope, undefined);
  const dry = await callTool(drapeTools as never, "heightfield_drape", sessionOf(onS), { ...base, voxelSize: 2.5, craterSlope: true, dryRun: true });
  assert.deepEqual([dry.craterSlope.voxelSize, dry.craterSlope.wall, dry.applied], [{ asked: 2.5, used: 2 }, { asked: 2.5, used: 2 }, false]);
  assert.ok(dry.skin.cells > dry.craterSlope.skinAsAsked.cells, "the finer skin costs more cells, and says so");
  await callTool(drapeTools as never, "heightfield_drape", sessionOf(onS), { ...base, voxelSize: 2.5, craterSlope: true });
  assert.equal((checkCraterTraps(grids(onS), { x0: 705, z0: -25, x1: 755, z1: 25, spacing: 10 }, [], null) as any).ok, true);
  assert.equal((checkCraterTraps(grids(plainS), { x0: 705, z0: -25, x1: 755, z1: 25, spacing: 10 }, [], null) as any).ok, false);
  // a size already under the cap is left alone; the rule's own numbers move the cap
  const fine = await callTool(drapeTools as never, "heightfield_drape", sessionOf(fineS), { ...base, voxelSize: 1.5, craterSlope: true });
  assert.deepEqual([fine.craterSlope.voxelSize, fine.craterSlope.skinAsAsked], [{ asked: 1.5, used: 1.5 }, undefined]);
  const two = await callTool(drapeTools as never, "heightfield_drape", sessionOf(newStore()), { ...base, voxelSize: 2.5, craterSlope: { layers: 2 }, dryRun: true });
  assert.equal(two.craterSlope.voxelSize.used, 1);
});

// ------------------------------------------------------------------ check_pits: filters, enclosed rooms

test("check_pits: at, radius and exclude pick pits whole; a roofed room no start walks into is enclosed, and fix leaves it", async () => {
  const st = newStore(), s = sessionOf(st);
  // two open pits in one slab, and a sealed hall in a keep beside it
  await callTool(structureTools as never, "build_compound", s, { at: [615, 0, 6], place: "new", material: 3, shapes: [
    { kind: "box", size: [30, 12], height: 6 }, { kind: "box", carve: true, size: [5, 5], height: 3, at: [-8, 3, 0] }, { kind: "box", carve: true, size: [5, 5], height: 3, at: [8, 3, 0] }] });
  await callTool(structureTools as never, "build_compound", s, { at: [615, 0, 40], place: "new", material: 3, shapes: [
    { kind: "box", size: [20, 20], height: 12 }, { kind: "box", carve: true, size: [10, 10], height: 5, at: [0, 3, 0] }] });
  const REGION = { x0: 590, z0: -10, x1: 640, z1: 60, units: "map" as const };
  const all = checkPits(st, REGION) as any;
  assert.equal(all.pits.length, 3);
  const hall = all.pits.find((p: { roofed?: boolean }) => p.roofed);
  assert.deepEqual([hall.enclosed, all.enclosed.count, all.pits.filter((p: { enclosed?: boolean }) => !p.enclosed).length], [true, 1, 2]);
  assert.equal(all.filter, undefined);
  // enclosed: "skip" leaves the hall out of the list and the verdict
  const skip = checkPits(st, { ...REGION, enclosed: "skip" }) as any;
  assert.deepEqual([skip.pits.length, skip.enclosed.listed], [2, false]);
  // a start that walks into the hall makes it a pit a worm is in: roofed, not enclosed
  const walked = checkPits(st, { ...REGION, from: [[615, 3, 40]] }) as any;
  const h2 = walked.pits.find((p: { roofed?: boolean }) => p.roofed);
  assert.deepEqual([h2.enclosed, h2.reachedFrom], [undefined, ["from[0]"]]);
  // at + radius: the west pit only; exclude: the east pit and the hall out
  const west = checkPits(st, { ...REGION, at: [607, 6], radius: 4 }) as any;
  assert.deepEqual([west.pits.length, Math.round(west.pits[0].at[0]) < 612, west.filter.at], [1, true, [607, 6]]);
  const ex = checkPits(st, { ...REGION, exclude: [{ at: [623, 6], radius: 4 }, { x0: 600, z0: 25, x1: 630, z1: 55 }] }) as any;
  assert.deepEqual([ex.pits.length, ex.filter.exclude], [1, 2]);
  assert.throws(() => checkPits(st, { ...REGION, radius: 3 }), /radius goes with at/);
  assert.throws(() => checkPits(st, { ...REGION, exclude: [{ x0: 1 }] }), /exclude\[0\]/);
  // fix: the two open pits are filled, the hall is left and named, with the default fixMaxDepth and with a deep one
  const hallCells = () => spansAt(grids(st).filter((g) => g.frame.new), 615.5, 40.5).length;
  assert.equal(hallCells(), 2);
  const r = fixPits(st, { ...REGION, fix: true, fixMaxDepth: 20 }) as any;
  assert.deepEqual([r.fix.before, r.pits.length, r.pits[0].enclosed], [3, 1, true]);
  assert.match(r.fix.skipped[0].why, /enclosed room/);
  assert.equal(hallCells(), 2, "the hall is still hollow");
  // fixEnclosed fills it
  const f = fixPits(st, { ...REGION, fix: true, fixMaxDepth: 20, fixEnclosed: true }) as any;
  assert.equal(f.pits.length, 0);
  assert.equal(hallCells(), 1);
});

// ------------------------------------------------------------------ build_outcrop

test("build_outcrop: with mesa.top and mesa.floor the steps come from that height, not from `height`", async () => {
  const a = newStore(), b = newStore();
  for (const st of [a, b]) await slab(st, 4);
  // height 4 alone: two voxels a step, two steps
  const plain = await callTool(mesaTools as never, "build_outcrop", sessionOf(a), { at: [600, 0], radius: 8, height: 4, material: 3, dryRun: true });
  assert.equal(plain.outcrop.face, undefined);
  assert.equal(plain.outcrop.mesa.strata.step, 2);
  // the mesa built is 16 tall (floor 2, top 18): four steps of four voxels, where `height` alone gave a step of one
  const own = await callTool(mesaTools as never, "build_outcrop", sessionOf(b), { at: [600, 0], radius: 8, height: 4, material: 3, dryRun: true, mesa: { floor: 2, top: 18 } });
  assert.deepEqual([own.outcrop.face, own.outcrop.mesa.strata.step, own.outcrop.mesa.top, own.outcrop.mesa.floor], [{ voxels: 16, steps: 4, step: 4, from: "mesa.top - mesa.floor" }, 4, 18, 2]);
  // a mesa voxelSize counts too
  const big = await callTool(mesaTools as never, "build_outcrop", sessionOf(b), { at: [600, 0], radius: 8, height: 4, material: 3, dryRun: true, mesa: { floor: 2, top: 18, voxelSize: 2 } });
  assert.deepEqual([big.outcrop.face.voxels, big.outcrop.face.steps, big.outcrop.mesa.strata.step], [8, 4, 2]);
});

// ------------------------------------------------------------------ the lints' search

test("searchTerms and matchesSearch: one string or a list, by name or by resource, whatever the case", () => {
  assert.deepEqual(searchTerms(undefined), ["_dtl"]);
  assert.deepEqual(searchTerms("Rock"), ["rock"]);
  assert.deepEqual(searchTerms(["A", "", "b"]), ["a", "b"]);
  assert.deepEqual(searchTerms(undefined, []), []);
  const d = { name: "VISIBLE_boulder_dtl", resource: "dustcrown.Rocks" };
  assert.equal(matchesSearch(d, searchTerms("_DTL")), true);
  assert.equal(matchesSearch(d, searchTerms("rocks")), true, "by resource");
  assert.equal(matchesSearch(d, searchTerms(["palm", "boulder"])), true);
  assert.equal(matchesSearch(d, searchTerms("palm")), false);
  assert.equal(matchesSearch({ name: "x", resource: null }, searchTerms("x")), true);
  assert.equal(matchesSearch(d, []), false);
});

// ------------------------------------------------------------------ the grade preview

test("gradePreview: a measured colour is the record, a colour beside it leans on it, a far one is the fit; no jump between them", () => {
  const fit = fitGrade([{ from: [200, 180, 120], to: [180, 150, 90] }, { from: [100, 90, 60], to: [80, 66, 40] }, { from: [240, 230, 200], to: [230, 215, 170] }]);
  const g: Grade = { ...fit, tint: { m: fit.m, pairs: 3, residual: 0 }, measured: [{ material: 9, from: [216, 173, 106], to: [203, 174, 113], tinted: true }] };
  const at = (c: [number, number, number]) => gradePreview(g, 9, c, "sun", true);
  const exact = at([216, 173, 106]);
  assert.deepEqual([exact.rgb, exact.how!.by, exact.how!.record!.distance], [[203, 174, 113], "measured", 0]);
  // one unit off: all but the record still, and said so
  const near = at([217, 173, 106]);
  assert.equal(near.how!.by, "interpolated");
  assert.ok(near.how!.pull! > 0.9 && Math.max(...near.rgb!.map((v, i) => Math.abs(v - [203, 174, 113][i]))) <= 2, JSON.stringify(near));
  // the old rule jumped from the record to the fit seven units away; the preview moves a little at a time
  let last = exact.rgb!, worst = 0;
  for (let d = 1; d <= GRADE_REACH + 4; d++) {
    const p = at([216 + d, 173, 106]).rgb!;
    worst = Math.max(worst, ...p.map((v, i) => Math.abs(v - last[i])));
    last = p;
  }
  const jump = Math.max(...gradeMaterial(g, 9, [222, 173, 106], true).map((v, i) => Math.abs(v - gradeMaterial(g, 9, [223, 173, 106], true)[i])));
  assert.ok(worst <= 4, `the preview's largest step is ${worst}`);
  assert.ok(jump > worst, `gradeMaterial steps ${jump} between 6 and 7 off the record`);
  // past the reach: the fit, named
  const far = at([216 + GRADE_REACH, 173, 106]);
  assert.deepEqual([far.how!.by, far.rgb], ["tint fit", gradeMaterial(g, 9, [216 + GRADE_REACH, 173, 106], true)]);
  // a material with no record of its own leans on the records measured the same way; shade without a shade fit is null
  assert.equal(gradePreview(g, 3, [216, 173, 107], "sun", true).how!.record!.material, 9);
  assert.equal(gradePreview(g, 3, [216, 173, 107], "sun", false).how!.by, "fit");
  assert.deepEqual(gradePreview(g, 9, [216, 173, 106], "shade", true), { rgb: null, how: null });
});

// ------------------------------------------------------------------ describe_dunes

const profileOf = (f: (d: number) => number, len = 80, step = 0.5, mat: (d: number) => number = () => 1): ProfileSample[] =>
  Array.from({ length: Math.round(len / step) + 1 }, (_, i) => ({ d: i * step, y: f(i * step), material: mat(i * step) }));

test("findCrests: a sine hill rolls over; an asymmetric dune has a sharp crest and a slip face; a step is no crest", () => {
  const hills = findCrests(profileOf((d) => 4 + 2 * Math.sin((d / 40) * 2 * Math.PI)));
  assert.equal(hills.length, 2);
  assert.ok(hills[0].slopeBreak < 20 && hills[0].lee - hills[0].windward < 3, JSON.stringify(hills[0]));
  assert.deepEqual(crestFaults(hills[0], null).map((f) => f.split(" (")[0]).slice(0, 3), ["the crest rolls over", "no slip face", "symmetric"]);
  // a saw-tooth dune: 12 degrees up over 30, 32 degrees down
  const up = Math.tan((12 * Math.PI) / 180), down = Math.tan((32 * Math.PI) / 180), peak = 30 * up, back = peak / down;
  const dune = (d: number) => { const t = d % 40; return 3 + (t < 30 ? t * up : Math.max(0, peak - (t - 30) * down)); };
  const dunes = findCrests(profileOf(dune));
  assert.equal(dunes.length, 2);
  const c = dunes[0];
  assert.ok(Math.abs(c.d - 30) <= 0.5 && Math.abs(c.windward - 12) < 1.5 && Math.abs(c.lee - 32) < 1.5 && c.leeSide === "after", JSON.stringify(c));
  assert.ok(Math.abs(c.slopeBreak - 44) < 3 && c.topWidth <= 1 && back < 12, JSON.stringify(c));
  assert.ok(hills[0].topWidth >= 3, `a sine hill's top is flat for ${hills[0].topWidth}`);
  assert.deepEqual(crestFaults(c, 40), []);
  assert.deepEqual(crestFaults(c, 5).map((f) => f.split(" (")[0]), ["the crest and its trough are 5 apart in brightness"]);
  // from the trough before it the crest stands over a worm's eye
  const wv = wormView(c)!;
  assert.ok(wv.skyline && wv.elevation > 5);
  // a single voxel step up is not a crest (nothing falls after it), and a low ripple is under the prominence
  assert.equal(findCrests(profileOf((d) => (d < 40 ? 2 : 6))).length, 0);
  assert.equal(findCrests(profileOf((d) => 4 + 0.4 * Math.sin(d))).length, 0);
  assert.equal(DUNE_RULES.minLee, 26);
});

test("describe_dunes: a draped saw-tooth field reads as dunes across it and not along it; soft hills are named; the colours give the contrast", () => {
  const st = newStore();
  // dunes running north-south (the height changes along x): 16.7 degrees up over 14, a slip face of 35 after
  skin(st, "3 + min(mod(x + 30, 20) * 0.3, max(0, 4.2 - (mod(x + 30, 20) - 14) * 0.7))", 1);
  const gs = grids(st);
  const colour = (m: number): [number, number, number] => (m === 2 ? [200, 180, 120] : [90, 80, 60]);
  const r = describeDunes(gs, { lines: [[702, 0, 758, 0]] }, null, colour) as any;
  assert.ok(r.field.crests >= 2, JSON.stringify(r.field));
  assert.ok(r.field.median.lee > r.field.median.windward + 8 && r.field.median.slopeBreak > 30, JSON.stringify(r.field.median));
  assert.ok(r.crests.every((c: { leeSide: string }) => c.leeSide === "after"));
  // one material everywhere: the crest and the trough are one tone, and that is the only fault
  assert.ok(r.crests.every((c: { faults?: string[] }) => c.faults?.length === 1 && /one tone/.test(c.faults[0])), JSON.stringify(r.crests[0]));
  // without colours the shape alone is judged, and it passes
  const shape = describeDunes(gs, { lines: [[702, 0, 758, 0]] }, null, null) as any;
  assert.deepEqual([shape.ok, shape.field.crested, typeof shape.colours], [true, shape.field.crests, "string"]);
  // with no line: both axes are tried and the one across the dunes (azimuth 90, along x) is taken
  const auto = describeDunes(gs, {}, null, null) as any;
  assert.match(auto.summary, /azimuth 90/);
  assert.equal(auto.lines.length, 6);
  // a line along a ridge finds no crest
  assert.equal((describeDunes(gs, { lines: [[712, -28, 712, 28]] }, null, null) as any).crests.length, 0);
  // soft sine hills fail by shape
  const soft = newStore();
  skin(soft, "5 + 1.5 * sin(x / 60 * 6.2832 * 2)", 1);
  const sr = describeDunes(grids(soft), { lines: [[702, 0, 758, 0]] }, null, null) as any;
  assert.equal(sr.ok, false);
  assert.match(sr.summary, /rolls over/);
});
