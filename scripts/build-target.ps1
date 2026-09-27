# Builds one CMake target in the x86 release tree, e.g. .\scripts\build-target.ps1 xom_roundtrip
param([Parameter(Mandatory)][string]$Target, [string]$Config = "x86-release")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars32.bat`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}
$cmakeDir = Get-ChildItem "$root\tools\cmake" -Directory | Select-Object -First 1
$env:PATH = "$($cmakeDir.FullName)\bin;$root\tools\ninja;$env:PATH"
cmake --build "$root\build\$Config" --target $Target
exit $LASTEXITCODE
