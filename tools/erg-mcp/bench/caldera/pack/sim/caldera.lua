-- Caldera: the sea rises toward the volcano from round 4, and the crater's mine factory is the only source of mines.
--
-- wum.sim.waterSchedule steps the water at every turn start (two teams: two turns a round): the first rise is turn 7, the
-- first turn of round 4, and the sea climbs STEP world units a turn until it stands CAP at turn 21. The shore castles (floors
-- 4, wall-walks about 6 higher) go under around turns 11-12, the lower treads' (floor 19-20, walls to 26) around 17-18, and the
-- two high castles (37-41) and the cone above 41 map units (the black ash) stay dry. Everything is the sim's own turn counter,
-- so every machine floods alike. The mines come from the factory on the crater's dais (level settings: target 16, no idle
-- turns, no start mines), so the first ones appear on the first turns.

local FIRST = 7           -- the turn the sea first rises on (round 4 with two teams)
local STEP = 56           -- world units a turn (2.8 map units)
local CAP = 820           -- world units: the sea's top, 41 map units; the crater floor is at 920
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

wum.sim.waterSchedule{ startTurn = 7, step = 56, cap = 820, units = "world" }   -- FIRST, STEP and CAP, as literals (check_flood reads this call)

wum.events.on("sim.turnStarted", function(name, turn)
  local water = wum.sim.getData("Water.Level") or 0
  if turn == 1 then
    announce("warn", "The volcano rumbles... mines spill from the crater, and the sea will rise in round 4. Climb the mountain!")
  elseif turn == FIRST then
    announce("rise", "The sea is rising! Head for the crater!")
  elseif water >= CAP then
    announce("cap", "The sea has reached its height. Hold the mountain!")
  elseif turn > FIRST and math.mod(turn - FIRST, 4) == 0 then
    announce("again" .. turn, "The sea keeps rising...")
  end
end)
