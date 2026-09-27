# Builds and runs the offline self-tests (no game needed). Usage: .\scripts\selftest.ps1 [-Config x86-release]
# Currently: bus_selftest (event bus, docs/m0-design.md §3.B). Exit code 0 = all passed.
param([string]$Config = "x86-release")
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot
Push-Location $repo
try {
    # build.ps1 imports the x86 developer environment and builds the plugin; dot-source it so the environment
    # stays set for the extra target below.
    . "$repo\build.ps1" -Config $Config
    cmake --build --preset $Config --target bus_selftest
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    & "$repo\build\$Config\bus_selftest.exe"
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
