# Regenerates the mod's own artwork from scratch (no game assets are read or copied):
#   assets/icons/megabazooka.png   - 64x64 RGBA panel icon
#   assets/loose/mega-bazooka.hud.tga - 64x64 32bpp uncompressed HUD icon
# Both are a plain concentric "badge", drawn with GDI+. Run this after changing the design below;
# dist/Mods/mega-bazooka/build.ps1 calls it for you.
param()
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$root = Split-Path $PSScriptRoot -Parent   # dist/Mods/mega-bazooka
$iconDir = Join-Path $root "assets\icons"
$looseDir = Join-Path $root "assets\loose"
New-Item -ItemType Directory -Force -Path $iconDir | Out-Null
New-Item -ItemType Directory -Force -Path $looseDir | Out-Null

function New-Badge([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)
    $c = $size / 2.0
    $r1 = $size * 0.46
    $r2 = $size * 0.34
    $r3 = $size * 0.16
    $ring = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 255, 200, 60))
    $mid = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 230, 90, 30))
    $core = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 140, 40, 20))
    $outline = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 60, 20, 10)), ([Math]::Max(1.0, $size / 32.0))
    $g.FillEllipse($ring, $c - $r1, $c - $r1, $r1 * 2, $r1 * 2)
    $g.DrawEllipse($outline, $c - $r1, $c - $r1, $r1 * 2, $r1 * 2)
    $g.FillEllipse($mid, $c - $r2, $c - $r2, $r2 * 2, $r2 * 2)
    $g.FillEllipse($core, $c - $r3, $c - $r3, $r3 * 2, $r3 * 2)
    $g.Dispose()
    $ring.Dispose(); $mid.Dispose(); $core.Dispose(); $outline.Dispose()
    return $bmp
}

function Write-Tga([System.Drawing.Bitmap]$bmp, [string]$path) {
    $w = $bmp.Width
    $h = $bmp.Height
    $header = New-Object byte[] 18
    $header[2] = 2                                  # image type: uncompressed truecolor
    $header[12] = [byte]($w -band 0xFF)
    $header[13] = [byte](($w -shr 8) -band 0xFF)
    $header[14] = [byte]($h -band 0xFF)
    $header[15] = [byte](($h -shr 8) -band 0xFF)
    $header[16] = 32                                 # bits per pixel
    $header[17] = 0x28                               # 8 alpha bits, bit5 set = top-left origin (unambiguous row order)
    $stream = New-Object System.IO.FileStream $path, ([System.IO.FileMode]::Create)
    $stream.Write($header, 0, $header.Length)
    for ($y = 0; $y -lt $h; $y++) {
        for ($x = 0; $x -lt $w; $x++) {
            $p = $bmp.GetPixel($x, $y)
            $stream.WriteByte($p.B); $stream.WriteByte($p.G); $stream.WriteByte($p.R); $stream.WriteByte($p.A)
        }
    }
    $stream.Close()
}

$bmp = New-Badge 64
$pngPath = Join-Path $iconDir "megabazooka.png"
$bmp.Save($pngPath, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host "wrote $pngPath"
$tgaPath = Join-Path $looseDir "mega-bazooka.hud.tga"
Write-Tga $bmp $tgaPath
Write-Host "wrote $tgaPath"
$bmp.Dispose()
