# Builds the yt-dlp lane's files (design 1.1.2 as revised by section 7): yt-dlp-<version>.exe and the unsigned hash
# manifest media-ytdlp.json {format, version, url, size, sha256, serial}, ready to attach to the rolling release
# media-ytdlp of JaminB/melange. Core installs the exe only if its SHA-256 matches the manifest and the serial is higher
# than the installed one (no rollback), so this script never publishes an older yt-dlp than the current manifest.
#
# Trust chain (all checked here, nothing is trusted because it is "latest"): the release's SHA2-256SUMS must carry a
# valid GPG signature (SHA2-256SUMS.sig) from the maintainers' key committed as media\yt-dlp-public.key, pinned by
# fingerprint and file hash in media\media.lock.json; yt-dlp.exe must hash to the line in SHA2-256SUMS.
#
#   .\scripts\media-ytdlp.ps1 -Out out\ytdlp [-Version <tag, default: the latest release>] [-Gpg <gpg.exe>]
#       [-CurrentManifest <local media-ytdlp.json, default: fetched from the rolling release>] [-Serial <n>]
# Writes GITHUB_OUTPUT: changed=true|false, version, serial. Exit 0 with changed=false when nothing new is due.
param([Parameter(Mandatory)][string]$Out, [string]$Version = "", [string]$Gpg = "", [string]$CurrentManifest = "",
      [long]$Serial = 0, [string]$Repo = "JaminB/melange")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$lock = (Get-Content (Join-Path $root "media\media.lock.json") -Raw | ConvertFrom-Json).ytdlp
$utf8 = New-Object Text.UTF8Encoding $false
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$work = Join-Path $Out "work"
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory $work | Out-Null

function Set-Output($k, $v) { if ($env:GITHUB_OUTPUT) { "$k=$v" >> $env:GITHUB_OUTPUT }; Write-Host "$k=$v" }
function Get-Sha256($Path) { (Get-FileHash -Algorithm SHA256 $Path).Hash.ToLowerInvariant() }
# "2026.10.21" and "2026.10.21.123456" compare part by part as numbers.
function Compare-Version($a, $b) {
    $x = $a.Split('.') | ForEach-Object { [long]$_ }
    $y = $b.Split('.') | ForEach-Object { [long]$_ }
    for ($i = 0; $i -lt [Math]::Max($x.Count, $y.Count); $i++) {
        $p = if ($i -lt $x.Count) { $x[$i] } else { 0 }
        $q = if ($i -lt $y.Count) { $y[$i] } else { 0 }
        if ($p -ne $q) { return [Math]::Sign($p - $q) }
    }
    0
}
function Get-Url($Url, $To) {
    curl.exe -fsSL --retry 3 -o $To $Url
    if ($LASTEXITCODE) { throw "download failed: $Url" }
}

if (-not $Gpg) {
    foreach ($c in "$env:ProgramFiles\Git\usr\bin\gpg.exe", "${env:ProgramFiles(x86)}\GnuPG\bin\gpg.exe") { if (Test-Path $c) { $Gpg = $c; break } }
    if (-not $Gpg) { $cmd = Get-Command gpg -ErrorAction SilentlyContinue; if ($cmd) { $Gpg = $cmd.Source } }
    if (-not $Gpg) { throw "gpg not found: pass -Gpg" }
}
$keyFile = Join-Path $root $lock.gpgKeyFile.Replace('/', '\')
if ((Get-Sha256 $keyFile) -ne $lock.gpgKeySha256) { throw "$($lock.gpgKeyFile) does not match media.lock.json" }

if (-not $Version) {
    $hdr = @{ "User-Agent" = "melange-media-ci" }
    if ($env:GH_TOKEN) { $hdr["Authorization"] = "Bearer $env:GH_TOKEN" }
    $Version = (Invoke-RestMethod "https://api.github.com/repos/$($lock.repo)/releases/latest" -Headers $hdr).tag_name
}
if ($Version -notmatch '^\d{4}\.\d{2}\.\d{2}(\.\d{1,9})?$') { throw "unexpected yt-dlp version '$Version'" }

# The manifest now on the rolling release: never go backwards, and do nothing when it is already this version.
$have = $null
$cur = $CurrentManifest
if (-not $cur) {
    $cur = Join-Path $work "current.json"
    curl.exe -fsSL --retry 3 -o $cur "https://github.com/$Repo/releases/download/media-ytdlp/media-ytdlp.json" 2>$null
    if ($LASTEXITCODE) { $cur = "" }
}
if ($cur -and (Test-Path $cur)) {
    try { $have = Get-Content $cur -Raw | ConvertFrom-Json } catch { $have = $null }
}
if ($have -and $have.version) {
    $c = Compare-Version $Version $have.version
    if ($c -lt 0) { Write-Host "latest yt-dlp $Version is older than the published $($have.version); keeping the published one"; Set-Output changed false; exit 0 }
    if ($c -eq 0) { Write-Host "yt-dlp $Version is already published"; Set-Output changed false; exit 0 }
}

$base = "https://github.com/$($lock.repo)/releases/download/$Version"
Get-Url "$base/SHA2-256SUMS" "$work\SHA2-256SUMS"
Get-Url "$base/SHA2-256SUMS.sig" "$work\SHA2-256SUMS.sig"
Get-Url "$base/$($lock.asset)" "$work\yt-dlp.exe"

# GPG: a fresh keyring that holds only the committed key; the signature must be VALIDSIG by that key (or a subkey of it).
$gnupg = Join-Path $work "gnupg"
New-Item -ItemType Directory $gnupg | Out-Null
$fpr = $lock.gpgKeyFingerprint.ToUpperInvariant()
# Git's gpg is an MSYS program: it wants /c/x/y paths. gpg writes progress to stderr, which must not stop the script.
function Fwd($p) {
    $p = $p.Replace([string][char]92, '/')
    if ($Gpg -like '*usr*bin*' -and $p -match '^([A-Za-z]):(.*)$') { $p = '/' + $Matches[1].ToLowerInvariant() + $Matches[2] }
    $p
}
$ErrorActionPreference = "Continue"
& $Gpg --homedir (Fwd $gnupg) --batch --no-tty --import (Fwd $keyFile) 2>&1 | Out-Null
if ($LASTEXITCODE) { throw "gpg could not import the pinned key" }
$status = & $Gpg --homedir (Fwd $gnupg) --batch --no-tty --status-fd 1 --verify (Fwd "$work\SHA2-256SUMS.sig") (Fwd "$work\SHA2-256SUMS") 2>$null
$verifyExit = $LASTEXITCODE
$ErrorActionPreference = "Stop"
if ($verifyExit) { throw "the signature of SHA2-256SUMS is not valid" }
$valid = $status | Where-Object { $_ -match '^\[GNUPG:\] VALIDSIG ' } | Select-Object -First 1
if (-not $valid) { throw "gpg reported no valid signature on SHA2-256SUMS" }
$fields = $valid -split ' '
if (($fields | Select-Object -Last 1).ToUpperInvariant() -ne $fpr) { throw "SHA2-256SUMS is signed by a key other than the pinned $fpr" }
if ($status | Where-Object { $_ -match '^\[GNUPG:\] (BADSIG|ERRSIG|REVKEYSIG|EXPKEYSIG|EXPSIG) ' }) { throw "gpg flagged the signature" }

$line = (Get-Content "$work\SHA2-256SUMS") | Where-Object { $_ -match "^([0-9a-f]{64})\s+\*?$([regex]::Escape($lock.asset))\s*$" } | Select-Object -First 1
if (-not $line) { throw "SHA2-256SUMS has no line for $($lock.asset)" }
$want = ([regex]::Match($line, '^[0-9a-f]{64}')).Value
$got = Get-Sha256 "$work\yt-dlp.exe"
if ($got -ne $want) { throw "yt-dlp.exe ($got) does not match the signed SHA2-256SUMS ($want)" }
$size = (Get-Item "$work\yt-dlp.exe").Length
if ($size -lt 1MB -or $size -gt 96MB) { throw "yt-dlp.exe has an odd size ($size)" }

# Serial: seconds since 1970, and always above the published one.
if ($Serial -le 0) { $Serial = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds() }
if ($have -and $have.serial -and $Serial -le [long]$have.serial) { $Serial = [long]$have.serial + 1 }

$exeName = "yt-dlp-$Version.exe"
Move-Item "$work\yt-dlp.exe" (Join-Path $Out $exeName) -Force
$manifest = "{`"format`":`"melange-media-ytdlp-1`",`"version`":`"$Version`"," +
    "`"url`":`"https://github.com/$Repo/releases/download/media-ytdlp/$exeName`"," +
    "`"size`":$size,`"sha256`":`"$got`",`"serial`":$Serial}`n"
[IO.File]::WriteAllText((Join-Path $Out "media-ytdlp.json"), $manifest, $utf8)
Remove-Item $work -Recurse -Force
Write-Host "yt-dlp $Version -> $Out ($size bytes, sha256 $got, serial $Serial)"
Set-Output changed true
Set-Output version $Version
Set-Output serial $Serial
