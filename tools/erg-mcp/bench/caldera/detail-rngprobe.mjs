// In-game evidence for the detail layer's neutrality: the logic and second RNG states of the last ~480 sim ticks of a
// running match (wormsign.detail, the per-tick ring the engine hash reads), so a Test of the dressed level and one of the
// undressed level can be compared while nothing is input: an emitter, a light or a sound that drew from the sim's RNG would
// move it on ticks where the undressed level's stays put. Prints the counts of ticks whose rng or rng2 changed, the active
// worm and team, and writes every tick to --out.
//
//   tools\node\node.exe tools\erg-mcp\bench\caldera\detail-rngprobe.mjs --url <the test game's launch url> --out <file.json> [--ticks 480] [--wait 0]
import fs from "node:fs";

const argv = process.argv.slice(2);
const opt = (f, d) => { const i = argv.indexOf(f); return i >= 0 ? argv[i + 1] : d; };
const url = new URL(opt("--url"));
const N = +opt("--ticks", 480), WAIT = +opt("--wait", 0), OUT = opt("--out", "rngprobe.json");
const ws = new WebSocket(`ws://${url.host}/ws?k=${encodeURIComponent(url.searchParams.get("k"))}`);
let id = 0, lastTick = null;
const pend = new Map();
const call = (m, p = {}) => new Promise((res, rej) => { const i = ++id; pend.set(i, { res, rej }); ws.send(JSON.stringify({ t: "call", id: i, m, p })); });
ws.onmessage = (e) => {
  const m = JSON.parse(e.data);
  if (m.t === "res" && pend.has(m.id)) { pend.get(m.id).res(m.r); pend.delete(m.id); }
  else if (m.t === "err" && pend.has(m.id)) { pend.get(m.id).rej(new Error(`${m.code} ${m.msg}`)); pend.delete(m.id); }
  else if (m.t === "ev" && m.ch === "wormsign.tick") lastTick = m.d;
};
await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
ws.send(JSON.stringify({ t: "hello", proto: 1, build: "detail-rngprobe", client: "caldet" }));
ws.send(JSON.stringify({ t: "sub", ch: "wormsign.tick", filter: { hz: 10 } }));
const t0 = Date.now();
while ((!lastTick || Date.now() - t0 < WAIT * 1000) && Date.now() - t0 < 30000 + WAIT * 1000) await new Promise((r) => setTimeout(r, 100));
if (!lastTick) throw new Error("no wormsign.tick: is a match playing?");
const T = lastTick.tick, rows = [];
for (let k = Math.max(1, T - N); k <= T; k++) {
  try {
    const r = await call("wormsign.detail", { tick: k });
    const l = typeof r.local === "string" ? JSON.parse(r.local) : r.local;
    rows.push({ tick: k, rng: l.rng, rng2: l.rng2, curTeam: l.curTeam, activeWorm: l.activeWorm, proj: (l.proj ?? l.projectiles ?? []).length ?? null });
  } catch (e) { rows.push({ tick: k, error: e.message }); }
}
ws.close();
const ok = rows.filter((r) => !r.error);
const changes = (k) => ok.slice(1).filter((r, i) => r[k] !== ok[i][k]).length;
const summary = { lastTick: T, ticks: ok.length, errors: rows.length - ok.length, rngChanged: changes("rng"), rng2Changed: changes("rng2"),
  turns: [...new Set(ok.map((r) => `${r.curTeam}/${r.activeWorm}`))], projectilesMax: Math.max(0, ...ok.map((r) => r.proj ?? 0)) };
fs.writeFileSync(OUT, JSON.stringify({ summary, rows }, null, 1));
console.log(JSON.stringify(summary));
process.exit(0);
