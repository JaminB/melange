// The Game state panel in the installed Microsoft Edge (headless, playwright-core).
//   node web/test/e2e/gamestate.mjs [<built web dir>]   against a mock game server with fixed data (default dir:
//                                                        build/x86-release/web-dist)
//   node web/test/e2e/gamestate.mjs --url <launch url>   against a running game in a match (structure checks only)
//   node web/test/e2e/gamestate.mjs [<dir>] --serve      only serve the mock, for trying the panel by hand
// The mock speaks protocol v1 (hello/welcome, sub/ev, call/res/err) for the state and entities channels and the
// state.*, entities.list and entity.inspect methods.
import { createHash } from "node:crypto";
import { readFileSync, existsSync } from "node:fs";
import { createServer } from "node:http";
import { extname, join, normalize, resolve } from "node:path";
import { chromium } from "playwright-core";

const argv = process.argv.slice(2);
const urlArg = argv.includes("--url") ? argv[argv.indexOf("--url") + 1] : undefined;
const dist = resolve(argv.find((a) => !a.startsWith("--") && a !== urlArg) ?? "build/x86-release/web-dist");

const results = [];
const check = (name, ok, detail = "") => {
  results.push({ name, ok: !!ok });
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? ` ${detail}` : ""}`);
};

// ---------------------------------------------------------------- mock game
const v3 = (x, y, z) => ({ x, y, z });
function snapshot(frame) {
  const t = frame / 30;
  return {
    available: true, frame, matchSerial: 2,
    match: { inMatch: true, online: false, currentTeam: 1, activeWorm: 2, turnMs: 45000, turnMsLeft: 30000 - (frame % 300) * 10,
      roundMs: 1800000, roundMsLeft: 1500000, windSpeed: 4.2e-5, windDir: 0.5, waterLevel: -40, turnsStarted: 3,
      suddenDeath: false, theme: "ARABIAN" },
    teams: [
      { slot: 0, name: "Atreides", active: true, ai: false, local: true, colour: 0, alliance: 0, roundsWon: 0, score: 120 },
      { slot: 1, name: "Harkonnen", active: true, ai: true, local: false, colour: 1, alliance: 1, roundsWon: 1, score: 80 },
    ],
    worms: [
      { slot: 0, team: 0, posInTeam: 0, name: "Paul", active: true, alive: true, health: 100, physicsState: 6, weapon: 1, pos: v3(-300, 20, 40), vel: v3(0, 0, 0) },
      { slot: 1, team: 0, posInTeam: 1, name: "Chani", active: true, alive: true, health: 54, physicsState: 6, weapon: -1, pos: v3(-120, 60, -30), vel: v3(0, 0, 0) },
      { slot: 2, team: 1, posInTeam: 0, name: "Feyd", active: true, alive: true, health: 87, physicsState: 0, weapon: 13, pos: v3(150 + Math.sin(t) * 5, 10, 10), vel: v3(1, 0, 0) },
      { slot: 3, team: 1, posInTeam: 1, name: "Rabban", active: false, alive: false, health: 0, physicsState: 8, weapon: -1, pos: v3(260, -45, 0), vel: v3(0, -1, 0) },
    ],
  };
}
function entities(frame) {
  const s = snapshot(frame);
  return [
    ...s.worms.filter((w) => w.active).map((w, i) => ({ handle: 0x1000 * (i + 1) + 10 + i, object: 0x2a000000 + i * 0x200, vtable: 0x85ecdc,
      kind: "Worm", type: "WXWormLogicEntity", label: w.name, pos: w.pos, vel: w.vel })),
    { handle: 0x5021, object: 0x2b000100, vtable: 0x85b504, kind: "Projectile", type: "ParabolicPayloadLogicEntity", label: "Bazooka",
      pos: v3(-200 + (frame % 100), 80, 10), vel: v3(3, 1, 0) },
    { handle: 0x1022, object: 0x2b000400, vtable: 0x8619a0, kind: "Crate", type: "CrateLogicEntity", label: "", pos: v3(0, 120, 0), vel: v3(0, 0, 0) },
    { handle: 0x1023, object: 0x2b000500, vtable: 0x8627ec, kind: "Barrel", type: "OilDrumLogicEntity", label: "", pos: v3(90, 5, -20), vel: v3(0, 0, 0) },
    { handle: 0x1001, object: 0x2b000600, vtable: 0x851fc4, kind: "Other", type: "TimerLogicEntity", label: "", pos: null, vel: null },
  ];
}
const VARS = [
  { name: "ActiveWormIndex", type: "Int", value: 2 },
  { name: "Wind.Speed", type: "Float", value: 4.2e-5 },
  { name: "Wind.Direction", type: "Float", value: 0.5 },
  { name: "Land.Theme", type: "String", value: "ARABIAN" },
  { name: "Land.MinBounds", type: "Vector", value: [-400, -60, -100] },
  { name: "Worm.Data02", type: "Container", value: { addr: 0x2c001000, class: "WormDataContainer" } },
];
function hexOf(addr, len) {
  let s = "";
  for (let i = 0; i < len; i++) {
    if (i >= 48 && i < 52) { s += "??"; continue; }
    const b = i < 4 ? [0x04, 0xb5, 0x85, 0x00][i] : (addr + i * 7) & 0xff;
    s += b.toString(16).padStart(2, "0");
  }
  return s;
}

function wsFrame(text) {
  const body = Buffer.from(text);
  const n = body.length;
  const head = n < 126 ? Buffer.from([0x81, n]) : n < 65536 ? Buffer.from([0x81, 126, n >> 8, n & 255]) :
    Buffer.concat([Buffer.from([0x81, 127, 0, 0, 0, 0]), Buffer.from([(n >>> 24) & 255, (n >>> 16) & 255, (n >>> 8) & 255, n & 255])]);
  return Buffer.concat([head, body]);
}

function startMock(root) {
  const build = existsSync(join(root, "build.txt")) ? readFileSync(join(root, "build.txt"), "utf8").trim() : "dev";
  const types = { ".html": "text/html", ".js": "text/javascript", ".css": "text/css", ".svg": "image/svg+xml", ".txt": "text/plain" };
  const server = createServer((rq, res) => {
    const path = rq.url.split("?")[0];
    const rel = path === "/" ? "index.html" : normalize(path.slice(1));
    const file = join(root, rel);
    if (rel.startsWith("..") || !existsSync(file)) { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { "Content-Type": types[extname(file)] ?? "application/octet-stream" });
    res.end(readFileSync(file));
  });
  const stats = { vars: 0, inspect: [], subs: {} };
  server.on("upgrade", (rq, sock) => {
    const accept = createHash("sha1").update(rq.headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest("base64");
    sock.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`);
    let buf = Buffer.alloc(0), frame = 0, lastVars = 0;
    const timers = new Map();
    const send = (m) => { if (!sock.destroyed) sock.write(wsFrame(JSON.stringify(m))); };
    const seq = {};
    const push = (ch, d) => send({ t: "ev", ch, seq: (seq[ch] = (seq[ch] ?? 0) + 1), d });
    const tick = setInterval(() => frame++, 16);
    const onMsg = (m) => {
      if (m.t === "hello") return send({ t: "welcome", proto: 1, build, server: "game", game: { exeBuild: 1077, melange: "mock" },
        channels: ["state", "entities"], methods: ["sys.ping", "state.get", "state.vars", "entities.list", "entity.inspect"], panels: [],
        limits: { maxClients: 4, maxMessageKB: 1024, maxQueueKB: 2048, maxQueuedCalls: 16, readOnly: false } });
      if (m.t === "sub" || m.t === "unsub") {
        clearInterval(timers.get(m.ch));
        timers.delete(m.ch);
        if (m.t === "sub") {
          const hz = Math.max(1, Math.min(m.ch === "state" ? 10 : 5, m.filter?.hz ?? (m.ch === "state" ? 5 : 2)));
          stats.subs[m.ch] = hz;
          const fire = () => push(m.ch, m.ch === "state" ? snapshot(frame) : entities(frame));
          fire();
          timers.set(m.ch, setInterval(fire, 1000 / hz));
        }
        if (m.id !== undefined) send({ t: "res", id: m.id, r: true });
        return;
      }
      if (m.t !== "call") return;
      const ok = (r) => send({ t: "res", id: m.id, r });
      const err = (code, msg) => send({ t: "err", id: m.id, code, msg });
      const p = m.p ?? {};
      switch (m.m) {
        case "sys.ping": return ok({ frame, ms: Date.now() });
        case "state.get": return ok(snapshot(frame));
        case "state.vars":
          if (Date.now() - lastVars < 1000) return err(-32002, "state.vars: at most one call per second");
          lastVars = Date.now();
          stats.vars++;
          return ok(VARS.filter((v) => !p.prefix || v.name.startsWith(p.prefix)));
        case "entities.list": return ok(entities(frame).filter((e) => !p.kinds || p.kinds.includes(e.kind)));
        case "entity.inspect": {
          stats.inspect.push(p);
          const len = p.len ?? 256;
          if (p.handle !== undefined) {
            const e = entities(frame).find((x) => x.handle === p.handle);
            if (!e) return err(-32602, "no live entity with that handle");
            return ok({ handle: e.handle, addr: e.object, len, vtable: e.vtable, type: e.type, kind: e.kind,
              fields: { pos: e.pos, vel: e.vel, label: e.label, ...(e.kind === "Projectile" ? { fuse: -1 } : {}) }, hex: hexOf(e.object, len) });
          }
          return ok({ addr: p.addr, len, vtable: 0x8747d4, type: "WormDataContainer", fields: {}, hex: hexOf(p.addr, len) });
        }
        default: return err(-32601, "unknown method");
      }
    };
    sock.on("data", (chunk) => {
      buf = Buffer.concat([buf, chunk]);
      for (;;) {
        if (buf.length < 2) return;
        const op = buf[0] & 15;
        let len = buf[1] & 127, off = 2;
        if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); off = 4; }
        else if (len === 127) { if (buf.length < 10) return; len = Number(buf.readBigUInt64BE(2)); off = 10; }
        if (buf.length < off + 4 + len) return;
        const mask = buf.subarray(off, off + 4);
        const data = Buffer.from(buf.subarray(off + 4, off + 4 + len));
        for (let i = 0; i < data.length; i++) data[i] ^= mask[i & 3];
        buf = buf.subarray(off + 4 + len);
        if (op === 8) { sock.end(); return; }
        if (op === 1) { try { onMsg(JSON.parse(data.toString("utf8"))); } catch { /* ignore */ } }
      }
    });
    sock.on("close", () => { clearInterval(tick); for (const t of timers.values()) clearInterval(t); });
    sock.on("error", () => {});
  });
  return new Promise((res) => server.listen(0, "127.0.0.1", () => res({ server, stats, url: `http://127.0.0.1:${server.address().port}/` })));
}

// ---------------------------------------------------------------- the checks
const mock = urlArg ? undefined : await startMock(dist);
if (argv.includes("--serve")) {
  console.log(`mock game at ${mock.url} (Ctrl+C to stop)`);
  await new Promise(() => {});
}
const url = urlArg ?? mock.url;
const browser = await chromium.launch({ channel: "msedge", headless: true });
try {
  const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
  const errors = [];
  page.on("pageerror", (e) => errors.push(e.message));
  page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
  await page.goto(url, { waitUntil: "load" });
  await page.waitForSelector(".badge-open", { timeout: 15000 });
  check("Game state panel listed", await page.locator('nav [data-panel="entities"]').count() === 1);
  await page.locator('nav [data-panel="entities"]').click();
  await page.locator('[data-gs-tab="worms"]').click();
  await page.waitForSelector('.gs-bar[data-gs="match"]', { timeout: 10000 });
  check("match bar says in a match", (await page.locator(".gs-bar").textContent()).includes("In a match"));
  await page.waitForSelector('table[data-gs="worms"] tbody tr', { timeout: 5000 });
  const wormRows = await page.locator('table[data-gs="worms"] tbody tr').count();
  const teamRows = await page.locator('table[data-gs="teams"] tbody tr').count();
  if (mock) {
    check("four worms and two teams", wormRows === 4 && teamRows === 2, `${wormRows} worms, ${teamRows} teams`);
    check("active worm highlighted", await page.locator('table[data-gs="worms"] tr.cur').getAttribute("data-slot") === "2");
    check("dead worm marked", await page.locator('table[data-gs="worms"] tr.dead').count() === 1);
    check("state subscribed at 5 Hz", mock.stats.subs.state === 5, `hz=${mock.stats.subs.state}`);
    await page.locator('table[data-gs="worms"] tr[data-slot="1"]').click();
    await page.waitForFunction(() => /WXWormLogicEntity/.test(document.querySelector(".gs-insp-title")?.textContent ?? ""), null, { timeout: 5000 });
    check("clicking a worm inspects its entity", true);
  } else {
    check("worms and teams listed", wormRows > 0 && teamRows > 0, `${wormRows} worms, ${teamRows} teams`);
    check("one active worm highlighted", await page.locator('table[data-gs="worms"] tr.cur').count() <= 1);
  }

  await page.locator('[data-gs-tab="entities"]').click();
  await page.waitForSelector(".gs-list .vt-row", { timeout: 5000 });
  const entRows = await page.locator(".gs-list .vt-row").count();
  if (mock) {
    check("entity list without Other by default", entRows === 6, `${entRows} rows`);
    check("entities subscribed", mock.stats.subs.entities === 2, `hz=${mock.stats.subs.entities}`);
    await page.locator(".gs-list .vt-row", { hasText: "Bazooka" }).click();
    await page.waitForFunction(() => /ParabolicPayloadLogicEntity/.test(document.querySelector(".gs-insp-title")?.textContent ?? ""), null, { timeout: 5000 });
    check("inspector shows the projectile", true);
    check("inspected by handle", mock.stats.inspect.some((p) => p.handle === 0x5021 && p.len === 256));
    const hexRowsN = await page.locator(".gs-hex-row").count();
    check("hex view has 16 rows of 16 bytes", hexRowsN === 16, `${hexRowsN} rows`);
    check("unreadable bytes shown as ??", await page.locator(".gs-byte.na").count() === 4);
    const values = await page.locator('[data-gs="values"]').textContent();
    check("typed values at offset 0", values.includes("u32 8762628"), values);
    await page.locator(".gs-byte").nth(8).click();
    check("picking a byte moves the offset", (await page.locator('[data-gs="values"]').textContent()).includes("+0x8"));
    await page.locator(".fb-chip", { hasText: "Other" }).click();
    await page.waitForFunction(() => document.querySelectorAll(".gs-list .vt-row").length === 7, null, { timeout: 3000 });
    check("kind chip shows Other entities", true);
  } else {
    check("entity list has rows", entRows > 0, `${entRows} rows`);
  }

  await page.locator('[data-gs-tab="map"]').click();
  await page.waitForSelector(".gs-svg", { timeout: 5000 });
  const dots = await page.locator(".gs-dot.worm").count();
  check("map plots the worms", mock ? dots === 4 : dots > 0, `${dots} worms`);
  if (mock) {
    check("active worm ringed", await page.locator(".gs-ring").count() === 1);
    check("objects plotted", await page.locator(".gs-dot.k-Crate").count() === 1 && await page.locator(".gs-dot.k-Projectile").count() === 1);
    await page.getByRole("button", { name: "Side (X/Y)" }).click();
    check("side view draws the water", await page.locator(".gs-water").count() === 1);
  }

  await page.locator('[data-gs-tab="vars"]').click();
  await page.locator('[data-action="load-vars"]').click();
  await page.waitForSelector(".gs-list .vt-row", { timeout: 10000 });
  const varRows = await page.locator(".gs-list .vt-row").count();
  check("variables loaded", mock ? varRows === 6 : varRows > 10, `${varRows} rows`);
  if (mock) {
    await page.locator('[data-action="load-vars"]').click();
    await page.waitForFunction(() => /at most one call per second/.test(document.querySelector(".gs-vars-bar")?.textContent ?? ""), null, { timeout: 3000 });
    check("rate limit shown", true);
    await page.locator(".gs-link").first().click();
    await page.waitForFunction(() => /WormDataContainer/.test(document.querySelector(".gs-insp-title")?.textContent ?? ""), null, { timeout: 5000 });
    check("container opens in the inspector", mock.stats.inspect.some((p) => p.addr === 0x2c001000));
    await page.locator('[data-action="follow"]').click();
    await page.waitForFunction(() => /0x0085b504/.test(document.querySelector(".gs-facts")?.textContent ?? ""), null, { timeout: 5000 });
    check("follow pointer", mock.stats.inspect.some((p) => p.addr === 0x85b504));
  }
  check("no page errors", errors.length === 0, errors.join(" | "));
} finally {
  await browser.close();
  mock?.server.close();
}
const failed = results.filter((r) => !r.ok).length;
console.log(`gamestate e2e: ${results.length - failed}/${results.length} passed`);
process.exit(failed ? 1 : 0);
