// Browser tests of the shell and the logs, events, console, mods and settings panels in the installed Microsoft
// Edge (headless), against the mock server (no game needed).
//   node web/test/e2e/panels.mjs [<built web app folder>] [--quick]
// --quick skips the 30 s log-throughput run (2000 lines/s, 60 fps, heap under 200 MB).
import { chromium } from "playwright-core";
import { startMock } from "./mock-server.mjs";

const root = process.argv.slice(2).find((a) => !a.startsWith("--")) ?? "web/dist";
const quick = process.argv.includes("--quick");
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
    const shot = `web/test/out/fail-${name.replace(/\W+/g, "_")}.png`;
    await current?.screenshot({ path: shot }).then(() => console.log(`     screenshot: ${shot}`), () => {});
  }
};

async function openPage(browser, mock) {
  const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
  const errors = [];
  page.on("pageerror", (e) => errors.push(e.message));
  page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
  current = page;
  await page.goto(mock.url, { waitUntil: "load" });
  await page.waitForSelector(".badge-open", { timeout: 15000 });
  return { page, errors };
}

const tab = (page, id) => page.locator(`nav [data-panel="${id}"]`);
const host = (page, id) => page.locator(`[data-panel-host="${id}"]`);

async function gamePanels(browser) {
  const mock = await startMock({ root });
  mock.setLogRate(20);
  const { page, errors } = await openPage(browser, mock);
  try {
    const ids = await page.locator("nav [data-panel]").evaluateAll((els) => els.map((e) => e.getAttribute("data-panel")));
    check("tabs: logs, events, console, mods, settings, about", ["logs", "events", "console", "mods", "ini", "about"].every((x) => ids.includes(x)), ids.join(","));

    await attempt("logs", async () => {
      await tab(page, "logs").click();
      await page.waitForSelector('[data-panel-host="logs"] .vt-row', { timeout: 10000 });
      check("logs: live records shown", (await host(page, "logs").locator(".vt-row").count()) > 0);
      await page.selectOption('[data-control="level"]', "3");
      await page.waitForTimeout(200);
      const lv = await host(page, "logs").locator(".vt-row .lvl").allTextContents();
      check("logs: level filter", lv.length > 0 && lv.every((x) => ["warn", "error", "fatal"].includes(x)), `${lv.length} rows`);
      await page.selectOption('[data-control="level"]', "0");
      await host(page, "logs").locator("input[type=search]").fill("boot record 7");
      await page.waitForTimeout(200);
      const msgs = await host(page, "logs").locator(".vt-row").allTextContents();
      check("logs: text filter", msgs.length >= 1 && msgs.every((m) => m.includes("boot record 7")), `${msgs.length} rows`);
      await host(page, "logs").locator(".vt-row").first().click();
      check("logs: detail shows the record", await page.locator("[data-detail] .jt").count() === 1);
      await host(page, "logs").locator("input[type=search]").fill("");
      await page.locator('[data-action="pause"]').click();
      await page.waitForTimeout(150);
      const a = await page.locator("[data-count]").textContent();
      await page.waitForTimeout(700);
      const b = await page.locator("[data-count]").textContent();
      check("logs: pause freezes the view", a === b, `${a} / ${b}`);
      await page.locator('[data-action="pause"]').click();
      await page.waitForTimeout(700);
      check("logs: resume continues", (await page.locator("[data-count]").textContent()) !== b);
      await page.locator('[data-control="source"]').focus();
      await page.waitForTimeout(300);
      const opt = await page.locator('[data-control="source"] option').nth(1).getAttribute("value");
      await page.selectOption('[data-control="source"]', opt);
      await page.waitForFunction(() => /past record/.test(document.querySelector('[data-panel-host="logs"] .vt-body')?.textContent ?? ""), null, { timeout: 5000 });
      check("logs: a past session opens from /logs/", true, (await page.locator("[data-count]").textContent()) ?? "");
      await page.selectOption('[data-control="source"]', "live");
    });

    await attempt("events", async () => {
      await tab(page, "events").click();
      await page.waitForSelector('[data-picker] [data-name="GameLogic.Turn.Started"]', { timeout: 5000 });
      check("events: names from bus.names", true);
      await page.locator('[data-picker] input[placeholder="Name or Prefix.*"]').fill("Explosion.*");
      await page.locator('[data-picker] form button[type=submit]').click();
      await page.waitForSelector("[data-watched]", { timeout: 2000 });
      await page.waitForTimeout(300);
      const t0 = Date.now();
      mock.postBus("Explosion.Probe", { radius: 50, sent: t0 });
      await page.waitForFunction(() => /Explosion\.Probe/.test(document.querySelector('[data-panel-host="events"] .vt-body')?.textContent ?? ""), null, { timeout: 3000, polling: 10 });
      const dt = Date.now() - t0;
      check("events: a watched message appears within 250 ms", dt <= 250, `${dt} ms`);
      await host(page, "events").locator(".vt-row", { hasText: "Explosion.Probe" }).first().click();
      await page.waitForSelector("[data-detail]", { timeout: 2000 });
      const detail = (await page.locator("[data-detail]").textContent()) ?? "";
      check("events: decoded payload in the JSON tree", /radius/.test(detail), detail.slice(0, 120));
      const other = await host(page, "events").locator(".vt-row", { hasText: "GameLogic" }).count();
      check("events: unwatched names are not streamed", other === 0, `${other}`);
      await page.locator('[data-tab="counts"]').click();
      await page.waitForFunction(() => document.querySelectorAll('[data-panel-host="events"] .vt-row').length > 0, null, { timeout: 4000 });
      check("events: counts view", true, `${await host(page, "events").locator(".vt-row").count()} names`);
      await page.locator('[data-tab="events"]').click();
    });

    await attempt("console", async () => {
      await tab(page, "console").click();
      await page.waitForSelector('[data-editor="lua"]', { timeout: 10000 });
      const ed = page.locator('[data-editor="lua"]');
      await ed.click();
      await page.keyboard.type("return wum.game.scene()");
      await page.keyboard.press("Enter");
      await page.waitForSelector('[data-entry="ok"]', { timeout: 5000 });
      check("console: client eval returns \"match\"", /match/.test((await page.locator('[data-entry="ok"] .entry-text').last().textContent()) ?? ""));
      await page.keyboard.type("x @@ y");
      await page.keyboard.press("Enter");
      await page.waitForSelector('[data-entry="error"]', { timeout: 5000 });
      check("console: a syntax error comes back as an error", /unexpected symbol/.test((await page.locator('[data-entry="error"]').last().textContent()) ?? ""));
      await page.selectOption('[data-control="target"]', "match");
      await ed.click();
      await page.keyboard.type("return GetData ~= nil");
      await page.keyboard.press("Enter");
      await page.waitForFunction(() => [...document.querySelectorAll('[data-entry="ok"]')].some((e) => /match>/.test(e.textContent ?? "") && /true/.test(e.textContent ?? "")), null, { timeout: 5000 });
      check("console: match eval returns true", true);
      const sent = mock.state.calls.filter((c) => c.m === "lua.eval").map((c) => c.p.target);
      check("console: target switch is sent", sent.includes("client") && sent.includes("match"), sent.join(","));
      await ed.click();
      await page.keyboard.type("wum.ga");
      await page.waitForSelector(".cm-tooltip-autocomplete", { timeout: 3000 });
      check("console: completion from lua.complete", /game/.test((await page.locator(".cm-tooltip-autocomplete").textContent()) ?? ""));
      await page.keyboard.press("Tab");
      await page.waitForTimeout(100);
      check("console: completion accepted", ((await ed.textContent()) ?? "").includes("wum.game"), await ed.textContent());
      await page.keyboard.press("Control+a");
      await page.keyboard.press("Backspace");
      await page.keyboard.press("ArrowUp");
      check("console: Up recalls the last input", (await ed.textContent()) === "return GetData ~= nil", await ed.textContent());
      await page.keyboard.press("ArrowDown");
      check("console: Down returns to the draft", (await page.locator('[data-editor="lua"] .cm-placeholder').count()) === 1, await ed.textContent());
      await page.selectOption('[data-control="target"]', "client");
    });

    await attempt("mods", async () => {
      await tab(page, "mods").click();
      await page.waitForSelector("[data-mod]", { timeout: 5000 });
      check("mods: list", (await page.locator("[data-mod]").count()) === 4);
      await page.locator('[data-toggle="hello-spice"]').click();
      await page.waitForSelector('[data-mod="hello-spice"] [data-state="disabled"]', { timeout: 3000 });
      check("mods: disable a client mod", mock.state.mods.find((m) => m.id === "hello-spice").on === false);
      await page.locator('[data-toggle="big-crates"]').click();
      await page.waitForSelector('[data-mod="big-crates"] [data-state="restart-required"]', { timeout: 3000 });
      check("mods: a content mod shows restart required", await page.locator("[data-restart]").count() === 1);
      check("mods: granted Deep Desert shows Revoke", await page.locator('[data-revoke="memwatch"]').count() === 1);
      check("mods: an ungranted Deep Desert mod has no grant control", await page.locator('[data-mod="rawpeek"] button').count() === 0);
      await page.locator('[data-revoke="memwatch"]').click();
      await page.waitForSelector('[data-mod="memwatch"] [data-dd="grant-in-game"]', { timeout: 3000 });
      check("mods: revoke Deep Desert", mock.state.mods.find((m) => m.id === "memwatch").deepDesert.granted === false);
      check("mods: no Grant button anywhere", (await page.locator("button", { hasText: /grant/i }).count()) === 0);
      const methods = new Set(mock.state.calls.map((c) => c.m));
      check("mods: only list, setEnabled and revoke were called", [...methods].filter((m) => m.startsWith("mods.")).every((m) => ["mods.list", "mods.setEnabled", "mods.revokeDeepDesert"].includes(m)));
    });

    await attempt("settings", async () => {
      await tab(page, "ini").click();
      await page.waitForSelector('[data-section="Oasis"]', { timeout: 5000 });
      const before = mock.state.ini;
      await page.locator('[data-edit="Oasis.MaxClients"]').click();
      await page.locator('[data-key="Oasis.MaxClients"] input').fill("6");
      await page.locator('[data-key="Oasis.MaxClients"] button[type=submit]').click();
      await page.waitForSelector('[data-key="Oasis.MaxClients"] [data-mark="restart"]', { timeout: 3000 });
      const want = before.replace("MaxClients=4 ; browser tabs", "MaxClients=6 ; browser tabs");
      check("settings: one value changed, comments kept", mock.state.ini === want);
      check("settings: restart mark", (await page.locator("[data-restart]").count()) === 1);
      await page.locator('[data-edit="FrameInterval.IntervalMs"]').click();
      await page.locator('[data-key="FrameInterval.IntervalMs"] input').fill("8");
      await page.locator('[data-key="FrameInterval.IntervalMs"] button[type=submit]').click();
      await page.waitForSelector('[data-key="FrameInterval.IntervalMs"] [data-mark="live"]', { timeout: 3000 });
      check("settings: a live key applies without restart", true);
      check("settings: the grant salt cannot be edited", await page.locator('[data-edit="Thumper.GrantSalt"]').isDisabled());
      await page.locator('[data-edit="Thumper.AutoGrantDeepDesert"]').click();
      await page.selectOption('[data-key="Thumper.AutoGrantDeepDesert"] select', "1");
      await page.locator('[data-key="Thumper.AutoGrantDeepDesert"] button[type=submit]').click();
      await page.waitForSelector("[data-edit-error]", { timeout: 3000 });
      check("settings: Deep Desert auto-grant refused", /AutoGrantDeepDesert=0/.test(mock.state.ini) && !mock.state.calls.some((c) => c.m === "ini.set" && /grant/i.test(c.p.key) && c.p.value !== "0"));
      await page.locator('[data-key="Thumper.AutoGrantDeepDesert"] button', { hasText: "Cancel" }).click();
      await page.locator('[data-action="raw"]').click();
      const raw = (await page.locator("[data-raw]").textContent()) ?? "";
      check("settings: raw text with the salt masked", raw.includes("GrantSalt=********") && !raw.includes("0123456789abcdef"));
      await page.locator('[data-action="raw"]').click();
    });

    await attempt("shell", async () => {
      await page.keyboard.press("Control+k");
      await page.waitForSelector(".palette-input", { timeout: 2000 });
      await page.keyboard.type("mods beside");
      await page.keyboard.press("Enter");
      await page.waitForSelector('[data-frame="mods"]', { timeout: 3000 });
      check("shell: palette opens a panel beside", (await page.locator("[data-frame]").count()) === 2);
      await page.locator('[data-action="theme"]').click();
      await page.waitForFunction(() => document.documentElement.dataset.theme === "light", null, { timeout: 2000 });
      check("shell: theme switch", true);
      await page.reload({ waitUntil: "load" });
      await page.waitForSelector(".badge-open", { timeout: 10000 });
      await page.waitForSelector('[data-frame="mods"]', { timeout: 5000 });
      check("shell: layout and theme remembered", (await page.locator("[data-frame]").count()) === 2 &&
        (await page.evaluate(() => document.documentElement.dataset.theme)) === "light");
      await page.locator('[data-close="mods"]').click();
      check("shell: close the side panel", (await page.locator("[data-frame]").count()) === 1);
      await page.locator('[data-action="theme"]').click();
      await page.locator('[data-action="theme"]').click();
      mock.kick();
      await page.waitForSelector(".badge-closed, .badge-connecting", { timeout: 3000 });
      await page.waitForSelector(".badge-open", { timeout: 8000 });
      check("shell: reconnects after a kick", true);
    });
    check("game: no page errors", errors.length === 0, errors.slice(0, 3).join(" | "));
  } finally {
    await page.close();
    await mock.close();
  }
}

async function throughput(browser) {
  const mock = await startMock({ root });
  const { page, errors } = await openPage(browser, mock);
  try {
    await tab(page, "logs").click();
    await page.waitForSelector('[data-panel-host="logs"] .vt-row', { timeout: 10000 });
    const cdp = await page.context().newCDPSession(page);
    await cdp.send("Performance.enable");
    mock.setLogRate(2000);
    await page.waitForTimeout(24000);
    await page.evaluate(() => {
      const w = window;
      w.__frames = [];
      const loop = (t) => { w.__frames.push(t); if (w.__frames.length < 100000) requestAnimationFrame(loop); };
      requestAnimationFrame(loop);
    });
    await page.waitForTimeout(6000);
    const frames = await page.evaluate(() => window.__frames);
    const secs = (frames[frames.length - 1] - frames[0]) / 1000;
    const fps = (frames.length - 1) / secs;
    const gaps = frames.slice(1).map((t, i) => t - frames[i]).sort((a, b) => a - b);
    const p99 = gaps[Math.floor(gaps.length * 0.99)];
    await cdp.send("HeapProfiler.collectGarbage");
    const { metrics } = await cdp.send("Performance.getMetrics");
    const heap = metrics.find((m) => m.name === "JSHeapUsedSize")?.value ?? 0;
    const shown = (await page.locator("[data-count]").textContent()) ?? "";
    mock.setLogRate(0);
    check("throughput: 2000 lines/s keeps ~60 fps", fps >= 55, `${fps.toFixed(1)} fps, p99 frame ${p99.toFixed(1)} ms`);
    check("throughput: heap under 200 MB at the 50 000-line cap", heap < 200 * 1024 * 1024 && /of 50,000/.test(shown), `${(heap / 1048576).toFixed(1)} MB, ${shown}`);
    check("throughput: no page errors", errors.length === 0, errors.slice(0, 3).join(" | "));
  } finally {
    await page.close();
    await mock.close();
  }
}

async function policyModes(browser) {
  const online = await startMock({ root, online: true });
  let { page, errors } = await openPage(browser, online);
  try {
    await tab(page, "console").click();
    await page.waitForSelector('[data-editor="lua"]', { timeout: 10000 });
    await page.selectOption('[data-control="target"]', "match");
    await page.locator('[data-editor="lua"]').click();
    await page.keyboard.type("return 1");
    await page.keyboard.press("Enter");
    await page.waitForSelector('[data-entry="refused"]', { timeout: 5000 });
    check("online: match eval refused with the console's reason", /online matches/.test((await page.locator('[data-entry="refused"]').textContent()) ?? ""));
    await page.selectOption('[data-control="target"]', "client");
    check("online: no page errors", errors.length === 0, errors.join(" | "));
  } finally {
    await page.close();
    await online.close();
  }

  const ro = await startMock({ root, readOnly: true });
  ({ page, errors } = await openPage(browser, ro));
  try {
    await tab(page, "mods").click();
    await page.waitForSelector("[data-mod]", { timeout: 5000 });
    check("read-only: mod toggles disabled", await page.locator('[data-toggle="hello-spice"]').isDisabled());
    check("read-only: revoke disabled", await page.locator('[data-revoke="memwatch"]').isDisabled());
    await tab(page, "ini").click();
    await page.waitForSelector('[data-section="Oasis"]', { timeout: 5000 });
    check("read-only: settings not editable", await page.locator('[data-edit="Oasis.Port"]').isDisabled());
    await tab(page, "console").click();
    await page.waitForSelector('[data-action="run"]', { timeout: 10000 });
    check("read-only: console cannot run", await page.locator('[data-action="run"]').isDisabled());
    check("read-only: no page errors", errors.length === 0, errors.join(" | "));
  } finally {
    await page.close();
    await ro.close();
  }

  const sa = await startMock({ root, standalone: true });
  ({ page, errors } = await openPage(browser, sa));
  try {
    check("standalone: events and console greyed out", await tab(page, "events").isDisabled() && await tab(page, "console").isDisabled());
    await page.goto(sa.url.replace(/\/\?k=.*$/, "/#/console"), { waitUntil: "load" });
    await page.waitForSelector(".badge-open", { timeout: 10000 });
    await page.waitForSelector('[data-unavailable="console"]', { timeout: 5000 });
    check("standalone: a game panel says the game is not running", /Game not running/.test((await page.locator('[data-unavailable="console"]').textContent()) ?? ""));
    for (const id of ["logs", "mods", "ini", "about"]) {
      await tab(page, id).click();
      await page.waitForSelector(`[data-panel-host="${id}"] *`, { timeout: 5000 });
    }
    await page.waitForTimeout(300);
    check("standalone: logs, mods, settings and about open", true);
    check("standalone: no page errors", errors.length === 0, errors.join(" | "));
  } finally {
    await page.close();
    await sa.close();
  }
}

const browser = await chromium.launch({ channel: "msedge", headless: true });
try {
  await gamePanels(browser);
  await policyModes(browser);
  if (!quick) await throughput(browser);
} finally {
  await browser.close();
}
const failed = results.filter((r) => !r.ok).length;
console.log(`panels e2e: ${results.length - failed}/${results.length} passed`);
process.exit(failed ? 1 : 0);
