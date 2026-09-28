-- sim-sampler, the client side (Lua 5.4, the Sandbox VM): shows the sim script's pings on the HUD.
-- It only reads engine messages; nothing here can reach the simulation.

local pings = 0

wum.events.on("Melange.Sample.Ping", function()
  pings = pings + 1
end)

wum.draw.on("hud", function()
  wum.draw.hudText(16, 64, string.format("sim-sampler: %d pings", pings))
end)
