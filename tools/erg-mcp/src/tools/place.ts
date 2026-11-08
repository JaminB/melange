// Tool group "place": spawn knots, level objects (crates, telepads, triggers, the mine factory) and placed mines and
// oil drums, moving, turning, scaling and removing them, the level's settings (water, theme, time of day, spawn and
// surround modes), the level script, and shipping the level (export, build, test, live packs). Placing uses the Erg
// panel's own model (model/placing.ts, ground.ts), so a placed object is the one the palette places.
import {
  AddDetail, AddObjects, BUILTIN, Group, KNOT_RESOURCE, KNOTS, LIMITS, PAD_SPACING, RemoveDetail, SetDetail, SetLevel, SetObject, THEMES, TIMES_OF_DAY,
  catalogOf, checkScene, defaultObject, nextObjectKnot, nextTelepadGroup, objectOf, place, targetFrame,
  type Command, type CrateSpec, type Detail, type DetailFields, type EditorStore, type FactorySpec, type LevelSettings, type ObjectSpec, type ObjectType,
  type TimeOfDay, type Vec3,
} from "../core";
import {
  DEG, commit, detailInfo, findDetail, pointOut, pointToMap, resolvePoint, unitsSchema, vec3, type PointArgs, type Units,
} from "../lib/place-geom";
import { resolveFile, themeFile } from "../lib/materials";
import { runTest } from "../lib/place-wait";
import { stopHold } from "./game";
import { CAMERA_RISE, WALL_RADIUS, spawnWarnings } from "../lib/spawn-check";
import { checkFactory, factoryLines } from "../lib/factory-check";
import { scheduleFromScript } from "../lib/flood";
import { scriptWater } from "../lib/script-water";
import { rotation, slotOf } from "../lib/turns";
import { waterBackdropWarning } from "../lib/backdrop";
import { spansAt, waterMap } from "../lib/inspect-field";
import { grids, tidy } from "../lib/voxel";
import { ToolError, type Schema, type Tool } from "../registry";
import type { Session } from "../session";
import { validateExportForm } from "../../../../web/src/panels/erg/test/model";

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

/** The spawn checks (a wall at the turn camera, ground under the water) as warning lines in the caller's units. */
function cameraWarnings(store: EditorStore, units: Units, a: CameraArgs): string[] {
  const k = units === "map" ? 1 : 20;
  const len = (v: number) => `${Math.round(v * k * 100) / 100} ${units}`;
  return spawnWarnings(store, len, a.wallRadius === undefined ? WALL_RADIUS : a.wallRadius / k, a.cameraRise === undefined ? CAMERA_RISE : a.cameraRise / k);
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
    const d = store.scene.details.find((x) => x.name === KNOTS[w]);
    if (!d) continue;
    const p = store.frames.detailWorld(d), g = spansAt(gs, p[0], p[2], p[1] + 1.5)[0]?.top ?? null;
    let floods: number | null = null;
    if (g !== null && waterAt) for (let turn = 1; turn <= 200; turn++) if (waterAt(turn) >= g) { floods = turn; break; }
    all.set(w, { worm: KNOTS[w], ground: g, floods });
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

function spawnState(store: EditorStore, units: Units) {
  const scene = store.scene;
  const spawns = scene.details.filter((d) => d.role === "spawn").map((d) => detailInfo(store, d, units));
  const have = new Set(scene.details.map((d) => d.name));
  return { spawnMode: scene.spawns.mode, spawns, missingKnots: KNOTS.filter((k) => !have.has(k)) };
}

async function placeSpawnKnots(s: Session, a: { mode?: "random" | "knots"; spawns?: SpawnArg[]; units?: Units; balance?: Balance | boolean } & CameraArgs) {
  const store = s.store, scene = store.scene, units = a.units ?? "world";
  const mode = a.mode ?? "knots";
  const cmds: Command[] = [];
  if (scene.spawns.mode !== mode) cmds.push(new SetLevel({ spawns: mode }, `Spawn mode ${mode}`));
  const seen = new Set<number>();
  const done: { worm: number; action: "added" | "moved"; dropped: boolean; pos: Vec3; group?: string; was?: number }[] = [];
  // resolve every point first; with balance the worm numbers are dealt again over them
  const items = (a.spawns ?? []).map((sp) => {
    if (seen.has(sp.worm)) throw new ToolError(`WORM${sp.worm} is given twice`);
    seen.add(sp.worm);
    const r = resolvePoint(store, sp, units, `WORM${sp.worm}`);
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
    const name = KNOTS[sp.worm];
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
  const teams = bal?.teams ?? 2, per = bal?.wormsPerTeam ?? (bal ? Math.ceil(items.length / teams) : 4);
  if (!cmds.length) return { mode, changed: false, note: "nothing to change", ...spawnState(store, units), teams: await teamReport(s, units, teams, per), warnings: cameraWarnings(store, units, a) };
  commit(store, cmds.length === 1 ? cmds[0] : new Group(a.spawns?.length ? "Place spawn knots" : `Spawn mode ${mode}`, cmds));
  return { mode, changed: true, placed: done, ...(bal ? { balanced: { mode: bal.mode ?? "height", teams, wormsPerTeam: per, note: "worm numbers dealt over the given positions (was: the worm asked for): heights snaked over the teams, a team's spawns spread over groups, the lowest of a team moving first" } } : {}),
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
}

type FactoryArgs = Pick<PlaceArgs, "activation" | "inactiveTurns" | "schemeMines" | "keepKnot" | "launchHeight" | "launchRadius" | "launch">;
const FACTORY_GAME = { activation: 20, inactiveTurns: 7, schemeMines: true, keepKnot: false } as const;

/** The factory settings after `a` is applied to `f`: a value equal to the game's is dropped (the game's value stays). */
function factoryOf(f: FactorySpec | undefined, a: FactoryArgs): FactorySpec | undefined {
  const next: Record<string, number | boolean> = { ...(f ?? {}) };
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
  return Object.keys(next).length ? (next as FactorySpec) : undefined;
}

const factoryProps: Record<string, Schema> = {
  activation: { type: "integer", minimum: 0, maximum: 255, description: "minefactory: it fires at a turn start when fewer than this many mines are in the level, topping up to it, at most 10 per fire (game 20; 0 never fires)." },
  inactiveTurns: { type: "integer", minimum: 0, maximum: 255, description: "minefactory: turn starts it waits after it is placed and after each fire before it checks again (game 7; 0 checks every turn)." },
  schemeMines: { type: "boolean", description: "minefactory: false = the scheme's 15 random start mines are not placed, so mines come only from the factory (oil drums stay; default true)." },
  keepKnot: { type: "boolean", description: "minefactory: true = when the game's drop from the knot hits no land (a draped skin, the crater pads on Caldera) the factory stays at its knot instead of the match's stale sweep point, where it was lost (Melange 0.13+; default false, the game's own drop). Put the knot on the surface (place_object snap: \"surface\")." },
  launchHeight: { type: "integer", minimum: 0, maximum: 255, description: "minefactory: map units over the factory its mines appear at when it fires (Melange 0.13+; with launchRadius alone, 20): they fall from there round the factory, instead of the game's placement, which rains them onto random worms and objects (on Caldera 28-95 map units from the factory). 0 gives the game's placement back." },
  launchRadius: { type: "integer", minimum: 0, maximum: 255, description: "minefactory: map units round the factory its mines appear within (an even spread over the disc; default 0, straight over it), so they land on the cone round a crater." },
  launch: { type: "boolean", description: "minefactory: false clears launchHeight and launchRadius (the game's placement)." },
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
        const f = factoryOf(undefined, a);
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

async function listObjects(s: Session, a: { role?: string; search?: string; limit?: number; units?: Units } & CameraArgs) {
  const store = s.store, scene = store.scene, units = a.units ?? "world";
  const q = a.search?.toLowerCase();
  const roles = a.role ? [a.role] : ["spawn", "object"];
  const all = scene.details.filter((d) => roles.includes(d.role) && (!q || `${d.name} ${d.resource}`.toLowerCase().includes(q)));
  const listed = all.slice(0, a.limit ?? 200).map((d) => detailInfo(store, d, units));
  return {
    spawnMode: scene.spawns.mode, water: scene.water.level, units, matching: all.length, listed: listed.length,
    spawns: listed.filter((d) => d.role === "spawn"),
    levelObjects: listed.filter((d) => d.object),
    placed: listed.filter((d) => d.role !== "spawn" && !d.object),
    counts: { objects: `${scene.objects?.length ?? 0} of ${LIMITS.objects}`, details: scene.details.length },
    issues: [...issuesOf(store), ...cameraWarnings(store, units, a), ...(await factoryReport(s, units))],
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

async function savedFirst(s: Session, why: string) {
  const p = s.requireProject();
  if (!p.store.dirty) return undefined;
  const r = await s.save();
  if (r.problems.length) throw new ToolError(`the project has problems and was not ${why}: ${r.problems.join("; ")}`);
  return r;
}

const modId: Schema = { type: "string", description: "Mod id: a-z, 0-9, '-' and '_' (it becomes the pack prefix; 'ergtest' is reserved)." };

export const tools: Tool[] = [
  {
    name: "place_spawn_knots",
    description: "Set the spawn mode and place or move the spawn knots WORM0-WORM7. mode 'knots' makes worm i start on knot i (a match needs all 8); 'random' leaves the game's random spawns (existing knots stay but are unused). Each spawn is a position [x,y,z] or xz (dropped onto the terrain). An existing WORMn is moved, a missing one added. One undo step. Answers the spawns, the knots still missing and warnings: a spawn whose turn camera may start inside a wall (ground within wallRadius at cameraRise above the ground it stands on) or that stands on ground under the water.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        mode: { type: "string", enum: ["random", "knots"], description: "Spawn mode (default knots)." },
        spawns: {
          type: "array", maxItems: 8, description: "The knots to place or move.",
          items: { type: "object", additionalProperties: false, required: ["worm"],
            properties: { worm: { type: "integer", minimum: 0, maximum: 7, description: "0-7: WORM0-WORM7." }, ...pointProps,
              group: { type: "string", description: "With balance: a label such as the castle the spawn is in; a team's spawns are spread over different groups." } } },
        },
        balance: { anyOf: [{ type: "boolean" }, { type: "object", additionalProperties: false, properties: {
          teams: { type: "integer", minimum: 1, maximum: 8 }, wormsPerTeam: { type: "integer", minimum: 1, maximum: 8 }, mode: { type: "string", enum: ["height", "interleave"] } } }],
          description: "Deal the worm numbers again over the given positions so the teams stand alike (WORMn is worm n % wormsPerTeam of team floor(n / wormsPerTeam); default 2 teams of 4): height (default) snakes the heights over the teams (A B B A ...: equal sums) and spreads a team's spawns over different `group`s; interleave alternates A B A B. In a team the lowest spawn moves first. The answer's teams gives each team's mean and lowest spawn height and earliest flood turn." },
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
        limit: { type: "integer", minimum: 1, maximum: 5000, description: "Details to list (default 200)." }, units: unitsSchema, ...cameraProps },
    },
    handler: (s, a) => listObjects(s, a),
  },
  {
    name: "set_mine_factory",
    description: "Change the level's mine factory settings (place it first with place_object type minefactory). launchHeight and launchRadius (Melange 0.13+) make its mines appear over it and fall round it instead of raining onto random worms and objects (the game's). A value equal to the game's (activation 20, inactiveTurns 7, schemeMines true, keepKnot false) is dropped, so the game's value applies. Melange writes activation and inactiveTurns into the game's kMineFactoryData at level start and again at the factory's own set-up, just before it reads them (Melange 0.13+), and puts the game's back when the match ends; test's look.factory says what the set-up found and applied. The settings are the project's: save, and Test the same project on the same server's projects folder (a project of the same id in another game's ProjectsDir is another project). One undo step.",
    inputSchema: { type: "object", additionalProperties: false, properties: factoryProps },
    handler(s, a: FactoryArgs) {
      const store = s.store, scene = store.scene;
      const o = scene.objects?.find((x) => x.type === "minefactory");
      if (!o || o.type !== "minefactory") throw new ToolError("the level has no mine factory: place one with place_object type minefactory");
      if (a.activation === undefined && a.inactiveTurns === undefined && a.schemeMines === undefined && a.keepKnot === undefined && a.launchHeight === undefined &&
        a.launchRadius === undefined && a.launch === undefined) throw new ToolError("give activation, inactiveTurns, schemeMines, keepKnot, launchHeight, launchRadius or launch");
      if (a.launch === true) throw new ToolError("launch: false clears the launch; set launchHeight and/or launchRadius to give one");
      const factory = factoryOf(o.factory, a);
      commit(store, new SetObject(o.knot, factory ? { knot: o.knot, type: "minefactory", factory } : { knot: o.knot, type: "minefactory" }));
      return { knot: o.knot, factory: factory ?? {}, game: FACTORY_GAME, changed: true };
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
        mode: { type: "string", enum: ["source", "install"] }, survivor: { type: "boolean", description: "Also list the level in Survivor's Prebuilt maps (default false)." } },
    },
    async handler(s, a: { modId: string; name: string; version?: string; mode?: "source" | "install"; survivor?: boolean }) {
      const p = s.requireProject();
      const form = { modId: a.modId, name: a.name, version: a.version ?? "1.0.0", mode: a.mode ?? "source", survivor: !!a.survivor };
      const bad = validateExportForm(form);
      if (bad.length) throw new ToolError(bad.join("; "));
      const first = await savedFirst(s, "exported");
      const r = await (await s.client()).call<{ dir: string; files: string[]; restartRequired: boolean }>(
        "level.export", { project: p.id, ...form }, 120000);
      return { ...r, mode: form.mode, ...(first ? { savedFirst: true, warnings: first.warnings } : {}),
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
        timeoutSec: { type: "integer", minimum: 1, maximum: 600, description: "Stop waiting after this long (default 60)." },
        cleanMods: { type: "boolean", description: "A clean room for this Test match only: enabled content mods' sim code (entry.sim) does not run, only the level script, and the post-FX stack is off (unless postfx: true), so asserts and lighting belong to the map. Nothing is changed on disk; launch-time content (weapon clones, file overrides, shader mods, client scripts) stays. On a game build without clean rooms it refuses while such mods are enabled." },
        postfx: { type: "boolean", description: "false: the post-FX stack (every effect and code pass, like the Post-FX panel's Bypass all) is off for this Test match; true with cleanMods keeps it on. Default: on, or off with cleanMods." },
        restart: { type: "boolean", description: "When a Test match is running: leave it for the main menu (level.test.quit), wait for the menu, then test again (default false: refused in a match). Leaving a match writes the game's save (its statistics), as the game's own Quit does." } },
    },
    async handler(s, a: { tod?: TimeOfDay; waitFor?: "playing" | "armed"; timeoutSec?: number; cleanMods?: boolean; postfx?: boolean; restart?: boolean }) {

      const p = s.requireProject();
      const conn = await s.ensure();
      if (!conn.methods.includes("level.test")) throw new ToolError("no level.test here: Test needs the game running at its main menu (Melange.exe --serve has none)");
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
      // the project's own time of day unless one is given: without it the frontend's (Quick Game: DAY) loads
      const db = p.store.scene.databank, tod = a.tod ?? (TIMES_OF_DAY.includes(db.timeOfDay as TimeOfDay) ? (db.timeOfDay as TimeOfDay) : undefined);
      // the game refuses a Test outside its frontend: say how to get there instead of its bare refusal
      let run = await runTest(p.erg, { tod, goal: a.waitFor, timeoutMs: sec * 1000, cleanMods: a.cleanMods, postfx: a.postfx }).catch((e: Error) => {
        if (/must be at the frontend/.test(e.message))
          throw new ToolError(`${e.message}: a match is running. Pass restart: true to leave a Test match for the menu and test again in one call, or call end_match first (leaving writes the game's save, as its own Quit does)`);
        throw e;
      });
      // Just after a match the frontend may not take Quick Game yet, and the menu's attract demo can start first: the
      // game refuses that Test (kAttractRefused); from the menu a second try starts it.
      for (let tries = 0; restarted && run.final === "failed" && /attract demo/.test(String(run.failed)) && tries < 2; tries++) {
        await new Promise((ok) => setTimeout(ok, 3000));
        run = await runTest(p.erg, { tod, goal: a.waitFor, timeoutMs: sec * 1000, cleanMods: a.cleanMods, postfx: a.postfx });
        restarted.retries = tries + 1;
      }
      const suppressed = !!a.cleanMods && !!run.cleanRoom && !run.cleanRoom.simMods;
      const fo = p.store.scene.objects?.find((o) => o.type === "minefactory");
      const look = run.final === "playing" ? await appliedLook(s, { theme: db.theme, tod: tod ?? "DAY", factory: fo && fo.type === "minefactory" ? fo.factory ?? {} : null, stem: run.key?.replace(/^Multi\./, ""),
        script: run.final === "playing" && !!(await (async () => (await p.erg.script()) ?? "")().catch(() => "")).trim() }) : null;
      return { ...run, ...(first ? { savedFirst: true } : {}), ...(restarted ? { restarted } : {}), ...(look ? { look: look.look, ...(look.warnings.length ? { warnings: look.warnings } : {}) } : {}),
        ...(simMods.length ? suppressed ? { simMods, simModsNote: "these mods' sim code is suppressed in this Test match (clean room)" }
          : { simMods, simModsNote: "these mods' sim code runs in this match beside the level script" } : {}),
        match: "Quick Game (QuickStartHvC: a human team against a computer team) with the game's own Quick Game scheme; the scheme, turn time and team sizes are the game's, not chosen here",
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
      const r = await backToMenu(s);
      return r ? { left: true, ...r, next: "test (the game is at its menu)" } : { left: false, note: "no match is running: the game is at its menu" };
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
