// Tool group "artpass" (detail layer round 9): the round-8 asset judge's tool gaps.
//   plan_contour       contour-following placement (lib/contour.ts): sites along iso-height lines with the slope tangent
//   prop_pixels        gameplay-distance size of props per camera (lib/prop-pixels.ts): what drops under a few pixels
//   game_overlap       which WormsMayhem games run besides this test folder's (lib/perf-guard.ts)
//   game_perf_guarded  game_perf that refuses (or flags) a run overlapping another folder's game
// The first two are read-only on the project; the last two read the machine's processes.
import { execFileSync } from "node:child_process";
import { dropPoint } from "../core";
import { topOf } from "../lib/place-geom";
import { contourRuns, type HeightGrid } from "../lib/contour";
import { pixelReport, type PropBox, type V3 } from "../lib/prop-pixels";
import { parseCim, splitGames, type Proc } from "../lib/perf-guard";
import { seeded } from "../lib/tint-jitter";
import { classifyDetail } from "../lib/sim-fingerprint";
import { placedMeshes } from "../lib/scenery-mesh";
import { tools as gameTools } from "./game";
import { ToolError, type Tool } from "../registry";

const r2 = (v: number) => Math.round(v * 100) / 100;
const r3 = (v: number) => Math.round(v * 1000) / 1000;

function runningGames(): Proc[] {
  const ps = "Get-CimInstance Win32_Process -Filter \"Name='WormsMayhem.exe'\" | Select-Object ProcessId,ExecutablePath | ConvertTo-Json -Compress";
  try {
    return parseCim(execFileSync("powershell", ["-NoProfile", "-NonInteractive", "-Command", ps], { encoding: "utf8", windowsHide: true, timeout: 20000 }));
  } catch (e) {
    throw new ToolError(`could not list the running games: ${(e as Error).message.slice(0, 200)}`);
  }
}
function gameDirArg(d?: string): string {
  const g = d ?? process.env.MELANGE_GAME_DIR;
  if (!g) throw new ToolError("give gameDir (the measured game's folder) or set MELANGE_GAME_DIR");
  return g;
}

export const tools: Tool[] = [
  {
    name: "plan_contour",
    description: "Contour-following placement sites: the ground (the highest solid voxel of every frame, map units) is sampled on a `cell` grid (default 1.5) over `area` ({centre: [x, z], rMin, rMax} or {min: [x, z], max: [x, z]}), and iso-height lines are traced (marching squares) at each of `levels` (or `band` {from, to, every}). Sites are laid along each line every `spacing` (default 3; `jitter` 0-0.9 varies each step, seeded by `seed`), each with pos (on the line, y = its level), the ground's normal, the horizontal `tangent` along the contour (lay a piece's long axis on it: place_aligned takes normal and tangent as they are) and its slope; consecutive sites whose slope is within `minSlope`..`maxSlope` (default 0-90) form a RUN (a run breaks where the slope leaves the band). Runs shorter than `minRun` sites (default 1) are dropped. For ledges, slabs and strata that follow a face instead of beading across it. Read-only.",
    inputSchema: { type: "object", additionalProperties: false, required: ["area"], properties: {
      area: { type: "object", additionalProperties: false, properties: {
        centre: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 }, rMin: { type: "number", minimum: 0 }, rMax: { type: "number", minimum: 0 },
        min: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 }, max: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 } } },
      levels: { type: "array", items: { type: "number" }, minItems: 1, maxItems: 200 },
      band: { type: "object", additionalProperties: false, required: ["from", "to", "every"], properties: { from: { type: "number" }, to: { type: "number" }, every: { type: "number", minimum: 0.1 } } },
      cell: { type: "number", minimum: 0.25, maximum: 10 }, spacing: { type: "number", minimum: 0.2, maximum: 100 },
      jitter: { type: "number", minimum: 0, maximum: 0.9 }, seed: { type: "integer" },
      minSlope: { type: "number", minimum: 0, maximum: 90 }, maxSlope: { type: "number", minimum: 0, maximum: 90 },
      minRun: { type: "integer", minimum: 1, maximum: 1000 }, limit: { type: "integer", minimum: 1, maximum: 20000 },
    } },
    handler(s, a: { area: { centre?: number[]; rMin?: number; rMax?: number; min?: number[]; max?: number[] }; levels?: number[]; band?: { from: number; to: number; every: number };
      cell?: number; spacing?: number; jitter?: number; seed?: number; minSlope?: number; maxSlope?: number; minRun?: number; limit?: number }) {
      const { store } = s.requireProject();
      let x0: number, z0: number, x1: number, z1: number, inArea: (x: number, z: number) => boolean;
      if (a.area.centre) {
        const [cx, cz] = a.area.centre, rMax = a.area.rMax ?? 50, rMin = a.area.rMin ?? 0;
        x0 = cx - rMax; x1 = cx + rMax; z0 = cz - rMax; z1 = cz + rMax;
        inArea = (x, z) => { const r = Math.hypot(x - cx, z - cz); return r >= rMin && r <= rMax; };
      } else if (a.area.min && a.area.max) {
        [x0, z0] = a.area.min; [x1, z1] = a.area.max; inArea = () => true;
      } else throw new ToolError("area: give centre (with rMax) or min and max");
      const levels = a.levels ?? (a.band ? Array.from({ length: Math.floor((a.band.to - a.band.from) / a.band.every + 1e-9) + 1 }, (_, i) => a.band!.from + i * a.band!.every) : null);
      if (!levels || !levels.length) throw new ToolError("give levels or band");
      if (levels.length > 200) throw new ToolError(`${levels.length} levels: at most 200`);
      const cell = a.cell ?? 1.5, nx = Math.floor((x1 - x0) / cell) + 1, nz = Math.floor((z1 - z0) / cell) + 1;
      if (nx * nz > 160000) throw new ToolError(`the grid would be ${nx} x ${nz}: use a larger cell or a smaller area`);
      const top = topOf(store), vox = (id: number) => store.voxelsOf({ id });
      const y: (number | null)[] = new Array(nx * nz);
      for (let iz = 0; iz < nz; iz++) for (let ix = 0; ix < nx; ix++) {
        const x = x0 + ix * cell, z = z0 + iz * cell;
        y[iz * nx + ix] = inArea(x, z) ? dropPoint(store.scene, store.frames, vox, [x, top, z])?.[1] ?? null : null;
      }
      const g: HeightGrid = { x0, z0, cell, nx, nz, y };
      const runs = contourRuns(g, levels, { spacing: a.spacing ?? 3, minSlope: a.minSlope, maxSlope: a.maxSlope, minRun: a.minRun, jitter: a.jitter, rand: seeded(a.seed ?? 1) });
      runs.sort((p, q) => q.sites.length - p.sites.length);
      let left = a.limit ?? 4000;
      const out = [];
      for (const r of runs) {
        if (left <= 0) break;
        const sites = r.sites.slice(0, left);
        left -= sites.length;
        out.push({ level: r2(r.level), count: sites.length, length: r2(r.length), sites: sites.map((t) => ({ pos: t.pos.map(r2), normal: t.normal.map(r3), tangent: t.tangent.map(r3), slope: Math.round(t.slope) })) });
      }
      return { units: "map", grid: [nx, nz], levels: levels.length, runs: out, total: runs.reduce((n, r) => n + r.sites.length, 0), truncated: left <= 0 };
    },
  },
  {
    name: "prop_pixels",
    description: "Gameplay-distance size of the layer's props: the scenery copies whose names contain any of `search` (default [\"_dtl\"]), read as placed (their meshes' boxes, map units), are projected into each of `cams` ({name, pos, target, fov (vertical degrees, default 55)}, map units; `width` x `height` default 1280 x 720): per camera the props in view, how many draw under `minPx` (default 3: the projected diameter of the box's bounding sphere) and the median size; per prop its largest size over the cameras that see it; and `subPixel`, the ids under minPx in every camera that sees them (or seen by none): the props that only add draw calls at that distance. `byTag` splits the counts by each search fragment. Read-only.",
    inputSchema: { type: "object", additionalProperties: false, required: ["cams"], properties: {
      search: { type: "array", items: { type: "string" }, minItems: 1, maxItems: 8 },
      cams: { type: "array", minItems: 1, maxItems: 32, items: { type: "object", required: ["pos", "target"], properties: {
        name: { type: "string" }, pos: { type: "array", items: { type: "number" }, minItems: 3, maxItems: 3 }, target: { type: "array", items: { type: "number" }, minItems: 3, maxItems: 3 }, fov: { type: "number", minimum: 5, maximum: 170 } } } },
      width: { type: "integer", minimum: 16, maximum: 8192 }, height: { type: "integer", minimum: 16, maximum: 8192 },
      minPx: { type: "number", minimum: 0 }, limit: { type: "integer", minimum: 0, maximum: 5000 },
    } },
    async handler(s, a: { search?: string[]; cams: { name?: string; pos: number[]; target: number[]; fov?: number }[]; width?: number; height?: number; minPx?: number; limit?: number }) {
      const { store } = s.requireProject();
      const qs = (a.search ?? ["_dtl"]).map((q) => q.toLowerCase());
      const ds = store.scene.details.filter((d) => d.src === null && classifyDetail(d).role === "scenery" && qs.some((q) => d.name.toLowerCase().includes(q)));
      if (!ds.length) throw new ToolError(`no scenery copy's name contains any of ${JSON.stringify(a.search ?? ["_dtl"])}`);
      const { meshes, without } = await placedMeshes(s, store.frames, ds);
      const tagOf = new Map(ds.map((d) => [d.id, a.search ? (a.search.find((q) => d.name.toLowerCase().includes(q.toLowerCase())) ?? "") : "_dtl"]));
      const props: PropBox[] = meshes.map((m) => {
        const min: V3 = [Infinity, Infinity, Infinity], max: V3 = [-Infinity, -Infinity, -Infinity];
        for (let i = 0; i < m.tris.length; i += 3) for (let k = 0; k < 3; k++) { min[k] = Math.min(min[k], m.tris[i + k]); max[k] = Math.max(max[k], m.tris[i + k]); }
        return { id: m.detail, min, max, tag: tagOf.get(m.detail) };
      });
      const minPx = a.minPx ?? 3;
      const cams = a.cams.map((c, i) => ({ name: c.name ?? `cam${i}`, pos: c.pos as V3, target: c.target as V3, fov: c.fov, width: a.width, height: a.height }));
      const rep = pixelReport(props, cams, minPx);
      const sub = new Set(rep.subPixel);
      const byTag: Record<string, { props: number; subPixel: number }> = {};
      for (const p of props) { const t = (byTag[p.tag ?? ""] ??= { props: 0, subPixel: 0 }); t.props++; if (sub.has(p.id)) t.subPixel++; }
      const lim = a.limit ?? 400;
      return { units: "map", minPx, props: props.length, noMesh: without.slice(0, 10), byTag,
        cams: rep.cams.map((c) => ({ name: c.name, inView: c.inView, under: c.under, median: c.median })),
        subPixel: rep.subPixel.slice(0, lim), subPixelCount: rep.subPixel.length,
        smallest: rep.best.filter((b) => b.px !== null).sort((p, q) => p.px! - q.px!).slice(0, Math.min(lim, 40)) };
    },
  },
  {
    name: "game_overlap",
    description: "Which WormsMayhem.exe games run on this machine besides the one in `gameDir` (default MELANGE_GAME_DIR): each with its pid, path and test folder (testenv\\B ...). Two games share the GPU, so a frame time measured while another folder's game runs is not this level's cost: `clear` is true when no other game runs. Read-only (Windows: Get-CimInstance Win32_Process).",
    inputSchema: { type: "object", additionalProperties: false, properties: { gameDir: { type: "string" } } },
    handler(_s, a: { gameDir?: string }) {
      const dir = gameDirArg(a.gameDir), sp = splitGames(runningGames(), dir);
      return { gameDir: dir, clear: sp.others.length === 0, mine: sp.mine, others: sp.others };
    },
  },
  {
    name: "game_perf_guarded",
    description: "game_perf (same arguments: seconds, everyMs, label, against) with a lock against other games: before and after the run it lists the WormsMayhem games outside `gameDir` (default MELANGE_GAME_DIR). It REFUSES to measure while another folder's game runs (unless `allowOverlap`: then the run is measured and flagged `overlap`, and its label gets the suffix _OVERLAP so it never stands as a clean baseline). Answers game_perf's answer plus `guard` {before, after, overlap}.",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      seconds: { type: "number", minimum: 1, maximum: 120 }, everyMs: { type: "integer", minimum: 50, maximum: 2000 }, label: { type: "string" }, against: { type: "string" },
      gameDir: { type: "string" }, allowOverlap: { type: "boolean" },
    } },
    async handler(s, a: { seconds?: number; everyMs?: number; label?: string; against?: string; gameDir?: string; allowOverlap?: boolean }) {
      const dir = gameDirArg(a.gameDir);
      const perf = gameTools.find((t) => t.name === "game_perf");
      if (!perf) throw new ToolError("game_perf is missing from the game group");
      const before = splitGames(runningGames(), dir).others;
      if (before.length && !a.allowOverlap)
        throw new ToolError(`refused: another game runs (${before.map((o) => `pid ${o.pid} ${o.testenv ? "testenv " + o.testenv : o.path ?? "?"}`).join(", ")}): its GPU load would be counted as this level's. Wait for it to end, or pass allowOverlap to measure a flagged run.`);
      const label = before.length ? `${a.label ?? "run"}_OVERLAP` : a.label;
      const r = await perf.handler(s, { seconds: a.seconds, everyMs: a.everyMs, label, against: a.against });
      const after = splitGames(runningGames(), dir).others;
      const overlap = before.length > 0 || after.length > 0;
      return { ...(typeof r === "object" && r ? r : { result: r }), guard: { before, after, overlap, ...(overlap ? { note: "another folder's game ran during the run: not a clean measure" } : {}) } };
    },
  },
];
