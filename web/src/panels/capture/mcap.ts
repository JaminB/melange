// Types and pure helpers for the .mcap format (docs/capture-format.md). No zip or DOM code here: see zip.ts and
// Capture.tsx.
export class UnsupportedCaptureError extends Error {}

export interface McapCounts {
  calls: number;
  draws: number;
  textures: number;
  programs: number;
  programsWithAsm: number;
  payloads: number;
  events: number;
}
export interface McapOptions {
  frames: number;
  textures: boolean;
  shaders: boolean;
  frameImage: boolean;
  bufferSizes: boolean;
  maxTextureMB: number;
}
export interface Manifest {
  format: string;
  version: number;
  melangeVersion?: string;
  exeSha256?: string;
  exeBuild?: string;
  gl?: { vendor: string; renderer: string; version: string };
  window?: { w: number; h: number };
  frames: number[];
  scene?: string;
  counts: McapCounts;
  options?: McapOptions;
  files: string[];
  droppedRecords?: number;
  droppedPayloads?: number;
  readbackMs?: number;
  notes?: string[];
}

// Any object member this viewer does not know is ignored; only a version this viewer cannot interpret is refused.
export function checkManifest(value: unknown): asserts value is Manifest {
  if (!value || typeof value !== "object") throw new UnsupportedCaptureError("manifest.json is not an object");
  const m = value as Record<string, unknown>;
  if (m.format !== "melange-capture") throw new UnsupportedCaptureError(`not a Melange capture (format ${JSON.stringify(m.format)})`);
  if (typeof m.version !== "number" || m.version > 1)
    throw new UnsupportedCaptureError(`capture format version ${String(m.version)} is newer than this viewer supports (1)`);
  if (!m.counts || typeof m.counts !== "object") throw new UnsupportedCaptureError("manifest.json is missing counts");
}

export function parseJsonl<T>(lines: readonly string[]): T[] {
  const out: T[] = [];
  for (const line of lines) {
    const trimmed = line.trim();
    if (trimmed) out.push(JSON.parse(trimmed) as T);
  }
  return out;
}

export interface CallRecord {
  i: number;
  f: number;
  fn: string;
  src: string;
  caller?: string;
  pass?: number;
  args?: unknown;
  text?: string;
  payload?: unknown;
  tsc?: number;
  raw?: boolean;
  truncated?: boolean;
  bytes?: number;
}
export interface EventRecord {
  at: number;
  type: string;
  frame?: number;
  pass?: number;
  stage?: string;
  target?: number;
  arb?: number;
  program?: string;
  text?: string;
}

// docs/capture-format.md's Categorize() prefixes: mirrored here so "draws only" matches manifest.counts.draws.
const DRAW_PREFIXES = ["glDrawArrays", "glDrawElements", "glDrawRangeElements", "glMultiDrawArrays", "glMultiDrawElements"];
export function isDraw(fn: string): boolean {
  return DRAW_PREFIXES.some((p) => fn.startsWith(p));
}

export interface Marks {
  frame: Int32Array;
  pass: Int32Array;
  stage: (string | undefined)[];
}
// One frame/pass/stage value per call, carried forward from the events that precede it (events.jsonl `at`).
export function markCalls(callCount: number, events: readonly EventRecord[]): Marks {
  const frame = new Int32Array(callCount).fill(-1);
  const pass = new Int32Array(callCount).fill(-1);
  const stage: (string | undefined)[] = new Array(callCount).fill(undefined);
  const sorted = [...events].sort((a, b) => a.at - b.at);
  let curFrame = -1, curPass = -1, curStage: string | undefined;
  let ei = 0;
  for (let i = 0; i < callCount; i++) {
    while (ei < sorted.length && sorted[ei].at <= i) {
      const e = sorted[ei++];
      if (e.type === "frame-begin" && typeof e.frame === "number") curFrame = e.frame;
      if (e.type === "pass" && typeof e.pass === "number") curPass = e.pass;
      if (e.type === "stage" && typeof e.stage === "string") curStage = e.stage;
    }
    frame[i] = curFrame;
    pass[i] = curPass;
    stage[i] = curStage;
  }
  return { frame, pass, stage };
}

export interface ProgramBinding { at: number; arb: number }
// cg-bind events in order, kept only where an ARB program was actually bound (arb 0 = unbind).
export function programBindings(events: readonly EventRecord[]): ProgramBinding[] {
  return events
    .filter((e): e is EventRecord & { arb: number } => e.type === "cg-bind" && typeof e.arb === "number" && e.arb !== 0)
    .map((e) => ({ at: e.at, arb: e.arb }))
    .sort((a, b) => a.at - b.at);
}
export function activeProgramAt(bindings: readonly ProgramBinding[], callIndex: number): number | undefined {
  let result: number | undefined;
  for (const b of bindings) {
    if (b.at > callIndex) break;
    result = b.arb;
  }
  return result;
}

export interface TextureEntry {
  gl: number;
  w: number;
  h: number;
  levels: number;
  internalFormat: number;
  depth?: boolean;
  name?: string;
  file?: string;
  skipped?: string;
}
export interface ProgramEntry {
  n: number;
  file: string;
  entry: string;
  stage: string;
  source?: string;
  cgProgram?: number;
  arbName?: number;
  binds?: number;
  failed?: boolean;
  overridden?: boolean;
  glsl?: boolean;
  owner?: string;
  asm?: string;
}

export interface StateDiffRow { key: string; before: unknown; after: unknown }
export function stateDiff(begin: Record<string, unknown> | undefined, end: Record<string, unknown> | undefined): StateDiffRow[] {
  const keys = new Set([...Object.keys(begin ?? {}), ...Object.keys(end ?? {})]);
  const out: StateDiffRow[] = [];
  for (const key of [...keys].sort()) {
    const before = begin?.[key];
    const after = end?.[key];
    if (JSON.stringify(before) !== JSON.stringify(after)) out.push({ key, before, after });
  }
  return out;
}
