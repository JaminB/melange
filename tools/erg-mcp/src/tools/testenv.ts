// Tool group "testenv": a test game's plugins for a run. test_plugins turns on only the plugins a check needs (Sunstone,
// say) in a game folder and turns the rest off, installing a plugin from a folder first when asked, and puts everything
// back afterwards: the game's Mods\thumper-state.json (which plugins are on) and every Mods\<id> folder it replaced
// are kept in <gameDir>\Melange\erg-mcp\plugins-backup until restore. It works on the files, so the game must be closed
// (plugins load at launch).
import { cpSync, existsSync, mkdirSync, readFileSync, readdirSync, renameSync, rmSync, statSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { documentsDir } from "../oasis";
import { gameBuild } from "../lib/game-build";
import { projectLacks } from "../lib/ship-gate-export";
import { ToolError, type Tool } from "../registry";
import type { Session } from "../session";

interface Manifest { at: string; only: string[]; installed: { id: string; from: string; replaced: boolean }[]; }

const ID = /^[a-z0-9][a-z0-9._-]{0,63}$/;

function gameDirOf(dir?: string): string {
  const d = dir ?? process.env.MELANGE_GAME_DIR;
  if (!d) throw new ToolError("give gameDir (the test game's folder, where WormsMayhem.exe is) or set MELANGE_GAME_DIR");
  if (!existsSync(join(d, "WormsMayhem.exe"))) throw new ToolError(`${d} has no WormsMayhem.exe: gameDir is the game's folder`);
  if (!existsSync(join(d, "Mods"))) throw new ToolError(`${d} has no Mods folder (Melange's plugins live there)`);
  return d;
}

/** Whether a WormsMayhem.exe runs (any folder: tasklist cannot say which; the test machine runs one game at a time). */
function gameRunning(): boolean {
  try {
    return /WormsMayhem\.exe/i.test(execFileSync("tasklist", ["/FI", "IMAGENAME eq WormsMayhem.exe", "/NH"], { encoding: "utf8", windowsHide: true, timeout: 15000 }));
  } catch {
    return false;
  }
}

/** A plugin folder's id and version, from its spice.json. */
function spiceOf(dir: string): { id: string; version: string } {
  const f = join(dir, "spice.json");
  if (!existsSync(f)) throw new ToolError(`${dir} has no spice.json: install takes a plugin's folder (the one with spice.json, e.g. melange-plugins\\plugins\\sunstone\\mod)`);
  let j: { id?: unknown; version?: unknown };
  try {
    j = JSON.parse(readFileSync(f, "utf8"));
  } catch (e) {
    throw new ToolError(`${f}: ${(e as Error).message}`);
  }
  if (typeof j.id !== "string" || !ID.test(j.id)) throw new ToolError(`${f}: no valid id`);
  return { id: j.id, version: typeof j.version === "string" ? j.version : "?" };
}

/** A game folder's projects folder: `[Erg] ProjectsDir` of its Melange.ini (the first such key), else Documents\Melange\erg\projects. */
export function projectsDirOf(gameDir: string): { dir: string; from: string } {
  const ini = join(gameDir, "Melange.ini");
  if (existsSync(ini)) {
    let section = "";
    for (const line of readFileSync(ini, "utf8").split(/\r?\n/)) {
      const sec = /^\s*\[([^\]]+)\]/.exec(line);
      if (sec) { section = sec[1].trim().toLowerCase(); continue; }
      const kv = /^\s*ProjectsDir\s*=\s*([^;\r\n]*)/i.exec(line);
      if (kv && section === "erg" && kv[1].trim()) return { dir: kv[1].trim(), from: "[Erg] ProjectsDir of its Melange.ini" };
    }
  }
  const docs = documentsDir();
  if (!docs) throw new ToolError("no Documents\\Melange folder was found, and the game's Melange.ini sets no [Erg] ProjectsDir");
  return { dir: join(docs, "Melange", "erg", "projects"), from: "Documents\\Melange\\erg\\projects (its Melange.ini sets no [Erg] ProjectsDir)" };
}

/** A folder's files (relative paths, sorted) hashed with their bytes: equal folders hash the same. */
function folderHash(dir: string): { hash: string; files: number } {
  const h = createHash("sha256"), names: string[] = [];
  const walk = (d: string, rel: string) => {
    for (const n of readdirSync(d).sort()) {
      const f = join(d, n);
      if (statSync(f).isDirectory()) walk(f, `${rel}${n}/`);
      else names.push(`${rel}${n}`);
    }
  };
  walk(dir, "");
  for (const n of names) { h.update(n); h.update(readFileSync(join(dir, n))); }
  return { hash: h.digest("hex").slice(0, 16), files: names.length };
}

/** Every *.png under `dir` (relative paths with '/'), and the text files a record could be in. */
function evidenceFiles(dir: string): { pngs: { rel: string; bytes: number; mtimeMs: number }[]; texts: string[] } {
  const pngs: { rel: string; bytes: number; mtimeMs: number }[] = [], texts: string[] = [];
  const walk = (d: string, rel: string, depth: number) => {
    for (const n of readdirSync(d)) {
      const f = join(d, n);
      let st;
      try { st = statSync(f); } catch { continue; }
      if (st.isDirectory()) { if (depth < 8) walk(f, `${rel}${n}/`, depth + 1); }
      else if (/\.png$/i.test(n)) pngs.push({ rel: `${rel}${n}`, bytes: st.size, mtimeMs: st.mtimeMs });
      else if (/\.(json|jsonl|md|txt|log|csv)$/i.test(n) && st.size <= 32 * 1024 * 1024) texts.push(f);
    }
  };
  walk(dir, "", 0);
  return { pngs, texts };
}

/** The PNG file names a text names (a probe record's `shot`, a log line's path): base names, lower case. */
export function namedPngs(text: string): Set<string> {
  const out = new Set<string>();
  for (const m of text.matchAll(/[A-Za-z0-9_()-]+(?:\.[A-Za-z0-9_()-]+)*?\.png/gi)) out.add(m[0].toLowerCase());
  return out;
}

export const tools: Tool[] = [{
  name: "prune_evidence",
  description: "Free disk in an evidence folder without losing the pictures its records point at: every *.png under `dir` whose file name appears in no record is a candidate, and a picture named in any record is kept. Records are the .json, .jsonl, .md, .txt, .log and .csv files under `dir` and under each of `refs` (more folders or files: a probe's JSON kept elsewhere, the run's log). Round 2's pruning by hand deleted the shot of the hole a worm died in, which the probe's record named. Candidates are removed oldest first until the folder's pictures take at most `keepMB` (default 0: every unreferenced one), never one younger than `minAgeMinutes` (default 0) or whose name holds one of `keep`. `dryRun` defaults to TRUE: it answers what would go; pass dryRun: false to delete. A folder inside a game's own folder (a testenv's Melange\\docshot or m1t, which hold other sessions' shots) is refused unless `prefix` limits the candidates to names that start with it.",
  inputSchema: {
    type: "object", additionalProperties: false, required: ["dir"],
    properties: {
      dir: { type: "string", description: "The evidence folder." },
      refs: { type: "array", maxItems: 16, items: { type: "string" }, description: "More folders or files whose records name pictures to keep." },
      keep: { type: "array", maxItems: 32, items: { type: "string" }, description: "Name fragments: a picture whose path holds one is kept." },
      prefix: { type: "string", description: "Only pictures whose file name starts with this are candidates." },
      keepMB: { type: "number", minimum: 0, description: "Stop when the folder's pictures take at most this (default 0: remove every unreferenced one)." },
      minAgeMinutes: { type: "number", minimum: 0, description: "Never remove a picture younger than this (default 0)." },
      dryRun: { type: "boolean", description: "Default true: only answer what would be removed." },
    },
  },
  handler(_s: Session, a: { dir: string; refs?: string[]; keep?: string[]; prefix?: string; keepMB?: number; minAgeMinutes?: number; dryRun?: boolean }) {
    if (!existsSync(a.dir) || !statSync(a.dir).isDirectory()) throw new ToolError(`no folder ${a.dir}`);
    // inside a game's folder: other sessions' shots live there, so only a prefix's own files
    for (let d = a.dir, i = 0; i < 6; i++) {
      if (existsSync(join(d, "WormsMayhem.exe")) && !a.prefix) throw new ToolError(`${a.dir} is inside a game's folder (${d}): give prefix, so only this session's shots are candidates`);
      const up = join(d, "..");
      if (up === d) break;
      d = up;
    }
    const ev = evidenceFiles(a.dir), named = new Set<string>();
    const texts = [...ev.texts];
    for (const r of a.refs ?? []) {
      if (!existsSync(r)) throw new ToolError(`refs: no ${r}`);
      if (statSync(r).isDirectory()) texts.push(...evidenceFiles(r).texts);
      else texts.push(r);
    }
    for (const t of texts) { try { for (const n of namedPngs(readFileSync(t, "utf8"))) named.add(n); } catch { /* unreadable: names nothing */ } }
    const base = (rel: string) => rel.slice(rel.lastIndexOf("/") + 1);
    const now = Date.now(), minAge = (a.minAgeMinutes ?? 0) * 60000;
    const total = ev.pngs.reduce((n, p) => n + p.bytes, 0);
    const kept = { referenced: 0, byKeep: 0, young: 0, notPrefix: 0 };
    const cand = ev.pngs.filter((p) => {
      const b = base(p.rel);
      if (named.has(b.toLowerCase())) { kept.referenced++; return false; }
      if (a.prefix && !b.startsWith(a.prefix)) { kept.notPrefix++; return false; }
      if (a.keep?.some((k) => p.rel.includes(k))) { kept.byKeep++; return false; }
      if (now - p.mtimeMs < minAge) { kept.young++; return false; }
      return true;
    }).sort((x, y) => x.mtimeMs - y.mtimeMs);
    const budget = (a.keepMB ?? 0) * 1024 * 1024, dry = a.dryRun !== false;
    let left = total, freed = 0;
    const gone: string[] = [];
    for (const p of cand) {
      if (left <= budget) break;
      if (!dry) { try { rmSync(join(a.dir, p.rel)); } catch { continue; } }
      left -= p.bytes; freed += p.bytes; gone.push(p.rel);
    }
    const mb = (v: number) => Math.round(v / 10485.76) / 100;
    return { dir: a.dir, dryRun: dry, pictures: ev.pngs.length, records: texts.length, namedInRecords: named.size, kept,
      [dry ? "wouldRemove" : "removed"]: gone.length, freedMB: mb(freed), beforeMB: mb(total), afterMB: mb(left), examples: gone.slice(0, 12),
      note: dry ? "nothing was removed: pass dryRun: false to delete these" : "a picture named in any record was kept" };
  },
}, {
  name: "push_project_to_game",
  description: "Copy a saved project from the server it was built on into a test game's projects folder, so that game's Test plays the level just built: Test runs in the game and reads the game's own folder ([Erg] ProjectsDir of its Melange.ini, else Documents\\Melange\\erg\\projects), which is another folder than the one Melange.exe --serve for a different install keeps (Dustcrown's builds copied the folder by hand and edited the ini). `id` (default the open project) is read from the connected server's projectsDir, and copied over <game's projects folder>\\<id> (the folder it replaces is kept beside it as <id>.erg-mcp-old until the next push; `keepOld: false` drops it). Refused while the open project has unsaved edits (save first) and when both folders are the same one (nothing to do: `same: true`). Answers both folders and their content hash (equal after the copy). Then connect to the game and test {expectHash}. `dryRun` only answers where it would go.",
  inputSchema: {
    type: "object", additionalProperties: false,
    properties: {
      id: { type: "string", description: "Project id (default the open project)." },
      gameDir: { type: "string", description: "The test game's folder, where WormsMayhem.exe is (default MELANGE_GAME_DIR)." },
      from: { type: "string", description: "The projects folder to read from (default the connected server's projectsDir)." },
      keepOld: { type: "boolean", description: "Keep the replaced folder as <id>.erg-mcp-old (default true)." },
      dryRun: { type: "boolean" },
    },
  },
  async handler(s: Session, a: { id?: string; gameDir?: string; from?: string; keepOld?: boolean; dryRun?: boolean }) {
    const id = a.id ?? s.project?.id;
    if (!id) throw new ToolError("give id, or open a project");
    if (!/^[a-z0-9]{1,24}$/.test(id)) throw new ToolError("id is a project id: 1-24 characters a-z, 0-9");
    if (s.project?.id === id && s.project.store.dirty) throw new ToolError("the open project has unsaved edits: save first (the copy is of the saved project)");
    const game = gameDirOf(a.gameDir);
    let fromDir = a.from;
    if (!fromDir) {
      const l = (await (await s.levels()).list()) as { projectsDir?: unknown };
      if (typeof l.projectsDir !== "string") throw new ToolError("the server did not answer its projectsDir (an older Melange): give from");
      fromDir = l.projectsDir;
    }
    const src = join(fromDir, id), to = projectsDirOf(game), dst = join(to.dir, id);
    if (!existsSync(join(src, "project.ergpatch.json"))) throw new ToolError(`no saved project ${id} in ${fromDir} (no project.ergpatch.json)`);
    const same = src.replace(/[\\/]+$/, "").toLowerCase() === dst.replace(/[\\/]+$/, "").toLowerCase();
    const before = folderHash(src);
    const out = { id, from: src, to: dst, gameProjectsDir: to.dir, gameProjectsDirFrom: to.from, hash: before.hash, files: before.files };
    if (same) return { ...out, same: true, note: "the game reads the folder the server writes: nothing to copy" };
    if (a.dryRun) return { ...out, dryRun: true, replaces: existsSync(dst) };
    const old = `${dst}.erg-mcp-old`, tmp = `${dst}.erg-mcp-new`;
    rmSync(tmp, { recursive: true, force: true });
    mkdirSync(to.dir, { recursive: true });
    cpSync(src, tmp, { recursive: true });
    rmSync(old, { recursive: true, force: true });
    const replaced = existsSync(dst);
    if (replaced) renameSync(dst, old);
    renameSync(tmp, dst);
    if (replaced && a.keepOld === false) rmSync(old, { recursive: true, force: true });
    const after = folderHash(dst);
    // the game's own build against what the project uses (an older melange.asi refuses the project at Test)
    const gb = gameBuild(game), lacks = gb && s.project?.id === id ? projectLacks(s.project.store, gb.version) : [];
    if (after.hash !== before.hash) throw new ToolError(`the copy differs from the source (${after.hash} against ${before.hash})`);
    return { ...out, copied: true, replaced, ...(replaced && a.keepOld !== false ? { old } : {}), ...(gb ? { gameBuild: gb.version } : {}),
      ...(lacks.length ? { warning: `the game's melange.asi is Melange ${gb!.version}, older than this project needs: ${lacks.map((n) => `${n.feature} (${n.melange})`).join("; ")}: put the build of this branch there before Test` } : {}),
      next: "connect to the game (it must run for Test), open_project, then test {expectHash: <save's projectHash>}" };
  },
}, {
  name: "test_plugins",
  description: "Set a test game's plugins for a run, and put them back after: `only` (e.g. [\"sunstone\"]) turns those plugins on and every other one off in <gameDir>\\Mods\\thumper-state.json; `install` ([{id, from}]) copies a plugin's folder (the one with spice.json, e.g. C:\\...\\melange-plugins\\plugins\\sunstone\\mod) into Mods\\<id> first, keeping the folder it replaces; `restore: true` puts back the state file and every replaced folder (and removes a plugin that was not there) from <gameDir>\\Melange\\erg-mcp\\plugins-backup. With neither it answers the plugins and whether a backup is waiting. The game must be closed (plugins load at launch): checks with Sunstone on are test_plugins {only: [\"sunstone\"]}, launch, test, ..., quit, test_plugins {restore: true}. A second set-up keeps the first backup, so restore always goes back to the state before the first.",
  inputSchema: {
    type: "object", additionalProperties: false,
    properties: {
      gameDir: { type: "string", description: "The test game's folder (default MELANGE_GAME_DIR)." },
      only: { type: "array", maxItems: 32, items: { type: "string" }, description: "The plugin ids to turn on; every other plugin in Mods is turned off." },
      install: { type: "array", maxItems: 8, items: { type: "object", additionalProperties: false, required: ["id", "from"], properties: { id: { type: "string" }, from: { type: "string" } } },
        description: "[{id, from}]: copy each plugin folder (`from`, with its spice.json, whose id must match) into Mods\\<id> before `only` applies; installed plugins are turned on." },
      restore: { type: "boolean", description: "Put back the plugins as they were before the first set-up of this backup." },
      force: { type: "boolean", description: "Go on although a WormsMayhem.exe runs (another folder's game)." },
    },
  },
  handler(_s, a: { gameDir?: string; only?: string[]; install?: { id: string; from: string }[]; restore?: boolean; force?: boolean }) {
    const dir = gameDirOf(a.gameDir), mods = join(dir, "Mods"), stateFile = join(mods, "thumper-state.json");
    const bk = join(dir, "Melange", "erg-mcp", "plugins-backup"), manFile = join(bk, "manifest.json");
    const listMods = () => readdirSync(mods).filter((n) => !n.startsWith(".") && existsSync(join(mods, n, "spice.json")) && statSync(join(mods, n)).isDirectory());
    const readState = (): { enabled?: Record<string, boolean> } & Record<string, unknown> => (existsSync(stateFile) ? JSON.parse(readFileSync(stateFile, "utf8")) : { version: 1, enabled: {} });
    const view = () => {
      const st = readState();
      return listMods().map((id) => ({ id, on: st.enabled?.[id] === true, version: (() => { try { return spiceOf(join(mods, id)).version; } catch { return "?"; } })() }));
    };
    const changing = a.restore || a.only || a.install;
    if (changing && !a.force && gameRunning()) throw new ToolError("a WormsMayhem.exe is running: plugins load at launch, so close the game first (force: true if it is another folder's game)");
    if (a.restore) {
      if (a.only || a.install) throw new ToolError("restore puts the plugins back: give it alone");
      if (!existsSync(manFile)) return { restored: false, note: "no backup to restore (nothing was set up, or it was restored already)", plugins: view() };
      const man: Manifest = JSON.parse(readFileSync(manFile, "utf8"));
      const done: string[] = [];
      for (const x of man.installed) {
        const target = join(mods, x.id);
        if (existsSync(target)) rmSync(target, { recursive: true, force: true });
        if (x.replaced) { cpSync(join(bk, "mods", x.id), target, { recursive: true, preserveTimestamps: true }); done.push(`${x.id}: the earlier folder back`); }
        else done.push(`${x.id}: removed (it was not installed before)`);
      }
      if (existsSync(join(bk, "thumper-state.json"))) { cpSync(join(bk, "thumper-state.json"), stateFile); done.push("thumper-state.json back"); }
      else if (existsSync(stateFile)) { rmSync(stateFile, { force: true }); done.push("thumper-state.json removed (there was none)"); }
      rmSync(bk, { recursive: true, force: true });
      return { restored: true, done, setUpAt: man.at, plugins: view() };
    }
    if (!changing) return { plugins: view(), backup: existsSync(manFile) ? JSON.parse(readFileSync(manFile, "utf8")) : null, stateFile };
    for (const id of a.only ?? []) if (!ID.test(id)) throw new ToolError(`only: '${id}' is not a plugin id`);
    // the backup: taken once, at the first set-up, so restore goes back to the state before any of this
    const first = !existsSync(manFile);
    const man: Manifest = first ? { at: new Date().toISOString(), only: [], installed: [] } : JSON.parse(readFileSync(manFile, "utf8"));
    if (first) {
      mkdirSync(join(bk, "mods"), { recursive: true });
      if (existsSync(stateFile)) cpSync(stateFile, join(bk, "thumper-state.json"));
    }
    const installed: { id: string; version: string; from: string; replaced: boolean }[] = [];
    for (const { id, from } of a.install ?? []) {
      const sp = spiceOf(from);
      if (sp.id !== id) throw new ToolError(`install: ${from} is plugin '${sp.id}', not '${id}'`);
      const target = join(mods, id), had = existsSync(target);
      // keep the folder it replaces, once (a second install of the same id keeps the first backup)
      if (!man.installed.some((x) => x.id === id)) {
        if (had) cpSync(target, join(bk, "mods", id), { recursive: true, preserveTimestamps: true });
        man.installed.push({ id, from, replaced: had });
      }
      const tmp = `${target}.erg-mcp-new`;
      rmSync(tmp, { recursive: true, force: true });
      cpSync(from, tmp, { recursive: true });
      if (had) rmSync(target, { recursive: true, force: true });
      renameSync(tmp, target);
      installed.push({ id, version: sp.version, from, replaced: had });
    }
    const st = readState(), on = new Set([...(a.only ?? []), ...installed.map((x) => x.id)]);
    if (a.only) {
      const missing = a.only.filter((id) => !listMods().includes(id));
      if (missing.length) throw new ToolError(`only: no plugin ${missing.join(", ")} in ${mods} (install it: install [{id, from}])`);
      const en: Record<string, boolean> = {};
      for (const id of new Set([...Object.keys(st.enabled ?? {}), ...listMods()])) en[id] = on.has(id);
      st.enabled = en;
      man.only = [...on];
    } else for (const x of installed) st.enabled = { ...(st.enabled ?? {}), [x.id]: true };
    writeFileSync(stateFile, JSON.stringify(st));
    writeFileSync(manFile, JSON.stringify(man, null, 1));
    return { set: true, ...(installed.length ? { installed } : {}), on: view().filter((x) => x.on), off: view().filter((x) => !x.on).map((x) => x.id),
      backup: bk, note: `${first ? "the state before was kept" : "the backup from " + man.at + " is kept"}: test_plugins {restore: true} puts it back. Launch the game now: plugins load at launch` };
  },
}];
