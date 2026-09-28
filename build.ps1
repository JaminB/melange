# Builds dist\melange.asi (x86). Usage: .\build.ps1 [-Config x86-release|x86-debug] [-PrivateDir <path>]
param([string]$Config = "x86-release", [string]$PrivateDir = "")
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
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
cmake --preset $Config -S $root "-DMELANGE_PRIVATE_DIR=$PrivateDir"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build --preset $Config
if ($LASTEXITCODE) { exit $LASTEXITCODE }
New-Item -ItemType Directory -Force "$root\dist" | Out-Null
Copy-Item "$root\build\$Config\melange.asi" "$root\dist\" -Force
Copy-Item "$root\build\$Config\Melange.pdb" "$root\dist\" -Force -ErrorAction SilentlyContinue
Write-Host "Built dist\melange.asi"
