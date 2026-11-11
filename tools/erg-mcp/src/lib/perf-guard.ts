// A perf capture's lock against other games (detail layer round 9; the round-8 judge's tool gap "perf capture has no lock
// against B's game running: the measure should refuse or flag runs that overlap another testenv's game"). Two games on one
// machine share the GPU, so a frame time measured while another test folder's game runs is not this level's cost. Pure:
// the running WormsMayhem processes (pid and executable path) and the measured game's folder in, which are this folder's
// and which are others' out. tools/perfguard.ts lists the processes (Windows CIM) and wraps game_perf with it.

export interface Proc { pid: number; path: string | null }
export interface GameSplit { mine: Proc[]; others: (Proc & { testenv: string | null })[] }

const normDir = (p: string) => p.replace(/\//g, "\\").replace(/\\+$/, "").toLowerCase();

/** The folder name after "testenv\" in a path (B, C, D), or null. */
export function testenvOf(path: string | null): string | null {
  if (!path) return null;
  const m = /\\testenv\\([^\\]+)\\/i.exec(path.replace(/\//g, "\\"));
  return m ? m[1] : null;
}

/** Splits the running games into this folder's and the others' (a process whose path cannot be read counts as other). */
export function splitGames(procs: Proc[], gameDir: string): GameSplit {
  const dir = normDir(gameDir) + "\\";
  const mine: Proc[] = [], others: GameSplit["others"] = [];
  for (const p of procs) {
    if (p.path && normDir(p.path).startsWith(dir)) mine.push(p);
    else others.push({ ...p, testenv: testenvOf(p.path) });
  }
  return { mine, others };
}

/** Parses `Get-CimInstance Win32_Process | ConvertTo-Json` output (one object or an array; ProcessId, ExecutablePath). */
export function parseCim(json: string): Proc[] {
  const t = json.trim();
  if (!t) return [];
  const v = JSON.parse(t) as unknown;
  const list = Array.isArray(v) ? v : [v];
  return list.map((o) => {
    const r = o as { ProcessId?: unknown; ExecutablePath?: unknown };
    return { pid: Number(r.ProcessId), path: typeof r.ExecutablePath === "string" ? r.ExecutablePath : null };
  }).filter((p) => Number.isFinite(p.pid));
}
