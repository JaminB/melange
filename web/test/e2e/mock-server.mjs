// A stand-in for the game's Oasis server, for browser tests without the game: serves a built web app and speaks
// protocol v1 over a small RFC 6455 implementation. It fakes the log, bus, bus.counts, mods and store channels and the
// lua.*, mods.*, ini.*, bus.names, log.sessions, level.* and store.* methods, with the same rules the real handlers apply.
//   import { startMock } from "./mock-server.mjs"; const m = await startMock({ root: "web/dist" });
//   node web/test/e2e/mock-server.mjs [--root web/dist] [--port 0] [--standalone] [--read-only] [--online]
import { createHash, randomBytes } from "node:crypto";
import { readFileSync, existsSync, statSync } from "node:fs";
import { createServer } from "node:http";
import { extname, join, normalize, resolve } from "node:path";
import { AFTER, ergService } from "./erg-mock.mjs";
import { storeService } from "./store-mock.mjs";
import { launcherService } from "./launcher-mock.mjs";
import { importService } from "./import-mock.mjs";

const CSP = "default-src 'self'; connect-src 'self'; img-src 'self' blob: data:; style-src 'self' 'unsafe-inline'; frame-src 'self'; " +
  "frame-ancestors 'none'; base-uri 'none'; form-action 'none'";
const TYPES = { ".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8", ".css": "text/css; charset=utf-8",
  ".svg": "image/svg+xml", ".txt": "text/plain; charset=utf-8", ".json": "application/json", ".jsonl": "application/x-ndjson" };
const LEVELS = ["trace", "debug", "info", "warn", "error", "fatal"];

const INI = [
  "; Melange settings. Comments like this one must survive every edit.",
  "[Oasis]",
  "Enabled=1",
  "MaxClients=4 ; browser tabs",
  "Port=8765",
  "",
  "; frame pacing",
  "[FrameInterval]",
  "IntervalMs=16",
  "",
  "[Thumper]",
  "GrantSalt=0123456789abcdef",
  "AutoGrantDeepDesert=0",
  "",
  "[Extra]",
  "Undeclared=yes",
  "",
].join("\r\n");
const SCHEMA = [
  ["Oasis", "Enabled", "1", false], ["Oasis", "MaxClients", "4", false], ["Oasis", "Port", "8765", false],
  ["Oasis", "ReadOnly", "0", false], ["FrameInterval", "IntervalMs", "16", true],
  ["Thumper", "GrantSalt", "", false], ["Thumper", "AutoGrantDeepDesert", "0", false],
];

function iniEntries(text) {
  const out = [];
  let section = null;
  text.split(/\r?\n/).forEach((line, i) => {
    const t = line.trim();
    const h = /^\[([^\]]*)\]/.exec(t);
    if (h) { section = h[1].trim(); return; }
    if (!section || !t || t.startsWith(";")) return;
    const eq = line.indexOf("=");
    if (eq < 0) return;
    const key = line.slice(0, eq).trim();
    let v = line.slice(eq + 1).trim();
    const c = v.indexOf(";");
    if (c >= 0) v = v.slice(0, c).trimEnd();
    if (!out.some((e) => e.section.toLowerCase() === section.toLowerCase() && e.key.toLowerCase() === key.toLowerCase()))
      out.push({ section, key, value: v, line: i + 1 });
  });
  return out;
}

function iniSet(text, section, key, value) {
  const lines = text.split("\r\n");
  let inSec = false, lastKey = -1;
  for (let i = 0; i < lines.length; i++) {
    const t = lines[i].trim();
    const h = /^\[([^\]]*)\]/.exec(t);
    if (h) {
      if (inSec) break;
      inSec = h[1].trim().toLowerCase() === section.toLowerCase();
      if (inSec) lastKey = i;
      continue;
    }
    if (!inSec || !t || t.startsWith(";")) continue;
    const eq = lines[i].indexOf("=");
    if (eq < 0) continue;
    lastKey = i;
    if (lines[i].slice(0, eq).trim().toLowerCase() !== key.toLowerCase()) continue;
    const rest = lines[i].slice(eq + 1);
    const lead = rest.length - rest.trimStart().length;
    const c = rest.indexOf(";");
    let comment = "";
    if (c >= 0) {
      let k = c;
      while (k > 0 && /[ \t]/.test(rest[k - 1])) k--;
      comment = rest.slice(k);
    }
    lines[i] = lines[i].slice(0, eq + 1) + rest.slice(0, lead) + value + comment;
    return lines.join("\r\n");
  }
  if (lastKey >= 0) { lines.splice(lastKey + 1, 0, `${key}=${value}`); return lines.join("\r\n"); }
  return `${text}\r\n[${section}]\r\n${key}=${value}\r\n`;
}

function initialMods() {
  return [
    { id: "hello-spice", name: "Hello Spice", version: "1.0.0", authors: "Melange", dir: "Mods\\hello-spice", kind: "client",
      state: "enabled", reason: "", on: true, restartRequired: false, implicitManifest: false, hasClient: true, hasSim: false,
      deepDesert: { declared: false, granted: false }, order: 0, sandbox: { loaded: true, error: "", callbacks: 3, disabledCallbacks: 0, faults: 0, bytes: 20480 } },
    { id: "big-crates", name: "Big Crates", version: "0.3.0", authors: "Someone", dir: "Mods\\big-crates", kind: "content",
      state: "enabled", reason: "", on: true, restartRequired: false, implicitManifest: false, hasClient: false, hasSim: true,
      deepDesert: { declared: false, granted: false }, order: 1 },
    { id: "memwatch", name: "Memory Watch", version: "2.1.0", authors: "Tinkerer", dir: "Mods\\memwatch", kind: "client",
      state: "enabled", reason: "", on: true, restartRequired: false, implicitManifest: false, hasClient: true, hasSim: false,
      deepDesert: { declared: true, granted: true }, order: 2 },
    { id: "rawpeek", name: "Raw Peek", version: "0.1.0", authors: "Tinkerer", dir: "Mods\\rawpeek", kind: "client",
      state: "pending-consent", reason: "asks for Deep Desert", on: true, restartRequired: false, implicitManifest: false, hasClient: true, hasSim: false,
      deepDesert: { declared: true, granted: false }, order: 3 },
    { id: "dune-maps", name: "Dune Maps", version: "1.0.0", authors: "Mapper", dir: "Mods\\dune-maps", kind: "content",
      state: "disabled", reason: "", on: false, restartRequired: false, implicitManifest: false, hasClient: false, hasSim: false,
      deepDesert: { declared: false, granted: false }, order: 4 },
  ].map((m) => ({ ...m, source: m.id === "hello-spice" ? "local" : "store" }));   // hello-spice was put in Mods\ by hand
}

// mods.view: the "Show local plugins" choice and one compatibility-sweep notice.
function initialModsView() {
  return { showLocal: false, notices: [{ key: "2026-10-01T10:00:00Z-old-hud", id: "old-hud", name: "Old HUD", version: "0.1.0",
    action: "quarantined", reason: "needs Melange >=9.0.0, you have 0.3.6", melange: "0.3.6", at: "2026-10-01T10:00:00Z",
    folder: ".incompatible\\old-hud", detail: "", text: "Moved Old HUD to Mods\\.incompatible\\old-hud: needs Melange >=9.0.0, you have 0.3.6" }] };
}

const BUS = ["Camera.HasUpdated", "GameLogic.Turn.Started", "GameLogic.Turn.Ended", "GameLogic.Weapon.Fired", "Explosion.Created", "Worm.Damaged"];

export async function startMock(opts = {}) {
  const root = resolve(opts.root ?? "web/dist");
  const token = randomBytes(12).toString("base64url");
  const cookie = randomBytes(12).toString("hex");
  const launcherMode = !!opts.launcher;
  const state = {
    server: launcherMode ? "standalone" : opts.standalone ? "standalone" : "game", readOnly: !!opts.readOnly, online: !!opts.online, inMatch: opts.inMatch ?? true,
    ini: INI, mods: initialMods(), modsView: initialModsView(), calls: [], logRate: 0, seq: 0, busPosted: 0,
  };
  const build = existsSync(join(root, "build.txt")) ? readFileSync(join(root, "build.txt"), "utf8").trim() : "dev";
  const clients = new Set();
  const session = "2026-09-27_10-00-00_pid4242";
  const sessionText = Array.from({ length: 300 }, (_, i) =>
    JSON.stringify({ v: 1, seq: i + 1, t: i * 10, wall: `2026-09-27T10:00:${String(i % 60).padStart(2, "0")}.000-04:00`, lvl: LEVELS[2 + (i % 3)], cat: i % 2 ? "net" : "core", msg: `past record ${i + 1}` })).join("\n") + "\n";

  const erg = ergService(state);
  const store = storeService(state, (ch, d) => broadcast(ch, d));
  const launcher = launcherMode ? launcherService(state, (ch, d) => broadcast(ch, d), opts.scenario) : undefined;
  const imports = launcherMode ? importService(state, (ch, d) => broadcast(ch, d)) : undefined;
  if (imports && opts.import) imports.setScenario(opts.import);
  // The Plugins page's importer button (spec §12.1) sits on an already-installed plugin's own row, so the launcher
  // fixture needs a "caravan" mod next to the main mock's unrelated ones.
  if (imports) state.mods = [...state.mods, { id: "caravan", name: "Caravan", version: "1.0.0", authors: "Melange", dir: "Mods\\caravan",
    kind: "client", state: "enabled", reason: "", on: true, restartRequired: false, implicitManifest: false, hasClient: false, hasSim: false,
    deepDesert: { declared: false, granted: false }, order: 5, source: "store" }];
  const methods = [...(state.server === "game"
    ? ["sys.ping", "lua.eval", "lua.complete", "mods.list", "mods.setEnabled", "mods.revokeDeepDesert", "mods.view", "mods.setShowLocal",
       "mods.dismissNotice", "levels.live", "ini.get", "ini.set", "bus.names", "log.sessions"]
    : ["sys.ping", "mods.list", "mods.view", "mods.setShowLocal", "mods.dismissNotice", "ini.get", "ini.set", "log.sessions"]),
    ...erg.methods, ...(state.server === "game" ? store.methods : []),
    ...(launcher ? [...store.methods, ...launcher.methods, ...(imports?.methods ?? [])] : [])];
  const channels = state.server === "game" ? ["log", "bus", "bus.counts", "mods", "stats", "store"] : launcher ? ["log", "store", "setup", "import", "update"] : ["log"];

  const logRecord = (lvl, cat, msg) => {
    state.seq++;
    const wall = new Date().toISOString();
    const j = { v: 1, seq: state.seq, t: Math.round(performance.now()), wall, frame: state.seq, lvl: LEVELS[lvl], cat, msg };
    return { seq: state.seq, lvl: LEVELS[lvl], cat, ts: wall, j: JSON.stringify(j) };
  };
  const backlog = [];
  const pushLog = (lvl, cat, msg) => {
    const r = logRecord(lvl, cat, msg);
    backlog.push(r);
    if (backlog.length > 500) backlog.shift();
    for (const c of clients) c.queue("log", r);
  };
  for (let i = 0; i < 40; i++) pushLog(i % 5 === 0 ? 3 : 2, i % 3 ? "core" : "mods", `boot record ${i + 1}`);

  const reply = (c, id, r) => c.send({ t: "res", id, r });
  const fail = (c, id, code, msg, data) => c.send(data !== undefined ? { t: "err", id, code, msg, data } : { t: "err", id, code, msg });
  const modPublic = () => JSON.parse(JSON.stringify(state.mods));

  const handlers = {
    "sys.ping": () => ({ frame: state.seq, ms: Date.now() }),
    "bus.names": () => BUS.map((name, i) => ({ id: i + 1, name, posts: 100 * (i + 1), deliveries: 150 * (i + 1) })),
    "log.sessions": () => [{ id: session, files: ["events.jsonl"], bytes: sessionText.length }],
    "lua.eval": (p) => {
      if (typeof p.code !== "string") throw [-32602, "code must be a string"];
      if (p.code.length > 65536) throw [-32602, "code is longer than 64 KB"];
      const target = p.target ?? "client";
      if (target === "match") {
        if (!state.inMatch) throw [-32001, "not in a match"];
        if (state.online) throw [-32000, "match console is off in online matches ([LuaConsole] MatchConsoleOnline=1 to allow)"];
      }
      if (target === "mod" && !state.mods.some((m) => m.id === p.mod && m.state === "enabled" && m.hasClient))
        throw [-32000, `mod '${p.mod}' is not an enabled mod with client code`];
      const code = p.code.trim();
      if (code.includes("@@")) return { ok: false, text: `[string "console"]:1: unexpected symbol near '@'` };
      if (/^return wum\.game\.scene\(\)$/.test(code)) return { ok: true, text: "match" };
      if (/^return GetData ~= nil$/.test(code)) return target === "match" ? { ok: true, text: "true" } : { ok: true, text: "false" };
      if (code.startsWith("=")) return { ok: true, text: code.slice(1).trim() };
      return { ok: true, text: `${target}: ${code}` };
    },
    "lua.complete": (p) => {
      const pre = String(p.prefix ?? "");
      if (pre === "wum.") return ["game", "log", "ui"];
      if (pre === "wum.ga") return ["game"];
      if (pre === "wum.game.") return ["scene", "worms", "teams"];
      return [];
    },
    "mods.list": () => modPublic(),
    "mods.setEnabled": (p) => {
      const m = state.mods.find((x) => x.id === p.id);
      if (!m) throw [-32602, `no mod '${p.id}'`];
      if (typeof p.on !== "boolean") throw [-32602, "on must be true or false"];
      m.on = p.on;
      if (m.kind === "content") { m.state = "restart-required"; m.restartRequired = true; m.reason = "enable/disable takes effect next launch"; }
      else if (m.deepDesert.declared && !m.deepDesert.granted && p.on) { m.state = "pending-consent"; m.reason = "asks for Deep Desert"; }
      else { m.state = p.on ? "enabled" : "disabled"; m.reason = ""; }
      setTimeout(() => broadcast("mods", modPublic()), 10);
      return JSON.parse(JSON.stringify(m));
    },
    "mods.view": () => JSON.parse(JSON.stringify(state.modsView)),
    "mods.setShowLocal": (p) => {
      if (typeof p.on !== "boolean") throw [-32602, "on must be true or false"];
      state.modsView.showLocal = p.on;
      return JSON.parse(JSON.stringify(state.modsView));
    },
    "mods.dismissNotice": (p) => {
      if (p.key !== undefined && typeof p.key !== "string") throw [-32602, "key must be a string"];
      state.modsView.notices = state.modsView.notices.filter((n) => p.key !== undefined && n.key !== p.key);
      return JSON.parse(JSON.stringify(state.modsView));
    },
    "levels.live": (p) => {
      const m = state.mods.find((x) => x.id === p.modId);
      if (!m) return { ok: false, reason: `no mod ${p.modId} is installed` };
      if (m.id !== "dune-maps") return { ok: false, reason: "the pack has scripts or weapons, so it needs a restart" };
      m.on = p.on; m.state = p.on ? "enabled" : "disabled"; m.reason = "";
      setTimeout(() => broadcast("mods", modPublic()), 10);
      return { ok: true, reason: "" };
    },
    "mods.revokeDeepDesert": (p) => {
      const m = state.mods.find((x) => x.id === p.id);
      if (!m) throw [-32602, `no mod '${p.id}'`];
      if (!m.deepDesert.declared) throw [-32602, `mod '${p.id}' does not ask for Deep Desert`];
      if (m.deepDesert.granted) { m.deepDesert.granted = false; m.state = "pending-consent"; m.reason = "Deep Desert revoked"; }
      setTimeout(() => broadcast("mods", modPublic()), 10);
      return JSON.parse(JSON.stringify(m));
    },
    "ini.get": () => {
      const entries = iniEntries(state.ini);
      const text = iniSet(state.ini, "Thumper", "GrantSalt", "********");
      const keys = SCHEMA.map(([section, key, def, live]) => {
        const e = entries.find((x) => x.section.toLowerCase() === section.toLowerCase() && x.key.toLowerCase() === key.toLowerCase());
        return { section, key, def, live, declared: true, current: e ? (key === "GrantSalt" ? "********" : e.value) : null, ...(e ? { line: e.line } : {}) };
      });
      for (const e of entries)
        if (!SCHEMA.some(([s, k]) => s.toLowerCase() === e.section.toLowerCase() && k.toLowerCase() === e.key.toLowerCase()))
          keys.push({ section: e.section, key: e.key, def: null, live: false, declared: false, current: e.value, line: e.line });
      return { path: "C:\\Games\\WUM\\Melange.ini", encoding: "ansi", text, keys };
    },
    "ini.set": (p) => {
      for (const k of ["section", "key", "value"]) if (typeof p[k] !== "string") throw [-32602, `${k} must be a string`];
      if (/[\r\n;]/.test(p.value) || /^\s|\s$/.test(p.value)) throw [-32602, "the value cannot contain line breaks or ';'"];
      if (p.section.toLowerCase() === "thumper" && p.key.toLowerCase() === "grantsalt") throw [-32000, "the Deep Desert grant salt can only be changed in Melange.ini itself"];
      if (p.section.toLowerCase() === "thumper" && p.key.toLowerCase() === "autograntdeepdesert" && p.value !== "0")
        throw [-32000, "Deep Desert can be revoked from the browser but never granted"];
      const decl = SCHEMA.find(([s, k]) => s.toLowerCase() === p.section.toLowerCase() && k.toLowerCase() === p.key.toLowerCase());
      const cur = iniEntries(state.ini).find((e) => e.section.toLowerCase() === p.section.toLowerCase() && e.key.toLowerCase() === p.key.toLowerCase());
      if (!decl && !cur) throw [-32602, `[${p.section}] ${p.key} is not a Melange setting`];
      const live = !!decl?.[3];
      if (cur && cur.value === p.value) return { live, restart: false, changed: false };
      state.ini = iniSet(state.ini, p.section, p.key, p.value);
      return { live, restart: !live, changed: true };
    },
  };
  Object.assign(handlers, erg.handlers, store.handlers, launcher?.handlers, imports?.handlers);
  const mutating = new Set(["lua.eval", "mods.setEnabled", "levels.live", "mods.revokeDeepDesert", "mods.setShowLocal", "mods.dismissNotice", "ini.set", ...erg.mutating, ...store.mutating, ...(launcher?.mutating ?? []), ...(imports?.mutating ?? [])]);

  function broadcast(ch, d) { for (const c of clients) if (c.subs.has(ch)) c.queue(ch, d); }

  const onMessage = (c, text) => {
    let m;
    try { m = JSON.parse(text); } catch { return c.close(4002, "bad json"); }
    if (!c.hello) {
      if (m.t !== "hello") return c.close(4002, "hello expected");
      if (m.proto !== 1) { c.send({ t: "bye", reason: "protocol", want: 1 }); return c.close(4001, "protocol"); }
      c.hello = true;
      return c.send({ t: "welcome", proto: 1, build, server: state.server, game: state.server === "game" ? { exeBuild: 1077, melange: "0.2.0-mock" } : undefined,
        caps: launcher ? ["launcher"] : [], channels, methods, panels: [],
        limits: { maxClients: 4, maxMessageKB: 1024, maxQueueKB: 2048, maxQueuedCalls: 16, readOnly: state.readOnly } });
    }
    if (m.t === "sub" || m.t === "unsub") {
      if (!channels.includes(m.ch)) { if (m.id !== undefined) fail(c, m.id, -32602, "unknown channel"); return; }
      if (m.t === "unsub") { c.subs.delete(m.ch); return; }
      c.subs.set(m.ch, m.filter ?? {});
      if (m.id !== undefined) reply(c, m.id, true);
      if (m.ch === "log") for (const r of backlog) c.queue("log", r);
      if (m.ch === "mods") c.queue("mods", modPublic());
      if (m.ch === "store") c.queue("store", store.initial());
      if (m.ch === "setup" && launcher) c.queue("setup", { status: launcher.status() });
      if (m.ch === "update" && launcher) c.queue("update", { status: launcher.updateStatus() });
      if (m.ch === "import" && imports) c.queue("import", imports.initial());
      return;
    }
    if (m.t === "call") {
      state.calls.push({ m: m.m, p: m.p });
      if (!methods.includes(m.m)) return fail(c, m.id, -32601, "unknown method");
      if (state.readOnly && mutating.has(m.m)) return fail(c, m.id, -32003, "Oasis is read-only ([Oasis] ReadOnly=1)");
      const onErr = (e) => (Array.isArray(e) ? fail(c, m.id, e[0], e[1], e[2]) : fail(c, m.id, -32004, String(e)));
      const run = () => {
        try {
          const r = handlers[m.m](m.p ?? {});
          if (r && typeof r.then === "function") return r.then((v) => reply(c, m.id, v), onErr);
          if (r && r[AFTER]) {
            reply(c, m.id, r.value);
            return r[AFTER](c);
          }
          return reply(c, m.id, r);
        } catch (e) {
          return onErr(e);
        }
      };
      // A test can hold one call back (state.delay(method, params) -> ms) to make two requests overlap.
      const wait = state.delay?.(m.m, m.p ?? {}) ?? 0;
      return wait > 0 ? void setTimeout(run, wait) : run();
    }
    fail(c, null, -32600, "unknown message type");
  };

  const server = createServer((req, res) => {
    const url = new URL(req.url, "http://x");
    const headers = { "Content-Security-Policy": CSP, "X-Content-Type-Options": "nosniff", "Referrer-Policy": "no-referrer" };
    if (url.searchParams.get("k") === token && url.pathname === "/") {
      if (launcher && url.searchParams.get("scenario")) launcher.setScenario(url.searchParams.get("scenario"));
      if (imports && url.searchParams.get("import")) imports.setScenario(url.searchParams.get("import"));
      res.writeHead(303, { ...headers, "Set-Cookie": `oasis_s=${cookie}; HttpOnly; SameSite=Strict; Path=/`, Location: "/", "Cache-Control": "no-store" });
      return res.end();
    }
    if (!(req.headers.cookie ?? "").includes(`oasis_s=${cookie}`)) { res.writeHead(403, headers); return res.end(); }
    if (url.pathname === `/logs/${session}/events.jsonl`) {
      res.writeHead(200, { ...headers, "Content-Type": TYPES[".jsonl"], "Cache-Control": "no-store" });
      return res.end(sessionText);
    }
    if (url.pathname.startsWith("/store/shots/") && state.server === "game") {
      const img = store.route(url.pathname);
      if (!img) { res.writeHead(404, headers); return res.end(); }
      res.writeHead(200, { ...headers, "Content-Type": "image/png", "Cache-Control": "no-store" });
      return res.end(img);
    }
    if (url.pathname.startsWith("/import/previews/") && imports) {
      const img = imports.route(url.pathname);
      if (!img) { res.writeHead(404, headers); return res.end(); }
      res.writeHead(200, { ...headers, "Content-Type": "image/png", "Cache-Control": "no-store" });
      return res.end(img);
    }
    const rel = url.pathname === "/" ? "index.html" : decodeURIComponent(url.pathname.slice(1));
    const file = normalize(join(root, rel));
    if (!file.startsWith(root) || !existsSync(file) || !statSync(file).isFile()) { res.writeHead(404, headers); return res.end(); }
    res.writeHead(200, { ...headers, "Content-Type": TYPES[extname(file)] ?? "application/octet-stream", "Cache-Control": "no-cache" });
    res.end(readFileSync(file));
  });

  server.on("upgrade", (req, sock) => {
    if (Date.now() < (state.holdUntil ?? 0)) {
      sock.end("HTTP/1.1 503 Service Unavailable\r\n\r\n");
      return;
    }
    const origin = req.headers.origin;
    const host = req.headers.host;
    if (!(req.headers.cookie ?? "").includes(`oasis_s=${cookie}`) || origin !== `http://${host}`) {
      sock.end("HTTP/1.1 403 Forbidden\r\n\r\n");
      return;
    }
    const accept = createHash("sha1").update(req.headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest("base64");
    sock.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`);
    sock.setNoDelay(true);
    const c = { hello: false, subs: new Map(), out: new Map(), seqs: new Map(), timer: undefined, dead: false };
    const frame = (op, payload) => {
      const len = payload.length;
      const head = len < 126 ? Buffer.from([0x80 | op, len]) : len < 65536 ? Buffer.from([0x80 | op, 126, len >> 8, len & 255]) : (() => {
        const b = Buffer.alloc(10); b[0] = 0x80 | op; b[1] = 127; b.writeBigUInt64BE(BigInt(len), 2); return b; })();
      if (!c.dead) sock.write(Buffer.concat([head, payload]));
    };
    c.send = (m) => frame(1, Buffer.from(JSON.stringify(m)));
    c.sendBinary = (ref, data) => {
      const head = Buffer.alloc(4);
      head.writeUInt32LE(ref, 0);
      c.send({ t: "bin", ref, ch: "erg", len: data.length, meta: null });
      frame(2, Buffer.concat([head, data]));
    };
    c.close = (code, reason) => {
      const b = Buffer.alloc(2 + Buffer.byteLength(reason)); b.writeUInt16BE(code, 0); b.write(reason, 2);
      frame(8, b);
      c.dead = true;
      setTimeout(() => sock.destroy(), 50);
    };
    c.queue = (ch, d) => {
      if (!c.subs.has(ch)) return;
      if (ch === "bus") {
        const names = c.subs.get(ch)?.names ?? [];
        if (!names.some((p) => (p.endsWith(".*") ? d.name.startsWith(p.slice(0, -1)) : d.name === p))) return;
        if (c.subs.get(ch)?.decode === false) { d = { ...d }; delete d.d; }
      }
      if (ch === "mods" || ch === "bus.counts" || ch === "stats") c.out.set(ch, [d]);
      else { const q = c.out.get(ch) ?? []; q.push(d); c.out.set(ch, q); }
      if (!c.timer) c.timer = setTimeout(flush, 50);
    };
    const flush = () => {
      c.timer = undefined;
      for (const [ch, items] of c.out) {
        const seq = (c.seqs.get(ch) ?? 0) + 1;
        c.seqs.set(ch, seq + items.length - 1);
        for (let i = 0; i < items.length; i += 256) {
          const b = items.slice(i, i + 256);
          c.send(b.length === 1 ? { t: "ev", ch, seq: seq + i, d: b[0] } : { t: "ev", ch, seq: seq + i, b });
        }
      }
      c.out.clear();
    };
    let buf = Buffer.alloc(0);
    sock.on("data", (chunk) => {
      buf = Buffer.concat([buf, chunk]);
      while (buf.length >= 2) {
        const op = buf[0] & 15, masked = buf[1] & 128;
        let len = buf[1] & 127, off = 2;
        if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); off = 4; }
        else if (len === 127) { if (buf.length < 10) return; len = Number(buf.readBigUInt64BE(2)); off = 10; }
        if (!masked) return c.close(1002, "unmasked");
        if (buf.length < off + 4 + len) return;
        const mask = buf.subarray(off, off + 4);
        const data = Buffer.from(buf.subarray(off + 4, off + 4 + len));
        for (let i = 0; i < data.length; i++) data[i] ^= mask[i & 3];
        buf = buf.subarray(off + 4 + len);
        if (op === 1) onMessage(c, data.toString("utf8"));
        else if (op === 8) { frame(8, data.subarray(0, 2)); c.dead = true; sock.end(); }
        else if (op === 9) frame(10, data);
      }
    });
    const gone = () => { c.dead = true; clients.delete(c); if (c.timer) clearTimeout(c.timer); };
    sock.on("close", gone);
    sock.on("error", gone);
    clients.add(c);
  });

  // Producers: the log at state.logRate records a second, bus traffic in a match, counts once a second.
  let carry = 0, lastTick = performance.now();
  const logTimer = setInterval(() => {
    const now = performance.now();
    carry += (state.logRate * (now - lastTick)) / 1000;
    lastTick = now;
    const n = Math.floor(carry);
    carry -= n;
    for (let i = 0; i < n; i++) pushLog(state.seq % 50 === 0 ? 3 : state.seq % 7 === 0 ? 1 : 2, state.seq % 4 ? "load" : "net", `synthetic line ${state.seq + 1}`);
  }, 10);
  const counts = new Map();
  const busTimer = setInterval(() => {
    if (state.server !== "game") return;
    const name = BUS[1 + (state.busPosted % (BUS.length - 1))];
    state.busPosted++;
    counts.set(name, (counts.get(name) ?? 0) + 1);
    broadcast("bus", { seq: state.busPosted, frame: 1000 + state.busPosted, name, cls: "Message", path: "GM.Logic", handle: "0x1234",
      d: { value: state.busPosted, sent: Date.now() } });
  }, 100);
  const countTimer = setInterval(() => {
    if (state.server !== "game") return;
    broadcast("bus.counts", Object.fromEntries(counts));
    counts.clear();
  }, 1000);

  await new Promise((r) => server.listen(opts.port ?? 0, "127.0.0.1", r));
  const port = server.address().port;
  return {
    port, token, state,
    url: `http://127.0.0.1:${port}/?k=${token}`,
    setLogRate: (n) => { state.logRate = n; },
    postBus: (name, d) => {
      state.busPosted++;
      broadcast("bus", { seq: state.busPosted, frame: 1000 + state.busPosted, name, cls: "Message", path: "GM.Logic", handle: "0x1", d });
    },
    kick: () => { for (const c of clients) c.close(4000, "kicked"); },
    // Drops every client and refuses new connections for `ms`.
    drop: (ms) => { state.holdUntil = Date.now() + ms; for (const c of clients) c.close(4000, "kicked"); },
    clients: () => clients.size,
    close: async () => {
      clearInterval(logTimer); clearInterval(busTimer); clearInterval(countTimer);
      store.close();
      launcher?.close();
      imports?.close();
      for (const c of clients) c.close(1001, "bye");
      await new Promise((r) => server.close(r));
      server.closeAllConnections?.();
    },
  };
}

if (process.argv[1] && import.meta.url.endsWith(process.argv[1].replace(/\\/g, "/").split("/").pop())) {
  const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
  const m = await startMock({ root: arg("--root", "web/dist"), port: Number(arg("--port", "0")), standalone: process.argv.includes("--standalone"),
    readOnly: process.argv.includes("--read-only"), online: process.argv.includes("--online"), launcher: process.argv.includes("--launcher"),
    scenario: arg("--scenario", undefined), import: arg("--import", undefined) });
  m.setLogRate(Number(arg("--log-rate", "5")));
  console.log(m.url);
}
