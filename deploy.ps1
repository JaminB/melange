# Installs Melange into the game folder: Ultimate ASI Loader (dinput8.dll), melange.asi, and Melange.ini if missing.
# Usage: .\deploy.ps1 [-GameDir <path>] [-LoaderPath <dinput8.dll>]
# Without -LoaderPath, an existing dinput8.dll in the game folder is kept; otherwise the latest x86 release of
# Ultimate ASI Loader is downloaded from GitHub.
param(
    [string]$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\WormsXHD",
    [string]$LoaderPath = ""
)
$ErrorActionPreference = "Stop"
$dist = Join-Path $PSScriptRoot "dist"
if (-not (Test-Path "$GameDir\WormsMayhem.exe")) { throw "WormsMayhem.exe not found in $GameDir (pass -GameDir)" }
if (-not (Test-Path "$dist\melange.asi")) { throw "dist\melange.asi not found; run build.ps1 first" }
if (Get-Process WormsMayhem -ErrorAction SilentlyContinue) { throw "Close the game first" }

$d8 = "$GameDir\dinput8.dll"
if ($LoaderPath) {
    if (-not (Test-Path $LoaderPath)) { throw "$LoaderPath not found" }
    # Keep a copy of a different dinput8.dll so uninstall.ps1 can restore it.
    if ((Test-Path $d8) -and ((Get-FileHash $d8).Hash -ne (Get-FileHash $LoaderPath).Hash) -and -not (Test-Path "$d8.melange-backup")) {
        Copy-Item $d8 "$d8.melange-backup"
        Write-Host "Backed up existing dinput8.dll -> dinput8.dll.melange-backup"
    }
    Copy-Item $LoaderPath $d8 -Force
} elseif (Test-Path $d8) {
    Write-Host "Keeping existing dinput8.dll (pass -LoaderPath to replace it)"
} else {
    Write-Host "Downloading Ultimate ASI Loader..."
    $rel = Invoke-RestMethod "https://api.github.com/repos/ThirteenAG/Ultimate-ASI-Loader/releases/latest"
    $asset = $rel.assets | Where-Object name -eq "Ultimate-ASI-Loader.zip" | Select-Object -First 1
    if (-not $asset) { throw "x86 loader not found in release $($rel.tag_name); download it manually and pass -LoaderPath" }
    $tmp = Join-Path ([IO.Path]::GetTempPath()) "melange-ual"
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory $tmp | Out-Null
    Invoke-WebRequest $asset.browser_download_url -OutFile "$tmp\ual.zip" -UseBasicParsing
    Expand-Archive "$tmp\ual.zip" $tmp
    Copy-Item "$tmp\dinput8.dll" $d8
    Remove-Item $tmp -Recurse -Force
    Write-Host "Installed Ultimate ASI Loader $($rel.tag_name)"
}

Copy-Item "$dist\melange.asi" $GameDir -Force
if (Test-Path "$dist\Melange.pdb") { Copy-Item "$dist\Melange.pdb" $GameDir -Force }
if (Test-Path "$dist\Mods") {
    New-Item -ItemType Directory -Force "$GameDir\Mods" | Out-Null
    Copy-Item "$dist\Mods\*" "$GameDir\Mods" -Recurse -Force
}
if (-not (Test-Path "$GameDir\Melange.ini")) { Copy-Item "$PSScriptRoot\dist\Melange.ini" $GameDir }
Write-Host "Melange deployed to $GameDir"
