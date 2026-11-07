-- Caldera: the sea rises toward the volcano. From the third turn the water climbs a step at the start of every turn,
-- announced on screen, until it stands just under the crater: the shore castles drown first, then the shelves, and
-- only the upper cone stays dry. Everything runs off the sim's turns and ticks, so every machine floods alike.

CALDERA = { base = nil, level = nil, target = nil, rises = 0 }

local FIRST_TURN = 3      -- the first turn the sea rises on
local STEP = 64           -- world units per turn
local CAP = 680           -- world units: the highest the sea goes (the crater floor is at 770)
local PER_TICK = 2        -- world units per tick while the sea is moving (a step takes about 0.6 s)

local function announce(text)
  wum.sim.setData("CommentaryPanel.Comment", text)
  wum.sim.send("CommentaryPanel.TimedText")
  wum.log.info("caldera: " .. text)
end

wum.sim.after(1, function()
  CALDERA.base = wum.sim.getData("Water.Level") or -20
  CALDERA.level = CALDERA.base
  CALDERA.target = CALDERA.base
end)

wum.events.on("sim.turnStarted", function(name, turn)
  if not CALDERA.level or turn < FIRST_TURN or CALDERA.target >= CAP then return end
  CALDERA.rises = CALDERA.rises + 1
  CALDERA.target = math.min(CAP, CALDERA.base + STEP * CALDERA.rises)
  if CALDERA.rises == 1 then
    announce("The volcano stirs and the sea is rising! Climb!")
  elseif CALDERA.target >= CAP then
    announce("The sea has reached the crater. Only the summit is safe.")
  else
    announce("The sea rises again!")
  end
end)

wum.sim.every(1, function()
  if not CALDERA.level or CALDERA.level >= CALDERA.target then return end
  CALDERA.level = math.min(CALDERA.target, CALDERA.level + PER_TICK)
  wum.sim.setData("Water.Level", CALDERA.level)
end)
