# Oasis web tests with the portable toolchain.
#   .\scripts\web\test.ps1                     unit tests (node --test on the compiled web\test\unit\*.test.ts)
#   .\scripts\web\test.ps1 -Url <launch url>   ...then the e2e suite in headless Edge against a running server
#   [-Reconnect]                               e2e also expects the server to drop the client once (the caller kicks it)
param([string]$Url = "", [switch]$Reconnect, [switch]$NoUnit)
$ErrorActionPreference = "Stop"
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$web = Join-Path $root "web"
$node = Join-Path $root "tools\node\node.exe"
$esbuild = Join-Path $root "tools\esbuild\esbuild.exe"
$fail = 0

if (-not $NoUnit) {
    $out = Join-Path $web "test\out"
    if (Test-Path $out) { Remove-Item -Recurse -Force $out }
    $tests = @(Get-ChildItem (Join-Path $web "test\unit") -Filter *.test.ts | ForEach-Object { $_.FullName })
    & $esbuild @tests --bundle --platform=node --format=esm --target=node20 --jsx=automatic --jsx-import-source=preact `
        --outdir="$out" --out-extension:.js=.mjs --log-level=warning
    if ($LASTEXITCODE) { throw "esbuild (unit tests) failed" }
    & $node --test (Get-ChildItem $out -Filter *.mjs | ForEach-Object { $_.FullName })
    if ($LASTEXITCODE) { $fail++ }
}

if ($Url) {
    # Edge processes started by this run are closed afterwards even if the suite dies; nothing else is touched.
    $before = @(Get-Process msedge -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
    $args_ = @((Join-Path $web "test\e2e\smoke.mjs"), $Url)
    if ($Reconnect) { $args_ += "--reconnect" }
    try {
        & $node @args_
        if ($LASTEXITCODE) { $fail++ }
    } finally {
        Start-Sleep -Milliseconds 500
        Get-Process msedge -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.Id } | ForEach-Object {
            $cl = (Get-CimInstance Win32_Process -Filter "ProcessId=$($_.Id)" -ErrorAction SilentlyContinue).CommandLine
            if ($cl -match "playwright") { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
        }
    }
}
if ($fail) { Write-Host "web tests: FAILED"; exit 1 }
Write-Host "web tests: passed"
