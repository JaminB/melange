// The launcher's setup.*, plugins.*, defaults.*, recommended.*, update.* and launcher.* methods and the `setup` and
// `update` channels, for the mock server (--launcher). Scenario fixtures (picked by ?scenario=, see startMock): fresh,
// not-found, wrong-build, ual-present, reshade, restore, update-ready, update-running. No real file or registry access:
// everything lives in `state.launcher`.
const GAME_PATH = "C:\\Games\\WormsMayhem";
const OK_EXE = { size: 5713408, timestamp: 1367508505, sha256: "041c8c6eb3b9f4fbaf367748f713ccb8f7bef68d13e825472c88c1ecf711ab7d", build: "Steam/GOG #1077" };
const BAD_EXE = { size: 5820224, timestamp: 1420000000, sha256: "b".repeat(64) };
const UAL_DLL = { file: "dinput8.dll", sha256: "ec2f4824eca58dd40f425756a4a7cec77b8e381f21d11d7c846ec4b339b617ab", size: 139264,
  product: "Ultimate ASI Loader", company: "ThirteenAG", description: "Ultimate ASI Loader", version: "9.7.4", kind: "ual", known: true };
const RESHADE_DLL = { file: "dinput8.dll", sha256: "c".repeat(64), size: 1540096, product: "ReShade", company: "Crosire",
  description: "ReShade 6.3.0", version: "6.3.0", kind: "reshade", known: true, ours: false };

const okCheck = (path, extra = {}) => ({ path, verdict: "ok", store: "steam", running: false, writable: true, exe: OK_EXE, ...extra });

const SUNSTONE_DECL = [{ key: "quality", type: "enum", label: "Quality", default: "bold", options: ["off", "low", "subtle", "bold", "ultra"],
  optionLabels: { bold: { label: "Bold", help: "The full look. Recommended." } } }];
const SUNSTONE = { id: "sunstone", name: "Sunstone", description: "Graphics overhaul.", why: "Sharper, richer graphics. Client-only: it never affects other players.",
  settings: { quality: "bold" }, installed: false, compatible: true, decl: SUNSTONE_DECL };

function freshStatus() {
  return {
    game: okCheck(GAME_PATH), running: false, melangeLoaded: false,
    loader: { state: "none" }, otherLoaders: [],
    melange: { state: "missing", duplicates: [] },
    ini: { present: false, missingKeys: 0 }, legacy: [],
    payload: { ok: true, version: "0.4.0", missing: [], fromGameFolder: false },
    backups: [],
  };
}

function scenarioFixture(name) {
  switch (name) {
    case "not-found":
      return { candidates: [], status: freshStatus() };
    case "wrong-build":
      return { candidates: [{ path: GAME_PATH, source: "steam", library: "C:\\Program Files (x86)\\Steam",
        check: { path: GAME_PATH, verdict: "wrongBuild", store: "steam", running: false, writable: true, exe: BAD_EXE } }],
        status: { ...freshStatus(), game: { path: GAME_PATH, verdict: "wrongBuild", store: "steam", running: false, writable: true, exe: BAD_EXE } } };
    case "ual-present":
      return { candidates: base(), status: { ...freshStatus(), loader: { state: "ual", dll: { ...UAL_DLL, ours: false } } } };
    case "reshade":
      return { candidates: base(), status: { ...freshStatus(), loader: { state: "other", dll: RESHADE_DLL } } };
    case "restore":
      return {
        candidates: base(),
        status: {
          ...freshStatus(),
          loader: { state: "ual", dll: { ...UAL_DLL, ours: true } },
          melange: { state: "installed", version: "0.4.0", path: `${GAME_PATH}\\melange.asi`, duplicates: [], lastLoad: { at: "3 Oct, 14:02", version: "0.4.0" } },
          ini: { present: true, missingKeys: 0 },
          backups: [{ id: "20261003-142107-install", created: "2026-10-03 14:21", action: "install",
            files: [{ path: "dinput8.dll", op: "replaced", description: "ReShade 6.3.0" }] }],
          install: { melange: "0.4.0", installedAt: "2026-10-03T14:21:07Z", loader: "replaced" },
        },
        firstRun: false,
      };
    case "plugins-busy": {
      const r = scenarioFixture("restore");
      return { ...r, status: { ...r.status, busy: { action: "recommended", step: 1, of: 2, label: "Installing Sunstone…" } } };
    }
    case "update-ready": {
      const r = scenarioFixture("restore");
      return { ...r, update: { current: "0.4.0", phase: "ready", latest: "0.4.1", htmlUrl: "https://github.com/JaminB/melange/releases/tag/v0.4.1" } };
    }
    case "update-running": {
      const r = scenarioFixture("update-ready");
      return { ...r, status: { ...r.status, running: true, game: { ...r.status.game, running: true } } };
    }
    case "fresh":
    default:
      return { candidates: base(), status: freshStatus() };
  }
}
function base() {
  return [{ path: GAME_PATH, source: "steam", library: "C:\\Program Files (x86)\\Steam", check: okCheck(GAME_PATH) }];
}

function planFor(status, action, opts) {
  if (action === "uninstall") {
    const steps = [];
    if (status.melange.state !== "missing") steps.push({ op: "remove", path: "melange.asi", detail: "" });
    if (status.install?.loader === "added") steps.push({ op: "remove", path: "dinput8.dll", detail: "Melange added this loader" });
    if (opts.removeData) steps.push({ op: "remove", path: "Melange.ini", detail: "" });
    return { planId: `uninstall-${steps.length}-${opts.removeData ? 1 : 0}`, steps };
  }
  const steps = [];
  let needsChoice;
  if (status.loader.state === "none") steps.push({ op: "add", path: "dinput8.dll", detail: "Ultimate ASI Loader" });
  else if (status.loader.state === "other") {
    if (!opts.replaceLoader) needsChoice = "loader";
    else { steps.push({ op: "backup", path: "dinput8.dll", detail: status.loader.dll?.description ?? "" }); steps.push({ op: "replace", path: "dinput8.dll", detail: "Ultimate ASI Loader" }); }
  }
  steps.push({ op: status.melange.state === "missing" ? "add" : "replace", path: "melange.asi", detail: "0.4.0" });
  steps.push({ op: status.ini.present ? "merge" : "add", path: "Melange.ini", detail: "" });
  for (const f of status.legacy) steps.push({ op: "remove", path: f, detail: "backed up" });
  return { planId: `${action}-${steps.length}-${opts.replaceLoader ? 1 : 0}`, steps, needsChoice };
}

export function launcherService(state, broadcast, initialScenario) {
  const l = state.launcher = {
    version: "0.4.0", theme: "system", elevated: false, scenario: initialScenario || "fresh",
    shortcuts: { startMenu: false, desktop: false }, defaults: { plugins: [], seeded: false }, timers: new Set(),
  };
  const apply = (name) => {
    const f = scenarioFixture(name);
    l.scenario = name;
    l.candidates = f.candidates.map((c) => ({ ...c }));
    l.status = JSON.parse(JSON.stringify(f.status));
    l.gameDir = f.firstRun === false ? GAME_PATH : null;
    l.firstRun = f.firstRun !== false;
    l.update = { current: l.version, phase: "current", latest: l.version, auto: true, ...(f.update ?? {}) };
    l.checkInGame = true;   // what update.setAuto writes to Melange.ini's [Update] CheckInGame
    l.updateApplied = false;
  };
  apply(l.scenario);

  const later = (ms, fn) => { const t = setTimeout(() => { l.timers.delete(t); fn(); }, ms); l.timers.add(t); };
  const publicStatus = () => JSON.parse(JSON.stringify(l.status));
  const pushStatus = () => broadcast("setup", { status: publicStatus() });
  const updateStatus = () => JSON.parse(JSON.stringify(l.update));

  const findCandidate = (path) => l.candidates.find((c) => c.path === path);
  // Mirrors the real server: setup.*/plugins.setSettings refuse with -32002 while a recommended-plugins batch
  // holds the setup lock (l.status.busy), naming what is busy instead of a plain "try again".
  const busyGuard = () => { if (l.status.busy) throw [-32002, `Installing plugins — this finishes in a moment. ${l.status.busy.label}`]; };

  const handlers = {
    "launcher.state": () => ({ version: l.version, firstRun: l.firstRun, gameDir: l.gameDir, theme: l.theme, webview: false, elevated: l.elevated, protected: [] }),
    "launcher.setTheme": (p) => { l.theme = p.theme === "light" || p.theme === "dark" ? p.theme : "system"; return {}; },
    "launcher.launch": () => ({ how: l.status.game?.store === "steam" ? "steam" : "exe" }),
    "launcher.openPath": (p) => {
      if (p.what === "export" && !l.exported) throw [-32000, "That export isn't there any more."];
      return {};
    },
    "launcher.exportLogs": () => {
      l.exported = true;
      return { path: "C:\\Users\\Player\\Desktop\\Melange-logs-20261005-121500.zip", bytes: 482133, entries: 23,
        onDesktop: true, sessionId: "2026-10-05_11-40-02_pid4242", pid: 4242 };
    },
    "launcher.shortcuts": (p) => { l.shortcuts = { startMenu: !!p.startMenu, desktop: !!p.desktop }; return {}; },
    "setup.detect": () => ({ candidates: l.candidates }),
    "setup.browse": () => ({ path: l.candidates[0]?.path ?? GAME_PATH }),
    "setup.validate": (p) => {
      const c = findCandidate(p.path);
      if (c) return c.check;
      if (p.path === GAME_PATH) { const check = okCheck(GAME_PATH); l.candidates = [{ path: GAME_PATH, source: "manual", check }]; return check; }
      return { path: p.path, verdict: "notFound", store: "unknown", running: false, writable: false };
    },
    "setup.select": (p) => {
      busyGuard();
      const c = findCandidate(p.path);
      if (c) l.status.game = c.check;
      if (p.save) l.gameDir = p.path;
      return publicStatus();
    },
    "setup.status": () => publicStatus(),
    "setup.plan": (p) => planFor(l.status, p.action, p),
    "setup.apply": (p) => {
      busyGuard();
      const plan = planFor(l.status, p.action, p);
      if (p.planId !== plan.planId) throw [-32013, "What will change has changed.", undefined];
      return new Promise((resolve) => {
        let i = 0;
        const steps = plan.steps.length || 1;
        const tick = () => {
          i++;
          broadcast("setup", { progress: { action: p.action, step: i, of: steps, label: plan.steps[i - 1] ? `${plan.steps[i - 1].op} ${plan.steps[i - 1].path}` : "Finishing…" } });
          if (i < steps) { later(25, tick); return; }
          later(25, () => {
            let backupId;
            if (p.action === "uninstall") {
              l.status.melange = { state: "missing", duplicates: [] };
              delete l.status.install;
            } else {
              const replacing = l.status.loader.state === "other" && p.replaceLoader;
              if (l.status.loader.state === "none" || replacing) {
                if (replacing) {
                  backupId = `${new Date().toISOString().slice(0, 10).replace(/-/g, "")}-000001-install`;
                  l.status.backups = [{ id: backupId, created: "just now", action: "install", files: [{ path: "dinput8.dll", op: "replaced", description: l.status.loader.dll?.description }] }, ...l.status.backups];
                }
                l.status.loader = { state: "ual", dll: { ...UAL_DLL, ours: true } };
              }
              l.status.melange = { state: "installed", version: "0.4.0", path: `${l.status.game?.path ?? GAME_PATH}\\melange.asi`, duplicates: [] };
              l.status.ini = { present: true, missingKeys: 0 };
              l.status.legacy = [];
              l.status.install = { melange: "0.4.0", installedAt: new Date().toISOString(), loader: replacing ? "replaced" : l.status.loader.state === "ual" ? "reused" : "added" };
            }
            pushStatus();
            resolve({ ok: true, backupId, status: publicStatus() });
          });
        };
        tick();
      });
    },
    "setup.restore": (p) => {
      busyGuard();
      const b = l.status.backups.find((x) => x.id === p.backupId);
      if (!b) throw [-32602, "unknown backup"];
      l.status.loader = { state: "other", dll: RESHADE_DLL };
      if (l.status.install) l.status.install.loader = "other-name";
      pushStatus();
      return { ok: true, status: publicStatus() };
    },
    "setup.deleteBackup": (p) => { busyGuard(); l.status.backups = l.status.backups.filter((b) => b.id !== p.backupId); pushStatus(); return {}; },
    "setup.setMelangeEnabled": (p) => {
      busyGuard();
      if (l.status.melange.state !== "missing") l.status.melange.state = p.on ? "installed" : "disabled";
      pushStatus();
      return publicStatus();
    },
    "setup.elevate": () => { l.elevated = true; return {}; },
    "plugins.settings": () => ({ decl: SUNSTONE_DECL, values: { quality: "bold" }, defaults: { quality: "bold" } }),
    "plugins.setSettings": (p) => { busyGuard(); return { values: p.values }; },
    "plugins.resetSettings": () => ({ values: { quality: "bold" } }),
    "defaults.get": () => l.defaults,
    "defaults.set": (p) => { l.defaults = { plugins: Array.isArray(p.plugins) ? p.plugins : [], seeded: true }; return l.defaults; },
    "update.status": () => updateStatus(),
    "update.check": () => updateStatus(),
    "update.setAuto": (p) => {
      if (typeof p?.on !== "boolean") throw [-32602, "expected {on}"];
      busyGuard();
      l.update.auto = l.checkInGame = p.on;
      broadcast("update", { status: updateStatus() });
      return updateStatus();
    },
    "update.apply": () => {
      if (l.update.phase !== "ready") throw [-32000, "No update is ready yet."];
      if (l.status.running) throw [-32000, "Close the game to update."];
      l.updateApplied = true;
      return {};
    },
    "recommended.get": () => (l.scenario === "offline-store" ? { source: "builtin", items: [] } : { source: "index", items: [SUNSTONE] }),
    "recommended.apply": (p) => {
      l.defaults = { plugins: (p.items ?? []).map((it) => ({ id: it.id, enabled: true, settings: it.settings ?? {} })), seeded: true };
      return { queued: (p.items ?? []).map((it) => it.id) };
    },
  };

  return {
    methods: Object.keys(handlers),
    mutating: ["launcher.setTheme", "launcher.shortcuts", "setup.select", "setup.apply", "setup.restore", "setup.deleteBackup",
      "setup.setMelangeEnabled", "plugins.setSettings", "plugins.resetSettings", "defaults.set", "recommended.apply", "update.apply", "update.setAuto"],
    handlers,
    status: publicStatus,
    updateStatus,
    setScenario: apply,
    close: () => { for (const t of l.timers) clearTimeout(t); l.timers.clear(); },
  };
}
