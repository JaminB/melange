# Builds the public release zip: out\melange-<version>.zip, everything a user needs to install Melange.
# Usage: .\scripts\release.ps1 [-Config x86-release] [-StageOnly]
# -StageOnly stops after staging into out\stage, so CI can sign the binaries there before zipping.
param([string]$Config = "x86-release", [switch]$StageOnly)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
Push-Location $root
try {
    # Public config: drop the CMake cache and generated build files (they may record a -PrivateDir from a
    # developer build) so this reconfigures from scratch with no -PrivateDir, and no out-of-tree modules are
    # compiled in. Fetched third-party sources under _deps are left alone; they are pinned by tag and identical
    # either way.
    $buildDir = Join-Path $root "build\$Config"
    if (Test-Path $buildDir) {
        Remove-Item (Join-Path $buildDir "CMakeCache.txt") -Force -ErrorAction SilentlyContinue
        Remove-Item (Join-Path $buildDir "CMakeFiles") -Recurse -Force -ErrorAction SilentlyContinue
    }
    & "$root\build.ps1" -Config $Config
    if ($LASTEXITCODE) { throw "build failed" }

    # Primary guard: confirm the fresh CMake cache really has no private dir configured. This is the reliable
    # check, since a private module (e.g. one named "LocalNet") can't be told apart from a public one by its
    # Name() string alone.
    $cacheFile = Join-Path $buildDir "CMakeCache.txt"
    if (-not (Test-Path $cacheFile)) { throw "CMakeCache.txt not found after build" }
    $cacheLine = Select-String -Path $cacheFile -Pattern '^MELANGE_PRIVATE_DIR:PATH=(.*)$' | Select-Object -First 1
    if (-not $cacheLine) { throw "MELANGE_PRIVATE_DIR not found in CMakeCache.txt" }
    $privateDirValue = $cacheLine.Matches[0].Groups[1].Value.Trim()
    if ($privateDirValue) { throw "MELANGE_PRIVATE_DIR is set to '$privateDirValue'; refusing to release a build with private modules configured" }

    $verMatch = Select-String -Path "$root\CMakeLists.txt" -Pattern 'project\(Melange VERSION ([0-9.]+)' | Select-Object -First 1
    if (-not $verMatch) { throw "could not find the project version in CMakeLists.txt" }
    $version = $verMatch.Matches[0].Groups[1].Value
    Write-Host "Melange version $version"

    # Secondary scan: a few private module names that are safe to match literally (they never appear in the
    # public source, unlike e.g. "LocalNet" or "Probe", which the public code already references defensively).
    $asi = "$root\dist\melange.asi"
    if (-not (Test-Path $asi)) { throw "dist\melange.asi not found" }
    $asiText = [Text.Encoding]::GetEncoding("ISO-8859-1").GetString([IO.File]::ReadAllBytes($asi))
    foreach ($marker in @("Automation", "DocShot", "MirageTest")) {
        if ($asiText.Contains($marker)) { throw "melange.asi contains the private-module marker '$marker'; refusing to release a private build" }
    }
    if ([regex]::Match($asiText, "M[0-9]+(Probe|Test)").Success) {
        throw "melange.asi contains a private-module marker (M<n>Probe/M<n>Test); refusing to release a private build"
    }

    $outDir = Join-Path $root "out"
    $stage = Join-Path $outDir "stage"
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    New-Item -ItemType Directory -Force $stage | Out-Null
    New-Item -ItemType Directory -Force (Join-Path $stage "tools") | Out-Null

    Copy-Item "$root\dist\melange.asi" $stage
    Copy-Item "$root\dist\Melange.ini" $stage
    Copy-Item "$root\dist\oasis.exe" $stage
    Copy-Item "$root\dist\tools\xomtool.exe" (Join-Path $stage "tools")
    Copy-Item "$root\tools\ual\dinput8.dll" $stage
    Copy-Item "$root\LICENSE" $stage
    Copy-Item "$root\THIRD_PARTY.md" $stage
    Copy-Item "$root\dist\Mods" (Join-Path $stage "Mods") -Recurse

    @"
Installing Melange
==================

1. Copy dinput8.dll, melange.asi and Melange.ini into your game folder, next to WormsMayhem.exe
   (...\steamapps\common\WormsXHD). Copy oasis.exe there too if you want Oasis, Melange's browser tool,
   with the game closed.
2. Start the game. The window title shows [Melange $version].

Installing a mod
----------------

1. Put the mod's folder in <game>\Mods\, so that you have <game>\Mods\<mod>\spice.json.
2. Start the game and press the grave key (to the left of 1, above Tab) to open the overlay.
3. On the Thumper/Mods page, switch the mod on. Mods that change gameplay take effect the next time you start
   the game.

The Mods\ folder in this zip has example mods, all switched off. Copy one into <game>\Mods\ to try it.

Good to know
------------

- Online play: everyone in a match needs the same gameplay mods. Mods that only change your screen, such as
  effects and UI, don't matter to other players.
- Permissions: a mod that asks for raw access to the game's memory ("Deep Desert") shows a consent dialog
  first. Only allow mods you trust.

Uninstalling
------------

Delete melange.asi, Melange.ini and the Melange and Mods folders from the game folder. Delete dinput8.dll too,
unless other .asi mods still need it.

Reporting a bug
---------------

Press Ctrl+Shift+F11 in the game (or File > Save logs as... in the overlay) and attach the zip it saves. User
names are removed, and Steam IDs and IP addresses are hashed.

License: see LICENSE and THIRD_PARTY.md. Melange is an unofficial fan project, not affiliated with or endorsed
by Team17. You need your own copy of Worms Ultimate Mayhem.
"@ -replace "\r?\n", "`r`n" | Set-Content -Path (Join-Path $stage "INSTALL.txt") -Encoding ascii -NoNewline

    # No game assets (the only binaries are ours or UAL's, checked above and by hand), and no machine-specific
    # paths leaking into anything a user would read.
    $textFiles = Get-ChildItem $stage -Recurse -File | Where-Object { $_.Extension -in @(".txt", ".md", ".ini", ".json", ".lua", ".ps1", ".glsl", ".frag", ".hlsl", ".patch") }
    foreach ($f in $textFiles) {
        $content = Get-Content $f.FullName -Raw -ErrorAction SilentlyContinue
        if ($content -and $content -match [regex]::Escape("C:\Users")) { throw "a local user path leaked into $($f.FullName)" }
    }

    if ($StageOnly) { Write-Host "Staged $stage"; return }

    $zipPath = Join-Path $outDir "melange-$version.zip"
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
    Compress-Archive -Path "$stage\*" -DestinationPath $zipPath
    Remove-Item $stage -Recurse -Force

    $hash = (Get-FileHash $zipPath -Algorithm SHA256).Hash
    $size = (Get-Item $zipPath).Length
    Write-Host "Built $zipPath ($size bytes)"
    Write-Host "SHA-256: $hash"
} finally {
    Pop-Location
}
