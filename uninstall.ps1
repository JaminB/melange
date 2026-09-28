# Removes Melange from the game folder (keeps logs/dumps in Melange\ unless -Purge).
param([string]$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\WormsXHD", [switch]$Purge)
$ErrorActionPreference = "Stop"
foreach ($f in "melange.asi", "Melange.pdb", "Melange.ini") { Remove-Item "$GameDir\$f" -ErrorAction SilentlyContinue }
$d8 = "$GameDir\dinput8.dll"
if (Test-Path "$d8.melange-backup") { Move-Item "$d8.melange-backup" $d8 -Force; Write-Host "Restored original dinput8.dll" }
elseif (-not (Get-ChildItem $GameDir -Filter *.asi)) { Remove-Item $d8 -ErrorAction SilentlyContinue }  # keep loader if other .asi mods remain
if ($Purge) { Remove-Item "$GameDir\Melange" -Recurse -ErrorAction SilentlyContinue }
Write-Host "Melange removed"
