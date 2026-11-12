// Tool group "landtint": recolour a level's landscape textures (lib/landtint.ts). Sunstone's landscape shaders draw every
// tile of a tinted texture toward the colour (Melange's mg_landTint), keeping the texture's detail; Sunstone off, the
// landscape draws as before. The level carries each tint as a tiny scenery copy under the sea bed whose name holds the
// tag; names of scenery copies are a visual channel, so sim_fingerprint is unchanged.
import { AddDetail, Group, RemoveDetail, targetFrame, type Scene, type Vec3 } from "../core";
import { commit } from "../lib/place-geom";
import { classifyDetail } from "../lib/sim-fingerprint";
import { tintHex } from "../lib/tint";
import { carrierName, landTintsOf, linearToSrgb, materialTiles, srgbToLinearHex, textureDigest, tileTwins, tintTheme, type TintTheme } from "../lib/landtint";
import { gradeMaterial, gradePreview, shadeDrift, type GradeHow } from "../lib/grade";
import { loadGrade } from "../lib/grade-file";
import { hex, type Rgb } from "../lib/inspect-png";
import { loadScene } from "./dress";
import { ToolError, type Tool } from "../registry";

export const tools: Tool[] = [
  {
    name: "tint_landscape",
    description: "Recolour landscape TEXTURES of the open level (every voxel or drape tile that draws with them, wherever it is): Sunstone's landscape shaders draw a tinted texture as the colour times the texel's luminance over the texture's mean, so its detail stays and its average becomes the colour (Melange's mg_landTint; Sunstone off: unchanged). For a theme's shared texture that reads wrong on one map, e.g. Prehistoric's pale grey side tile P05 (the steep faces of path02 and grass01) on a black volcano. Name each texture by `texture` (a Prehistoric tile P01-P64, an Arabian tile A01-A64, or a 16-hex texture digest) or by `material` (a material index or name of `theme`: PREHISTORIC or ARABIAN, default the open level's theme when it is one of them, else PREHISTORIC) and `faces` (side: the steep faces' tiles, the default; top; all). Some tiles are one image under two names (Arabian A03 is A11: grass02's top is the sand's), so a tint draws on all of them: the answer lists them as `sameImage`. color \"none\" removes a tint; `list: true` (alone) answers the level's tints and the theme's landscape materials' tiles. `colorSpace: \"srgb\"` reads each colour as the one to see (the tag takes its linear value; without it a hex draws lighter than it looks). render_view and render_topdown preview the tints with `tints: true`. Each tint is a scenery copy (no collision) at scale 0.05 under the level, named \"..._landtint_ltint<digest><rrggbb>\" (remove_scenery {search: \"_landtint\"} removes them all): a visual channel, sim_fingerprint unchanged. A texture used by castles and courts is tinted there too: check which materials use it. One undo step.",
    inputSchema: { type: "object", additionalProperties: false, properties: {
      tints: { type: "array", minItems: 1, maxItems: 32, items: { type: "object", additionalProperties: false, required: ["color"], properties: {
        texture: { type: "string", description: "A Prehistoric tile name (P05), an Arabian one (A11) or a 16-hex texture digest (Melange's texture-tag scheme)." },
        material: { description: "A material index or name of `theme` (rock02, path02): its tiles by `faces`." },
        faces: { type: "string", enum: ["side", "top", "all"] },
        color: { type: "string", description: "\"#rrggbb\" (the texture's mean becomes this), or \"none\" to remove." },
      } } },
      theme: { type: "string", enum: ["PREHISTORIC", "ARABIAN"], description: "Whose material records `material` names, and whose tile names the answer uses first (default the open level's theme when it is one of these, else PREHISTORIC)." },
      base: { type: "string", description: "Base to take the carrier piece from (default the open project's base)." },
      carrier: { type: "string", enum: ["base", "theme"], description: "Where the carrier piece comes from when base and id are not given: base (default, as before: the open project's base's first visual scenery piece) or theme (a piece of the level's OWN theme: the project's base when its theme is the level's, else the first game base of the level's theme with a scenery piece, so a level whose theme was changed with set_theme keeps no mesh of its base's theme). The answer's `carrier` says which piece was taken." },
      id: { type: "integer", description: "The carrier: a scenery detail of that base (default its first visual scenery detail)." },
      colorSpace: { type: "string", enum: ["linear", "srgb"], description: "How `color` is read. linear (default, as before): the hex is written to the tag as it is, and Sunstone multiplies it in linear light, so it draws lighter than the hex looks. srgb: the hex is the colour you want to SEE (a picker's value); the tag gets its linear-light value and the answer gives both (`asked`, `color`)." },
      list: { type: "boolean" },
      grade: { type: "string", description: "Preview each tint as the game plays it, in the sun and in shade: a grade file (game_material_shots {saveGrade, faces: [\"steep\", \"shade\"]}) or a built-in grade's name. Each tint then answers `plays`: sun (the colour by the grade's tinted fit, or as measured for that material), shade (by the grade's shade fit, or as measured; null when the grade was measured in the sun only) and `drift`, when the shade leaves the colour's family: redder (salmon, orange) or more saturated; and `from`, what each colour rests on: measured (the grade was measured on exactly this colour: `record`), interpolated (the fit bent toward the measured records within 48 of the colour, so a tint a few units off a measured one does not jump from the record to the fit: `record` the nearest, `pull` how much of its measured offset holds, `records` how many took part) or the fit alone (fit, tint fit, shade fit). With list: true it previews the level's tints as they are. Nothing else changes." },
    } },
    async handler(s, a: { tints?: { texture?: string; material?: number | string; faces?: "side" | "top" | "all"; color: string }[]; theme?: TintTheme; base?: string; id?: number; list?: boolean; colorSpace?: "linear" | "srgb"; carrier?: "base" | "theme"; grade?: string }) {
      const { store } = s.requireProject();
      const theme: TintTheme = a.theme ?? tintTheme(store.scene.databank.theme) ?? "PREHISTORIC";
      // grade: a tint's colour as the game plays it in the sun and in shade (`tag` is the tag's linear hex; material -1
      // when the tint names a texture, so only the fits apply)
      const gr = a.grade !== undefined ? loadGrade(a.grade).grade : null;
      const plays = (tag: string, material = -1) => {
        if (!gr) return {};
        // the fit bent toward the measured records by their distance in colour (gradePreview): a tint a few units off a
        // measured one no longer jumps from the record to the fit, and `from` says what each colour rests on
        const seen = linearToSrgb(tag) as Rgb, ps = gradePreview(gr, material, seen, "sun", true), ph = gradePreview(gr, material, seen, "shade", true);
        const sun = ps.rgb ?? gradeMaterial(gr, material, seen, true), shade = ph.rgb, drift = shade ? shadeDrift(sun, shade) : null;
        const how = (h: GradeHow | null) => (h ? { by: h.by, ...(h.by === "interpolated" ? { fit: h.fit, pull: h.pull, records: h.records } : {}),
          ...(h.record ? { record: { material: h.record.material, drawn: hex(h.record.from), played: hex(h.record.to), distance: h.record.distance, ...(h.record.kind ? { kind: h.record.kind } : {}) } } : {}) } : null);
        return { plays: { asSeen: hex(seen), sun: hex(sun), shade: shade ? hex(shade) : null, ...(drift ? { drift } : {}),
          from: { sun: how(ps.how), shade: how(ph.how) },
          ...(shade ? {} : { note: "this grade was measured in the sun only: game_material_shots {faces: [\"steep\", \"shade\"], saveGrade} measures shade" }) } };
      };
      const carriers = () => store.scene.details.filter((d) => d.src === null && landTintsOf(d.name).length);
      const current = () => carriers().flatMap((d) => landTintsOf(d.name, theme).map((t) => ({ ...t, id: d.id })));
      if (a.list || !a.tints) {
        if (!a.list) throw new ToolError("give tints, or list: true");
        const listed = theme === "ARABIAN" ? [0, 2, 3, 4, 6, 8, 9, 10, 11, 12, 15, 24, 57] : [0, 2, 4, 6, 8, 9, 10, 12, 16, 19, 27, 49, 50, 51];
        return { tints: current().map((t) => ({ ...t, ...plays(t.color) })), theme, materials: listed.map((m) => ({ ...materialTiles(m, "all", theme), side: materialTiles(m, "side", theme).tiles })) };
      }
      const want = new Map<string, { color: string | null; asked?: string; texture: string | null; from: string; material?: number }>();
      a.tints.forEach((t, i) => {
        let color: string | null = null, asked: string | undefined;
        try { if (t.color !== "none") color = `#${tintHex(t.color, `tints[${i}].color`)}`; } catch (e) { throw new ToolError((e as Error).message); }
        if (color && a.colorSpace === "srgb") { asked = color; color = srgbToLinearHex(color); }
        if ((t.texture === undefined) === (t.material === undefined)) throw new ToolError(`tints[${i}]: give texture or material`);
        try {
          if (t.texture !== undefined) { const d = textureDigest(t.texture, theme); want.set(d.digest, { color, asked, texture: d.texture, from: t.texture }); }
          else {
            const m = materialTiles(t.material!, t.faces ?? "side", theme);
            for (const tile of m.tiles) { const d = textureDigest(tile, theme); want.set(d.digest, { color, asked, texture: d.texture, from: `${m.name} ${t.faces ?? "side"}`, material: m.index }); }
          }
        } catch (e) { throw new ToolError(`tints[${i}].${(e as Error).message}`); }
      });
      // The carrier: the base's first visual scenery piece (the default, as before: Caldera's committed pack carries
      // DinerMight's sign under its Prehistoric volcano and must build byte for byte), or with carrier: "theme" a piece
      // of the level's OWN theme, so a level whose theme was changed (set_theme) keeps no mesh of its base's theme: the
      // project's base when its theme is the level's, else the first game base of that theme with a scenery piece.
      const isScenery = (d: Scene["details"][number]) => { const c = classifyDetail(d); return c.cls === "visual" && c.role === "scenery"; };
      let key = a.base ?? store.scene.base.key, carrierWhy = a.base ? "base given" : "the project's base";
      let scene = await loadScene(s, key);
      const levelTheme = String(store.scene.databank.theme ?? "").toUpperCase();
      if (a.carrier === "theme" && (a.base !== undefined || a.id !== undefined)) throw new ToolError("carrier: \"theme\" picks the piece itself: give it without base and id");
      if (a.carrier === "theme" && levelTheme) {
        const lv = await s.levels();
        const bases = typeof (lv as { list?: unknown }).list === "function" ? (await lv.list()).bases.filter((b) => b.source === "game") : [];
        const own = bases.find((b) => b.key === key);
        if (own && own.theme.toUpperCase() !== levelTheme) {
          for (const b of bases.filter((x) => x.theme.toUpperCase() === levelTheme)) {
            let sc: Scene;
            try { sc = await loadScene(s, b.key); } catch { continue; }
            if (!sc.details.some(isScenery)) continue;
            key = b.key; scene = sc; carrierWhy = `the level's theme is ${levelTheme} and its base ${own.key} is ${own.theme.toUpperCase()}: the first ${levelTheme} base with a scenery piece`;
            break;
          }
        }
      }
      const src = a.id !== undefined ? scene.details.find((d) => d.id === a.id) : scene.details.find(isScenery);
      if (!src) throw new ToolError(a.id !== undefined ? `no detail ${a.id} in ${key}` : `${key} has no visual scenery detail to carry the tag: pass base and id`);
      const cl = classifyDetail(src);
      if (cl.cls !== "visual" || cl.role !== "scenery") throw new ToolError(`detail ${src.id} (${src.name}) is not visual scenery (${cl.why ?? cl.role})`);
      // under the level's lowest voxels, at its centre: the carrier never shows
      let low = 0;
      for (const d of store.scene.details) low = Math.min(low, store.frames.detailWorld(d)[1]);
      const at: Vec3 = [0, low - 20, 0];
      const frame = targetFrame(store.scene, "scenery");
      const cmds: (AddDetail | RemoveDetail)[] = [], out: object[] = [];
      for (const [digest, w] of want) {
        for (const d of carriers()) if (landTintsOf(d.name).some((t) => t.digest === digest)) cmds.push(new RemoveDetail(d.id));
        if (w.color) {
          const name = carrierName(src.name, digest, w.color);
          if (name.length > 120) throw new ToolError(`the carrier's name would be ${name.length} characters: pass a carrier id with a shorter name`);
          cmds.push(new AddDetail(frame, { name, resource: src.resource, pos: store.frames.toLocal(frame, at), rot: [0, 0, 0], scale: [0.05, 0.05, 0.05] }));
        }
        const twins = tileTwins(digest, theme);
        out.push({ digest, texture: w.texture, from: w.from, color: w.color ?? "none", ...(w.asked ? { asked: w.asked, colorSpace: "srgb" } : {}), ...(twins.length > 1 ? { sameImage: twins } : {}), ...(w.color ? plays(w.color, w.material) : {}) });
      }
      if (cmds.length) commit(store, cmds.length === 1 ? cmds[0] : new Group(`Tint ${want.size} landscape textures`, cmds));
      return { tints: out, level: current(), carrier: { base: key, id: src.id, name: src.name, resource: src.resource, why: carrierWhy }, note: "drawn by Sunstone's landscape shaders (Melange MirageTints, mg_landTint); sim_fingerprint is unchanged", undo: "undo restores the previous tints" };
    },
  },
];
