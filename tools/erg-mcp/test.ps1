# erg-mcp tests with the portable Node: builds the bundle, then runs test\*.test.mjs (the MCP protocol, and the project
# tools against web\test\e2e\mock-server.mjs) and test\*.test.ts (bundled to test\out first: the voxel helpers).
#   .\tools\erg-mcp\test.ps1 [-SkipBuild]
param([switch]$SkipBuild)
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$root = Split-Path (Split-Path $here -Parent) -Parent
$node = Join-Path $root "tools\node\node.exe"
$esbuild = Join-Path $root "tools\esbuild\esbuild.exe"
if (-not $SkipBuild) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here "build.ps1")
    if ($LASTEXITCODE) { throw "erg-mcp build failed" }
}
# a mesh bank a test or a tool call with a relative `out` leaves under tools\erg-mcp\out\meshes is removed afterwards,
# unless the folder was there before this run (then it is somebody's own)
$strayMeshes = Join-Path $here "out\meshes"
$hadMeshes = Test-Path $strayMeshes
$out = Join-Path $here "test\out"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
$ts = @(Get-ChildItem (Join-Path $here "test") -Filter *.test.ts | ForEach-Object { $_.FullName })
if ($ts.Count) {
    & $esbuild @ts --bundle --platform=node --format=esm --target=node22 --outdir="$out" --out-extension:.js=.mjs --log-level=warning
    if ($LASTEXITCODE) { throw "esbuild (erg-mcp tests) failed" }
}
$files = @(Get-ChildItem (Join-Path $here "test") -Filter *.test.mjs | ForEach-Object { $_.FullName })
if (Test-Path $out) { $files += @(Get-ChildItem $out -Filter *.mjs | ForEach-Object { $_.FullName }) }
& $node --test --test-concurrency=1 @files
$failed = $LASTEXITCODE
if (-not $hadMeshes -and (Test-Path $strayMeshes)) {
    Remove-Item -Recurse -Force $strayMeshes
    $parent = Split-Path $strayMeshes -Parent
    if (-not (Get-ChildItem $parent -Force)) { Remove-Item -Force $parent }
}
if ($failed) { exit 1 }
