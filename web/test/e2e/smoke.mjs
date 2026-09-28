// Oasis smoke test in the installed Microsoft Edge (headless, playwright-core, no browser download).
//   node web/test/e2e/smoke.mjs <launch url> [--reconnect]
// Opens the launch URL, checks the token redirect, the handshake (welcome, proto 1), the About panel and a
// sys.ping round trip. With --reconnect it then waits for the server to drop the connection (the caller kicks
// the client) and checks that the page reconnects on its own.
import { chromium } from "playwright-core";

const url = process.argv[2] || process.env.OASIS_URL;
const wantReconnect = process.argv.includes("--reconnect");
if (!url) {
  console.error("usage: smoke.mjs <launch url> [--reconnect]");
  process.exit(2);
}

const results = [];
const check = (name, ok, detail = "") => {
  results.push({ name, ok: !!ok, detail });
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? ` ${detail}` : ""}`);
};

const browser = await chromium.launch({ channel: "msedge", headless: true });
try {
  const page = await browser.newPage();
  const frames = [];
  const requests = [];
  const errors = [];
  let closedAt = 0;
  page.on("websocket", (ws) => {
    ws.on("framereceived", (f) => {
      try { frames.push({ at: Date.now(), m: JSON.parse(f.payload) }); } catch { /* binary */ }
    });
    ws.on("close", () => { closedAt = Date.now(); });
  });
  page.on("request", (r) => requests.push({ url: r.url(), referer: r.headers()["referer"] ?? "" }));
  page.on("pageerror", (e) => errors.push(e.message));
  page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });

  const t0 = Date.now();
  await page.goto(url, { waitUntil: "load" });
  check("launch URL redirects to / without the token", !new URL(page.url()).search.includes("k="), page.url());
  await page.waitForSelector(".badge-open", { timeout: 15000 });
  check("connected", true, `${Date.now() - t0} ms from navigation`);
  const welcome = frames.map((f) => f.m).find((m) => m.t === "welcome");
  check("welcome with proto 1", welcome && welcome.proto === 1, welcome ? `build=${welcome.build} server=${welcome.server}` : "none");
  check("About panel listed", await page.locator('nav [data-panel="about"]').count() === 1);
  await page.locator('nav [data-panel="about"]').click();
  await page.waitForSelector('[data-fact="proto"]', { timeout: 5000 });
  check("About shows protocol 1", (await page.locator('[data-fact="proto"]').textContent()) === "1");
  await page.locator('[data-action="ping"]').click();
  await page.waitForFunction(() => /round trip/.test(document.querySelector('[data-result="ping"]')?.textContent ?? ""), null, { timeout: 5000 });
  check("sys.ping round trip", true, await page.locator('[data-result="ping"]').textContent());
  const leak = requests.filter((r) => /[?&]k=/.test(r.referer));
  check("no request carries the token in Referer", leak.length === 0, `${requests.length} requests`);

  if (wantReconnect) {
    const deadline = Date.now() + 30000;
    while (!closedAt && Date.now() < deadline) await page.waitForTimeout(100);
    check("server closed the connection", !!closedAt);
    if (closedAt) {
      const until = Date.now() + 15000;
      let again;
      while (!again && Date.now() < until) {
        again = frames.find((f) => f.m.t === "welcome" && f.at > closedAt);
        if (!again) await page.waitForTimeout(50);
      }
      check("page reconnected on its own", !!again, again ? `${again.at - closedAt} ms after the close` : "no second welcome");
      await page.waitForSelector(".badge-open", { timeout: 5000 });
    }
  }
  check("no page errors", errors.length === 0, errors.join(" | "));
} finally {
  await browser.close();
}
const failed = results.filter((r) => !r.ok).length;
console.log(`e2e: ${results.length - failed}/${results.length} passed`);
process.exit(failed ? 1 : 0);
