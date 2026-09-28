-- bazooka-plus: multiplies the Bazooka's worm damage by 1.5x.
-- Set is only meaningful during this top-level chunk, at match Init (identical on every peer).
local bazooka = wum.sim.weapon("kWeaponBazooka")
local base = bazooka:get("WormDamageMagnitude")
local tweaked = base * 1.5
bazooka:set("WormDamageMagnitude", tweaked)
wum.log.info(string.format("bazooka-plus: WormDamageMagnitude %.1f -> %.1f", base, tweaked))
