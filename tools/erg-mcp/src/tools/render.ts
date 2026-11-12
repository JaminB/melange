// Tool group "render": render_view, a perspective 3/4 view of the open level (src/lib/render-3d.ts), for judging
// silhouettes (a volcano's cone, a castle's towers) before a Test. Read-only, like the inspect group.
import type { Vec3 } from "../core";
import { extentXZ, markers, waterMap, type Region } from "../lib/inspect-field";
import { encodePng } from "../lib/inspect-png";
import { landTintPreview, projectMaterials, tintFor, type PaletteMode } from "../lib/materials";
import { applyGrade, gradeMaterial } from "../lib/grade";
import { GRADE_NAMES, loadGrade } from "../lib/grade-file";
import { MARKER_RGB, boxCorners, cameraAngles, fitDistance, gridCorner, lookFor, orbitCamera, renderView, type Camera, type View } from "../lib/render-3d";
import { WORLD_PER_MAP, grids, isSolid, type Grid } from "../lib/voxel";
import { ToolError, image, type Tool } from "../registry";
import type { Session } from "../session";
import { sceneryMeshesFor } from "../lib/scenery-view";
import { HAZE_MODEL, hazeAt, hazeVerdict, levelLooks, seaOpacity, type LevelLooks } from "../lib/sea-look";
import { levelCameraOf } from "../lib/level-camera";

type Units = "map" | "world" | undefined;
type Preset = "nw" | "ne" | "sw" | "se" | "orbit" | "game" | "level";

interface ViewArgs {
  view?: Preset; pos?: number[]; target?: number[]; at?: number[]; units?: "map" | "world"; fov?: number;
  azimuth?: number; elevation?: number; distance?: number;
  region?: { x0?: number; z0?: number; x1?: number; z1?: number }; clip?: boolean;
  width?: number; height?: number; fog?: number; timeOfDay?: "DAY" | "EVENING" | "NIGHT"; markers?: boolean; palette?: PaletteMode;
  antialias?: boolean; edges?: boolean; scenery?: boolean; tints?: boolean; grade?: string; sideTextures?: boolean;
  waterLook?: boolean; gameMatch?: boolean | { hidden?: number; seaBy?: "mod" | "game" };
}

const BEARING: Record<"nw" | "ne" | "sw" | "se", number> = { ne: 45, se: 135, sw: 225, nw: 315 };
const GAME_DISTANCE = 170 / WORLD_PER_MAP;     // the turn camera's distance behind the worm, about 170 world units
const r2 = (v: number) => Math.round(v * 100) / 100;
const inMap = (v: number, u: Units) => (u === "world" ? v / WORLD_PER_MAP : v);
const vecIn = (p: number[], u: Units, what: string): Vec3 => {
  if (p.length !== 3 || !p.every(Number.isFinite)) throw new ToolError(`${what}: [x, y, z]`);
  return [inMap(p[0], u), inMap(p[1], u), inMap(p[2], u)];
};

/** Indented JSON with each array or object of plain values on one line (as the inspect group answers). */
const compact = (v: unknown) => JSON.stringify(v, null, 1).replace(/[[{][^[\]{}]*[\]}]/g, (m) => m.replace(/\s*\n\s*/g, " "));

/** The points to frame: the corners of each frame's solid voxels (through its own matrix, so a rotated frame's are its
 * own), kept to the region's xz; the region's corners at y 0 when nothing solid is in it. */
function framePoints(gs: Grid[], r: Region): Vec3[] {
  const out: Vec3[] = [], c = new Float64Array(3);
  for (const g of gs) {
    const b = g.bounds;
    if (b.max[0] <= r.x0 || b.min[0] >= r.x1 || b.max[2] <= r.z0 || b.min[2] >= r.z1) continue;
    const [X, Y, Z] = g.size, lo = [X, Y, Z], hi = [-1, -1, -1];
    for (let z = 0; z < Z; z++)
      for (let x = 0; x < X; x++)
        for (let y = 0; y < Y; y++) {
          if (!isSolid(g.words[(z * X + x) * Y + y])) continue;
          if (x < lo[0]) lo[0] = x; if (y < lo[1]) lo[1] = y; if (z < lo[2]) lo[2] = z;
          if (x > hi[0]) hi[0] = x; if (y > hi[1]) hi[1] = y; if (z > hi[2]) hi[2] = z;
        }
    if (hi[0] < 0) continue;
    for (const k of [0, 1, 2, 3, 4, 5, 6, 7]) {
      gridCorner(g, k & 1 ? hi[0] + 1 : lo[0], k & 2 ? hi[1] + 1 : lo[1], k & 4 ? hi[2] + 1 : lo[2], c, 0);
      out.push([Math.min(r.x1, Math.max(r.x0, c[0])), c[1], Math.min(r.z1, Math.max(r.z0, c[2]))]);
    }
  }
  return out.length ? out : boxCorners([r.x0, 0, r.z0], [r.x1, 0, r.z1]);
}

/** The middle of the points' box. */
function middle(ps: Vec3[]): Vec3 {
  const lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
  for (const p of ps) for (let k = 0; k < 3; k++) { lo[k] = Math.min(lo[k], p[k]); hi[k] = Math.max(hi[k], p[k]); }
  return [(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2];
}

/** The cameras for the call (four for orbit), each with how it was chosen. */
function cameras(a: ViewArgs, gs: Grid[], points: Vec3[], aspect: number, level?: Camera | null): { cam: Camera; how: string }[] {
  const u = a.units, fov = a.fov ?? 60;
  const centre = middle(points);
  if (a.view === "level") {
    if (a.pos || a.target || a.at) throw new ToolError("view level is the pose set_level_camera set: no pos, target or at");
    if (!level) throw new ToolError("view level: the open project has no level camera (set_level_camera)");
    return [{ cam: { ...level, fov: a.fov ?? level.fov }, how: "level: set_level_camera's pose" }];
  }
  if (a.pos) {
    if (a.view) throw new ToolError("give pos and target, or a view preset, not both");
    if (!a.target) throw new ToolError("pos needs a target");
    return [{ cam: { pos: vecIn(a.pos, u, "pos"), target: vecIn(a.target, u, "target"), fov }, how: "pos and target" }];
  }
  if (a.view === "game") {
    if (!a.at) throw new ToolError("view game needs at: the point the turn camera looks at");
    const at = vecIn(a.at, u, "at");
    const ext = extentXZ(gs);
    // behind the point as seen from the level's middle: the camera stands outside, looking in
    const ox = at[0] - (ext ? (ext.x0 + ext.x1) / 2 : at[0]), oz = at[2] - (ext ? (ext.z0 + ext.z1) / 2 : at[2]);
    const az = a.azimuth ?? (Math.hypot(ox, oz) < 1e-6 ? 180 : ((Math.atan2(ox, -oz) * 180) / Math.PI + 360) % 360);
    return [{ cam: orbitCamera(at, az, a.elevation ?? 22, a.distance === undefined ? GAME_DISTANCE : inMap(a.distance, u), fov), how: "game: the turn camera behind and above at" }];
  }
  if (a.at) throw new ToolError("at is for view game; use target to centre another view");
  const target = a.target ? vecIn(a.target, u, "target") : centre;
  const el = a.elevation ?? 35;
  const one = (az: number, how: string) => ({
    cam: orbitCamera(target, az, el, a.distance === undefined ? fitDistance(points, target, az, el, fov, aspect) : inMap(a.distance, u), fov), how,
  });
  const view = a.view ?? "se";
  if (view === "orbit") {
    const start = a.azimuth ?? 315;
    return [0, 90, 180, 270].map((d) => one((start + d) % 360, `orbit, azimuth ${(start + d) % 360}`));
  }
  return [one(a.azimuth ?? BEARING[view], `${view}${a.distance === undefined ? ", distance fitted to the " + (a.region ? "region" : "level") : ""}`)];
}

export async function renderViewTool(s: Session, a: ViewArgs) {
  const t0 = performance.now();
  const store = s.store, gs = grids(store);
  const ext = extentXZ(gs);
  if (!ext) throw new ToolError("the level has no voxel frames");
  const u = a.units;
  const rg = a.region ?? {};
  const r: Region = {
    x0: rg.x0 === undefined ? ext.x0 : inMap(rg.x0, u), z0: rg.z0 === undefined ? ext.z0 : inMap(rg.z0, u),
    x1: rg.x1 === undefined ? ext.x1 : inMap(rg.x1, u), z1: rg.z1 === undefined ? ext.z1 : inMap(rg.z1, u),
  };
  if (r.x0 > r.x1) [r.x0, r.x1] = [r.x1, r.x0];
  if (r.z0 > r.z1) [r.z0, r.z1] = [r.z1, r.z0];
  if (r.x1 - r.x0 < 1e-6 || r.z1 - r.z0 < 1e-6) throw new ToolError("the region is empty");
  const W = a.width ?? 960, H = a.height ?? 540;
  const orbit = a.view === "orbit";
  const tw = orbit ? Math.floor(W / 2) : W, th = orbit ? Math.floor(H / 2) : H;
  const points = framePoints(gs, r);
  const lc = s.project ? levelCameraOf(s.project.id) : null;
  const cams = cameras(a, gs, points, tw / th, lc ? { pos: lc.pos.map((v) => v / WORLD_PER_MAP) as Vec3, target: lc.target.map((v) => v / WORLD_PER_MAP) as Vec3, fov: lc.fov } : null);

  const set = await projectMaterials(s);
  const lt = a.tints ? landTintPreview(store, set, tintFor(set, a.palette)) : null;
  const tint0 = lt?.tint ?? tintFor(set, a.palette);
  const real = a.palette !== "index" && !!set?.colors;
  // grade: the colours as the game shows them (a grade fitted from game_material_shots' pairs); sideTextures: the faces
  // that are not tops in the colour of the material's side texture
  const grade = a.grade === undefined ? null : loadGrade(a.grade);
  // a material the grade was measured on, still drawn in that colour, takes the game's colour as measured
  const tintedSet = new Set((lt?.materials ?? []).map((m) => m.material));
  const tint = grade ? (m: number) => gradeMaterial(grade.grade, m, tint0(m), tintedSet.has(m)) : tint0;
  const sides = a.sideTextures && real && set?.side ? set.side : null;
  const tintSide = sides ? (m: number) => { const c = sides[m] ?? tint0(m); return grade ? applyGrade(grade.grade, c) : c; } : undefined;
  const tod = a.timeOfDay ?? (store.scene.databank.timeOfDay || "DAY");
  const water = waterMap(store);
  // waterLook (and gameMatch): the level's own sea material, opacity and haze, read from its script's set_water_look
  // block; without them the picture is what it always was (a translucent blue plane, no haze)
  const gm = a.gameMatch ? (typeof a.gameMatch === "object" ? a.gameMatch : {}) : null;
  let looks: LevelLooks | null = null;
  if (a.waterLook || gm) {
    let script = "";
    try { script = (await s.requireProject().erg.script()) ?? ""; } catch { /* a server without scripts: no looks */ }
    looks = levelLooks(script);
  }
  const seaBy = gm?.seaBy ?? "mod";
  const seaOp = looks ? seaOpacity(looks, seaBy) : null;
  const seaIn = looks && (looks.sea || seaOp !== null) ? { ...(looks.sea && seaBy === "mod" ? { rgb: (grade ? applyGrade(grade.grade, looks.sea.albedo as [number, number, number]) : looks.sea.albedo) as [number, number, number] } : {}), opacity: seaOp ?? 0.75 } : undefined;
  const hz = looks?.haze ?? null;
  const hazeIn = hz ? { rgb: hz.color as [number, number, number], amount: hz.amount, at: (d: number) => hazeAt(d * WORLD_PER_MAP, hz) } : undefined;
  const input = {
    ...(seaIn ? { sea: seaIn } : {}), ...(hazeIn ? { haze: hazeIn } : {}),
    grids: gs, tint, ...(tintSide ? { tintSide } : {}), ...(grade ? { normaliseTop: true } : {}), look: lookFor(tod), water, markers: a.markers === false ? [] : markers(store),
    fog: a.fog === undefined ? 0 : inMap(a.fog, u), clip: a.clip ? r : null, edges: a.edges,
    ...(a.scenery ? await sceneryMeshesFor(s, a.clip ? r : null) : {}),
  };
  const views: View[] = cams.map(({ cam }) => renderView(input, cam, tw, th, a.antialias ? 2 : 1));

  // one picture, or the four orbit tiles in a 2 x 2 sheet with a dark seam
  let rgb: Uint8Array, w = tw, h = th;
  if (!orbit) rgb = views[0].rgb;
  else {
    w = tw * 2; h = th * 2;
    rgb = new Uint8Array(w * h * 3);
    views.forEach((v, k) => {
      const ox = (k % 2) * tw, oy = Math.floor(k / 2) * th;
      for (let y = 0; y < th; y++) rgb.set(v.rgb.subarray(y * tw * 3, (y + 1) * tw * 3), ((oy + y) * w + ox) * 3);
    });
    for (let y = 0; y < h; y++) for (let x = tw - 1; x <= tw; x++) rgb.fill(16, (y * w + x) * 3, (y * w + x) * 3 + 3);
    for (let x = 0; x < w; x++) for (let y = th - 1; y <= th; y++) rgb.fill(16, (y * w + x) * 3, (y * w + x) * 3 + 3);
  }
  const png = encodePng(w, h, rgb);
  const ms = Math.round(performance.now() - t0);

  const both = (p: Vec3) => ({ map: p.map(r2), world: p.map((v) => r2(v * WORLD_PER_MAP)) });
  const info = {
    image: { width: w, height: h, ...(orbit ? { tiles: "2 x 2, left to right then top to bottom, in the cameras' order", tile: [tw, th] } : {}), antialias: !!a.antialias },
    cameras: cams.map(({ cam, how }, k) => {
      const ang = cameraAngles(cam);
      return {
        how, pos: both(cam.pos), target: both(cam.target), fov: cam.fov,
        azimuth: r2(ang.azimuth), elevation: r2(ang.elevation), distance: { map: r2(ang.distance), world: r2(ang.distance * WORLD_PER_MAP) },
        faces: views[k].faces, framesDrawn: views[k].framesDrawn,
      };
    }),
    reading: "fov is vertical, in degrees; azimuth is the bearing from the target to the eye, clockwise from north (-z): 0 n, 90 e, 180 s, 270 w; elevation is degrees above the target. Faces are lit by the time of day's sun (or moon) and an ambient term, so tops and each side of a voxel step differ; silhouette edges are darkened.",
    timeMs: ms,
    framesDrawn: `${Math.max(...views.map((v) => v.framesDrawn))} of ${gs.length} voxel frames`,
    timeOfDay: tod,
    palette: real ? "real: each material's texture colour (the server's average of its surface texture)" : "index: a distinct hue per material index" + (a.palette === "index" ? "" : " (the server gave no material colours)"),
    ...(grade ? { grade: { name: grade.name, pairs: grade.grade.pairs, residual: grade.grade.residual, ...(grade.grade.source ? { source: grade.grade.source } : {}),
      ...(grade.grade.measured ? { measuredMaterials: grade.grade.measured.length } : {}), ...(grade.grade.tint ? { tintFit: { pairs: grade.grade.tint.pairs, residual: grade.grade.tint.residual } } : {}),
      note: "a material the grade was measured on, still drawn in the colour it had then, is drawn in the colour the game showed; the others by the grade's fit (a saturation, a gain and an offset per channel in linear light; a tinted material by the fit over the tinted ones); the light is scaled so a level top in the sun is exactly that colour" } } : {}),
    ...(a.sideTextures ? { sideTextures: sides ? "every face but a top is drawn in the colour of the material's side texture (a cliff of grass01 is rock)" : "the server gave no side colours (Melange 0.15+): the sides are drawn in the tops' colours" } : {}),
    ...(looks ? { waterLook: {
      sea: looks.sea ? { albedo: looks.sea.albedo, opacity: looks.sea.opacity, drawn: seaBy === "mod" ? "a matte plane of this colour (what a water shader mod that honours the sea material draws: Sunstone 2.4+)" : "not drawn: seaBy game is the game's own water shader, which takes no colour" } : null,
      opacity: seaOp === null ? "the theme's own (drawn at 0.75)" : seaOp,
      haze: hz ? { ...hz, model: HAZE_MODEL.source, drawn: "the land and the sea fade to it with distance, and the band under the horizon is its colour (the game alone draws no haze)" } : null,
      ...(!looks.sea && !looks.look && !hz ? { note: "the level's script has no set_water_look block: the picture is the plain one" } : {}) } } : {}),
    ...(gm ? { gameMatch: (() => {
      // the haze from each camera of this picture, and from the cameras a match and the harness show the level from
      const land = tint0(0), dist = (c: Camera) => Math.hypot(c.pos[0] - c.target[0], c.pos[1] - c.target[1], c.pos[2] - c.target[2]) * WORLD_PER_MAP;
      const ext2 = extentXZ(gs)!, span = Math.max(ext2.x1 - ext2.x0, ext2.z1 - ext2.z0) * WORLD_PER_MAP;
      const from: { what: string; distance: number }[] = [
        ...cams.map(({ cam, how }) => ({ what: `this picture's camera (${how})`, distance: dist(cam) })),
        lc ? { what: "the level camera (set_level_camera)", distance: Math.hypot(lc.pos[0] - lc.target[0], lc.pos[1] - lc.target[1], lc.pos[2] - lc.target[2]) }
          : { what: "the default overview (game_camera preset overview: 0.95 x the level's span)", distance: span * 0.95 },
        { what: "the turn camera", distance: GAME_DISTANCE * WORLD_PER_MAP },
      ];
      const rows = hz ? from.map((f) => ({ from: f.what, ...hazeVerdict(f.distance, hz, { land, hidden: gm.hidden, what: f.what }) })) : [];
      const warnings = [...rows.filter((r) => r.warning).map((r) => r.warning as string),
        ...(seaOp !== null && seaOp < 0.995 ? [`the sea is drawn at opacity ${seaOp}: the land under it shows through near the shore (check_sea_opacity lists where; set_water_look {opaque: true})`] : []),
        ...(seaOp === null && looks && !looks.sea ? ["the level sets no sea opacity: the theme's translucent water (set_water_look {opaque: true}, or a sea material)"] : [])];
      return { ok: warnings.length === 0, haze: hz ? rows.map(({ warning: _w, ...r }) => r) : "the level has no haze", ...(warnings.length ? { warnings } : {}),
        note: "the picture is drawn with the level's own sea and haze; the haze rows say how much of the land's colour the haze takes from each camera's distance (hidden: at or over the limit, default 0.33)" };
    })() } : {}),
    ...(a.tints ? { tints: lt ? { materials: lt.materials, note: "each material whose top tile a tint_landscape carrier recolours is drawn in the tint's colour (what Sunstone draws; with Sunstone off the game shows the theme's textures)" } : "the level has no landscape tint on a top tile of its material file" } : {}),
    water: water === null ? "the level's own height (not known here, so not drawn)" : { map: r2(water), world: r2(water * WORLD_PER_MAP), drawn: seaIn ? `the level's sea at opacity ${seaIn.opacity} (waterLook)` : "a translucent plane" },
    fog: input.fog ? { distance: r2(a.fog!), units: u ?? "map" } : "none",
    region: { x: [r2(r.x0), r2(r.x1)], z: [r2(r.z0), r2(r.z1)], units: "map", clip: !!a.clip },
    ...("sceneryInfo" in input ? { scenery: input.sceneryInfo } : {}),
    markers: {
      legend: Object.fromEntries(Object.entries(MARKER_RGB).map(([k, c]) => [k, `#${c.map((v) => v.toString(16).padStart(2, "0")).join("")}`])),
      note: "a square on a post 1.4 map units above the spot; hollow when terrain hides it",
      placed: views.flatMap((v, k) => v.markers.slice(0, 40).map((m) => ({ ...(orbit ? { tile: k } : {}), kind: m.kind, name: m.name, px: m.px, visible: m.visible }))),
    },
  };
  return { content: [image(png), { type: "text", text: compact(info) }] };
}

const vec3 = (d: string) => ({ type: "array", items: { type: "number" }, minItems: 3, maxItems: 3, description: d }) as const;
const num = (d: string, minimum?: number, maximum?: number) => ({ type: "number", description: d, ...(minimum !== undefined ? { minimum } : {}), ...(maximum !== undefined ? { maximum } : {}) }) as const;

export const tools: Tool[] = [
  {
    name: "render_view",
    description: "A perspective 3/4 picture of the open level (image content), to judge silhouettes before a Test: every voxel frame (rotated and scaled ones too) in its material's real colour, lit by the time of day's sun and ambient light (tops and sides shade apart, so voxel steps read), the project's water level as a translucent plane, spawns and objects as small coloured billboards, a sky by the time of day, optional fog. Camera: `pos` and `target` (with `fov`, default 60), or a `view` preset that frames the whole level or `region` from about 35 degrees up (nw, ne, sw, se; orbit draws all four in a 2x2 sheet), with `azimuth`, `elevation`, `distance` overrides; `game` mimics the in-game turn camera, about 170 world units behind and above `at`. The text part gives the camera(s) in map and world units, the time taken, the frames drawn and the markers' pixels.",
    inputSchema: {
      type: "object", additionalProperties: false,
      properties: {
        view: { type: "string", enum: ["nw", "ne", "sw", "se", "orbit", "game", "level"], description: "Preset camera (default se when no pos): from that side, 35 degrees up, distance fitted to the level or region; orbit: all four sides in a 2x2 sheet; game: the turn camera looking at `at`; level: the pose set_level_camera set for the open project." },
        pos: vec3("Eye [x, y, z] (with target; instead of view)."), target: vec3("Point looked at [x, y, z]; with a preset, the point it orbits (default the middle of the level or region)."),
        at: vec3("For view game: the point the turn camera looks at (a worm's spot)."),
        units: { type: "string", enum: ["map", "world"], description: "Unit of pos, target, at, distance, region and fog: map (a voxel; the default) or world (map x 20)." },
        fov: num("Vertical field of view in degrees (default 60).", 5, 150),
        azimuth: num("Bearing from the target to the eye in degrees, clockwise from north (-z): 0 n, 90 e, 180 s, 270 w. Overrides the preset's; for orbit the first tile's (default 315).", 0, 360),
        elevation: num("Degrees above the target (default 35; game 22).", -89, 89),
        distance: num("From the target to the eye (default: fitted to the level or region; game 170 world units).", 0.1),
        region: { type: "object", additionalProperties: false, description: "The xz box the presets frame (edges left out: the level's extent).",
          properties: { x0: { type: "number" }, z0: { type: "number" }, x1: { type: "number" }, z1: { type: "number" } } },
        clip: { type: "boolean", description: "Draw only the voxels inside region (default false: everything, framed on region)." },
        width: { type: "integer", minimum: 64, maximum: 1920, description: "Image width in pixels (default 960)." },
        height: { type: "integer", minimum: 64, maximum: 1080, description: "Image height in pixels (default 540)." },
        fog: num("Distance fog: the distance at which it hides about 63% (default none).", 0),
        timeOfDay: { type: "string", enum: ["DAY", "EVENING", "NIGHT"], description: "Sky and light (default the level's)." },
        markers: { type: "boolean", description: "Draw spawns and objects (default true)." },
        palette: { type: "string", enum: ["real", "index"], description: "Material colours: real (default; each material's texture colour) or index (a distinct hue per index)." },
        tints: { type: "boolean", description: "Preview the level's landscape tints (tint_landscape): a material whose top tile is tinted draws in the tint's colour (default false: the textures' own colours)." },
        grade: { type: "string", description: "Draw the colours as the game shows them: the name of a built-in grade (" + GRADE_NAMES + ") or the path of a grade file game_material_shots {saveGrade} wrote in a match (with the plugins and the time of day the level is played in). A tint that reads ochre here can play as saturated orange: with a grade the picture shows the played colour. Default none: the textures' mean colours and the tints as written." },
        sideTextures: { type: "boolean", description: "Draw every face but a top in the colour of the material's SIDE texture (a material's sides draw its second texture: a cliff, a wall or a strata band of grass01 is rock). Default false: the top's colour on every face, as before. Needs Melange 0.15+ on the server." },
        waterLook: { type: "boolean", description: "Draw the level's own looks (set_water_look, read from the level script): the sea as its sea material's matte colour at its opacity (1 hides everything under it; without a sea material the water plane's `opacity`), and the level haze (the land and the sea fade to its colour with distance by Sunstone 2.4's model, amount, start and distance as set; the band under the horizon is the haze). Default false: the translucent blue plane and no haze, as before. The text part's `waterLook` says what was drawn." },
        gameMatch: { description: "true, or {hidden (0-1, default 0.33), seaBy: \"mod\" (default: a water shader mod draws the sea material) | \"game\" (the game's own water shader: no sea colour, the plane's opacity)}: waterLook, plus `gameMatch` in the text part: how much of the land's colour the haze takes from this picture's camera, from the level camera (set_level_camera; else the default overview at 0.95 x the level's span) and from the turn camera, with a warning for each camera the haze hides the island from (`hidden` or more of its colour gone) and for a sea that is not opaque. Default off.",
          anyOf: [{ type: "boolean" }, { type: "object", additionalProperties: false, properties: { hidden: { type: "number", minimum: 0.05, maximum: 1 }, seaBy: { type: "string", enum: ["mod", "game"] } } }] },
        antialias: { type: "boolean", description: "Render at 2x2 samples per pixel (smoother edges, about 3-4 times slower; default false)." },
        edges: { type: "boolean", description: "Darken silhouette edges (default true)." },
        scenery: { type: "boolean", description: "Also draw the scenery details (copies included) by their preview meshes, flat-shaded in each mesh's average texture colour (default false; lights and emitters are not drawn). The text part then gives `scenery`: meshes drawn and resources with no mesh." },
      },
    },
    handler: (s, a) => renderViewTool(s, a),
  },
];
