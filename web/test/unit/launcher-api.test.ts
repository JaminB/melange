import assert from "node:assert/strict";
import { test } from "node:test";
import {
  candidatesOf, defaultsOf, gameCheckOf, launcherStateOf, planOf, recommendedOf, setupStatusOf, settingOf, sizeText, valuesOf,
} from "../../src/launcher/api";

test("launcherStateOf: garbage input never throws and falls back sanely", () => {
  assert.deepEqual(launcherStateOf(null), { version: "", firstRun: false, gameDir: null, theme: "system", webview: false, elevated: false, protected: [] });
  const s = launcherStateOf({ version: "0.4.0", firstRun: true, gameDir: "C:\\WUM", theme: "dark", webview: true, elevated: true, protected: ["C:\\WUM"] });
  assert.equal(s.theme, "dark");
  assert.equal(s.gameDir, "C:\\WUM");
});

test("gameCheckOf: unknown verdict falls back to notFound", () => {
  assert.equal(gameCheckOf({ verdict: "nonsense" }).verdict, "notFound");
  const ok = gameCheckOf({ path: "p", verdict: "ok", store: "gog", running: true, writable: true, exe: { size: 1, timestamp: 2, sha256: "a", build: "b" } });
  assert.equal(ok.store, "gog");
  assert.equal(ok.exe?.sha256, "a");
});

test("candidatesOf: skips malformed entries but keeps good ones", () => {
  const c = candidatesOf([{ path: "a", source: "steam", check: { verdict: "ok" } }, { path: "b", source: "xyz" }, { path: 5 }, "nope"]);
  assert.equal(c.length, 2);
  assert.equal(c[0].path, "a");
  assert.equal(c[1].source, "manual", "an unrecognised source falls back to manual");
});

test("setupStatusOf: round-trips a full status and defaults missing fields", () => {
  const s = setupStatusOf({
    game: { path: "p", verdict: "ok" }, running: false, melangeLoaded: true,
    loader: { state: "ual", dll: { file: "dinput8.dll", sha256: "x", size: 1, kind: "ual", known: true, ours: true } },
    otherLoaders: [], melange: { state: "installed", version: "0.4.0", duplicates: [], lastLoad: { at: "now", version: "0.4.0" } },
    ini: { present: true, missingKeys: 2 }, legacy: ["WUMFix.asi"],
    payload: { ok: true, version: "0.4.0", missing: [], fromGameFolder: false },
    backups: [{ id: "1", created: "x", action: "install", files: [{ path: "dinput8.dll", op: "replaced" }] }],
    install: { melange: "0.4.0", installedAt: "x", loader: "added" },
  });
  assert.equal(s.loader.state, "ual");
  assert.equal(s.melange.lastLoad?.at, "now");
  assert.equal(s.backups[0].files[0].op, "replaced");
  assert.equal(s.install?.loader, "added");
  const empty = setupStatusOf({});
  assert.equal(empty.game, null);
  assert.equal(empty.melange.state, "missing");
});

test("planOf: unknown op falls back to keep", () => {
  const p = planOf({ planId: "x", steps: [{ op: "bogus", path: "a", detail: "" }], needsChoice: "loader" });
  assert.equal(p.steps[0].op, "keep");
  assert.equal(p.needsChoice, "loader");
});

test("settingOf and valuesOf", () => {
  const s = settingOf({ key: "quality", type: "enum", label: "Quality", default: "bold", options: ["low", "bold"], optionLabels: { bold: { label: "Bold" } } });
  assert.equal(s?.optionLabels?.bold.label, "Bold");
  assert.equal(settingOf({ type: "bool" }), undefined, "a setting without a key is dropped");
  assert.deepEqual(valuesOf({ a: 1, b: "x", c: true, d: { nested: true } }), { a: 1, b: "x", c: true, d: "" });
});

test("defaultsOf and recommendedOf", () => {
  const d = defaultsOf({ plugins: [{ id: "sunstone", enabled: true, settings: { quality: "bold" } }], seeded: true });
  assert.equal(d.plugins[0].id, "sunstone");
  const r = recommendedOf({ source: "builtin", items: [{ id: "sunstone", settings: { quality: "bold" }, compatible: true }] });
  assert.equal(r.source, "builtin");
  assert.equal(r.items[0].name, "sunstone", "name falls back to id");
  assert.equal(recommendedOf(null).items.length, 0);
});

test("sizeText", () => {
  assert.equal(sizeText(500), "500 B");
  assert.equal(sizeText(2048), "2 KiB");
  assert.equal(sizeText(5 * 1048576), "5.0 MiB");
});
