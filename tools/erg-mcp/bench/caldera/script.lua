-- Caldera: the sea rises toward the volcano, and the factory in the crater is the only source of mines.
--
-- Eight worms on two teams. The turn order seen in game (sim turns): team 1 and team 0 alternate, team 1 first, team 1 in the order
-- worms 4, 5, 6, 7 and team 0 in the order 1, 2, 3, 0, so sim turns 1-8 are the movers 4, 1, 5, 2, 6, 3, 7, 0 and turns 9-16 the
-- same again (WORM0 moves in turn 8 and 16, last of its round). The sea starts at 20 world units (1 map) and does not move until
-- turn 9, after every worm has had a turn of its own. All numbers below are WORLD units (20 to the map unit).
-- Phase 1: +10 a turn from turn 9: 30 at turn 9, 100 at turn 16 (5 map: the shore castles' courtyards, floor 3-4 map, are awash, their
-- wall-walks at 9-10.5 map are dry), so every worm has had two turns before the water reaches a spawn (the spawns are on wall-walks
-- and above). Phase 2 (from turn 17): +50 a turn: 150 at turn 17, 350 at turn 21 (the lower treads' castles, floors 17 map = 340 and
-- 23 map = 460, go under between turns 21 and 23), the cap 500 (25 map) in turn 24; only the cone (the rim is 70 and over), the
-- crater and the two high castles (floors 26 and 30 map) stay dry.
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

-- phase 1 (this call is the one check_flood and place_spawn_knots read; phase 2 replaces it at turn 16)
wum.sim.waterSchedule{ startTurn = 9, step = 10, cap = 100, units = "world" }

-- the countdown: the sea's height in map units (divided by 20 for the map's unit) and the next rise, announced each turn from 9 on
local function countdown(turn)
  local w = wum.sim.getData("Water.Level") or 0
  local step = (turn < 16) and 10 or 50
  if w >= CAP then return end
  announce("t" .. turn, "Sea level " .. math.floor(w / 20) .. ", rising " .. math.floor(step / 20 * 10) / 10 .. " a turn. Climb the volcano!")
end

wum.events.on("sim.turnStarted", function(name, turn)
  if turn == 1 then
    announce("warn", "The volcano rumbles... mines spill from the crater, and the sea will rise in 8 turns. Climb the mountain!")
  elseif turn == 5 then
    announce("soon", "The sea rises in 4 turns. Head for the crater!")
  elseif turn == 9 then
    announce("rise", "The sea is rising! Head for the crater!")
  elseif turn == 13 then
    announce("shore", "The shore castles are going under...")
  elseif turn == 16 then
    wum.sim.waterSchedule{ startTurn = 17, step = 50, cap = 500, units = "world" }
  elseif turn == 17 then
    announce("race", "The sea is racing up the mountain!")
  elseif turn > 17 and ((wum.sim.getData("Water.Level") or 0) >= CAP) then
    announce("cap", "The sea has reached its height. Hold the mountain!")
  end
  if turn > 9 and turn <= 24 and turn ~= 17 then countdown(turn) end
end)
