# Removes WUMFix from the game folder (keeps logs/dumps in WUMFix\ unless -Purge).
param([string]$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\WormsXHD", [switch]$Purge)
$ErrorActionPreference = "Stop"
foreach ($f in "WUMFix.asi", "WUMFix.pdb", "WUMFix.ini") { Remove-Item "$GameDir\$f" -ErrorAction SilentlyContinue }
$d8 = "$GameDir\dinput8.dll"
if (Test-Path "$d8.wumfix-backup") { Move-Item "$d8.wumfix-backup" $d8 -Force; Write-Host "Restored original dinput8.dll" }
elseif (-not (Get-ChildItem $GameDir -Filter *.asi)) { Remove-Item $d8 -ErrorAction SilentlyContinue }  # keep loader if other .asi mods remain
if ($Purge) { Remove-Item "$GameDir\WUMFix" -Recurse -ErrorAction SilentlyContinue }
Write-Host "WUMFix removed"
