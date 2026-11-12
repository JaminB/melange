// Tool group "levellook": what a level's pack says about the level beyond the level file, both written by `export`
// into the level's spice.json entry (Melange 0.15): its time of day pinned for every match (levels[].timeOfDay: Versus
// picks one at random otherwise, so a map lit for DAY, with a sea material and a land light set for it, plays at night)
// and its picture in the map menus (levels[].image, a PNG in the pack; without one every Erg level shows its base's,
// the diner). The settings are kept per project for this erg-mcp session and `export` reads them (its own `timeOfDay`
// and `thumbnail` arguments say the same for one export).
import { existsSync, readFileSync, statSync } from "node:fs";
import { SetLevel, TIMES_OF_DAY, type TimeOfDay, type Vec3 } from "../core";
import { waterMap } from "../lib/inspect-field";
import { fittedPose, landPoints, levelCameraOf, setLevelCamera } from "../lib/level-camera";
import { commit } from "../lib/place-geom";
import { WORLD_PER_MAP, grids, isSolid } from "../lib/voxel";
import { ToolError, type Tool } from "../registry";
import type { Session } from "../session";
import { renderViewTool } from "./render";

export interface LevelLook { lock?: boolean; thumbnail?: { png: Uint8Array; from: string } }
const looks = new Map<string, LevelLook>();
/** The open project's level look of this session (set_level_look, set_level_thumbnail). */
export const levelLookOf = (projectId: string): LevelLook => looks.get(projectId) ?? {};
const edit = (id: string, f: (l: LevelLook) => void) => { const l = { ...levelLookOf(id) }; f(l); looks.set(id, l); return l; };

const PNG_SIG = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];
/** A PNG's bytes checked: the signature, its size (from the IHDR) within Melange's 4096 a side, at most 8 MB. */
export function checkPng(png: Uint8Array, what: string): { width: number; height: number } {
  if (png.length < 33 || !PNG_SIG.every((b, i) => png[i] === b)) throw new ToolError(`${what} is not a PNG`);
  if (png.length > 8 * 1024 * 1024) throw new ToolError(`${what} is over 8 MB`);
  const v = new DataView(png.buffer, png.byteOffset, png.byteLength), width = v.getUint32(16), height = v.getUint32(20);
  if (width < 16 || height < 16 || width > 4096 || height > 4096) throw new ToolError(`${what} is ${width} x ${height}: a picture of 16 to 4096 pixels a side`);
  return { width, height };
}

/** A thumbnail from a file: its bytes, checked. */
export function thumbnailFromFile(file: string): { png: Uint8Array; from: string; width: number; height: number } {
  if (!existsSync(file) || statSync(file).isDirectory()) throw new ToolError(`no file ${file}`);
  const png = new Uint8Array(readFileSync(file));
  return { png, from: file, ...checkPng(png, file) };
}

const BEARINGS: Record<string, number> = { n: 0, ne: 45, e: 90, se: 135, s: 180, sw: 225, w: 270, nw: 315 };
interface CameraArgs { pos?: number[]; target?: number[]; view?: string; azimuth?: number; elevation?: number; distance?: number; fit?: "island" | "solid"; margin?: number; fov?: number;
  aspect?: number; units?: "map" | "world"; clear?: boolean; }

export const tools: Tool[] = [
  {
    name: "set_level_camera",
    description: "The level camera: the one pose that shows the whole level, chosen by its builder instead of the far default overview (0.95 x the level's span back, where a level haze washes the island out). Give `pos` and `target`, or a side: `view` (n, ne, e, se, s, sw, w, nw: where the eye stands) or `azimuth`, with `elevation` (default 40) and optionally `target` (default the middle of what is framed) and `distance`; left out, the distance is the closest that shows all of `fit`: island (default: the land over the project's water, so the sea bed, a shell under the skin and a wiped base's empty frames do not pull it back) or solid (every voxel), times `margin` (default 1.08), in a view of `fov` (default 60) and `aspect` (default 16:9). `clear: true` drops it; no arguments answer the current one. It is kept for the open project in this erg-mcp session, like set_level_look, and is what these then use: game_camera and game_shots {preset: \"overview\"} (unless the call names its own azimuth, elevation, distance or fit), render_view {view: \"level\"}, set_level_thumbnail {render} with no camera of its own, and render_view {gameMatch}'s haze check. It edits nothing in the level (no undo step, sim_fingerprint unchanged) and the game's own cameras are not moved by it: a match still opens on the game's turn camera.",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      pos: { type: "array", items: { type: "number" }, minItems: 3, maxItems: 3, description: "Eye [x, y, z] (with target)." },
      target: { type: "array", items: { type: "number" }, minItems: 3, maxItems: 3, description: "Point looked at [x, y, z]; with a view, the point it orbits." },
      view: { type: "string", enum: Object.keys(BEARINGS), description: "The side the eye stands on." },
      azimuth: { type: "number", minimum: 0, maximum: 360, description: "The bearing from the target to the eye, degrees clockwise from north (-z), instead of view." },
      elevation: { type: "number", minimum: 5, maximum: 89, description: "Degrees above the target (default 40)." },
      distance: { type: "number", minimum: 0.1, description: "From the target to the eye (default: fitted)." },
      fit: { type: "string", enum: ["island", "solid"], description: "What a fitted distance frames (default island)." },
      margin: { type: "number", minimum: 1, maximum: 4, description: "Times the closest fitting distance (default 1.08)." },
      fov: { type: "number", minimum: 5, maximum: 150, description: "Vertical field of view, degrees (default 60)." },
      aspect: { type: "number", minimum: 0.2, maximum: 5, description: "Width over height the fit is for (default 16:9, the game's)." },
      units: { type: "string", enum: ["map", "world"], description: "Unit of pos, target and distance (default map)." },
      clear: { type: "boolean" },
    } },
    handler(s: Session, a: CameraArgs) {
      const p = s.requireProject(), k = a.units === "world" ? 1 / WORLD_PER_MAP : 1;
      const r2 = (v: number) => Math.round(v * 100) / 100;
      const out = () => {
        const c = levelCameraOf(p.id);
        if (!c) return { project: p.id, camera: null, note: "no level camera: the overview preset's own framing (set_overview) is used" };
        const d = Math.hypot(c.pos[0] - c.target[0], c.pos[1] - c.target[1], c.pos[2] - c.target[2]);
        return { project: p.id, camera: { pos: { map: c.pos.map((v) => r2(v / WORLD_PER_MAP)), world: c.pos.map(r2) }, target: { map: c.target.map((v) => r2(v / WORLD_PER_MAP)), world: c.target.map(r2) }, fov: c.fov,
          distance: { map: r2(d / WORLD_PER_MAP), world: r2(d) }, how: c.how },
          usedBy: "game_camera and game_shots {preset: \"overview\"}, render_view {view: \"level\"}, set_level_thumbnail {render}, render_view {gameMatch}",
          note: "kept for this project in this erg-mcp session; nothing in the level is edited, and the game's own cameras are not moved by it" };
      };
      const given = [a.pos, a.target, a.view, a.azimuth, a.elevation, a.distance, a.fit, a.margin].some((v) => v !== undefined);
      if (a.clear) { if (given) throw new ToolError("clear drops the level camera: give nothing else with it"); setLevelCamera(p.id, null); return out(); }
      if (!given) return out();
      const vec = (v: number[], what: string): Vec3 => { if (v.length !== 3 || !v.every(Number.isFinite)) throw new ToolError(`${what}: [x, y, z]`); return [v[0] * k, v[1] * k, v[2] * k]; };
      const fov = a.fov ?? 60;
      if (a.pos) {
        if (a.view !== undefined || a.azimuth !== undefined || a.elevation !== undefined || a.distance !== undefined || a.fit !== undefined) throw new ToolError("give pos and target, or a view (or azimuth) with elevation, not both");
        if (!a.target) throw new ToolError("pos needs a target");
        const pos = vec(a.pos, "pos"), target = vec(a.target, "target");
        if (Math.hypot(pos[0] - target[0], pos[1] - target[1], pos[2] - target[2]) < 1e-6) throw new ToolError("pos and target are the same point");
        setLevelCamera(p.id, { pos: pos.map((v) => v * WORLD_PER_MAP) as Vec3, target: target.map((v) => v * WORLD_PER_MAP) as Vec3, fov, how: "pos and target" });
        return out();
      }
      if (a.view !== undefined && a.azimuth !== undefined) throw new ToolError("give view or azimuth, not both");
      if (a.view === undefined && a.azimuth === undefined) throw new ToolError("give pos and target, or a view (or azimuth)");
      const gs = grids(p.store).filter((g) => g.words.some(isSolid));
      if (!gs.length) throw new ToolError("the level has no voxels to frame: give pos and target");
      const fit = a.fit ?? "island", water = waterMap(p.store);
      let pts = landPoints(gs, water, fit);
      let framed: string = fit === "island" ? (water === null ? "every column's top (the project keeps the level's own water height)" : "the land over the water") : "every column's top";
      if (!pts.length) { pts = landPoints(gs, null, "solid"); framed = "every column's top (no land stands over the water)"; }
      const az = a.azimuth ?? BEARINGS[a.view!], el = a.elevation ?? 40;
      const pose = fittedPose(pts, { azimuth: az, elevation: el, ...(a.target ? { target: vec(a.target, "target") } : {}), ...(a.distance !== undefined ? { distance: a.distance * k } : {}), fov, aspect: a.aspect, margin: a.margin });
      setLevelCamera(p.id, { pos: pose.pos.map((v) => v * WORLD_PER_MAP) as Vec3, target: pose.target.map((v) => v * WORLD_PER_MAP) as Vec3, fov,
        how: `${a.view ?? `azimuth ${az}`}, ${el} degrees up, ${a.distance !== undefined ? "at the distance given" : `fitted to ${framed}`}` });
      return out();
    },
  },
  {
    name: "set_level_look",
    description: "The level's time of day and whether it is PINNED for every match. `timeOfDay` (DAY, EVENING, NIGHT) sets the level's own, as set_time_of_day does (one undo step). `lock: true` pins it: `export` then writes the level's time of day into its pack entry (spice.json levels[].timeOfDay, Melange 0.15+), and Melange loads the level at that time in Versus and network games too, where the game otherwise picks DAY, EVENING or NIGHT at random and a look set for one (set_water_look's sea and land light, a tint) plays under another. `lock: false` takes the pin off (the frontend's pick again). The pin is kept for the open project in this erg-mcp session: call it before export (export's own `timeOfDay` argument says the same for one export). With no arguments it answers the current state.",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      timeOfDay: { type: "string", enum: TIMES_OF_DAY, description: "The level's time of day (as set_time_of_day)." },
      lock: { type: "boolean", description: "true: pin the level's time of day in every match (written by export); false: no pin." },
    } },
    handler(s: Session, a: { timeOfDay?: TimeOfDay; lock?: boolean }) {
      const p = s.requireProject();
      if (a.timeOfDay !== undefined && a.timeOfDay !== p.store.scene.databank.timeOfDay) commit(p.store, new SetLevel({ databank: { timeOfDay: a.timeOfDay } }, "Change time of day"));
      const l = a.lock === undefined ? levelLookOf(p.id) : edit(p.id, (x) => { x.lock = a.lock; });
      const tod = p.store.scene.databank.timeOfDay || "DAY";
      return { timeOfDay: tod, locked: !!l.lock,
        export: l.lock ? `export writes levels[].timeOfDay "${tod}": every match of the level loads at ${tod} (the pack then needs Melange 0.15+)` : "export writes no pin: Quick Game loads DAY, Versus and network games the frontend's pick",
        note: "the pin is this erg-mcp session's, per project: set it before export" };
    },
  },
  {
    name: "set_level_thumbnail",
    description: "The level's picture in the game's map menus (the level pickers show its base's otherwise: every Erg level built on Multi.DinerMight shows the diner). Give `from` (a PNG file: a game shot from game_camera or game_shots, or any picture, 16-4096 pixels a side), or `render` (render_view's arguments: the tools' own 3/4 view of the open level, drawn square at `size` pixels, default 512; `tints: true` previews landscape tints), or `clear: true`. `export` then writes it into the pack as thumb/<level>.png and names it in the level's entry (spice.json levels[].image, Melange 0.15+); Melange shows its middle square at 256 x 256 (the frontend's own picture format, converted when the pack registers). Kept for the open project in this erg-mcp session: call it before export (export's `thumbnail` argument takes a PNG file for one export). With no arguments it answers the current state. Answers the picture as image content with `show: true`.",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      from: { type: "string", description: "A PNG file." },
      render: { type: "object", description: "render_view's arguments for a view of the open level (view, pos, target, azimuth, elevation, fov, fog, timeOfDay, tints, scenery, ...); width and height are set from size." },
      size: { type: "integer", minimum: 128, maximum: 1024, description: "render: the picture's side in pixels (default 512)." },
      clear: { type: "boolean" },
      show: { type: "boolean", description: "Answer the picture as image content too (default false)." },
    } },
    async handler(s: Session, a: { from?: string; render?: Record<string, unknown>; size?: number; clear?: boolean; show?: boolean }) {
      const p = s.requireProject();
      if ([a.from, a.render, a.clear].filter((v) => v !== undefined && v !== false).length > 1) throw new ToolError("give one of from, render and clear");
      if (a.clear) edit(p.id, (x) => { delete x.thumbnail; });
      else if (a.from !== undefined) { const t = thumbnailFromFile(a.from); edit(p.id, (x) => { x.thumbnail = { png: t.png, from: t.from }; }); }
      else if (a.render !== undefined) {
        const size = a.size ?? 512;
        // a preset only when the render names no camera of its own (render_view refuses pos with a view): the level
        // camera when set_level_camera set one, else from the south-east
        const own = a.render.pos === undefined && a.render.view === undefined ? { view: levelCameraOf(p.id) ? "level" : "se" } : {};
        const r = await renderViewTool(s, { ...own, antialias: true, markers: false, ...a.render, width: size, height: size } as never);
        const img = r.content.find((c) => c.type === "image") as { data?: string } | undefined;
        if (!img?.data) throw new ToolError("render_view gave no picture");
        const png = new Uint8Array(Buffer.from(img.data, "base64"));
        checkPng(png, "the render");
        edit(p.id, (x) => { x.thumbnail = { png, from: "render_view" }; });
      }
      const t = levelLookOf(p.id).thumbnail;
      const out = t ? { thumbnail: { from: t.from, ...checkPng(t.png, "the thumbnail"), bytes: t.png.length },
        export: `export writes thumb/${p.id}.png and levels[].image (the pack then needs Melange 0.15+); Melange shows its middle square at 256 x 256` }
        : { thumbnail: null, export: "export writes no picture: the base's shows in the map menus" };
      return a.show && t ? { content: [{ type: "image" as const, data: Buffer.from(t.png).toString("base64"), mimeType: "image/png" }, { type: "text" as const, text: JSON.stringify(out) }] } : out;
    },
  },
];
