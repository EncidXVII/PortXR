# Builds native core, plugins and app, then assembles dist\: Parallax.exe plus the payloads it copies into games
# (payload\ = BepInEx 5 for Mono games, payload-il2cpp\ = BepInEx 6 for IL2CPP games).
param([string]$Config = "Release")
$root = $PSScriptRoot
$build = Join-Path $root "build"
$dist = Join-Path $root "dist"
# Each download is pinned by its SHA-256: what ships inside games is exactly what was reviewed.
$deps = @{
    bepinex5 = @("https://github.com/BepInEx/BepInEx/releases/download/v5.4.23.5/BepInEx_win_x64_5.4.23.5.zip",
        "82f9878551030f54657792c0740d9d51a09500eeae1fba21106b0c441e6732c4")
    # Pinned bleeding-edge build: be.788 crashes Il2CppInterop on Unity 6000.3.
    bepinex6 = @("https://builds.bepinex.dev/projects/bepinex_be/791/BepInEx-Unity.IL2CPP-win-x64-6.0.0-be.791%2Bb782da4.zip",
        "bb1c510909b69522733791f3a0817a487379a52da6468a492f1c9c19b3ec6f9a")
}
New-Item -ItemType Directory -Force (Join-Path $build "deps") | Out-Null
foreach ($name in $deps.Keys) {
    $dir = Join-Path $build "deps\$name"
    if (-not (Test-Path $dir)) {
        $url, $sha = $deps[$name]
        Invoke-WebRequest $url -OutFile "$dir.zip"
        if ((Get-FileHash "$dir.zip" -Algorithm SHA256).Hash -ne $sha) { Remove-Item "$dir.zip"; throw "$name download does not match its pinned SHA-256" }
        Expand-Archive "$dir.zip" $dir
    }
}

# The newest Visual Studio installed (2022 or later) builds it.
cmake -S "$root\native" -B "$build\native" -A x64 -Wno-author -Wno-deprecated | Out-Null
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
cargo build --release --manifest-path "$root\app\Cargo.toml" --target-dir "$build\app"
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
    # MoonSharp runs the per-game Lua scripts; each plugin build ships the copy built for its runtime.
    Copy-Item (@($pluginDll, (Join-Path (Split-Path $pluginDll) "MoonSharp.Interpreter.dll")) + $native) $plugin
    return $payload
}
$mono = Payload "payload" "bepinex5" "$build\managed\Parallax.Core.dll"
# The loader's own GameObject stays hidden so games that scan the scene do not trip over it.
New-Item -ItemType Directory -Force "$mono\BepInEx\config" | Out-Null
Set-Content "$mono\BepInEx\config\BepInEx.cfg" "[Chainloader]`r`nHideManagerGameObject = true`r`n" -Encoding ascii
if ($il2cpp) {
    $il = Payload "payload-il2cpp" "bepinex6" "$build\managed-il2cpp\Parallax.Core.dll"
    # BepInEx 6 opens a console window by default; it can take focus from the game, which then gets no keys or clicks.
    New-Item -ItemType Directory -Force "$il\BepInEx\config" | Out-Null
    Set-Content "$il\BepInEx\config\BepInEx.cfg" "[Logging.Console]`r`nEnabled = false`r`n" -Encoding ascii
}
Copy-Item "$build\app\release\Parallax.exe" $dist
# Theater for games Parallax cannot go inside: the window-capture host and the VR core it drives.
New-Item -ItemType Directory -Force "$dist\theater" | Out-Null
Copy-Item (@("$build\native\$Config\ParallaxTheater.exe") + $native) "$dist\theater"
# Godot 4.3+ games: the VR core as a GDExtension, its OpenXR loader and the adapter script (copied into a ParallaxVR folder).
New-Item -ItemType Directory -Force "$dist\godot" | Out-Null
Copy-Item @("$build\native\$Config\parallax_godot.dll", "$build\native\$Config\openxr_loader.dll", "$root\godot\core\parallax.gd", "$root\godot\core\parallax.gdextension") "$dist\godot"
# Unreal games: the VR core as dwmapi.dll and its OpenXR loader, copied beside the game's Shipping exe.
New-Item -ItemType Directory -Force "$dist\unreal" | Out-Null
Copy-Item @("$build\native\$Config\dwmapi.dll", "$build\native\$Config\openxr_loader.dll") "$dist\unreal"
Copy-Item "$root\LICENSE", "$root\THIRD_PARTY_NOTICES.md", "$root\SCRIPTING.md" $dist
Write-Host "dist ready: $dist"
