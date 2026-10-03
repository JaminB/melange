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
    webview: bool(o, "webview"), elevated: bool(o, "elevated"), protected: strs(o, "protected") };
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

// Logs are shared by every copy of the game, so a session from before this install says nothing about it.
export function loadedSinceInstall(status: SetupStatus): boolean {
  const last = status.melange.lastLoad;
  if (!last) return false;
  const loaded = Date.parse(last.at), installed = status.install ? Date.parse(status.install.installedAt) : NaN;
  return Number.isNaN(loaded) || Number.isNaN(installed) || loaded >= installed;
}
