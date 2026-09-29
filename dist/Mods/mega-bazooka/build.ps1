# Rebuilds this mod's assets/ from its src/ sources. Usage: .\build.ps1
#
# Today this only regenerates the panel icon and HUD icon (src/gen-assets.ps1), both original art with no
# game bytes in them. The clone ships with no custom mesh: C0 (docs/m5-design.md §11.3, private) found that
# LoadBank cannot carry an XMeshDescriptor, so Component D's mesh/glTF converter has nothing to build yet for
# this mod. Once D ships `xomtool` and a bundle-mesh loader lands, this script also builds
# assets/data/megabazooka.xom from a src/*.gltf source and this file gains that step.
param()
$ErrorActionPreference = "Stop"
& (Join-Path $PSScriptRoot "src\gen-assets.ps1")
