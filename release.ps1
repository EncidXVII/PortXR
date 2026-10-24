# Builds dist\ and packs it as release\Parallax-<version>.zip plus SHA256SUMS.txt. The version comes from the
# plugin's BepInPlugin attribute. A release needs both payloads; -MonoOnly allows a build without the IL2CPP one
# (CI has no interop assemblies, see fixtures\interop.ps1).
param([switch]$MonoOnly)
$root = $PSScriptRoot
& "$root\build.ps1"
if (-not $?) { throw "build failed" }
$dist = Join-Path $root "dist"
if (-not $MonoOnly -and -not (Test-Path "$dist\payload-il2cpp")) { throw "IL2CPP payload missing: run fixtures\interop.ps1 first" }

$plugin = Get-Content "$root\managed\Parallax.Core\Plugin.cs" -Raw
if ($plugin -notmatch 'BepInPlugin\("[^"]+", "[^"]+", "([^"]+)"\)') { throw "plugin version not found" }
$version = $Matches[1]
$name = if ($MonoOnly) { "Parallax-$version-mono-only" } else { "Parallax-$version" }

$out = Join-Path $root "release"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory $out | Out-Null
$zip = Join-Path $out "$name.zip"
Compress-Archive "$dist\*" $zip
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
Set-Content (Join-Path $out "SHA256SUMS.txt") "$hash  $name.zip" -Encoding ascii
Write-Host "$zip`n$hash"
