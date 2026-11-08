-- Caldera: the sea rises toward the volcano from round 3, and the crater's mine factory is the only source of mines.
--
-- wum.sim.waterSchedule steps the water at every turn start (two teams: two turns a round): the first rise is turn 5, the
-- first turn of round 3, and the sea climbs STEP world units a turn until it stands CAP at turn 16. The shore castles (floors
-- 11-13, wall-walks about 6 higher) go under around turns 9-10, the middle ones in the low teens, the highest (34) at the end;
-- only the upper cone (above 37 map units, the dark ash) stays dry. Everything is the sim's own turn counter, so every machine
-- floods alike. The mines come from the factory in the crater (level settings: target 12, one idle turn, no start mines).

local FIRST = 5           -- the turn the sea first rises on (round 3 with two teams)
local STEP = 62           -- world units a turn (3.1 map units)
local CAP = 740           -- world units: the sea's top, 37 map units; the crater floor is at 920
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

wum.sim.waterSchedule{ startTurn = 5, step = 62, cap = 740, units = "world" }   -- FIRST, STEP and CAP, as literals (check_flood reads this call)

wum.events.on("sim.turnStarted", function(name, turn)
  local water = wum.sim.getData("Water.Level") or 0
  if turn == 1 then
    announce("warn", "The volcano rumbles... the sea will rise in round 3. Climb the mountain!")
  elseif turn == FIRST then
    announce("rise", "The sea is rising! Head for the crater!")
  elseif water >= CAP then
    announce("cap", "The sea has reached its height. Hold the mountain!")
  elseif turn > FIRST and math.mod(turn - FIRST, 4) == 0 then
    announce("again" .. turn, "The sea keeps rising...")
  end
end)
