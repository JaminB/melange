# Regenerates assets\levels from src\sample.ergpatch.json against this machine's own install.
# Usage: .\build.ps1 [-Game <game folder>] [-XomTool <xomtool.exe>]
param([string]$Game = "", [string]$XomTool = "")
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
if (-not $XomTool) {
    $cmd = Get-Command xomtool -ErrorAction SilentlyContinue
    if ($cmd) { $XomTool = $cmd.Source }
}
if (-not $XomTool -or -not (Test-Path $XomTool)) {
    throw "xomtool.exe not found; pass -XomTool <path>, or put it on PATH (it ships at dist\tools\xomtool.exe in the melange repo)"
}
& $XomTool level build --patch (Join-Path $root "src\sample.ergpatch.json") --game $Game --out (Join-Path $root "assets\levels")
if ($LASTEXITCODE) { throw "xomtool level build failed" }
