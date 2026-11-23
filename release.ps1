# Copyright (C) 2026 EncidXVII
# SPDX-License-Identifier: GPL-3.0-only

# Builds dist\ and packs it as release\PortXR-<version>.zip plus SHA256SUMS.txt. The version comes from the
# plugin's BepInPlugin attribute. A release needs both payloads; -MonoOnly allows a build without the IL2CPP one
# (CI has no interop assemblies, see fixtures\interop.ps1). -Test packs a tester build: named after the commit, with
# TESTING.md inside. Either way the working tree must be committed, so every zip maps to one commit (RELEASING.md).
param([switch]$MonoOnly, [switch]$Test)
# Compress-Archive in Windows PowerShell 5.1 names entries "dir\file", which Linux tools and some extractors read
# as one file with a backslash in its name. This writes "dir/file", as the zip format specifies. Like
# Compress-Archive, a folder goes in under its own name and a file at the top.
function New-Zip([string]$Zip, [string[]]$Paths) {
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    if (Test-Path $Zip) { Remove-Item $Zip }
    $archive = [IO.Compression.ZipFile]::Open($Zip, 'Create')
    try {
        foreach ($path in $Paths) {
            $item = Get-Item $path
            $base = Split-Path $item.FullName -Parent
            $files = if ($item.PSIsContainer) { Get-ChildItem $item.FullName -Recurse -File } else { @($item) }
            foreach ($file in $files) {
                $name = $file.FullName.Substring($base.Length + 1).Replace('\', '/')
                [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $name, 'Optimal')
            }
            # Empty folders too (a mod loader may expect one to exist), as "dir/" entries.
            if ($item.PSIsContainer) {
                foreach ($dir in Get-ChildItem $item.FullName -Recurse -Directory | Where-Object { -not (Get-ChildItem $_.FullName -Force) }) {
                    [void]$archive.CreateEntry($dir.FullName.Substring($base.Length + 1).Replace('\', '/') + '/')
                }
            }
        }
    } finally { $archive.Dispose() }
}

$root = $PSScriptRoot
if (git -C $root status --porcelain) { throw "uncommitted changes: commit first, so the zip matches a commit" }
$commit = (git -C $root rev-parse --short HEAD).Trim()
& "$root\build.ps1"
if (-not $?) { throw "build failed" }
$dist = Join-Path $root "dist"
if (-not $MonoOnly -and -not (Test-Path "$dist\payload-il2cpp")) { throw "IL2CPP payload missing: run fixtures\interop.ps1 first" }

$plugin = Get-Content "$root\managed\PortXR.Core\Plugin.cs" -Raw
if ($plugin -notmatch 'BepInPlugin\("[^"]+", "[^"]+", "([^"]+)"\)') { throw "plugin version not found" }
$version = $Matches[1]
if ((Get-Content "$root\app\Cargo.toml" -Raw) -notmatch "version = `"$([regex]::Escape($version))`"") { throw "app\Cargo.toml version differs from the plugin ($version)" }
$name = if ($MonoOnly) { "PortXR-$version-mono-only" } else { "PortXR-$version" }
if ($Test) { $name += "-test-$commit" }

$out = Join-Path $root "release"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory $out | Out-Null
$zip = Join-Path $out "$name.zip"
$items = @(Get-ChildItem $dist | ForEach-Object FullName)
if ($Test) { $items += Join-Path $root "TESTING.md" }
New-Zip $zip $items
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
# LF, not CRLF: `sha256sum -c` on Linux reads a CR as part of the file name.
[IO.File]::WriteAllText((Join-Path $out "SHA256SUMS.txt"), "$hash  $name.zip`n")
Write-Host "$zip`n$hash"
