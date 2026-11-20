# Copyright (C) 2026 EncidXVII
# SPDX-License-Identifier: GPL-3.0-only

# Builds dist\ and packs it as release\PortXR-<version>.zip plus SHA256SUMS.txt. The version comes from the
# plugin's BepInPlugin attribute. A release needs both payloads; -MonoOnly allows a build without the IL2CPP one
# (CI has no interop assemblies, see fixtures\interop.ps1). -Test packs a tester build: named after the commit, with
# TESTING.md inside. Either way the working tree must be committed, so every zip maps to one commit (RELEASING.md).
param([switch]$MonoOnly, [switch]$Test)
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
Compress-Archive $items $zip
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
Set-Content (Join-Path $out "SHA256SUMS.txt") "$hash  $name.zip" -Encoding ascii
Write-Host "$zip`n$hash"
