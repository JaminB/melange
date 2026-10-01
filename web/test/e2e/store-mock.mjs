// The Store's store.* methods, `store` channel and /store/shots/ route for the mock server, over a small fixture:
// one installed plugin with an update, a content map pack, a Deep Desert plugin, one whose download fails its hash
// check and one that needs a newer Melange (hidden unless asked for).
import { deflateSync } from "node:zlib";

// A 4x3 RGB PNG, built here so the fixture holds no binary.
function png() {
  const crcTable = Array.from({ length: 256 }, (_, n) => {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    return c >>> 0;
  });
  const crc = (buf) => {
    let c = 0xffffffff;
    for (const b of buf) c = crcTable[(c ^ b) & 255] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
  };
  const chunk = (type, data) => {
    const len = Buffer.alloc(4);
    len.writeUInt32BE(data.length);
    const body = Buffer.concat([Buffer.from(type, "ascii"), data]);
    const sum = Buffer.alloc(4);
    sum.writeUInt32BE(crc(body));
    return Buffer.concat([len, body, sum]);
  };
  const w = 4, h = 3;
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0);
  ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8;
  ihdr[9] = 2;
  const rows = [];
  for (let y = 0; y < h; y++) rows.push(Buffer.from([0, ...Array.from({ length: w }, () => [40, 120, 200]).flat()]));
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr), chunk("IDAT", deflateSync(Buffer.concat(rows))),
    chunk("IEND", Buffer.alloc(0))]);
}
const PNG = png();

function fixture() {
  const v = (version, extra = {}) => ({ version, released: "2026-10-01", melange: ">=0.2.0", size: 1834221, changelog: `Changes in ${version}.`,
    yanked: false, compatible: true, ...extra });
  return [
    { id: "hd-water", name: "HD Water", authors: ["someone"], description: "Water that looks like water.", categories: ["graphics"],
      licence: "MIT", homepage: "https://example.com/hd-water", kind: "client-only", unsafe: false, filesystem: "none",
      versions: [v("1.1.0"), v("1.0.0")], installed: { version: "1.0.0", managed: true, state: "enabled", enabled: true },
      screenshots: [{ n: 1, caption: "Water at sunset" }], dependencies: [], conflicts: [] },
    { id: "dune-pack", name: "Dune Pack", authors: ["mapper"], description: "Six desert maps.", categories: ["maps"],
      licence: "CC-BY-4.0", homepage: "", kind: "content", unsafe: false, filesystem: "none", versions: [v("2.0.0")], installed: null,
      screenshots: [], dependencies: [], conflicts: [] },
    { id: "raw-tools", name: "Raw Tools", authors: ["tinkerer"], description: "Memory inspection helpers.", categories: ["tools"],
      licence: "MIT", homepage: "", kind: "client-only", unsafe: true, filesystem: "none", versions: [v("0.3.0")], installed: null,
      screenshots: [], dependencies: [{ id: "hd-water", range: "^1.0.0" }], conflicts: [] },
    { id: "broken-zip", name: "Broken Zip", authors: ["oops"], description: "Its release asset was replaced.", categories: ["tools"],
      licence: "MIT", homepage: "", kind: "client-only", unsafe: false, filesystem: "none", versions: [v("1.0.0")], installed: null,
      screenshots: [], dependencies: [], conflicts: [] },
    { id: "future-only", name: "Future Only", authors: ["later"], description: "Needs a newer Melange.", categories: ["gameplay"],
      licence: "MIT", homepage: "", kind: "client-only", unsafe: false, filesystem: "none",
      versions: [v("1.0.0", { melange: ">=9.0.0", compatible: false })], installed: null, screenshots: [], dependencies: [], conflicts: [] },
  ];
}

const cmp = (a, b) => {
  const pa = a.split(".").map(Number), pb = b.split(".").map(Number);
  for (let i = 0; i < 3; i++) if (pa[i] !== pb[i]) return pa[i] < pb[i] ? -1 : 1;
  return 0;
};

export function storeService(state, broadcast) {
  const s = state.store = {
    plugins: fixture(), serial: 7, fetchedAt: "", fetching: false, haveIndex: false, gate: "", busy: false, pending: [], shots: 0,
    shotsFetched: new Set(), job: { phase: "idle", id: "", version: "", bytes: 0, total: 0, message: "" }, rowError: {},
    opened: [], refreshes: 0, slow: false, timers: new Set(),
  };
  s.setGate = (g) => { s.gate = g; push(); };
  const later = (ms, fn) => { const t = setTimeout(() => { s.timers.delete(t); fn(); }, ms); s.timers.add(t); };
  const event = () => ({ ...s.job, pending: [...s.pending], shots: s.shots, busy: s.busy, gate: s.gate, fetching: s.fetching, serial: s.serial });
  const push = () => broadcast("store", event());
  const find = (id) => s.plugins.find((p) => p.id === id);

  const item = (p) => {
    const compatible = p.versions.find((v) => v.compatible && !v.yanked);
    let action = "none", state = "", reason = "", canRemove = false;
    if (s.pending.includes(p.id)) { state = "pending"; reason = "Applies at the next launch"; }
    else if (!p.installed) {
      if (compatible) action = "install";
      else { state = "incompatible"; reason = `Incompatible: needs Melange ${p.versions[0].melange}`; }
    } else {
      canRemove = true;
      if (compatible && cmp(compatible.version, p.installed.version) > 0) { action = "update"; state = "update"; }
      else { action = "remove"; state = "installed"; }
    }
    const shown = compatible ?? p.versions[0];
    return { id: p.id, name: p.name, authors: p.authors, description: p.description, categories: p.categories, kind: p.kind, unsafe: p.unsafe,
      licence: p.licence, latest: p.versions[0].version, compatible: compatible?.version ?? "", size: shown.size,
      installed: p.installed ? { ...p.installed } : null, action, canRemove, state, reason, error: s.rowError[p.id] ?? "" };
  };

  const finish = (phase, id, version, message) => {
    s.job = { phase, id, version, bytes: 0, total: 0, message };
    s.busy = false;
    if (phase === "error") s.rowError[id] = message;
    push();
  };

  const runInstall = (p, version) => {
    const total = p.versions.find((v) => v.version === version)?.size ?? 1000;
    const step = s.slow ? 300 : 40;
    let i = 0;
    const tick = () => {
      if (s.job.phase === "cancelled") return finish("error", p.id, version, "cancelled");
      if (i <= 4) {
        s.job = { phase: "downloading", id: p.id, version, bytes: Math.round((total * i) / 4), total, message: "" };
        push();
        i++;
        return later(step, tick);
      }
      if (p.id === "broken-zip")
        return finish("error", p.id, version, "Downloaded file does not match the store's record (sha256 " + "0".repeat(64) +
          ", expected " + "a".repeat(64) + "). Nothing was installed.");
      s.job = { phase: "verifying", id: p.id, version, bytes: total, total, message: "" };
      push();
      later(step, () => {
        s.job = { phase: "installing", id: p.id, version, bytes: 0, total: 0, message: "" };
        push();
        later(step, () => {
          const content = p.kind === "content";
          p.installed = { version, managed: true, state: content ? "restart-required" : p.unsafe ? "pending-consent" : "enabled", enabled: !content };
          finish("done", p.id, version, `Installed ${p.name} ${version}`);
        });
      });
    };
    tick();
  };

  const guard = (id) => {
    if (s.busy) throw [-32002, "another install, update or remove is running"];
    if (s.gate) throw [-32000, s.gate];
    const p = find(id);
    if (!p) throw [-32602, `no plugin '${id}' in the list`];
    return p;
  };

  const handlers = {
    "store.status": () => ({ enabled: true, indexUrl: "file:///C:/fixtures/store/index.json", customIndex: true, fetchedAt: s.fetchedAt,
      offline: false, serial: s.serial, plugins: s.haveIndex ? s.plugins.length : 0, job: { ...s.job }, gate: s.gate, fetching: s.fetching,
      haveIndex: s.haveIndex, rollback: false, busy: s.busy, error: "", pending: [...s.pending], notices: [] }),
    "store.refresh": () => {
      s.refreshes++;
      if (!s.fetching) {
        s.fetching = true;
        s.job = { phase: "fetching", id: "", version: "", bytes: 0, total: 0, message: "" };
        push();
        later(30, () => {
          s.fetching = false;
          s.haveIndex = true;
          s.fetchedAt = "2026-10-01 14:02";
          s.job = { phase: "idle", id: "", version: "", bytes: 0, total: 0, message: "" };
          push();
        });
      }
      return { started: true };
    },
    "store.list": (p) => {
      if (!s.haveIndex) return [];
      const q = String(p.query ?? "").toLowerCase();
      const filter = p.filter ?? "all";
      if (!["all", "installed", "updates"].includes(filter)) throw [-32602, "filter must be all, installed or updates"];
      return s.plugins.filter((x) => !q || [x.name, x.id, x.description, ...x.authors].some((t) => t.toLowerCase().includes(q)))
        .filter((x) => !p.category || x.categories.includes(p.category))
        .map(item)
        .filter((it) => filter === "all" || (filter === "installed" ? it.installed : it.action === "update"))
        .filter((it) => it.state !== "incompatible" || p.incompatible === true)
        .sort((a, b) => (a.action === "update") !== (b.action === "update") ? (a.action === "update" ? -1 : 1) : a.name.localeCompare(b.name));
    },
    "store.details": (p) => {
      const x = find(p.id);
      if (!x) throw [-32602, `no plugin '${p.id}' in the list`];
      if (x.screenshots.length && !s.shotsFetched.has(x.id)) {
        s.shotsFetched.add(x.id);
        later(50, () => { s.shots++; push(); });
      }
      const it = item(x);
      const ver = x.versions.find((v) => v.version === it.compatible) ?? x.versions[0];
      return { ...it, homepage: x.homepage, permissions: { unsafe: x.unsafe, filesystem: x.filesystem }, content: x.kind === "content",
        dependencies: x.dependencies, conflicts: x.conflicts,
        screenshots: x.screenshots.map((sh) => ({ ...sh, ready: s.shots > 0 && s.shotsFetched.has(x.id) })),
        versions: x.versions.map((v) => ({ ...v })), dependants: s.plugins.filter((o) => o.installed && o.dependencies.some((d) => d.id === x.id)).map((o) => o.id),
        conflictsEnabled: [], plan: [{ id: x.id, version: ver.version }], planError: "" };
    },
    "store.install": (p) => {
      const x = guard(p.id);
      const version = p.version ?? item(x).compatible;
      if (!version) throw [-32000, item(x).reason || "no compatible version"];
      if (x.installed && !x.installed.managed && p.replaceManual !== true) throw [-32000, `Mods\\${x.id} was installed by hand; confirm to replace it`];
      delete s.rowError[x.id];
      s.busy = true;
      later(5, () => runInstall(x, version));
      return { started: true };
    },
    "store.update": (p) => {
      const x = guard(p.id);
      if (!x.installed?.managed) throw [-32000, `${x.id} was not installed from the Store`];
      s.busy = true;
      later(5, () => runInstall(x, item(x).compatible));
      return { started: true };
    },
    "store.remove": (p) => {
      const x = guard(p.id);
      if (!x.installed?.managed) throw [-32000, `${x.id} was not installed from the Store`];
      s.busy = true;
      s.lastRemove = { id: x.id, deleteData: p.deleteData === true };
      s.job = { phase: "removing", id: x.id, version: "", bytes: 0, total: 0, message: "" };
      push();
      later(40, () => { x.installed = null; finish("done", x.id, "", "Removed"); });
      return { started: true };
    },
    "store.cancel": () => {
      if (!s.busy || s.job.phase !== "downloading") return { cancelled: false };
      s.job = { ...s.job, phase: "cancelled" };
      return { cancelled: true };
    },
    "store.openHomepage": (p) => {
      const x = find(p.id);
      if (!x?.homepage) throw [-32000, "the plugin has no https:// homepage"];
      s.opened.push(x.homepage);
      return {};
    },
  };

  return {
    methods: Object.keys(handlers),
    mutating: ["store.install", "store.update", "store.remove", "store.cancel"],
    handlers,
    initial: () => event(),
    // GET /store/shots/<id>/<n>
    route: (pathname) => {
      const m = /^\/store\/shots\/([a-z0-9_-]+)\/([1-6])$/.exec(pathname);
      if (!m) return undefined;
      const x = find(m[1]);
      if (!x || Number(m[2]) > x.screenshots.length || !s.shotsFetched.has(x.id) || !s.shots) return null;
      return PNG;
    },
    close: () => { for (const t of s.timers) clearTimeout(t); s.timers.clear(); },
  };
}
