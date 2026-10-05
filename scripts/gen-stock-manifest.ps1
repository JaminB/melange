# Regenerates res\wum-1077-stock.tsv, the list of every file of the stock game that Restore vanilla keeps (and
# checks by size), from a clean Steam install of Worms Ultimate Mayhem build #1077.
#   .\scripts\gen-stock-manifest.ps1 -GameDir "C:\Program Files (x86)\Steam\steamapps\common\WormsXHD"
# Use a freshly verified install: no Melange, no other mods, never started since Steam's "Verify integrity of game
# files" (the game writes local.cfg, logs and *.csh shader caches into its folder; *.csh are left out here, the rest
# would end up in the list). Output: "relative\path<TAB>size", CRLF, sorted case-insensitively. Size 0 (the stock
# game has seven empty files) is read as "present, size not checked".
param([Parameter(Mandatory)][string]$GameDir, [string]$Out = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
if (-not $Out) { $Out = "$root\res\wum-1077-stock.tsv" }
$game = (Resolve-Path $GameDir).Path.TrimEnd('\')
$exe = Join-Path $game "WormsMayhem.exe"
if (-not (Test-Path $exe)) { throw "WormsMayhem.exe isn't in $game" }
if ((Get-Item $exe).Length -ne 5713408) { throw "WormsMayhem.exe isn't build #1077 (expected 5713408 bytes)" }
foreach ($f in "melange.asi", "dinput8.dll", "Melange.ini", "Melange.exe", "Version.txt", "Data2", "Mods", "Melange") {
    if (Test-Path (Join-Path $game $f)) { throw "$f is in the game folder: this isn't a clean install. Verify it in Steam first." }
}
$skip = @("local.cfg", "steam_appid.txt")
$lines = Get-ChildItem $game -Recurse -File -Force |
    Where-Object { $_.Extension -ne ".csh" -and $_.Name -notmatch '^(XOM.*-.*|Net_.*)\.log$' } |
    ForEach-Object {
        $rel = $_.FullName.Substring($game.Length + 1)
        if ($skip -contains $rel.ToLowerInvariant() -or $rel -like "Redist\*") { return }
        "$rel`t$($_.Length)"
    } | Sort-Object { $_.ToLowerInvariant() }
[IO.File]::WriteAllText($Out, (($lines -join "`r`n") + "`r`n"), [Text.UTF8Encoding]::new($false))
Write-Host "Wrote $($lines.Count) files to $Out"
