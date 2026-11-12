// Tool group "levellook": what a level's pack says about the level beyond the level file, both written by `export`
// into the level's spice.json entry (Melange 0.15): its time of day pinned for every match (levels[].timeOfDay: Versus
// picks one at random otherwise, so a map lit for DAY, with a sea material and a land light set for it, plays at night)
// and its picture in the map menus (levels[].image, a PNG in the pack; without one every Erg level shows its base's,
// the diner). The settings are kept per project for this erg-mcp session and `export` reads them (its own `timeOfDay`
// and `thumbnail` arguments say the same for one export).
import { existsSync, readFileSync, statSync } from "node:fs";
import { SetLevel, TIMES_OF_DAY, type TimeOfDay } from "../core";
import { commit } from "../lib/place-geom";
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

export const tools: Tool[] = [
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
        const r = await renderViewTool(s, { view: "se", antialias: true, markers: false, ...a.render, width: size, height: size } as never);
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
