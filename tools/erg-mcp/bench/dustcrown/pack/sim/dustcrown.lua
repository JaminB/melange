-- erg-mcp set_water_look: begin (generated: set_water_look rewrites this block)
if wum.level and wum.level.water then
  local ok, why = wum.level.water{LightAmbient = {128, 125, 119}, LightDiffuse = {255, 250, 238}, NearOpacity = 1, ReflectionStrength = 0, SpecularContrast = 0, SubtractColourScale = -1.5}
  if not ok then wum.log.warn("water look: " .. tostring(why)) end
end
-- the sea material (docs/erg.md, Melange 0.15): a water shader mod draws the sea as this matte colour
if wum.level and wum.level.water then
  local ok, why = wum.level.water{Sea = {255, 250, 180, 1}, SeaKeep = {0, 0}}
  if not ok then wum.log.info("sea material: " .. tostring(why)) end
end
-- the level haze (docs/erg.md, Melange 0.15): a graphics mod hazes the distance and the horizon band toward this colour
if wum.level and wum.level.water then
  local ok, why = wum.level.water{Haze = {240, 228, 176, 1}, HazeRange = {3000, 9000}}
  if not ok then wum.log.info("level haze: " .. tostring(why)) end
end
-- erg-mcp set_water_look: end
