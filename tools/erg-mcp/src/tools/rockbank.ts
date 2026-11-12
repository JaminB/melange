// Tool group "rockbank": build_rock_bank, a map's procedural rock meshes (faceted hull boulders, strata slabs, scree heaps,
// seeded, in a named or custom palette) written as ONE mod mesh bank a pack lists under spice.json `meshes`. The generator
// is src/lib/rockmesh.ts (Caldera's bench/caldera/meshes/gen.mjs generalised), the bank build src/lib/rockbank.ts (build.mjs
// and merge.mjs). Visual only: place_mod_mesh then puts copies of `<id>.<Name>` in the level.
import { buildBank } from "../lib/rockbank";
import { PALETTES, ROCK_KINDS, buildRocks, type Palette, type RockSpec } from "../lib/rockmesh";
import { registerModMesh } from "../lib/scenery-mesh";
import { loadGrade } from "../lib/grade-file";
import { rockSheet } from "../lib/rock-preview";
import { ToolError, image, type Schema, type Tool } from "../registry";

const rgb: Schema = { type: "array", items: { type: "integer", minimum: 0, maximum: 255 }, minItems: 3, maxItems: 3 };
const paletteSchema: Schema = {
  description: `A palette name (${Object.keys(PALETTES).join(", ")}) or {base: [r, g, b], dust: [r, g, b], dustShare?, textureSeed?}: the stone and the dust over its top faces (sRGB bytes), the share of dust (0-1, default 1; 0.3 reads as bare rock) and the texture's noise seeds. The albedo is baked into the texture.`,
  anyOf: [{ type: "string", enum: Object.keys(PALETTES) },
    { type: "object", additionalProperties: false, required: ["base", "dust"], properties: { base: rgb, dust: rgb, dustShare: { type: "number", minimum: 0, maximum: 1 }, textureSeed: { type: "array", items: { type: "integer" }, minItems: 2, maxItems: 2 } } }],
};
const NAME = /^[A-Za-z0-9_.-]{1,96}$/, ID = /^[a-z0-9][a-z0-9_-]{0,63}$/;

interface Args {
  id: string; rocks?: RockSpec[]; palette?: string | Palette; seed?: number; out?: string; bank?: string; section?: number;
  gameDir?: string; xomtool?: string; keepSource?: boolean; dryRun?: boolean; register?: boolean;
  preview?: boolean | { grade?: string; ground?: string; cell?: number };
}

export const tools: Tool[] = [
  {
    name: "build_rock_bank",
    description: `A map's own rock meshes as one mod mesh bank. \`rocks\` (default Boulder, Slab, Wedge, Shard) are named, seeded procedural models: boulder (a chamfered block), slab (flat, tilted top), wedge (the top falls away to one side), shard (squat and pointed) are faceted convex hulls of 60-150 vertices, flat shaded; strata is stacked irregular slabs, each stepped back from the one below (it lies along a steep face: place_aligned or place_mod_mesh with \`size\`); scree is a heap of small hulls in one mesh (a boulder field or a talus fan in one copy). Each takes \`seed\`, \`scale\` (0.1-4) and its own \`palette\`; the map's \`palette\` (basalt, sandstone, redrock, limestone or custom) and \`seed\` (added to each kind's seed; 0 is Caldera's own, so a basalt set at seed 0 is Caldera's rocks) are the defaults. Meshes are named \`<id>.<Name>\`. With \`out\` (a folder) it writes \`<id>.<bank>.xom\` (default \`<id>.Rocks.xom\`): the meshes through xomtool with the material copied from the game's HORROR5 rock (needs \`gameDir\` or MELANGE_GAME_DIR for Data/Bundles/Bundl31.xom, and xomtool: \`xomtool\`, XOMTOOL, or dist/tools/xomtool.exe), made matte, merged into ONE bank (the 44 mod mesh sections are shared by every enabled mod, so a map ships one bank). Without \`out\`, or with \`dryRun\`, nothing is written and the answer is the plan. The answer gives each mesh's vertices, triangles, seed and \`box\` (map units: place_mod_mesh's \`box\`), and the \`spice\` entry to list the bank under \`meshes\`. The rocks are also registered for this session, so render_view {scenery: true} draws placed copies of \`<id>.<Name>\` flat-shaded in the palette's colour (the server has no preview of a mod mesh); a copy draws nothing in the game until the bank is loaded by a content mod (docs/spice.md, \`melange.range\` required). The albedo is baked: colours drew darker in the game than the palette's bytes (Caldera needed 1.8x), so judge by a Test, not the render. Limits: 256 meshes in a bank, 65535 vertices a mesh, 128 x 128 textures; no collision.`,
    inputSchema: { type: "object", additionalProperties: false, required: ["id"], properties: {
      id: { type: "string", description: "The mod id every mesh name starts with (lowercase letters, digits, `_-`): the content mod's id." },
      rocks: { type: "array", minItems: 1, maxItems: 64, items: { type: "object", additionalProperties: false, required: ["name", "kind"], properties: {
        name: { type: "string", description: "The mesh's name after the id (letters, digits, `_.-`)." },
        kind: { type: "string", enum: [...ROCK_KINDS] },
        seed: { type: "integer", description: "Overrides the derived seed (the kind's own + the map seed + 101 per earlier rock of the kind)." },
        scale: { type: "number", minimum: 0.1, maximum: 4 },
        palette: paletteSchema,
        strata: { type: "object", additionalProperties: false, description: "kind strata: layers (1-8; default Caldera's three), length and depth (half sizes, xom units: 20 = 1 map unit; defaults 40 and 16), thickness (one layer's, default 9).",
          properties: { layers: { type: "integer", minimum: 1, maximum: 8 }, length: { type: "number", minimum: 6, maximum: 200 }, depth: { type: "number", minimum: 4, maximum: 100 }, thickness: { type: "number", minimum: 3, maximum: 40 } } },
        scree: { type: "object", additionalProperties: false, description: "kind scree: rocks (3-24, default 9) and radius (xom units, default 70).",
          properties: { rocks: { type: "integer", minimum: 3, maximum: 24 }, radius: { type: "number", minimum: 20, maximum: 300 } } },
      } } },
      palette: paletteSchema,
      seed: { type: "integer", description: "Added to each kind's default seed (default 0)." },
      out: { type: "string", description: "The folder the bank is written to (created). Omit it to plan only." },
      bank: { type: "string", description: "The bank's name after the id (default Rocks)." },
      section: { type: "integer", minimum: 476, maximum: 519, description: "The mod mesh section hint of the first mesh (default 486); Melange relocates a taken one." },
      gameDir: { type: "string", description: "A game folder holding Data/Bundles/Bundl31.xom (default MELANGE_GAME_DIR)." },
      xomtool: { type: "string" },
      keepSource: { type: "boolean", description: "Keep the glTF and PNG sources in <out>/src (default false)." },
      dryRun: { type: "boolean", description: "Plan and register only: write nothing even with `out`." },
      register: { type: "boolean", description: "Register the rocks for render_view {scenery: true} (default true)." },
      preview: { description: "A picture of the palette before (or with) the build: true, or {grade, ground, cell}. Every rock is drawn flat shaded on the ground's colour, its sides in the palette's stone and its upward faces under its dust; `grade` (a grade file from game_material_shots {saveGrade}, or a built-in's name) draws the colours as the game plays them, `ground` (\"#rrggbb\", the colour the ground plays in: a material's inGame from game_material_shots) is the background, and each rock then answers `contrast`: how far its lit top and its shaded side stand from the ground in brightness (0-255; a lit top under about 12 from it reads by its shadow only). The picture is image content; with no `out` nothing is written.",
        anyOf: [{ type: "boolean" }, { type: "object", additionalProperties: false, properties: { grade: { type: "string" }, ground: { type: "string" }, cell: { type: "integer", minimum: 64, maximum: 320 } } }] },
    } },
    handler(_s, a: Args) {
      if (!ID.test(a.id)) throw new ToolError(`id "${a.id}": lowercase letters, digits, "_" and "-", starting with a letter or digit`);
      const rocks = a.rocks ?? [{ name: "Boulder", kind: "boulder" }, { name: "Slab", kind: "slab" }, { name: "Wedge", kind: "wedge" }, { name: "Shard", kind: "shard" }] as RockSpec[];
      for (const r of rocks) {
        if (!NAME.test(r.name)) throw new ToolError(`rock name "${r.name}": letters, digits and "_.-"`);
        if (r.strata && r.kind !== "strata") throw new ToolError(`${r.name}: strata options on a ${r.kind}`);
        if (r.scree && r.kind !== "scree") throw new ToolError(`${r.name}: scree options on a ${r.kind}`);
      }
      let built;
      try { built = buildRocks(rocks, { palette: a.palette, seed: a.seed }); } catch (e) { throw new ToolError((e as Error).message); }
      const mesh = (r: (typeof built.rocks)[number]) => ({ name: `${a.id}.${r.name}`, kind: r.kind, seed: r.seed, vertices: r.mesh.vertices, triangles: r.mesh.triangles, box: r.box });
      if (a.register !== false) for (const r of built.rocks) registerModMesh(`${a.id}.${r.name}`, r.tris, r.albedo);
      const registered = a.register !== false ? "render_view {scenery: true} draws these" : "not registered";
      // preview: the palette as the game plays it, on the ground's colour
      let sheet: ReturnType<typeof rockSheet> | null = null, pv: Record<string, unknown> | undefined;
      if (a.preview) {
        const po = typeof a.preview === "object" ? a.preview : {};
        const gr = po.grade !== undefined ? loadGrade(po.grade) : null;
        if (po.ground !== undefined && !/^#[0-9a-fA-F]{6}$/.test(po.ground)) throw new ToolError("preview.ground is \"#rrggbb\"");
        const ground = po.ground ? [1, 3, 5].map((i) => parseInt(po.ground!.slice(i, i + 2), 16)) as [number, number, number] : null;
        sheet = rockSheet(built.rocks, { grade: gr?.grade ?? null, ground, cell: po.cell });
        const faint = sheet.swatches.filter((w) => w.contrast && w.contrast.sun < 12).map((w) => w.name);
        pv = { rocks: sheet.swatches, size: [sheet.width, sheet.height], ...(gr ? { grade: gr.name } : { grade: null, note: "no grade: the palette's own colours (a grade file from game_material_shots {saveGrade} draws them as the game plays them)" }),
          ...(ground ? { ground: po.ground } : {}), ...(faint.length ? { warning: `${faint.join(", ")} ${faint.length === 1 ? "has" : "have"} lit faces less than 12 of 255 from the ground in brightness: the rock reads by its shadow only (a darker or a paler stone)` } : {}) };
      }
      const withSheet = (o: Record<string, unknown>) => (sheet ? { content: [image(sheet.png), { type: "text" as const, text: JSON.stringify({ ...o, preview: pv }) }] } : o);
      if (!a.out || a.dryRun) {
        return withSheet({ written: false, meshes: built.rocks.map(mesh), textures: built.textures.size, registered,
          spice: { meshes: [{ file: `assets/meshes/${a.id}.${a.bank ?? "Rocks"}.xom` }] },
          next: "give `out` to write the bank, then place_mod_mesh with resource and box from meshes" });
      }
      let r;
      try { r = buildBank(a.out, { id: a.id, rocks, palette: a.palette, seed: a.seed, bank: a.bank, section: a.section, gameDir: a.gameDir, xomtool: a.xomtool, keepSource: a.keepSource }, built); }
      catch (e) { throw new ToolError(`building the bank failed: ${(e as Error).message}`); }
      return withSheet({ written: true, file: r.file, bytes: r.bytes, section: r.section, meshes: r.meshes, textures: built.textures.size, registered, spice: r.spice,
        next: "copy the bank to the pack's assets/meshes/ and list it under spice.json `meshes` (docs/spice.md), then place_mod_mesh with resource and box from meshes" });
    },
  },
];
