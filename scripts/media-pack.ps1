# Assembles the Melange media pack (docs/media-helper.md, design 1.1.1): holo-media.exe, the pinned zhongfly LGPL
# libmpv-2.dll and the pinned deno.exe, LICENSES\*, SOURCES.md and pack.json, zipped as melange-media-<version>.zip.
# Everything upstream comes from media\media.lock.json and is refused unless its size and SHA-256 match (and the
# publisher's own checksum file agrees); nothing except holo-media.exe is built. CI (.github\workflows\media-pack.yml)
# builds and signs the helper first, then runs this.
#
#   .\scripts\media-pack.ps1 -Version 1 -Helper <holo-media.exe> [-Out out\media] [-Cache <download cache>]
#       [-SourceArchive melange-media-src-1.tar.gz] [-LocalMpvDir <folder with libmpv-2.dll>]
#   .\scripts\media-pack.ps1 -Dev [-Helper <holo-media.exe>] [-YtDlp <yt-dlp.exe>] [-Config x86-release]
#       A local dev pack: fetches the pinned libmpv and deno (verified), and hands them to scripts\make-dev-pack.ps1,
#       which zips them with the helper and compiles the manifest into a private build. Nothing is published.
#   -Version is the pack version (the folder under Documents\Melange\media and the tag media-v<version>).
#   .\scripts\media-pack.ps1 -MpvDevDir <folder>
#       Extracts the pinned libmpv dev archive (include\mpv\*.h and libmpv-2.dll) there, for building the helper, and exits.
#   -LocalMpvDir skips the archive and uses an already extracted libmpv-2.dll (still checked against the lock).
param([string]$Version = "", [string]$Helper = "", [string]$Out = "", [string]$Cache = "", [string]$SourceArchive = "",
      [string]$LocalMpvDir = "", [string]$MpvDevDir = "", [switch]$Dev, [string]$YtDlp = "", [string]$Config = "x86-release")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$lock = Get-Content (Join-Path $root "media\media.lock.json") -Raw | ConvertFrom-Json
if (-not $Cache) { $Cache = Join-Path $root "out\media-dl" }
New-Item -ItemType Directory -Force $Cache | Out-Null
$utf8 = New-Object Text.UTF8Encoding $false

function Get-Sha256($Path) { (Get-FileHash -Algorithm SHA256 $Path).Hash.ToLowerInvariant() }

# Downloads $Url into the cache (reusing a cached copy that still verifies) and returns the path. A mismatch is fatal.
function Get-Pinned($Url, [long]$Size, $Sha256) {
    $file = Join-Path $Cache ([IO.Path]::GetFileName(([Uri]$Url).AbsolutePath))
    if (-not ((Test-Path $file) -and (Get-Item $file).Length -eq $Size -and (Get-Sha256 $file) -eq $Sha256)) {
        curl.exe -fsSL --retry 3 -o $file $Url
        if ($LASTEXITCODE) { throw "download failed: $Url" }
    }
    $got = Get-Sha256 $file
    if ((Get-Item $file).Length -ne $Size -or $got -ne $Sha256) { throw "$Url does not match media.lock.json (size $((Get-Item $file).Length), sha256 $got)" }
    $file
}
# The publisher's own checksum must agree with the lock too, so a wrong lock edit cannot pass alone.
# zhongfly's sha256.txt: "<hash> <name>" lines. deno's .sha256sum: a "Hash : <HASH>" line.
function Assert-PublishedHash($Url, $Name, $Sha256) {
    $text = (curl.exe -fsSL --retry 3 $Url) -join "`n"
    if ($LASTEXITCODE) { throw "download failed: $Url" }
    $hit = if ($Name) { [regex]::Match($text, "(?im)^([0-9a-f]{64})\s+\*?$([regex]::Escape($Name))\s*$") }
           else { [regex]::Match($text, "(?im)^\s*Hash\s*:\s*([0-9a-f]{64})\s*$") }
    if (-not $hit.Success -or $hit.Groups[1].Value.ToLowerInvariant() -ne $Sha256) { throw "the publisher's checksum at $Url differs from media.lock.json" }
}
function Assert-Extracted($Path, $Want) {
    if ((Get-Sha256 $Path) -ne $Want) { throw "$Path does not match the sha256 in media.lock.json" }
}

# --- the pinned upstream files -----------------------------------------------------------------------------------
function Get-Libmpv([string]$All = "") {
    $want = $lock.libmpv.extract[0]
    if ($LocalMpvDir -and -not $All) {
        $dll = Join-Path $LocalMpvDir "libmpv-2.dll"
        Assert-Extracted $dll $want.sha256
        return $dll
    }
    $archive = Get-Pinned $lock.libmpv.url $lock.libmpv.size $lock.libmpv.sha256
    Assert-PublishedHash $lock.libmpv.sha256sumsUrl $lock.libmpv.asset $lock.libmpv.sha256
    $dir = Join-Path $Cache "libmpv-extract"
    if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
    if ($All) { $dir = $All }
    New-Item -ItemType Directory -Force $dir | Out-Null
    # Windows' bsdtar reads .7z; GNU tar from Git does not, so the path is explicit.
    & (Join-Path $env:SystemRoot "System32\tar.exe") -xf $archive -C $dir $want.from "include"
    if ($LASTEXITCODE) { throw "tar could not extract $archive" }
    $dll = Join-Path $dir $want.from
    Assert-Extracted $dll $want.sha256
    $dll
}
function Get-Deno {
    $want = $lock.deno.extract[0]
    $zip = Get-Pinned $lock.deno.url $lock.deno.size $lock.deno.sha256
    Assert-PublishedHash $lock.deno.sha256sumUrl "" $lock.deno.sha256
    $dir = Join-Path $Cache "deno-extract"
    if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
    Expand-Archive $zip $dir
    $exe = Join-Path $dir $want.from
    Assert-Extracted $exe $want.sha256
    $exe
}

if ($MpvDevDir) {
    New-Item -ItemType Directory -Force $MpvDevDir | Out-Null
    $null = Get-Libmpv -All (Resolve-Path $MpvDevDir).Path
    if (-not (Test-Path (Join-Path $MpvDevDir "include\mpv\client.h"))) { throw "the libmpv dev archive has no include\mpv\client.h" }
    Write-Host "libmpv dev files -> $MpvDevDir"
    exit 0
}
if ($Dev) {
    if (-not $Helper) { $Helper = Join-Path $root "helper\holo-media\build\x64-release\holo-media.exe" }
    $maker = Join-Path $PSScriptRoot "make-dev-pack.ps1"
    if (-not (Test-Path $maker)) { throw "scripts\make-dev-pack.ps1 is not in this checkout (it comes with the pack installer)" }
    $devArgs = @("-Helper", $Helper, "-Libmpv", (Get-Libmpv), "-Deno", (Get-Deno), "-Config", $Config)
    if ($YtDlp) { $devArgs += @("-YtDlp", $YtDlp) }
    & $maker @devArgs
    exit $LASTEXITCODE
}

if (-not $Version -or -not $Helper -or -not (Test-Path $Helper -PathType Leaf)) { throw "pass -Version <n> and -Helper <holo-media.exe>" }
if ($Version -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,31}$' -or $Version.EndsWith(".") -or $Version.Contains("..")) { throw "bad -Version '$Version'" }
if (-not $Out) { $Out = Join-Path $root "out\media" }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$stage = Join-Path $Out "stage-$Version"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force "$stage\LICENSES" | Out-Null

Copy-Item $Helper "$stage\holo-media.exe"
Copy-Item (Get-Libmpv) "$stage\libmpv-2.dll"
Copy-Item (Get-Deno) "$stage\deno.exe"
foreach ($f in Get-ChildItem (Join-Path $root "media\licenses") -File) { Copy-Item $f.FullName "$stage\LICENSES\$($f.Name)" }

# SOURCES.md from the template and the lock.
$src = @{}
foreach ($a in $lock.sources.archives) { $src[$a.name] = $a }
$commits = "mpv ``$($src['mpv'].commit)`` (https://github.com/$($src['mpv'].repo)), FFmpeg ``$($src['ffmpeg'].commit)`` " +
    "(https://github.com/$($src['ffmpeg'].repo)), and the build scripts zhongfly/mpv-winbuild ``$($src['zhongfly-mpv-winbuild'].commit)`` " +
    "and shinchiro/mpv-winbuild-cmake ``$($src['shinchiro-mpv-winbuild-cmake'].commit)`` (the newest commit of each before that build)."
$state = if ($SourceArchive) {
    "The source mirror is attached to the same GitHub release as this pack: **$SourceArchive**. It holds the tarballs of $commits " +
    "Its ``SOURCES.json`` lists the SHA-256 of each tarball."
} else {
    "No source mirror was attached to this release. The commits are $commits On request Melange provides the source " +
    "(open an issue at https://github.com/JaminB/melange)."
}
$tokens = @{
    PACKVERSION = $Version; DENO_URL = $lock.deno.url; DENO_VERSION = $lock.deno.version; LIBMPV_RELEASE = $lock.libmpv.release
    LIBMPV_URL = $lock.libmpv.url; LIBMPV_SHA256 = $lock.libmpv.sha256; BUILD_RUN = $lock.libmpv.buildRun; SOURCE_STATE = $state
}
$text = [IO.File]::ReadAllText((Join-Path $root "media\SOURCES.md.in"))
foreach ($k in $tokens.Keys) { $text = $text.Replace("{{$k}}", [string]$tokens[$k]) }
if ($text -match '\{\{[A-Z_]+\}\}') { throw "SOURCES.md.in has an unfilled token: $($Matches[0])" }
[IO.File]::WriteAllText("$stage\SOURCES.md", $text.Replace("`r`n", "`n"), $utf8)

# pack.json: every other file, '/' separated, in the shape scripts\gen-media-manifest.py reads.
$files = Get-ChildItem $stage -Recurse -File | ForEach-Object { [pscustomobject]@{ Rel = $_.FullName.Substring($stage.Length + 1).Replace('\', '/'); Item = $_ } } |
    Sort-Object { $_.Rel.ToLowerInvariant() }
$rows = foreach ($f in $files) {
    if ($f.Rel -eq "pack.json") { continue }
    if ($f.Rel -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}(/[A-Za-z0-9][A-Za-z0-9._-]{0,63}){0,2}$') { throw "unsafe file name in the pack: $($f.Rel)" }
    "  {`"path`": `"$($f.Rel)`", `"size`": $($f.Item.Length), `"sha256`": `"$(Get-Sha256 $f.Item.FullName)`"}"
}
$pj = "{`n `"format`": `"melange-media-pack-1`",`n `"version`": `"$Version`",`n `"files`": [`n" + ($rows -join ",`n") + "`n ]`n}`n"
[IO.File]::WriteAllText("$stage\pack.json", $pj, $utf8)

$zip = Join-Path $Out "melange-media-$Version.zip"
& (Join-Path $PSScriptRoot "zip.ps1") -Source $stage -Destination $zip
$hash = Get-Sha256 $zip
$size = (Get-Item $zip).Length
Write-Host "Pack $Version -> $zip ($size bytes, sha256 $hash)"
if ($env:GITHUB_OUTPUT) {
    "zip=$zip" >> $env:GITHUB_OUTPUT
    "zip_sha256=$hash" >> $env:GITHUB_OUTPUT
    "zip_size=$size" >> $env:GITHUB_OUTPUT
}
