-- mega-bazooka: a weapon clone sample built on wum.sim.weapons (see docs/weapons.md).
-- Lua 5.0 dialect: no "#" (use table.getn), no "%" (use math.mod).
local NAME = "kWeaponMegaBazooka"

local fires, ticks, impacts, explosions = 0, 0, 0, 0

-- Three extras around the original blast, all in the same tick (<= [Weapons] ExtraPerExplosion, default 8).
local EXTRA_OFFSETS = { {90, 0, 0}, {-45, 0, 78}, {-45, 0, -78} }

wum.sim.weapons.on("fire", NAME, function(event, name, tick)
    fires = fires + 1
    wum.log.info("fire", tick)
end)

wum.sim.weapons.on("tick", NAME, function(event, name, tick, x, y, z)
    ticks = ticks + 1
    -- x, y, z are only given once a future build finds the live payload position; this build has none (see
    -- docs/weapons.md, "What does not work yet"). Log the tick count only, never assume a position argument.
end)

wum.sim.weapons.on("impact", NAME, function(event, name, tick, message)
    impacts = impacts + 1
    wum.log.info("impact", tick, message)
end)

wum.sim.weapons.on("explosion", NAME, function(event, name, tick, x, y, z)
    explosions = explosions + 1
    wum.log.info("explosion", tick, x, y, z)
    for i = 1, table.getn(EXTRA_OFFSETS) do
        local d = EXTRA_OFFSETS[i]
        local ok, why = wum.sim.weapons.explode(d[1], d[2], d[3])
        if not ok then
            wum.log.warn("explode failed:", why)
        end
    end
end)

wum.events.on("GameLogic.Turn.Ended", function()
    wum.log.info(string.format("mega-bazooka totals: fires=%d ticks=%d impacts=%d explosions=%d", fires, ticks,
        impacts, explosions))
end)
