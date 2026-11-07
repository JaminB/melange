// The terrain brush controls: mode, shape, size in voxels, the theme's 64 materials, the last step's result, and the added
// blocks.
import { useEffect, useReducer } from "preact/hooks";
import { LIMITS, type Vec3 } from "../../../sdk/erg";
import { MAX_BRUSH, type BrushMode, type BrushShape } from "./brush";
import { flatPalette } from "./materials";
import type { TerrainTool } from "./tool";

const MODES: [BrushMode, string, string][] = [
  ["carve", "Carve", "Clear solid voxels; reaches every frame the brush overlaps"],
  ["fill", "Fill", "Fill empty voxels of the frame under the cursor"],
  ["paint", "Paint", "Change the material of solid voxels"],
  ["second", "2nd material", "Experimental: paint the second material and its corner mask (also changes collision slightly)"],
  ["block", "Add block", "Click to add a new solid block of this size and material"],
];
const SHAPES: [BrushShape, string][] = [["sphere", "Sphere"], ["box", "Box"]];
const AXES = ["X", "Y", "Z"];

/** `names`: the level material file's record names (level.materials), when the server sent them. */
export function TerrainTools({ tool, palette, names }: { tool: TerrainTool; palette?: string[]; names?: string[] }) {
  const [, redraw] = useReducer((n: number, _: void) => n + 1, 0);
  useEffect(() => tool.onChange(() => redraw()), [tool]);
  const b = tool.brush, colors = palette ?? flatPalette(), last = tool.last;
  const uniform = b.size[0] === b.size[1] && b.size[1] === b.size[2], block = b.mode === "block", second = b.mode === "second";
  const blocks = tool.sculptor.blockList;
  const setAxis = (i: number, v: number) => tool.setBrush({ size: b.size.map((s, k) => (k === i ? v : s)) as Vec3 });
  const kb = Math.round(tool.sculptor.patchBytes / 1024), limitKb = Math.round(LIMITS.patchBytes / 1024);
  const cells = tool.sculptor.cells, maxSide = block ? LIMITS.newFrameSide : MAX_BRUSH, scale = b.blockScale ?? 1;

  return (
    <div class="erg-terrain pad" data-erg-terrain>
      <div class="row" role="group" aria-label="Brush mode">
        {MODES.map(([m, label, title]) => (
          <button class={`btn${b.mode === m ? " on" : ""}`} aria-pressed={b.mode === m} data-mode={m} title={title}
            onClick={() => tool.setBrush({ mode: m })}>{label}</button>
        ))}
      </div>
      {!block ? (
        <div class="row" role="group" aria-label="Brush shape" style={{ marginTop: "8px" }}>
          {SHAPES.map(([s, label]) => (
            <button class={`btn${b.shape === s ? " on" : ""}`} aria-pressed={b.shape === s} data-shape={s} onClick={() => tool.setBrush({ shape: s })}>{label}</button>
          ))}
        </div>
      ) : null}
      <label class="row" style={{ marginTop: "8px" }}>
        <span>Size</span>
        <input type="range" min={1} max={maxSide} value={Math.max(...b.size)} data-size
          onInput={(e) => { const v = +(e.currentTarget as HTMLInputElement).value; tool.setBrush({ size: [v, v, v] }); }} />
        <span class="muted">{uniform ? `${b.size[0]} voxels` : b.size.join(" × ")}</span>
      </label>
      {b.shape === "box" || block ? (
        <div class="row" style={{ marginTop: "4px" }}>
          {AXES.map((ax, i) => (
            <label>{ax} <input type="number" min={1} max={maxSide} value={b.size[i]} style={{ width: "4em" }} data-axis={ax}
              onChange={(e) => setAxis(i, +(e.currentTarget as HTMLInputElement).value)} /></label>
          ))}
        </div>
      ) : null}
      {block ? (
        <label class="row" style={{ marginTop: "4px" }} title="Map units per voxel: a larger scale covers more ground with the same cells">
          <span>Scale</span>
          <input type="number" min={LIMITS.newFrameScale.min} max={LIMITS.newFrameScale.max} step={0.1} value={scale} style={{ width: "5em" }}
            data-block-scale onChange={(e) => tool.setBrush({ blockScale: +(e.currentTarget as HTMLInputElement).value })} />
          <span class="muted">{`${+(b.size[0] * scale).toFixed(2)} × ${+(b.size[1] * scale).toFixed(2)} × ${+(b.size[2] * scale).toFixed(2)} map units`}</span>
        </label>
      ) : null}
      {b.mode !== "carve" ? (
        <div style={{ marginTop: "10px" }}>
          {second ? (
            <p class="warn small" data-second-note>Experimental: paints the level's second material over solid voxels, with
              partial corners on the border. Each corner also changes the collision shape slightly, so test the level to
              check the result.</p>
          ) : null}
          <div class="row between">
            <span>{second && b.material === "none" ? "Removing the second material" : b.material === "column" ? "Material from the column"
              : `Material ${b.material}${typeof b.material === "number" && names?.[b.material] ? `: ${names[b.material]}` : ""}`}</span>
            {b.mode === "fill" ? (
              <button class={`btn${b.material === "column" ? " on" : ""}`} data-column onClick={() => tool.setBrush({ material: "column" })}
                title="Each filled voxel takes the material of the highest solid voxel above it in its frame">Match column</button>
            ) : second ? (
              <button class={`btn${b.material === "none" ? " on" : ""}`} data-second-none onClick={() => tool.setBrush({ material: "none" })}
                title="Restore the loaded level's second material inside the brush">Remove</button>
            ) : null}
          </div>
          <div role="listbox" aria-label="Material" data-materials
            style={{ display: "grid", gridTemplateColumns: "repeat(16, 1fr)", gap: "2px", marginTop: "6px" }}>
            {colors.map((c, m) => (
              <button role="option" aria-selected={b.material === m} title={names?.[m] ? `${m}: ${names[m]}` : `Material ${m}`} data-material={m}
                onClick={() => tool.setBrush({ material: m })}
                style={{ aspectRatio: "1", minWidth: "12px", background: c, cursor: "pointer", borderRadius: "2px",
                  opacity: names && (!names[m] || names[m] === "NULL") ? 0.35 : 1,
                  border: b.material === m ? "2px solid var(--focus)" : "1px solid var(--border)" }} />
            ))}
          </div>
        </div>
      ) : null}
      <p class="muted" style={{ marginTop: "10px" }} data-status>
        {last?.refused ? <span class="error">{last.refused}</span>
          : block ? "Click the terrain to add a block of this size and material resting there. A block is new solid ground."
          : last ? `${last.changed} voxels in ${last.frames.length} frame${last.frames.length === 1 ? "" : "s"}, ${last.ms.toFixed(1)} ms` : "Drag over the terrain to sculpt. [ and ] resize the brush."}
      </p>
      <p class="muted" data-budget>Terrain edits: {kb} KB of the patch's {limitKb} KB.</p>
      <p class={cells.used > cells.max ? "error" : "muted"} data-cells
        title="The game counts every cell of every voxel frame (X × Y × Z, solid or empty) and crashes loading a map past this">
        Voxel cells: {cells.used.toLocaleString("en-US")} of the game's {cells.max.toLocaleString("en-US")}
        {cells.used > cells.max ? " — over the limit: export, build and Test refuse this map" : ` (${cells.free.toLocaleString("en-US")} free)`}.
      </p>
      {block || blocks.length ? (
        <div data-blocks>
          <p class="muted">Added blocks: {blocks.length} of {LIMITS.newFrames}, each side up to {LIMITS.newFrameSide} voxels.</p>
          {blocks.map((f) => (
            <div class="row between" data-block={f.name}>
              <span>{f.name} <span class="muted">{f.size.join(" × ")}{f.scale[0] !== 1 ? ` at scale ${f.scale[0]}` : ""}</span></span>
              <button class="btn" data-remove-block={f.id} onClick={() => tool.sculptor.removeBlock(f.id)}>Remove</button>
            </div>
          ))}
        </div>
      ) : null}
      <p class="muted">The view's terrain is an approximation of the game's; test the level to see the real result.</p>
    </div>
  );
}
