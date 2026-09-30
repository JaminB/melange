# Fetches the Oasis web toolchain and dependencies without npm: no npm client runs and no install script executes.
#   .\scripts\web\fetch.ps1                  Node + esbuild into tools\ (web\toolchain.lock.json), then every package of
#                                            web\web.lock.json into web\node_modules, and THIRD_PARTY.md
#   .\scripts\web\fetch.ps1 -ToolchainOnly   Node + esbuild only (what scripts\web\lock.mjs needs)
#   .\scripts\web\fetch.ps1 -Lock <file> -Dest <dir> [-Cache <dir>]   another lock into another folder (tests)
# Every download is checked against the lock (SHA-256 for the toolchain, the registry's sha512 integrity for
# packages) and every licence against the allowlist, before anything is extracted. A mismatch stops the fetch.
param([switch]$ToolchainOnly, [string]$Lock = "", [string]$Dest = "", [string]$Cache = "", [switch]$NoThirdParty)
$ErrorActionPreference = "Stop"
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$allow = @("MIT", "ISC", "BSD-2-Clause", "BSD-3-Clause", "Apache-2.0", "0BSD")
if (-not $Cache) { $Cache = Join-Path $root "tools\web-cache" }
New-Item -ItemType Directory -Force $Cache | Out-Null
$tar = Join-Path $env:SystemRoot "System32\tar.exe"
$curl = Join-Path $env:SystemRoot "System32\curl.exe"

# An SPDX expression is allowed when every AND term, and at least one side of every OR, is on the allowlist.
function License-Ok([string]$Expr) {
    if (-not $Expr) { return $false }
    $e = $Expr.Trim()
    while ($e.StartsWith("(") -and $e.EndsWith(")")) { $e = $e.Substring(1, $e.Length - 2).Trim() }
    if ($e -match '\(') { return $false }
    foreach ($alt in ($e -split '\s+OR\s+')) {
        $ok = $true
        foreach ($term in ($alt -split '\s+AND\s+')) { if ($allow -notcontains $term.Trim()) { $ok = $false } }
        if ($ok) { return $true }
    }
    $false
}

function Fail([string]$Msg) { Write-Host "fetch: REFUSED: $Msg" -ForegroundColor Red; exit 1 }

function Download([string]$Url, [string]$File) {
    if ($Url -notmatch '^https://(registry\.npmjs\.org|nodejs\.org)/') { Fail "unexpected download host in $Url" }
    if (Test-Path $File) { return }
    Write-Host "fetch: downloading $Url"
    & $curl -fsSL --retry 3 -o "$File.part" $Url
    if ($LASTEXITCODE) { Remove-Item "$File.part" -ErrorAction SilentlyContinue; Fail "download failed: $Url" }
    Move-Item "$File.part" $File -Force
}

function Sha256([string]$File) { (Get-FileHash -Algorithm SHA256 $File).Hash.ToLowerInvariant() }

function Integrity-Ok([string]$File, [string]$Integrity) {
    if ($Integrity -notmatch '^sha512-(.+)$') { return $false }
    $hex = (Get-FileHash -Algorithm SHA512 $File).Hash
    $bytes = [byte[]]::new($hex.Length / 2)
    for ($i = 0; $i -lt $bytes.Length; $i++) { $bytes[$i] = [Convert]::ToByte($hex.Substring($i * 2, 2), 16) }
    [Convert]::ToBase64String($bytes) -eq $matches[1]
}

function Extract-Tgz([string]$File, [string]$To) {
    if (Test-Path $To) { Remove-Item -Recurse -Force $To }
    New-Item -ItemType Directory -Force $To | Out-Null
    & $tar -xzf $File -C $To
    if ($LASTEXITCODE) { Fail "cannot extract $File" }
}

# ---------------------------------------------------------------- toolchain
$tlock = Get-Content (Join-Path $root "web\toolchain.lock.json") -Raw | ConvertFrom-Json
foreach ($name in @("node", "esbuild")) {
    $t = $tlock.$name
    if (-not (License-Ok $t.license)) { Fail "$name licence '$($t.license)' is not on the allowlist" }
    $dir = Join-Path $root "tools\$($t.dir)"
    $marker = Join-Path $dir ".fetched"
    if ((Test-Path $marker) -and (Get-Content $marker -Raw).Trim() -eq $t.sha256) { continue }
    $file = Join-Path $Cache ([IO.Path]::GetFileName($t.url))
    Download $t.url $file
    $got = Sha256 $file
    if ($got -ne $t.sha256) { Fail "$name SHA-256 mismatch for $file (expected $($t.sha256), got $got); delete the file to download it again" }
    if ($t.integrity -and -not (Integrity-Ok $file $t.integrity)) { Fail "$name sha512 integrity mismatch for $file" }
    $tmp = Join-Path $Cache "x-$name"
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    if ($name -eq "node") {
        if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
        & $tar -xf $file -C (New-Item -ItemType Directory -Force $tmp).FullName
        if ($LASTEXITCODE) { Fail "cannot extract $file" }
        Move-Item (Get-ChildItem $tmp -Directory | Select-Object -First 1).FullName $dir
        # Only node.exe is used: the bundled package managers are removed so nothing can run them by accident.
        Get-ChildItem $dir | Where-Object { $_.Name -match '^(npm|npx|corepack)' } | Remove-Item -Recurse -Force
        Remove-Item -Recurse -Force (Join-Path $dir "node_modules") -ErrorAction SilentlyContinue
    } else {
        Extract-Tgz $file $tmp
        New-Item -ItemType Directory -Force $dir | Out-Null
        Move-Item (Join-Path $tmp "package\esbuild.exe") $dir
        Copy-Item (Join-Path $tmp "package\package.json") $dir
    }
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    Set-Content $marker $t.sha256
    Write-Host "fetch: $name $($t.version) ready in tools\$($t.dir)"
}
if ($ToolchainOnly) { exit 0 }

# ---------------------------------------------------------------- packages
if ($Lock) { $NoThirdParty = $true } else { $Lock = Join-Path $root "web\web.lock.json" }
if (-not $Dest) { $Dest = Join-Path $root "web" }
$lockData = Get-Content $Lock -Raw | ConvertFrom-Json
$pkgs = @($lockData.packages)
foreach ($p in $pkgs) {
    if (-not $p.name -or -not $p.version -or -not $p.tarball -or -not $p.integrity) { Fail "incomplete lock entry $($p | ConvertTo-Json -Compress)" }
    if (-not (License-Ok $p.license)) { Fail "$($p.name)@$($p.version): licence '$($p.license)' is not on the allowlist ($($allow -join ', '))" }
}
$nm = Join-Path $Dest "node_modules"
New-Item -ItemType Directory -Force $nm | Out-Null
foreach ($p in $pkgs) {
    $target = Join-Path $nm ($p.name -replace '/', '\')
    $marker = Join-Path $target ".integrity"
    if ((Test-Path $marker) -and (Get-Content $marker -Raw).Trim() -eq $p.integrity) { continue }
    $file = Join-Path $Cache (($p.name -replace '^@', '' -replace '/', '-') + "-$($p.version).tgz")
    Download $p.tarball $file
    if (-not (Integrity-Ok $file $p.integrity)) { Fail "$($p.name)@$($p.version): integrity mismatch for $file; delete the file to download it again" }
    $tmp = Join-Path $Cache "x-pkg"
    Extract-Tgz $file $tmp
    $src = Get-ChildItem $tmp -Directory | Select-Object -First 1
    $pj = Get-Content (Join-Path $src.FullName "package.json") -Raw | ConvertFrom-Json
    $lic = if ($pj.license -is [string]) { $pj.license } elseif ($pj.license.type) { $pj.license.type } else { "" }
    if ($pj.name -ne $p.name -or $pj.version -ne $p.version) { Fail "$($p.name)@$($p.version): the tarball holds $($pj.name)@$($pj.version)" }
    if (-not (License-Ok $lic)) { Fail "$($p.name)@$($p.version): package.json licence '$lic' is not on the allowlist" }
    if (Test-Path $target) { Remove-Item -Recurse -Force $target }
    New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
    Move-Item $src.FullName $target
    Remove-Item -Recurse -Force $tmp
    Set-Content $marker $p.integrity
    Write-Host "fetch: $($p.name)@$($p.version) ($lic)"
}

# ---------------------------------------------------------------- licence notices
function License-Text([string]$Dir) {
    $f = Get-ChildItem $Dir -File | Where-Object { $_.Name -match '^(LICEN[CS]E|COPYING|NOTICE)(\.(md|txt))?$' } | Sort-Object Name
    ($f | ForEach-Object { (Get-Content $_.FullName -Raw -Encoding UTF8).Trim() }) -join "`n`n"
}
$ships = @($pkgs | Where-Object { -not $_.dev })
$txt = New-Object Text.StringBuilder
[void]$txt.Append("Third-party software in the Oasis web app`n=========================================`n")
foreach ($p in $ships) {
    [void]$txt.Append("`n----------------------------------------------------------------`n$($p.name) $($p.version) ($($p.license))`n`n")
    [void]$txt.Append((License-Text (Join-Path $nm ($p.name -replace '/', '\'))) + "`n")
}
[IO.File]::WriteAllText((Join-Path $nm ".licenses.txt"), $txt.ToString().Replace("`r`n", "`n"))
if (-not $NoThirdParty) {
    $md = New-Object Text.StringBuilder
    [void]$md.Append("# Third-party notices`n`nGenerated by ``scripts/web/fetch.ps1`` from ``web/web.lock.json`` and ``web/toolchain.lock.json``. ")
    [void]$md.Append("Packages marked *ships* are bundled into the Oasis web app (and listed at ``/app/licenses.txt``); the others are build and test tools only.`n`n")
    [void]$md.Append("| Package | Version | Licence | Use |`n|---|---|---|---|`n")
    foreach ($p in $pkgs) { [void]$md.Append("| $($p.name) | $($p.version) | $($p.license) | $(if ($p.dev) { 'build/test' } else { 'ships' }) |`n") }
    foreach ($n in @("node", "esbuild")) { [void]$md.Append("| $(if ($n -eq 'node') { 'Node.js (win-x64)' } else { '@esbuild/win32-x64' }) | $($tlock.$n.version) | $($tlock.$n.license) | build/test |`n") }
    [void]$md.Append("| civetweb | 1.16 | MIT | ships (melange.asi) |`n| miniz | 3.1.2 | MIT | ships (melange.asi) |`n")
    foreach ($p in $pkgs) {
        [void]$md.Append("`n## $($p.name) $($p.version)`n`n``````text`n$(License-Text (Join-Path $nm ($p.name -replace '/', '\')))`n```````n")
    }
    [IO.File]::WriteAllText((Join-Path $root "THIRD_PARTY.md"), $md.ToString().Replace("`r`n", "`n"))
}
Write-Host "fetch: $($pkgs.Count) packages ready in $nm"
