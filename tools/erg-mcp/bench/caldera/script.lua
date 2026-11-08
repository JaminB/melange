-- Caldera: the sea rises toward the volcano from turn 9, and the factory on the crater rim is the only source of mines.
--
-- Eight worms on two teams: one round is 8 turns (WORM0-3 team 0, WORM4-7 team 1, a worm moves in turn 1 + team + 2 * index),
-- so every worm gets its first move in turns 1-8 and its second in turns 9-16. The sea starts at 36 world units (1.8 map) over
-- the diner backdrop's ground planes, so the island stands in open sea from turn 1. All numbers below are WORLD units (20 to
-- the map unit). Phase 1 climbs 6 a turn from turn 9: 42 at turn 9, 66 at turn 13, 84 at turn 16. The shore castles' courtyards
-- (ground 60 world, 3 map) go under in turn 13 and their wall-walks (floor 80) soon after, with their worms' two moves done
-- (first movers: turns 1-2 and 9-10). Phase 2 (from turn 17) climbs 26 a turn: the lower treads' castles (floors 380-400,
-- 19-20 map) go under in turns 28-29, the sea stops at 640 world (32 map) in turn 38,
-- and the two high castles (floors 740 and 820, 37 and 41 map), the cone (the rim is about 1400) and the crater stay dry.
-- Everything is the sim's own turn counter, so every machine floods alike.

local FIRST = 9           -- the turn the sea first rises on (round 2 with eight worms)
local CAP = 640           -- world units: the sea's top, 32 map units
local TOLD = { }          -- which announcements were made

local function announce(key, text)
  if TOLD[key] then return end
  TOLD[key] = 1
  if wum.sim.announce then
    wum.sim.announce(text, 6)
  else
    wum.sim.setData("CommentaryPanel.Comment", text)
    wum.sim.send("CommentaryPanel.TimedText")
  end
  wum.log.info("caldera: " .. text)
end

-- phase 1 (this call is the one check_flood and place_spawn_knots read; phase 2 replaces it at turn 16)
wum.sim.waterSchedule{ startTurn = 9, step = 6, cap = 84, units = "world" }

wum.events.on("sim.turnStarted", function(name, turn)
  if turn == 1 then
    announce("warn", "The volcano rumbles... mines spill from the crater, and the sea rises in 8 turns. Climb the mountain!")
  elseif turn == FIRST then
    announce("rise", "The sea is rising! Head for the crater!")
  elseif turn == 16 then
    wum.sim.waterSchedule{ startTurn = 17, step = 26, cap = 640, units = "world" }
  elseif (turn > FIRST) and ((wum.sim.getData("Water.Level") or 0) >= CAP) then
    announce("cap", "The sea has reached its height. Hold the mountain!")
  elseif turn > FIRST and math.mod(turn - FIRST, 8) == 0 then
    announce("again" .. turn, "The sea keeps rising...")
  end
end)
