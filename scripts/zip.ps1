# Zips a folder's contents with forward-slash entry names (Compress-Archive in Windows PowerShell writes backslashes).
param([Parameter(Mandatory)][string]$Source, [Parameter(Mandatory)][string]$Destination)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

$root = (Resolve-Path $Source).Path.TrimEnd('\') + '\'
if (Test-Path $Destination) { Remove-Item $Destination -Force }
$zip = [IO.Compression.ZipFile]::Open($Destination, [IO.Compression.ZipArchiveMode]::Create)
try {
    Get-ChildItem $root -Recurse -File | Sort-Object FullName | ForEach-Object {
        $name = $_.FullName.Substring($root.Length).Replace('\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $_.FullName, $name, [IO.Compression.CompressionLevel]::Optimal)
    }
} finally { $zip.Dispose() }
