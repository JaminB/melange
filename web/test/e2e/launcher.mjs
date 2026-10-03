// Headless Edge e2e for the Melange.exe launcher's web UI, against the mock server (--launcher) and its scenario
// fixtures (web/test/e2e/launcher-mock.mjs): fresh (found/install/recommended), not-found, wrong-build, ual-present,
// reshade (other dinput8 + backup + restore). Screenshots of every wizard step and main-app section, light and dark,
// go into --shots (default web/test/out/launcher-shots).
//   node web/test/e2e/launcher.mjs [<built web app folder>] [--shots <dir>]
import { mkdirSync } from "node:fs";
import { chromium } from "playwright-core";
import { startMock } from "./mock-server.mjs";

const args = process.argv.slice(2);
const root = args.find((a) => !a.startsWith("--")) ?? "web/dist";
const shotsArg = args.indexOf("--shots");
const shotsDir = shotsArg >= 0 ? args[shotsArg + 1] : "web/test/out/launcher-shots";
mkdirSync(shotsDir, { recursive: true });

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
    const path = `${shotsDir}/fail-${name.replace(/\W+/g, "_")}.png`;
    await current?.screenshot({ path }).then(() => console.log(`     screenshot: ${path}`), () => {});
  }
};

async function shot(page, name) {
  for (const theme of ["light", "dark"]) {
    await page.evaluate((t) => { document.documentElement.dataset.theme = t; }, theme);
    await page.waitForTimeout(30);
    await page.screenshot({ path: `${shotsDir}/${name}-${theme}.png` });
  }
  await page.evaluate(() => { delete document.documentElement.dataset.theme; });
}

async function openPage(browser, mock, scenario) {
  const page = await browser.newPage({ viewport: { width: 1120, height: 740 } });
  const errors = [];
  page.on("pageerror", (e) => errors.push(e.message));
  page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
  current = page;
  await page.goto(`${mock.url}&scenario=${scenario}`, { waitUntil: "load" });
  await page.waitForSelector("[data-step], .la", { timeout: 15000 });
  return { page, errors };
}

const h1Text = (page) => page.locator("main h1, .lw-card h1").first().textContent();

async function foundInstallRecommended(browser) {
  const mock = await startMock({ root, launcher: true });
  const { page, errors } = await openPage(browser, mock, "fresh");
  try {
    await attempt("welcome", async () => {
      check("welcome: shows the brand and the intro", /Welcome to Melange/.test(await h1Text(page)));
      check("welcome: the page/window title says Melange, not Oasis", await page.title() === "Melange");
      await shot(page, "01-welcome");
      await page.locator('[data-action="get-started"]').click();
    });

    await attempt("find: found", async () => {
      await page.waitForSelector("[data-candidate]", { timeout: 10000 });
      check("find: one candidate, preselected and verdict Ready", await page.locator('[data-candidate][aria-checked="true"] .verdict-ok').count() === 1);
      await shot(page, "02-find-found");
      await page.locator('[data-action="continue"]').click();
    });

    await attempt("check: ok", async () => {
      await page.waitForSelector('[data-step="check"][data-verdict="ok"]', { timeout: 10000 });
      check("check: ready copy", /ready for Melange/.test(await h1Text(page)));
      await shot(page, "03-check-ok");
      await page.locator('[data-action="primary"]').click();
    });

    await attempt("install: fresh, no loader yet", async () => {
      await page.waitForSelector("[data-checklist]", { timeout: 10000 });
      const loaderLine = (await page.locator('[data-item="loader"]').textContent()) ?? "";
      check("install: offers to add Ultimate ASI Loader", /Add Ultimate ASI Loader/.test(loaderLine), loaderLine);
      const footBefore = await page.locator(".lw-foot-right").boundingBox();
      await shot(page, "04-install-plan");
      await page.locator('[data-action="install"]').click();
      await page.waitForSelector('[data-step="recommended"]', { timeout: 10000 });
      const footAfter = await page.locator(".lw-foot").boundingBox();
      check("install: the footer area does not jump between skeleton and loaded state", !!footBefore && !!footAfter);
    });

    await attempt("recommended: sunstone defaults to on, Bold", async () => {
      check("recommended: sunstone pre-selected", await page.locator('[data-plugin="sunstone"] input[type="checkbox"]').isChecked());
      check("recommended: Bold is the default quality", await page.locator('[data-plugin="sunstone"] [data-option="bold"]').getAttribute("aria-pressed") === "true");
      await shot(page, "05-recommended");
      await page.locator('[data-action="install-selected"]').click();
      await page.waitForSelector('[data-step="ready"]', { timeout: 10000 });
    });

    await attempt("ready: finishes into the main app", async () => {
      check("ready: names the install path", /installed in/.test((await page.locator("main").textContent()) ?? ""));
      await shot(page, "06-ready");
      await page.locator('[data-action="open-melange"]').click();
      await page.waitForSelector(".la", { timeout: 10000 });
      check("home: Melange now shows installed", /0\.4\.0/.test((await page.locator('[data-card="melange"]').textContent()) ?? ""));
      await shot(page, "07-home");
    });

    await attempt("plugins and settings drawer", async () => {
      await page.locator('[data-page-tab="plugins"]').click();
      await page.waitForSelector("[data-plugin]", { timeout: 10000 });
      await shot(page, "08-plugins");
      await page.locator("[data-settings]").first().click();
      await page.waitForSelector('[role="dialog"]', { timeout: 5000 });
      await shot(page, "09-plugin-settings");
      await page.locator('.lx-drawer button:has-text("Close")').click();
    });

    await attempt("settings page", async () => {
      await page.locator('[data-page-tab="settings"]').click();
      await page.waitForSelector('[data-section="melange"]', { timeout: 10000 });
      await shot(page, "10-settings");
    });

    check("fresh scenario: no page errors", errors.length === 0, errors.join(" | "));
  } finally {
    await page.close();
    await mock.close();
  }
}

async function notFound(browser) {
  const mock = await startMock({ root, launcher: true });
  const { page, errors } = await openPage(browser, mock, "not-found");
  try {
    await page.locator('[data-action="get-started"]').click();
    await attempt("find: not found, then browse finds it", async () => {
      await page.waitForSelector('[data-step="find"]:not([data-loading])', { timeout: 10000 });
      check("find: not-found copy", /couldn't find/.test(await h1Text(page)));
      await shot(page, "11-find-not-found");
      await page.locator('[data-action="browse"]').click();
      await page.waitForSelector("[data-candidate]", { timeout: 10000 });
      check("find: browse recovers a candidate", (await page.locator("[data-candidate]").count()) === 1);
    });
    check("not-found scenario: no page errors", errors.length === 0, errors.join(" | "));
  } finally {
    await page.close();
    await mock.close();
  }
}

async function wrongBuild(browser) {
  const mock = await startMock({ root, launcher: true });
  const { page, errors } = await openPage(browser, mock, "wrong-build");
  try {
    await page.locator('[data-action="get-started"]').click();
    await page.waitForSelector("[data-candidate]", { timeout: 10000 });
    await attempt("check: wrong build is refused with an explanation", async () => {
      check("find: wrong-build chip", (await page.locator(".verdict-bad").count()) === 1);
      await page.locator('[data-action="continue"]').click();
      await page.waitForSelector('[data-step="check"][data-verdict="wrongBuild"]', { timeout: 10000 });
      check("check: names build #1077", /#1077/.test(await page.locator("main").textContent()));
      const details = await page.locator("[data-details]").textContent();
      check("check: shows the expected hash", /041c8c/.test(details));
      check("check: shows the found file's size and hash", /Found.*5,820,224 bytes.*SHA-256/s.test(details), details);
      await shot(page, "12-check-wrong-build");
      await page.locator('[data-action="primary"]').click();
      await page.waitForTimeout(400);
      check("check: Check again re-validates and stays refused", await page.locator('[data-step="check"][data-verdict="wrongBuild"]').count() === 1);
    });
    check("wrong-build scenario: no page errors", errors.length === 0, errors.join(" | "));
  } finally {
    await page.close();
    await mock.close();
  }
}

async function ualPresent(browser) {
  const mock = await startMock({ root, launcher: true });
  const { page, errors } = await openPage(browser, mock, "ual-present");
  try {
    await page.locator('[data-action="get-started"]').click();
    await page.waitForSelector("[data-candidate]", { timeout: 10000 });
    await page.locator('[data-action="continue"]').click();
    await page.waitForSelector('[data-step="check"][data-verdict="ok"]', { timeout: 10000 });
    await page.locator('[data-action="primary"]').click();
    await attempt("install: an existing Ultimate ASI Loader is reused", async () => {
      await page.waitForSelector("[data-checklist]", { timeout: 10000 });
      const loaderLine = (await page.locator('[data-item="loader"]').textContent()) ?? "";
      check("install: reuses UAL 9.7.4", /already installed.*9\.7\.4/.test(loaderLine), loaderLine);
      check("install: no loader choice needed", (await page.locator("[data-needs-choice]").count()) === 0);
      await shot(page, "13-install-ual-present");
    });
    check("ual-present scenario: no page errors", errors.length === 0, errors.join(" | "));
  } finally {
    await page.close();
    await mock.close();
  }
}

async function reshadeBackupAndRestore(browser) {
  const mock = await startMock({ root, launcher: true });
  const { page, errors } = await openPage(browser, mock, "reshade");
  try {
    await page.locator('[data-action="get-started"]').click();
    await page.waitForSelector("[data-candidate]", { timeout: 10000 });
    await page.locator('[data-action="continue"]').click();
    await page.waitForSelector('[data-step="check"][data-verdict="ok"]', { timeout: 10000 });
    await page.locator('[data-action="primary"]').click();
    await attempt("install: another program's dinput8.dll needs a choice", async () => {
      await page.waitForSelector('[data-needs-choice="loader"]', { timeout: 10000 });
      check("install: names ReShade", /ReShade/.test(await page.locator('[data-needs-choice="loader"]').textContent()));
      check("install button disabled until a choice is made", await page.locator('[data-action="install"]').isDisabled());
      check("install: the undecided loader item is not ticked", await page.locator('[data-item="loader"].undecided').count() === 1);
      await shot(page, "14-install-needs-choice");
      await page.locator('input[name="loader-choice"]').first().click();
      await page.waitForSelector('[data-action="install"]:not([disabled])', { timeout: 5000 });
      check("install: choosing clears the undecided state", await page.locator('[data-item="loader"].undecided').count() === 0);
      await shot(page, "15-install-replace-chosen");
      await page.locator('[data-action="install"]').click();
      await page.waitForSelector('[data-step="recommended"]', { timeout: 10000 });
    });
    await attempt("reshade: skip to ready and open Melange", async () => {
      await page.locator('button:has-text("Skip")').click();
      await page.waitForSelector('[data-step="ready"]', { timeout: 10000 });
      await page.locator('[data-action="open-melange"]').click();
      await page.waitForSelector(".la", { timeout: 10000 });
    });
    await attempt("home: offers to restore the replaced dinput8.dll", async () => {
      await page.waitForSelector('[data-notice="restore"]', { timeout: 10000 });
      await shot(page, "16-home-restore-notice");
      await page.locator('[data-notice="restore"] button:has-text("Restore it")').click();
      await page.waitForSelector('[data-notice="restore"]', { state: "detached", timeout: 5000 });
      check("home: the notice is dismissed after restoring", true);
    });
    check("reshade scenario: no page errors", errors.length === 0, errors.join(" | "));
  } finally {
    await page.close();
    await mock.close();
  }
}

async function installedHome(browser) {
  const mock = await startMock({ root, launcher: true });
  const { page, errors } = await openPage(browser, mock, "restore");
  try {
    await attempt("installed-home: skips the wizard and offers Restore from Settings", async () => {
      await page.waitForSelector(".la", { timeout: 10000 });
      check("home: no wizard shown when firstRun is false", (await page.locator(".lw").count()) === 0);
      await shot(page, "17-home-installed");
      await page.locator('[data-page-tab="settings"]').click();
      await page.waitForSelector("[data-backup]", { timeout: 10000 });
      await shot(page, "18-settings-backups");
      await page.locator('[data-backup] button:has-text("Restore")').click();
      await page.waitForTimeout(200);
      check("settings: restore ran without an error", (await page.locator(".error").count()) === 0);
    });
    check("restore scenario: no page errors", errors.length === 0, errors.join(" | "));
  } finally {
    await page.close();
    await mock.close();
  }
}

async function pluginsBusy(browser) {
  const mock = await startMock({ root, launcher: true });
  const { page, errors } = await openPage(browser, mock, "plugins-busy");
  try {
    await attempt("home: a running recommended batch shows a calm notice and disables Restore", async () => {
      await page.waitForSelector(".la", { timeout: 10000 });
      await page.waitForSelector('[data-notice="busy"]', { timeout: 10000 });
      const busyText = (await page.locator('[data-notice="busy"]').textContent()) ?? "";
      check("home: busy notice names the plugin and progress", /Sunstone/.test(busyText) && /1 of 2/.test(busyText), busyText);
      const restoreBtn = page.locator('[data-notice="restore"] button:has-text("Restore it")');
      check("home: Restore it is disabled while busy", await restoreBtn.isDisabled());
      check("home: Restore it explains why in a tooltip", !!(await restoreBtn.getAttribute("title")));
      await shot(page, "19-home-plugins-busy");
    });
    await attempt("settings: setup actions are disabled with a tooltip while busy", async () => {
      await page.locator('[data-page-tab="settings"]').click();
      await page.waitForSelector('[data-section="melange"]', { timeout: 10000 });
      await page.waitForSelector("[data-busy]", { timeout: 10000 });
      const repair = page.locator('[data-section="melange"] button:has-text("Repair")');
      check("settings: Repair is disabled while busy", await repair.isDisabled());
      check("settings: Repair explains why in a tooltip", !!(await repair.getAttribute("title")));
      const backupRestore = page.locator('[data-backup] button:has-text("Restore")');
      check("settings: backup Restore is disabled while busy", await backupRestore.isDisabled());
      await shot(page, "20-settings-busy");
    });
    await attempt("plugins: the settings drawer is read-only while busy", async () => {
      await page.locator('[data-page-tab="plugins"]').click();
      await page.waitForSelector("[data-busy]", { timeout: 10000 });
      await page.locator("[data-settings]").first().click();
      await page.waitForSelector('[role="dialog"]', { timeout: 5000 });
      check("plugins: setting control is disabled while busy", await page.locator('[data-setting="quality"] [data-option="bold"]').isDisabled());
      await shot(page, "21-plugins-busy");
      await page.locator('.lx-drawer button:has-text("Close")').click();
    });
    check("plugins-busy scenario: no page errors", errors.length === 0, errors.join(" | "));
  } finally {
    await page.close();
    await mock.close();
  }
}

const browser = await chromium.launch({ channel: "msedge", headless: true });
try {
  for (const scenario of [foundInstallRecommended, notFound, wrongBuild, ualPresent, reshadeBackupAndRestore, installedHome, pluginsBusy]) {
    try {
      await scenario(browser);
    } catch (e) {
      check(scenario.name, false, String(e?.message ?? e).split("\n")[0]);
    }
  }
} finally {
  await browser.close();
}
const failed = results.filter((r) => !r.ok).length;
console.log(`launcher e2e: ${results.length - failed}/${results.length} passed, screenshots in ${shotsDir}`);
process.exit(failed ? 1 : 0);
