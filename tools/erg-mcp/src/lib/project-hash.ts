// A project's content hash, so a Test can prove it plays the level that was built: the patch (its sorted JSON, without
// the stem and title, which an export renames, and without the script's metadata, which the server rewrites) and the
// script's text (line ends and trailing blank space normalised). The same hash comes from the open project (the patch
// save would send and the server's script) and from files on disk: a project folder (project.ergpatch.json,
// script.lua) or an exported source pack (src\<stem>.ergpatch.json, sim\<stem>.lua). In the Caldera iteration-10 run
// the builder exported to one game while the Test ran another's older project of the same id; test {expectHash,
// source} refuses that, or with sync copies the built level into the game's project first.
import { createHash } from "node:crypto";
import { existsSync, readFileSync, readdirSync, statSync } from "node:fs";
import { basename, dirname, join } from "node:path";
import { ToolError } from "../registry";

/** JSON with every object's keys sorted, so key order never changes the hash. */
export function canonical(v: unknown): string {
  if (v === null || typeof v !== "object") return JSON.stringify(v) ?? "null";
  if (Array.isArray(v)) return `[${v.map(canonical).join(",")}]`;
  const o = v as Record<string, unknown>;
  return `{${Object.keys(o).filter((k) => o[k] !== undefined).sort().map((k) => `${JSON.stringify(k)}:${canonical(o[k])}`).join(",")}}`;
}

export const normScript = (t: string) => t.replace(/\r\n?/g, "\n").replace(/\s+$/, "");

/** A level object as the server stores it (src/erg/scene.cpp ObjectsJson): a crate and a trigger with every field, the
 * defaults filled in (CrateSpec, TriggerSpec), a mine factory with only the settings that are set (none: no `factory`).
 * The patch save sends leaves a default out (a crate placed without a count) where the project file has it, so save's
 * hash and the saved project's (test {expectHash}) differed: f5dc0f39 against a9798d62 in the Caldera v2 round-2 run,
 * the four crates' count: 1 the only difference. */
export function storedObject(o: Record<string, unknown>): Record<string, unknown> {
  const out: Record<string, unknown> = { knot: o.knot, type: o.type };
  if (o.type === "crate") {
    const c = (o.crate ?? {}) as Record<string, unknown>;
    out.crate = c.kind === "health"
      ? { kind: c.kind, amount: c.amount ?? 0, hitpoints: c.hitpoints ?? 25, parachute: c.parachute ?? false }
      : { kind: c.kind ?? "weapon", contents: c.contents ?? "", count: c.count ?? 1, hitpoints: c.hitpoints ?? 25, parachute: c.parachute ?? false };
  } else if (o.type === "telepad") out.group = o.group;
  else if (o.type === "trigger") {
    const t = (o.trigger ?? {}) as Record<string, unknown>;
    out.trigger = { index: t.index ?? 0, radius: t.radius ?? 60, teamCollect: t.teamCollect ?? 0, teamDestroy: t.teamDestroy ?? 4, hitpoints: t.hitpoints ?? 1,
      wormCollect: t.wormCollect ?? false };
  } else if (o.type === "minefactory") {
    const f = (o.factory ?? {}) as Record<string, unknown>, set: Record<string, unknown> = {};
    for (const k of ["activation", "inactiveTurns", "launchHeight", "launchRadius"]) if (f[k] !== undefined && f[k] !== null) set[k] = f[k];
    if (f.schemeMines === false) set.schemeMines = false;
    if (f.keepKnot === true) set.keepKnot = true;
    if (Array.isArray(f.launchers) && f.launchers.length) set.launchers = f.launchers;
    if (Object.keys(set).length) out.factory = set;
  } else return o;
  return out;
}

/** The hash (64 hex digits) of a patch and its script, the patch taken in the form the server stores it (its objects
 * with their defaults: storedObject), so the patch save sends, the project file the server wrote, an exported pack and
 * the project opened again all hash the same. */
export function projectHash(patch: object, script: string): string {
  const { stem: _s, title: _t, script: _m, ...rest } = patch as Record<string, unknown>;
  if (Array.isArray(rest.objects)) rest.objects = rest.objects.map((o) => (o && typeof o === "object" ? storedObject(o as Record<string, unknown>) : o));
  return createHash("sha256").update(canonical(rest)).update("\n--script--\n").update(normScript(script)).digest("hex");
}

/** A built level on disk: a project folder, an exported source pack (its only level, or `stem`), or a .ergpatch.json
 * file (its script beside it as script.lua, or the pack's sim\<stem>.lua). */
export function readSource(path: string): { patch: Record<string, unknown>; script: string; patchFile: string; scriptFile: string | null } {
  if (!existsSync(path)) throw new ToolError(`source: ${path} does not exist`);
  let patchFile: string, scriptFile: string | null = null;
  if (statSync(path).isDirectory()) {
    if (existsSync(join(path, "project.ergpatch.json"))) {
      patchFile = join(path, "project.ergpatch.json");
      scriptFile = join(path, "script.lua");
    } else {
      const src = existsSync(join(path, "src")) ? join(path, "src") : path;
      const found = readdirSync(src).filter((f) => f.toLowerCase().endsWith(".ergpatch.json"));
      if (found.length !== 1) throw new ToolError(`source: ${src} holds ${found.length} .ergpatch.json files: give the project folder, a pack with one level, or the file itself`);
      patchFile = join(src, found[0]);
      scriptFile = join(path, "sim", found[0].replace(/\.ergpatch\.json$/i, ".lua"));
    }
  } else {
    patchFile = path;
    const stem = basename(path).replace(/\.ergpatch\.json$/i, "");
    scriptFile = [join(dirname(path), "script.lua"), join(dirname(path), "..", "sim", `${stem}.lua`)].find((f) => existsSync(f)) ?? null;
  }
  let patch: Record<string, unknown>;
  try {
    patch = JSON.parse(readFileSync(patchFile, "utf8"));
  } catch (e) {
    throw new ToolError(`source: ${patchFile} is not a patch (${e instanceof Error ? e.message : String(e)})`);
  }
  if (!patch || typeof patch !== "object" || !("format" in patch)) throw new ToolError(`source: ${patchFile} is not an Erg patch (no format)`);
  const hasScript = !!scriptFile && existsSync(scriptFile);
  return { patch, script: hasScript ? readFileSync(scriptFile!, "utf8") : "", patchFile, scriptFile: hasScript ? scriptFile : null };
}
