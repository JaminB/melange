// Caldera's flood, derived from the map instead of tuned by feel. All heights in map units, the sea in world units (20 to the map).
//   Spawns (v2: four human teams, each in its own castle near the shore): every spawn's floor stays DRY_MARGIN (1.55) over the
//   water at the start of its worm's first 3 own turns. The engine's rotation: the teams take turns from team 0 and each moves its
//   worms in turn, so worm p of team t has its own turn n in game turn t + 1 + teams * p + teams * wormsPerTeam * (n - 1); a round
//   is `teams` game turns (wum.sim's round: it goes up once every team has had a turn), so the sea at that round is at most
//   (floor - 1.55) * 20 for that worm. The sea holds under that until the last of those rounds.
//   The shore castles: the sea reaches their highest wall-walk + 1 by ROUND_SHORE (10).
//   F = the highest castle wall-walk: the sea reaches F + 1 by ROUND_F (15): every castle is under by then.
//   the cap = the crater floor less CAP_BELOW (6), from ROUND_CAP: the mine factory explodes when the water is more than 10 world
//   over it, and the launchers stand on the crater's walls over the floor.
// Between those the level is linear in the round, and the round is what script.lua reads, so every machine floods alike. The
// announcements are built from the same numbers: one a round at most, in player terms (the castles' names, rounds until the
// shore castles drown), each at most MSG_MAX characters (v2 round 5: the game's banner is one line, and round 4's 120-character
// turn-1 text was cut off at about 60).
export const MSG_MAX = 46;
export const ROUND_START = 3, ROUND_SHORE = 10, ROUND_F = 15, ROUND_CAP = 16, CAP_BELOW = 6, DRY_MARGIN = 1.55, OWN_TURNS = 3;
// the round of a worm's n-th own turn (worm p of team t)
export const ownTurnRound = (team, place, n, teams, wormsPerTeam) => Math.ceil((team + 1 + teams * place + teams * wormsPerTeam * (n - 1)) / teams);

// spawns: [{worm, team, place, y}] (place: the worm's place in its team; only the worms that play, place < wormsPerTeam, count)
// castles: [{name, top (wall-walk), shore}]
export function schedule({ startWorld, spawns, teams, wormsPerTeam, F, craterFloor, castles }) {
  const playing = spawns.filter((sp) => sp.place < wormsPerTeam);
  const lowest = Math.min(...playing.map((sp) => sp.y));
  const cap = Math.round((craterFloor - CAP_BELOW) * 20);
  const due = playing.map((sp) => ({ worm: sp.worm, r: ownTurnRound(sp.team, sp.place, OWN_TURNS, teams, wormsPerTeam), c: Math.floor((sp.y - DRY_MARGIN) * 20 / 5) * 5 }));
  const dryRound = Math.max(...due.map((d) => d.r)), dryCap = Math.min(...due.map((d) => d.c));
  const shoreTop = Math.max(...castles.filter((c) => c.shore).map((c) => c.top));
  const pts = [[ROUND_START, startWorld], [dryRound, Math.max(startWorld, dryCap)], [ROUND_SHORE, Math.round((shoreTop + 1) * 20)], [ROUND_F, Math.round((F + 1) * 20)], [ROUND_CAP, cap]];
  const level = (r) => {
    if (r <= pts[0][0]) return startWorld;
    for (let i = 1; i < pts.length; i++) if (r <= pts[i][0]) { const [r0, w0] = pts[i - 1], [r1, w1] = pts[i]; return Math.min(cap, Math.floor((w0 + (w1 - w0) * (r - r0) / (r1 - r0)) / 5) * 5); }
    return cap;
  };
  const levels = [];
  for (let r = 1; r <= ROUND_CAP + 1; r++) levels.push(level(r));
  const reach = (topMap) => levels.findIndex((w) => w >= topMap * 20) + 1;      // first round whose level covers a wall-walk
  const msg = {};
  const add = (r, t) => { if (r >= 1) (msg[r] ??= []).push(t); };
  const and = (l) => (l.length > 1 ? l.slice(0, -1).join(", ") + " and " + l[l.length - 1] : l[0]);
  const rise = levels.findIndex((w) => w > startWorld) + 1;
  add(1, "Mines erupt! The sea rises in " + (rise - 1) + " rounds.");
  add(rise - 1, "The sea rises next round!");
  add(rise, "The sea is rising. Climb the switchbacks!");
  const byRound = {};
  for (const c of castles) (byRound[reach(c.top)] ??= []).push(c.name);
  // the shore castles together as "the shore castles" (four names do not fit on the banner)
  const shore = castles.filter((c) => c.shore), shoreRound = Math.max(...shore.map((c) => reach(c.top)));
  const nameOf = (names) => (names.length > 2 && names.every((n) => shore.some((c) => c.name === n)) ? "The shore castles" : and(names));
  for (const [r, names] of Object.entries(byRound)) {
    const who = nameOf(names), many = names.length > 1;
    add(+r - 1, who + (many ? " flood next round!" : " floods next round!"));
    add(+r, "The sea reaches " + (who === "The shore castles" ? "the shore castles" : who) + (many ? "' walls." : "'s walls."));
  }
  for (const n of [3, 2]) add(shoreRound - n, n + " rounds until the shore castles drown!");
  add(levels.findIndex((w) => w >= cap) + 1, "The sea has stopped rising. Hold the volcano!");
  const messages = {};
  for (const [r, l] of Object.entries(msg)) {
    // the countdown and the 'floods next' go together while they fit on the banner, else the first alone
    let t = l.join(" ");
    if (t.length > MSG_MAX) t = l[0];
    if (t.length > MSG_MAX) throw new Error(`announcement for round ${r} is ${t.length} characters (at most ${MSG_MAX}): ${t}`);
    messages[r] = t;
  }
  return { levels, messages, cap, S: lowest, F, dryRound, dryCap, caps: due, reach: Object.fromEntries(castles.map((c) => [c.name, reach(c.top)])), shoreRound, teams, wormsPerTeam };
}

export function luaOf(sch) {
  const q = (t) => JSON.stringify(t);
  const lv = sch.levels.map((w, i) => `[${i + 1}] = ${w}`).join(", ");
  const ms = Object.entries(sch.messages).map(([r, t]) => `[${r}] = ${q(t)}`).join(",\n  ");
  return `-- Caldera: the sea rises toward the volcano, and the factory in the crater is the only source of mines.
-- GENERATED by build.mjs (schedule.mjs) from the map: do not tune by hand.
--
-- The sea is set once at the first turn start of each ROUND (wum.sim's own counter: it only goes up when a team starts a turn it
-- already had in the round, so a wiped-out team shortens the round and the sea still rises). The levels are WORLD units (20 to the
-- map unit): ${sch.levels[0]} at the start; every spawn dry (1.55 map under its floor) through its worm's 3rd own turn (${sch.teams} teams of
-- ${sch.wormsPerTeam}: round ${sch.dryRound}, at most ${sch.dryCap}); the shore castles' wall-walks + 1 map by round ${ROUND_SHORE}; F+1 = the highest castle
-- wall-walk + 1 map = ${sch.levels[ROUND_F - 1]} by round ${ROUND_F}; the cap ${sch.cap} (6 map under the crater floor, the factory floor) from round ${ROUND_CAP}.
-- Lowest spawn S = ${sch.S} map, highest wall-walk F = ${sch.F} map. Each message is shown once, at the first turn of its round.

local LEVEL = { ${lv} }
local CAP = ${sch.cap}
local MSG = {
  ${ms}
}
local lastRound = 0

wum.events.on("sim.turnStarted", function(name, turn, round)
  if round == nil or round < 1 then round = math.floor((turn + ${sch.teams - 1}) / ${sch.teams}) end
  if round == lastRound then return end
  lastRound = round
  local w = LEVEL[round] or CAP
  if w > CAP then w = CAP end
  local cur = wum.sim.getData("Water.Level") or 0
  if w > cur then wum.sim.setData("Water.Level", w) end
  local text = MSG[round]
  if text then
    if wum.sim.announce then wum.sim.announce(text, 8) end
    wum.log.info("caldera round " .. round .. " sea " .. w .. ": " .. text)
  end
end)
`;
}
