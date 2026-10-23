# Builds native core, plugin and app, then assembles dist\ (Parallax.exe + payload\ copied into games).
param([string]$Config = "Release")
$root = $PSScriptRoot
$build = Join-Path $root "build"
$dist = Join-Path $root "dist"
$bepinexVersion = "5.4.23.5"

$bepinex = Join-Path $build "deps\bepinex5"
if (-not (Test-Path $bepinex)) {
    $zip = "$bepinex.zip"
    Invoke-WebRequest "https://github.com/BepInEx/BepInEx/releases/download/v$bepinexVersion/BepInEx_win_x64_$bepinexVersion.zip" -OutFile $zip
    Expand-Archive $zip $bepinex
}

cmake -S "$root\native" -B "$build\native" -G "Visual Studio 17 2022" -A x64 -Wno-dev -Wno-deprecated | Out-Null
if ($LASTEXITCODE) { throw "native configure failed" }
cmake --build "$build\native" --config $Config
if ($LASTEXITCODE) { throw "native build failed" }
dotnet build "$root\managed\Parallax.Core" -c $Config -o "$build\managed"
if ($LASTEXITCODE) { throw "plugin build failed" }
dotnet build "$root\app\Parallax.App" -c $Config -o "$build\app"
if ($LASTEXITCODE) { throw "app build failed" }

if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
$payload = Join-Path $dist "payload"
$plugin = Join-Path $payload "BepInEx\plugins\Parallax"
New-Item -ItemType Directory -Force $plugin, "$payload\BepInEx\config" | Out-Null
Copy-Item "$bepinex\*" $payload -Recurse
Remove-Item "$payload\changelog.txt"
Copy-Item "$build\managed\Parallax.Core.dll", "$build\native\$Config\parallax_xr.dll", "$build\native\$Config\openxr_loader.dll" $plugin
# The loader's own GameObject stays hidden so games that scan the scene do not trip over it.
Set-Content "$payload\BepInEx\config\BepInEx.cfg" "[Chainloader]`r`nHideManagerGameObject = true`r`n" -Encoding ascii
Copy-Item "$build\app\Parallax.exe", "$build\app\Parallax.exe.config" $dist
Copy-Item "$root\LICENSE", "$root\THIRD_PARTY_NOTICES.md" $dist
Write-Host "dist ready: $dist"
