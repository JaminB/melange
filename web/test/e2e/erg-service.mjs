// The Erg level service over a live Oasis server (the game's or oasis.exe), in headless Edge.
//   node web/test/e2e/erg-service.mjs <launch url> [--base <key>] [--dump <file.json>]
// Opens the launch URL, then over the page's own authenticated origin: level.list, level.themes, level.palette,
// level.load of one base with every blob arriving as a binary frame (ref, size and meta checked against the scene),
// and the refusals of bad params. --dump writes the scene and each blob's sha256 so the game's and oasis.exe's answers
// can be compared file to file.
import { createHash } from "node:crypto";
import { writeFileSync } from "node:fs";
import { chromium } from "playwright-core";

const url = process.argv[2] || process.env.OASIS_URL;
const arg = (name) => {
  const i = process.argv.indexOf(name);
  return i > 0 ? process.argv[i + 1] : undefined;
};
if (!url) {
  console.error("usage: erg-service.mjs <launch url> [--base <key>] [--dump <file.json>]");
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
  await page.goto(url, { waitUntil: "load" });
  const out = await page.evaluate(async (wantBase) => {
    const ws = new WebSocket(`ws://${location.host}/ws`);
    ws.binaryType = "arraybuffer";
    const pending = new Map();
    const bins = [];
    let nextId = 1;
    let welcome;
    const opened = new Promise((resolve, reject) => {
      ws.onerror = () => reject(new Error("socket error"));
      ws.onmessage = (ev) => {
        if (ev.data instanceof ArrayBuffer) {
          bins.push(ev.data);
          return;
        }
        const m = JSON.parse(ev.data);
        if (m.t === "welcome") {
          welcome = m;
          resolve();
        } else if (m.t === "bin") {
          bins.push(m);
        } else if ((m.t === "res" || m.t === "err") && pending.has(m.id)) {
          pending.get(m.id)(m);
          pending.delete(m.id);
        }
      };
      ws.onopen = () => ws.send(JSON.stringify({ t: "hello", proto: 1, build: "dev", client: "erg-e2e" }));
    });
    await opened;
    const call = (m, p) => new Promise((resolve) => {
      const id = nextId++;
      pending.set(id, resolve);
      ws.send(JSON.stringify({ t: "call", id, m, p }));
    });
    const r = { methods: welcome.methods.filter((m) => m.startsWith("level.")), server: welcome.server };
    r.list = await call("level.list", {});
    r.themes = await call("level.themes", {});
    const bases = r.list.r?.bases ?? [];
    const base = wantBase ?? bases.find((b) => b.source === "game")?.key;
    r.palette = await call("level.palette", { theme: bases.find((b) => b.key === base)?.theme || "BUILDING" });
    r.bad = [await call("level.load", {}), await call("level.load", { base: "Multi.NoSuchLevel" }), await call("level.palette", { theme: "MOON" }),
      await call("level.save", { project: "../x", patch: {} })];
    const t0 = performance.now();
    bins.length = 0;
    r.load = await call("level.load", { base });
    const blobs = r.load.r?.blobs ?? [];
    const deadline = Date.now() + 30000;
    while (bins.length < blobs.length * 2 && Date.now() < deadline) await new Promise((res) => setTimeout(res, 20));
    r.loadMs = performance.now() - t0;
    r.base = base;
    r.bins = [];
    for (let i = 0; i + 1 < bins.length; i += 2) {
      const a = bins[i], frame = bins[i + 1];
      const ref = frame instanceof ArrayBuffer && frame.byteLength >= 4 ? new DataView(frame).getUint32(0, true) : -1;
      const body = frame instanceof ArrayBuffer ? Array.from(new Uint8Array(frame, 4)) : [];
      r.bins.push({ announce: a, ref, bytes: body });
    }
    ws.close();
    return r;
  }, arg("--base"));

  const levelMethods = ["level.list", "level.new", "level.load", "level.save", "level.export", "level.build", "level.themes", "level.palette", "level.close"];
  check("the level methods are offered", levelMethods.every((m) => out.methods.includes(m)), `${out.server}: ${out.methods.join(",")}`);
  const bases = out.list.r?.bases ?? [];
  check("level.list names bases", out.list.t === "res" && bases.length > 0 && bases.every((b) => b.key && b.stem && b.title && b.source),
    `${bases.length} bases, ${(out.list.r?.projects ?? []).length} projects`);
  check("level.themes: eleven themes, three times of day", out.themes.r?.themes?.length === 11 && out.themes.r?.timesOfDay?.length === 3,
    `${out.themes.r?.materialFiles?.length ?? 0} material files`);
  const entries = out.palette.r?.entries ?? [];
  check("level.palette: knots and objects", entries.filter((e) => e.role === "spawn").length === 8 && entries.some((e) => e.name === "oildrum"),
    `${entries.length} entries`);
  check("bad params are refused", out.bad.every((m) => m.t === "err") && out.bad[0].code === -32602 && out.bad[2].code === -32602,
    out.bad.map((m) => m.code).join(","));
  const scene = out.load.r;
  check("level.load returns a scene", out.load.t === "res" && scene?.format === "erg-scene/1" && scene.base.key === out.base,
    out.load.t === "res" ? `${out.base}: ${scene.frames.length} frames, ${scene.details.length} details` : JSON.stringify(out.load));
  const blobs = scene?.blobs ?? [];
  let same = out.bins.length === blobs.length;
  for (let i = 0; same && i < blobs.length; i++) {
    const b = out.bins[i];
    same = b.announce.t === "bin" && b.announce.ref === blobs[i].ref && b.announce.ch === "erg" && b.announce.len === blobs[i].bytes &&
      b.ref === blobs[i].ref && b.bytes.length === blobs[i].bytes && b.announce.meta?.kind === blobs[i].kind && b.announce.meta?.frame === blobs[i].frame;
  }
  check("one binary frame per blob, in order, after the result", same, `${out.bins.length}/${blobs.length} in ${out.loadMs.toFixed(0)} ms`);
  const dump = arg("--dump");
  if (dump && scene) {
    const blobsSha = out.bins.map((b) => ({ ref: b.ref, sha256: createHash("sha256").update(Buffer.from(b.bytes)).digest("hex") }));
    writeFileSync(dump, JSON.stringify({ server: out.server, list: out.list.r, scene, blobs: blobsSha }, null, 1));
    check("dump written", true, dump);
  }
} finally {
  await browser.close();
}
const failed = results.filter((r) => !r.ok).length;
console.log(`erg e2e: ${results.length - failed}/${results.length} passed`);
process.exit(failed ? 1 : 0);
