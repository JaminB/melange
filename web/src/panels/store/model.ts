// store.* results as the page uses them. Everything from the index is shown as text, never as HTML.
export type Action = "install" | "update" | "remove" | "none";

export interface Installed { version: string; managed: boolean; state: string; enabled: boolean; }
export interface Item {
  id: string; name: string; authors: string[]; description: string; categories: string[];
  kind: "client-only" | "content"; unsafe: boolean; licence: string; latest: string; compatible: string; size: number;
  installed: Installed | null; action: Action; canRemove: boolean; state: string; reason: string; error: string;
}
export interface Dep { id: string; range: string; }
export interface Shot { n: number; caption: string; ready: boolean; }
export interface VersionRow { version: string; released: string; melange: string; size: number; changelog: string; yanked: boolean; compatible: boolean; }
export interface ImportsLine { title: string; publisher: string; host: string; size: number; }
export interface Details extends Item {
  homepage: string; permissions: { unsafe: boolean; filesystem: string }; content: boolean;
  dependencies: Dep[]; conflicts: Dep[]; screenshots: Shot[]; versions: VersionRow[];
  dependants: string[]; conflictsEnabled: string[]; plan: { id: string; version: string }[]; planError: string;
  // A plugin with a recipe (spec §11.1 `imports` index field): what it can download, shown before install, and
  // (§12.2 "Store remove confirm") how many maps it imported, shown before remove.
  imports: ImportsLine[]; importedMaps: number;
}
export interface Job { phase: string; id: string; version: string; bytes: number; total: number; message: string; }
export interface Status {
  enabled: boolean; indexUrl: string; customIndex: boolean; fetchedAt: string; offline: boolean; serial: number;
  plugins: number; job: Job; gate: string; fetching: boolean; haveIndex: boolean; rollback: boolean; busy: boolean;
  error: string; pending: string[]; notices: string[];
}
export interface Event { phase: string; id: string; version: string; bytes: number; total: number; message: string; pending: string[]; shots: number; busy: boolean; gate: string; fetching: boolean; }

export const CATEGORIES = ["graphics", "gameplay", "maps", "weapons", "audio", "interface", "tools", "libraries"] as const;
export const PRIVACY = "The Store downloads the plugin list and the files you choose from GitHub, only when you open it or press a button. " +
  "Nothing about you or your game is sent.";
export const DEEP_DESERT = "This plugin asks for Deep Desert: raw access to the game's memory. It will run sandboxed until you allow it " +
  "in the game's overlay. Only allow it if you trust the author.";

type Rec = Record<string, unknown>;
const obj = (v: unknown): Rec => (v && typeof v === "object" && !Array.isArray(v) ? (v as Rec) : {});
const str = (o: Rec, k: string) => (typeof o[k] === "string" ? (o[k] as string) : "");
const num = (o: Rec, k: string) => (typeof o[k] === "number" && Number.isFinite(o[k]) ? (o[k] as number) : 0);
const bool = (o: Rec, k: string) => o[k] === true;
const strs = (o: Rec, k: string) => (Array.isArray(o[k]) ? (o[k] as unknown[]).filter((x): x is string => typeof x === "string") : []);
const deps = (o: Rec, k: string): Dep[] =>
  Array.isArray(o[k]) ? (o[k] as unknown[]).map(obj).filter((d) => typeof d.id === "string").map((d) => ({ id: str(d, "id"), range: str(d, "range") })) : [];
const ACTIONS: Action[] = ["install", "update", "remove", "none"];

export function itemOf(v: unknown): Item | undefined {
  const o = obj(v);
  if (typeof o.id !== "string") return undefined;
  const inst = o.installed && typeof o.installed === "object" ? obj(o.installed) : undefined;
  return {
    id: o.id, name: str(o, "name") || o.id, authors: strs(o, "authors"), description: str(o, "description"),
    categories: strs(o, "categories"), kind: o.kind === "content" ? "content" : "client-only", unsafe: bool(o, "unsafe"),
    licence: str(o, "licence"), latest: str(o, "latest"), compatible: str(o, "compatible"), size: num(o, "size"),
    installed: inst ? { version: str(inst, "version"), managed: bool(inst, "managed"), state: str(inst, "state"), enabled: bool(inst, "enabled") } : null,
    action: ACTIONS.includes(o.action as Action) ? (o.action as Action) : "none", canRemove: bool(o, "canRemove"),
    state: str(o, "state"), reason: str(o, "reason"), error: str(o, "error"),
  };
}

export function itemsOf(v: unknown): Item[] {
  return Array.isArray(v) ? v.map(itemOf).filter((x): x is Item => !!x) : [];
}

export function detailsOf(v: unknown): Details | undefined {
  const it = itemOf(v);
  if (!it) return undefined;
  const o = obj(v);
  const perm = obj(o.permissions);
  const shots = Array.isArray(o.screenshots) ? (o.screenshots as unknown[]).map(obj) : [];
  const versions = Array.isArray(o.versions) ? (o.versions as unknown[]).map(obj) : [];
  const plan = Array.isArray(o.plan) ? (o.plan as unknown[]).map(obj) : [];
  return {
    ...it, homepage: str(o, "homepage"), permissions: { unsafe: bool(perm, "unsafe"), filesystem: str(perm, "filesystem") || "none" },
    content: bool(o, "content"), dependencies: deps(o, "dependencies"), conflicts: deps(o, "conflicts"),
    screenshots: shots.filter((s) => num(s, "n") >= 1).map((s) => ({ n: num(s, "n"), caption: str(s, "caption"), ready: bool(s, "ready") })),
    versions: versions.filter((x) => typeof x.version === "string").map((x) => ({
      version: str(x, "version"), released: str(x, "released"), melange: str(x, "melange"), size: num(x, "size"),
      changelog: str(x, "changelog"), yanked: bool(x, "yanked"), compatible: bool(x, "compatible"),
    })),
    dependants: strs(o, "dependants"), conflictsEnabled: strs(o, "conflictsEnabled"),
    plan: plan.map((p) => ({ id: str(p, "id"), version: str(p, "version") })), planError: str(o, "planError"),
    imports: (Array.isArray(o.imports) ? o.imports : []).map(obj).filter((x) => typeof x.title === "string")
      .map((x) => ({ title: str(x, "title"), publisher: str(x, "publisher"), host: str(x, "host"), size: num(x, "size") })),
    importedMaps: num(o, "importedMaps"),
  };
}

function jobOf(v: unknown): Job {
  const o = obj(v);
  return { phase: str(o, "phase") || "idle", id: str(o, "id"), version: str(o, "version"), bytes: num(o, "bytes"), total: num(o, "total"), message: str(o, "message") };
}

export function statusOf(v: unknown): Status {
  const o = obj(v);
  return {
    enabled: bool(o, "enabled"), indexUrl: str(o, "indexUrl"), customIndex: bool(o, "customIndex"), fetchedAt: str(o, "fetchedAt"),
    offline: bool(o, "offline"), serial: num(o, "serial"), plugins: num(o, "plugins"), job: jobOf(o.job), gate: str(o, "gate"),
    fetching: bool(o, "fetching"), haveIndex: bool(o, "haveIndex"), rollback: bool(o, "rollback"), busy: bool(o, "busy"),
    error: str(o, "error"), pending: strs(o, "pending"), notices: strs(o, "notices"),
  };
}

export function eventOf(v: unknown): Event {
  const o = obj(v);
  return { ...jobOf(o), pending: strs(o, "pending"), shots: num(o, "shots"), busy: bool(o, "busy"), gate: str(o, "gate"), fetching: bool(o, "fetching") };
}

export function sizeText(n: number): string {
  if (n >= 1 << 20) return `${(n / 1048576).toFixed(1)} MiB`;
  return `${Math.max(1, Math.round(n / 1024))} KiB`;
}

export function fetchLine(s: Status): string {
  if (s.fetching) return "Fetching the list…";
  if (s.offline && s.haveIndex) return `Offline: showing the list from ${s.fetchedAt}`;
  if (s.error) return s.error;
  if (s.haveIndex) return `Fetched ${s.fetchedAt}, ${s.plugins} plugin${s.plugins === 1 ? "" : "s"}`;
  return "Not fetched yet.";
}

export function actionLabel(it: Item): string | undefined {
  if (it.action === "install") return it.installed ? "Replace" : "Install";
  if (it.action === "update") return "Update";
  if (it.action === "remove") return "Remove";
  return undefined;
}

export function stateLine(it: Item): string {
  if (it.state === "update") return `Update to ${it.compatible}`;
  if (it.state === "pending") return "Applies at the next launch";
  if (it.state === "manual" || it.state === "incompatible" || it.state === "yanked") return it.reason;
  if (it.installed) return `Installed ${it.installed.version}`;
  return it.reason;
}

export function progress(j: Job): { active: boolean; pct: number; text: string } {
  const active = ["downloading", "verifying", "installing", "removing"].includes(j.phase);
  const pct = j.total > 0 ? Math.min(100, Math.round((j.bytes / j.total) * 100)) : 0;
  const amount = j.phase === "downloading" && j.total > 0 ? ` ${sizeText(j.bytes)} / ${sizeText(j.total)}` : "";
  return { active, pct, text: `${j.phase} ${j.id}${j.version ? ` ${j.version}` : ""}${amount}` };
}

export function permissionsText(d: Details): string {
  const parts = [d.permissions.unsafe ? "Deep Desert (raw access to the game's memory)" : "sandboxed Lua only"];
  if (d.permissions.filesystem !== "none") parts.push(`files: ${d.permissions.filesystem}`);
  return parts.join(", ");
}

export type Ask = { kind: "install"; version: string; older: boolean } | { kind: "remove" };

// The lines a confirm shows before anything is downloaded or deleted.
export function confirmLines(d: Details, ask: Ask): string[] {
  const out: string[] = [];
  if (ask.kind === "remove") {
    if (d.dependants.length) out.push(`${d.dependants.join(", ")} need${d.dependants.length === 1 ? "s" : ""} this; they will be blocked.`);
    if (d.importedMaps > 0) out.push(`This also removes the ${d.importedMaps} map${d.importedMaps === 1 ? "" : "s"} it imported.`);
    return out;
  }
  out.push(`${sizeText(d.size)}, ${d.kind}`);
  for (const im of d.imports)
    out.push(`${d.name} can download ${im.title} (${sizeText(im.size)}) from ${im.host} when you ask it to. The maps are imported on your PC; the plugin contains none of them.`);
  if (d.installed && !d.installed.managed) out.push(`Replace the copy in Mods\\${d.id} (version ${d.installed.version})?`);
  if (ask.older) out.push("This is older than what you have.");
  if (d.unsafe) {
    out.push(DEEP_DESERT);
    if (d.installed) out.push("Updates ask for Deep Desert again.");
  }
  if (d.content) out.push("Content: everyone in an online match needs the same version.");
  const also = d.plan.filter((p) => p.id !== d.id).map((p) => `${p.id} ${p.version}`);
  if (also.length) out.push(`Also installs: ${also.join(", ")}`);
  if (d.planError) out.push(d.planError);
  if (d.conflictsEnabled.length) out.push(`Conflicts with ${d.conflictsEnabled.join(", ")}: Thumper will block both while they are enabled.`);
  return out;
}

export function compareVersions(a: string, b: string): number {
  const parse = (v: string) => {
    const [core, pre] = v.split("-", 2);
    return { n: core.split(".").map((x) => Number.parseInt(x, 10) || 0), pre };
  };
  const x = parse(a), y = parse(b);
  for (let i = 0; i < 3; i++) if ((x.n[i] ?? 0) !== (y.n[i] ?? 0)) return (x.n[i] ?? 0) < (y.n[i] ?? 0) ? -1 : 1;
  if (x.pre === y.pre) return 0;
  if (x.pre === undefined) return 1;
  if (y.pre === undefined) return -1;
  return x.pre < y.pre ? -1 : 1;
}

export function shotUrl(id: string, n: number, bust: number): string {
  return `/store/shots/${encodeURIComponent(id)}/${n}?v=${bust}`;
}
