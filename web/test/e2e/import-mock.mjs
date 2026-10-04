// The local content importer's import.* methods, `import` channel and preview route for the mock server
// (--launcher), over one synthetic plugin "caravan" (a stand-in recipe/content; no real mod data). Scenarios (picked
// by ?import=, see startMock): fresh (status "none"), imported, stale, damaged, hash-error (the job always fails
// with reason "hash").
import { deflateSync } from "node:zlib";

// A 4x3 RGB PNG, built here so the fixture holds no binary (same recipe as store-mock.mjs's).
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
  for (let y = 0; y < h; y++) rows.push(Buffer.from([0, ...Array.from({ length: w }, () => [90, 150, 90]).flat()]));
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr), chunk("IDAT", deflateSync(Buffer.concat(rows))),
    chunk("IEND", Buffer.alloc(0))]);
}
const PNG = png();

const CONTENT = {
  title: "Fixture Mod 0.1", publisher: "fixture.example", termsUrl: "https://fixture.example/terms",
  credit: "Maps by their original authors, collected by the fixture.example community.",
};
const SOURCE = { id: "mirror", name: "fixture.example", host: "fixture.example", fileName: "FixtureMod_0.1.zip", size: 15000000, sha256: "f".repeat(64) };
const RECIPE_VERSION = "1.0.0";

function fixtureMaps() {
  const row = (file, group, groupLabel, category, categoryLabel, extra = {}) => ({
    file, stem: file.toLowerCase(), pack: category === "mode" ? "caravan-2" : "caravan-1", title: extra.title ?? file,
    author: extra.author, group, groupLabel, category, categoryLabel, mode: extra.mode, theme: extra.theme ?? "ARABIAN",
    timeOfDay: extra.timeOfDay ?? "DAY", survivor: extra.survivor !== false, hidden: category === "mode", preview: extra.preview !== false,
  });
  return [
    row("Alpine", "mmp", "Mega Map Pack", "play", "Plays as designed", { author: "mapper1", theme: "ARCTIC" }),
    row("Boulder", "mmp", "Mega Map Pack", "play", "Plays as designed", { author: "mapper2" }),
    row("Caldera", "mmp", "Mega Map Pack", "play", "Plays as designed", { theme: "LUNAR", timeOfDay: "NIGHT" }),
    row("Delta", "mmp", "Mega Map Pack", "play", "Plays as designed", {}),
    row("Estuary", "mmp", "Mega Map Pack", "play", "Plays as designed", { preview: false }),
    row("Foxhole", "mmp", "Mega Map Pack", "play", "Plays as designed", { theme: "WAR" }),
    row("W3D_Garden", "w3d", "Worms 3D ports", "play", "Plays as designed", { title: "Garden", author: "team17" }),
    row("balloon", "vanilla", "Standard maps", "play", "Plays as designed", { title: "Balloon", survivor: false }),
    row("re_Harbour", "renewation", "Fixture Mod", "dm", "Deathmatch only", { title: "Harbour", theme: "PIRATE" }),
    row("re_Isthmus", "renewation", "Fixture Mod", "dm", "Deathmatch only", { title: "Isthmus" }),
    row("re_Jungle_PRO", "renewation", "Fixture Mod", "mode", "Mode not supported", { title: "Jungle", mode: "Pro", theme: "PREHISTORIC" }),
    row("re_Keep_CS", "renewation", "Fixture Mod", "mode", "Mode not supported", { title: "Keep", mode: "Castle siege", theme: "CAMELOT" }),
  ];
}
function fixturePacks(maps) {
  const count = (pack) => maps.filter((m) => m.pack === pack).length;
  return [
    { id: "caravan-1", name: "Caravan maps 1", enabled: true, levels: count("caravan-1"), category: ["play", "dm"] },
    { id: "caravan-2", name: "Caravan maps 2", enabled: true, levels: count("caravan-2"), category: ["mode"] },
  ];
}
function counts(maps) {
  return { play: maps.filter((m) => m.category === "play").length, dm: maps.filter((m) => m.category === "dm").length,
    mode: maps.filter((m) => m.category === "mode").length, skipped: 0 };
}

function scenarioFixture(name) {
  const maps = fixtureMaps();
  const packs = fixturePacks(maps);
  const base = { plugin: "caravan", name: "Caravan", recipe: "fixture-mod-0.1", recipeVersion: RECIPE_VERSION, format: 1, supported: true,
    content: CONTENT, sources: [SOURCE], expect: { maps: maps.length }, gate: "" };
  const imported = { recipeVersion: RECIPE_VERSION, fingerprint: "a1b2c3d4e5f6", importedAt: "2026-10-01T12:00:00Z",
    maps: maps.length, counts: counts(maps), packs, bytes: 2345678 };
  switch (name) {
    case "imported":
      return { ...base, status: "imported", imported, zip: { bytes: SOURCE.size, verified: true }, maps, packs };
    case "stale":
      return { ...base, status: "stale", imported: { ...imported, recipeVersion: "0.9.0" }, zip: { bytes: SOURCE.size, verified: true }, maps, packs };
    case "damaged":
      return { ...base, status: "damaged", statusReason: "caravan-2 is missing", imported, maps, packs };
    case "unsupported":
      return { ...base, status: "unsupported", supported: false, statusReason: "this recipe needs format 2", maps: [], packs: [] };
    case "hash-error":
    case "fresh":
    default:
      return { ...base, status: "none", maps, packs };
  }
}

export function importService(state, broadcast) {
  const im = state.imports = { scenario: "fresh", timers: new Set() };
  const apply = (name) => { im.scenario = name; im.fixture = scenarioFixture(name); im.job = undefined; im.lastBrowsed = undefined; };
  apply(im.scenario);

  const later = (ms, fn) => { const t = setTimeout(() => { im.timers.delete(t); fn(); }, ms); im.timers.add(t); };

  const ACTIVE_PHASES = ["downloading", "copying", "verifying", "reading", "building", "placing"];
  const publicImporter = () => {
    const f = im.fixture;
    const busy = !!im.job && ACTIVE_PHASES.includes(im.job.phase);
    const out = { plugin: f.plugin, name: f.name, recipe: f.recipe, recipeVersion: f.recipeVersion, format: f.format, supported: f.supported,
      content: f.content, sources: f.sources, expect: f.expect, status: busy ? "busy" : f.status, statusReason: f.statusReason,
      gate: f.gate };
    if (f.imported) out.imported = f.imported;
    if (f.zip) out.zip = f.zip;
    if (im.job) out.job = { ...im.job };
    return JSON.parse(JSON.stringify(out));
  };
  const pushImporters = () => broadcast("import", { importers: [publicImporter()] });
  const pushJob = () => { if (im.job) broadcast("import", { job: { ...im.job } }); };

  const guardGame = () => { /* the fixture never gates on game state; a real server would check noGame/gameRunning here */ };

  const runImport = (source, keepZip) => {
    const maps = im.fixture.maps, total = source.kind === "download" ? SOURCE.size : 9000000;
    let i = 0;
    const steps = 4;
    const acquirePhase = source.kind === "download" ? "downloading" : "copying";
    const tick = () => {
      if (im.job?.cancelRequested) { im.job = { plugin: "caravan", phase: "cancelled", bytes: 0, total: 0, step: 0, of: 0 }; pushJob(); later(5, pushImporters); return; }
      if (i <= steps) {
        im.job = { plugin: "caravan", phase: acquirePhase, bytes: Math.round((total * i) / steps), total, step: 0, of: 0 };
        pushJob();
        i++;
        return later(25, tick);
      }
      im.job = { plugin: "caravan", phase: "verifying", bytes: total, total, step: 0, of: 0 };
      pushJob();
      later(25, () => {
        if (im.scenario === "hash-error") {
          im.job = { plugin: "caravan", phase: "error", bytes: 0, total: 0, step: 0, of: 0, reason: "hash" };
          pushJob();
          later(5, pushImporters);
          return;
        }
        im.job = { plugin: "caravan", phase: "reading", bytes: 0, total: 0, step: 0, of: 0 };
        pushJob();
        later(25, () => {
          let s = 0;
          const build = () => {
            s++;
            im.job = { plugin: "caravan", phase: "building", bytes: 0, total: 0, step: s, of: maps.length };
            pushJob();
            if (s < maps.length) return later(10, build);
            later(20, () => {
              im.job = { plugin: "caravan", phase: "placing", bytes: 0, total: 0, step: 0, of: 0 };
              pushJob();
              later(25, () => {
                const packs = fixturePacks(maps);
                im.fixture.status = "imported";
                im.fixture.imported = { recipeVersion: RECIPE_VERSION, fingerprint: "a1b2c3d4e5f6", importedAt: new Date().toISOString(),
                  maps: maps.length, counts: counts(maps), packs, bytes: 2345678 };
                im.fixture.zip = keepZip ? { bytes: SOURCE.size, verified: true } : undefined;
                im.fixture.maps = maps;
                im.fixture.packs = packs;
                im.job = { plugin: "caravan", phase: "done", bytes: 0, total: 0, step: 0, of: 0,
                  result: { maps: maps.length, counts: counts(maps), packs: packs.map((p) => p.id), bytes: 2345678, fingerprint: "a1b2c3d4e5f6", skipped: 0 } };
                pushJob();
                later(5, pushImporters);
              });
            });
          };
          build();
        });
      });
    };
    tick();
  };

  const handlers = {
    "import.list": () => ({ importers: [publicImporter()] }),
    "import.status": (p) => { if (p.plugin !== "caravan") throw [-32602, `no importer for '${p.plugin}'`]; return publicImporter(); },
    "import.browse": () => { im.lastBrowsed = { path: "C:\\Downloads\\MyFixtureCopy.zip", name: "MyFixtureCopy.zip", size: SOURCE.size }; return im.lastBrowsed; },
    "import.start": (p) => {
      guardGame();
      if (im.job && !["done", "error", "cancelled"].includes(im.job.phase)) throw [-32002, "an import is already running"];
      if (p.accepted !== true) throw [-32000, "the disclosure has not been accepted", JSON.stringify({ reason: "notAccepted" })];
      if (p.source?.kind === "file" && (!im.lastBrowsed || p.source.path !== im.lastBrowsed.path))
        throw [-32000, "that path was not chosen with Browse", JSON.stringify({ reason: "badPath" })];
      im.job = { plugin: "caravan", phase: "downloading", bytes: 0, total: 0, step: 0, of: 0 };
      later(5, () => runImport(p.source, p.keepZip !== false));
      return { started: true };
    },
    "import.cancel": () => {
      if (!im.job || ["done", "error", "cancelled"].includes(im.job.phase)) return { cancelling: false };
      im.job.cancelRequested = true;
      return { cancelling: true };
    },
    "import.maps": (p) => ({ maps: im.fixture.maps ?? [], packs: im.fixture.packs ?? [] }),
    "import.setHidden": (p) => {
      const hidden = new Set(im.fixture.maps.filter((m) => m.hidden).map((m) => m.file));
      for (const f of p.files ?? []) { if (p.hidden) hidden.add(f); else hidden.delete(f); }
      im.fixture.maps = im.fixture.maps.map((m) => ({ ...m, hidden: hidden.has(m.file) }));
      return { hidden: hidden.size };
    },
    "import.setPacks": (p) => {
      const wanted = new Map((p.packs ?? []).map((x) => [x.id, x.enabled]));
      im.fixture.packs = im.fixture.packs.map((pk) => (wanted.has(pk.id) ? { ...pk, enabled: wanted.get(pk.id) } : pk));
      return { packs: im.fixture.packs };
    },
    "import.uninstall": (p) => {
      const removed = (im.fixture.packs ?? []).map((pk) => pk.id);
      im.fixture.status = "none";
      im.fixture.imported = undefined;
      im.fixture.maps = fixtureMaps();
      im.fixture.packs = fixturePacks(im.fixture.maps);
      if (p.deleteZip) im.fixture.zip = undefined;
      later(5, pushImporters);
      return { removed };
    },
    "import.deleteZip": () => { const freed = im.fixture.zip?.bytes ?? 0; im.fixture.zip = undefined; later(5, pushImporters); return { freed }; },
  };

  return {
    methods: Object.keys(handlers),
    mutating: ["import.start", "import.cancel", "import.setHidden", "import.setPacks", "import.uninstall", "import.deleteZip"],
    handlers,
    initial: () => ({ importers: [publicImporter()] }),
    setScenario: apply,
    // GET /import/previews/<plugin>/<stem>.png — only for a map this fixture marks `preview: true`.
    route: (pathname) => {
      const m = /^\/import\/previews\/([a-z0-9_-]+)\/([a-z0-9_]+)\.png$/.exec(pathname);
      if (!m || m[1] !== "caravan") return undefined;
      const map = (im.fixture.maps ?? []).find((x) => x.stem === m[2]);
      return map?.preview ? PNG : null;
    },
    close: () => { for (const t of im.timers) clearTimeout(t); im.timers.clear(); },
  };
}
