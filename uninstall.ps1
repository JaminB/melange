# Removes Melange from the game folder. Logs and dumps in <game>\Melange are kept unless -Purge, which also deletes the
# media pack (Documents\Melange\media) and the media helper's cache (AppData\LocalLow\Melange\media-cache).
# Usage: .\uninstall.ps1 [-GameDir <path>] [-Purge]
param([string]$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\WormsXHD", [switch]$Purge)
$ErrorActionPreference = "Stop"
if (-not (Test-Path "$GameDir\WormsMayhem.exe")) { throw "WormsMayhem.exe not found in $GameDir (pass -GameDir)" }
if (Get-Process WormsMayhem -ErrorAction SilentlyContinue) { throw "Close the game first" }
foreach ($f in "melange.asi", "Melange.pdb", "Melange.ini") {
    Remove-Item "$GameDir\$f" -ErrorAction SilentlyContinue
}
$d8 = "$GameDir\dinput8.dll"
if (Test-Path "$d8.melange-backup") {
    Move-Item "$d8.melange-backup" $d8 -Force
    Write-Host "Restored original dinput8.dll"
} elseif (-not (Get-ChildItem $GameDir -Filter *.asi)) {
    # Only remove the loader when no other .asi mods need it.
    Remove-Item $d8 -ErrorAction SilentlyContinue
}
if ($Purge) {
    Remove-Item "$GameDir\Melange" -Recurse -ErrorAction SilentlyContinue
    $docs = [Environment]::GetFolderPath("MyDocuments")
    if ($docs -and (Test-Path "$docs\Melange\media")) {
        Remove-Item "$docs\Melange\media" -Recurse -Force -ErrorAction SilentlyContinue
        Write-Host "Removed the media pack ($docs\Melange\media)"
    }
    $low = Join-Path $env:USERPROFILE "AppData\LocalLow\Melange\media-cache"
    if (Test-Path $low) { Remove-Item $low -Recurse -Force -ErrorAction SilentlyContinue }
}
Write-Host "Melange removed"
