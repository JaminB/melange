// Tool group "dunes": describe_dunes, the dunes of the open level measured along lines across them (lib/dune-profile.ts):
// each crest's slope break, slip-face angle, asymmetry and top width, and how far its top stands off its trough in
// colour, with a verdict per crest and for the field. Read-only. render_side draws the same line as a picture.
import { DUNE_RULES, crestFaults, findCrests, median, wormView, type Crest, type DuneRules, type ProfileSample } from "../lib/dune-profile";
import { applyGrade, gradeMaterial } from "../lib/grade";
import { GRADE_NAMES, loadGrade } from "../lib/grade-file";
import { extentXZ, spansAt, waterMap } from "../lib/inspect-field";
import { hex, type Rgb } from "../lib/inspect-png";
import { landTintPreview, projectMaterials, tintFor, type MaterialSet } from "../lib/materials";
import { WORLD_PER_MAP, grids, isSolid, type Grid } from "../lib/voxel";
import { ToolError, type Tool } from "../registry";

const r2 = (v: number) => Math.round(v * 100) / 100;
const luma = (c: Rgb) => 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];

interface Args { lines?: number[][]; from?: number[]; to?: number[]; azimuth?: number; count?: number; step?: number; minProminence?: number; near?: number; smooth?: number;
  frames?: "skin" | "all"; tints?: boolean; grade?: string; rules?: Partial<DuneRules>; eye?: number; units?: "map" | "world"; limit?: number; }

/** A line's ground profile in runs of unbroken ground (a gap: no ground of the frames read, or the sea). */
function profile(on: Grid[], all: Grid[], a: [number, number], b: [number, number], step: number, water: number | null): ProfileSample[][] {
  const len = Math.hypot(b[0] - a[0], b[1] - a[1]), n = Math.max(1, Math.round(len / step)), runs: ProfileSample[][] = [];
  let run: ProfileSample[] = [];
  for (let i = 0; i <= n; i++) {
    const x = a[0] + ((b[0] - a[0]) * i) / n, z = a[1] + ((b[1] - a[1]) * i) / n;
    const t = spansAt(on, x, z)[0], top = spansAt(all, x, z)[0];
    // something else stands on the skin here (a mesa, a wall): not dune ground
    if (!t || (top && top.top > t.top + 1e-6) || (water !== null && t.top <= water)) { if (run.length) runs.push(run); run = []; continue; }
    run.push({ d: (len * i) / n, y: t.top, material: t.material });
  }
  if (run.length) runs.push(run);
  return runs;
}

/** describe_dunes over grids with a colour per material (null: no colours, the shape alone is judged). */
export function describeDunes(gs0: Grid[], a: Args, waterY: number | null, colour: ((m: number) => Rgb) | null) {
  const k = a.units === "world" ? 1 / WORLD_PER_MAP : 1, out = (v: number) => r2(v / k);
  const solid = gs0.filter((g) => g.words.some(isSolid));
  if (!solid.length) throw new ToolError("the level has no voxels");
  const skins = solid.filter((g) => g.frame.corners), on = a.frames === "all" || !skins.length ? solid : skins;
  const step = (a.step ?? 0.5 / k) * k;
  const lines: { name: string; a: [number, number]; b: [number, number]; azimuth: number }[] = [];
  const az = (p: [number, number], q: [number, number]) => ((Math.atan2(q[0] - p[0], -(q[1] - p[1])) * 180) / Math.PI + 360) % 360;
  if (a.lines?.length || a.from) {
    if (a.lines && a.from) throw new ToolError("give lines, or from and to, not both");
    const list = a.lines ?? [[...(a.from ?? []), ...(a.to ?? [])]];
    list.forEach((l, i) => {
      if (!Array.isArray(l) || l.length !== 4 || !l.every(Number.isFinite)) throw new ToolError(a.lines ? `lines[${i}]: [x0, z0, x1, z1]` : "from and to: [x, z] each");
      const p: [number, number] = [l[0] * k, l[1] * k], q: [number, number] = [l[2] * k, l[3] * k];
      if (Math.hypot(q[0] - p[0], q[1] - p[1]) < step * 4) throw new ToolError(`line ${i} is shorter than four samples`);
      lines.push({ name: `line ${i}`, a: p, b: q, azimuth: az(p, q) });
    });
  } else {
    // no line given: `count` parallel lines across the skin's extent along each azimuth (both axes when none is named)
    const e = extentXZ(on)!, cx = (e.x0 + e.x1) / 2, cz = (e.z0 + e.z1) / 2, span = Math.max(e.x1 - e.x0, e.z1 - e.z0), count = a.count ?? 3;
    for (const dir of a.azimuth !== undefined ? [a.azimuth] : [90, 0]) {
      const t = (dir * Math.PI) / 180, ux = Math.sin(t), uz = -Math.cos(t), px = -uz, pz = ux;
      for (let i = 0; i < count; i++) {
        const off = count === 1 ? 0 : ((i / (count - 1)) - 0.5) * span * 0.5;
        lines.push({ name: `azimuth ${dir}, offset ${out(off)}`, azimuth: dir, a: [cx + px * off - ux * span / 2, cz + pz * off - uz * span / 2], b: [cx + px * off + ux * span / 2, cz + pz * off + uz * span / 2] });
      }
    }
  }
  const rules: DuneRules = { ...DUNE_RULES, ...(a.rules ?? {}) };
  // the run a face's slope is read over: 2 map units, and never under a voxel and a half of the coarsest ground read
  const voxel = Math.max(...on.map((g) => Math.hypot(g.toMap[0], g.toMap[4], g.toMap[8])));
  const near = a.near !== undefined ? a.near * k : Math.max(2, 1.5 * voxel);
  const water = waterY, limit = a.limit ?? 24;
  type Row = { line: string; azimuth: number; at: number[]; crest: Crest; contrast: number | null; faults: string[]; colours: { crest: string; trough: string | null } | null };
  const rows: Row[] = [];
  const perLine = lines.map((l) => {
    const runs = profile(on, solid, l.a, l.b, step, water), len = Math.hypot(l.b[0] - l.a[0], l.b[1] - l.a[1]);
    let crests = 0, ground = 0;
    for (const run of runs) {
      ground += run.length;
      for (const c of findCrests(run, { minProminence: (a.minProminence ?? 1.5 / k) * k, near, smooth: a.smooth === undefined ? undefined : a.smooth * k })) {
        crests++;
        const cc = colour ? colour(c.materials.crest) : null, tc = colour && c.materials.trough !== null ? colour(c.materials.trough) : null;
        const contrast = cc && tc ? Math.abs(luma(cc) - luma(tc)) : null;
        const f = c.d / len;
        rows.push({ line: l.name, azimuth: l.azimuth, at: [l.a[0] + (l.b[0] - l.a[0]) * f, c.y, l.a[1] + (l.b[1] - l.a[1]) * f], crest: c, contrast, faults: crestFaults(c, contrast, rules),
          colours: cc ? { crest: hex(cc), trough: tc ? hex(tc) : null } : null });
      }
    }
    return { line: l.name, from: l.a.map(out), to: l.b.map(out), azimuth: r2(l.azimuth), samples: ground, crests };
  });
  if (!rows.length) return { ok: false, summary: "no crest found on the lines: no ground rises " + out((a.minProminence ?? 1.5 / k) * k) + " over the troughs beside it (a lower minProminence, other lines, or frames: \"all\")", lines: perLine, crests: [], rules };
  // the lines that cross the dunes: the azimuth with the most crests (a line along a ridge meets few)
  const byAz = new Map<number, Row[]>();
  for (const r of rows) byAz.set(Math.round(r.azimuth), [...(byAz.get(Math.round(r.azimuth)) ?? []), r]);
  const across = [...byAz.entries()].sort((p, q) => q[1].length - p[1].length)[0];
  const use = across[1], good = use.filter((r) => !r.faults.length);
  const med = (f: (r: Row) => number) => r2(median(use.map(f)));
  const contrasts = use.map((r) => r.contrast).filter((v): v is number => v !== null);
  const row = (r: Row) => {
    const wv = wormView(r.crest, (a.eye ?? 1 / k) * k);
    return { line: r.line, at: r.at.map(out), prominence: out(r.crest.prominence), slopeBreak: r2(r.crest.slopeBreak), lee: r2(r.crest.lee), windward: r2(r.crest.windward), leeSide: r.crest.leeSide,
      topWidth: out(r.crest.topWidth), ...(r.crest.wavelength !== null ? { wavelength: out(r.crest.wavelength) } : {}),
      ...(r.colours ? { colours: r.colours, contrast: r.contrast === null ? null : Math.round(r.contrast) } : {}),
      ...(wv ? { fromTrough: { elevation: r2(wv.elevation), skyline: wv.skyline } } : {}),
      ...(r.faults.length ? { faults: r.faults } : { reads: "a crested dune" }) };
  };
  const share = good.length / use.length;
  return {
    ok: share >= 0.6,
    summary: `${use.length} crests across the dunes (azimuth ${across[0]}): ${good.length} read as crested dunes` + (share >= 0.6 ? "" : `; the rest: ${[...new Set(use.flatMap((r) => r.faults.map((f) => f.split(" (")[0])))].slice(0, 4).join(", ")}`),
    field: { crests: use.length, crested: good.length, share: r2(share), median: { slopeBreak: med((r) => r.crest.slopeBreak), lee: med((r) => r.crest.lee), windward: med((r) => r.crest.windward), topWidth: out(median(use.map((r) => r.crest.topWidth))),
      prominence: out(median(use.map((r) => r.crest.prominence))), ...(contrasts.length ? { contrast: Math.round(median(contrasts)) } : {}) } },
    crests: [...use].sort((p, q) => q.faults.length - p.faults.length || q.crest.prominence - p.crest.prominence).slice(0, limit).map(row), ...(use.length > limit ? { more: use.length - limit } : {}),
    lines: perLine,
    rules: { ...rules, minProminence: out((a.minProminence ?? 1.5 / k) * k), near: out(near), step: out(step), units: a.units ?? "map" },
    ...(colour ? {} : { colours: "the server gave no material colours: the shape alone was judged" }),
    note: "slopeBreak: how far the surface turns at the top, the two faces' slopes beside the crest added (each the steepest stretch of `near` that starts within `near` of it) (a rolling hill a few degrees, a dune's crest 40 or more); lee: the steeper face (the slip face; dry sand rests at 30 to 34 degrees); windward: the gentler one; topWidth: how long the top is flat (the ground through the crest sloping under 5 degrees); contrast: the crest's top colour against its trough's in brightness (0-255), tints previewed and through `grade` when given; fromTrough: the angle the crest stands over the eye of a worm in the trough before it (skyline false: it does not rise over the worm's horizon). A crest reads as a dune when it passes every rule; ok when six in ten do. render_side {points} draws a line's profile",
  };
}

export const tools: Tool[] = [{
  name: "describe_dunes",
  description: "Do the dunes read as crested dunes? Measures the open level's ground along lines across the dune field: every crest (a top at least `minProminence` over the troughs beside it) with its slope break (how sharply the surface turns at the top), slip-face angle (the steeper side), windward angle, the width of its top, the wavelength, and how far the crest's top colour stands off its trough's in brightness (the materials' colours, landscape tints previewed, through a look `grade` when given), and from a worm's eye in the trough whether the crest rises over its horizon. Each crest is judged against `rules` (a slope break of 38 degrees, a slip face of 26, the two sides 8 apart, a top no wider than 3 map units, a contrast of 18) and names what it misses: rolls over, no slip face, symmetric, flat top, one tone. Give `lines` ([x0, z0, x1, z1] each) or `from` and `to`; with neither it lays `count` lines across the skin along `azimuth`, or along both axes and takes the one that crosses more crests. Only draped skin blocks are read by default (a mesa or a wall standing on the skin breaks the line): frames \"all\" reads every ground. Read-only.",
  inputSchema: { type: "object", additionalProperties: false, properties: {
    lines: { type: "array", minItems: 1, maxItems: 32, items: { type: "array", items: { type: "number" }, minItems: 4, maxItems: 4 }, description: "[x0, z0, x1, z1] each." },
    from: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 }, to: { type: "array", items: { type: "number" }, minItems: 2, maxItems: 2 },
    azimuth: { type: "number", minimum: 0, maximum: 360, description: "With no line: the way the lines run, degrees clockwise from north (-z); default both 90 and 0." },
    count: { type: "integer", minimum: 1, maximum: 9, description: "With no line: lines per azimuth, spread over the middle half of the skin (default 3)." },
    step: { type: "number", minimum: 0.1, maximum: 4, description: "Sample spacing along a line (units; default 0.5 map units)." },
    minProminence: { type: "number", minimum: 0.1, description: "A crest stands at least this over the higher trough beside it (units; default 1.5 map units)." },
    near: { type: "number", minimum: 0.25, description: "The run a face's slope is measured over, beside the crest (units; default 2 map units, or a voxel and a half of the coarsest ground read when that is more)." },
    smooth: { type: "number", minimum: 0, description: "Average the heights over this run first (units; default none: a draped skin is smooth already; a voxel-stepped field wants about two voxels)." },
    frames: { type: "string", enum: ["skin", "all"] },
    tints: { type: "boolean", description: "Preview the level's landscape tints in the colours (default true)." },
    grade: { type: "string", description: "The colours as the game plays them: a built-in grade's name (" + GRADE_NAMES + ") or a grade file (game_material_shots {saveGrade})." },
    rules: { type: "object", additionalProperties: false, description: "Override a rule: minBreak, minLee, minAsymmetry (degrees), maxTopWidth (map units), minContrast (0-255).",
      properties: { minBreak: { type: "number" }, minLee: { type: "number" }, minAsymmetry: { type: "number" }, maxTopWidth: { type: "number" }, minContrast: { type: "number" } } },
    eye: { type: "number", minimum: 0, description: "A worm's eye over the ground (units; default 1 map unit)." },
    limit: { type: "integer", minimum: 1, maximum: 200, description: "Crests listed (default 24), the ones with most faults first." },
    units: { type: "string", enum: ["map", "world"] },
  } },
  async handler(s, a: Args) {
    const { store } = s.requireProject();
    const set: MaterialSet | null = await projectMaterials(s).catch(() => null);
    let colour: ((m: number) => Rgb) | null = null;
    if (set?.colors) {
      const base = tintFor(set), lt = a.tints === false ? null : landTintPreview(store, set, base), t0 = lt?.tint ?? base;
      const tinted = new Set((lt?.materials ?? []).map((m) => m.material));
      const grade = a.grade === undefined ? null : loadGrade(a.grade).grade;
      colour = grade ? (m) => (grade.measured ? gradeMaterial(grade, m, t0(m), tinted.has(m)) : applyGrade(grade, t0(m))) : t0;
    } else if (a.grade !== undefined) loadGrade(a.grade);
    return describeDunes(grids(store), a, waterMap(store), colour);
  },
}];
