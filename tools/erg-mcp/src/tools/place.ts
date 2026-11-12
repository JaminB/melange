// Tool group "place": spawn knots, level objects (crates, telepads, triggers, the mine factory) and placed mines and
// oil drums, moving, turning, scaling and removing them, the level's settings (water, theme, time of day, spawn and
// surround modes), the level script, and shipping the level (export, build, test, live packs). Placing uses the Erg
// panel's own model (model/placing.ts, ground.ts), so a placed object is the one the palette places.
import fs from "node:fs";
import path from "node:path";
import {
  AddDetail, AddObjects, AREA_KNOTS, BUILTIN, Group, KNOT_RESOURCE, KNOTS, LIMITS, TEAM_KNOTS, PAD_SPACING, RemoveDetail, SPAWN_AREA_LIMITS, SetDetail, SetLevel, SetObject, THEMES, TIMES_OF_DAY,
  areaFirstKnot, catalogOf, checkScene, defaultObject, nextObjectKnot, nextTelepadGroup, objectOf, place, spawnsProblems, targetFrame,
  type Command, type CrateSpec, type Detail, type DetailFields, type EditorStore, type FactorySpec, type LevelSettings, type ObjectSpec, type ObjectType,
  type SpawnArea, type Spawns, type TimeOfDay, type Vec3,
} from "../core";
import {
  DEG, commit, detailInfo, findDetail, pointOut, pointToMap, resolvePoint, unitsSchema, vec3, type PointArgs, type Units,
} from "../lib/place-geom";
import { resolveFile, themeFile } from "../lib/materials";
import { runTest, type TestTeam } from "../lib/place-wait";
import { appendFileSync, existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { documentsDir } from "../oasis";
import { automationFile, holdRoundClock, stopHold, tools as gameTools } from "./game";
import { CAMERA_RISE, WALL_RADIUS, nearestTread, spawnWarnings, treadAt, treadWarnings } from "../lib/spawn-check";
import { dealPreview } from "../lib/spawn-deal";
import { expandWaterPreset, hazeLook, readHaze, readSea, readWaterLook, seaLook, shaderOverrideWarnings, waterLookProblems, HAZE_EVENT, SEA_EVENT, WATER_PRESETS, withWaterLook, type HazeLook, type SeaLook, type WaterLook, type WaterShaderMod } from "../lib/water-look";
import { checkFactory, factoryLines } from "../lib/factory-check";
import { projectHash, readSource, storedThrow } from "../lib/project-hash";
import { count, diff, fromSaved, mergeRuns, pairOf, summary, toSaved, type AssertRun, type SavedRun } from "../lib/assert-log";
import { scheduleFromScript } from "../lib/flood";
import { scriptWater } from "../lib/script-water";
import { rotation, slotOf } from "../lib/turns";
import { waterBackdropWarning } from "../lib/backdrop";
import { spansAt, waterMap } from "../lib/inspect-field";
import { grids, tidy } from "../lib/voxel";
import { ToolError, type Schema, type Tool } from "../registry";
import type { Session } from "../session";
import { validateExportForm } from "../../../../web/src/panels/erg/test/model";
import { exportGate, shipFeatures } from "../lib/ship-gate-export";
import { lacking, versionOf } from "../lib/ship-gate";
import { levelLookOf, thumbnailFromFile } from "./levellook";

const pointProps: Record<string, Schema> = {
  pos: vec3("Position [x, y, z] in the level (units, default world). Give this or xz."),
  xz: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2, description: "[x, z]: start above the whole level and drop straight down onto the terrain (the panel's Drop to ground). Give this or pos." },
  drop: { type: "boolean", description: "With pos: also drop the point straight down onto the terrain below it (an error over empty space)." },
};
const idsProps: Record<string, Schema> = {
  id: { type: "integer", description: "A detail id (list_objects)." },
  ids: { type: "array", items: { type: "integer" }, minItems: 1, maxItems: 256, description: "Several detail ids." },
};

function idsOf(a: { id?: number; ids?: number[] }): number[] {
  const ids = [...(a.id === undefined ? [] : [a.id]), ...(a.ids ?? [])];
  if (!ids.length) throw new ToolError("give id or ids");
  return [...new Set(ids)];
}

const infos = (s: Session, ids: number[], units: Units) => ids.map((id) => detailInfo(s.store, s.store.detail(id)!, units));

/** The editor's checks, errors first and at most 30 lines (a base level has dozens of details outside its voxel frames). */
function issuesOf(store: EditorStore, max = 30): string[] {
  const all = checkScene(store.scene, store.frames).sort((a, b) => Number(b.level === "error") - Number(a.level === "error")).map((i) => `${i.level}: ${i.text}`);
  return all.length > max ? [...all.slice(0, max), `... and ${all.length - max} more`] : all;
}

// ------------------------------------------------------------------ spawns

interface CameraArgs { wallRadius?: number; cameraRise?: number; }
const cameraProps: Record<string, Schema> = {
  wallRadius: { type: "number", minimum: 0, maximum: 400, description: `Spawn check: ground this close to a spawn horizontally at its turn camera's height is warned of (units; default ${WALL_RADIUS} map units, ${WALL_RADIUS * 20} world).` },
  cameraRise: { type: "number", minimum: 0, maximum: 2000, description: `Spawn check: how far above the ground a spawn stands on its turn camera sits (units; default ${CAMERA_RISE} map units, ${CAMERA_RISE * 20} world, an assumption).` },
};

/** The spawn checks (a wall at the turn camera, ground under the water, a knot in solid, with no ground near under it
 * or on an edge narrower than a worm) as warning lines in the caller's units. */
function cameraWarnings(store: EditorStore, units: Units, a: CameraArgs): string[] {
  const k = units === "map" ? 1 : 20;
  const len = (v: number) => `${Math.round(v * k * 100) / 100} ${units}`;
  const pt = (p: Vec3) => `[${pointOut(p, units).join(", ")}] ${units}`;
  return [...spawnWarnings(store, len, a.wallRadius === undefined ? WALL_RADIUS : a.wallRadius / k, a.cameraRise === undefined ? CAMERA_RISE : a.cameraRise / k),
    ...treadWarnings(store, pt)];
}

/** The tread warnings (a spawn knot in solid, with no ground near under it, on an edge) for save, in world units. */
export function spawnTreadWarnings(store: EditorStore): string[] {
  return treadWarnings(store, (p: Vec3) => `[${pointOut(p, "world").join(", ")}] world`);
}

interface SpawnArg extends PointArgs { worm: number; group?: string; }

interface Balance { teams?: number; wormsPerTeam?: number; mode?: "height" | "interleave" }

/** The worm numbers re-dealt over the given spawn positions so the teams stand alike: WORMn is worm n % perTeam of team
 * floor(n / perTeam), as check_flood assumes. Positions are sorted by the ground they stand on, highest first, and dealt
 * to the teams in a snake (A B B A A B B A: equal height sums for two teams), then pairs swapped between teams where that
 * leaves fewer of a team's spawns in one `group` (a castle) without widening the teams' mean heights by more than 1 map
 * unit. "interleave" deals A B A B down the heights instead. In a team the lowest spawn gets worm 0 (it moves first: it
 * floods first). */
function balanceSpawns(items: { arg: SpawnArg; map: Vec3; ground: number }[], b: Balance): number[] {
  const teams = b.teams ?? 2, per = b.wormsPerTeam ?? Math.ceil(items.length / teams);
  if (teams * per < items.length) throw new ToolError(`balance: ${teams} teams of ${per} hold ${teams * per} worms, fewer than the ${items.length} spawns`);
  const order = items.map((_, i) => i).sort((x, y) => items[y].ground - items[x].ground);
  const team = new Array<number>(items.length);
  order.forEach((idx, rank) => {
    const lap = Math.floor(rank / teams), k = rank % teams;
    team[idx] = b.mode === "interleave" || lap % 2 === 0 ? k : teams - 1 - k;
  });
  // capacity: a team takes at most `per` (only matters for uneven counts)
  const mean = (t: number) => { const g = items.filter((_, i) => team[i] === t).map((it) => it.ground); return g.length ? g.reduce((x, y) => x + y, 0) / g.length : 0; };
  const spread = () => { const m = Array.from({ length: teams }, (_, t) => mean(t)); return Math.max(...m) - Math.min(...m); };
  const dupes = () => { let d = 0; for (let t = 0; t < teams; t++) { const seen = new Map<string, number>(); items.forEach((it, i) => { if (team[i] === t && it.arg.group) seen.set(it.arg.group, (seen.get(it.arg.group) ?? 0) + 1); }); for (const n of seen.values()) d += n - 1; } return d; };
  const limit = spread() + 1;
  for (let pass = 0; pass < 4; pass++) {
    let better = false;
    for (let i = 0; i < items.length; i++) for (let j = i + 1; j < items.length; j++) {
      if (team[i] === team[j]) continue;
      const d0 = dupes();
      if (!d0) break;
      [team[i], team[j]] = [team[j], team[i]];
      if (dupes() < d0 && spread() <= limit) better = true;
      else [team[i], team[j]] = [team[j], team[i]];
    }
    if (!better) break;
  }
  // worm numbers: per team, lowest ground first
  const worm = new Array<number>(items.length);
  for (let t = 0; t < teams; t++) {
    const mine = items.map((_, i) => i).filter((i) => team[i] === t).sort((x, y) => items[x].ground - items[y].ground);
    mine.forEach((i, k) => { worm[i] = t * per + k; });
  }
  if (worm.some((w) => w > 7)) throw new ToolError("balance: the teams need worm numbers past WORM7 (give fewer teams or worms per team)");
  return worm;
}

/** Per team: its spawns, mean and lowest ground, and the first game turn the water reaches one of them (the project's
 * water and its script's wum.sim.waterSchedule, when it has one). */
async function teamReport(s: Session, units: Units, teams: number, per: number) {
  const store = s.store, k = units === "map" ? 1 : 20, gs = grids(store);
  const r = (v: number) => Math.round(v * k * 100) / 100;
  let waterAt: ((t: number) => number) | null = null, waterFrom: string | undefined, waterWarnings: string[] = [];
  const own = waterMap(store);
  if (own !== null && store.scene.script?.present) {
    try {
      const sw = await scriptWater(s, own);
      if (sw) ({ waterAt, from: waterFrom, warnings: waterWarnings } = sw);
    } catch {
      // no script to read: no flood turns
    }
  }
  const out = [];
  const all = new Map<number, { worm: string; ground: number | null; floods: number | null }>();
  for (let w = 0; w < Math.min(8, teams * per); w++) {
    const d = store.scene.details.find((x) => x.name === TEAM_KNOTS[w]);
    if (!d) continue;
    const p = store.frames.detailWorld(d), g = spansAt(gs, p[0], p[2], p[1] + 1.5)[0]?.top ?? null;
    let floods: number | null = null;
    if (g !== null && waterAt) for (let turn = 1; turn <= 200; turn++) if (waterAt(turn) >= g) { floods = turn; break; }
    all.set(w, { worm: TEAM_KNOTS[w], ground: g, floods });
  }
  // each worm's first two own turns in the engine's rotation (lib/turns.ts), a drowned worm skipped, and the water then
  const rot = rotation({ worms: [...all.keys()], teams, perTeam: per, horizon: 200, drownsAt: (w) => all.get(w)?.floods ?? null });
  for (let t = 0; t < teams; t++) {
    const rows = [...all.entries()].filter(([w]) => slotOf(w, per, teams).team === t).map(([w, x]) => {
      const own = rot.own.get(w) ?? [];
      const at = (k: number) => {
        if (own[k] === undefined) return null;
        const wl = waterAt ? waterAt(own[k]) : null;
        return { gameTurn: own[k], ...(wl !== null ? { water: r(wl), dry: x.ground !== null && x.ground > wl } : {}) };
      };
      return { ...x, first: at(0), second: at(1) };
    });
    const gr = rows.map((x) => x.ground).filter((x): x is number => x !== null);
    const fl = rows.map((x) => x.floods).filter((x): x is number => x !== null);
    out.push({ team: t, worms: rows.map((x) => x.worm), meanHeight: gr.length ? r(gr.reduce((a, b) => a + b, 0) / gr.length) : null, lowest: gr.length ? r(Math.min(...gr)) : null,
      ...(waterAt ? { earliestFloodTurn: fl.length ? Math.min(...fl) : null } : {}),
      ownTurns: rows.map((x) => ({ worm: x.worm, first: x.first, second: x.second })) });
  }
  const wet = out.flatMap((t) => t.ownTurns.filter((x) => x.first?.dry === false || x.second?.dry === false).map((x) => x.worm));
  return { units, teams: out, note: `WORMn is worm n % ${per} of team floor(n / ${per}) (the Quick Game's human team has WORM0-3); ownTurns: each worm's first and second own game turns in the engine's rotation ` +
    "(the teams alternate from team 0 in game turn 1, each moves its living worms in turn, a drowned worm is skipped; game turns count from 1 as wum.sim.turnStarted's do) and the water at their start" +
    (waterAt ? `; earliestFloodTurn: the first game turn the project's water and its script reach one of the team's spawns (water from ${waterFrom})` : "; no script schedule: no flood turns"),
    ...(wet.length ? { wetBeforeSecondTurn: wet } : {}),
    ...(waterWarnings.length ? { waterWarnings } : {}) };
}

/** The knots the spawn mode needs: WORM0-7 (knots), WORM0-15 (teams), the areas' knots (areas). */
function neededKnots(sp: Spawns): string[] {
  if (sp.mode === "areas") return AREA_KNOTS.slice(0, (sp.areas ?? []).reduce((n, a) => n + a.knots, 0));
  return sp.mode === "teams" ? TEAM_KNOTS : KNOTS;
}

function spawnState(store: EditorStore, units: Units) {
  const scene = store.scene;
  const spawns = scene.details.filter((d) => d.role === "spawn").map((d) => detailInfo(store, d, units));
  const have = new Set(scene.details.map((d) => d.name));
  // positions: every knot's place by name, whether this call moved it or not (the same numbers as placed[].pos and spawns[].pos)
  return { spawnMode: scene.spawns.mode, spawns, positions: Object.fromEntries(spawns.map((d) => [d.name, d.pos])), missingKnots: neededKnots(scene.spawns).filter((k) => !have.has(k)) };
}

interface AreaArg { name: string; group: string; knots: PointArgs[]; }

/** spawns.mode areas: the areas' knots are WORM<n> in area order (area 0 from WORM0); each match deals the teams over
 * the areas from its seed (lib/spawn-deal.ts, Melange's DealAreas). */
async function placeSpawnAreas(s: Session, a: { areas?: AreaArg[]; pattern?: string[]; opposite?: (number | string)[][]; spawns?: SpawnArg[]; balance?: unknown; units?: Units; snap?: "tread" } & CameraArgs) {
  const store = s.store, scene = store.scene, units = a.units ?? "world";
  if (a.spawns?.length || a.balance) throw new ToolError("mode areas places its knots per area: give areas [{name, group, knots: [{xz: [x, z]}, ...]}, ...], not spawns or balance");
  const prev = scene.spawns.mode === "areas" ? scene.spawns : null;
  if (!a.areas && !prev) throw new ToolError("mode areas needs areas: [{name, group, knots: [{xz: [x, z]}, ...]}, ...] (each area 1-8 knots)");
  const areas: SpawnArea[] = a.areas ? a.areas.map((ar) => ({ name: ar.name, group: ar.group, knots: ar.knots.length })) : structuredClone(prev!.areas ?? []);
  const index = (v: number | string, where: string) => {
    const i = typeof v === "number" ? v : areas.findIndex((x) => x.name === v);
    if (!Number.isInteger(i) || i < 0 || i >= areas.length) throw new ToolError(`${where}: ${JSON.stringify(v)} is not an area (give an area name or index 0-${areas.length - 1})`);
    return i;
  };
  const opposite = a.opposite ? a.opposite.map((pr, k) => {
    if (pr.length !== 2) throw new ToolError(`opposite[${k}]: give two areas [a, b]`);
    return [index(pr[0], `opposite[${k}]`), index(pr[1], `opposite[${k}]`)] as [number, number];
  }) : a.areas ? undefined : prev?.opposite;
  const pattern = a.pattern ?? (a.areas ? undefined : prev?.pattern);
  const next: Spawns = { mode: "areas", areas, ...(pattern?.length ? { pattern } : {}), ...(opposite?.length ? { opposite } : {}) };
  const bad = spawnsProblems(next);
  if (bad.length) throw new ToolError(bad.join("; "));
  const cmds: Command[] = [];
  if (JSON.stringify(scene.spawns) !== JSON.stringify(next)) cmds.push(new SetLevel({ spawnSetup: next }, "Spawn areas"));
  const snapped: { knot: string; from: Vec3; to: Vec3; why: string[] }[] = [];
  const placed: { area: string; knots: { knot: string; action: "added" | "moved"; dropped: boolean; pos: Vec3 }[] }[] = [];
  (a.areas ?? []).forEach((ar, i) => {
    const first = areaFirstKnot(areas, i), out: (typeof placed)[number]["knots"] = [];
    ar.knots.forEach((pt, k) => {
      const name = `WORM${first + k}`;
      const r = resolvePoint(store, pt, units, name);
      if (a.snap === "tread") {
        const gs = grids(store), t = treadAt(gs, r.map);
        if (!t.ok) {
          const to = nearestTread(gs, r.map);
          if (!to) throw new ToolError(`${name} (area ${ar.name}): ${t.problems.join("; ")}, and no point within 3 map units passes the tread checks: give another position`);
          snapped.push({ knot: name, from: pointOut(r.map, units), to: pointOut(to, units), why: t.problems });
          r.map = to;
        }
      }
      const have = scene.details.find((d) => d.name === name);
      if (have) cmds.push(new SetDetail(have.id, { pos: tidy(store.frames.toLocal(have.frame, r.map), 6) }, `Move ${name}`));
      else {
        const frame = targetFrame(scene, "spawn");
        cmds.push(new AddDetail(frame, { name, resource: KNOT_RESOURCE, pos: tidy(store.frames.toLocal(frame, r.map), 6) }));
      }
      out.push({ knot: name, action: have ? "moved" : "added", dropped: r.dropped, pos: pointOut(r.map, units) });
    });
    placed.push({ area: ar.name, knots: out });
  });
  if (cmds.length) commit(store, cmds.length === 1 ? cmds[0] : new Group("Place spawn areas", cmds));
  const total = areas.reduce((n, x) => n + x.knots, 0);
  const unused = store.scene.details.filter((d) => /^WORM\d+$/.test(d.name) && Number(d.name.slice(4)) >= total).map((d) => d.name);
  const spec = { areas, pattern: next.pattern, opposite: next.opposite };
  return {
    mode: "areas", changed: cmds.length > 0, ...(cmds.length ? {} : { note: "nothing to change" }),
    areas: areas.map((x, i) => ({ index: i, name: x.name, group: x.group, knots: Array.from({ length: x.knots }, (_, k) => `WORM${areaFirstKnot(areas, i) + k}`) })),
    pattern: next.pattern ?? [], opposite: (next.opposite ?? []).map(([p, q]) => [areas[p].name, areas[q].name]),
    ...(placed.length ? { placed } : {}), ...(snapped.length ? { snapped } : {}),
    deal: [2, 3, 4].map((t) => dealPreview(spec, t)),
    dealNote: "each match deals the teams over the areas from its seed (every machine alike): the pattern's steps in order (each an unused area of its group; group:opposite the partner of an area dealt earlier), then the rest in a seeded order, the teams shuffled over them; a team's worms stand on its area's knots in slot order. deal: for 2-4 teams, the sets of areas 1000 seeds give and their share; fixed: the deal without Melange's ergSpawnDeal (the chunk's fallback), by team. A team past the areas, or a worm past its area's knots, keeps the game's random spawn.",
    ...(unused.length ? { unusedKnots: unused, unusedNote: "WORM knots past the areas' are not used in mode areas (remove_object removes them)" } : {}),
    ...spawnState(store, units), issues: issuesOf(store).filter((t) => /WORM|spawn/i.test(t)), warnings: cameraWarnings(store, units, a),
  };
}

async function placeSpawnKnots(s: Session, a: { mode?: "random" | "knots" | "teams" | "areas"; spawns?: SpawnArg[]; units?: Units; balance?: Balance | boolean; snap?: "tread"; areas?: AreaArg[]; pattern?: string[]; opposite?: (number | string)[][] } & CameraArgs) {
  if (a.mode === "areas") return placeSpawnAreas(s, a);
  if (a.areas || a.pattern || a.opposite) throw new ToolError("areas, pattern and opposite are for mode areas");
  const store = s.store, scene = store.scene, units = a.units ?? "world";
  const mode = a.mode ?? "knots";
  const cmds: Command[] = [];
  if (scene.spawns.mode !== mode) cmds.push(new SetLevel({ spawns: mode }, `Spawn mode ${mode}`));
  const seen = new Set<number>();
  const done: { worm: number; action: "added" | "moved"; dropped: boolean; pos: Vec3; group?: string; was?: number }[] = [];
  const snapped: { worm: number; from: Vec3; to: Vec3; why: string[] }[] = [];
  // resolve every point first; with balance the worm numbers are dealt again over them
  const items = (a.spawns ?? []).map((sp) => {
    if (seen.has(sp.worm)) throw new ToolError(`WORM${sp.worm} is given twice`);
    if (sp.worm > 7 && mode !== "teams") throw new ToolError(`WORM${sp.worm}: knots spawns use WORM0-WORM7; WORM8-WORM15 are for mode teams (team floor(n / 4)'s worm n % 4)`);
    seen.add(sp.worm);
    const r = resolvePoint(store, sp, units, `WORM${sp.worm}`);
    // snap: tread moves a point a worm cannot stand on to the nearest one it can
    if (a.snap === "tread") {
      const gs = grids(store), t = treadAt(gs, r.map);
      if (!t.ok) {
        const to = nearestTread(gs, r.map);
        if (!to) throw new ToolError(`WORM${sp.worm}: ${t.problems.join("; ")}, and no point within 3 map units passes the tread checks: give another position`);
        snapped.push({ worm: sp.worm, from: pointOut(r.map, units), to: pointOut(to, units), why: t.problems });
        r.map = to;
      }
    }
    const g = spansAt(grids(store), r.map[0], r.map[2], r.map[1] + 1.5)[0]?.top ?? r.map[1];
    return { arg: sp, map: r.map, dropped: r.dropped, ground: g };
  });
  const bal: Balance | null = a.balance === true ? {} : a.balance ? a.balance : null;
  if (bal) {
    if (items.length < 2) throw new ToolError("balance deals the worm numbers over the given spawns: give at least two");
    const dealt = balanceSpawns(items, bal);
    items.forEach((it, i) => { it.arg = { ...it.arg, worm: dealt[i] }; (it as { was?: number }).was = (a.spawns ?? [])[i].worm; });
  }
  for (const it of items) {
    const sp = it.arg, map = it.map, dropped = it.dropped;
    const name = TEAM_KNOTS[sp.worm];
    const have = scene.details.find((d) => d.name === name);
    if (have) {
      cmds.push(new SetDetail(have.id, { pos: tidy(store.frames.toLocal(have.frame, map), 6) }, `Move ${name}`));
      done.push({ worm: sp.worm, action: "moved", dropped, pos: pointOut(map, units), ...(sp.group ? { group: sp.group } : {}), ...(bal ? { was: (it as { was?: number }).was } : {}) });
    } else {
      const frame = targetFrame(scene, "spawn");
      cmds.push(new AddDetail(frame, { name, resource: KNOT_RESOURCE, pos: tidy(store.frames.toLocal(frame, map), 6) }));
      done.push({ worm: sp.worm, action: "added", dropped, pos: pointOut(map, units), ...(sp.group ? { group: sp.group } : {}), ...(bal ? { was: (it as { was?: number }).was } : {}) });
    }
  }
  // mode teams: WORMn is team floor(n / 4)'s worm n % 4, whatever the slots (the generated chunk deals them so)
  const teams = bal?.teams ?? (mode === "teams" ? 4 : 2), per = mode === "teams" ? 4 : bal?.wormsPerTeam ?? (bal ? Math.ceil(items.length / teams) : 4);
  if (!cmds.length) return { mode, changed: false, note: "nothing to change", ...spawnState(store, units), teams: await teamReport(s, units, teams, per), warnings: cameraWarnings(store, units, a) };
  commit(store, cmds.length === 1 ? cmds[0] : new Group(a.spawns?.length ? "Place spawn knots" : `Spawn mode ${mode}`, cmds));
  return { mode, changed: true, placed: done, ...(snapped.length ? { snapped } : {}), ...(bal ? { balanced: { mode: bal.mode ?? "height", teams, wormsPerTeam: per, note: "worm numbers dealt over the given positions (was: the worm asked for): heights snaked over the teams, a team's spawns spread over groups, the lowest of a team moving first" } } : {}),
    ...spawnState(store, units), teams: await teamReport(s, units, teams, per), issues: issuesOf(store).filter((t) => /WORM|spawn/i.test(t)), warnings: cameraWarnings(store, units, a) };
}

// ------------------------------------------------------------------ objects

/** The highest the project's script raises the water (map units) over 200 turns: simulated where the server can run
 * the script, else its first wum.sim.waterSchedule's cap; null without one. */
export async function scriptWaterTop(s: Session): Promise<number | null> {
  const p = s.requireProject();
  if (!p.store.scene.script?.present) return null;
  try {
    const sw = await scriptWater(s, waterMap(p.store) ?? -50);
    if (!sw) return null;
    if (!sw.simulated) {
      const sc = scheduleFromScript((await p.erg.script()) ?? "");
      return sc ? sc.cap / (sc.units === "map" ? 1 : 20) : null;
    }
    let top = -Infinity;
    for (let t = 1; t <= sw.turns; t++) top = Math.max(top, sw.waterAt(t));
    return Number.isFinite(top) ? top : null;
  } catch {
    return null;
  }
}

/** The mine factory checked as the game places it (lib/factory-check.ts), as answer lines in the caller's units. */
export async function factoryReport(s: Session, units: Units): Promise<string[]> {
  const k = units === "map" ? 1 : 20;
  return factoryLines(checkFactory(s.store, (v) => `${Math.round(v * k * 100) / 100} ${units}`, await scriptWaterTop(s)));
}

const OBJECT_TYPES = ["crate", "health", "telepad", "trigger", "mine", "oildrum", "minefactory"] as const;
type PlaceType = (typeof OBJECT_TYPES)[number];

interface PlaceArgs extends PointArgs {
  type: PlaceType; units?: Units; kind?: "weapon" | "health" | "utility"; contents?: string; count?: number; amount?: number;
  hitpoints?: number; parachute?: boolean; pos2?: number[]; index?: number; radius?: number; teamCollect?: number; teamDestroy?: number; wormCollect?: boolean;
  activation?: number; inactiveTurns?: number; schemeMines?: boolean; keepKnot?: boolean; launchHeight?: number; launchRadius?: number; launch?: boolean; snap?: "surface";
  launchers?: number[][];
  throw?: ThrowArg | false;
}
/** set_mine_factory / place_object's `throw`: the spec's fields, targets in the call's units ([x, z]). */
interface ThrowArg { minRange?: number; maxRange?: number; apex?: number; pattern?: "random" | "ring" | "targets"; avoidWater?: boolean; targets?: number[][]; scatter?: number; targetShare?: number;
  volley?: number; avoidRadius?: number; avoid?: number[][][]; }

type FactoryArgs = Pick<PlaceArgs, "activation" | "inactiveTurns" | "schemeMines" | "keepKnot" | "launchHeight" | "launchRadius" | "launch"> & {
  launchers?: [number, number, number][]; throw?: (Omit<ThrowArg, "targets" | "avoid"> & { targets?: [number, number][]; avoid?: [number, number][][] }) | boolean };
const FACTORY_GAME = { activation: 20, inactiveTurns: 7, schemeMines: true, keepKnot: false } as const;

/** The factory settings after `a` is applied to `f`: a value equal to the game's is dropped (the game's value stays). */
function factoryOf(f: FactorySpec | undefined, a: FactoryArgs): FactorySpec | undefined {
  const next: Record<string, unknown> = { ...(f ?? {}) };
  for (const k of ["activation", "inactiveTurns", "schemeMines", "keepKnot"] as const) {
    const v = a[k];
    if (v === undefined) continue;
    if (v === FACTORY_GAME[k]) delete next[k];
    else next[k] = v;
  }
  // the launch: launchHeight 0 or launch false gives the game's placement back
  if (a.launch === false || a.launchHeight === 0) { delete next.launchHeight; if (a.launch === false) delete next.launchRadius; }
  else if (a.launchHeight !== undefined) next.launchHeight = a.launchHeight;
  if (a.launchRadius !== undefined && a.launch !== false) next.launchRadius = a.launchRadius;
  const out = next as FactorySpec;
  if (a.launch === false || (a.launchers && !a.launchers.length)) delete out.launchers;
  else if (a.launchers) out.launchers = a.launchers;
  // the throw: merged into the one there (so a later call can change one field), stored as the server stores it
  if (a.launch === false || a.throw === false) delete out.throw;
  else if (a.throw === true) throw new ToolError("throw: give its settings as an object ({maxRange, ...}), or false to clear the throw");
  else if (a.throw) {
    const merged = { ...(out.throw ?? {}), ...a.throw } as Record<string, unknown>;
    if (typeof merged.maxRange !== "number") throw new ToolError("throw.maxRange: the farthest landing point (map units from the launch point) is required");
    if (typeof merged.minRange === "number" && merged.minRange > (merged.maxRange as number)) throw new ToolError("throw.minRange must not be over throw.maxRange");
    if (merged.pattern === "targets" && !(Array.isArray(merged.targets) && merged.targets.length)) throw new ToolError("throw.pattern targets needs throw.targets");
    if (Array.isArray(merged.avoid) && merged.avoid.length && !merged.avoidRadius) throw new ToolError("throw.avoid needs throw.avoidRadius (map units the mines keep off each line)");
    out.throw = storedThrow(merged) as unknown as FactorySpec["throw"];
  }
  return Object.keys(out).length ? out : undefined;
}

/** The throw's targets and avoid lines in whole world units ([x, z] in `units`). */
function throwWorld(t: ThrowArg | boolean | undefined, units: Units): FactoryArgs["throw"] {
  if (!t || t === true) return t;
  const { targets, avoid, ...rest } = t;
  const k = units === "map" ? 20 : 1;
  const point = (p: number[], what: string): [number, number] => {
    if (!Array.isArray(p) || p.length !== 2 || !p.every(Number.isFinite)) throw new ToolError(`${what}: [x, z]`);
    const w = [Math.round(p[0] * k), Math.round(p[1] * k)] as [number, number];
    if (w.some((v) => Math.abs(v) > 100000)) throw new ToolError(`${what}: within 100000 world units`);
    return w;
  };
  const out: FactoryArgs["throw"] = { ...rest };
  if (targets !== undefined) {
    if (targets.length > 16) throw new ToolError("throw.targets: at most 16");
    out.targets = targets.map((p, i) => point(p, `throw.targets[${i}]`));
  }
  if (avoid !== undefined) {
    if (!Array.isArray(avoid) || avoid.length > 8) throw new ToolError("throw.avoid: at most 8 lines");
    out.avoid = avoid.map((ln, i) => {
      if (!Array.isArray(ln) || ln.length < 2 || ln.length > 32) throw new ToolError(`throw.avoid[${i}]: 2-32 [x, z] points (thin a longer line out)`);
      return ln.map((p, j) => point(p, `throw.avoid[${i}][${j}]`));
    });
  }
  return out;
}

/** Mine gravity (world units per ms^2) and the eruption levels/factory.cpp throws a launcher's mines on, for the answers. */
const MINE_GRAVITY = 2.5e-4;
function launchPlan(store: EditorStore, f: FactorySpec) {
  const c = checkFactory(store, (v) => String(v));
  if (!c) return [];
  if (f.throw) {
    // the throw: from each launcher (or the factory), landing points minRange-maxRange out; the longest arc's flight,
    // level ground assumed (levels/factory.cpp tops the arc out apex over the higher end)
    const t = f.throw, apex = (t.apex ?? 15) * 20, up = Math.sqrt(2 * MINE_GRAVITY * apex), ms = 2 * up / MINE_GRAVITY;
    const from = f.launchers?.length ? f.launchers : [[c.knot[0] * 20, c.knot[1] * 20, c.knot[2] * 20] as [number, number, number]];
    return from.map((l, i) => ({ launcher: f.launchers?.length ? i : "factory", at: l.map((v) => v / 20), landsBetween: [t.minRange ?? 0, t.maxRange], pattern: String(t.pattern ?? "random"),
      apexOverEnds: apex / 20, flightSeconds: Math.round(ms / 100) / 10, sidewaysAtMax: Math.round(t.maxRange * 20 / ms * 1000) / 1000,
      ...(t.targets?.length ? { targets: t.targets.map((g) => [g[0] / 20, g[1] / 20]), scatter: t.scatter ?? 10, targetShare: t.targetShare ?? 50 } : {}),
      ...(t.volley ? { volley: t.volley } : {}), ...(t.avoid?.length ? { avoidLines: t.avoid.length, avoidRadius: t.avoidRadius ?? 0 } : {}),
      landing: "Melange lands each mine where its flight first meets the land (the engine's own land sweep), so it rests on the ground it hits; a held mine the sea reaches is handed to the engine's drowning" }));
  }
  if (!f.launchers) return [];
  const h = (f.launchHeight ?? 20) * 20, up = Math.sqrt(2 * MINE_GRAVITY * h);
  const ms = up / MINE_GRAVITY + Math.sqrt(2 * (h + 20) / MINE_GRAVITY);
  return f.launchers.map((l, i) => ({ launcher: i, at: l.map((v) => v / 20), toFactory: Math.round(Math.hypot(l[0] / 20 - c.knot[0], l[2] / 20 - c.knot[2]) * 10) / 10,
    apex: Math.round((l[1] + 20 + h) / 2) / 10, flightSeconds: Math.round(ms / 100) / 10, landsWithin: f.launchRadius ?? 0 }));
}

/** The launchers argument in world units for the spec: [x, y, z] as given, [x, z] on the top of the ground in that
 * column (the highest solid top, a draped skin at its sheared height), each rounded to a whole world unit. */
function launchersWorld(store: EditorStore, pts: number[][] | undefined, units: Units): [number, number, number][] | undefined {
  if (pts === undefined) return undefined;
  if (pts.length > 8) throw new ToolError("launchers: at most 8 (the factory's mines are shared out over them)");
  const k = units === "map" ? 1 : 1 / 20;
  return pts.map((p, i) => {
    if (!Array.isArray(p) || (p.length !== 2 && p.length !== 3) || !p.every(Number.isFinite)) throw new ToolError(`launchers[${i}]: [x, y, z] or [x, z]`);
    let map: Vec3;
    if (p.length === 3) map = [p[0] * k, p[1] * k, p[2] * k];
    else {
      const tops = spansAt(grids(store), p[0] * k, p[1] * k).map((s) => s.top);
      if (!tops.length) throw new ToolError(`launchers[${i}]: no ground in the column at (${p[0]}, ${p[1]}): give [x, y, z]`);
      map = [p[0] * k, Math.max(...tops), p[1] * k];
    }
    const w = map.map((v) => Math.round(v * 20)) as [number, number, number];
    if (w.some((v) => Math.abs(v) > 100000)) throw new ToolError(`launchers[${i}]: within 100000 world units`);
    return w;
  });
}

/** everyNTurns' target: the factory fires when fewer mines than this are in the level, so 255 never stops a fire. */
const EVERY_N_ACTIVATION = 255;
/** How the factory decides, from its turn-start handler (levels/factory.cpp, 0x5cfe40), for the answers. */
const FACTORY_RULE = { rule: "at each game turn start the factory takes one from its countdown (set to inactiveTurns when it is placed and after each fire); at 0 or under it fires when the level holds fewer mines than activation, min(activation - mines, 10) of them, and a fire waits while the mines are not fewer: a target of 16 with 10 mines from the first fire still standing gives a second fire of 6, then none until mines go off. everyNTurns makes it depend on the turn count alone." };

const factoryProps: Record<string, Schema> = {
  activation: { type: "integer", minimum: 0, maximum: 255, description: "minefactory: it fires at a turn start when fewer than this many mines are in the level, topping up to it, at most 10 per fire (game 20; 0 never fires)." },
  inactiveTurns: { type: "integer", minimum: 0, maximum: 255, description: "minefactory: turn starts it waits after it is placed and after each fire before it checks again (game 7; 0 checks every turn)." },
  schemeMines: { type: "boolean", description: "minefactory: false = the scheme's 15 random start mines are not placed, so mines come only from the factory (oil drums stay; default true)." },
  keepKnot: { type: "boolean", description: "minefactory: true = when the game's drop from the knot hits no land (a draped skin, the crater pads on Caldera) the factory stays at its knot instead of the match's stale sweep point, where it was lost (Melange 0.13+; default false, the game's own drop). Put the knot on the surface (place_object snap: \"surface\")." },
  launchHeight: { type: "integer", minimum: 0, maximum: 255, description: "minefactory: map units over the factory its mines appear at when it fires (Melange 0.13+; with launchRadius alone, 20): they fall from there round the factory, instead of the game's placement, which rains them onto random worms and objects (on Caldera 28-95 map units from the factory). 0 gives the game's placement back." },
  launchRadius: { type: "integer", minimum: 0, maximum: 255, description: "minefactory: map units round the factory its mines appear within (an even spread over the disc; default 0, straight over it), so they land on the cone round a crater." },
  launch: { type: "boolean", description: "minefactory: false clears launchHeight, launchRadius and launchers (the game's placement)." },
  throw: { anyOf: [{ type: "boolean" }, { type: "object", additionalProperties: false, properties: {
    minRange: { type: "integer", minimum: 0, maximum: 500, description: "The nearest landing point, map units from the launch point (default 0)." },
    maxRange: { type: "integer", minimum: 1, maximum: 500, description: "The farthest landing point, map units (required the first time)." },
    apex: { type: "integer", minimum: 1, maximum: 255, description: "Map units the arc tops out over the higher of its ends (default 15): higher clears ridges and takes longer." },
    pattern: { type: "string", enum: ["random", "ring", "targets"], description: "random (default): any bearing, even over the band; ring: a fire's mines spread evenly round the compass; targets: targetShare % of them at one of `targets`, the rest random." },
    avoidWater: { type: "boolean", description: "A landing point whose ground is under the water is drawn again (default true; 6 draws, then straight up)." },
    targets: { type: "array", maxItems: 16, items: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 }, description: "[x, z] points in `units` (castle courts, say) for pattern targets; stored as whole world units." },
    scatter: { type: "integer", minimum: 0, maximum: 255, description: "Map units round a target a mine aimed at it lands within (default 10)." },
    targetShare: { type: "integer", minimum: 0, maximum: 100, description: "Percent of a fire's mines aimed at the targets, the rest random over the band (default 50)." },
    volley: { type: "integer", minimum: 0, maximum: 10, description: "At most this many mines a fire, whatever the engine's min(activation - mines, 10) asked (default 0: the engine's count): a smaller volley more often spreads the landings over the match." },
    avoidRadius: { type: "integer", minimum: 0, maximum: 50, description: "Map units the landing points keep off each `avoid` line (default 0: none); the blast of a mine reaches about 2.5 map units, so a trail bed half 2.6 wide wants 5." },
    avoid: { type: "array", maxItems: 8, items: { type: "array", minItems: 2, maxItems: 32, items: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 } },
      description: "Up to 8 polylines of 2-32 [x, z] points in `units` (the trails, say): a landing point within avoidRadius of one is drawn again, so the mines keep off the corridors worms walk; stored as whole world units. [] clears them." } } }],
    description: "minefactory (the erg-mcp branch's Melange): throw the mines OUT instead of straight up: each mine of a fire is lobbed from its launcher (the launchers in turn; without launchers, from the factory) at a landing point minRange-maxRange map units away whose ground the game's own land sweep finds, on a ballistic arc topping out `apex` over the higher end, and Melange lands it where its flight first meets the land (the engine's mine collision let mines through some ground, and landing them at the straight sweep's hit also leaves no sideways speed to sink on: docs/erg.md). Later calls merge into the throw there (give one field to change it); false clears it (launch: false does too). The answer's launchPlan gives each launch point's band, pattern, flight and sideways speed." },
  launchers: { type: "array", maxItems: 8, items: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 3 },
    description: "minefactory: 1-8 launch points, [x, y, z] or [x, z] (on the top of the ground in that column), in `units` (Melange 0.13+ of this branch): a fire's mines are shared out over them (from a random one, in turn) and each erupts straight up from a spot within launchRadius of its launcher (1 map unit over the launcher's height, the fire's spots kept 1.5 map units apart where they can be), tops out launchHeight (default 20) over it and falls back onto the spot: stand each launcher on flat ground at least launchRadius wide (a pad) and check with game_mine_drop that the ground there holds a mine (on Caldera half the crater did not: the mine fell through to the sea). The game runs one factory a match (a second trips its 'Only allowed one mine factory at a time' assert, tried 2026-11-08), so these are the one factory's launch points. [] clears them. A mine's flight is the engine's (velocity in world units per ms, falling at 2.5e-4 world units/ms^2, measured 2026-11-08)." },
};

/** pos with snap "surface": the knot on the top of the ground in its column, the highest solid top at most 1 map unit
 * over pos's y (a draped skin at its sheared height), so an object sits exactly there with no drop. */
function snapToSurface(store: EditorStore, map: Vec3, what: string): Vec3 {
  const tops = spansAt(grids(store), map[0], map[2]).map((s) => s.top).filter((t) => t <= map[1] + 1);
  if (!tops.length) throw new ToolError(`${what}: snap "surface" found no ground at or under the point (x ${tidy([map[0], 0, 0])[0]}, z ${tidy([0, 0, map[2]])[2]} map)`);
  return [map[0], Math.max(...tops), map[2]];
}

function crateOf(a: PlaceArgs): CrateSpec {
  const kind = a.type === "health" ? "health" : a.kind ?? "health";
  const hitpoints = a.hitpoints ?? 25, parachute = a.parachute ?? false;
  if (kind === "health") return { kind, amount: a.amount ?? 25, hitpoints, parachute };
  if (!a.contents) throw new ToolError(`a ${kind} crate needs contents (the weapon or utility name)`);
  return { kind, contents: a.contents, ...(a.count !== undefined ? { count: a.count } : {}), hitpoints, parachute };
}

/** The install's own name for crate contents (level.objects lists kWeaponBazooka, kUtilityJetpack; export refuses any
 * other spelling). A short name that matches one entry is replaced by it; anything else is kept with a warning. */
async function contentsName(s: Session, c: CrateSpec): Promise<{ contents?: string; note?: string }> {
  if (c.kind === "health") return {};
  try {
    const cat = catalogOf(await (await s.client()).call("level.objects", {}));
    const list = c.kind === "weapon" ? cat.weapons : cat.utilities;
    if (!list.length || list.includes(c.contents)) return {};
    const short = (n: string) => n.replace(/^k(Weapon|Utility)/, "").toLowerCase();
    const same = list.filter((n) => short(n) === short(c.contents));
    if (same.length === 1) return { contents: same[0], note: `'${c.contents}' is called ${same[0]} in this install` };
    return { note: `'${c.contents}' is not among the install's ${c.kind === "weapon" ? "weapons" : "utilities"} (${list.slice(0, 12).join(", ")}${list.length > 12 ? ", ..." : ""}); export will refuse it` };
  } catch {
    return {};   // the catalogue is optional
  }
}

async function placeObject(s: Session, a: PlaceArgs) {
  const store = s.store, scene = store.scene, units = a.units ?? "world";
  if (a.snap && (a.pos === undefined || a.drop)) throw new ToolError("snap \"surface\" takes pos (the y says which surface: the highest at most 1 map unit over it) and no drop");
  const r0 = resolvePoint(store, a, units, a.type);
  const dropped = r0.dropped, map = a.snap ? snapToSurface(store, r0.map, a.type) : r0.map;
  let cmd: AddDetail | AddObjects, note: string | undefined;
  if (a.type === "mine" || a.type === "oildrum") {
    const r = place(scene, store.frames, BUILTIN.find((e) => e.id === a.type)!, map, null);
    if (typeof r === "string") throw new ToolError(r);
    cmd = r as AddDetail;
  } else {
    const type: ObjectType = a.type === "health" ? "crate" : a.type;
    const specs: ObjectSpec[] = [];
    const at: Vec3[] = [];
    const frame = targetFrame(scene, "object");
    const local = (p: Vec3) => tidy(store.frames.toLocal(frame, p), 6);
    if ((scene.objects?.length ?? 0) + (type === "telepad" ? 2 : 1) > LIMITS.objects) throw new ToolError(`a level holds at most ${LIMITS.objects} objects`);
    if (type === "telepad") {
      const g = nextTelepadGroup(scene);
      if (g === null) throw new ToolError(`all ${LIMITS.telepadGroups} telepad groups are used`);
      const k1 = nextObjectKnot(scene, type, g), k2 = k1 && nextObjectKnot(scene, type, g, [k1]);
      if (!k1 || !k2) throw new ToolError(`telepad group ${g} has no free knot`);
      specs.push(defaultObject(type, k1, g), defaultObject(type, k2, g));
      const second = a.pos2 ? resolvePoint(store, { pos: a.pos2, drop: a.drop }, units, "pos2").map : ([map[0] + PAD_SPACING, map[1], map[2]] as Vec3);
      at.push(local(map), local(second));
    } else {
      const knot = nextObjectKnot(scene, type);
      if (!knot) throw new ToolError(type === "minefactory" ? "the level already has a mine factory" : `no free ${type} knot is left`);
      const spec = defaultObject(type, knot);
      if (spec.type === "crate") {
        spec.crate = crateOf(a);
        const r = await contentsName(s, spec.crate);
        if (r.contents && spec.crate.kind !== "health") spec.crate.contents = r.contents;
        note = r.note;
      } else if (spec.type === "trigger") {
        const t = spec.trigger;
        if (a.index !== undefined) t.index = a.index;
        if (a.radius !== undefined) t.radius = a.radius;
        if (a.teamCollect !== undefined) t.teamCollect = a.teamCollect;
        if (a.teamDestroy !== undefined) t.teamDestroy = a.teamDestroy;
        if (a.hitpoints !== undefined) t.hitpoints = a.hitpoints;
        if (a.wormCollect !== undefined) t.wormCollect = a.wormCollect;
      } else if (spec.type === "minefactory") {
        const f = factoryOf(undefined, { ...a, launchers: launchersWorld(store, a.launchers, units), throw: throwWorld(a.throw, units) });
        if (f) spec.factory = f;
      }
      specs.push(spec);
      at.push(local(map));
    }
    cmd = new AddObjects(frame, specs, at, `Add ${a.type}`);
  }
  commit(store, cmd);
  const ids = cmd instanceof AddObjects ? cmd.ids : [cmd.detailId];
  const factory = a.type === "minefactory" ? await factoryReport(s, units) : [];
  return {
    placed: infos(s, ids, units), dropped, ...(a.snap ? { snapped: pointOut(map, units) } : {}), ...(note ? { warning: note } : {}),
    objects: `${scene.objects?.length ?? 0} of ${LIMITS.objects}`,
    issues: [...issuesOf(store).filter((t) => ids.some((id) => t.includes(`#${id} `))), ...factory],
    ...(factory.some((l) => l.startsWith("error:")) ? { factoryWillBeLost: true } : {}),
  };
}

/** place_object {objects}: each item placed as a call of its own would place it (one undo step each), in order; an item
 * that fails is listed with its index, label and why, and the rest go on. */
async function placeObjects(s: Session, a: { objects: (PlaceArgs & { label?: string })[]; units?: Units }) {
  const placed: object[] = [], failed: object[] = [];
  for (const [i, item] of a.objects.entries()) {
    const { label, ...one } = item;
    const name = label ?? `objects[${i}] (${item.type})`;
    try {
      const r = await placeObject(s, { ...one, units: one.units ?? a.units });
      placed.push({ index: i, ...(label ? { label } : {}), ...r });
    } catch (e) {
      failed.push({ index: i, ...(label ? { label } : {}), type: item.type, ...(item.xz ? { xz: item.xz } : {}), ...(item.pos ? { pos: item.pos } : {}),
        error: `${name}: ${e instanceof Error ? e.message : String(e)}` });
    }
  }
  return { placedCount: placed.length, failedCount: failed.length, ...(failed.length ? { failed } : {}), placed,
    objects: `${s.store.scene.objects?.length ?? 0} of ${LIMITS.objects}`, undo: "each placed item is one undo step" };
}

// The editor's "outside every terrain frame" warning: a base's cameras, look-at points and props stay where they were
// after remove_base_frames empties the frames under them (about 21 on Caldera), and say nothing about the level.
const OUTSIDE = /outside every terrain frame/;

async function listObjects(s: Session, a: { role?: string; search?: string; limit?: number; units?: Units; outsideFrames?: "list" | "summary" | "none" } & CameraArgs) {
  const store = s.store, scene = store.scene, units = a.units ?? "world";
  const q = a.search?.toLowerCase();
  const roles = a.role ? [a.role] : ["spawn", "object"];
  const all = scene.details.filter((d) => roles.includes(d.role) && (!q || `${d.name} ${d.resource}`.toLowerCase().includes(q)));
  // world units (this tool's default) also give each position in map units, the unit of check_reachability, query_ground
  // and the other inspect tools
  const listed = all.slice(0, a.limit ?? 200).map((d) => {
    const info = detailInfo(store, d, units);
    return units === "world" ? { ...info, mapPos: pointOut(store.frames.detailWorld(d), "map") } : info;
  });
  const editor = issuesOf(store, Infinity), outside = editor.filter((t) => OUTSIDE.test(t)), rest = editor.filter((t) => !OUTSIDE.test(t));
  const mode = a.outsideFrames ?? "list";
  const shown = mode === "list" ? issuesOf(store) : [...rest.slice(0, 30), ...(mode === "summary" && outside.length ? [`warn: ${outside.length} detail${outside.length === 1 ? " is" : "s are"} outside every terrain frame (a removed base's cameras, look-at points and props; outsideFrames: "list" names them)`] : [])];
  return {
    spawnMode: scene.spawns.mode, water: scene.water.level, units, matching: all.length, listed: listed.length,
    spawns: listed.filter((d) => d.role === "spawn"),
    levelObjects: listed.filter((d) => d.object),
    placed: listed.filter((d) => d.role !== "spawn" && !d.object),
    counts: { objects: `${scene.objects?.length ?? 0} of ${LIMITS.objects}`, details: scene.details.length },
    issues: [...shown, ...cameraWarnings(store, units, a), ...(await factoryReport(s, units))],
    ...(units === "world" ? { unitsNote: "pos and localPos are world units (this tool's default; units: \"map\" lists map units); mapPos is the same position in map units, as check_reachability, query_ground and the other inspect tools give them" } : {}),
  };
}

// ------------------------------------------------------------------ move, rotate, scale, remove, drop

/** SetDetail commands as one undo step (a single one stays plain). */
function setFields(s: Session, changes: [number, DetailFields][], label: string) {
  const cmds = changes.map(([id, f]) => new SetDetail(id, f, label));
  commit(s.store, cmds.length === 1 ? cmds[0] : new Group(label, cmds));
}

function moveDetails(s: Session, a: { id?: number; ids?: number[]; pos?: number[]; by?: number[]; drop?: boolean; units?: Units }) {
  const store = s.store, units = a.units ?? "world";
  const ids = idsOf(a);
  if ((a.pos === undefined) === (a.by === undefined)) throw new ToolError("give exactly one of pos (new position, one detail) or by (offset, any number)");
  if (a.pos && ids.length !== 1) throw new ToolError("pos moves one detail: use by for several");
  const changes = ids.map((id): [number, DetailFields] => {
    const d = findDetail(store, id);
    const from = store.frames.detailWorld(d);
    let to: Vec3 = a.pos ? pointToMap([a.pos[0], a.pos[1], a.pos[2]], units) : (() => {
      const b = pointToMap([a.by![0], a.by![1], a.by![2]], units);
      return [from[0] + b[0], from[1] + b[1], from[2] + b[2]] as Vec3;
    })();
    if (a.drop) to = resolvePoint(store, { pos: to, drop: true }, "map", `detail ${id}`).map;
    return [id, { pos: tidy(store.frames.toLocal(d.frame, to), 6) }];
  });
  setFields(s, changes, ids.length === 1 ? `Move ${store.detail(ids[0])!.name}` : "Move details");
  return { moved: infos(s, ids, units) };
}

function dropDetails(s: Session, a: { id?: number; ids?: number[]; units?: Units }) {
  const store = s.store, ids = idsOf(a);
  const changes = ids.map((id): [number, DetailFields] => {
    const d = findDetail(store, id);
    const hit = resolvePoint(store, { pos: [...store.frames.detailWorld(d)], drop: true }, "map", `detail ${id}`).map;
    return [id, { pos: tidy(store.frames.toLocal(d.frame, hit), 6) }];
  });
  setFields(s, changes, "Drop to ground");
  return { dropped: infos(s, ids, a.units ?? "world") };
}

function transformable(store: EditorStore, d: Detail, what: string) {
  if (store.translationOnly.has(d.frame)) throw new ToolError(`detail ${d.id} (${d.name}) is in a frame that swings in the game: only move works, not ${what}`);
}

function turn(s: Session, a: { id?: number; ids?: number[]; rot?: number[]; by?: number[]; units?: Units }) {
  const store = s.store, ids = idsOf(a);
  if ((a.rot === undefined) === (a.by === undefined)) throw new ToolError("give exactly one of rot (absolute, degrees) or by (added, degrees)");
  const changes = ids.map((id): [number, DetailFields] => {
    const d = findDetail(store, id);
    transformable(store, d, "rotate");
    const deg = a.rot ?? [d.rot[0] * DEG + a.by![0], d.rot[1] * DEG + a.by![1], d.rot[2] * DEG + a.by![2]];
    return [id, { rot: tidy([deg[0] / DEG, deg[1] / DEG, deg[2] / DEG], 6) }];
  });
  setFields(s, changes, "Rotate");
  return { rotated: infos(s, ids, a.units ?? "world") };
}

function resize(s: Session, a: { id?: number; ids?: number[]; scale?: number[]; by?: number; uniform?: number; units?: Units }) {
  const store = s.store, ids = idsOf(a);
  if ([a.scale, a.by, a.uniform].filter((v) => v !== undefined).length !== 1) throw new ToolError("give exactly one of scale ([x, y, z]), uniform (one number) or by (a factor)");
  const changes = ids.map((id): [number, DetailFields] => {
    const d = findDetail(store, id);
    transformable(store, d, "scale");
    const v: Vec3 = a.by !== undefined ? [d.scale[0] * a.by, d.scale[1] * a.by, d.scale[2] * a.by]
      : a.uniform !== undefined ? [a.uniform, a.uniform, a.uniform] : [a.scale![0], a.scale![1], a.scale![2]];
    if (v.some((x) => !(x > 0))) throw new ToolError("scale must be positive");
    return [id, { scale: tidy(v, 6) }];
  });
  setFields(s, changes, "Scale");
  return { scaled: infos(s, ids, a.units ?? "world") };
}

function removeDetails(s: Session, a: { id?: number; ids?: number[]; units?: Units }) {
  const store = s.store, ids = idsOf(a);
  const gone = ids.map((id) => {
    const d = findDetail(store, id);
    return { ...detailInfo(store, d, a.units ?? "world"), ...(objectOf(store.scene, d) ? { alsoRemoved: "its level object" } : {}) };
  });
  commit(store, ids.length === 1 ? new RemoveDetail(ids[0]) : new Group("Delete details", ids.map((id) => new RemoveDetail(id))));
  store.select([]);
  return { removed: gone, issues: issuesOf(store).filter((t) => !/outside every terrain/.test(t)) };
}

// ------------------------------------------------------------------ level settings

function setLevel(s: Session, settings: LevelSettings, label: string, report: () => object) {
  commit(s.store, new SetLevel(settings, label));
  return report();
}

// ------------------------------------------------------------------ shipping

/** Saves first when there are unsaved edits (export and Test work on the saved project, as in the panel). */
/** test {restart}: from a running Test match back to the main menu (level.test.quit), waiting until the game is out
 * of the match and not loading; at the menu already, nothing. Reads the game through the client console: inMatch(),
 * since scene() still says "loading" through a match's first minute. */
async function backToMenu(s: Session): Promise<{ fromScene: string; ms: number; retries?: number } | undefined> {
  const conn = await s.ensure(), client = await s.client(), t0 = Date.now();
  if (!conn.methods.includes("lua.eval")) throw new ToolError("restart needs lua.eval (the game's console) to see where the game is");
  const scene = async () => {
    const r = await client.call<{ ok: boolean; text: string }>("lua.eval", { target: "client", code: "return wum.game.inMatch() and \"match\" or wum.game.scene()" }, 15000);
    return r.ok ? r.text : "?";
  };
  const from = await scene();
  if (from !== "match" && from !== "loading") return undefined;
  if (!conn.methods.includes("level.test.quit")) throw new ToolError("this game has no level.test.quit: leave the match from the game's menu, then test");
  // a match still loading (its intro) is left once it is a match; the quit is sent once
  let sent = false;
  for (let now = from; ; now = await scene().catch(() => "?")) {
    if (now === "match" && !sent) {
      await client.call("level.test.quit", {}, 15000).catch((e: Error) => { throw new ToolError(`could not leave the match: ${e.message}`); });
      sent = true;
    } else if (now !== "match" && now !== "loading" && now !== "?") break;
    if (Date.now() - t0 > 60000) throw new ToolError(`the game did not reach the menu within 60 s (scene '${now}')`);
    await new Promise((ok) => setTimeout(ok, 500));
  }
  await new Promise((ok) => setTimeout(ok, 4000));   // the frontend settles before Quick Game is posted
  return { fromScene: from, ms: Date.now() - t0 };
}

/** What the game applied of the level's look once the Test plays: its time of day (level.test.applied: the frontend's
 * value cleared so the Test build's loads, or why not) and the match's theme (wum.game.theme(), the game's Land.Theme),
 * against what was asked; warnings where one fell back. Fields the game cannot answer are left out. A game of Melange
 * 0.13+ also answers the sim mods and level scripts loaded in the match (`simModsLoaded`) and the mine factory as its
 * set-up applied it (`factory`: what kMineFactoryData held just before, what was written, where the drop put it). */
async function appliedLook(s: Session, asked: { theme: string; tod: string; factory?: FactorySpec | null; stem?: string; script?: boolean }) {
  const conn = await s.ensure(), client = await s.client(), warnings: string[] = [];
  const out: Record<string, unknown> = { asked };
  type Field = { asked: number; write: string; found?: number } | null;
  type Applied = { key: string; tod: { asked: string; applied: boolean; why: string }; simMods?: string[];
    factory?: { stem: string; activation: Field; inactiveTurns: Field; knot: number[]; settled: number[]; landed: boolean; keptKnot: boolean } | null };
  if (conn.methods.includes("level.test.applied")) {
    let r = await client.call<Applied>("level.test.applied", {}, 15000).catch(() => null);
    // the factory's set-up runs as the match places its objects: wait a little for it when the level has one
    // and for the level script among the loaded sim code, which loads as the match's VM initialises
    const waiting = (x: Applied) => (asked.factory !== undefined && asked.factory !== null && "factory" in x && (!x.factory || x.factory.stem !== asked.stem)) ||
      (!!asked.script && !!x.simMods && !x.simMods.length);
    for (let i = 0; r && waiting(r) && i < 10; i++) {
      await new Promise((ok) => setTimeout(ok, 500));
      r = await client.call<Applied>("level.test.applied", {}, 15000).catch(() => r);
    }
    if (r) {
      const tod = r.tod.applied ? r.tod.asked : "DAY";
      out.timeOfDay = { applied: tod, ...(r.tod.applied ? {} : { fellBack: true, why: r.tod.why || "the frontend's time of day loaded" }) };
      if (tod !== asked.tod) warnings.push(`the time of day asked for was ${asked.tod} but the match loaded ${tod}: ${r.tod.why || "the frontend's value"}`);
      if (r.simMods) out.simModsLoaded = r.simMods;
      if (asked.factory !== undefined && asked.factory !== null && "factory" in r) {
        const f = r.factory && r.factory.stem === asked.stem ? r.factory : null;
        if (!f) {
          out.factory = { note: "the match has not set the level's mine factory up (yet): it is placed with the level's objects; game_mines {alive: true} says whether it is there" };
        } else {
          const w = (v: number[]) => v.map((x) => Math.round(x * 100) / 100);
          const field = (name: string, x: Field, want: number | undefined) => {
            if (!x) return want === undefined ? "the game's" : { asked: want, note: "not written" };
            if (x.write !== "ok") warnings.push(`the factory's ${name} (${x.asked}) was not written: ${x.write}`);
            if (x.found !== undefined && x.found !== x.asked) warnings.push(`kMineFactoryData.${name} held ${x.found} at the factory's set-up, not the level's ${x.asked} written at level start: something set it in between (the set-up's write put the level's back)`);
            return { written: x.asked, write: x.write, ...(x.found !== undefined ? { heldBefore: x.found } : {}) };
          };
          out.factory = {
            activation: field("NumMineActivation", f.activation, asked.factory.activation),
            inactiveTurns: field("NumTurnsInactive", f.inactiveTurns, asked.factory.inactiveTurns),
            dropWorld: { knot: w(f.knot), settled: w(f.settled), landed: f.landed, keptKnot: f.keptKnot },
          };
          if (!f.landed && !f.keptKnot) warnings.push("the mine factory's drop hit no land: it took the stale point of the match's last sweep and will likely be lost (set keepKnot)");
        }
      }
    }
  } else out.timeOfDay = { note: "this game cannot say which time of day loaded (no level.test.applied)" };
  if (conn.methods.includes("lua.eval")) {
    for (let i = 0; i < 10; i++) {
      const r = await client.call<{ ok: boolean; text: string }>("lua.eval", { target: "client", code: "return tostring(wum.game.theme())" }, 15000).catch(() => null);
      if (r?.ok && r.text && r.text !== "nil") {
        out.theme = { applied: r.text };
        if (r.text.toUpperCase() !== asked.theme.toUpperCase()) warnings.push(`the theme asked for was ${asked.theme} but the match reports ${r.text}`);
        break;
      }
      await new Promise((ok) => setTimeout(ok, 500));
    }
    if (!out.theme) out.theme = { note: "the match did not report its theme (wum.game.theme() was nil)" };
  }
  return { look: out, warnings };
}

/** The open project's hash: the patch save sends and the script on the server. */
export async function currentHash(s: Session): Promise<string> {
  const p = s.requireProject();
  return projectHash(p.store.patch(), (await p.erg.script()) ?? "");
}

/** test's expectHash and source: the game's project against the built one, refused when they differ, or (sync) the
 * source written into the game's project and the project opened again. */
async function checkProjectFor(s: Session, a: { expectHash?: string; source?: string; sync?: boolean }) {
  if (a.sync && !a.source) throw new ToolError("sync writes `source` into the game's project: give source");
  if (a.expectHash !== undefined && !/^[0-9a-f]{8,64}$/.test(a.expectHash)) throw new ToolError("expectHash: 8 to 64 hex digits (lower case), as save answers projectHash");
  const p = s.requireProject();
  let mine = await currentHash(s);
  const out: Record<string, unknown> = { project: p.id, projectHash: mine };
  if (a.source) {
    const src = readSource(a.source);
    const theirs = projectHash(src.patch, src.script);
    Object.assign(out, { source: src.patchFile, sourceHash: theirs, sourceScript: src.scriptFile });
    if (theirs !== mine) {
      if (!a.sync)
        throw new ToolError(`the game's project '${p.id}' (${mine.slice(0, 12)}) is not the built level ${src.patchFile} (${theirs.slice(0, 12)}): a stale copy in this game's projects folder? ` +
          "Pass sync: true to write the source into the game's project first, or build against this game (connect {gameDir}) and save");
      if (p.store.dirty) throw new ToolError(`the game's project '${p.id}' has unsaved edits: save or discard them (close {discard: true}, open_project) before sync overwrites it`);
      const own = p.store.patch() as unknown as Record<string, unknown>, base = src.patch.base as { key?: string } | undefined;
      if (base?.key && (own.base as { key?: string } | undefined)?.key !== base.key)
        throw new ToolError(`source is built on ${base.key}, the game's project '${p.id}' on ${(own.base as { key?: string }).key}: sync copies a level onto its own base only`);
      const saved = await p.erg.save({ ...src.patch, stem: own.stem, title: own.title } as unknown as Parameters<typeof p.erg.save>[0]);
      if (saved.saved === false) throw new ToolError("sync: the game's server did not save the source's patch");
      const sc = await p.erg.saveScript(src.script);
      if (!sc.saved) throw new ToolError(`sync: the game's server did not save the source's script: ${sc.problems.map((x) => x.message ?? String(x)).join("; ")}`);
      await s.open(p.id, true);
      mine = await currentHash(s);
      if (mine !== theirs) throw new ToolError(`sync wrote the source, but the game's project now hashes ${mine.slice(0, 12)}, not ${theirs.slice(0, 12)}: the server changed the patch on save (${saved.warnings?.join("; ") || "no warning"})`);
      Object.assign(out, { synced: true, projectHash: mine, note: "the source's patch and script were written into the game's project and the project opened again" });
    } else out.matches = true;
  }
  if (a.expectHash && !mine.startsWith(a.expectHash))
    throw new ToolError(`the game's project '${p.id}' hashes ${mine.slice(0, 12)}, not the expected ${a.expectHash.slice(0, 12)}: it is not the level that was built (save answers projectHash; pass source with sync: true to bring the built level over)`);
  if (a.expectHash) out.expectHash = "matches";
  return out;
}

async function savedFirst(s: Session, why: string) {
  const p = s.requireProject();
  if (!p.store.dirty) return undefined;
  const r = await s.save();
  if (r.problems.length) throw new ToolError(`the project has problems and was not ${why}: ${r.problems.join("; ")}`);
  return r;
}

// ------------------------------------------------------------------ asserts and the vanilla baseline

// Every Test match's asserts and GL errors (the game's log, category engine) are counted per run (lib/assert-log.ts):
// test starts a run, end_match and the next test end it; test {map, noLevel} runs the game's own map as a baseline and
// diffs it against the last Erg run, and test_asserts answers them.
const assertRuns: AssertRun[] = [];
let assertOff: (() => void) | null = null;

// The runs file (Documents\Melange\erg-mcp\assert-runs.json): every run this process ends is written there and the
// runs of earlier erg-mcp processes read back, so a baseline from one process and the Erg Test of the next compare.
const runsFile = () => { const d = documentsDir(); return d ? join(d, "Melange", "erg-mcp", "assert-runs.json") : null; };
function savedRuns(): SavedRun[] {
  const f = runsFile();
  if (!f || !existsSync(f)) return [];
  try {
    const j = JSON.parse(readFileSync(f, "utf8")) as { runs?: SavedRun[] };
    return Array.isArray(j.runs) ? j.runs.filter((r) => r && typeof r.label === "string" && (r.kind === "erg" || r.kind === "baseline") && Array.isArray(r.counts)) : [];
  } catch {
    return [];
  }
}
function saveRuns() {
  const f = runsFile();
  if (!f) return;
  try {
    mkdirSync(join(f, ".."), { recursive: true });
    writeFileSync(f, JSON.stringify({ note: "erg-mcp's Test runs (asserts and GL errors by signature), for test_asserts' compare across erg-mcp processes", runs: allRuns().map(toSaved) }));
  } catch {
    // the compare then covers this process's runs only
  }
}
const allRuns = () => mergeRuns(assertRuns.filter((r) => r.from !== "saved"), savedRuns());
// This process's own runs against every run, the file's too: test_asserts answers the session's by default, so a run
// another erg-mcp process made (another tester's, hours before) is not paired with this session's unnoticed (Caldera v2
// round 3: a 4-assert baseline from 11:45 stood beside the 14:03 run); scope "all" reads them all, each saved run labelled.
const PROCESS_START = Date.now();
const sessionRuns = () => assertRuns.filter((r) => r.from !== "saved" && r.startedAt >= PROCESS_START - 1000);
export type AssertScope = "session" | "all";
const runsFor = (scope: AssertScope | undefined) => (scope === "all" ? allRuns() : sessionRuns());

async function startAssertRun(s: Session, label: string, kind: AssertRun["kind"]): Promise<AssertRun | null> {
  endAssertRun();
  const client = await s.client();
  if (!client.has("log")) return null;
  const conn = await s.ensure();
  const run: AssertRun = { label, kind, startedAt: Date.now(), counts: new Map(), total: 0, game: `${conn.build ?? "?"}@${conn.url.replace(/\?.*$/, "")}` };
  assertRuns.push(run);
  if (assertRuns.length > 12) assertRuns.shift();
  // the channel sends the last 500 matching records first (its backlog): only lines logged since the run started count
  assertOff = client.subscribe<{ j?: unknown }>("log", { cats: ["engine"] }, (m) => {
    let j = m.j as { msg?: string; wall?: string } | string | undefined;
    if (typeof j === "string") try { j = JSON.parse(j) as { msg?: string; wall?: string }; } catch { return; }
    if (!j || typeof j !== "object" || typeof j.msg !== "string" || run.endedAt) return;
    const wall = j.wall ? Date.parse(j.wall) : NaN;
    if (Number.isFinite(wall) && wall < run.startedAt - 500) return;
    count(run, j.msg);
  });
  return run;
}

export function endAssertRun() {
  assertOff?.();
  assertOff = null;
  const last = assertRuns[assertRuns.length - 1];
  if (last && !last.endedAt) { last.endedAt = Date.now(); saveRuns(); }
}

function dropAssertRun(run: AssertRun) {
  if (assertRuns[assertRuns.length - 1] === run) endAssertRun();
  const i = assertRuns.indexOf(run);
  if (i >= 0) assertRuns.splice(i, 1);
}


// The baseline's turn reader: the game turn (turnsStarted), the phase, a computer team's turn, the active worm and its weapon.
const BASE_LUA = `local t = wum.game.turn() local m = wum.game.match and wum.game.match() or nil local wp = nil
for _, x in ipairs(wum.game.worms() or {}) do if x.slot == t.worm then wp = x.weapon end end
return tostring(wum.game.inMatch()) .. ";" .. tostring(m and m.turnsStarted or t.index) .. ";" .. tostring(t.phase) .. ";" .. tostring(t.ai) .. ";" .. tostring(t.worm) .. ";" .. tostring(wp)`;

/** test {map, noLevel}: the game's own map in a Quick Game with no Erg level, `turns` game turns watched (each human
 * turn firing the selected weapon with `fire`), its asserts and GL errors counted and diffed against the last Erg run. */
async function runBaseline(s: Session, a: { map?: string; noLevel?: boolean; turns?: number; fire?: boolean | "bazooka"; fireMs?: number; tod?: TimeOfDay; timeoutSec?: number;
  cleanMods?: boolean; postfx?: boolean; restart?: boolean; simMods?: "all" | "level-only"; teams?: TestTeam[]; gameDir?: string; perf?: boolean | number }) {
  if (!a.map || !a.noLevel) throw new ToolError("a baseline Test is test {map: \"Multi.<map>\", noLevel: true}: the game's own map with no Erg level");
  if (!/^Multi\.[A-Za-z0-9_-]+$/.test(a.map)) throw new ToolError("map: a game level's key, Multi.<map> (list_bases lists them)");
  const conn = await s.ensure(), client = await s.client();
  if (!conn.methods.includes("level.test")) throw new ToolError("no level.test here: Test needs the game running at its main menu (Melange.exe --serve has none)");
  if (a.fire && !conn.methods.includes("input.drive")) throw new ToolError("fire needs the game's input.drive (Melange 0.13+ with the Controls module)");
  // fire "bazooka" takes it first: level.test.selectWeapon (this branch's Melange: the panel's own message sent in the
  // sim, whatever the window's focus or DirectInput's polls) where the game has it, else from the weapon panel (a right
  // click opens it, a click takes its highlighted first cell; Automation module: test builds, the game window focused).
  // In the Caldera v2 round-3 baseline the panel's clicks took nothing (the injected mouse read polls=0), every turn's
  // weapon stayed nil and R2 could not be scored, so a fire that never happens is now an error, not a quiet nil.
  const direct = a.fire === "bazooka" && conn.methods.includes("level.test.selectWeapon");
  const panel = a.fire === "bazooka" && !direct ? automationFile(a.gameDir) : null;
  const clean = a.simMods === "level-only" ? { cleanMods: true, postfx: a.postfx ?? true } : { cleanMods: a.cleanMods, postfx: a.postfx };
  stopHold();
  const restarted = a.restart ? await backToMenu(s) : undefined;
  const run = await startAssertRun(s, a.map, "baseline");
  const erg = {
    test: async (t?: { tod?: TimeOfDay; cleanMods?: boolean; postfx?: boolean; teams?: TestTeam[] }) => client.call<{ key: string; state: string; cleanRoom?: { simMods: boolean; postfx: boolean }; baseline?: boolean }>(
      "level.test", { base: a.map, ...(t ?? {}) }, 120000).catch((e: Error) => {
      if (/project/.test(e.message) && !/base/.test(e.message)) throw new ToolError(`this game has no baseline Test (level.test {base}, Melange 0.13+): ${e.message}`);
      if (/must be at the frontend/.test(e.message)) throw new ToolError(`${e.message}: a match is running. Pass restart: true, or call end_match first`);
      throw e;
    }),
    onTest: (fn: (x: { state: string; key: string; detail: string }) => void) => client.subscribe<{ state?: string; key?: string; detail?: string }>("erg", undefined, (m) => {
      if (m && typeof m.state === "string") fn({ state: m.state, key: m.key ?? "", detail: m.detail ?? "" });
    }),
  };
  const started = await runTest(erg as unknown as Parameters<typeof runTest>[0], { tod: a.tod, timeoutMs: (a.timeoutSec ?? 60) * 1000, ...clean, teams: a.teams });
  if (started.final !== "playing") {
    if (run) dropAssertRun(run);
    return { ...started, baseline: a.map, note: "the baseline did not reach playing: nothing was counted" };
  }
  // the turns: wait for `turns` game turns to have started and the next to start (or the timeout), firing on human turns
  const turns = a.turns ?? 3, limit = Date.now() + Math.max(60, turns * 120) * 1000;
  const fired: { turn: number; worm: number; weapon: string; fired: boolean; why?: string }[] = [];
  let seen = 0, firedTurn = -1, last = "";
  for (;;) {
    const t = await client.call<{ ok: boolean; text: string }>("lua.eval", { target: "client", code: BASE_LUA }, 15000).catch(() => ({ ok: false, text: "" }));
    if (t.ok) {
      last = t.text;
      const [inMatch, turn, phase, ai, worm, weapon] = t.text.split(";");
      if (inMatch !== "true") break;
      seen = Math.max(seen, Number(turn) || 0);
      if (seen > turns) break;
      if (a.fire && phase === "play" && ai === "false" && firedTurn !== seen) {
        firedTurn = seen;
        await sleep(1500);   // the turn's camera settles on the worm
        let wp = weapon, took: string | undefined;
        if (direct) {
          took = await client.call<{ weapon: number; name: string }>("level.test.selectWeapon", { weapon: "kWeaponBazooka" }, 15000)
            .then((r) => `level.test.selectWeapon: ${r.name} (${r.weapon})`, (e: Error) => `level.test.selectWeapon refused: ${e.message}`);
        } else if (panel) {
          appendFileSync(panel, ["click right", "wait 500", "click", "wait 300", ""].join("\n"));
          took = "the weapon panel's first cell (Automation clicks)";
        }
        if (direct || panel) {
          // the sim applies the pick at its next tick; the panel's clicks take about 1.5 s
          for (let i = 0; i < (direct ? 8 : 1) && (wp === "nil" || wp === weapon); i++) {
            await sleep(direct ? 250 : 1600);
            const r = await client.call<{ ok: boolean; text: string }>("lua.eval", { target: "client", code: BASE_LUA }, 15000).catch(() => ({ ok: false, text: "" }));
            if (r.ok) wp = r.text.split(";")[5];
          }
        }
        // Input.Fire fires the worm's selected weapon: with none selected (weapon nil) it does nothing
        const none = wp === "nil" || wp === undefined;
        if (!none) await client.call("input.drive", { hold: ["Input.Fire"], ms: a.fireMs ?? 700, replace: true }, 15000).catch(() => undefined);
        fired.push({ turn: seen, worm: Number(worm), weapon: wp, ...(took ? { selected: weapon === wp ? `${took}: the pick did not change the weapon` : `${took} (was ${weapon})` } : {}),
          ...(none ? { fired: false, why: `no weapon selected (the game's turns start with none${panel ? "; the panel's click did not take one: is the game window focused?" : direct ? "; level.test.selectWeapon did not take" : "; fire: \"bazooka\" takes it first"})` } : { fired: true }) });
      }
    }
    if (Date.now() > limit) break;
    await sleep(500);
  }
  if (run) { run.turns = seen; }
  // the frame cost while the match plays, in the same session (the rubric's R2: fps against the Erg run's)
  const perf: unknown = a.perf ? await Promise.resolve(gameTools.find((t) => t.name === "game_perf")!.handler(s, { seconds: a.perf === true ? 10 : a.perf, label: "baseline" })).catch((e: Error) => ({ error: e.message })) : undefined;
  endAssertRun();
  // the last Erg run of this game: this session's first, else an earlier erg-mcp process's (the runs file), said so
  const ergOf = (rs: AssertRun[]) => (run ? [...rs].reverse().find((r) => r.kind === "erg" && (!run.game || !r.game || r.game === run.game)) : undefined);
  const erg0 = ergOf(sessionRuns()) ?? ergOf(allRuns());
  // a fire asked for that never happened is an error: the asserts of a baseline that fired nothing are not a baseline
  // for a level whose matches fire (the run is kept and counted all the same: test_asserts lists it)
  if (a.fire && !fired.some((f) => f.fired)) {
    const why = fired.length ? fired.map((f) => `turn ${f.turn}: ${f.why ?? "not fired"}`).join("; ") : `no human turn in play was seen in ${seen} turns`;
    throw new ToolError(`the baseline fired nothing: ${why}. Asserts counted all the same: ${run ? `${run.total} (${run.counts.size} signatures)` : "not counted"}; the match goes on (end_match leaves it). ${direct ? "level.test.selectWeapon took no weapon: is a human turn in play?" : "Use a Melange of this branch (level.test.selectWeapon selects the weapon in the sim, without the panel or DirectInput)"}`);
  }
  return { baseline: a.map, key: started.key, states: started.states, ...(restarted ? { restarted } : {}), ...(started.cleanRoom ? { cleanRoom: started.cleanRoom } : {}),
    turnsSeen: seen, ...(seen <= turns ? { warning: `only ${seen} game turns started before the timeout (last read: ${last})` } : {}),
    ...(a.fire ? { fired, fireNote: "each human turn fires the worm's selected weapon (Input.Fire held fireMs, the power) through input.drive; weapon is the game's weapon index of the active worm (wum.game.worms().weapon), taken first by level.test.selectWeapon (this branch's Melange) or the weapon panel" } : {}),
    asserts: run ? summary(run) : { note: "this game has no log channel: asserts were not counted" },
    ...(perf ? { perf } : {}),
    ...(run && erg0 ? { compare: diff(erg0, run), ...(erg0.from === "saved" ? { compareFrom: `the Erg run is an earlier erg-mcp process's (${new Date(erg0.startedAt).toISOString()}, the runs file); this session has none: test the project in this session for a same-session pair` } : {}) }
      : { compareNote: "no Erg Test run of this game yet (this process's or an earlier one's) to compare with: test the project, then test_asserts compares them" }),
    next: "the match goes on: end_match leaves it (or test {restart: true} for the next Test)" };
}

const sleep = (ms: number) => new Promise((ok) => setTimeout(ok, ms));

/** What the Test's match is: the Quick Game start the teams pick, with the worms each team was given. */
export function teamsLine(teams?: TestTeam[]): string {
  if (!teams) return "Quick Game (QuickStartHvC: a human team against a computer team) with the game's own Quick Game scheme; the scheme, turn time and team sizes are the game's, not chosen here";
  const all = teams.every((t) => t.human !== false);
  const start = all ? ["", "", "QuickStartHvH", "QuickStartHvHvH", "QuickStartHvHvHvH"][teams.length] : "QuickStartHvC";
  return `Quick Game (${start}: ${teams.map((t, i) => `team ${i} ${t.human === false ? "computer" : "human"}, ${t.worms ?? 4} worms`).join("; ")}) with the game's own Quick Game scheme and turn time`;
}

const modId: Schema = { type: "string", description: "Mod id: a-z, 0-9, '-' and '_' (it becomes the pack prefix; 'ergtest' is reserved)." };

/** A Test's mesh banks: the files given are copied into the project's meshes folder (which level.test loads), and the
 * mods whose meshes the project places are checked against the banks there. Undefined when the project places none
 * and none was given. */
export async function testMeshBanks(s: Session, id: string, store: EditorStore, files?: string[]) {
  const mods = [...new Set((shipFeatures(store).modMeshes ?? []).map((m) => m.slice(0, m.indexOf("."))))];
  if (!mods.length && !files?.length) return undefined;
  const dirs = (await (await s.levels()).list()) as { projectsDir?: unknown };
  const root = typeof dirs.projectsDir === "string" ? dirs.projectsDir : null;
  if (!root || !existsSync(join(root, id))) {
    if (files?.length) throw new ToolError(root ? `the project's folder ${join(root, id)} is not on this machine: meshBanks copies into it` : "the server did not answer its projectsDir (an older Melange): meshBanks needs it");
    return { warning: `the project places meshes of ${mods.join(", ")}: a Test draws them only when that mod is installed and on, or its bank is in the project's meshes folder (not checked: the projects folder is not readable here)` };
  }
  const dir = join(root, id, "meshes"), copied: string[] = [];
  for (const f of files ?? []) {
    if (!/\.xom$/i.test(f) || !existsSync(f)) throw new ToolError(`meshBanks: no .xom file ${f}`);
    mkdirSync(dir, { recursive: true });
    const to = join(dir, path.basename(f)), bytes = readFileSync(f);
    const old = existsSync(to) ? readFileSync(to) : null;
    if (!old || old.length !== bytes.length || old.some((v, i) => v !== bytes[i])) { writeFileSync(to, bytes); copied.push(path.basename(f)); }
  }
  const have = existsSync(dir) ? fs.readdirSync(dir).filter((f) => /\.xom$/i.test(f)).sort() : [];
  const missing = mods.filter((m) => !have.some((f) => f.slice(0, f.indexOf(".")) === m));
  const oldServer = lacking(s.conn?.build, [{ melange: "0.15.0" }]).length > 0;
  return { dir, banks: have, ...(copied.length ? { copied } : {}),
    ...(missing.length ? { warning: `the project places meshes of ${missing.join(", ")} and no ${missing.map((m) => `${m}.*.xom`).join(" or ")} is in its meshes folder: they will NOT draw in this Test unless that mod is installed and on in the game (test {meshBanks: ["<the bank .xom>"]} copies one there; build_rock_bank writes it)` } : {}),
    ...(oldServer && have.length ? { serverNote: `the server is Melange ${versionOf(s.conn?.build)}: a Test loads a project's meshes folder from 0.15.0` } : {}),
    note: "level.test loads the banks of the project's meshes folder once a game launch, before the match starts: a bank changed since needs a game restart ([erg] test: mesh bank ... in Melange.log)" };
}

/** The enabled mods of a game folder whose client script listens for the level haze (HAZE_EVENT); null when the folder
 * cannot be read. */
export function hazeMods(gameDir: string | undefined): string[] | null {
  if (!gameDir) return null;
  const modsDir = path.join(gameDir, "Mods");
  let enabled: Record<string, boolean> = {};
  try { enabled = JSON.parse(fs.readFileSync(path.join(modsDir, "thumper-state.json"), "utf8")).enabled ?? {}; } catch { return null; }
  const out: string[] = [];
  for (const id of Object.keys(enabled).filter((k) => enabled[k]).sort()) {
    try { if (fs.readFileSync(path.join(modsDir, id, "client", "init.lua"), "utf8").includes(HAZE_EVENT)) out.push(id); } catch { /* no client script */ }
  }
  return out;
}

/** The mods of a game folder that replace the water plane's shader (Mods\<id>\shaders\Water.*.glsl), whether each is
 * enabled (Mods\thumper-state.json) and whether its shader or client script names the sea material's message. */
export function waterShaderMods(gameDir: string | undefined): WaterShaderMod[] {
  if (!gameDir) return [];
  const modsDir = path.join(gameDir, "Mods"), out: WaterShaderMod[] = [];
  let enabled: Record<string, boolean> | null = null;
  try { enabled = JSON.parse(fs.readFileSync(path.join(modsDir, "thumper-state.json"), "utf8")).enabled ?? null; } catch { enabled = null; }
  let ids: string[] = [];
  try { ids = fs.readdirSync(modsDir).filter((n) => fs.statSync(path.join(modsDir, n)).isDirectory()); } catch { return []; }
  for (const id of ids.sort()) {
    let files: string[] = [];
    try { files = fs.readdirSync(path.join(modsDir, id, "shaders")).filter((f) => /^Water\..*\.glsl$/i.test(f)); } catch { continue; }
    if (!files.length) continue;
    const texts = [...files.map((f) => path.join(modsDir, id, "shaders", f)), path.join(modsDir, id, "client", "init.lua")];
    const honours = texts.some((f) => { try { return fs.readFileSync(f, "utf8").includes(SEA_EVENT); } catch { return false; } });
    out.push({ mod: id, file: `shaders\\${files[0]}`, honoursSea: honours, enabled: enabled && id in enabled ? !!enabled[id] : null });
  }
  return out;
}

export const tools: Tool[] = [
  {
    name: "place_spawn_knots",
    description: "Set the spawn mode and place or move the spawn knots WORM0-WORM7 (WORM0-WORM15 in mode teams). mode 'knots' makes worm i start on knot i (a match needs all 8); 'teams' makes each worm start on WORM<4 x team + its place in the team> whatever slots the match deals (team 0 on WORM0-3, team 1 on WORM4-7, team 2 on WORM8-11, team 3 on WORM12-15), so with a castle per team every team spawns in its own castle (a 2-team game needs WORM0-7, four teams WORM0-15; Test's teams sets the match's teams and worms); 'areas' (Melange after 0.14.1) deals each team one of `areas` per match from the match's seed, the same on every machine (pattern: the order the groups are dealt in, e.g. ['sand', 'sand:opposite', 'palace', 'sand'] gives 2 teams two opposite sand areas, 3 teams + the palace; opposite: the area pairs facing each other), and a team's worms stand on its area's knots (area 0's knots WORM0.., then area 1's; at most 8 areas, 8 knots each, 32 in all) - give areas [{name, group, knots: [{xz}, ...]}], not spawns; the answer's deal lists what 2-4 teams get over 1000 seeds; 'random' leaves the game's random spawns (existing knots stay but are unused). Each spawn is a position [x,y,z] or xz (dropped onto the terrain). An existing WORMn is moved, a missing one added. One undo step. Answers the spawns, the knots still missing and warnings: a spawn whose turn camera may start inside a wall (ground within wallRadius at cameraRise above the ground it stands on) or that stands on ground under the water.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        mode: { type: "string", enum: ["random", "knots", "teams", "areas"], description: "Spawn mode (default knots)." },
        areas: {
          type: "array", minItems: 1, maxItems: SPAWN_AREA_LIMITS.areas, description: "Mode areas: the spawn areas in order (area i's knots follow area i-1's: WORM0.. for area 0). Given, they replace the level's areas (and its pattern and opposite pairs unless given too); omitted, the level's areas stay and pattern / opposite change alone.",
          items: { type: "object", additionalProperties: false, required: ["name", "group", "knots"], properties: {
            name: { type: "string", description: "A label (1-24 printable characters, unique), e.g. 'SAND W'." },
            group: { type: "string", description: "[a-z][a-z0-9_]{0,15}: the area's group for the pattern, e.g. 'sand' or 'palace'." },
            knots: { type: "array", minItems: 1, maxItems: SPAWN_AREA_LIMITS.knotsPerArea, description: "The area's knots, in the order its team's worms take them (worm 0 of the team on the first).",
              items: { type: "object", additionalProperties: false, properties: { ...pointProps } } } } },
        },
        pattern: { type: "array", maxItems: SPAWN_AREA_LIMITS.steps, items: { type: "string" },
          description: "Mode areas: the groups in deal order; 'group:opposite' takes the opposite partner of an area dealt earlier (by `opposite`) when it is free. The areas the pattern leaves are dealt after it in a seeded order." },
        opposite: { type: "array", maxItems: SPAWN_AREA_LIMITS.areas, items: { type: "array", minItems: 2, maxItems: 2, items: { anyOf: [{ type: "string" }, { type: "integer" }] } },
          description: "Mode areas: pairs of areas facing each other, by name or index, e.g. [['SAND W', 'SAND E'], ['SAND N', 'SAND S']]; an area is in at most one pair." },
        spawns: {
          type: "array", maxItems: 16, description: "The knots to place or move.",
          items: { type: "object", additionalProperties: false, required: ["worm"],
            properties: { worm: { type: "integer", minimum: 0, maximum: 15, description: "0-7: WORM0-WORM7; with mode teams 0-15 (team floor(n / 4)'s worm n % 4)." }, ...pointProps,
              group: { type: "string", description: "With balance: a label such as the castle the spawn is in; a team's spawns are spread over different groups." } } },
        },
        balance: { anyOf: [{ type: "boolean" }, { type: "object", additionalProperties: false, properties: {
          teams: { type: "integer", minimum: 1, maximum: 8 }, wormsPerTeam: { type: "integer", minimum: 1, maximum: 8 }, mode: { type: "string", enum: ["height", "interleave"] } } }],
          description: "Deal the worm numbers again over the given positions so the teams stand alike (WORMn is worm n % wormsPerTeam of team floor(n / wormsPerTeam); default 2 teams of 4): height (default) snakes the heights over the teams (A B B A ...: equal sums) and spreads a team's spawns over different `group`s; interleave alternates A B A B. In a team the lowest spawn moves first. The answer's teams gives each team's mean and lowest spawn height and earliest flood turn." },
        snap: { type: "string", enum: ["tread"], description: "tread: a spawn whose point fails the tread checks (inside solid, no ground within 3 map units under it, no room for a worm over its ground, or on an edge narrower than a worm: no ground at its height 0.4 map units out in one of eight directions) is moved to the nearest point within 3 map units that passes (sampled every 0.25), and the answer's `snapped` says from where and why. Without it such a spawn is placed where asked and warned of, with the point it would snap to." },
        units: unitsSchema, ...cameraProps,
      },
    },
    handler: (s, a) => {
      if (a.balance !== undefined && typeof a.balance !== "boolean" && (typeof a.balance !== "object" || Array.isArray(a.balance))) throw new ToolError("balance: true, or {teams, wormsPerTeam, mode}");
      return placeSpawnKnots(s, a);
    },
  },
  {
    name: "place_object",
    description: "Place an object as the Erg palette does: crate (kind weapon|health|utility, contents, count, amount, hitpoints, parachute), health (a health crate), telepad (a pair in a new group; pos2 for the second pad, default 2 map units along x), trigger (index, radius, teamCollect, teamDestroy, hitpoints, wormCollect), mine, oildrum, or minefactory (one per level; activation, inactiveTurns, schemeMines, changed later with set_mine_factory). Position by pos [x,y,z] or xz (dropped onto the terrain). Crates, telepads, triggers and the mine factory make the project format v2 (at most 256 objects). One undo step. `objects` places several (up to 64, each with these fields and an optional `label`) in one call: each is its own undo step, and one that fails (nothing below to drop onto, no free knot) is listed in `failed` with its index, label and why while the rest are placed.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        objects: { type: "array", minItems: 1, maxItems: 64, description: "Several objects, each as one call's arguments (type, pos or xz, ...) plus an optional label naming it in the answer; the call's own units apply where an item gives none.",
          items: { type: "object", additionalProperties: false, required: ["type"], properties: {} } },
        type: { type: "string", enum: OBJECT_TYPES },
        ...pointProps,
        pos2: vec3("telepad: position of the second pad (units like pos)."),
        kind: { type: "string", enum: ["weapon", "health", "utility"], description: "crate: what it holds (default health)." },
        contents: { type: "string", description: "weapon/utility crate: the weapon or utility name, e.g. Bazooka." },
        count: { type: "integer", minimum: 1, description: "weapon/utility crate: how many." },
        amount: { type: "number", minimum: 1, description: "health crate: hit points restored (default 25)." },
        hitpoints: { type: "number", description: "crate or trigger: its own hit points (crate default 25)." },
        parachute: { type: "boolean", description: "crate: falls on a parachute (default false)." },
        index: { type: "integer", minimum: 0, maximum: 255, description: "trigger: its index (default the knot's number)." },
        radius: { type: "number", description: "trigger: radius (default 60)." },
        teamCollect: { type: "integer", minimum: 0, description: "trigger: teams that collect it (default 0)." },
        teamDestroy: { type: "integer", minimum: 0, description: "trigger: teams that destroy it (default 4)." },
        wormCollect: { type: "boolean", description: "trigger: a worm collects it (default false)." },
        snap: { type: "string", enum: ["surface"], description: "With pos (no drop): put the object exactly on the top of the ground in its column, the highest solid top at most 1 map unit over pos's y, a draped skin at its sheared height (where the game draws and collides it). No drop is involved, so a knot over a skin or in a crater sits where it is put." },
        ...factoryProps,
        units: unitsSchema,
      },
    },
    handler: (s, a: PlaceArgs & { objects?: (PlaceArgs & { label?: string })[] }) => {
      if (a.objects) {
        const { objects, units, ...rest } = a;
        if (Object.keys(rest).length) throw new ToolError(`objects places a list: give every other field (${Object.keys(rest).join(", ")}) inside each item`);
        return placeObjects(s, { objects, units });
      }
      if (!a.type) throw new ToolError("give type (or objects, a list)");
      return placeObject(s, a);
    },
  },
  {
    name: "move",
    description: "Move details: one to a new position (pos), or any number by an offset (by). drop also drops the result onto the terrain below it. Positions are in the level, not the detail's frame. One undo step.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: { ...idsProps, pos: vec3("New position [x,y,z] (one detail)."), by: vec3("Offset [x,y,z] added to each detail."),
        drop: { type: "boolean", description: "Drop the new position onto the terrain." }, units: unitsSchema },
    },
    async handler(s, a) {
      const r = moveDetails(s, a);
      // the mine factory's knot moved: how the game will place it
      const f = s.store.scene.objects?.find((o) => o.type === "minefactory");
      const d = f && s.store.scene.details.find((x) => x.name === f.knot && x.src === null);
      if (!d || !idsOf(a).includes(d.id)) return r;
      return { ...r, factory: await factoryReport(s, a.units ?? "world") };
    },
  },
  {
    name: "rotate",
    description: "Set (rot) or add to (by) the rotation of details, in degrees about x, y, z (applied z, then y, then x). Refused for details in frames that swing in the game.",
    inputSchema: { type: "object", additionalProperties: false, properties: { ...idsProps, rot: vec3("Absolute rotation, degrees."), by: vec3("Degrees to add.") , units: unitsSchema } },
    handler: (s, a) => turn(s, a),
  },
  {
    name: "scale",
    description: "Set the scale of details: scale [x,y,z], uniform (one number) or by (a factor multiplying the current scale). Refused for details in frames that swing in the game.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: { ...idsProps, scale: vec3("Absolute [x,y,z] scale."), uniform: { type: "number", description: "Absolute scale on all axes." },
        by: { type: "number", description: "Factor to multiply the scale by." }, units: unitsSchema },
    },
    handler: (s, a) => resize(s, a),
  },
  {
    name: "remove",
    description: "Delete details (an added level object's knot takes its object with it). One undo step.",
    inputSchema: { type: "object", additionalProperties: false, properties: { ...idsProps, units: unitsSchema } },
    handler: (s, a) => removeDetails(s, a),
  },
  {
    name: "drop_to_ground",
    description: "Drop details straight down onto the terrain below them, as the panel's Drop to ground. A detail over empty space is refused (move it above the ground first).",
    inputSchema: { type: "object", additionalProperties: false, properties: { ...idsProps, units: unitsSchema } },
    handler: (s, a) => dropDetails(s, a),
  },
  {
    name: "list_objects",
    description: "The open level's spawn knots, level objects (crates with contents, telepads, triggers, the mine factory) and placed mines and oil drums, with positions, plus the editor's checks (missing knots, objects under water, telepads without a partner) and the spawn checks (a wall at a spawn's turn camera, a spawn on ground under the water). role lists another role instead (e.g. scenery, light).",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: { role: { type: "string", description: "Only this role (default spawn and object)." }, search: { type: "string", description: "Name or resource contains this (any case)." },
        limit: { type: "integer", minimum: 1, maximum: 5000, description: "Details to list (default 200)." }, units: unitsSchema, ...cameraProps,
        outsideFrames: { type: "string", enum: ["list", "summary", "none"], description: "The editor's \"outside every terrain frame\" warnings, which a removed base's cameras, look-at points and props raise (about 21 on Caldera after remove_base_frames): list (default) names each, summary counts them in one line, none leaves them out. The other checks are listed whatever this is." } },
    },
    handler: (s, a) => listObjects(s, a),
  },
  {
    name: "set_mine_factory",
    description: "Change the level's mine factory settings (place it first with place_object type minefactory). launchHeight and launchRadius (Melange 0.13+) make its mines appear over it and fall round it instead of raining onto random worms and objects (the game's); launchers (1-8 points, in `units`) make them erupt from those points (the sides of a caldera) and land round them, and the answer's launchPlan gives each launcher's flight and landing radius (check the ground holds mines with game_mine_drop); `throw` {minRange, maxRange, pattern, targets, ...} lobs them OUT from the launchers (or the factory) to landing points over the whole island, Melange landing each where its flight meets the land (this branch). A value equal to the game's (activation 20, inactiveTurns 7, schemeMines true, keepKnot false) is dropped, so the game's value applies. Melange writes activation and inactiveTurns into the game's kMineFactoryData at level start and again at the factory's own set-up, just before it reads them (Melange 0.13+), and puts the game's back when the match ends; test's look.factory says what the set-up found and applied. The settings are the project's: save, and Test the same project on the same server's projects folder (a project of the same id in another game's ProjectsDir is another project). One undo step.",
    inputSchema: { type: "object", additionalProperties: false, properties: { ...factoryProps, units: unitsSchema,
      everyNTurns: { type: "integer", minimum: 1, maximum: 254, description: "A fire every N game turns, whatever the level holds: sets activation 255 (the level always holds fewer mines, so the count never stops a fire) and inactiveTurns N (the factory's countdown starts at N when it is placed and again after each fire, and each turn start takes one: it fires at the start of game turn N, 2N, 3N, ...), 10 mines a fire (the game's most). Not with activation or inactiveTurns." } } },
    async handler(s, a1: Omit<FactoryArgs, "launchers" | "throw"> & { everyNTurns?: number; launchers?: number[][]; throw?: ThrowArg | false; units?: Units }) {
      const store = s.store, scene = store.scene;
      const o = scene.objects?.find((x) => x.type === "minefactory");
      if (!o || o.type !== "minefactory") throw new ToolError("the level has no mine factory: place one with place_object type minefactory");
      const { units: lu, ...rest } = a1;
      const a0: FactoryArgs & { everyNTurns?: number } = { ...rest, launchers: launchersWorld(store, a1.launchers, lu ?? "world"), throw: throwWorld(a1.throw, lu ?? "world") };
      if (a0.launchers === undefined) delete a0.launchers;
      if (a0.throw === undefined) delete a0.throw;
      // everyNTurns: the two engine fields that make the fires depend on the turn count alone (levels/factory.cpp)
      if (a0.everyNTurns !== undefined && (a0.activation !== undefined || a0.inactiveTurns !== undefined)) throw new ToolError("everyNTurns sets activation and inactiveTurns: give it alone, or those two");
      const a: FactoryArgs = a0.everyNTurns !== undefined ? { ...a0, activation: EVERY_N_ACTIVATION, inactiveTurns: a0.everyNTurns } : a0;
      delete (a as { everyNTurns?: number }).everyNTurns;
      if (a.activation === undefined && a.inactiveTurns === undefined && a.schemeMines === undefined && a.keepKnot === undefined && a.launchHeight === undefined &&
        a.launchRadius === undefined && a.launch === undefined && a.launchers === undefined && a.throw === undefined)
        throw new ToolError("give activation, inactiveTurns, everyNTurns, schemeMines, keepKnot, launchHeight, launchRadius, launchers, throw or launch");
      if (a.launch === true) throw new ToolError("launch: false clears the launch; set launchHeight, launchRadius and/or launchers to give one");
      const factory = factoryOf(o.factory, a);
      commit(store, new SetObject(o.knot, factory ? { knot: o.knot, type: "minefactory", factory } : { knot: o.knot, type: "minefactory" }));
      // the placement checked again with the new settings: keepKnot turns the drop's advisories into warnings or clears them
      const issues = a.keepKnot !== undefined || a.launchers !== undefined ? await factoryReport(s, "world") : undefined;
      return { knot: o.knot, factory: factory ?? {}, game: FACTORY_GAME, changed: true, ...(factory?.launchers || factory?.throw ? { launchPlan: launchPlan(store, factory) } : {}), ...(issues ? { issues, ...(issues.some((l) => l.startsWith("error:")) ? { factoryWillBeLost: true } : {}) } : {}),
        ...(a0.everyNTurns !== undefined ? { everyNTurns: {
        n: a0.everyNTurns, mapsTo: { activation: EVERY_N_ACTIVATION, inactiveTurns: a0.everyNTurns }, firesAtGameTurns: [1, 2, 3, 4].map((i) => i * a0.everyNTurns!),
        note: "fires of 10 mines at the start of every N-th game turn, counted from the factory's placement at level start; a fire still running at the next one is not stopped" } } : {}),
      ...FACTORY_RULE };
    },
  },
  {
    name: "set_water",
    description: "Set the water level in world units (sea level 0; -1000 to 1000), or useDefault for the map's own water. The answer's backdrop gives coverLevel, the lowest water that covers the base's ground planes (list_backdrop), with a warning when the water is under it (on Multi.DinerMight a sea at -20 world shows the yellow table).",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: { level: { type: "number", minimum: -1000, maximum: 1000, description: "Water height, in world units (map units with units: map)." },
        useDefault: { type: "boolean" }, units: unitsSchema },
    },
    async handler(s, a: { level?: number; useDefault?: boolean; units?: Units }) {
      if ((a.level === undefined) === !a.useDefault) throw new ToolError("give exactly one of level or useDefault: true");
      const world = a.level === undefined ? null : a.units === "map" ? a.level * 20 : a.level;
      if (world !== null && Math.abs(world) > 1000) throw new ToolError("the water level is within -1000 and 1000 world units");
      const r = setLevel(s, { water: world }, "Change water", () => ({ water: { world, map: world === null ? null : world / 20 } })) as Record<string, unknown>;
      // the base's ground plane (a table, a seabed mesh) shows through a sea under it
      const sea = world === null ? null : await waterBackdropWarning(s).catch(() => null);
      return sea ? { ...r, backdrop: sea } : r;
    },
  },
  {
    name: "set_water_look",
    description: "The water's look (and the land's light) in this level's matches, over the theme's (or a named `preset` of fields: sand, lava, murk): opacity (0 clear to 1 opaque: the water plane's alpha, NearOpacity), reflection (how much of the sky it reflects, ReflectionStrength), shade (SubtractColourScale: negative paler, positive darker), landAmbient / landDiffuse (the terrain's ambient and sun light, LightAmbient / LightDiffuse: a tint of the whole land), or any field of the theme's WaterPlaneTweaks by name in `fields` (ReflectionContrast, SpecularContrast, SpecularPower, LightSpecular [r, g, b], ...). Tried in a Test on PREHISTORIC EVENING: each takes effect in the next frame, also from a turn handler. It writes one wum.level.water{...} call (Melange 0.13+; an older game skips it) in a marked block at the top of the level script, which this tool rewrites or (clear) removes, leaving the rest of the script as it is; the game sets the fields when the match's level script starts and puts the theme's back when the match ends. The script is written at once (not an undo step). Render only: nothing the match plays changes. The theme's fog and distance haze are not here: the game's shaders (CG\\Landscape.cg, water.cg) have none, so the haze of a time of day is its sky and lighting (test {postfx: false} shows whether a post-FX mod adds more).",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      preset: { type: "string", enum: Object.keys(WATER_PRESETS), description: "A named look expanded to fields for the project's theme and time of day: sand (a pale opaque sea that hides what is under it: opacity 1, reflection 0, shade -1.5, no highlight; tried on ARABIAN DAY), lava (a dark opaque sea), murk (a dull near-opaque one); lava and murk are not tried anywhere yet. Explicit fields below override the preset's. The answer's preset says whether it is unverified for this theme and time of day." },
      tint: { type: "array", items: { type: "number" }, minItems: 3, maxItems: 3, description: "With preset: [r, g, b] 0-255, a warm or cold tint of the land's light (LightDiffuse = the tint, LightAmbient = half of it). The water plane itself takes no colour (water.cg has none): its colour is the theme's texture, so the tint goes to the terrain; explicit landAmbient / landDiffuse override it." },
      opacity: { type: "number", minimum: 0, maximum: 1, description: "The water plane's alpha, NearOpacity (theme values about 0.6-0.9): 1 hides what is under the water, so a rising sea reads as a solid surface." },
      reflection: { type: "number", minimum: 0, maximum: 8, description: "ReflectionStrength: how much of the environment (sky) map the water reflects (0: the water shows its own texture only, which on PREHISTORIC EVENING turned it from grey-blue to lavender in a Test)." },
      shade: { type: "number", minimum: -4, maximum: 4, description: "SubtractColourScale: how much of the ripple texture's darker layer is taken off the water's colour (per CG\water.cg: higher is darker, deep blue then black; negative is paler, white by -4: -1.5 gave a pale opaque sea on ARABIAN DAY)." },
      landAmbient: { type: "array", items: { type: "number" }, minItems: 3, maxItems: 3, description: "LightAmbient [r, g, b] 0-255: the land's ambient light (the landscape shader's globalAmbient, from the same tweaks): a tint of the whole terrain. In a Test red turned the island red." },
      landDiffuse: { type: "array", items: { type: "number" }, minItems: 3, maxItems: 3, description: "LightDiffuse [r, g, b] 0-255: the land's sunlight (globalDiffuse)." },
      fields: { type: "object", description: "More WaterPlaneTweaks fields by name: a number, or [r, g, b(, a)] 0-255 for a colour, [x, y, z] for a vector." },
      sea: { type: "object", additionalProperties: false, required: ["albedo"], description: "The sea material: the water plane as a matte surface of one colour (a sand sea, lava, tar), which the game's own water shader cannot draw (its colour is the theme's blue texture: the sand preset is pale, never tan). The level script sets it with wum.level.water{Sea = {r, g, b, opacity}, SeaKeep = {reflection, foam}} (Melange 0.15; an older one skips it), Melange tells client mods with the event melange.level.sea, and a mod that replaces the water shader draws it: Sunstone 2.4+ does (opaque albedo lit by the sun, no reflection, refraction or foam unless asked). Without such a mod the fields and preset still apply, so give both: {preset: \"sand\", sea: {albedo: [214, 190, 140]}}. The answer's shaderMods lists the installed water shader mods of gameDir and whether each honours it.",
        properties: { albedo: { type: "array", items: { type: "number", minimum: 0, maximum: 255 }, minItems: 3, maxItems: 3, description: "[r, g, b] 0-255, as the colour should read in daylight." },
          opacity: { type: "number", minimum: 0, maximum: 1, description: "Default 1: nothing under the sea shows, and what stands in it ends at a crisp waterline." },
          reflection: { type: "number", minimum: 0, maximum: 1, description: "How much of the shader's sky reflection is kept (default 0)." },
          foam: { type: "number", minimum: 0, maximum: 1, description: "How much of the shader's shore foam is kept (default 0)." } } },
      haze: { description: "The level haze: the colour the far distance and the band under the horizon fade to, {color: [r, g, b] or \"#rrggbb\", amount (0-1, default 1: how far the mod's own haze colour gives way to it), start, distance (world units; 0, the default: the mod's own range)}, or false to take the level's haze off. The game has no fog of its own; a graphics mod hazes toward the sky's horizon colour (with Sunstone a sand sea is ringed in pale blue and meets a blue band under the horizon), and with a haze the level says the colour itself: the script sets wum.level.water{Haze, HazeRange} (Melange 0.15) and Melange sends melange.level.haze, which Sunstone 2.4+ honours. Given alone it is added to the look and sea the script already has; left out, the script's haze stays.",
        anyOf: [{ type: "boolean" }, { type: "object", additionalProperties: false, required: ["color"], properties: {
          color: { anyOf: [{ type: "string" }, { type: "array", items: { type: "number", minimum: 0, maximum: 255 }, minItems: 3, maxItems: 3 }] },
          amount: { type: "number", minimum: 0, maximum: 1 }, start: { type: "number", minimum: 0, maximum: 100000 }, distance: { type: "number", minimum: 0, maximum: 100000 } } }] },
      gameDir: { type: "string", description: "The game folder whose Mods are checked for water shader mods (default MELANGE_GAME_DIR)." },
      clear: { type: "boolean", description: "Remove the block: the theme's water again." },
    } },
    async handler(s, a: { preset?: string; tint?: number[]; opacity?: number; reflection?: number; shade?: number; landAmbient?: number[]; landDiffuse?: number[]; fields?: WaterLook; clear?: boolean;
      sea?: { albedo?: number[]; opacity?: number; reflection?: number; foam?: number }; gameDir?: string;
      haze?: { color?: number[] | string; amount?: number; start?: number; distance?: number } | boolean }) {
      const p = s.requireProject();
      let look: WaterLook = { ...(a.fields ?? {}) };
      if (a.opacity !== undefined) look.NearOpacity = a.opacity;
      if (a.reflection !== undefined) look.ReflectionStrength = a.reflection;
      if (a.shade !== undefined) look.SubtractColourScale = a.shade;
      if (a.landAmbient !== undefined) look.LightAmbient = a.landAmbient;
      if (a.landDiffuse !== undefined) look.LightDiffuse = a.landDiffuse;
      if (a.tint && !a.preset) throw new ToolError("tint goes with a preset");
      if (a.preset && a.clear) throw new ToolError("clear removes the block: give no preset with it");
      let preset: Record<string, unknown> | undefined;
      if (a.preset) {
        if (!(a.preset in WATER_PRESETS)) throw new ToolError(`preset '${a.preset}' is not one of ${Object.keys(WATER_PRESETS).join(", ")}`);
        const d = p.store.scene.databank;
        const e = expandWaterPreset(a.preset, a.tint, look, d.theme, d.timeOfDay);
        look = e.look;
        preset = { name: a.preset, theme: d.theme, timeOfDay: d.timeOfDay, verified: e.verified, ...(e.overridden.length ? { overridden: e.overridden } : {}), note: e.note };
      }
      if (a.clear && (Object.keys(look).length || a.sea || a.haze)) throw new ToolError("clear removes the block: give no fields with it");
      let sea: SeaLook | null = null;
      if (a.sea !== undefined) {
        const sl = seaLook(a.sea ?? {});
        if (sl.problems.length) throw new ToolError(sl.problems.join("; "));
        sea = sl.sea!;
      }
      const before = (await p.erg.script()) ?? "";
      // haze: the level's own; left out, the script's haze stays (a look set again keeps it); false takes it off. Given
      // alone it is added to the look and sea the script has
      if (a.haze === true) throw new ToolError("haze: {color, amount, start, distance}, or false to take it off");
      let haze: HazeLook | null = a.clear || a.haze === false ? null : readHaze(before);
      if (a.haze) {
        const hl = hazeLook(a.haze);
        if (hl.problems.length) throw new ToolError(hl.problems.join("; "));
        haze = hl.haze!;
      }
      const hazeOnly = a.haze !== undefined && !Object.keys(look).length && !sea && !a.clear;
      if (hazeOnly) { look = readWaterLook(before) ?? {}; for (const k of ["Sea", "SeaKeep", "Haze", "HazeRange"]) delete look[k]; sea = readSea(before); }
      if (!a.clear && !Object.keys(look).length && !sea && a.haze === undefined) throw new ToolError("give a preset, opacity, reflection, shade, landAmbient, landDiffuse or fields, a sea or a haze (or clear: true)");
      const bad = waterLookProblems(look);
      if (bad.length) throw new ToolError(bad.join("; "));
      const text = withWaterLook(before, a.clear ? null : look, sea, haze);
      // the water shader mods of the game folder: a look they do not read is said here, not found in a screenshot
      const mods = waterShaderMods(a.gameDir ?? process.env.MELANGE_GAME_DIR);
      const shader = a.clear ? {} : { ...(mods.length ? { shaderMods: mods } : {}), ...(() => { const w = shaderOverrideWarnings(mods, look, sea); return w.length ? { warnings: w } : {}; })() };
      if (text === before) return { changed: false, look: readWaterLook(before), ...(readSea(before) ? { sea: readSea(before) } : {}), ...(readHaze(before) ? { haze: readHaze(before) } : {}), ...(preset ? { preset } : {}), ...shader, note: "the script already sets this look" };
      const r = await p.erg.saveScript(text);
      if (!r.saved) throw new ToolError(`the server did not save the script: ${r.problems.map((x) => x.message ?? String(x)).join("; ")}`);
      return { changed: true, look: a.clear ? null : look, ...(sea ? { sea, seaEvent: SEA_EVENT } : {}), ...(haze ? { haze, hazeEvent: HAZE_EVENT, hazeNote: "the distance and the band under the horizon fade to this colour where a graphics mod honours it (Sunstone 2.4+); the game's own picture has no haze",
          ...(() => { const hm = hazeMods(a.gameDir ?? process.env.MELANGE_GAME_DIR); return hm === null ? {} : hm.length ? { hazeMods: hm } : { hazeWarning: `no enabled mod of the game folder listens for ${HAZE_EVENT}: the haze will not show there (Sunstone 2.4+ draws it)` }; })() } : {}), ...(preset ? { preset } : {}), was: readWaterLook(before), problems: r.problems, syntaxChecked: r.syntaxChecked ?? false, ...shader,
        note: a.clear ? "the block is gone: the theme's water" : "a wum.level.water{...} block at the top of the level script (Melange 0.13+): Test to see it; the game logs each field it set ([levels] water look: <container>.<field> set)" +
          (sea ? `; the sea material goes to client mods as the event ${SEA_EVENT} (Melange 0.15), which a water shader mod that honours it draws (shaderMods)` : "") };
    },
  },
  {
    name: "set_theme",
    description: "Set the level's theme (the scenery set, sky and the folder textures come from; the terrain is untouched). The voxels' materials come from the level's material file, which a theme change leaves as it is unless materialFile is given: \"theme\" for the new theme's own file, or a file name (list_materials, preview_materials). One undo step.",
    inputSchema: { type: "object", additionalProperties: false, required: ["theme"], properties: { theme: { type: "string", enum: THEMES },
      materialFile: { type: "string", description: "Also set the material file: \"theme\" for the theme's own (ThemeCamelot\\ThemeCamelot.txt for CAMELOT), \"keep\" (default) to leave it, or a file name such as Maps\\dinermight.txt." } } },
    async handler(s, a: { theme: string; materialFile?: string }) {
      const keep = a.materialFile === undefined || a.materialFile === "keep";
      let file: string | undefined, note: string | undefined;
      if (!keep) {
        const r = await resolveFile(s, a.materialFile === "theme" ? themeFile(a.theme) : a.materialFile!);
        const files = await (await s.levels()).themes().then((t) => t.materialFiles, () => null);
        if (files && !files.includes(r.file)) throw new ToolError(`'${a.materialFile}' is not a material file of this install (list_materials lists them)`);
        file = r.file;
        note = r.note;
      }
      return setLevel(s, { databank: { theme: a.theme, ...(file ? { materialFile: file } : {}) } }, "Change theme", () => ({
        theme: s.store.scene.databank.theme, materialFile: s.store.scene.databank.materialFile, ...(note ? { note } : {}),
        ...(keep ? { materialNote: "the material file is unchanged: voxel materials keep their indices in it (materialFile: \"theme\" switches to the theme's own)" } : {}),
      }));
    },
  },

  {
    name: "set_time_of_day",
    description: "Set the level's time of day. Quick Game and network games load DAY whatever is set; test can pick its own.",
    inputSchema: { type: "object", additionalProperties: false, required: ["timeOfDay"], properties: { timeOfDay: { type: "string", enum: TIMES_OF_DAY } } },
    handler: (s, a: { timeOfDay: string }) => setLevel(s, { databank: { timeOfDay: a.timeOfDay } }, "Change time of day", () => ({ timeOfDay: s.store.scene.databank.timeOfDay })),
  },
  {
    name: "set_surround_mode",
    description: "How the area around the map is made: copy (the base's), none, flat, or paint (an editable surround; needs the base's surround loaded).",
    inputSchema: { type: "object", additionalProperties: false, required: ["mode"], properties: { mode: { type: "string", enum: ["copy", "none", "flat", "paint"] } } },
    handler(s, a: { mode: "copy" | "none" | "flat" | "paint" }) {
      if (a.mode === "paint" && !s.store.canPaintSurround) throw new ToolError("the base level's surround did not load: reopen the project to paint it");
      return setLevel(s, { hmp: a.mode }, `Surround ${a.mode}`, () => ({ surround: s.store.scene.hmp.mode }));
    },
  },
  {
    name: "get_script",
    description: "The level's Lua script (script.lua of the project); empty when it has none.",
    inputSchema: { type: "object", additionalProperties: false, properties: {} },
    async handler(s) {
      const text = await s.requireProject().erg.script();
      return { present: text.length > 0, lines: text ? text.split("\n").length : 0, text };
    },
  },
  {
    name: "put_script",
    description: "Replace the level's Lua script (written to the server at once; not an undo step and not part of save). Answers problems with line numbers. A script the server rejects is not saved; a syntax error is reported but saved. syntaxChecked is false when the server cannot compile Lua: the game then checks it on test.",
    inputSchema: { type: "object", additionalProperties: false, required: ["text"], properties: { text: { type: "string", description: "The whole script; empty removes it." } } },
    async handler(s, a: { text: string }) {
      const r = await s.requireProject().erg.saveScript(a.text);
      return { saved: r.saved, problems: r.problems, syntaxChecked: r.syntaxChecked ?? false,
        ...(r.syntaxChecked ? {} : { note: "the server cannot compile Lua here; the game checks the script on test" }) };
    },
  },
  {
    name: "export",
    description: "Export the open project as a mod under Mods\\: mode 'source' (your edits plus a build script, shareable; the default) or 'install' (built map files for this machine only: do not redistribute). Saves the project first when it has unsaved edits. Exporting a mod id again adds or updates the level in it.",
    inputSchema: {
      type: "object", additionalProperties: false, required: ["modId", "name"],
      properties: { modId, name: { type: "string", description: "Display name (1-64 printable characters)." }, version: { type: "string", description: "x.y.z (default 1.0.0)." },
        mode: { type: "string", enum: ["source", "install"] }, survivor: { type: "boolean", description: "Also list the level in Survivor's Prebuilt maps (default false)." },
        timeOfDay: { type: "string", enum: ["DAY", "EVENING", "NIGHT", "level", "none"], description: "Pin the level's time of day in every match (spice.json levels[].timeOfDay, Melange 0.15+): DAY, EVENING or NIGHT, or level (the built level's own, set_time_of_day's). Default: what set_level_look {lock: true} set for this project (the level's own time of day, by name), else no pin. none: no pin whatever set_level_look says." },
        thumbnail: { type: "string", description: "A PNG file for the level's picture in the map menus (levels[].image, Melange 0.15+): written into the pack as thumb/<level>.png. Default: set_level_thumbnail's picture for this project, else none (the base's picture shows). \"none\": no picture whatever set_level_thumbnail says." },
        melangeRange: { type: "string", description: "spice.json melange.range (a semver range such as \">=0.14.0\"). Default: \">=0.2.0\", raised automatically to the newest Melange a feature the level uses needs (lib/ship-gate.ts): more details than the vanilla mesh pool loads or meshes of the pack's own bank (0.14.0), spawns.mode areas (0.15.0). The answer's gate says which and why (needs), and warns when a range given here admits an older Melange than the level needs. A Melange the range does not admit quarantines the pack, and build says so." } },
    },
    async handler(s, a: { modId: string; name: string; version?: string; mode?: "source" | "install"; survivor?: boolean; melangeRange?: string; timeOfDay?: string; thumbnail?: string }) {
      const p = s.requireProject();
      // the level entry's look: export's own arguments, else what set_level_look and set_level_thumbnail set this session
      const look = levelLookOf(p.id);
      const tod = a.timeOfDay === "none" ? undefined : a.timeOfDay ?? (look.lock ? p.store.scene.databank.timeOfDay || "DAY" : undefined);
      const thumb = a.thumbnail === "none" ? undefined : a.thumbnail !== undefined ? thumbnailFromFile(a.thumbnail) : look.thumbnail;
      const image = thumb ? `thumb/${p.id}.png` : undefined;
      const levelKeys = [...(tod ? ["timeOfDay"] : []), ...(image ? ["image"] : [])];
      const form = { modId: a.modId, name: a.name, version: a.version ?? "1.0.0", mode: a.mode ?? "source", survivor: !!a.survivor };
      const bad = validateExportForm(form);
      if (bad.length) throw new ToolError(bad.join("; "));
      const first = await savedFirst(s, "exported");
      const gate = exportGate(p.store, a.melangeRange, levelKeys);
      const r = await (await s.client()).call<{ dir: string; files: string[]; restartRequired: boolean }>(
        "level.export", { project: p.id, ...form, ...(gate.gated ? { melangeRange: gate.melangeRange } : {}), ...(tod ? { timeOfDay: tod } : {}), ...(image ? { image } : {}) }, 120000);
      // a server older than these keys ignores them: say so instead of shipping a pack that silently lacks them
      const levelLook: Record<string, unknown> = {};
      if (levelKeys.length) {
        let entry: Record<string, unknown> | undefined;
        try { entry = (JSON.parse(fs.readFileSync(path.join(r.dir, "spice.json"), "utf8")).levels as Record<string, unknown>[]).find((l) => l.slug === p.id); } catch { entry = undefined; }
        if (tod) levelLook.timeOfDay = entry?.timeOfDay === tod ? tod : { asked: tod, warning: "the server did not write levels[].timeOfDay (a Melange older than 0.15): no pin in this pack" };
        if (image && thumb) {
          if (entry?.image === image) {
            fs.mkdirSync(path.join(r.dir, "thumb"), { recursive: true });
            fs.writeFileSync(path.join(r.dir, image), thumb.png);
            r.files = [...r.files, image];
            levelLook.image = { file: image, from: thumb.from, bytes: thumb.png.length };
          } else levelLook.image = { asked: image, warning: "the server did not write levels[].image (a Melange older than 0.15): no picture in this pack" };
        }
      }
      return { ...r, mode: form.mode, gate, ...(levelKeys.length ? { levelLook } : {}), projectHash: await currentHash(s).catch(() => undefined), ...(first ? { savedFirst: true, warnings: first.warnings } : {}),
        ...(form.mode === "install" ? { note: "install exports contain modified game files: keep them on this machine" } : {}) };
    },
  },
  {
    name: "build",
    description: "Build the levels of a source-mode mod (made by export) into playable map files (level.build). The game must be closed.",
    inputSchema: { type: "object", additionalProperties: false, required: ["modId"], properties: { modId } },
    handler: async (s, a: { modId: string }) => (await s.client()).call("level.build", { modId: a.modId }, 120000),
  },
  {
    name: "test",
    description: "Play the project in the running game without exporting (level.test): saves unsaved edits, then follows the 'erg' channel (registering, registered, armed, starting, playing, ended, failed) and waits for 'playing' (or 'armed', where the game waits for Quick Game). Answers the states with timings. Needs the game running at its main menu; Melange.exe --serve has no Test. `restart: true` from a running Test match leaves it for the menu first (level.test.quit: the game's own GameLogic.QuitGame) and then tests the project again, so an edit is played without restarting the game.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: { tod: { type: "string", enum: TIMES_OF_DAY, description: "Time of day for this Test (default the project's own: set_time_of_day). The answer's `look` says what the match applied (time of day and theme) and `warnings` names a fallback." },
        waitFor: { type: "string", enum: ["playing", "armed"], description: "State to wait for (default playing)." },
        meshBanks: { type: "array", maxItems: 8, items: { type: "string" }, description: "The project's own mesh banks for the Test (.xom files, as build_rock_bank writes: <id>.<Bank>.xom): each is copied into the project's meshes folder (<projectsDir>\\<project>\\meshes), which a Test loads before it starts (Melange 0.15: level.test loads <project>\\meshes\\*.xom once a launch; a changed bank needs a game restart), so the Test shows the detail meshes an installed pack shows. Without it the folder is used as it is; the answer's meshBanks lists the banks there and warns when the project places a mod's meshes and no bank of that mod is in the folder (they do not draw in the Test unless that mod is installed and on)." },
        timeoutSec: { type: "integer", minimum: 1, maximum: 600, description: "Stop waiting after this long (default 60)." },
        cleanMods: { type: "boolean", description: "A clean room for this Test match only: enabled content mods' sim code (entry.sim) does not run, only the level script, and the post-FX stack is off (unless postfx: true), so asserts and lighting belong to the map. Nothing is changed on disk; launch-time content (weapon clones, file overrides, shader mods, client scripts) stays. On a game build without clean rooms it refuses while such mods are enabled." },
        postfx: { type: "boolean", description: "false: the post-FX stack (every effect and code pass, like the Post-FX panel's Bypass all) is off for this Test match; true with cleanMods keeps it on. Default: on, or off with cleanMods." },
        simMods: { type: "string", enum: ["all", "level-only"], description: "level-only: an isolated level Test: the other installed sim mods (Kindjal, Sandtide, ...) do not run in this match, only the level's chunk and its script, so the level's asserts and behaviour are its own; post-FX stays as it is (postfx: false turns it off too). It is cleanMods for the sim alone; all (default): every enabled sim mod runs beside the level script. The answer's cleanRoom and look.simModsLoaded say what ran." },
        restart: { type: "boolean", description: "When a Test match is running: leave it for the main menu (level.test.quit), wait for the menu, then test again (default false: refused in a match). Leaving a match writes the game's save (its statistics), as the game's own Quit does." },
        expectHash: { type: "string", description: "The project hash the Test must play (save and export answer projectHash; a prefix of 8 or more hex digits is enough): refused, nothing started, when the game's project (the patch save sends, without stem and title, and its script) hashes otherwise. Guards against testing a stale copy of the project in another game's projects folder." },
        source: { type: "string", description: "The built level the Test must play, on disk: a project folder (project.ergpatch.json, script.lua), an exported source pack (src\\<stem>.ergpatch.json, sim\\<stem>.lua) or a .ergpatch.json file. Its hash is compared with the game's project: refused when they differ, unless sync." },
        sync: { type: "boolean", description: "With source: when the game's project differs, write the source's patch and script into it first (level.save and the script save on the game's own server, then the project is opened again), so the Test plays exactly the built level. Refused while the open project has unsaved edits or the source's base differs. The answer's projectCheck says what was compared and done." },
        map: { type: "string", description: "With noLevel: true, a vanilla baseline instead of the project: the game's own level (Multi.DinerMight) in the same Quick Game with no Erg level (level.test {base}, Melange 0.13+). It watches `turns` game turns, fires the selected weapon on each human turn with `fire`, counts the game's ASSERT and GL ERROR lines (its log) and diffs them against the last Erg Test run of this erg-mcp session (`compare`: the signatures only the Erg level shows, only the baseline, and both), so whether an assert is the level's is answered from two runs. No project needs to be open. simMods, cleanMods, postfx, tod and restart apply." },
        noLevel: { type: "boolean", description: "With map: the baseline Test (no Erg level)." },
        baseline: { type: "boolean", description: "The vanilla baseline in one word: map Multi.DinerMight, noLevel, fire \"bazooka\" on each human turn (level.test.selectWeapon, this branch's Melange), 4 turns, perf 10 s, with the other arguments (teams, tod, postfx, restart) as given, so Sunstone's post-FX stays on unless postfx: false. Answers the asserts, the fps and GPU frame time (perf) and the compare with this session's Erg run; an error when nothing fired." },
        perf: { anyOf: [{ type: "boolean" }, { type: "number", minimum: 1, maximum: 120 }], description: "Baseline: after the turns, sample the frame cost (game_perf, labelled baseline) for this many seconds (true: 10), in the same session as the Erg run's." },
        suddenDeath: { type: "boolean", description: "false: no sudden death in this Test: once it plays, the round clock is held (level.test.roundTime {hold}: paused at once and again at every turn start until the Test ends), so the Quick Game's sudden death (30 minutes in: the water raised past the level's cap and every worm's health set to 1, Caldera v2 round 2) never comes in a long harness run; the turn clock runs as before. The answer's roundClock shows the round time left standing still. Default: the scheme's own." },
        turns: { type: "integer", minimum: 1, maximum: 20, description: "Baseline: game turns to watch (default 3)." },
        fire: { anyOf: [{ type: "boolean" }, { type: "string", enum: ["bazooka"] }], description: "Baseline: on each human turn, fire the worm's selected weapon (Input.Fire held fireMs through input.drive, whatever the window's focus). \"bazooka\" takes it from the weapon panel first (a right click opens the panel and a click takes its highlighted first cell, the bazooka, through the Automation module: test builds with the game window focused, gameDir or MELANGE_GAME_DIR). A worm with no weapon selected (the Quick Game's turns start with none) fires nothing: the answer lists each turn, the game's weapon index (nil: none) and whether it fired." },
        fireMs: { type: "integer", minimum: 50, maximum: 3000, description: "Baseline: how long fire is held, the shot's power (default 700)." },
        gameDir: { type: "string", description: "Baseline with fire \"bazooka\": the running game's folder, for its Automation command file (the weapon panel's clicks; default MELANGE_GAME_DIR)." },
        teams: { type: "array", minItems: 2, maxItems: 4, items: { type: "object", additionalProperties: false, properties: {
          human: { type: "boolean", description: "A human team (default true): the tester steers its worms (game_play_turns, game_walk)." },
          worms: { type: "integer", minimum: 1, maximum: 4, description: "Its worms (default the Quick Game's four)." } } },
          description: "The match's teams (Melange 0.13+ with team starts): 2-4 human teams (the Quick Game's 2-4 player starts, QuickStartHvH ... HvHvHvH) or a human team then a computer team (the default, QuickStartHvC), each with `worms` 1-4 (written into the match's set-up before the worms are dealt). E.g. [{human: true, worms: 2}, {human: true, worms: 2}, {human: true, worms: 2}, {human: true, worms: 2}]: four human teams of two, no computer team; with spawns.mode teams each team stands on its own knots (WORM<4 x team + place>). The baseline takes it too." } },
    },
    async handler(s, a0: { tod?: TimeOfDay; waitFor?: "playing" | "armed"; timeoutSec?: number; cleanMods?: boolean; postfx?: boolean; restart?: boolean; simMods?: "all" | "level-only";
      expectHash?: string; source?: string; sync?: boolean; map?: string; noLevel?: boolean; turns?: number; fire?: boolean | "bazooka"; fireMs?: number; teams?: TestTeam[]; gameDir?: string;
      suddenDeath?: boolean; baseline?: boolean; perf?: boolean | number; meshBanks?: string[] }) {
      if (a0.baseline) a0 = { map: "Multi.DinerMight", noLevel: true, fire: "bazooka", turns: 4, perf: true, ...a0 };
      if (a0.map !== undefined || a0.noLevel) {
        const b = await runBaseline(s, a0);
        return a0.suddenDeath === false && (b as { key?: string; states?: unknown }).states ? { ...b, roundClock: await holdRoundClock(s).catch((e: Error) => ({ held: false, note: e.message })) } : b;
      }
      if (a0.turns !== undefined || a0.fire !== undefined || a0.fireMs !== undefined || a0.perf !== undefined) throw new ToolError("turns, fire, fireMs and perf are for the baseline Test (map, noLevel: true, or baseline: true)");
      // simMods level-only: the clean room's sim half, post-FX kept unless postfx says otherwise
      if (a0.simMods === "level-only" && a0.cleanMods === false) throw new ToolError("simMods: \"level-only\" is a clean room for the sim: leave out cleanMods: false");
      if (a0.simMods === "all" && a0.cleanMods) throw new ToolError("simMods: \"all\" runs every sim mod, cleanMods none: give one of them");
      const a = a0.simMods === "level-only" ? { ...a0, cleanMods: true, postfx: a0.postfx ?? true } : a0;

      s.requireProject();
      const conn = await s.ensure();
      if (!conn.methods.includes("level.test")) throw new ToolError("no level.test here: Test needs the game running at its main menu (Melange.exe --serve has none)");
      // the project the game will play, against the one that was built: refused, or synced, before anything starts
      const projectCheck = a0.source || a0.expectHash ? await checkProjectFor(s, a0) : undefined;
      const p = s.requireProject();
      let restarted: { fromScene: string; ms: number; retries?: number } | undefined;
      stopHold();   // a held camera belongs to the match being left
      if (a.restart) restarted = await backToMenu(s);
      // Content mods with sim code run in every match, a Test too: name them, so a benchmark run knows it is not clean.
      let simMods: string[] = [];
      if (conn.methods.includes("mods.list")) {
        try {
          const mods = await (await s.client()).call<{ id: string; on: boolean; kind: string; hasSim?: boolean; state?: string }[]>("mods.list", {}, 15000);
          simMods = mods.filter((m) => m.on && m.kind === "content" && m.hasSim && m.state === "enabled").map((m) => m.id);
        } catch {
          // an older game: no list
        }
      }
      // A game with clean rooms lists level.test.cleanRoom; an older one ignores cleanMods, so the old refusal stays.
      const cleanRooms = conn.methods.includes("level.test.cleanRoom");
      if (a.cleanMods && simMods.length && !cleanRooms)
        throw new ToolError(`content mods with sim code are enabled and would run in the Test: ${simMods.join(", ")}. This game build has no clean-room Test: disable them in the game (they apply at the next launch) for a clean run, or leave out cleanMods`);
      const first = await savedFirst(s, "tested");
      const sec = a.timeoutSec ?? 60;
      const meshBanks = await testMeshBanks(s, p.id, p.store, a0.meshBanks).catch((e: Error) => { if (e instanceof ToolError) throw e; return undefined; });
      // the project's own time of day unless one is given: without it the frontend's (Quick Game: DAY) loads
      const db = p.store.scene.databank, tod = a.tod ?? (TIMES_OF_DAY.includes(db.timeOfDay as TimeOfDay) ? (db.timeOfDay as TimeOfDay) : undefined);
      // the game refuses a Test outside its frontend: say how to get there instead of its bare refusal
      // the match's asserts and GL errors, counted for test_asserts and a later baseline's compare
      const asserts = await startAssertRun(s, `Multi.ergtest_${p.id}`, "erg").catch(() => null);
      let run = await runTest(p.erg, { tod, goal: a.waitFor, timeoutMs: sec * 1000, cleanMods: a.cleanMods, postfx: a.postfx, teams: a.teams }).catch((e: Error) => {
        if (/must be at the frontend/.test(e.message))
          throw new ToolError(`${e.message}: a match is running. Pass restart: true to leave a Test match for the menu and test again in one call, or call end_match first (leaving writes the game's save, as its own Quit does)`);
        throw e;
      });
      // Just after a match the frontend may not take Quick Game yet, and the menu's attract demo can start first: the
      // game refuses that Test (kAttractRefused); from the menu a second try starts it.
      for (let tries = 0; restarted && run.final === "failed" && /attract demo/.test(String(run.failed)) && tries < 2; tries++) {
        await new Promise((ok) => setTimeout(ok, 3000));
        run = await runTest(p.erg, { tod, goal: a.waitFor, timeoutMs: sec * 1000, cleanMods: a.cleanMods, postfx: a.postfx, teams: a.teams });
        restarted.retries = tries + 1;
      }
      if (asserts && run.key) asserts.label = run.key;
      // a Test that did not play counts nothing: what the log shows next is another match (the menu's attract demo)
      if (asserts && run.final !== "playing" && a.waitFor !== "armed") dropAssertRun(asserts);
      const suppressed = !!a.cleanMods && !!run.cleanRoom && !run.cleanRoom.simMods;
      const fo = p.store.scene.objects?.find((o) => o.type === "minefactory");
      const look = run.final === "playing" ? await appliedLook(s, { theme: db.theme, tod: tod ?? "DAY", factory: fo && fo.type === "minefactory" ? fo.factory ?? {} : null, stem: run.key?.replace(/^Multi\./, ""),
        script: run.final === "playing" && !!(await (async () => (await p.erg.script()) ?? "")().catch(() => "")).trim() }) : null;
      const roundClock = a0.suddenDeath === false && run.final === "playing" ? await holdRoundClock(s).catch((e: Error) => ({ held: false, note: e.message })) : undefined;
      return { ...run, ...(roundClock ? { roundClock } : {}), ...(a0.simMods ? { isolated: a0.simMods === "level-only" ? (suppressed || !simMods.length ? "level only: no other sim mod runs in this match" : "level only was asked, but the game did not apply a clean room (cleanRoom)") : "all sim mods" } : {}),
        ...(first ? { savedFirst: true } : {}), ...(restarted ? { restarted } : {}), ...(projectCheck ? { projectCheck } : {}), ...(meshBanks ? { meshBanks } : {}),
        ...(look ? { look: look.look, ...(look.warnings.length ? { warnings: look.warnings } : {}) } : {}),
        ...(simMods.length ? suppressed ? { simMods, simModsNote: "these mods' sim code is suppressed in this Test match (clean room)" }
          : { simMods, simModsNote: "these mods' sim code runs in this match beside the level script" } : {}),
        match: teamsLine(a.teams),
        next: "game_state, game_wait {turn}, game_camera while it plays",
        ...(run.timedOut ? { note: `no '${a.waitFor ?? "playing"}' state within ${sec} s: the Test may still be running; start the match from the game's menu if it is armed` } : {}) };
    },
  },
  {
    name: "end_match",
    description: "Leave the running Test match for the game's main menu (level.test.quit: the game's own GameLogic.QuitGame, sent in the sim's tick) and wait until the menu is up (about 5-10 s), so the next test starts without a game restart. Only a match that test started, offline. Leaving writes the game's save (its statistics), as the game's own Quit does. Answers where the game was and how long it took; at the menu already, it says so.",
    inputSchema: { type: "object", additionalProperties: false, properties: {} },
    async handler(s) {
      stopHold();
      endAssertRun();
      const r = await backToMenu(s);
      return r ? { left: true, ...r, next: "test (the game is at its menu)" } : { left: false, note: "no match is running: the game is at its menu" };
    },
  },
  {
    name: "test_asserts",
    description: "The game's ASSERT and GL ERROR lines of the Test matches erg-mcp started, counted by signature (the line without its prefix) per run: an Erg Test (test) and the vanilla baseline (test {map, noLevel: true}); a run ends with end_match or the next test. Ended runs are kept in Documents\Melange\erg-mcp\assert-runs.json (the last 24), so a baseline run by one erg-mcp process and the Erg Test of a later one are compared too (the last Erg run against the last baseline of the same game build). Answers each run's totals and top signatures and, with an Erg run and a baseline, their diff: the signatures only the Erg level shows (the level's own), only the baseline, and both (the game's own), with counts per minute.",
    inputSchema: { type: "object", additionalProperties: false, properties: { limit: { type: "integer", minimum: 1, maximum: 200, description: "Signatures per run (default 20)." },
      scope: { type: "string", enum: ["session", "all"], description: "session (default): the runs this erg-mcp process started (since it launched), so the compare pairs this session's Erg Test with this session's baseline and nothing else; all: also the runs file's (earlier erg-mcp processes', each labelled savedRun), as before." } } },
    handler(_s, a: { limit?: number; scope?: AssertScope }) {
      const runs = runsFor(a.scope), others = a.scope === "all" ? 0 : allRuns().length - runs.length;
      if (!runs.length) return { runs: [], scope: a.scope ?? "session", note: `no Test run in this erg-mcp session yet (test counts them; a game without Oasis's log channel cannot)${others ? `; ${others} run${others > 1 ? "s" : ""} of earlier erg-mcp processes in the runs file: test_asserts {scope: "all"}` : ""}` };
      const pair = pairOf(runs), erg = pair.erg;
      let base = pair.base, standIn: string | undefined;
      // this session has an Erg run and no baseline: the runs file's latest baseline of the same game build stands in,
      // said so (Caldera v2 round 4: the baseline ran in one erg-mcp process and the Erg Test in the next, and the
      // session-scoped compare had no pair unless the tester asked for scope "all")
      if (a.scope !== "all" && erg && !base) {
        const sb = pairOf([...savedRuns().map(fromSaved).filter((r) => r.kind === "baseline"), erg]).base;
        if (sb) { base = sb; standIn = `this session has no baseline: the runs file's latest baseline of the same game build stands in (${sb.label}, ${new Date(sb.startedAt).toISOString()}, an earlier erg-mcp process's run); test {baseline: true} in this session for a same-session pair`; }
      }
      const from = (r: AssertRun) => (r.from === "saved" ? { savedRun: `an earlier erg-mcp process's run, ${new Date(r.startedAt).toISOString()}` } : {});
      return { scope: a.scope ?? "session", runs: [...runs, ...(standIn && base ? [base] : [])].map((r) => ({ ...summary(r, a.limit ?? 20), ...from(r) })), ...(others ? { otherProcesses: `${others} run${others > 1 ? "s" : ""} of earlier erg-mcp processes left out: test_asserts {scope: "all"} lists them` } : {}),
        ...(erg && base ? { compare: { ...diff(erg, base), ...(standIn ? { standIn } : erg.from === "saved" || base.from === "saved" ? { note: "a run from an earlier erg-mcp process (the runs file, Documents\\Melange\\erg-mcp\\assert-runs.json) is in the pair" } : {}) } }
          : { compareNote: `compare needs an Erg Test run and a vanilla baseline (test {baseline: true}) of the same game: there is ${erg ? "no baseline in this session or the runs file for this game build" : "no Erg run"} yet${others ? " (earlier processes' runs: scope \"all\")" : ""}` }) };
    },
  },
  {
    name: "levels_live",
    description: "Turn a map pack's levels live (on) or off in the running game (levels.live), so they show without a restart. Needs the game running offline at its main menu.",
    inputSchema: { type: "object", additionalProperties: false, required: ["modId", "on"], properties: { modId, on: { type: "boolean" } } },
    async handler(s, a: { modId: string; on: boolean }) {
      const conn = await s.ensure();
      if (!conn.methods.includes("levels.live")) throw new ToolError("no levels.live here: it needs the game running (Melange.exe --serve has none)");
      return (await s.client()).call("levels.live", { modId: a.modId, on: a.on });
    },
  },
];

// place_object's objects items take the single call's fields (but not objects) and a label.
{
  const po = tools.find((t) => t.name === "place_object")!.inputSchema.properties!;
  const { objects: _o, ...single } = po;
  po.objects.items!.properties = { ...single, label: { type: "string", description: "A name for this item in the answer (default objects[i] (type))." } };
}
