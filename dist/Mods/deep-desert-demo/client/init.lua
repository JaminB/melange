-- deep-desert-demo: reads game memory through wum.unsafe once the player allows it. It never writes.
local u = wum.unsafe
local info
local ok, name, known = pcall(u.build)
if ok then
  local base = u.base()
  local pe = base + u.read(base + 0x3c, "u32")
  info = string.format("%s (known build: %s), PE timestamp %08x", name, tostring(known), u.read(pe + 8, "u32"))
else
  info = name
  known = false
end
wum.log.info("deep-desert-demo:", info)

wum.ui.panel("main", "Deep Desert demo", function()
  wum.ui.text(info)
  if known then
    -- The logic RNG of build #1077 (simulation state: read, never written).
    wum.ui.text(string.format("Logic RNG: %08x", u.read(0x96d034, "u32")))
  end
end)
