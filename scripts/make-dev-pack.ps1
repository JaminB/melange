# Assembles a DEVELOPER media pack from local files and compiles it into this build, for private and dev builds that
# test Holo before the user publishes media-v1 (design section 7, R1). Writes build\<Config>\generated\
# media_dev_manifest.h, which src\media\pack_manifest.h prefers while it exists; CI's clean builds never have one.
# Rebuild (build.ps1) afterwards. -Remove deletes the header (rebuild again to go back to the compiled manifest).
# Usage: .\scripts\make-dev-pack.ps1 -Helper <holo-media.exe> -Libmpv <libmpv-2.dll> -Deno <deno.exe> [-YtDlp <yt-dlp.exe>]
#        [-Out <folder>] [-Config x86-release]
#        .\scripts\make-dev-pack.ps1 -Remove [-Config x86-release]
param([string]$Helper = "", [string]$Libmpv = "", [string]$Deno = "", [string]$YtDlp = "", [string]$Out = "",
      [string]$Config = "x86-release", [switch]$Remove)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$gen = Join-Path $root "build\$Config\generated"
$header = Join-Path $gen "media_dev_manifest.h"
if ($Remove) {
    Remove-Item $header -ErrorAction SilentlyContinue
    Write-Host "Removed $header; rebuild to use the compiled pack manifest again."
    exit 0
}
foreach ($f in @($Helper, $Libmpv, $Deno)) {
    if (-not $f -or -not (Test-Path $f -PathType Leaf)) { throw "pass -Helper, -Libmpv and -Deno (existing files)" }
}
if ($YtDlp -and -not (Test-Path $YtDlp -PathType Leaf)) { throw "-YtDlp $YtDlp not found" }
if (-not (Test-Path $gen)) { throw "build\$Config does not exist yet: run build.ps1 once first" }
$py = Get-Command py -ErrorAction SilentlyContinue
$pyArgs = @("-3")
if (-not $py) { $py = Get-Command python -ErrorAction Stop; $pyArgs = @() }

if (-not $Out) { $Out = Join-Path $root "build\dev-media-pack" }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$stamp = [DateTimeOffset]::UtcNow
$version = "dev-$($stamp.ToUnixTimeSeconds())"
$stage = Join-Path $Out "stage-$version"
New-Item -ItemType Directory -Force "$stage\LICENSES" | Out-Null
Copy-Item $Helper "$stage\holo-media.exe"
Copy-Item $Libmpv "$stage\libmpv-2.dll"
Copy-Item $Deno "$stage\deno.exe"
$utf8 = New-Object Text.UTF8Encoding $false
[IO.File]::WriteAllText("$stage\LICENSES\README-dev.txt", "A developer media pack assembled by scripts/make-dev-pack.ps1. Not for distribution.`n", $utf8)
[IO.File]::WriteAllText("$stage\SOURCES.md", "# Sources`n`nDeveloper pack $version; see the published media pack for the real list.`n", $utf8)
$zip = Join-Path $Out "melange-media-$version.zip"
$zipUrl = ([Uri]$zip).AbsoluteUri

$laneUrl = "https://github.com/JaminB/melange/releases/download/media-ytdlp/media-ytdlp.json"
if ($YtDlp) {
    # A dev lane beside the pack: file:/// manifest naming a file:/// exe; the serial is the time, so it only goes up.
    $laneVer = $stamp.ToString("yyyy.MM.dd.HHmm")
    $exe = Join-Path $Out "yt-dlp-$laneVer.exe"
    Copy-Item $YtDlp $exe -Force
    $item = Get-Item $exe
    $hash = (Get-FileHash $exe -Algorithm SHA256).Hash.ToLowerInvariant()
    $json = "{`"format`":`"melange-media-ytdlp-1`",`"version`":`"$laneVer`",`"url`":`"$(([Uri]$exe).AbsoluteUri)`"," +
            "`"size`":$($item.Length),`"sha256`":`"$hash`",`"serial`":$($stamp.ToUnixTimeSeconds())}"
    $laneJson = Join-Path $Out "media-ytdlp.json"
    [IO.File]::WriteAllText($laneJson, $json + "`n", $utf8)
    $laneUrl = ([Uri]$laneJson).AbsoluteUri
}

& $py.Source @pyArgs (Join-Path $PSScriptRoot "gen-media-manifest.py") --dev --assemble $stage --version $version --zip $zip `
    --url $zipUrl --lane-url $laneUrl --out $header
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Remove-Item $stage -Recurse -Force
Write-Host "Dev pack $version -> $zip"
Write-Host "Rebuild (build.ps1 -Config $Config) to compile it in; .\scripts\make-dev-pack.ps1 -Remove to drop it."
