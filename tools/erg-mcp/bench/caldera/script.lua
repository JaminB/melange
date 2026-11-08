-- Caldera: the sea rises toward the volcano, once per ROUND (every team has moved) rather than per turn. A round before
-- the first rise a message warns of it; from the third round the water climbs STEP world units at the start of each
-- round, announced on every screen, until it stands CAP: the shore castles drown first, then the shelves, and only the
-- upper cone stays dry. Everything runs off the sim's turns and ticks, so every machine floods alike.

CALDERA = { teams = 0, base = nil, level = nil, target = nil, rises = 0 }

local FIRST_ROUND = 3     -- the first round the sea rises on
local STEP = 36           -- world units per round (the first rise leaves the lowest castle floor, 180, dry for six rounds)
local CAP = 440           -- world units: the highest the sea goes (the crater floor is at 840)
local PER_TICK = 1        -- world units per tick while the sea moves (a step takes under a second)

local function announce(text)
  if wum.sim.announce then
    wum.sim.announce(text, 6)
  else
    wum.sim.setData("CommentaryPanel.Comment", text)
    wum.sim.send("CommentaryPanel.TimedText")
  end
  wum.log.info("caldera: " .. text)
end

-- How many teams there are: the distinct team slots of the worms in play (two when it cannot be read).
local function countTeams()
  local seen, n = {}, 0
  local slots = wum.sim.worms()
  if slots then
    for i = 1, table.getn(slots) do
      local w = wum.sim.worm(slots[i])
      if w and w:active() then
        local t = w:team()
        if not seen[t] then seen[t] = 1; n = n + 1 end
      end
    end
  end
  if n < 1 then n = 2 end
  return n
end

wum.events.on("sim.turnStarted", function(name, turn)
  if turn == 1 then
    CALDERA.teams = countTeams()
    CALDERA.base = wum.sim.getData("Water.Level") or -20
    CALDERA.level = CALDERA.base
    CALDERA.target = CALDERA.base
  end
  if not CALDERA.level or math.mod(turn - 1, CALDERA.teams) ~= 0 then return end
  local round = math.floor((turn - 1) / CALDERA.teams) + 1
  if round == FIRST_ROUND - 1 then
    announce("The volcano rumbles... the sea will rise next round. Head for the mountain!")
  elseif round >= FIRST_ROUND and CALDERA.target < CAP then
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
end)

wum.sim.every(1, function()
  if not CALDERA.level or CALDERA.level >= CALDERA.target then return end
  CALDERA.level = math.min(CALDERA.target, CALDERA.level + PER_TICK)
  wum.sim.setData("Water.Level", CALDERA.level)
end)
