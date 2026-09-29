// A canvas timeline of one .wsr file's engine-hash track: one row per engine component, zoom (wheel) and pan
// (drag), a divergence marker per DVRG chunk, and a click-to-inspect detail strip. Only the visible tick range
// is ever scanned or drawn, so this stays smooth over a multi-hour recording.
import { useEffect, useRef, useState } from "preact/hooks";
import type { TickRecord, WsrFile } from "../../sdk/wsr";
import {
  COMPONENT_NAMES, EXPECTED_FPUCW, changedMask, clampView, fracToTick, panView, tickAtOrBefore, tickToFrac, zoomView,
  type ViewRange,
} from "./model";

export interface TimelineProps {
  file: WsrFile;
  name: string;
  onClose: () => void;
}

const HEADER_H = 22;
const FAULT_H = 10;
const DETAIL_H = 0; // the detail strip is a separate DOM element below the canvas

function startIndex(ticks: readonly TickRecord[], t: number): number {
  let lo = 0, hi = ticks.length;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (ticks[mid].tick < t) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

function css(name: string, fallback: string): string {
  const v = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  return v || fallback;
}

export function Timeline({ file, name, onClose }: TimelineProps) {
  const total = file.tickRange ? file.tickRange.to + 1 : file.ticks.length;
  const [view, setView] = useState<ViewRange>(() => clampView({ from: 0, to: Math.max(0, total - 1) }, total, Math.min(50, Math.max(1, total))));
  const [selected, setSelected] = useState<number>();
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const boxRef = useRef<HTMLDivElement>(null);
  const dragRef = useRef<{ x: number; view: ViewRange } | null>(null);
  const movedRef = useRef(false);

  const draw = () => {
    const canvas = canvasRef.current, box = boxRef.current;
    if (!canvas || !box) return;
    const w = box.clientWidth, h = box.clientHeight;
    if (w === 0 || h === 0) return;
    const dpr = window.devicePixelRatio || 1;
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(h * dpr);
    canvas.style.width = `${w}px`;
    canvas.style.height = `${h}px`;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);

    const border = css("--border", "#333"), muted = css("--muted", "#888"), accent = css("--accent", "#e7a54b");
    const bad = css("--bad", "#c33"), surface2 = css("--surface-2", "#222");

    const rows = COMPONENT_NAMES.length;
    const rowH = Math.max(8, (h - HEADER_H - FAULT_H - DETAIL_H) / rows);

    // ruler
    ctx.strokeStyle = border;
    ctx.beginPath();
    ctx.moveTo(0, HEADER_H - 0.5);
    ctx.lineTo(w, HEADER_H - 0.5);
    ctx.stroke();
    ctx.fillStyle = muted;
    ctx.font = "11px sans-serif";
    const span = Math.max(1, view.to - view.from);
    const labelEvery = Math.max(1, Math.round(span / Math.max(1, w / 80)));
    const first = Math.ceil(view.from / labelEvery) * labelEvery;
    for (let t = first; t <= view.to; t += labelEvery) {
      const x = tickToFrac(t, view) * w;
      ctx.fillText(String(t), Math.max(0, Math.min(w - 28, x + 2)), 14);
    }

    // row backgrounds
    for (let r = 0; r < rows; r++) {
      if (r % 2 === 1) {
        ctx.fillStyle = surface2;
        ctx.fillRect(0, HEADER_H + r * rowH, w, rowH);
      }
    }

    // hash-change columns, bucketed one per device pixel column
    const width = Math.max(1, Math.ceil(w));
    const buckets = new Uint8Array(width);
    const faults = new Uint8Array(width);
    const i0 = startIndex(file.ticks, view.from);
    let prev: TickRecord | undefined = i0 > 0 ? file.ticks[i0 - 1] : undefined;
    for (let i = i0; i < file.ticks.length; i++) {
      const t = file.ticks[i];
      if (t.tick > view.to) break;
      const x = Math.floor(tickToFrac(t.tick, view) * width);
      if (x >= 0 && x < width) {
        buckets[x] |= changedMask(t, prev);
        if (t.fpucw !== EXPECTED_FPUCW) faults[x] = 1;
      }
      prev = t;
    }
    ctx.fillStyle = accent;
    for (let x = 0; x < width; x++) {
      for (let r = 0; r < rows; r++) if (buckets[x] & (1 << r)) ctx.fillRect(x, HEADER_H + r * rowH, 1, rowH - 1);
    }
    ctx.fillStyle = bad;
    for (let x = 0; x < width; x++) if (faults[x]) ctx.fillRect(x, HEADER_H + rows * rowH + 2, 1, FAULT_H - 4);

    // row labels
    ctx.fillStyle = muted;
    ctx.font = "10px sans-serif";
    for (let r = 0; r < rows; r++) ctx.fillText(COMPONENT_NAMES[r], 4, HEADER_H + r * rowH + rowH / 2 + 3);

    // divergence markers span the full height
    ctx.strokeStyle = bad;
    ctx.lineWidth = 1.5;
    for (const d of file.divergences) {
      const dt = d && typeof d === "object" ? (d as Record<string, unknown>).tick : undefined;
      if (typeof dt !== "number" || dt < view.from || dt > view.to) continue;
      const x = tickToFrac(dt, view) * w;
      ctx.beginPath();
      ctx.moveTo(x, 0);
      ctx.lineTo(x, h);
      ctx.stroke();
    }
    ctx.lineWidth = 1;

    // selection cursor
    if (selected !== undefined && selected >= view.from && selected <= view.to) {
      const x = tickToFrac(selected, view) * w;
      ctx.strokeStyle = accent;
      ctx.beginPath();
      ctx.moveTo(x, 0);
      ctx.lineTo(x, h);
      ctx.stroke();
    }
  };

  useEffect(draw);
  useEffect(() => {
    if (!boxRef.current) return;
    const ro = new ResizeObserver(() => draw());
    ro.observe(boxRef.current);
    return () => ro.disconnect();
  }, []);

  const onWheel = (e: WheelEvent) => {
    e.preventDefault();
    const box = boxRef.current;
    if (!box) return;
    const rect = box.getBoundingClientRect();
    const frac = Math.min(1, Math.max(0, (e.clientX - rect.left) / rect.width));
    const factor = e.deltaY > 0 ? 1.25 : 1 / 1.25;
    setView((v) => zoomView(v, total, factor, frac));
  };
  const onPointerDown = (e: PointerEvent) => {
    (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
    dragRef.current = { x: e.clientX, view };
    movedRef.current = false;
  };
  const onPointerMove = (e: PointerEvent) => {
    const drag = dragRef.current;
    const box = boxRef.current;
    if (!drag || !box) return;
    if (Math.abs(e.clientX - drag.x) > 3) movedRef.current = true;
    const dxPx = e.clientX - drag.x;
    const deltaTicks = -(dxPx / box.clientWidth) * (drag.view.to - drag.view.from);
    setView(panView(drag.view, total, deltaTicks));
  };
  const onPointerUp = () => { dragRef.current = null; };
  const onClick = (e: MouseEvent) => {
    if (movedRef.current) {
      movedRef.current = false;
      return;
    }
    const box = boxRef.current;
    if (!box) return;
    const rect = box.getBoundingClientRect();
    const frac = Math.min(1, Math.max(0, (e.clientX - rect.left) / rect.width));
    setSelected(fracToTick(frac, view));
  };

  const sel = selected !== undefined ? tickAtOrBefore(file.ticks, selected) : undefined;

  return (
    <div class="tl">
      <div class="tl-toolbar row between pad-x">
        <div class="row">
          <button class="btn" onClick={onClose}>&larr; Replays</button>
          <strong class="mono small">{name}</strong>
          <span class="muted small">{file.complete ? "complete" : "incomplete"}{file.header.online ? " · online" : ""}</span>
        </div>
        <span class="muted small">scroll to zoom · drag to pan · click a tick to inspect it</span>
      </div>
      <div
        class="tl-canvas-wrap"
        ref={boxRef}
        onWheel={onWheel}
        onPointerDown={onPointerDown}
        onPointerMove={onPointerMove}
        onPointerUp={onPointerUp}
        onClick={onClick}
      >
        <canvas ref={canvasRef} />
      </div>
      <div class="tl-detail pad-x">
        {sel ? (
          <div class="row small">
            <span><span class="muted">tick</span> {sel.tick}</span>
            <span><span class="muted">engine</span> <code class="mono">{sel.engine}</code></span>
            <span><span class="muted">mods</span> <code class="mono">{sel.mods}</code></span>
            <span>
              <span class="muted">fpucw</span> <code class="mono">{sel.fpucw.toString(16).padStart(4, "0")}</code>
              {sel.fpucw !== EXPECTED_FPUCW ? <span class="warn"> (expected 027f)</span> : null}
            </span>
            <span><span class="muted">inputs</span> {sel.inputs}</span>
          </div>
        ) : (
          <p class="muted small">Click the timeline to inspect a tick.</p>
        )}
      </div>
    </div>
  );
}
