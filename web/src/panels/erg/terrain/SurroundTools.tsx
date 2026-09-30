// The surround (.hmp) painter: a top-down 100x100 height grid with raise, lower, flatten and smooth brushes.
import { useEffect, useMemo, useReducer, useRef } from "preact/hooks";
import { SetLevel } from "../../../sdk/erg";
import type { EditorStore } from "../model/store";
import { MAX_SURROUND_RADIUS, SURROUND_SIDE, SurroundPainter, type SurroundMode } from "./surround";

const MODES: [SurroundMode, string][] = [["raise", "Raise"], ["lower", "Lower"], ["flatten", "Flatten"], ["smooth", "Smooth"]];
const PX = 3;

function draw(c: HTMLCanvasElement, h: Float32Array) {
  const g = c.getContext("2d");
  if (!g) return;
  const img = g.createImageData(SURROUND_SIDE, SURROUND_SIDE);
  for (let i = 0; i < h.length; i++) {
    const v = Math.min(1, Math.max(0, h[i]));
    img.data[i * 4] = 40 + 180 * v;
    img.data[i * 4 + 1] = 70 + 150 * v;
    img.data[i * 4 + 2] = 110 - 50 * v;
    img.data[i * 4 + 3] = 255;
  }
  g.putImageData(img, 0, 0);
}

export function SurroundTools({ store }: { store: EditorStore }) {
  const [, redraw] = useReducer((n: number, _: void) => n + 1, 0);
  const painter = useMemo(() => new SurroundPainter({ surround: store.surround, exec: (c, m) => store.exec(c, m) }), [store]);
  const canvas = useRef<HTMLCanvasElement>(null);
  const down = useRef(false);
  useEffect(() => {
    if (canvas.current) draw(canvas.current, store.surround.heights);
    return store.on((why) => { if (why === "scene" && canvas.current) draw(canvas.current, store.surround.heights); });
  }, [store]);

  const painting = store.scene.hmp.mode === "paint";
  const b = painter.brush;
  const set = (v: Parameters<SurroundPainter["setBrush"]>[0]) => { painter.setBrush(v); redraw(); };
  const cell = (e: PointerEvent): [number, number] => {
    const r = (e.currentTarget as HTMLCanvasElement).getBoundingClientRect();
    return [((e.clientX - r.left) / r.width) * SURROUND_SIDE, ((e.clientY - r.top) / r.height) * SURROUND_SIDE];
  };
  const onDown = (e: PointerEvent) => {
    if (!painting) return;
    const [x, y] = cell(e);
    if (e.shiftKey) { set({ level: painter.heightAt(x, y) }); return; }
    (e.currentTarget as HTMLCanvasElement).setPointerCapture(e.pointerId);
    down.current = true;
    painter.begin();
    painter.step(x, y);
  };
  const onMove = (e: PointerEvent) => { if (down.current) painter.step(...cell(e)); };
  const onUp = () => { if (down.current) { down.current = false; painter.end(); } };

  return (
    <div class="erg-terrain pad" data-erg-surround style={{ borderTop: "1px solid var(--border)" }}>
      <div class="row between"><strong>Surround</strong>
        {!painting ? (
          <button class="btn" data-surround-paint disabled={!store.canPaintSurround}
            onClick={() => store.exec(new SetLevel({ hmp: "paint" }, "Paint the surround"))}>Paint the surround</button>
        ) : null}
      </div>
      {!store.canPaintSurround ? <p class="muted error">The base level's surround did not load; reopen the project to paint it.</p> : null}
      <canvas ref={canvas} width={SURROUND_SIDE} height={SURROUND_SIDE} data-surround-grid
        style={{ width: `${SURROUND_SIDE * PX}px`, maxWidth: "100%", aspectRatio: "1", imageRendering: "pixelated", marginTop: "8px",
          cursor: painting ? "crosshair" : "default", opacity: painting ? 1 : 0.5, touchAction: "none" }}
        onPointerDown={onDown} onPointerMove={onMove} onPointerUp={onUp} onPointerCancel={onUp} />
      {painting ? (
        <>
          <div class="row" role="group" aria-label="Surround brush" style={{ marginTop: "8px" }}>
            {MODES.map(([m, label]) => (
              <button class={`btn${b.mode === m ? " on" : ""}`} aria-pressed={b.mode === m} data-surround-mode={m} onClick={() => set({ mode: m })}>{label}</button>
            ))}
          </div>
          <label class="row" style={{ marginTop: "8px" }}>
            <span>Radius</span>
            <input type="range" min={1} max={MAX_SURROUND_RADIUS} value={b.radius} data-surround-radius
              onInput={(e) => set({ radius: +(e.currentTarget as HTMLInputElement).value })} />
            <span class="muted">{b.radius} cells</span>
          </label>
          <label class="row" style={{ marginTop: "4px" }}>
            <span>Strength</span>
            <input type="range" min={0.05} max={1} step={0.05} value={b.strength} data-surround-strength
              onInput={(e) => set({ strength: +(e.currentTarget as HTMLInputElement).value })} />
            <span class="muted">{Math.round(b.strength * 100)}%</span>
          </label>
          {b.mode === "flatten" ? (
            <label class="row" style={{ marginTop: "4px" }}>
              <span>Level</span>
              <input type="range" min={0} max={1} step={1 / 256} value={b.level} data-surround-level
                onInput={(e) => set({ level: +(e.currentTarget as HTMLInputElement).value })} />
              <span class="muted">{b.level.toFixed(3)}</span>
            </label>
          ) : null}
          <p class="muted" style={{ marginTop: "8px" }}>
            Drag on the grid to paint; shift-click picks the flatten level. Heights are relative (0 to 1): test the level to see
            them in the game.
          </p>
        </>
      ) : (
        <p class="muted" style={{ marginTop: "8px" }}>The surround is the terrain around the level. Paint it to change its heights.</p>
      )}
    </div>
  );
}
