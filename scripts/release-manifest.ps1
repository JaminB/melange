# Writes the update manifest beside a release zip: out\melange-<version>.json, {"version","zip","sha256","size"}.
# Melange.exe's updater reads it from the GitHub release, so the zip and this file must both be attached to the
# release tagged v<version>; a release without it is never offered as an update.
# Usage: .\scripts\release-manifest.ps1 -Zip out\melange-<version>.zip -Version <version>
param([Parameter(Mandatory)][string]$Zip, [Parameter(Mandatory)][string]$Version)
$ErrorActionPreference = "Stop"
if ($Version -notmatch '^[0-9]+(\.[0-9]+){0,3}$') { throw "bad version '$Version'" }
$item = Get-Item $Zip
if ($item.Name -ne "melange-$Version.zip") { throw "the zip must be named melange-$Version.zip, not $($item.Name)" }
$hash = (Get-FileHash $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
$json = "{`"version`":`"$Version`",`"zip`":`"$($item.Name)`",`"sha256`":`"$hash`",`"size`":$($item.Length)}"
$out = Join-Path $item.DirectoryName "melange-$Version.json"
[IO.File]::WriteAllText($out, $json + "`n", (New-Object Text.UTF8Encoding $false))
Write-Host "Wrote $out"
