// Running level.test and following its progress on the 'erg' channel (TestState: registering, registered, armed,
// starting, playing, ended, failed) until the Test reaches the state the caller waits for, fails, or times out.
import type { ErgSession, TimeOfDay } from "../core";

export interface TestTeam { human?: boolean; worms?: number; }

export interface TestStep { state: string; key: string; detail: string; ms: number; }
export type TestGoal = "playing" | "armed";

export interface TestRun {
  key: string; final: string; reached: boolean; timedOut: boolean; failed: string | null; states: TestStep[];
  cleanRoom?: { simMods: boolean; postfx: boolean };   // what the game applied; absent from a game without clean rooms
}

const STOPS: Record<TestGoal, ReadonlySet<string>> = {
  playing: new Set(["playing", "ended", "failed"]),
  armed: new Set(["armed", "starting", "playing", "ended", "failed"]),
};

/** Starts the Test and waits for `goal` (or failed/ended) for at most `timeoutMs`. The subscription is made before the
 * call so no early state is missed; the call's own answer is the first state. */
export async function runTest(erg: Pick<ErgSession, "test" | "onTest">,
  opts: { tod?: TimeOfDay; goal?: TestGoal; timeoutMs: number; cleanMods?: boolean; postfx?: boolean; teams?: TestTeam[] }): Promise<TestRun> {
  const goal = opts.goal ?? "playing";
  const t0 = Date.now();
  const states: TestStep[] = [];
  let wake: (() => void) | undefined;
  let key = "", answered = false;
  const add = (s: { state: string; key: string; detail: string }) => {
    // the channel's last state can be an earlier Test's end (a restart from a match): it is not this run's
    if (!answered && (s.state === "ended" || s.state === "failed")) return;
    if (s.key) key = s.key;
    const last = states[states.length - 1];
    if (last && last.state === s.state && last.detail === s.detail) return;
    states.push({ state: s.state, key: s.key, detail: s.detail, ms: Date.now() - t0 });
    wake?.();
  };
  const off = erg.onTest(add);
  try {
    const t: { tod?: TimeOfDay; cleanMods?: boolean; postfx?: boolean; teams?: TestTeam[] } = {};
    if (opts.tod) t.tod = opts.tod;
    if (opts.cleanMods !== undefined) t.cleanMods = opts.cleanMods;
    if (opts.postfx !== undefined) t.postfx = opts.postfx;
    if (opts.teams) t.teams = opts.teams;
    const r = await erg.test(Object.keys(t).length ? t : undefined);
    answered = true;
    add({ state: r.state, key: r.key, detail: "" });
    const done = () => states.some((s) => STOPS[goal].has(s.state));
    while (!done()) {
      const left = opts.timeoutMs - (Date.now() - t0);
      if (left <= 0) break;
      await new Promise<void>((resolve) => {
        const timer = setTimeout(resolve, left);
        wake = () => { clearTimeout(timer); resolve(); };
      });
    }
    const hit = states.find((s) => STOPS[goal].has(s.state));
    const bad = states.find((s) => s.state === "failed");
    return { key, final: states[states.length - 1].state, reached: !!hit && hit.state !== "failed", timedOut: !hit, failed: bad ? bad.detail || "failed" : null, states,
      ...(r.cleanRoom ? { cleanRoom: r.cleanRoom } : {}) };
  } finally {
    off();
  }
}
