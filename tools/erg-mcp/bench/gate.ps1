# Gate 1 in one command: everything a map-builder change must pass before it is reported, with one line per step and a
# JSON summary. Run it from anywhere in the checkout with the game in -GameDir closed:
#   .\tools\erg-mcp\bench\gate.ps1 [-GameDir <test install>] [-Out <dir>] [-Skip build,web,...] [-Only caldera]
# Steps (names for -Skip / -Only):
#   build     .\build.ps1 (melange.asi, Melange.exe, the web app)
#   web       scripts\web\test.ps1 (unit)
#   panels    scripts\web\test.ps1 -Panels
#   erg       scripts\web\test.ps1 -Erg
#   selftest  the C++ self-tests of scripts\selftest.ps1, each run on its own so one failure does not hide the rest; the
#             failures named in -KnownSelftest (default launcher_selftest: this machine has no tools\ual\dinput8.dll) are
#             reported as known and do not fail the gate. jlog_selftest is run once more when it fails (its badpath case
#             flakes).
#   ergmcp    tools\erg-mcp\test.ps1
#   caldera   the Caldera bench rebuilt from scratch (build.mjs --no-export, detail.mjs, ship.mjs into a scratch folder,
#             about 8 minutes) and compared byte for byte with the committed bench\caldera\pack: no change to the
#             builder may change what an existing map builds to. It builds a scratch project, gatecal, so the game
#             folder's own caldera project is left alone; a level's file names and stem carry its project id, so the
#             id is put back to caldera in the scratch pack's text files before the compare (the mesh bank is compared
#             as it is).
# build.ps1 regenerates THIRD_PARTY.md; the gate puts the committed one back when that is the only change to it.
# Exit code 0 when every step passed (known self-test failures aside). Logs: <Out>\<step>.log; summary: <Out>\gate.json.
param(
    [string]$GameDir = $(if ($env:MELANGE_GAME_DIR) { $env:MELANGE_GAME_DIR } else { "C:\Users\Jamin\Desktop\WUMFix\testenv\C" }),
    [string]$Out = "",
    [string[]]$Skip = @(),
    [string[]]$Only = @(),
    [string[]]$KnownSelftest = @("launcher_selftest"),
    [string]$Config = "x86-release"
)
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$root = (Resolve-Path (Join-Path $here "..\..\..")).Path
if (-not $Out) { $Out = Join-Path $root "out\gate" }
New-Item -ItemType Directory -Force $Out | Out-Null
$node = Join-Path $root "tools\node\node.exe"
$steps = @("build", "web", "panels", "erg", "selftest", "ergmcp", "caldera")
foreach ($n in @($Skip + $Only)) { if ($steps -notcontains $n) { throw "unknown step '$n' (steps: $($steps -join ', '))" } }
$results = New-Object System.Collections.ArrayList

function Wanted([string]$name) { return ($Only.Count -eq 0 -or $Only -contains $name) -and ($Skip -notcontains $name) }
function Record([string]$name, [string]$state, [string]$note, [double]$seconds) {
    [void]$results.Add([ordered]@{ step = $name; state = $state; note = $note; seconds = [math]::Round($seconds, 1) })
    Write-Host ("{0,-9} {1,-6} {2,6:N0} s  {3}" -f $name, $state, $seconds, $note)
}
# Runs a child PowerShell script (or an exe) with its output in <Out>\<log>; answers the exit code.
function Run([string]$log, [string]$file, [string[]]$arguments) {
    $path = Join-Path $Out $log
    $quoted = @($arguments | Where-Object { $null -ne $_ -and $_ -ne "" } | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } })
    if ($file -like "*.ps1") { $exe = "powershell.exe"; $argv = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$file`"") + $quoted }
    else { $exe = $file; $argv = $quoted }
    $so = "$path.out"; $se = "$path.err"
    $sp = @{ FilePath = $exe; WorkingDirectory = $root; Wait = $true; PassThru = $true; NoNewWindow = $true; RedirectStandardOutput = $so; RedirectStandardError = $se }
    if ($argv.Count) { $sp.ArgumentList = $argv }
    $p = Start-Process @sp
    $text = (Get-Content $so -Raw -ErrorAction SilentlyContinue) + (Get-Content $se -Raw -ErrorAction SilentlyContinue)
    [IO.File]::WriteAllText($path, [string]$text)
    Remove-Item $so, $se -ErrorAction SilentlyContinue
    return $p.ExitCode
}
function Step([string]$name, [scriptblock]$body) {
    if (-not (Wanted $name)) { Record $name "skip" "" 0; return }
    $t = [Diagnostics.Stopwatch]::StartNew()
    try {
        $r = & $body
        Record $name $r.state $r.note $t.Elapsed.TotalSeconds
    } catch {
        Record $name "FAIL" "$($_.Exception.Message)" $t.Elapsed.TotalSeconds
    }
}
function Tail([string]$log, [int]$n = 3) {
    $p = Join-Path $Out $log
    if (-not (Test-Path $p)) { return "" }
    return ((Get-Content $p | Where-Object { $_.Trim() } | Select-Object -Last $n) -join " | ")
}

$thirdParty = (& git -C $root status --porcelain -- THIRD_PARTY.md)

Step "build" {
    $c = Run "build.log" (Join-Path $root "build.ps1") @("-Config", $Config)
    if ($c) { return @{ state = "FAIL"; note = "exit $c; " + (Tail "build.log") } }
    return @{ state = "pass"; note = "" }
}
Step "web" {
    $c = Run "web.log" (Join-Path $root "scripts\web\test.ps1") @()
    return @{ state = $(if ($c) { "FAIL" } else { "pass" }); note = (Tail "web.log" 1) }
}
Step "panels" {
    $c = Run "panels.log" (Join-Path $root "scripts\web\test.ps1") @("-Panels")
    return @{ state = $(if ($c) { "FAIL" } else { "pass" }); note = (Tail "panels.log" 1) }
}
Step "erg" {
    $c = Run "erg.log" (Join-Path $root "scripts\web\test.ps1") @("-Erg")
    return @{ state = $(if ($c) { "FAIL" } else { "pass" }); note = (Tail "erg.log" 1) }
}
Step "selftest" {
    # scripts\selftest.ps1 builds every test, then stops at the first that fails: run it for the build, then each exe here
    $src = Get-Content (Join-Path $root "scripts\selftest.ps1") -Raw
    $tests = [regex]::Matches($src, '"([a-z0-9_]+_selftest)"') | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique
    if (-not $tests) { throw "no tests found in scripts\selftest.ps1" }
    $c = Run "selftest-build.log" (Join-Path $root "scripts\selftest.ps1") @("-Config", $Config)
    $failed = @(); $known = @(); $missing = @()
    foreach ($t in $tests) {
        $exe = Join-Path $root "build\$Config\$t.exe"
        if (-not (Test-Path $exe)) { $missing += $t; continue }
        $targs = if ($t -eq "oasis_core_selftest") { @("--mutate", "20") } else { @() }
        $code = Run "selftest-$t.log" $exe $targs
        if ($code -and $t -eq "jlog_selftest") { $code = Run "selftest-$t.log" $exe $targs }
        if ($code) { if ($KnownSelftest -contains $t) { $known += $t } else { $failed += $t } }
    }
    $note = "$($tests.Count) tests"
    if ($known) { $note += "; known local failures: $($known -join ', ')" }
    if ($missing) { $note += "; NOT BUILT: $($missing -join ', ')" }
    if ($failed) { $note += "; FAILED: $($failed -join ', ')" }
    return @{ state = $(if ($failed -or $missing) { "FAIL" } else { "pass" }); note = $note }
}
Step "ergmcp" {
    $c = Run "ergmcp.log" (Join-Path $here "..\test.ps1") @()
    $sum = (Get-Content (Join-Path $Out "ergmcp.log") | Where-Object { $_ -match '(tests|pass|fail) d+s*$' }) -join ", "
    return @{ state = $(if ($c) { "FAIL" } else { "pass" }); note = $sum -replace '[^\x20-\x7e]', '' }
}
Step "caldera" {
    if (Get-Process WormsMayhem -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($GameDir, [StringComparison]::OrdinalIgnoreCase) }) { throw "the game in $GameDir is running: close it" }
    if (Test-Path (Join-Path $GameDir "Mods\caldera")) { throw "$GameDir\Mods\caldera exists: ship.mjs exports there; move it aside" }
    $work = Join-Path $Out "caldera"
    if (Test-Path $work) { Remove-Item -Recurse -Force $work }
    New-Item -ItemType Directory -Force $work | Out-Null
    $env:MELANGE_GAME_DIR = $GameDir; $env:CALDERA_ID = "gatecal"
    $bench = Join-Path $here "caldera"
    $committed = Join-Path $bench "pack"
    if (& git -C $root status --porcelain -- $committed) { throw "bench\caldera\pack has uncommitted changes: the compare is against the committed pack" }
    # build.mjs --no-export: without it the build writes its own (undressed) export over the committed pack
    foreach ($s in @(@("build.mjs", @("--out", "$work\build", "--no-export")), @("detail.mjs", @("--out", "$work\detail", "--build-out", "$work\build", "--project", "gatecal")),
            @("ship.mjs", @("--out", "$work\pack", "--project", "gatecal", "--game", $GameDir)))) {
        $c = Run "caldera-$($s[0]).log" $node (@((Join-Path $bench $s[0])) + $s[1])
        if ($c) { return @{ state = "FAIL"; note = "$($s[0]) exit $c; " + (Tail "caldera-$($s[0]).log") } }
    }
    if (& git -C $root status --porcelain -- $committed) { & git -C $root checkout -- $committed; throw "the bench wrote into bench\caldera\pack (put back): it must build into the scratch folder only" }
    # the scratch project's id back to caldera: the patch's file name, and the id in every text file
    $enc = New-Object System.Text.UTF8Encoding $false
    Get-ChildItem -Recurse -File "$work\pack" | Where-Object { $_.Extension -in ".json", ".ps1", ".lua" } | ForEach-Object {
        $t = [IO.File]::ReadAllText($_.FullName, $enc)
        if ($t.Contains("gatecal")) { [IO.File]::WriteAllText($_.FullName, $t.Replace("gatecal", "caldera"), $enc) }
        if ($_.Name.Contains("gatecal")) { Rename-Item $_.FullName ($_.Name.Replace("gatecal", "caldera")) }
    }
    # byte for byte against the committed pack (built levels are per machine and not committed)
    $list = { param($d) Get-ChildItem -Recurse -File $d | Where-Object { $_.FullName -notmatch '\\assets\\levels\\' } | ForEach-Object { $_.FullName.Substring($d.Length + 1) } | Sort-Object }
    $a = @(& $list $committed); $b = @(& $list "$work\pack")
    $diff = @()
    foreach ($f in ($a + $b | Select-Object -Unique)) {
        $pa = Join-Path $committed $f; $pb = Join-Path "$work\pack" $f
        if (-not (Test-Path $pa)) { $diff += "$f (new)"; continue }
        if (-not (Test-Path $pb)) { $diff += "$f (missing)"; continue }
        if ((Get-FileHash $pa -Algorithm SHA256).Hash -ne (Get-FileHash $pb -Algorithm SHA256).Hash) { $diff += $f }
    }
    Remove-Item -Recurse -Force (Join-Path $GameDir "Mods\caldera") -ErrorAction SilentlyContinue
    if ($diff) { return @{ state = "FAIL"; note = "differs from the committed pack: $($diff -join ', ') (kept in $work\pack)" } }
    Remove-Item -Recurse -Force $work
    return @{ state = "pass"; note = "$($a.Count) files byte-identical to bench\caldera\pack" }
}

# build.ps1's side effect: put the committed THIRD_PARTY.md back unless it was already changed before the gate ran
if (-not $thirdParty -and (& git -C $root status --porcelain -- THIRD_PARTY.md)) {
    & git -C $root checkout -- THIRD_PARTY.md
    Write-Host "THIRD_PARTY.md was regenerated by the build and put back (never commit that side effect)"
}
$bad = @($results | Where-Object { $_.state -eq "FAIL" })
$summary = [ordered]@{ ok = ($bad.Count -eq 0); gameDir = $GameDir; head = (& git -C $root rev-parse --short HEAD); dirty = [bool](& git -C $root status --porcelain); steps = $results }
[IO.File]::WriteAllText((Join-Path $Out "gate.json"), ($summary | ConvertTo-Json -Depth 5))
Write-Host ("GATE " + $(if ($bad.Count) { "FAILED: " + (($bad | ForEach-Object { $_.step }) -join ", ") } else { "PASSED" }) + "  ($Out\gate.json)")
exit $(if ($bad.Count) { 1 } else { 0 })
