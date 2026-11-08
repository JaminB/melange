-- Caldera: the sea rises toward the volcano from turn 5, and the factory on the crater floor is the only source of mines.
--
-- Eight worms on two teams: one round is 8 turns (WORM0-3 team 0, WORM4-7 team 1, a worm moves in turn 1 + team + 2 * index),
-- so every worm gets its first move in turns 1-8 and its second in turns 9-16. The sea starts at 36 world units (1.8 map) over
-- the diner backdrop's ground planes, so the island stands in open sea from turn 1. All numbers below are WORLD units (20 to
-- the map unit). Phase 1 climbs 15 a turn from turn 5: 51 at turn 5, 96 at turn 8 (4.8 map: the shore castles' courtyards, floor
-- 3-4 map, are awash), 141 at turn 11 (7 map: their 6-high walls half under), 156 at turn 12. Phase 2 (from turn 13) climbs
-- 50 a turn: 356 at turn 16 and 406 at turn 17 flood the lower treads' castles (floors 17 and 23 map = 340 and 460), the sea stops
-- at 500 (25 map) in turn 19, and only the cone (the rim is 70 and over), the crater and the two high castles (floors 26 and 30 map) stay dry.
-- Everything is the sim's own turn counter, so every machine floods alike.

local CAP = 500           -- world units: the sea's top, 25 map units
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

-- phase 1 (this call is the one check_flood and place_spawn_knots read; phase 2 replaces it at turn 12)
wum.sim.waterSchedule{ startTurn = 5, step = 15, cap = 156, units = "world" }

wum.events.on("sim.turnStarted", function(name, turn)
  if turn == 1 then
    announce("warn", "The volcano rumbles... mines spill from the crater, and the sea will rise in 4 turns. Climb the mountain!")
  elseif turn == 5 then
    announce("rise", "The sea is rising! Head for the crater!")
  elseif turn == 9 then
    announce("shore", "The shore castles are going under...")
  elseif turn == 12 then
    wum.sim.waterSchedule{ startTurn = 13, step = 50, cap = 500, units = "world" }
  elseif turn == 13 then
    announce("race", "The sea is racing up the mountain!")
  elseif turn > 13 and ((wum.sim.getData("Water.Level") or 0) >= CAP) then
    announce("cap", "The sea has reached its height. Hold the mountain!")
  end
end)
