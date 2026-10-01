// Browser tests of the Erg editor in headless Edge (SwiftShader WebGL) against the mock server's synthetic level
// service: lazy loading, the 400-frame load and frame rate, pick/move/rotate with undo and redo, placing knots and
// objects with water and theme and a reload, drafts, and a dropped connection.
//   node web/test/e2e/erg.mjs [<built web app folder>]
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { chromium } from "playwright-core";
import { startMock } from "./mock-server.mjs";

const root = process.argv.slice(2).find((a) => !a.startsWith("--")) ?? "web/dist";
const results = [];
const check = (name, ok, detail = "") => {
  results.push({ name, ok: !!ok });
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? ` ${detail}` : ""}`);
};
let current;
const attempt = async (name, fn) => {
  try {
    await fn();
  } catch (e) {
    check(name, false, String(e?.message ?? e).split("\n")[0]);
    const shot = `web/test/out/fail-erg-${name.replace(/\W+/g, "_")}.png`;
    await current?.screenshot({ path: shot }).then(() => console.log(`     screenshot: ${shot}`), () => {});
  }
};

async function openPage(browser, mock, hash = "", viewport = { width: 1400, height: 860 }) {
  const page = await browser.newPage({ viewport });
  const errors = [];
  const scripts = [];
  page.on("pageerror", (e) => errors.push(e.message));
  page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
  page.on("request", (r) => { if (r.url().endsWith(".js")) scripts.push(r.url()); });
  current = page;
  await page.goto(mock.url, { waitUntil: "load" });
  await page.waitForSelector(".badge-open", { timeout: 15000 });
  if (hash) await page.evaluate((h) => { location.hash = h; }, hash);
  return { page, errors, scripts };
}

// Runs fn(editor handles, arg) in the page (a DevTools evaluation, which the page's CSP does not cover).
const erg = (page, fn, arg) =>
  page.evaluate(`(${fn.toString()})(document.querySelector("[data-erg-editor]").__erg, ${JSON.stringify(arg ?? null)})`);

async function openErg(page) {
  await page.locator('nav [data-panel="erg"]').click();
  await page.waitForSelector("[data-erg-home], [data-erg-editor]", { timeout: 10000 });
}

async function createProject(page, base, slug, title) {
  await page.waitForSelector("[data-erg-new]", { timeout: 10000 });
  await page.selectOption('[data-control="base"]', base);
  await page.fill('[data-control="slug"]', slug);
  await page.fill('[data-control="title"]', title);
  const t0 = Date.now();
  await page.locator('[data-action="create"]').click();
  await page.waitForSelector("[data-erg-view][data-first-frame]", { timeout: 20000 });
  return Date.now() - t0;
}

// The static imports of main.js, recursively: what every page load pays for.
function initialJs(dist) {
  const seen = new Set();
  const walk = (rel) => {
    if (seen.has(rel)) return;
    seen.add(rel);
    const text = readFileSync(join(dist, "app", rel), "utf8");
    for (const m of text.matchAll(/(?:from|import)\s*"\.\/((?:chunks\/)?[^"]+\.js)"/g)) walk(m[1].startsWith("chunks/") || rel === "main.js" ? m[1] : `chunks/${m[1]}`);
  };
  walk("main.js");
  let bytes = 0, three = false, ergCode = false;
  for (const f of seen) {
    const t = readFileSync(join(dist, "app", f), "utf8");
    bytes += t.length;
    three ||= t.includes("WebGLRenderer") || t.includes("WEBGL_");
    ergCode ||= t.includes("erg-scene/1") || t.includes("erg-mesher");
  }
  return { files: [...seen], bytes, three, ergCode };
}

async function suite(browser) {
  const mock = await startMock({ root });
  let { page, errors, scripts } = await openPage(browser, mock);
  try {
    await attempt("lazy loading", async () => {
      const init = initialJs(root);
      check("initial JS has neither three.js nor the editor", !init.three && !init.ergCode, `${init.files.length} files, ${init.bytes} bytes`);
      check("the initial page load fetched no editor chunk", !scripts.some((u) => /\/(Erg|viewport)-/.test(u)), scripts.map((u) => u.split("/").pop()).join(","));
      await openErg(page);
      check("opening Erg fetches the panel chunk", scripts.some((u) => /\/Erg-/.test(u)));
      check("the project list shows before any 3D code loads", !scripts.some((u) => /\/viewport-/.test(u)));
    });

    await attempt("400-frame load", async () => {
      const ms = await createProject(page, "Multi.Synthetic400", "big", "Big Synthetic");
      const stats = await erg(page, (e) => e.view.stats());
      check("400 frames: first frame drawn within 2 s of Create", ms <= 2000, `${ms} ms end to end, view ${stats.firstFrameMs?.toFixed(0)} ms, mesher ${stats.meshMs?.toFixed(0)} ms, worker=${stats.threaded}`);
      check("400 frames: meshed off the main thread", stats.threaded === true);
      check("400 frames: at most 400 draw calls", stats.drawCalls > 0 && stats.drawCalls <= 400, `${stats.drawCalls} calls, ${stats.triangles} triangles, ${stats.buckets} buckets`);
      const fps = await erg(page, (e) => e.view.benchmark(3000));
      check("400 frames: orbit at 30 fps or more in SwiftShader", fps >= 30, `${fps.toFixed(1)} fps`);
      await page.screenshot({ path: "web/test/out/erg-400.png" });
      await page.locator('[data-action="close"]').click();
    });

    await attempt("pick, move, rotate, undo, redo", async () => {
      await createProject(page, "Multi.Synthetic12", "edit", "Edit Test");
      const p0 = await erg(page, (e) => e.store.patchText());
      const drum = await erg(page, (e) => {
        const d = e.store.scene.details.find((x) => x.name === "oildrum");
        e.store.select([]);
        return d.id;
      });
      await erg(page, (e, id) => { e.store.select([id]); e.view.focus(); }, drum);
      await page.waitForTimeout(150);
      await erg(page, (e) => e.store.select([]));
      const at = await erg(page, (e, id) => e.view.screenOf(id), drum);
      const box = await page.locator("[data-erg-view] canvas").boundingBox();
      await page.mouse.click(box.x + at[0], box.y + at[1]);
      const sel = await erg(page, (e) => e.store.selection);
      check("pick: clicking a marker selects it", sel.length === 1 && sel[0] === drum, JSON.stringify(sel));

      const before = await erg(page, (e, id) => e.store.detail(id).pos, drum);
      const c = await erg(page, (e, id) => e.view.screenOfWorld(e.store.frames.detailWorld(e.store.detail(id))), drum);
      const depth0 = await erg(page, (e) => e.store.stack.depth);
      await page.mouse.move(box.x + c[0], box.y + c[1]);
      await page.mouse.down();
      for (let i = 1; i <= 10; i++) await page.mouse.move(box.x + c[0] + i * 8, box.y + c[1] + i * 3);
      await page.mouse.up();
      const after = await erg(page, (e, id) => e.store.detail(id).pos, drum);
      const depth1 = await erg(page, (e) => e.store.stack.depth);
      check("move: dragging the gizmo moves the detail as one undo step", JSON.stringify(after) !== JSON.stringify(before) && depth1 === depth0 + 1,
        `${JSON.stringify(before)} -> ${JSON.stringify(after)}, depth ${depth0} -> ${depth1}`);

      await page.keyboard.press("e");
      await page.waitForTimeout(100);
      const c2 = await erg(page, (e, id) => e.view.screenOfWorld(e.store.frames.detailWorld(e.store.detail(id))), drum);
      await page.mouse.move(box.x + c2[0] + 2, box.y + c2[1] + 2);
      await page.mouse.down();
      for (let i = 1; i <= 10; i++) await page.mouse.move(box.x + c2[0] + 2 + i * 6, box.y + c2[1] + 2 - i * 4);
      await page.mouse.up();
      let rot = await erg(page, (e, id) => e.store.detail(id).rot, drum);
      if (rot.every((v) => v === 0)) {
        // The trackball handle missed: rotate through the properties panel instead.
        await page.locator('[data-tab="props"]').click();
        await page.fill('[data-field="rot.1"]', "45");
        await page.locator('[data-field="rot.1"]').press("Enter");
        await page.locator('[data-field="rot.1"]').blur();
        rot = await erg(page, (e, id) => e.store.detail(id).rot, drum);
      }
      check("rotate: the detail turns", rot.some((v) => v !== 0), JSON.stringify(rot));
      await page.keyboard.press("w");
      const p2 = await erg(page, (e) => e.store.patchText());
      const depth2 = await erg(page, (e) => e.store.stack.depth);
      await page.locator("[data-erg-view] canvas").focus();
      for (let i = depth0; i < depth2; i++) await page.keyboard.press("Control+z");
      const pu = await erg(page, (e) => e.store.patchText());
      for (let i = depth0; i < depth2; i++) await page.keyboard.press("Control+Shift+z");
      const pr = await erg(page, (e) => e.store.patchText());
      check("undo returns the patch byte for byte", pu === p0);
      check("redo returns the edited patch byte for byte", pr === p2 && p2 !== p0);
      const sculpt = await erg(page, (e) => {
        const sc = e.terrain.sculptor, g = sc.frames.find((x) => x.frame.size[0] >= 4 && x.frame.size[2] >= 4);
        const differ = e.store.scene.frames.some((f) => f.voxels !== null && f.voxels !== e.store.voxelRef(f.id));
        if (!g) return { frames: sc.frames.length, differ };
        const w = (p) => [0, 1, 2].map((i) => g.toWorld[i * 4] * p[0] + g.toWorld[i * 4 + 1] * p[1] + g.toWorld[i * 4 + 2] * p[2] + g.toWorld[i * 4 + 3]);
        const top = w([1.5, g.frame.size[1] + 5, 1.5]), low = w([1.5, 0, 1.5]);
        const hit = sc.pick(top, [low[0] - top[0], low[1] - top[1], low[2] - top[2]]);
        const brush = { mode: "carve", shape: "box", size: [3, 3, 3], material: 0 };
        const r = hit ? sc.step(sc.anchor(hit, brush), brush) : null;
        sc.end();
        const ops = JSON.parse(e.store.patchText()).ops.filter((o) => o.op === "voxels").length;
        if (r?.changed) e.store.undo();
        return { frames: sc.frames.length, differ, hit: !!hit, changed: r?.changed ?? 0, ops };
      });
      check("sculpt: with the project's refs unlike the base's, a carve adds a voxels op", sculpt.differ && sculpt.hit && sculpt.changed > 0 && sculpt.ops === 1,
        JSON.stringify(sculpt));
      await page.locator('[data-action="close"]').click();
    });

    await attempt("knots, objects, water, theme, save, reload", async () => {
      await createProject(page, "Multi.Synthetic12", "place", "Place Test");
      const roles = await page.locator("[data-erg-outliner] [data-role]").evaluateAll((els) => els.map((b) => b.getAttribute("data-role")));
      for (const r of roles) if (r !== "spawn") await page.locator(`[data-erg-outliner] [data-role="${r}"]`).click();
      const items = page.locator("[data-erg-outliner] [data-detail]");
      check("outliner: the role filter leaves the 8 knots", (await items.count()) === 8);
      await items.first().click();
      for (let i = 1; i < 8; i++) await items.nth(i).click({ modifiers: ["Control"] });
      await page.locator('[data-action="delete"]').click();
      check("delete removes the selected knots", (await erg(page, (e) => e.store.scene.details.filter((d) => /^WORM\d$/.test(d.name)).length)) === 0);
      for (const r of roles) if (r !== "spawn") await page.locator(`[data-erg-outliner] [data-role="${r}"]`).click();

      const spots = await erg(page, (e) => {
        const out = [];
        for (const f of e.store.scene.frames) {
          if (f.voxels === null || !f.size[0]) continue;
          const w = e.store.frames.worldOf(f.id);
          const p = [f.size[0] / 2, f.size[1], f.size[2] / 2];
          const q = [0, 1, 2].map((i) => w[i * 4] * p[0] + w[i * 4 + 1] * p[1] + w[i * 4 + 2] * p[2] + w[i * 4 + 3]);
          out.push(q);
        }
        return out;
      });
      await erg(page, (e) => { e.store.select([]); e.view.focus(); });
      const box = await page.locator("[data-erg-view] canvas").boundingBox();
      const clickAt = async (i) => {
        const box = await page.locator("[data-erg-view] canvas").boundingBox();
        const s = await erg(page, (e, p) => e.view.screenOfWorld(p), spots[i % spots.length]);
        await page.mouse.click(box.x + Math.min(box.width - 5, Math.max(5, s[0])), box.y + Math.min(box.height - 5, Math.max(5, s[1])));
        const msg = page.locator("[data-erg-message]");
        if (await msg.count()) notes.push(`${i}: ${await msg.textContent()}`);
      };
      const notes = [];
      await page.locator('[data-place="knot"]').click();
      for (let i = 0; i < 8; i++) await clickAt(i);
      await page.locator('[data-place="oildrum"]').click();
      for (let i = 0; i < 3; i++) await clickAt(i + 8);
      await page.locator('[data-place="mine"]').click();
      for (let i = 0; i < 2; i++) await clickAt(i + 3);
      await page.keyboard.press("Escape");
      const counts = await erg(page, (e) => {
        const d = e.store.scene.details.filter((x) => x.src === null);
        return { knots: d.filter((x) => /^WORM[0-7]$/.test(x.name)).length, drums: d.filter((x) => x.name === "oildrum").length, mines: d.filter((x) => x.name === "mine").length };
      });
      check("placed 8 knots, 3 drums and 2 mines", counts.knots === 8 && counts.drums === 3 && counts.mines === 2, `${JSON.stringify(counts)} ${notes.join("; ")}`);

      await page.locator('[data-tab="level"]').click();
      await page.locator('[data-field="water-set"]').check();
      await page.fill('[data-field="water"]', "40");
      await page.locator('[data-field="water"]').press("Enter");
      await page.locator('[data-field="water"]').blur();
      await page.selectOption('[data-field="theme"]', "CAMELOT");
      await page.selectOption('[data-field="spawns"]', "knots");
      const lvl = await erg(page, (e) => [e.store.scene.water.level, e.store.scene.databank.theme, e.store.scene.spawns.mode]);
      check("water 40, theme CAMELOT, knot spawns", JSON.stringify(lvl) === JSON.stringify([40, "CAMELOT", "knots"]), JSON.stringify(lvl));
      await page.locator('[data-tab="checks"]').click();
      const checks = await page.locator("[data-erg-checks]").getAttribute("data-erg-checks");
      check("checks list renders", checks !== null, `${checks} issues`);

      const saves = mock.state.erg.saves;
      await page.locator("[data-erg-view] canvas").focus();
      await page.keyboard.press("Control+s");
      await page.waitForSelector('[data-erg-save="ok"]', { timeout: 5000 });
      check("Ctrl+S saves through level.save", mock.state.erg.saves === saves + 1);
      // Blob refs run on across loads (as the service numbers them), so they are left out.
      const sceneBefore = await erg(page, (e) => JSON.stringify(e.store.scene, (k, v) => (["ref", "voxels", "heightMap"].includes(k) && typeof v === "number" ? 0 : v)));
      const patchBefore = await erg(page, (e) => e.store.patchText());
      await page.screenshot({ path: "web/test/out/erg-placed.png" });

      await page.reload({ waitUntil: "load" });
      await page.waitForSelector("[data-erg-view][data-first-frame]", { timeout: 20000 });
      const sceneAfter = await erg(page, (e) => JSON.stringify(e.store.scene, (k, v) => (["ref", "voxels", "heightMap"].includes(k) && typeof v === "number" ? 0 : v)));
      const patchAfter = await erg(page, (e) => e.store.patchText());
      check("after a reload the project reopens with the identical scene", sceneAfter === sceneBefore);
      check("after a reload the patch is identical and nothing is dirty", patchAfter === patchBefore &&
        (await page.locator("[data-erg-editor]").getAttribute("data-dirty")) === "0");
      check("no draft is offered after a save", (await page.locator("[data-erg-restored]").count()) === 0);
    });

    await attempt("drafts", async () => {
      const id = await erg(page, (e) => e.store.scene.details[0].id);
      await erg(page, (e, x) => e.store.select([x]), id);
      await page.locator('[data-tab="props"]').click();
      await page.fill('[data-field="pos.0"]', "123");
      await page.locator('[data-field="pos.0"]').press("Enter");
      await page.locator('[data-field="pos.0"]').blur();
      await page.waitForTimeout(500);
      await page.reload({ waitUntil: "load" });
      await page.waitForSelector("[data-erg-restored]", { timeout: 20000 });
      const pos = await erg(page, (e, x) => e.store.detail(x).pos[0], id);
      check("an unsaved edit survives a reload as a draft", Math.abs(pos * 20 - 123) < 1e-6 &&
        (await page.locator("[data-erg-editor]").getAttribute("data-dirty")) === "1", String(pos));
      await page.locator('[data-action="discard-draft"]').click();
      await page.waitForFunction(() => {
        const e = document.querySelector("[data-erg-editor]")?.__erg;
        return e && !e.store.dirty && e.view && document.querySelector("[data-erg-view]")?.dataset.firstFrame;
      }, null, { timeout: 20000 });
      check("discarding the draft returns to the saved project", (await page.locator("[data-erg-editor]").getAttribute("data-dirty")) === "0");
    });

    await attempt("dropped connection", async () => {
      const id = await erg(page, (e) => e.store.scene.details[1].id);
      await erg(page, (e, x) => e.store.select([x]), id);
      await page.fill('[data-field="pos.2"]', "77");
      await page.locator('[data-field="pos.2"]').press("Enter");
      await page.locator('[data-field="pos.2"]').blur();
      const depth = await erg(page, (e) => e.store.stack.depth);
      mock.drop(2500);
      await page.waitForSelector("[data-erg-offline]", { timeout: 3000 });
      check("a dropped connection shows 'not connected'", /Not connected/.test(await page.locator("[data-erg-offline]").textContent()));
      check("the undo stack is kept", (await erg(page, (e) => e.store.stack.depth)) === depth && depth > 0);
      await page.locator('[data-action="save"]').click();
      check("saving while offline is refused with a reason", /Not connected/.test(await page.locator("[data-erg-save]").textContent()));
      await page.waitForSelector("[data-erg-offline]", { state: "detached", timeout: 15000 });
      const saves = mock.state.erg.saves;
      await page.locator('[data-action="save"]').click();
      await page.waitForSelector('[data-erg-save="ok"]', { timeout: 5000 });
      check("after reconnecting, save works", mock.state.erg.saves === saves + 1 && (await page.locator("[data-erg-editor]").getAttribute("data-dirty")) === "0");
    });

    await attempt("side pane fits at 1280px", async () => {
      const narrow = await openPage(browser, mock, "", { width: 1280, height: 860 });
      try {
        await openErg(narrow.page);
        await createProject(narrow.page, "Multi.Synthetic12", "narrow", "Narrow Test");
        for (const t of ["props", "level", "checks", "terrain", "script", "export"]) {
          const loc = narrow.page.locator(`[data-tab="${t}"]`);
          if (await loc.count()) await loc.click();
        }
        const overflow = await narrow.page.evaluate(() => {
          const doc = document.documentElement;
          const tabs = document.querySelector(".erg-tabs");
          return {
            page: doc.scrollWidth <= doc.clientWidth + 1,
            tabs: !tabs || tabs.scrollWidth <= tabs.clientWidth + 1,
          };
        });
        check("at 1280px the page has no horizontal overflow", overflow.page, JSON.stringify(overflow));
        check("at 1280px the tab strip does not overflow its pane", overflow.tabs, JSON.stringify(overflow));
      } finally {
        await narrow.page.close();
        current = page;
      }
    });

    const real = errors.filter((e) => !/WebSocket connection .* 503/.test(e));
    check("no page errors", real.length === 0, real.slice(0, 5).join(" | "));
  } finally {
    await page.close();
    await mock.close();
  }
}

const browser = await chromium.launch({ channel: "msedge", headless: true, args: ["--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--ignore-gpu-blocklist"] });
try {
  await suite(browser);
} finally {
  await browser.close();
}
const failed = results.filter((r) => !r.ok).length;
console.log(`erg e2e: ${results.length - failed}/${results.length} passed`);
process.exit(failed ? 1 : 0);
