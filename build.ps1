# Builds native core, plugins and app, then assembles dist\: Parallax.exe plus the payloads it copies into games
# (payload\ = BepInEx 5 for Mono games, payload-il2cpp\ = BepInEx 6 for IL2CPP games).
param([string]$Config = "Release")
$root = $PSScriptRoot
$build = Join-Path $root "build"
$dist = Join-Path $root "dist"
$deps = @{
    bepinex5 = "https://github.com/BepInEx/BepInEx/releases/download/v5.4.23.5/BepInEx_win_x64_5.4.23.5.zip"
    # Pinned bleeding-edge build: be.788 crashes Il2CppInterop on Unity 6000.3.
    bepinex6 = "https://builds.bepinex.dev/projects/bepinex_be/791/BepInEx-Unity.IL2CPP-win-x64-6.0.0-be.791%2Bb782da4.zip"
}
foreach ($name in $deps.Keys) {
    $dir = Join-Path $build "deps\$name"
    if (-not (Test-Path $dir)) {
        Invoke-WebRequest $deps[$name] -OutFile "$dir.zip"
        Expand-Archive "$dir.zip" $dir
    }
}

cmake -S "$root\native" -B "$build\native" -G "Visual Studio 17 2022" -A x64 -Wno-author -Wno-deprecated | Out-Null
if ($LASTEXITCODE) { throw "native configure failed" }
cmake --build "$build\native" --config $Config
if ($LASTEXITCODE) { throw "native build failed" }
dotnet build "$root\managed\Parallax.Core" -c $Config -o "$build\managed"
if ($LASTEXITCODE) { throw "plugin build failed" }
# The IL2CPP plugin compiles against interop assemblies BepInEx generates from fixture F3 (see README).
$il2cpp = Test-Path "$build\deps\interop-f3\UnityEngine.CoreModule.dll"
if ($il2cpp) {
    dotnet build "$root\managed\Parallax.Core.Il2Cpp" -c $Config -o "$build\managed-il2cpp"
    if ($LASTEXITCODE) { throw "IL2CPP plugin build failed" }
} else { Write-Warning "build\deps\interop-f3 missing: skipping the IL2CPP payload" }
dotnet build "$root\app\Parallax.App" -c $Config -o "$build\app"
if ($LASTEXITCODE) { throw "app build failed" }

if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
$native = "$build\native\$Config\parallax_xr.dll", "$build\native\$Config\openxr_loader.dll"
function Payload($name, $loader, $pluginDll) {
    $payload = Join-Path $dist $name
    $plugin = Join-Path $payload "BepInEx\plugins\Parallax"
    New-Item -ItemType Directory -Force $payload | Out-Null
    Copy-Item "$build\deps\$loader\*" $payload -Recurse
    New-Item -ItemType Directory -Force $plugin | Out-Null
    Remove-Item "$payload\changelog.txt"
    Copy-Item (@($pluginDll) + $native) $plugin
    return $payload
}
$mono = Payload "payload" "bepinex5" "$build\managed\Parallax.Core.dll"
# The loader's own GameObject stays hidden so games that scan the scene do not trip over it.
New-Item -ItemType Directory -Force "$mono\BepInEx\config" | Out-Null
Set-Content "$mono\BepInEx\config\BepInEx.cfg" "[Chainloader]`r`nHideManagerGameObject = true`r`n" -Encoding ascii
if ($il2cpp) { Payload "payload-il2cpp" "bepinex6" "$build\managed-il2cpp\Parallax.Core.dll" | Out-Null }
Copy-Item "$build\app\Parallax.exe", "$build\app\Parallax.exe.config" $dist
Copy-Item "$root\LICENSE", "$root\THIRD_PARTY_NOTICES.md" $dist
Write-Host "dist ready: $dist"
