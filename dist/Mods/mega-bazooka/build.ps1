# Rebuilds this mod's assets/ from its src/ sources. Usage: .\build.ps1
#
# Regenerates the panel icon and HUD icon (src/gen-assets.ps1), both original art with no game bytes in them.
# The clone uses a vanilla mesh: the engine's bank loader cannot carry meshes yet, so there is no mesh to build.
param()
$ErrorActionPreference = "Stop"
& (Join-Path $PSScriptRoot "src\gen-assets.ps1")
