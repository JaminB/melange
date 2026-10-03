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
    Copy-Item "$root\dist\Melange.exe" $stage
    Copy-Item "$root\dist\tools\xomtool.exe" (Join-Path $stage "tools")
    Copy-Item "$root\tools\ual\dinput8.dll" $stage
    Copy-Item "$root\dist\INSTALL.txt" $stage
    Copy-Item "$root\LICENSE" $stage
    Copy-Item "$root\THIRD_PARTY.md" $stage
    Copy-Item "$root\dist\Mods" (Join-Path $stage "Mods") -Recurse

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
    & "$PSScriptRoot\zip.ps1" -Source $stage -Destination $zipPath
    Remove-Item $stage -Recurse -Force

    $hash = (Get-FileHash $zipPath -Algorithm SHA256).Hash
    $size = (Get-Item $zipPath).Length
    Write-Host "Built $zipPath ($size bytes)"
    Write-Host "SHA-256: $hash"
} finally {
    Pop-Location
}
