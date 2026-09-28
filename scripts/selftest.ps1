# Builds and runs the offline self-tests (no game needed). Usage: .\scripts\selftest.ps1 [-Config x86-release]
# Exit code 0 = all passed.
param([string]$Config = "x86-release")
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot
Push-Location $repo
try {
    # Dot-sourced so the x86 developer environment stays set for the extra targets.
    . "$repo\build.ps1" -Config $Config
    $tests = @("bus_selftest", "jlog_selftest", "trace_selftest", "gldebug_selftest", "shaders_selftest", "postfx_selftest",
        "draw_queue_selftest", "json_read_selftest", "lua54_selftest", "thumper_selftest", "sandbox_selftest", "sim_selftest", "console_selftest", "handshake_selftest", "tweak_selftest")
    cmake --build --preset $Config --target $tests
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    foreach ($t in $tests) {
        & "$repo\build\$Config\$t.exe"
        if ($LASTEXITCODE) { Write-Host "$t FAILED"; exit $LASTEXITCODE }
    }
    exit 0
} finally {
    Pop-Location
}
