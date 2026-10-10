// The launcher RPC contract (spec §6.3). Kept in sync with src/launcher/setup/*.h by hand; the web unit tests
// exercise every shape this file parses from `unknown`, exactly as the panels do for their own methods.
export type Verdict = "ok" | "wrongBuild" | "noExe" | "notFound" | "unreadable";
export type Source = "saved" | "self" | "steam" | "gog" | "manual";
export type Store = "steam" | "gog" | "unknown";
export type DllKind = "ual" | "microsoft" | "reshade" | "specialk" | "other";
export type MelangeState = "missing" | "installed" | "disabled" | "older" | "newer" | "damaged";
export type LoaderState = "none" | "ual" | "other";
export type PlanAction = "install" | "repair" | "uninstall";
export type PlanOp = "add" | "replace" | "remove" | "backup" | "merge" | "keep";
export type Theme = "system" | "light" | "dark";
export type Val = boolean | number | string;

export interface LauncherState {
  version: string; firstRun: boolean; gameDir: string | null; theme: Theme;
  webview: boolean; elevated: boolean; protected: string[];
  resume?: string;   // after an elevated restart (setup.elevate {resume}): what to pick up again, e.g. "vanilla"
}

export interface GameCheck {
  path: string; verdict: Verdict; store: Store; running: boolean; writable: boolean;
  exe?: { size: number; timestamp: number; sha256?: string; build?: string };
  error?: string;
}

export interface Candidate { path: string; source: Source; library?: string; check: GameCheck; }

export interface DllInfo {
  file: string; sha256: string; size: number; product?: string; company?: string; description?: string;
  version?: string; kind: DllKind; known: boolean; ours: boolean;
}

export interface SetupStatus {
  game: GameCheck | null; running: boolean; melangeLoaded: boolean;
  loader: { state: LoaderState; dll?: DllInfo };
  otherLoaders: DllInfo[];
  melange: { state: MelangeState; version?: string; path?: string; duplicates: string[]; lastLoad?: { at: string; version: string } };
  ini: { present: boolean; missingKeys: number };
  legacy: string[];
  payload: { ok: boolean; version: string; missing: string[]; fromGameFolder: boolean };
  backups: { id: string; created: string; action: string; files: { path: string; op: string; description?: string }[] }[];
  install?: { melange: string; installedAt: string; loader: "added" | "reused" | "replaced" | "other-name" };
  // A recommended-plugins batch (or other long transaction) holding the setup lock right now, if any: setup.select,
  // setup.apply, setup.restore, setup.deleteBackup, setup.setMelangeEnabled and plugins.setSettings all return
  // -32002 while this is set, so the UI should disable them and show `label` instead of letting the call fail.
  busy?: SetupProgress;
}

export interface PlanRequest { action: PlanAction; replaceLoader?: boolean; allowDowngrade?: boolean; removeData?: boolean; }
export interface PlanStep { op: PlanOp; path: string; detail: string; }
export interface Plan { planId: string; steps: PlanStep[]; needsChoice?: "loader"; refused?: string; }
export interface ApplyResult { ok: true; backupId?: string; status: SetupStatus; }

export interface Setting {
  key: string; type: "bool" | "int" | "float" | "string" | "enum"; label: string; default: Val;
  min?: number; max?: number; options?: string[]; help?: string; optionLabels?: Record<string, { label: string; help?: string }>;
}
export interface Defaults { plugins: { id: string; enabled: boolean; settings: Record<string, Val> }[]; seeded: boolean; }
export interface Recommended {
  id: string; name: string; description: string; why: string; settings: Record<string, Val>;
  installed: boolean; compatible: boolean; reason?: string; decl?: Setting[];
}

export interface SetupProgress { action: string; step: number; of: number; label: string; }
export interface SetupEvent { status?: SetupStatus; progress?: SetupProgress; }

// The folder picker is modal and waits on the user, so the default RPC timeout would drop their choice.
export const BROWSE_TIMEOUT_MS = 24 * 60 * 60 * 1000;

// launcher.exportLogs: the last game's logs zipped to the Desktop (else Documents\Melange\exports). sessionId is
// null when no session folder was found (the zip still holds Melange.log, launcher.log and the rest).
export interface ExportResult { path: string; bytes: number; entries: number; onDesktop: boolean; sessionId: string | null; pid: number; }
// Reading and deflating a long session's logs and recordings can take longer than an ordinary call.
export const EXPORT_TIMEOUT_MS = 5 * 60 * 1000;

// -- Error codes (spec §6.2) --------------------------------------------------------------------------------------
export const LauncherErrorCode = {
  Refused: -32000,
  Busy: -32002,
  BadParams: -32602,
  AccessDenied: -32010,
  PayloadMissing: -32011,
  Failed: -32012,
  PlanChanged: -32013,
} as const;

// -- Parsing: every value coming off the wire is `unknown` until checked here --------------------------------------
type Rec = Record<string, unknown>;
const obj = (v: unknown): Rec => (v && typeof v === "object" && !Array.isArray(v) ? (v as Rec) : {});
const str = (o: Rec, k: string): string => (typeof o[k] === "string" ? (o[k] as string) : "");
const strOpt = (o: Rec, k: string): string | undefined => (typeof o[k] === "string" ? (o[k] as string) : undefined);
const num = (o: Rec, k: string): number => (typeof o[k] === "number" && Number.isFinite(o[k]) ? (o[k] as number) : 0);
const numOpt = (o: Rec, k: string): number | undefined => (typeof o[k] === "number" && Number.isFinite(o[k]) ? (o[k] as number) : undefined);
const bool = (o: Rec, k: string): boolean => o[k] === true;
const strs = (o: Rec, k: string): string[] => (Array.isArray(o[k]) ? (o[k] as unknown[]).filter((x): x is string => typeof x === "string") : []);
const arr = (o: Rec, k: string): unknown[] => (Array.isArray(o[k]) ? (o[k] as unknown[]) : []);

const VERDICTS: Verdict[] = ["ok", "wrongBuild", "noExe", "notFound", "unreadable"];
const SOURCES: Source[] = ["saved", "self", "steam", "gog", "manual"];
const STORES: Store[] = ["steam", "gog", "unknown"];
const DLL_KINDS: DllKind[] = ["ual", "microsoft", "reshade", "specialk", "other"];
const MELANGE_STATES: MelangeState[] = ["missing", "installed", "disabled", "older", "newer", "damaged"];
const LOADER_STATES: LoaderState[] = ["none", "ual", "other"];
const PLAN_OPS: PlanOp[] = ["add", "replace", "remove", "backup", "merge", "keep"];

export function launcherStateOf(v: unknown): LauncherState {
  const o = obj(v);
  const theme = o.theme === "light" || o.theme === "dark" ? o.theme : "system";
  return { version: str(o, "version"), firstRun: bool(o, "firstRun"), gameDir: strOpt(o, "gameDir") ?? null, theme,
    webview: bool(o, "webview"), elevated: bool(o, "elevated"), protected: strs(o, "protected"),
    ...(typeof o.resume === "string" && o.resume ? { resume: o.resume } : {}) };
}

export function gameCheckOf(v: unknown): GameCheck {
  const o = obj(v);
  const exe = o.exe && typeof o.exe === "object" ? obj(o.exe) : undefined;
  return {
    path: str(o, "path"), verdict: VERDICTS.includes(o.verdict as Verdict) ? (o.verdict as Verdict) : "notFound",
    store: STORES.includes(o.store as Store) ? (o.store as Store) : "unknown", running: bool(o, "running"), writable: bool(o, "writable"),
    exe: exe ? { size: num(exe, "size"), timestamp: num(exe, "timestamp"), sha256: strOpt(exe, "sha256"), build: strOpt(exe, "build") } : undefined,
    error: strOpt(o, "error"),
  };
}

export function candidatesOf(v: unknown): Candidate[] {
  const out: Candidate[] = [];
  for (const c of Array.isArray(v) ? v : []) {
    const o = obj(c);
    if (typeof o.path !== "string" || !o.path) continue;
    out.push({ path: o.path, source: SOURCES.includes(o.source as Source) ? (o.source as Source) : "manual",
      library: strOpt(o, "library"), check: gameCheckOf(o.check) });
  }
  return out;
}

export function dllInfoOf(v: unknown): DllInfo | undefined {
  const o = obj(v);
  if (typeof o.file !== "string") return undefined;
  return { file: str(o, "file"), sha256: str(o, "sha256"), size: num(o, "size"), product: strOpt(o, "product"),
    company: strOpt(o, "company"), description: strOpt(o, "description"), version: strOpt(o, "version"),
    kind: DLL_KINDS.includes(o.kind as DllKind) ? (o.kind as DllKind) : "other", known: bool(o, "known"), ours: bool(o, "ours") };
}

export function setupStatusOf(v: unknown): SetupStatus {
  const o = obj(v);
  const loader = obj(o.loader);
  const melange = obj(o.melange);
  const ini = obj(o.ini);
  const payload = obj(o.payload);
  const install = o.install && typeof o.install === "object" ? obj(o.install) : undefined;
  const lastLoad = melange.lastLoad && typeof melange.lastLoad === "object" ? obj(melange.lastLoad) : undefined;
  return {
    game: o.game && typeof o.game === "object" ? gameCheckOf(o.game) : null,
    running: bool(o, "running"), melangeLoaded: bool(o, "melangeLoaded"),
    loader: { state: LOADER_STATES.includes(loader.state as LoaderState) ? (loader.state as LoaderState) : "none", dll: dllInfoOf(loader.dll) },
    otherLoaders: arr(o, "otherLoaders").map(dllInfoOf).filter((x): x is DllInfo => !!x),
    melange: { state: MELANGE_STATES.includes(melange.state as MelangeState) ? (melange.state as MelangeState) : "missing",
      version: strOpt(melange, "version"), path: strOpt(melange, "path"), duplicates: strs(melange, "duplicates"),
      lastLoad: lastLoad ? { at: str(lastLoad, "at"), version: str(lastLoad, "version") } : undefined },
    ini: { present: bool(ini, "present"), missingKeys: num(ini, "missingKeys") },
    legacy: strs(o, "legacy"),
    payload: { ok: bool(payload, "ok"), version: str(payload, "version"), missing: strs(payload, "missing"), fromGameFolder: bool(payload, "fromGameFolder") },
    backups: arr(o, "backups").map((b) => {
      const bo = obj(b);
      return { id: str(bo, "id"), created: str(bo, "created"), action: str(bo, "action"),
        files: arr(bo, "files").map((f) => { const fo = obj(f); return { path: str(fo, "path"), op: str(fo, "op"), description: strOpt(fo, "description") }; }) };
    }),
    install: install ? { melange: str(install, "melange"), installedAt: str(install, "installedAt"),
      loader: (["added", "reused", "replaced", "other-name"].includes(install.loader as string) ? install.loader : "added") as "added" | "reused" | "replaced" | "other-name" } : undefined,
    busy: o.busy && typeof o.busy === "object" ? (() => { const b = obj(o.busy); return { action: str(b, "action"), step: num(b, "step"), of: num(b, "of"), label: str(b, "label") }; })() : undefined,
  };
}

export function planOf(v: unknown): Plan {
  const o = obj(v);
  return { planId: str(o, "planId"), steps: arr(o, "steps").map((s) => {
    const so = obj(s);
    return { op: PLAN_OPS.includes(so.op as PlanOp) ? (so.op as PlanOp) : "keep", path: str(so, "path"), detail: str(so, "detail") };
  }), needsChoice: o.needsChoice === "loader" ? "loader" : undefined, refused: strOpt(o, "refused") };
}

export function applyResultOf(v: unknown): ApplyResult {
  const o = obj(v);
  return { ok: true, backupId: strOpt(o, "backupId"), status: setupStatusOf(o.status) };
}

export function settingOf(v: unknown): Setting | undefined {
  const o = obj(v);
  if (typeof o.key !== "string") return undefined;
  const types = ["bool", "int", "float", "string", "enum"];
  const optionLabels: Record<string, { label: string; help?: string }> = {};
  const ol = obj(o.optionLabels);
  for (const k of Object.keys(ol)) { const e = obj(ol[k]); if (typeof e.label === "string") optionLabels[k] = { label: e.label, help: strOpt(e, "help") }; }
  return { key: o.key, type: types.includes(o.type as string) ? (o.type as Setting["type"]) : "string",
    label: str(o, "label") || o.key, default: valOf(o.default), min: numOpt(o, "min"), max: numOpt(o, "max"),
    options: strs(o, "options").length ? strs(o, "options") : undefined, help: strOpt(o, "help"),
    optionLabels: Object.keys(optionLabels).length ? optionLabels : undefined };
}

export function valOf(v: unknown): Val {
  if (typeof v === "boolean" || typeof v === "number" || typeof v === "string") return v;
  return "";
}

export function valuesOf(v: unknown): Record<string, Val> {
  const o = obj(v);
  const out: Record<string, Val> = {};
  for (const k of Object.keys(o)) out[k] = valOf(o[k]);
  return out;
}

export function defaultsOf(v: unknown): Defaults {
  const o = obj(v);
  return { plugins: arr(o, "plugins").map((p) => { const po = obj(p); return { id: str(po, "id"), enabled: bool(po, "enabled"), settings: valuesOf(po.settings) }; }),
    seeded: bool(o, "seeded") };
}

export function recommendedOf(v: unknown): { source: "index" | "builtin"; items: Recommended[] } {
  const o = obj(v);
  return { source: o.source === "builtin" ? "builtin" : "index", items: arr(o, "items").map((r) => {
    const ro = obj(r);
    return { id: str(ro, "id"), name: str(ro, "name") || str(ro, "id"), description: str(ro, "description"), why: str(ro, "why"),
      settings: valuesOf(ro.settings), installed: bool(ro, "installed"), compatible: ro.compatible !== false, reason: strOpt(ro, "reason"),
      decl: Array.isArray(ro.decl) ? ro.decl.map(settingOf).filter((x): x is Setting => !!x) : undefined };
  }) };
}

export function setupEventOf(v: unknown): SetupEvent {
  const o = obj(v);
  const out: SetupEvent = {};
  if (o.status && typeof o.status === "object") out.status = setupStatusOf(o.status);
  if (o.progress && typeof o.progress === "object") {
    const p = obj(o.progress);
    out.progress = { action: str(p, "action"), step: num(p, "step"), of: num(p, "of"), label: str(p, "label") };
  }
  return out;
}

export function exportResultOf(v: unknown): ExportResult | undefined {
  const o = obj(v);
  if (typeof o.path !== "string" || !o.path) return undefined;
  return { path: o.path, bytes: num(o, "bytes"), entries: num(o, "entries"), onDesktop: bool(o, "onDesktop"),
    sessionId: strOpt(o, "sessionId") ?? null, pid: num(o, "pid") };
}

// "Desktop" or the folder, for "Saved to …".
export function exportPlaceText(r: ExportResult): string {
  if (r.onDesktop) return "Desktop";
  const i = Math.max(r.path.lastIndexOf("\\"), r.path.lastIndexOf("/"));
  return i > 0 ? r.path.slice(0, i) : r.path;
}

export function sizeText(n: number): string {
  if (n >= 1 << 20) return `${(n / 1048576).toFixed(1)} MiB`;
  if (n >= 1024) return `${Math.max(1, Math.round(n / 1024))} KiB`;
  return `${n} B`;
}

export function hashShort(sha256: string | undefined): string {
  return sha256 ? `${sha256.slice(0, 8)}…` : "";
}

export function whenText(at: string): string {
  const t = Date.parse(at);
  if (Number.isNaN(t)) return at;
  return new Date(t).toLocaleString(undefined, { day: "numeric", month: "short", hour: "2-digit", minute: "2-digit" });
}

// -- Local content importer (spec §10.2, `import.*`) --------------------------------------------------------------
export type ImportPhase = "idle" | "downloading" | "copying" | "verifying" | "reading" | "building" | "placing" | "done" | "error" | "cancelled";
export type ImportStatusKind = "none" | "imported" | "stale" | "damaged" | "unsupported" | "busy";
export type ImportCategory = "play" | "dm" | "mode";
export type ImportGroup = string;
export type ImportTimeOfDay = "DAY" | "EVENING" | "NIGHT";

export interface ImportContent { title: string; publisher: string; termsUrl: string; credit: string; }
export interface ImportSource { id: string; name: string; host: string; fileName: string; size: number; sha256: string; }
export interface ImportCounts { play: number; dm: number; mode: number; skipped: number; }
export interface ImportPack { id: string; name: string; enabled: boolean; levels: number; category: string[]; }
export interface ImportMap {
  file: string; stem: string; pack: string; title: string; author?: string;
  group: ImportGroup; groupLabel: string; category: ImportCategory; categoryLabel: string; mode?: string;
  theme: string; timeOfDay: ImportTimeOfDay; survivor: boolean; hidden: boolean; preview: boolean;
}
export interface ImportJobResult { maps: number; counts: ImportCounts; packs: string[]; bytes: number; fingerprint: string; skipped: number; }
export interface ImportJob {
  plugin: string; phase: ImportPhase; bytes: number; total: number; step: number; of: number;
  reason?: string; message?: string; result?: ImportJobResult;
}
export interface Importer {
  plugin: string; name: string;
  recipe: string; recipeVersion: string; format: number; supported: boolean;
  content: ImportContent;
  sources: ImportSource[];
  expect: { maps: number };
  status: ImportStatusKind;
  statusReason?: string;
  imported?: { recipeVersion: string; fingerprint: string; importedAt: string; maps: number; counts: ImportCounts; packs: ImportPack[]; bytes: number };
  zip?: { bytes: number; verified: boolean };
  gate: string;
  job?: ImportJob;
}

const IMPORT_PHASES: ImportPhase[] = ["idle", "downloading", "copying", "verifying", "reading", "building", "placing", "done", "error", "cancelled"];
const IMPORT_STATUSES: ImportStatusKind[] = ["none", "imported", "stale", "damaged", "unsupported", "busy"];
const IMPORT_CATEGORIES: ImportCategory[] = ["play", "dm", "mode"];
const IMPORT_TODS: ImportTimeOfDay[] = ["DAY", "EVENING", "NIGHT"];

function importCountsOf(v: unknown): ImportCounts {
  const o = obj(v);
  return { play: num(o, "play"), dm: num(o, "dm"), mode: num(o, "mode"), skipped: num(o, "skipped") };
}

export function importPackOf(v: unknown): ImportPack | undefined {
  const o = obj(v);
  if (typeof o.id !== "string") return undefined;
  return { id: o.id, name: str(o, "name") || o.id, enabled: bool(o, "enabled"), levels: num(o, "levels"), category: strs(o, "category") };
}
export function importPacksOf(v: unknown): ImportPack[] {
  return (Array.isArray(v) ? v : []).map(importPackOf).filter((x): x is ImportPack => !!x);
}

export function importMapOf(v: unknown): ImportMap | undefined {
  const o = obj(v);
  if (typeof o.file !== "string") return undefined;
  return {
    file: o.file, stem: str(o, "stem"), pack: str(o, "pack"), title: str(o, "title") || o.file, author: strOpt(o, "author"),
    group: str(o, "group"),
    groupLabel: str(o, "groupLabel"),
    category: IMPORT_CATEGORIES.includes(o.category as ImportCategory) ? (o.category as ImportCategory) : "play",
    categoryLabel: str(o, "categoryLabel"), mode: strOpt(o, "mode"), theme: str(o, "theme"),
    timeOfDay: IMPORT_TODS.includes(o.timeOfDay as ImportTimeOfDay) ? (o.timeOfDay as ImportTimeOfDay) : "DAY",
    survivor: bool(o, "survivor"), hidden: bool(o, "hidden"), preview: bool(o, "preview"),
  };
}
export function importMapsOf(v: unknown): ImportMap[] {
  return (Array.isArray(v) ? v : []).map(importMapOf).filter((x): x is ImportMap => !!x);
}

export function importJobOf(v: unknown): ImportJob | undefined {
  const o = obj(v);
  if (typeof o.plugin !== "string") return undefined;
  const result = o.result && typeof o.result === "object" ? obj(o.result) : undefined;
  return {
    plugin: o.plugin, phase: IMPORT_PHASES.includes(o.phase as ImportPhase) ? (o.phase as ImportPhase) : "idle",
    bytes: num(o, "bytes"), total: num(o, "total"), step: num(o, "step"), of: num(o, "of"),
    reason: strOpt(o, "reason"), message: strOpt(o, "message"),
    result: result ? { maps: num(result, "maps"), counts: importCountsOf(result.counts), packs: strs(result, "packs"),
      bytes: num(result, "bytes"), fingerprint: str(result, "fingerprint"), skipped: num(result, "skipped") } : undefined,
  };
}

export function importerOf(v: unknown): Importer | undefined {
  const o = obj(v);
  if (typeof o.plugin !== "string") return undefined;
  const content = obj(o.content);
  const imported = o.imported && typeof o.imported === "object" ? obj(o.imported) : undefined;
  const zip = o.zip && typeof o.zip === "object" ? obj(o.zip) : undefined;
  return {
    plugin: o.plugin, name: str(o, "name") || o.plugin, recipe: str(o, "recipe"), recipeVersion: str(o, "recipeVersion"),
    format: num(o, "format"), supported: o.supported !== false,
    content: { title: str(content, "title"), publisher: str(content, "publisher"), termsUrl: str(content, "termsUrl"), credit: str(content, "credit") },
    sources: arr(o, "sources").map((s) => {
      const so = obj(s);
      return { id: str(so, "id"), name: str(so, "name") || str(so, "id"), host: str(so, "host"), fileName: str(so, "fileName"), size: num(so, "size"), sha256: str(so, "sha256") };
    }),
    expect: { maps: num(obj(o.expect), "maps") },
    status: IMPORT_STATUSES.includes(o.status as ImportStatusKind) ? (o.status as ImportStatusKind) : "none",
    statusReason: strOpt(o, "statusReason"),
    imported: imported ? { recipeVersion: str(imported, "recipeVersion"), fingerprint: str(imported, "fingerprint"), importedAt: str(imported, "importedAt"),
      maps: num(imported, "maps"), counts: importCountsOf(imported.counts), packs: importPacksOf(imported.packs), bytes: num(imported, "bytes") } : undefined,
    zip: zip ? { bytes: num(zip, "bytes"), verified: bool(zip, "verified") } : undefined,
    gate: str(o, "gate"),
    job: o.job && typeof o.job === "object" ? importJobOf(o.job) : undefined,
  };
}
export function importersOf(v: unknown): Importer[] {
  const o = obj(v);
  return arr(o, "importers").map(importerOf).filter((x): x is Importer => !!x);
}

export function importMapsResultOf(v: unknown): { maps: ImportMap[]; packs: ImportPack[] } {
  const o = obj(v);
  return { maps: importMapsOf(o.maps), packs: importPacksOf(o.packs) };
}

export function importEventOf(v: unknown): { importers?: Importer[]; job?: ImportJob } {
  const o = obj(v);
  const out: { importers?: Importer[]; job?: ImportJob } = {};
  if (Array.isArray(o.importers)) out.importers = importersOf(o);
  if (o.job && typeof o.job === "object") out.job = importJobOf(o.job);
  return out;
}

export function fingerprintShort(fp: string): string {
  return fp.slice(0, 12);
}
export function importPreviewUrl(plugin: string, stem: string): string {
  return `/import/previews/${encodeURIComponent(plugin)}/${encodeURIComponent(stem)}.png`;
}

// Logs are shared by every copy of the game, so a session from before this install says nothing about it.
export function loadedSinceInstall(status: SetupStatus): boolean {
  const last = status.melange.lastLoad;
  if (!last) return false;
  const loaded = Date.parse(last.at), installed = status.install ? Date.parse(status.install.installedAt) : NaN;
  return Number.isNaN(loaded) || Number.isNaN(installed) || loaded >= installed;
}

// -- Melange updating itself (`update.*`, the "update" channel) ---------------------------------------------------
// idle: nothing known yet · checking · downloading · ready: downloaded and verified, "Restart to update" · current:
// this is the latest · error: a check the user asked for failed (an automatic one fails silently)
export type UpdatePhase = "idle" | "checking" | "downloading" | "ready" | "current" | "error";
export interface UpdateApplied { ok: boolean; version: string; message: string; warnings: string[]; }
export interface UpdateStatus {
  current: string; phase: UpdatePhase; latest?: string; htmlUrl?: string;
  progress?: { got: number; total: number }; error?: string; lastCheck?: string; applied?: UpdateApplied;
  auto?: boolean;   // Settings › Updates › Check for updates automatically (the launcher at start and the game daily)
}

const UPDATE_PHASES: UpdatePhase[] = ["idle", "checking", "downloading", "ready", "current", "error"];

export function updateStatusOf(v: unknown): UpdateStatus {
  const o = obj(v);
  const progress = o.progress && typeof o.progress === "object" ? obj(o.progress) : undefined;
  const applied = o.applied && typeof o.applied === "object" ? obj(o.applied) : undefined;
  const htmlUrl = strOpt(o, "htmlUrl");
  return {
    current: str(o, "current"),
    phase: UPDATE_PHASES.includes(o.phase as UpdatePhase) ? (o.phase as UpdatePhase) : "idle",
    latest: strOpt(o, "latest"),
    // Only a GitHub page is ever linked: the value comes from the network, through the launcher.
    htmlUrl: htmlUrl && /^https:\/\/github\.com\//.test(htmlUrl) ? htmlUrl : undefined,
    progress: progress ? { got: num(progress, "got"), total: num(progress, "total") } : undefined,
    error: strOpt(o, "error"), lastCheck: strOpt(o, "lastCheck"),
    applied: applied ? { ok: bool(applied, "ok"), version: str(applied, "version"), message: str(applied, "message"), warnings: strs(applied, "warnings") } : undefined,
    auto: typeof o.auto === "boolean" ? o.auto : undefined,
  };
}

export function updateEventOf(v: unknown): UpdateStatus | undefined {
  const o = obj(v);
  return o.status && typeof o.status === "object" ? updateStatusOf(o.status) : undefined;
}

// -- Restore vanilla (`setup.vanillaPlan` / `setup.vanillaApply`) ---------------------------------------------------
// Everything in the game folder that isn't the stock game goes, for good; replays move to Documents\Melange\replays.
export interface VanillaGroup { id: string; label: string; files: number; }
export interface VanillaPlan {
  planId: string; refused?: string; groups: VanillaGroup[]; files: number; bytes: number; sample: string[];
  replays: string[]; replaysDir: string; modified: string[]; modifiedCount: number; missing: string[]; missingCount: number;
  overwrites: boolean; verify: boolean; store: Store; selfInGame: boolean;
}
export interface VanillaResult {
  ok: boolean; deleted: number; dirsRemoved: number; moved: { from: string; to: string }[]; replaysDir: string;
  failed: string[]; modified: string[]; missing: string[]; modifiedCount: number; missingCount: number;
  verify: boolean; verifyStarted: boolean; store: Store; selfPending: boolean;
}

const storeOf = (o: Rec): Store => (STORES.includes(o.store as Store) ? (o.store as Store) : "unknown");

export function vanillaPlanOf(v: unknown): VanillaPlan {
  const o = obj(v);
  return {
    planId: str(o, "planId"), refused: strOpt(o, "refused"),
    groups: arr(o, "groups").map((g) => { const go = obj(g); return { id: str(go, "id"), label: str(go, "label") || str(go, "id"), files: num(go, "files") }; })
      .filter((g) => g.files > 0),
    files: num(o, "files"), bytes: num(o, "bytes"), sample: strs(o, "sample"), replays: strs(o, "replays"), replaysDir: str(o, "replaysDir"),
    modified: strs(o, "modified"), modifiedCount: numOpt(o, "modifiedCount") ?? strs(o, "modified").length,
    missing: strs(o, "missing"), missingCount: numOpt(o, "missingCount") ?? strs(o, "missing").length,
    overwrites: bool(o, "overwrites"), verify: bool(o, "verify"), store: storeOf(o), selfInGame: bool(o, "selfInGame"),
  };
}

export function vanillaResultOf(v: unknown): VanillaResult {
  const o = obj(v);
  return {
    ok: bool(o, "ok"), deleted: num(o, "deleted"), dirsRemoved: num(o, "dirsRemoved"),
    moved: arr(o, "moved").map((m) => { const mo = obj(m); return { from: str(mo, "from"), to: str(mo, "to") }; }),
    replaysDir: str(o, "replaysDir"), failed: strs(o, "failed"), modified: strs(o, "modified"), missing: strs(o, "missing"),
    modifiedCount: numOpt(o, "modifiedCount") ?? strs(o, "modified").length, missingCount: numOpt(o, "missingCount") ?? strs(o, "missing").length,
    verify: bool(o, "verify"), verifyStarted: bool(o, "verifyStarted"), store: storeOf(o), selfPending: bool(o, "selfPending"),
  };
}

// -- Settings › Display (`display.get` / `display.set`) -------------------------------------------------------------
// The window size the game opens at (local.cfg /W /H) and Melange's borderless fullscreen ([Display] Fullscreen in
// Melange.ini). Both are read when the game starts, so the server refuses to write them while it runs (`refused`).
export interface DisplaySize { w: number; h: number; }
export interface DisplayState {
  monitor: DisplaySize;                  // the primary monitor, in pixels
  modes: DisplaySize[];                  // window sizes to offer, largest first
  windowed: DisplaySize | null;          // the size the game opens at, null when no cfg file names one
  source: "local" | "default" | "none";  // which file that size came from
  localCfg: boolean; exclusive: boolean; // exclusive: the stock launcher's fullscreen (/FS) is on in local.cfg
  fullscreen: boolean; enabled: boolean; hotkey: string; melangeIni: boolean;
  running: boolean; refused?: string;
  removedFs?: boolean;                   // display.set just took /FS out of local.cfg
}

const sizeOf = (v: unknown): DisplaySize | undefined => {
  const o = obj(v);
  const w = num(o, "w"), h = num(o, "h");
  return w > 0 && h > 0 && Number.isInteger(w) && Number.isInteger(h) ? { w, h } : undefined;
};

export function displayOf(v: unknown): DisplayState {
  const o = obj(v);
  const source = o.source === "local" || o.source === "default" ? o.source : "none";
  return {
    monitor: sizeOf(o.monitor) ?? { w: 0, h: 0 },
    modes: arr(o, "modes").map(sizeOf).filter((s): s is DisplaySize => !!s),
    windowed: sizeOf(o.windowed) ?? null, source,
    localCfg: bool(o, "localCfg"), exclusive: bool(o, "exclusive"), fullscreen: bool(o, "fullscreen"),
    enabled: o.enabled !== false, hotkey: str(o, "hotkey") || "Alt+RETURN", melangeIni: bool(o, "melangeIni"),
    running: bool(o, "running"), refused: strOpt(o, "refused") || undefined,
    ...(o.removedFs === true ? { removedFs: true } : {}),
  };
}

export const sizeKey = (s: DisplaySize): string => `${s.w}x${s.h}`;
export function sizeFromKey(k: string): DisplaySize | undefined {
  const m = /^(\d{3,5})x(\d{3,5})$/.exec(k);
  return m ? { w: Number(m[1]), h: Number(m[2]) } : undefined;
}

// The window-size picker: the offered sizes plus the current one when it isn't among them, largest first. `native`
// marks the monitor's own size.
export function displaySizeOptions(d: DisplayState): { key: string; size: DisplaySize; native: boolean }[] {
  const all = [...d.modes];
  if (d.windowed && !all.some((s) => s.w === d.windowed!.w && s.h === d.windowed!.h)) all.push(d.windowed);
  all.sort((a, b) => (a.w !== b.w ? b.w - a.w : b.h - a.h));
  return all.map((s) => ({ key: sizeKey(s), size: s, native: s.w === d.monitor.w && s.h === d.monitor.h }));
}

// What the picker shows when nothing is set yet: the largest offered 16:9 size below the monitor's (a window the size
// of the whole monitor would not fit with its border), else the largest offered.
export function defaultWindowed(d: DisplayState): DisplaySize | undefined {
  if (d.windowed) return d.windowed;
  const below = d.modes.filter((s) => s.w < d.monitor.w && s.h < d.monitor.h);
  return below.find((s) => s.w * 9 === s.h * 16) ?? below[0] ?? d.modes[0];
}

// -- Settings › Memory (`launcher.laa.get` / `launcher.laa.set`) ---------------------------------------------------
// The opt-in 4 GB mode. `enabled` is [Game] LargeAddressAware in Melange.ini; `active` is the bit in WormsMayhem.exe
// (Steam's "Verify files" clears it until the next launch from Melange.exe); `byMelange` is the marker that lets
// Melange undo its own change. The server refuses to write while the game runs (`refused`).
export interface LaaState { enabled: boolean; active: boolean; byMelange: boolean; melangeIni: boolean; running: boolean; refused?: string; }

export function laaOf(v: unknown): LaaState {
  const o = obj(v);
  return {
    enabled: bool(o, "enabled"), active: bool(o, "active"), byMelange: bool(o, "byMelange"), melangeIni: bool(o, "melangeIni"),
    running: bool(o, "running"), refused: strOpt(o, "refused") || undefined,
  };
}
