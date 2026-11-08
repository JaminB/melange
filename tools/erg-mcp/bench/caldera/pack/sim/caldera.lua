-- Caldera: the sea rises toward the volcano, once per ROUND, and the crater's mine factory is the only source of mines.
--
-- Rounds are counted from the sim's turn counter and the number of teams still alive at each turn (a round is one turn
-- per living team), so the schedule survives an eliminated team. The first rise is at the start of round 2 (round 1
-- carries the warning); from then the water climbs STEP world units a round until it stands CAP: the shore castles
-- drown first, then the shelves, and only the upper cone (the ash above about 37 map units) stays dry. Everything runs
-- off the sim's turns and ticks, so every machine floods alike.

CALDERA = { acc = 0, round = 1, base = nil, level = nil, target = nil, rises = 0 }

local FIRST_RISE = 2      -- the round the sea first rises on
local STEP = 110          -- world units per round (5.5 map units: the lowest castle floor, 140, goes under in round 2)
local CAP = 740           -- world units: the highest the sea goes (the crater floor is at 800, the ash band above 740)
local PER_TICK = 2        -- world units per tick while the sea moves (a step takes about a second)

local function announce(text)
  if wum.sim.announce then
    wum.sim.announce(text, 6)
  else
    wum.sim.setData("CommentaryPanel.Comment", text)
    wum.sim.send("CommentaryPanel.TimedText")
  end
  wum.log.info("caldera: " .. text)
end

-- How many teams still have a living worm (at least one when none can be read).
local function aliveTeams()
  local seen, n = {}, 0
  local slots = wum.sim.worms()
  if slots then
    for i = 1, table.getn(slots) do
      local w = wum.sim.worm(slots[i])
      if w and w:alive() then
        local t = w:team()
        if not seen[t] then seen[t] = 1; n = n + 1 end
      end
    end
  end
  if n < 1 then n = 1 end
  return n
end

-- The crater's mine factory: the game's own one fires on a slow count of turns, so the script nudges it every turn.
local function fireFactory()
  -- the factory fires when it has been sent StartMineFactory seven times since it last fired; the game's script posts one
  -- a turn, so six more here bring it round every turn
  for i = 1, 6 do
    local ok, why = wum.sim.send("GameLogic.StartMineFactory")
    if i == 1 then wum.log.info("caldera: StartMineFactory -> " .. tostring(ok) .. " " .. tostring(why)) end
  end
end

local function onRound(round)
  if round < FIRST_RISE then return end
  CALDERA.rises = CALDERA.rises + 1
  CALDERA.target = math.min(CAP, CALDERA.base + STEP * CALDERA.rises)
  if CALDERA.target >= CAP then
    announce("The sea has reached its height. Hold the mountain!")
  elseif CALDERA.rises == 1 then
    announce("The sea is rising! Climb the volcano!")
  else
    announce("The sea rises again!")
  end
end

wum.events.on("sim.turnStarted", function(name, turn)
  if turn == 1 or not CALDERA.base then
    CALDERA.base = wum.sim.getData("Water.Level") or -20
    CALDERA.level = CALDERA.base
    CALDERA.target = CALDERA.base
    CALDERA.acc = 0
    announce("The volcano rumbles... the sea will rise next round. Head for the mountain!")
    return
  end
  fireFactory()                  -- from the second turn on, so the first turn is quiet
  CALDERA.acc = CALDERA.acc + 1 / aliveTeams()
  if CALDERA.acc >= 0.999 then
    CALDERA.acc = CALDERA.acc - 1
    if CALDERA.acc < 0 then CALDERA.acc = 0 end
    CALDERA.round = CALDERA.round + 1
    onRound(CALDERA.round)
  end
end)

wum.sim.every(1, function()
  if not CALDERA.level or CALDERA.level >= CALDERA.target then return end
  CALDERA.level = math.min(CALDERA.target, CALDERA.level + PER_TICK)
  wum.sim.setData("Water.Level", CALDERA.level)
end)
