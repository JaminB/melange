# Installs Melange into the game folder: Ultimate ASI Loader (dinput8.dll) + melange.asi (+ ini on first install).
# Usage: .\deploy.ps1 [-GameDir <path>]
param([string]$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\WormsXHD")
$ErrorActionPreference = "Stop"
$dist = Join-Path $PSScriptRoot "dist"
if (-not (Test-Path "$GameDir\WormsMayhem.exe")) { throw "WormsMayhem.exe not found in $GameDir" }
if (Get-Process WormsMayhem -ErrorAction SilentlyContinue) { throw "Close the game first" }

# Never clobber a foreign dinput8.dll (e.g. another mod's loader) without keeping a copy.
$d8 = "$GameDir\dinput8.dll"
if ((Test-Path $d8) -and ((Get-FileHash $d8).Hash -ne (Get-FileHash "$dist\dinput8.dll").Hash)) {
    $bak = "$d8.melange-backup"
    if (-not (Test-Path $bak)) { Copy-Item $d8 $bak; Write-Host "Backed up existing dinput8.dll -> $bak" }
}
Copy-Item "$dist\dinput8.dll" $GameDir -Force
Copy-Item "$dist\melange.asi" $GameDir -Force
if (Test-Path "$dist\Melange.pdb") { Copy-Item "$dist\Melange.pdb" $GameDir -Force }
if (-not (Test-Path "$GameDir\Melange.ini")) { Copy-Item "$dist\Melange.ini" $GameDir }
Write-Host "Melange deployed to $GameDir"
