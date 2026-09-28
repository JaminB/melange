-- hello-spice: a small tour of the client API (docs/lua-api.md). Edit this file while the game runs to see hot reload.
local color = "#ffd166"
local turn = 0
local frames = wum.storage.get("frames") or 0

local function describe(t)
  local parts = {}
  for k, v in pairs(t) do
    if type(v) == "table" then v = "{" .. table.concat(v, ", ") .. "}" end
    parts[#parts + 1] = k .. "=" .. tostring(v)
  end
  table.sort(parts)
  return "{" .. table.concat(parts, ", ") .. "}"
end

wum.events.on("GameLogic.Turn.Started", function(payload, name)
  turn = turn + 1
  wum.mod.keep.turn = turn
  wum.log.info(name, "turn", turn, describe(payload))
end)

wum.events.on("melange.match.start", function()
  turn = 0
end)

wum.events.on("melange.frame", function()
  frames = frames + 1
end)

wum.timers.every(1, function()
  wum.storage.set("frames", frames)
end)

-- Oasis: a channel the web panel below subscribes to, and the panel itself (web/index.html).
local ticks = wum.web and wum.web.channel("ticks")
if wum.web then
  wum.web.panel({ title = "Hello Spice", entry = "web/index.html" })
  wum.timers.every(1, function()
    ticks:publish({ turn = turn, frames = frames })
  end)
end

wum.draw.on("hud", function()
  if not wum.game.inMatch() or not wum.config.get("showHud") then return end
  wum.draw.hudRect(16, 280, 236, 324, {0, 0, 0, 0.6}, true)
  wum.draw.hudRect(16, 280, 236, 324, color, false, 2)
  wum.draw.hudText(28, 292, "hello-spice: turn " .. turn, color, 18)
end)

wum.draw.on("world", function()
  local cam = wum.render.camera()
  if not cam or not wum.game.inMatch() then return end
  local d = 150
  local p = {cam.pos.x + cam.fwd.x * d, cam.pos.y + cam.fwd.y * d, cam.pos.z + cam.fwd.z * d}
  wum.draw.text(p, wum.config.get("label"), color, 20)
end)

wum.ui.panel("main", "Hello Spice", function()
  local show, changed = wum.ui.checkbox("Show the HUD widget", wum.config.get("showHud"))
  if changed then wum.config.set("showHud", show) end
  wum.ui.text("Scene: " .. wum.game.scene())
  wum.ui.text("Turn: " .. turn)
  wum.ui.text("Frames counted (kept in wum.storage): " .. frames)
end)

wum.mod.onReload = function(prev)
  turn = prev and prev.turn or 0
  wum.mod.keep.turn = turn
  wum.log.info("hello-spice reloaded at turn", turn)
end

wum.log.info("hello-spice loaded; frames so far:", frames)
