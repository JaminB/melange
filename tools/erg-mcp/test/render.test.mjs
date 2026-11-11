// Tool group "render" against the mock Oasis server (synthetic level): render_view's presets, explicit camera, game
// camera, orbit sheet and refusals; the PNG is decoded here. The renderer itself is covered by render.test.ts.
import assert from "node:assert/strict";
import { test } from "node:test";
import { inflateSync } from "node:zlib";
import { fileURLToPath } from "node:url";
import { startMock } from "../../../web/test/e2e/mock-server.mjs";
import { spawnMcp } from "./harness.mjs";

const root = fileURLToPath(new URL("../../../web/dist", import.meta.url));

/** width, height and the distinct colours of an 8-bit truecolour PNG from an MCP image part. */
function decode(part) {
  assert.equal(part.type, "image");
  const png = Buffer.from(part.data, "base64");
  const w = png.readUInt32BE(16), h = png.readUInt32BE(20);
  let at = 8, idat = Buffer.alloc(0);
  while (at < png.length) {
    const len = png.readUInt32BE(at);
    if (png.toString("latin1", at + 4, at + 8) === "IDAT") idat = Buffer.concat([idat, png.subarray(at + 8, at + 8 + len)]);
    at += 12 + len;
  }
  const raw = inflateSync(idat);
  assert.equal(raw.length, h * (1 + w * 3));
  const colours = new Set();
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) colours.add(raw.subarray(y * (1 + w * 3) + 1 + x * 3, y * (1 + w * 3) + 4 + x * 3).join(","));
  return { w, h, colours };
}

test("render_view over the mock server", async () => {
  const mock = await startMock({ root });
  const m = spawnMcp({ ERG_MCP_URL: mock.url });
  try {
    await m.initialize();
    assert.ok((await m.request("tools/list")).result.tools.some((t) => t.name === "render_view"));
    assert.ok((await m.tool("render_view")).isError, "no project");
    await m.tool("new_project", { base: "Multi.Synthetic12", slug: "mcprender1", title: "Render" });
    assert.ok(!(await m.tool("open_project", { id: "mcprender1" })).isError);

    const d = await m.tool("render_view");
    assert.ok(!d.isError, d.text);
    const img = decode(d.content[0]), info = JSON.parse(d.content[1].text);
    assert.deepEqual([img.w, img.h], [960, 540]);
    assert.ok(img.colours.size > 100, `colours ${img.colours.size}`);
    const cam = info.cameras[0];
    assert.equal(cam.fov, 60);
    assert.ok(Math.abs(cam.azimuth - 135) < 0.01 && Math.abs(cam.elevation - 35) < 0.01);
    assert.ok(cam.pos.world.every((v, k) => Math.abs(v - cam.pos.map[k] * 20) < 0.1));
    assert.ok(cam.framesDrawn >= 10 && typeof info.timeMs === "number");
    assert.ok(info.markers.placed.some((p) => p.kind === "spawn"));

    const w = await m.tool("render_view", { pos: [0, 2000, 3000], target: [0, 0, 0], units: "world", fov: 45, width: 320, height: 200, timeOfDay: "NIGHT", fog: 4000 });
    assert.ok(!w.isError, w.text);
    const wi = JSON.parse(w.content[1].text);
    assert.deepEqual(wi.cameras[0].pos.map, [0, 100, 150]);
    assert.equal(wi.timeOfDay, "NIGHT");
    assert.deepEqual([decode(w.content[0]).w, decode(w.content[0]).h], [320, 200]);

    const g = await m.tool("render_view", { view: "game", at: [0, 10, 0], width: 200, height: 120 });
    assert.ok(!g.isError, g.text);
    assert.ok(Math.abs(JSON.parse(g.content[1].text).cameras[0].distance.world - 170) < 0.01);

    const o = await m.tool("render_view", { view: "orbit", width: 400, height: 240, antialias: true, markers: false });
    assert.ok(!o.isError, o.text);
    const oi = JSON.parse(o.content[1].text);
    assert.deepEqual(oi.cameras.map((c) => Math.round(c.azimuth)), [315, 45, 135, 225]);
    assert.deepEqual([decode(o.content[0]).w, decode(o.content[0]).h], [400, 240]);

    assert.ok((await m.tool("render_view", { pos: [0, 1, 0] })).isError, "pos without target");
    assert.ok((await m.tool("render_view", { view: "game" })).isError, "game without at");
    assert.ok((await m.tool("render_view", { width: 4000 })).isError, "too wide");
    assert.equal(m.stray.length, 0);
  } finally {
    await m.close();
    await mock.close();
  }
});
