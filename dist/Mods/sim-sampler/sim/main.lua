-- sim-sampler, the sim side. It runs in the match's Lua 5.0 VM: numbers are floats, there is no `#` operator
-- (use table.getn) and no `%` operator (use math.mod). Everything here must be deterministic: the same code runs
-- on every peer of an online match.

local s = wum.sim.storage
s.turn = 0
s.lastTick = wum.sim.tick()
s.seconds = 0

wum.events.on("GameLogic.Turn.Ended", function()
  s.turn = s.turn + 1
  local t = wum.sim.tick()
  local draw = wum.sim.random(1, 100)
  wum.log.info("turn", s.turn, "ended at tick", t, "ticks this turn", t - s.lastTick, "draw", draw)
  s.lastTick = t
  local ok, why = wum.sim.sendInt("Melange.Sample.Ping", s.turn)
  if not ok then wum.log.warn("ping not sent:", why) end
end)

wum.events.on("Weapon.Fired", function()
  wum.log.info("weapon fired at tick", wum.sim.tick())
end)

wum.events.on("Melange.Sample.Ping", function(name, turn)
  wum.log.info("ping", turn, "received at tick", wum.sim.tick())
end)

wum.sim.every(50, function(tick)
  s.seconds = s.seconds + 1
  if math.mod(s.seconds, 30) == 0 then wum.log.debug("tick", tick, "seconds", s.seconds) end
end)

wum.log.info("sim-sampler", wum.mod.version, "loaded at tick", wum.sim.tick())
