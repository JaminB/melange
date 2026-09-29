// Pure geometry and lookups for the timeline canvas: no DOM, no canvas context, so this is unit-testable on its
// own. The names and order below match melange/wormsign.h's Comp enum.
import type { TickRecord } from "../../sdk/wsr";

export const COMPONENT_NAMES = ["time+rng", "turn", "worms", "tasks", "projectiles", "teams"] as const;

export interface ViewRange { from: number; to: number } // inclusive tick range

export function clampView(view: ViewRange, totalTicks: number, minSpan = 5): ViewRange {
  const maxTick = Math.max(0, totalTicks - 1);
  const span = Math.max(minSpan, Math.min(view.to - view.from, maxTick + 1));
  let from = view.from, to = from + span;
  if (from < 0) {
    from = 0;
    to = from + span;
  }
  if (to > maxTick) {
    to = maxTick;
    from = to - span;
  }
  if (from < 0) from = 0; // the file is shorter than minSpan
  return { from: Math.round(from), to: Math.round(Math.max(from, to)) };
}

// `aroundFrac` (0..1) is where in the view the zoom is centered (e.g. the mouse position).
export function zoomView(view: ViewRange, totalTicks: number, factor: number, aroundFrac: number): ViewRange {
  const span = view.to - view.from;
  const center = view.from + span * aroundFrac;
  const newSpan = Math.max(1, span * factor);
  return clampView({ from: center - newSpan * aroundFrac, to: center + newSpan * (1 - aroundFrac) }, totalTicks);
}

export function panView(view: ViewRange, totalTicks: number, deltaTicks: number): ViewRange {
  return clampView({ from: view.from + deltaTicks, to: view.to + deltaTicks }, totalTicks);
}

export function tickToFrac(tick: number, view: ViewRange): number {
  return (tick - view.from) / Math.max(1, view.to - view.from);
}

export function fracToTick(frac: number, view: ViewRange): number {
  return Math.round(view.from + frac * (view.to - view.from));
}

// Bit i set = engine component i differs from the previous tick (TickHash.c, same order as COMPONENT_NAMES).
export function changedMask(cur: TickRecord, prev: TickRecord | undefined): number {
  if (!prev) return 0;
  let mask = 0;
  for (let i = 0; i < cur.c.length; i++) if (cur.c[i] !== prev.c[i]) mask |= 1 << i;
  return mask;
}

// Binary search over ticks sorted by tick number: the last record with tick <= target, or undefined if target
// is before the first recorded tick.
export function tickAtOrBefore(ticks: readonly TickRecord[], target: number): TickRecord | undefined {
  let lo = 0, hi = ticks.length - 1, ans = -1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (ticks[mid].tick <= target) {
      ans = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return ans >= 0 ? ticks[ans] : undefined;
}

// The FPU control word every tick should carry: 53-bit precision, round-to-nearest, all exceptions masked.
// Anything else is a fault worth a marker on the timeline.
export const EXPECTED_FPUCW = 0x027f;
export function fpuFaultTicks(ticks: readonly TickRecord[]): number[] {
  return ticks.filter((t) => t.fpucw !== EXPECTED_FPUCW).map((t) => t.tick);
}
