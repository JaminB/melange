-- erg-mcp set_water_look: begin (generated: set_water_look rewrites this block)
if wum.level and wum.level.water then
  local ok, why = wum.level.water{LightDiffuse = {255, 244, 225}, NearOpacity = 1, ReflectionStrength = 0, SpecularContrast = 0, SubtractColourScale = -1.5}
  if not ok then wum.log.warn("water look: " .. tostring(why)) end
end
-- erg-mcp set_water_look: end
