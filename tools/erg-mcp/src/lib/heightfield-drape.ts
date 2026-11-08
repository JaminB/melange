// A draped heightfield: the field's surface as a skin of blocks `thickness` voxels thick whose corner offsets (the
// .xan's per-frame HeightMap, Frame.corners) carry the true height, so slopes are smooth instead of voxel terraces.
//
// What the game does with the offsets (seen in the game, 2026-11-07): each column's vertices, bottom and top, move up
// by the bilinear offset of the column's corners, for drawing and for collision; offsets of 20 voxels on a 4-high block
// drew and collided as a 40-degree slab. Neighbouring columns share a corner, so a column of a block can only follow the
// surface when every column of the block is the same height: each skin block is a slab of whole columns, sheared.
//
// Under the skin the core (optional, default on) is the usual stepped field cut down to floor(lowest corner - 0.25), so
// its steps stay inside the skin; it is planned and packed by heightfield-plan (its cells, blocks and frames as there).
// The skin's corner heights are the mean of the field's heights of the columns round each corner.
import { AddFrame, Group, LIMITS, blockFrames, cellBudget, type Command, type EditorStore, type Frame, type Vec3 } from "../core";
import { ToolError } from "../registry";
import { apply, plan, sampleField, SIDE, type Field, type FieldArgs, type Plan } from "./heightfield-plan";
import { WORLD_PER_MAP, tidy } from "./voxel";

export interface DrapeArgs extends FieldArgs {
  thickness?: number;          // skin voxels, 1-8 (default 2)
  core?: boolean;              // fill under the skin with stepped blocks (default true)
  skirtTo?: number;            // level y (units): the core reaches down to here instead of the floor (a hollow skin gets one)
  skirtTop?: number;           // level y (units): only columns whose skin's lowest corner is at or under it get the core
}

interface Tile { x0: number; x1: number; z0: number; z1: number; y0: number; corners: number[]; columns: number; }

const num = (v: number) => tidy([v, 0, 0])[0];
const MAX_OFFSET = LIMITS.cornerOffset;

/** Heights (voxels above the floor) at the field's (nx+1) x (nz+1) lattice corners. */
function cornerHeights(f: Field): Float32Array {
  const W = f.nx + 1, out = new Float32Array(W * (f.nz + 1));
  for (let k = 0; k <= f.nz; k++)
    for (let i = 0; i <= f.nx; i++) {
      let sum = 0, n = 0, all = 0, alln = 0;
      for (const [a, b] of [[i - 1, k - 1], [i, k - 1], [i - 1, k], [i, k]]) {
        if (a < 0 || b < 0 || a >= f.nx || b >= f.nz) continue;
        const c = b * f.nx + a;
        all += f.height[c]; alln++;
        if (f.hi[c] > f.lo[c]) { sum += f.height[c]; n++; }
      }
      out[k * W + i] = n ? sum / n : alln ? all / alln : 0;
    }
  return out;
}

const live = (f: Field, c: number) => f.hi[c] > f.lo[c];

/** The skin's tiles: 31 x 31 columns, each a slab `T` thick placed so its offsets are centred. */
function tiles(f: Field, H: Float32Array, T: number): Tile[] {
  const W = f.nx + 1, out: Tile[] = [];
  for (let tz = 0; tz < f.nz; tz += SIDE)
    for (let tx = 0; tx < f.nx; tx += SIDE) {
      let x0 = Infinity, x1 = -Infinity, z0 = Infinity, z1 = -Infinity, columns = 0;
      for (let b = tz; b < Math.min(f.nz, tz + SIDE); b++)
        for (let a = tx; a < Math.min(f.nx, tx + SIDE); a++)
          if (live(f, b * f.nx + a)) { x0 = Math.min(x0, a); x1 = Math.max(x1, a + 1); z0 = Math.min(z0, b); z1 = Math.max(z1, b + 1); columns++; }
      if (!columns) continue;
      let lo = Infinity, hi = -Infinity;
      for (let k = z0; k <= z1; k++) for (let i = x0; i <= x1; i++) { lo = Math.min(lo, H[k * W + i]); hi = Math.max(hi, H[k * W + i]); }
      // The slab's top before the offsets, in scene y: the middle of the tile's heights.
      const top = f.floor + Math.round((lo + hi) / 2), y0 = top - T;
      const corners: number[] = [];
      for (let k = z0; k <= z1; k++)
        for (let i = x0; i <= x1; i++) {
          const d = f.floor + H[k * W + i] - top;
          if (Math.abs(d) > MAX_OFFSET)
            throw new ToolError(`the field rises ${num((hi - lo) * f.s)} map units across one skin block (${x1 - x0} x ${z1 - z0} voxels): a corner offset is at most ${MAX_OFFSET} voxels; use a larger voxelSize or lower heights`);
          corners.push(Math.round(d * 1000) / 1000);
        }
      out.push({ x0, x1, z0, z1, y0, corners, columns });
    }
  return out;
}

function skinWords(f: Field, t: Tile, T: number): Uint32Array {
  const X = t.x1 - t.x0, Z = t.z1 - t.z0, w = new Uint32Array(X * T * Z);
  for (let z = 0; z < Z; z++)
    for (let x = 0; x < X; x++) {
      const c = (t.z0 + z) * f.nx + t.x0 + x;
      if (!live(f, c)) continue;
      // Materials as the stepped field would give its top T voxels, so height bands and surface rules carry over.
      for (let y = 0; y < T; y++) w[(z * X + x) * T + y] = f.wordAt(c, f.hi[c] - T + y, true);
    }
  return w;
}

export interface DrapePlan { field: Field; T: number; tiles: Tile[]; core: Plan | null; report: Record<string, unknown>; }

export function planDrape(store: EditorStore, a: DrapeArgs): DrapePlan {
  const T = Math.round(a.thickness ?? 2);
  if (!(T >= 1 && T <= 8)) throw new ToolError("thickness is 1-8 voxels");
  const f = sampleField(store, a);
  if (f.sceneId === null) throw new ToolError("this level has no Scene frame, so no block can be added: a drape needs blocks");
  const H = cornerHeights(f), W = f.nx + 1;
  const ts = tiles(f, H, T);
  const skinCells = ts.reduce((n, t) => n + (t.x1 - t.x0) * T * (t.z1 - t.z0), 0);
  const used = blockFrames(store.scene).length, cells = cellBudget(store.scene);
  const slots = Math.min(LIMITS.newFrames - used, a.maxBlocks ?? Infinity), room = Math.min(cells.free, a.maxCells ?? Infinity);
  let why: string | undefined;
  if (ts.length > slots) why = `the skin needs ${ts.length} blocks and ${slots} may be added`;
  else if (skinCells > room) why = `the skin needs ${skinCells} cells and ${room} are left`;
  // The skirt: the core's bottom moved to skirtTo (scene voxels), and only under columns whose skin dips to skirtTop.
  const k = a.units === "world" ? 1 / WORLD_PER_MAP : 1;
  for (const [v, what] of [[a.skirtTo, "skirtTo"], [a.skirtTop, "skirtTop"]] as const)
    if (v !== undefined && !Number.isFinite(v)) throw new ToolError(`${what} is a level y`);
  if (a.skirtTop !== undefined && a.skirtTo === undefined) throw new ToolError("skirtTop limits a skirt: give skirtTo too");
  const skirt = a.skirtTo !== undefined;
  const skirtLo = skirt ? Math.round((a.skirtTo! * k - f.t[1]) / f.s) : 0;
  const skirtHi = a.skirtTop !== undefined ? (a.skirtTop * k - f.t[1]) / f.s : Infinity;
  let core: Plan | null = null, skirtColumns = 0;
  if (!why && (a.core !== false || skirt)) {
    // The core: each column cut to under its lowest corner, so the skin hides its steps.
    core = plan(store, { ...a, maxBlocks: slots - ts.length, maxCells: room - skinCells, partial: false }, (g) => {
      for (let b = 0; b < g.nz; b++)
        for (let i = 0; i < g.nx; i++) {
          const c = b * g.nx + i;
          if (!live(g, c)) continue;
          const m = Math.min(H[b * W + i], H[b * W + i + 1], H[(b + 1) * W + i], H[(b + 1) * W + i + 1]);
          g.hi[c] = Math.min(g.hi[c], g.floor + Math.floor(m - 0.25));
          if (!skirt) continue;
          // the skin's lowest corner, less its thickness: its underside there
          if (g.floor + m - T > skirtHi) { g.hi[c] = g.lo[c]; continue; }
          g.lo[c] = skirtLo;
          if (g.hi[c] > g.lo[c]) skirtColumns++;
        }
    });
    if (!core.fits) why = skirt ? `the skirt under the skin does not fit: ${core.why ?? ""} (raise skirtTo, lower skirtTop, or raise voxelSize)`
      : `the core under the skin does not fit: ${core.why ?? ""} (core: false leaves the skin hollow)`;
  }
  const s = f.s, top = Math.max(...ts.map((t) => t.y0 + T + Math.max(...t.corners)));
  const report = {
    fits: !why, ...(why ? { why } : {}),
    region: { voxelSize: s, size: [f.nx, f.nz], columns: ts.reduce((n, t) => n + t.columns, 0),
      ...(f.limited ? { slopeLimit: { relax: a.relax ?? 0, ...(a.maxGradient !== undefined ? { maxGradient: a.maxGradient } : {}), passes: f.limited.passes, steepestDegrees: Math.round(f.limited.steepest * 10) / 10,
        note: "the field's heights smoothed (relax) and slope-limited (maxGradient) before the skin and its core are cut, so both follow the same surface" } } : {}),
      peak: ts.length ? { map: num(top * s + f.t[1]), world: num((top * s + f.t[1]) * WORLD_PER_MAP) } : null },
    skin: { thickness: T, blocks: ts.length, cells: skinCells,
      list: ts.map((t) => ({ size: [t.x1 - t.x0, T, t.z1 - t.z0], columns: t.columns, offsets: [num(Math.min(...t.corners)), num(Math.max(...t.corners))] })) },
    core: core ? { blocks: core.kept.length, cells: core.kept.reduce((n, p) => n + (p.x1 - p.x0) * (p.y1 - p.y0) * (p.z1 - p.z0), 0), inFrames: core.report.voxels,
      neededCells: core.pieces.reduce((n, p) => n + (p.x1 - p.x0) * (p.y1 - p.y0) * (p.z1 - p.z0), 0) } : null,
    ...(skirt ? { skirt: { to: { map: num(skirtLo * s + f.t[1]), world: num((skirtLo * s + f.t[1]) * WORLD_PER_MAP) },
      ...(a.skirtTop !== undefined ? { top: { map: num(a.skirtTop * k), world: num(a.skirtTop * k * WORLD_PER_MAP) } } : {}), columns: skirtColumns,
      note: "the core is filled from skirtTo up to just under the skin" + (a.skirtTop !== undefined ? " in the columns whose skin dips to skirtTop" : "") +
        ", so a rising sea shows solid ground, not the skin's underside; core.cells is its cost" } } : {}),
    budget: { blocks: { before: used, after: used + ts.length + (core?.kept.length ?? 0), max: LIMITS.newFrames },
      cells: { before: cells.used, after: cells.used + skinCells + (core ? core.kept.reduce((n, p) => n + (p.x1 - p.x0) * (p.y1 - p.y0) * (p.z1 - p.z0), 0) : 0), max: cells.max } },
    note: "the skin's blocks carry corner offsets: the game draws and collides their columns at the field's height; the Erg panel's 3D view shows them unsheared",
  };
  return { field: f, T, tiles: ts, core, report };
}

export function applyDrape(store: EditorStore, p: DrapePlan): { blocks: { id: number; name: string; skin: boolean }[] } {
  if (p.report.why) throw new ToolError(String(p.report.why));
  const f = p.field, s = f.s, made: { id: number; name: string; skin: boolean }[] = [];
  const skin = (ids: Set<number>, names: Set<string>): Command[] => {
    const cmds: Command[] = [];
    let n = 0, tmp = -1;
    for (const t of p.tiles) {
      while (names.has(`ergdrape_${n}`)) n++;
      while (ids.has(tmp)) tmp--;
      if (tmp < -LIMITS.newFrames) throw new ToolError(`a level holds at most ${LIMITS.newFrames} added blocks`);
      const name = `ergdrape_${n}`, size: Vec3 = [t.x1 - t.x0, p.T, t.z1 - t.z0];
      names.add(name);
      ids.add(tmp);
      const frame: Frame = { id: tmp, new: true, parent: f.sceneId!, name,
        pos: [(f.i0 + t.x0 + size[0] / 2) * s, (t.y0 + size[1] / 2) * s, (f.j0 + t.z0 + size[2] / 2) * s],
        rot: [0, 0, 0], scale: [s, s, s], size, voxels: store.freshRef(), heightMap: null, folder: false, corners: t.corners };
      cmds.push(new AddFrame(store.voxels, frame, skinWords(f, t, p.T)));
      made.push({ id: tmp, name, skin: true });
    }
    return cmds;
  };
  if (p.core) {
    const r = apply(store, p.core, skin);
    return { blocks: [...r.blocks.map((b) => ({ ...b, skin: false })), ...made] };
  }
  const cmds = skin(new Set(blockFrames(store.scene).map((x) => x.id)), new Set(store.scene.frames.map((x) => x.name.toLowerCase())));
  if (!cmds.length) throw new ToolError("the field has no columns: nothing to drape");
  const before = new Set(store.problems());
  store.exec(new Group("Heightfield drape", cmds));
  const fresh = store.problems().filter((x) => !before.has(x));
  if (fresh.length) {
    store.undo();
    throw Object.assign(new ToolError("the drape would make the patch invalid, so it was not applied"), { details: fresh });
  }
  return { blocks: made };
}
