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
        "draw_queue_selftest", "json_read_selftest", "lua54_selftest", "thumper_selftest", "sandbox_selftest", "sim_selftest", "console_selftest", "handshake_selftest", "tweak_selftest", "oasis_core_selftest", "oasis_auth_selftest", "oasis_streams_selftest", "gamestate_selftest", "oasis_rpc_selftest", "oasis_standalone_selftest", "wormsign_format_selftest", "wormsign_contrib_selftest", "wormsign_recorder_selftest", "wormsign_detector_selftest", "wormsign_player_selftest", "weapons_manifest_selftest", "assets_selftest")
    cmake --build --preset $Config --target $tests
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    foreach ($t in $tests) {
        # The Oasis core also gets a 60 s randomized run of mutated HTTP requests and WebSocket frames.
        $targs = if ($t -eq "oasis_core_selftest") { @("--mutate", "60") } else { @() }
        & "$repo\build\$Config\$t.exe" @targs
        if ($LASTEXITCODE) { Write-Host "$t FAILED"; exit $LASTEXITCODE }
    }
    exit 0
} finally {
    Pop-Location
}
