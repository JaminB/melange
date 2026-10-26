# Best-effort source mirror for the LGPL libmpv the media pack repackages (LGPL-2.1 section 6): downloads the
# tarballs of the commits media\media.lock.json names under "sources" (mpv, FFmpeg and the two build-script repos) and
# packs them with SOURCES.json (the SHA-256 of each tarball) into melange-media-src-<version>.tar.gz.
# A tarball that cannot be fetched is skipped with a warning; the script exits 1 only if none could be. media-pack.ps1
# says in SOURCES.md whether the archive exists (-SourceArchive), so a failed mirror never blocks a pack.
#   .\scripts\media-sources.ps1 -Version 1 -Out out\media
param([Parameter(Mandatory)][string]$Version, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$lock = (Get-Content (Join-Path $root "media\media.lock.json") -Raw | ConvertFrom-Json).sources
$utf8 = New-Object Text.UTF8Encoding $false
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$stage = Join-Path $Out "src-stage-$Version"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory $stage | Out-Null

$rows = @()
foreach ($a in $lock.archives) {
    if ($a.commit -notmatch '^[0-9a-f]{40}$' -or $a.repo -notmatch '^[A-Za-z0-9._-]+/[A-Za-z0-9._-]+$') { throw "bad source entry $($a.name)" }
    $name = "$($a.name)-$($a.commit.Substring(0, 10)).tar.gz"
    $file = Join-Path $stage $name
    curl.exe -fsSL --retry 3 -o $file "https://codeload.github.com/$($a.repo)/tar.gz/$($a.commit)"
    if ($LASTEXITCODE -or -not (Test-Path $file) -or (Get-Item $file).Length -lt 1024) {
        Write-Warning "could not fetch $($a.repo)@$($a.commit); it is left out of the mirror"
        Remove-Item $file -ErrorAction SilentlyContinue
        continue
    }
    $rows += [pscustomobject]@{ name = $a.name; repo = $a.repo; commit = $a.commit; file = $name
        size = (Get-Item $file).Length; sha256 = (Get-FileHash -Algorithm SHA256 $file).Hash.ToLowerInvariant() }
    Write-Host "mirrored $($a.repo)@$($a.commit.Substring(0, 10)) ($((Get-Item $file).Length) bytes)"
}
if (-not $rows) { Write-Warning "no source could be fetched"; exit 1 }
$json = ($rows | ConvertTo-Json -Depth 3)
if ($rows.Count -eq 1) { $json = "[" + $json + "]" }
[IO.File]::WriteAllText("$stage\SOURCES.json", $json + "`n", $utf8)
[IO.File]::WriteAllText("$stage\README.txt",
    "Corresponding source for libmpv-2.dll of the Melange media pack $Version (see SOURCES.md in the pack).`n" +
    "The tarballs are the exact commits named in SOURCES.json. The other libraries inside libmpv-2.dll are as published`n" +
    "upstream by the build scripts included here.`n", $utf8)
$archive = Join-Path $Out "melange-media-src-$Version.tar.gz"
if (Test-Path $archive) { Remove-Item $archive -Force }
& (Join-Path $env:SystemRoot "System32\tar.exe") -czf $archive -C $stage .
if ($LASTEXITCODE) { throw "tar failed" }
Remove-Item $stage -Recurse -Force
Write-Host "Source mirror -> $archive ($((Get-Item $archive).Length) bytes, $($rows.Count) of $($lock.archives.Count) tarballs)"
if ($env:GITHUB_OUTPUT) { "src_archive=$archive" >> $env:GITHUB_OUTPUT }
