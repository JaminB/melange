// Whose turn it is: the engine's worm rotation for the flood checks (check_flood, check_reachability's flood,
// place_spawn_knots). Pure.
//
// Turn numbers are game turns counted from 1: game turn n is the n-th GameLogic.Turn.Started of the match, the `turn` of
// wum.sim.turnStarted and of wum.sim.turn(), game_state's `turn` and a waterSchedule's startTurn (its step is applied at
// the start of that turn, before sim.turnStarted runs). Seen in the Caldera iteration-7 Test (2026-11-08, Quick Game, two
// teams of four): turn 1 the human team's WORM0 (67 s: the turn timed out), then 1/WORM4, 0/WORM1, 1/WORM5, 0/WORM2,
// 1/WORM6, 0/WORM3, 1/WORM7: the teams alternate from team 0 and each moves its worms in turn. A monitor started in turn
// 2 reads it as "1/4, 0/1, 1/5, ...", which is the same rotation with turn 1 missed. A dead worm is skipped, so its
// team's others move sooner; a team with no worm left is skipped.

/** WORMn: worm n % perTeam of team floor(n / perTeam) (the Quick Game's human team has WORM0-3). */
export const slotOf = (n: number, perTeam: number, teams: number) => ({ team: Math.floor(n / perTeam) % teams, index: n % perTeam });

export interface RotationArgs {
  worms: number[];                 // the worm slots in the match (WORMn knots), ascending
  teams: number;
  perTeam: number;
  horizon: number;                 // game turns to play out
  order?: number[];                // an observed order of worm slots from game turn 1, repeated (taken as it is)
  drownsAt?: (worm: number) => number | null;   // the first game turn the worm is dead at its start (its ground under the water)
}

/** The worm that moves in each game turn 1..horizon (index 0 is turn 1; -1: no worm left), and each worm's own turns. */
export function rotation(a: RotationArgs): { byTurn: number[]; own: Map<number, number[]> } {
  const own = new Map<number, number[]>(a.worms.map((w) => [w, []]));
  const byTurn: number[] = [];
  if (a.order?.length) {
    for (let t = 1; t <= a.horizon; t++) {
      const w = a.order[(t - 1) % a.order.length];
      byTurn.push(w);
      own.get(w)?.push(t);
    }
    return { byTurn, own };
  }
  const dead = (w: number, t: number) => { const d = a.drownsAt?.(w); return d !== null && d !== undefined && t >= d; };
  const byTeam: number[][] = Array.from({ length: a.teams }, () => []);
  for (const w of a.worms) byTeam[slotOf(w, a.perTeam, a.teams).team].push(w);
  for (const l of byTeam) l.sort((x, y) => x - y);
  const next = new Array<number>(a.teams).fill(0);   // per team: the position in its list of the worm it moves next
  let team = 0;
  for (let t = 1; t <= a.horizon; t++) {
    let mover = -1;
    for (let k = 0; k < a.teams && mover < 0; k++) {
      const tm = (team + k) % a.teams, list = byTeam[tm];
      for (let j = 0; j < list.length && mover < 0; j++) {
        const pos = (next[tm] + j) % list.length;
        if (!dead(list[pos], t)) { mover = list[pos]; next[tm] = pos + 1; team = tm + 1; }
      }
    }
    byTurn.push(mover);
    if (mover >= 0) own.get(mover)!.push(t);
  }
  return { byTurn, own };
}
