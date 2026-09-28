-- hello-spice (client-only sample): placeholder pending component B (Sandbox).
-- Intended shape, against the wum.* surface in docs/lua-api.md once it exists:
--   wum.events.on("GameLogic.Turn.Started", function(msg) ... end)
--   wum.ui.panel("hello-spice", "Hello Spice", function() ... a checkbox bound to wum.config ... end)
--   wum.draw.hudText(...), wum.timers.every(...), wum.storage.get/set(...)
wum.log.info("hello-spice loaded: " .. wum.config.get("greeting", "Hello, Worms!"))
