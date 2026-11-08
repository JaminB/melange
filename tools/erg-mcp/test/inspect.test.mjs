// Tool group "inspect" against the mock Oasis server (synthetic level): the ASCII map, the PNG renders (decoded here),
// query_ground, list_materials, and the refusals. The real server is covered by the live run noted in docs/erg-mcp.md.
import assert from "node:assert/strict";
import { test } from "node:test";
import { inflateSync } from "node:zlib";
import { fileURLToPath } from "node:url";
import { startMock } from "../../../web/test/e2e/mock-server.mjs";
import { spawnMcp } from "./harness.mjs";

const root = fileURLToPath(new URL("../../../web/dist", import.meta.url));

/** width, height and the RGB pixels of an 8-bit truecolour PNG from an MCP image part. */
function decode(part) {
  assert.equal(part.type, "image");
  assert.equal(part.mimeType, "image/png");
  const png = Buffer.from(part.data, "base64");
  assert.deepEqual([...png.subarray(0, 8)], [137, 80, 78, 71, 13, 10, 26, 10]);
  const w = png.readUInt32BE(16), h = png.readUInt32BE(20);
  assert.equal(png[24], 8);
  assert.equal(png[25], 2);
  let at = 8, idat = Buffer.alloc(0);
  while (at < png.length) {
    const len = png.readUInt32BE(at), type = png.toString("latin1", at + 4, at + 8);
    if (type === "IDAT") idat = Buffer.concat([idat, png.subarray(at + 8, at + 8 + len)]);
    at += 12 + len;
  }
  const raw = inflateSync(idat);
  assert.equal(raw.length, h * (1 + w * 3));
  const px = (x, y) => [...raw.subarray(y * (1 + w * 3) + 1 + x * 3, y * (1 + w * 3) + 1 + x * 3 + 3)];
  const colours = new Set();
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) colours.add(px(x, y).join(","));
  return { w, h, px, colours };
}

const MAP_ROW = /^\s*-?[\d.]+ [.0-9a-zSCTRFO]+$/;

test("inspect tools over the mock server", async () => {
  const mock = await startMock({ root });
  const m = spawnMcp({ ERG_MCP_URL: mock.url });
  try {
    await m.initialize();
    const list = (await m.request("tools/list")).result.tools.map((t) => t.name);
    for (const n of ["heightmap_ascii", "render_topdown", "render_side", "query_ground", "list_materials"]) assert.ok(list.includes(n), n);

    const noProject = await m.tool("heightmap_ascii");
    assert.ok(noProject.isError && /no project is open/.test(noProject.text), noProject.text);
    await m.tool("new_project", { base: "Multi.Synthetic12", slug: "mcpinspect1", title: "Inspect" });
    assert.ok(!(await m.tool("open_project", { id: "mcpinspect1" })).isError);
    const lv = (await m.tool("describe_level")).json;
    const ext = lv.extent.map;

    // ---- heightmap_ascii
    const a = await m.tool("heightmap_ascii");
    assert.ok(!a.isError, a.text);
    const rows = a.text.split("\n").filter((l) => MAP_ROW.test(l));
    assert.ok(rows.length >= 20, `rows ${rows.length}`);
    assert.ok(rows.every((r) => r.length === rows[0].length), "rows are one width");
    assert.ok(/Key \(height bands/.test(a.text) && /\.  no ground/.test(a.text));
    assert.ok(/S {2}spawn/.test(a.text), "spawns are overlaid and keyed");
    assert.equal((a.text.match(/^S WORM\d+ at/gm) ?? []).length, lv.spawns.length);
    const fb = lv.frames[0].bounds.map, cx = Math.floor((fb.min[0] + fb.max[0]) / 2), cz = Math.floor((fb.min[2] + fb.max[2]) / 2);
    const sub = await m.tool("heightmap_ascii", { x0: cx - 15, z0: cz - 10, x1: cx + 15, z1: cz + 10, step: 1, markers: false });
    const subRows = sub.text.split("\n").filter((l) => MAP_ROW.test(l));
    assert.equal(subRows.length, 20);
    assert.ok(subRows.every((r) => r.trimEnd().split(" ").pop().length === 30));
    assert.ok(!/Markers:/.test(sub.text));
    const world = await m.tool("heightmap_ascii", { x0: (cx - 15) * 20, z0: (cz - 10) * 20, x1: (cx - 15) * 20 + 400, z1: (cz - 10) * 20 + 300, step: 20, units: "world", markers: false });
    assert.ok(/world units/.test(world.text) && world.text.split("\n").filter((l) => MAP_ROW.test(l)).length === 15, world.text.slice(0, 400));
    const big = await m.tool("heightmap_ascii", { step: 0.25 });
    assert.ok(big.isError && /too many/.test(big.text), big.text.slice(0, 200));
    const empty = await m.tool("heightmap_ascii", { x0: 5000, z0: 5000, x1: 5010, z1: 5010 });
    assert.ok(!empty.isError && /No ground in this region/.test(empty.text));
    const zero = await m.tool("heightmap_ascii", { x0: 1, z0: 1, x1: 1, z1: 5 });
    assert.ok(zero.isError && /empty/.test(zero.text));

    // ---- query_ground
    const px = cx, pz = cz;
    let found = null;
    for (let dx = -8; dx <= 8 && !found; dx += 0.5)
      for (let dz = -8; dz <= 8 && !found; dz += 0.5) {
        const q = await m.tool("query_ground", { x: px + dx, z: pz + dz });
        if (q.json?.ground) found = { x: px + dx, z: pz + dz, q: q.json };
      }
    assert.ok(found, "some column has ground");
    const g = found.q.ground;
    assert.ok(Math.abs(g.top.world - g.top.map * 20) < 0.05);
    assert.equal(typeof g.frameName, "string");
    assert.ok(g.material >= 0 && g.material < 64);
    assert.ok(found.q.spans.count >= 1 && found.q.spans.listed[0].top.map === g.top.map);
    assert.deepEqual(found.q.standOn, g.top);
    const inWorld = await m.tool("query_ground", { x: found.x * 20, z: found.z * 20, units: "world" });
    assert.equal(inWorld.json.ground.top.map, g.top.map);
    const under = await m.tool("query_ground", { x: found.x, z: found.z, below: g.bottom.map });
    assert.ok(under.json.ground === null || under.json.ground.top.map <= g.bottom.map + 1e-9);
    const nothing = await m.tool("query_ground", { x: 9999, z: 9999 });
    assert.equal(nothing.json.ground, null);
    assert.ok((await m.tool("query_ground", { x: 1 })).isError);

    // ---- render_topdown
    const t = await m.tool("render_topdown");
    assert.ok(!t.isError, t.text);
    assert.equal(t.content[0].type, "image");
    const info = JSON.parse(t.content[1].text);
    const img = decode(t.content[0]);
    assert.equal(img.w, info.image.width);
    assert.equal(img.h, info.image.height);
    assert.ok(img.colours.size > 20, `colours ${img.colours.size}`);
    assert.equal(info.markers.placed.filter((p) => p.kind === "spawn").length, lv.spawns.length);
    assert.ok(info.materials.length >= 1 && /^#[0-9a-f]{6}$/.test(info.materials[0].colour));
    assert.ok(info.reading.includes("x grows right"));
    // a spawn marker is drawn at its pixel (green fill)
    const sp = info.markers.placed.find((p) => p.kind === "spawn");
    const cell = Number(info.image.cellSize.split(" ")[0]), s = info.image.pixelsPerCell, left = info.image.margin;
    const sx = left + Math.round(((sp.at[0] - info.region.x[0]) / cell) * s);
    const sz = left + Math.round(((sp.at[2] - info.region.z[0]) / cell) * s);
    assert.deepEqual(img.px(sx, sz), [60, 220, 60]);
    const flat = await m.tool("render_topdown", { shading: "material", markers: false, scale: 2, x0: ext.min[0], z0: ext.min[2], x1: ext.min[0] + 40, z1: ext.min[2] + 40 });
    const flatImg = decode(flat.content[0]);
    assert.equal(flatImg.w, 40 * 2 + 14);
    assert.ok(!flatImg.colours.has("60,220,60"));
    assert.deepEqual(JSON.parse(flat.content[1].text).markers.placed, []);
    const huge = await m.tool("render_topdown", { step: 0.25 });
    assert.ok(huge.isError, "refused");

    // ---- render_side
        const side = await m.tool("render_side", { x0: fb.min[0] - 2, z0: cz, x1: fb.max[0] + 2, z1: cz });
    assert.ok(!side.isError, side.text);
    const sinfo = JSON.parse(side.content[1].text);
    const simg = decode(side.content[0]);
    assert.equal(simg.w, sinfo.image.width);
    assert.ok(simg.colours.size > 5);
    assert.ok(sinfo.topProfile.points.some((p) => p[1] !== null));
    assert.ok(Math.abs(sinfo.line.length - (fb.max[0] - fb.min[0] + 4)) < 0.01);
    assert.ok(sinfo.y[1] > sinfo.y[0]);
    const zeroLine = await m.tool("render_side", { x0: 1, z0: 1, x1: 1, z1: 1 });
    assert.ok(zeroLine.isError && /no length/.test(zeroLine.text));
    assert.ok((await m.tool("render_side", { x0: 1, z0: 1 })).isError);

    // ---- list_materials: the palette comes from the server; the material file may be missing on the mock
    const mat = await m.tool("list_materials", { search: "worm" });
    assert.ok(!mat.isError, mat.text);
    assert.equal(mat.json.palette.theme, "BUILDING");
    assert.ok(mat.json.palette.entries.every((e) => /worm/i.test(`${e.name} ${e.resource}`)));
    assert.ok(mat.json.materials, "materials answer is present (names or the server's error)");
    const spawnsOnly = await m.tool("list_materials", { role: "spawn", theme: "BUILDING" });
    assert.ok(spawnsOnly.json.palette.entries.every((e) => e.role === "spawn"));
    await m.tool("close");
    const noProj = await m.tool("list_materials");
    assert.ok(!noProj.isError && Array.isArray(noProj.json.themes), noProj.text);
    const byTheme = await m.tool("list_materials", { theme: "BUILDING" });
    assert.equal(byTheme.json.palette.theme, "BUILDING");
    // with no project, a theme lists its own material file, with the server's texture colours
    assert.equal(byTheme.json.materials.file, "ThemeBuilding\\ThemeBuilding.txt");
    assert.equal(byTheme.json.materials.list.length, 8);
    assert.deepEqual(byTheme.json.materials.list[6], { index: 6, name: "Lava", colour: "#c83c10", textured: true });
    assert.equal(byTheme.json.materials.list[7].textured, false);
    // a file whose separator was lost (a JSON "\d" read as "d") or written with a slash is matched to the listed one
    const lost = await m.tool("list_materials", { materialFile: "Mapssynthetic.txt", only: "materials" });
    assert.equal(lost.json.materials.file, "Maps\\synthetic.txt", lost.text);
    assert.match(lost.json.materials.note, /read as Maps\\synthetic\.txt/);
    const slash = await m.tool("list_materials", { materialFile: "maps/synthetic.txt", only: "materials" });
    assert.equal(slash.json.materials.file, "Maps\\synthetic.txt", slash.text);
    const none = await m.tool("list_materials", { materialFile: "Maps\\nope.txt", only: "materials" });
    assert.match(none.json.materials.error, /could not read/);
    assert.ok(none.json.materials.materialFiles.includes("Maps\\synthetic.txt"));
    // the swatch sheet
    const sw = await m.tool("preview_materials", { theme: "CAMELOT" });
    assert.ok(!sw.isError, sw.text);
    assert.equal(sw.content[0].type, "image");
    const sheet = JSON.parse(sw.content[1].text);
    assert.equal(sheet.file, "ThemeCamelot\\ThemeCamelot.txt");

    assert.equal(sheet.materials[0].colour, "#4a7a30");
    assert.equal(m.stray.length, 0);
  } finally {
    await m.close();
    await mock.close();
  }
});
