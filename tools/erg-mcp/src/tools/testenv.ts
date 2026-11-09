// Tool group "testenv": a test game's plugins for a run. test_plugins turns on only the plugins a check needs (Sunstone,
// say) in a game folder and turns the rest off, installing a plugin from a folder first when asked, and puts everything
// back afterwards: the game's Mods\thumper-state.json (which plugins are on) and every Mods\<id> folder it replaced
// are kept in <gameDir>\Melange\erg-mcp\plugins-backup until restore. It works on the files, so the game must be closed
// (plugins load at launch).
import { cpSync, existsSync, mkdirSync, readFileSync, readdirSync, renameSync, rmSync, statSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { execFileSync } from "node:child_process";
import { ToolError, type Tool } from "../registry";

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

export const tools: Tool[] = [{
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
