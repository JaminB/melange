-- desync-probe, the sim side (Lua 5.0, the match VM). Enable it on every machine of a match: it is content, so
-- the lobby handshake needs it on all of them.
--
-- Everything a sim script does must be the same on every peer. This mod breaks that rule on purpose, and only when
-- told to: test tooling raises the local event sim.test.desync(kind) on ONE machine, standing for a mod that reacts
-- to something only that machine sees. Wormsign's desync detector then reports the first tick the two machines
-- disagree on, and its bundle's diff.txt names what changed.
--   kind 0 (env)     changes a global of this mod: the mod's state hash differs from that tick on
--   kind 1 (energy)  nothing here: sim scripts cannot change a worm's energy, test tooling does that itself
--   kind 2 (rng)     draws one extra number from this mod's random stream, so every later draw differs too

desyncs = 0
lastDraw = 0

wum.events.on("sim.test.desync", function(name, kind)
  if kind == 0 then
    desyncs = desyncs + 1
  elseif kind == 2 then
    lastDraw = wum.sim.random(1, 1000000)
  end
  wum.log.warn("desync-probe:", name, kind, "at tick", wum.sim.tick())
end)

-- One draw a second, identical on every peer while nobody raised the event.
wum.sim.every(50, function(tick)
  lastDraw = wum.sim.random(1, 1000000)
end)

wum.log.info("desync-probe", wum.mod.version, "loaded at tick", wum.sim.tick())
