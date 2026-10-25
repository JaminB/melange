# Builds helper\holo-media\build\<Config>\holo-media.exe (x64). Usage:
#   .\build.ps1 [-Config x64-release|x64-debug] [-MpvDir <libmpv dev folder>] [-Vcpkg <vcpkg root>] [-Torrent]
# -MpvDir: the pinned libmpv dev zip extracted (include\mpv\*.h). -Torrent needs -Vcpkg (or $env:VCPKG_ROOT) with
# libtorrent installed for x64-windows-static. Without them the exe builds with no player and no torrents.
param([string]$Config = "x64-release", [string]$MpvDir = "", [string]$Vcpkg = $env:VCPKG_ROOT, [switch]$Torrent)
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$repo = Split-Path (Split-Path $here -Parent) -Parent
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio C++ build tools not found" }
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}
# The same portable CMake/Ninja the core build prefers (tools\ of the repo).
$cmakeDir = Get-ChildItem "$repo\tools\cmake" -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
if ($cmakeDir) { $env:PATH = "$($cmakeDir.FullName)\bin;$repo\tools\ninja;$env:PATH" }
$cfgArgs = @("--preset", $Config, "-S", $here)
if ($MpvDir) { $cfgArgs += "-DHOLO_MPV_DIR=$((Resolve-Path $MpvDir).Path -replace '\\', '/')" }
if ($Torrent) {
    if (-not $Vcpkg) { throw "-Torrent needs -Vcpkg <root> or VCPKG_ROOT" }
    $cfgArgs += "-DHOLO_TORRENT=ON", "-DCMAKE_TOOLCHAIN_FILE=$(($Vcpkg -replace '\\', '/'))/scripts/buildsystems/vcpkg.cmake",
        "-DVCPKG_TARGET_TRIPLET=x64-windows-static"
}
cmake @cfgArgs
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build --preset $Config
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Write-Host "Built helper\holo-media\build\$Config\holo-media.exe"
