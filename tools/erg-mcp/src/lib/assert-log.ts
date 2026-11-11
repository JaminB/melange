// The game's asserts and GL errors during a Test, for test's vanilla baseline: every engine line of the game's log
// (Oasis's log channel, category engine) that is an ASSERT(...) fail or a GL ERROR is counted by its signature (the
// line without the engine's prefix and padding: "ASSERT((((HRESULT)(hRes)) >= 0)) fail: 1187 @ .\ParticleEmitterEffectEntity.cpp").
// A run is one Test match; the baseline (the game's own map, no Erg level) and an Erg run are diffed by signature, so
// the question "does the Erg level cause this assert?" is answered from the two runs instead of asked again every
// iteration (the Caldera bench's R2). Pure: lines in, counts and diffs out.

export interface AssertRun {
  label: string;                  // the Test's key (Multi.ergtest_<project>, or the game level of a baseline)
  kind: "erg" | "baseline";
  startedAt: number;
  endedAt?: number;
  counts: Map<string, number>;
  total: number;
  turns?: number;                 // game turns the run saw (a baseline's own count)
  game?: string;                  // the game it ran in (its Melange build and launch URL's port), so runs of one game pair up
  from?: string;                  // "saved": read back from the runs file (another erg-mcp process's run)
}

/** A run as the runs file keeps it (JSON: the counts as [signature, n] pairs). */
export interface SavedRun { label: string; kind: AssertRun["kind"]; startedAt: number; endedAt?: number; counts: [string, number][]; total: number; turns?: number; game?: string; }
export const toSaved = (r: AssertRun): SavedRun => ({ label: r.label, kind: r.kind, startedAt: r.startedAt, ...(r.endedAt ? { endedAt: r.endedAt } : {}), counts: [...r.counts],
  total: r.total, ...(r.turns !== undefined ? { turns: r.turns } : {}), ...(r.game ? { game: r.game } : {}) });
export const fromSaved = (r: SavedRun): AssertRun => ({ ...r, counts: new Map(r.counts), from: "saved" });

/** The runs of memory and of the runs file together (one per label and start, memory's first), oldest first, the last
 * `keep`. Every erg-mcp process keeps its own runs in memory: the Caldera v2 round-2 tester ran the vanilla baseline in
 * one process and the Erg Test in the next, so test_asserts had no pair to compare; the runs file carries them over. */
export function mergeRuns(mem: AssertRun[], saved: SavedRun[], keep = 24): AssertRun[] {
  const key = (r: { label: string; startedAt: number }) => `${r.label}@${r.startedAt}`, seen = new Set(mem.map(key));
  return [...saved.filter((r) => !seen.has(key(r))).map(fromSaved), ...mem].sort((a, b) => a.startedAt - b.startedAt).slice(-keep);
}

/** The Erg run and the baseline to compare: the last Erg run, and the last baseline of the same game (any game when the
 * runs do not say), else none. */
export function pairOf(runs: AssertRun[]): { erg?: AssertRun; base?: AssertRun } {
  const erg = [...runs].reverse().find((r) => r.kind === "erg");
  const bases = [...runs].reverse().filter((r) => r.kind === "baseline");
  const base = bases.find((r) => !erg?.game || !r.game || r.game === erg.game) ?? undefined;
  return { erg, base };
}

/** The signature of an engine log line that is an assert or a GL error, else null. */
export function signature(msg: string): string | null {
  if (!/ASSERT\s*\(|GL ERROR/.test(msg)) return null;
  return msg.replace(/^[\s*]*XBase\s*\|\s*/, "").replace(/^\*+\s*Error\s*:\s*/, "").replace(/\s+/g, " ").trim();
}

/** Counts a line into the run; true when it was an assert or a GL error. */
export function count(run: AssertRun, msg: string): boolean {
  const sig = signature(msg);
  if (!sig) return false;
  run.counts.set(sig, (run.counts.get(sig) ?? 0) + 1);
  run.total++;
  return true;
}

export function summary(run: AssertRun, limit = 20) {
  return { label: run.label, kind: run.kind, total: run.total, signatures: run.counts.size, ...(run.turns !== undefined ? { turns: run.turns } : {}),
    seconds: Math.round(((run.endedAt ?? Date.now()) - run.startedAt) / 100) / 10,
    top: [...run.counts].sort((a, b) => b[1] - a[1]).slice(0, limit).map(([sig, n]) => ({ sig, n })) };
}

/** The Erg run against the baseline: the signatures only the Erg level shows (its own), the ones only the baseline
 * shows, and those both show (the game's own, whatever the level). Counts per minute make runs of different length
 * comparable. */
export function diff(erg: AssertRun, base: AssertRun) {
  const per = (r: AssertRun, n: number) => Math.round((n / Math.max(1 / 60, ((r.endedAt ?? Date.now()) - r.startedAt) / 60000)) * 10) / 10;
  const onlyErg: { sig: string; n: number; perMinute: number }[] = [], onlyBaseline: { sig: string; n: number; perMinute: number }[] = [];
  const both: { sig: string; erg: number; baseline: number; ergPerMinute: number; baselinePerMinute: number }[] = [];
  for (const [sig, n] of erg.counts) {
    const b = base.counts.get(sig);
    if (b === undefined) onlyErg.push({ sig, n, perMinute: per(erg, n) });
    else both.push({ sig, erg: n, baseline: b, ergPerMinute: per(erg, n), baselinePerMinute: per(base, b) });
  }
  for (const [sig, n] of base.counts) if (!erg.counts.has(sig)) onlyBaseline.push({ sig, n, perMinute: per(base, n) });
  onlyErg.sort((a, b) => b.n - a.n);
  onlyBaseline.sort((a, b) => b.n - a.n);
  both.sort((a, b) => b.erg - a.erg);
  return {
    erg: erg.label, baseline: base.label, onlyErg, onlyBaseline, both,
    verdict: onlyErg.length
      ? `${onlyErg.length} assert or GL error signature${onlyErg.length > 1 ? "s" : ""} only in the Erg run: the level's own (${onlyErg.slice(0, 3).map((x) => x.sig).join("; ")})`
      : both.length ? "every assert and GL error of the Erg run also comes in the vanilla baseline: the game's own, not the level's" : "no asserts or GL errors in the Erg run",
  };
}
