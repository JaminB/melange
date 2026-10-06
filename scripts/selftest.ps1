# Builds and runs the offline self-tests (no game needed). Usage: .\scripts\selftest.ps1 [-Config x86-release]
# Exit code 0 = all passed.
param([string]$Config = "x86-release")
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot
Push-Location $repo
try {
    # Dot-sourced so the x86 developer environment stays set for the extra targets.
    . "$repo\build.ps1" -Config $Config
    if ($LASTEXITCODE) { Write-Host "build FAILED"; exit $LASTEXITCODE }
    if (-not (Select-String -Quiet -SimpleMatch "OASIS_WEB_BUNDLED:INTERNAL=ON" "$repo\build\$Config\CMakeCache.txt")) {
        Write-Host "the Oasis web app was not built (placeholder page embedded): run scripts\web\fetch.ps1, and keep MELANGE_WEB on"
        exit 1
    }
    $tests = @("bus_selftest", "jlog_selftest", "trace_selftest", "gldebug_selftest", "shaders_selftest", "postfx_selftest",
        "draw_queue_selftest", "json_read_selftest", "lua54_selftest", "thumper_selftest", "textures_selftest", "store_selftest", "sandbox_selftest", "sim_selftest", "console_selftest", "handshake_selftest", "tweak_selftest", "crashfix_selftest", "oasis_core_selftest", "oasis_auth_selftest", "oasis_streams_selftest", "gamestate_selftest", "oasis_rpc_selftest", "oasis_standalone_selftest", "wormsign_format_selftest", "wormsign_contrib_selftest", "wormsign_recorder_selftest", "log_export_selftest", "wormsign_detector_selftest", "wormsign_player_selftest", "weapons_manifest_selftest", "xom_convert_selftest", "weapons_registry_selftest", "weapons_behaviour_selftest", "assets_selftest", "mega_bazooka_selftest", "erg_scene_selftest", "erg_level_selftest", "erg_preview_selftest", "levels_selftest", "erg_pack_selftest", "erg_voxels_selftest", "launcher_selftest", "import_selftest", "display_selftest")
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
