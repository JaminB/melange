# Builds dist\melange.asi (x86). Usage: .\build.ps1 [-Config x86-release|x86-debug] [-PrivateDir <path>] [-SpotifyClientId <id>] [-Out <name>]
# -SpotifyClientId: the built-in Spotify app's Client ID (32 letters and digits; default: env MELANGE_SPOTIFY_CLIENT_ID;
# empty = none). It is always passed, so an empty value clears one an earlier build left in the CMake cache.
# -Out <name> (letters, digits, - and _): build into build\<Config>-<name> and write dist-<name>\ instead of dist\, so a
# second build (a private one for in-game tests: -PrivateDir <path> -Out private) never replaces the public
# dist\melange.asi and build\<Config> that the tests and this checkout's Melange.exe use. Without it, as before.
param([string]$Config = "x86-release", [string]$PrivateDir = "", [string]$SpotifyClientId = $env:MELANGE_SPOTIFY_CLIENT_ID, [string]$Out = "")
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
if ($Out -and $Out -notmatch '^[A-Za-z0-9_-]{1,32}$') { throw "-Out: a short name of letters, digits, - and _" }
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio C++ build tools not found" }
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars32.bat`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}
# Prefer portable CMake/Ninja in tools\ if present.
$cmakeDir = Get-ChildItem "$root\tools\cmake" -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
if ($cmakeDir) { $env:PATH = "$($cmakeDir.FullName)\bin;$root\tools\ninja;$env:PATH" }
if ($PrivateDir) { $PrivateDir = (Resolve-Path $PrivateDir).Path -replace '\\', '/' }
if ($null -eq $SpotifyClientId) { $SpotifyClientId = "" }
$SpotifyClientId = $SpotifyClientId.Trim()
$bin = if ($Out) { "$root\build\$Config-$Out" } else { "$root\build\$Config" }
$dist = if ($Out) { "$root\dist-$Out" } else { "$root\dist" }
if ($Out) {
    cmake --preset $Config -S $root -B $bin "-DMELANGE_PRIVATE_DIR=$PrivateDir" "-DMELANGE_SPOTIFY_CLIENT_ID=$SpotifyClientId"
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    cmake --build $bin
} else {
    cmake --preset $Config -S $root "-DMELANGE_PRIVATE_DIR=$PrivateDir" "-DMELANGE_SPOTIFY_CLIENT_ID=$SpotifyClientId"
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    cmake --build --preset $Config
}
if ($LASTEXITCODE) { exit $LASTEXITCODE }
New-Item -ItemType Directory -Force $dist | Out-Null
New-Item -ItemType Directory -Force "$dist\tools" | Out-Null
Copy-Item "$bin\melange.asi" "$dist\" -Force
Copy-Item "$bin\Melange.pdb" "$dist\" -Force -ErrorAction SilentlyContinue
Copy-Item "$bin\Melange.exe" "$dist\" -Force
Copy-Item "$bin\Melange.exe.pdb" "$dist\" -Force -ErrorAction SilentlyContinue
Remove-Item "$dist\oasis.exe", "$dist\oasis.pdb" -Force -ErrorAction SilentlyContinue
Copy-Item "$bin\xomtool.exe" "$dist\tools\" -Force -ErrorAction SilentlyContinue
$d = Split-Path $dist -Leaf
Write-Host "Built $d\melange.asi, $d\Melange.exe and $d\tools\xomtool.exe"
