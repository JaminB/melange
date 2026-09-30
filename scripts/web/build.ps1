# Builds the Oasis web app into <Out>: tsc --noEmit (type check), then esbuild (bundle, split, minify) with the
# portable toolchain in tools\. The build id (sent in the protocol handshake) is a hash of every input, so the same
# sources and lock give the same output. Usage: .\scripts\web\build.ps1 [-Out <dir>] [-SkipTypeCheck]
param([string]$Out = "", [switch]$SkipTypeCheck)
$ErrorActionPreference = "Stop"
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$web = Join-Path $root "web"
if (-not $Out) { $Out = Join-Path $web "dist" }
$Out = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out)
$node = Join-Path $root "tools\node\node.exe"
$esbuild = Join-Path $root "tools\esbuild\esbuild.exe"
foreach ($f in @($node, $esbuild, (Join-Path $web "node_modules\typescript\bin\tsc"), (Join-Path $web "node_modules\preact\package.json"))) {
    if (-not (Test-Path $f)) { throw "missing $f; run scripts\web\fetch.ps1 first" }
}

# Build id: SHA-256 over the locks and every source file (relative path + content), in a fixed order.
$inputs = @("web\web.lock.json", "web\toolchain.lock.json", "web\tsconfig.json", "scripts\web\build.ps1") +
    @(Get-ChildItem (Join-Path $web "src"), (Join-Path $web "public") -Recurse -File | ForEach-Object { $_.FullName.Substring($root.Length + 1) })
$inputs = $inputs | Sort-Object { $_ -replace '\\', '/' } -CaseSensitive
$sha = [Security.Cryptography.SHA256]::Create()
$ms = New-Object IO.MemoryStream
foreach ($rel in $inputs) {
    $name = [Text.Encoding]::UTF8.GetBytes(($rel -replace '\\', '/') + "`n")
    $ms.Write($name, 0, $name.Length)
    $bytes = [IO.File]::ReadAllBytes((Join-Path $root $rel))
    $text = [Text.Encoding]::UTF8.GetString($bytes).Replace("`r`n", "`n")
    $norm = [Text.Encoding]::UTF8.GetBytes($text)
    $ms.Write($norm, 0, $norm.Length)
}
$build = (($sha.ComputeHash($ms.ToArray()) | ForEach-Object { $_.ToString("x2") }) -join "").Substring(0, 12)

if (-not $SkipTypeCheck) {
    & $node (Join-Path $web "node_modules\typescript\bin\tsc") --noEmit -p (Join-Path $web "tsconfig.json")
    if ($LASTEXITCODE) { throw "tsc failed" }
}

if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force (Join-Path $Out "app") | Out-Null
Push-Location $web
try {
    & $esbuild "src/main.tsx" "erg-mesher=src/panels/erg/terrain/mesher.worker.ts" --bundle --splitting --format=esm --minify --target=chrome110,firefox115,safari16 `
        --jsx=automatic --jsx-import-source=preact --charset=utf8 --legal-comments=none `
        "--define:__OASIS_BUILD__=\`"$build\`"" --entry-names="[name]" --chunk-names="chunks/[name]-[hash]" `
        --outdir="$Out\app" --log-level=warning
    if ($LASTEXITCODE) { throw "esbuild failed" }
} finally { Pop-Location }
Copy-Item (Join-Path $web "public\*") $Out -Recurse -Force
$lic = Join-Path $web "node_modules\.licenses.txt"
if (Test-Path $lic) { Copy-Item $lic (Join-Path $Out "app\licenses.txt") }
[IO.File]::WriteAllText((Join-Path $Out "build.txt"), "$build`n")
$js = (Get-ChildItem (Join-Path $Out "app") -Recurse -File -Filter *.js | Measure-Object Length -Sum).Sum
Write-Host "web: build $build, $([math]::Round($js / 1024, 1)) KB of JavaScript -> $Out"
