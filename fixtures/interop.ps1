# Regenerates build\deps\interop-f3, the interop assemblies the IL2CPP plugin compiles against. BepInEx 6 writes them
# on a game's first launch, so this runs a scratch copy of fixture F3 once (batchmode: no window) with BepInEx 6 in it.
# Needs fixtures\F3\Build (fixtures\build.ps1 -Name F3) and build\deps\bepinex6 (first .\build.ps1 run).
param([string]$Name = "F3", [int]$TimeoutSeconds = 600)
$root = Split-Path $PSScriptRoot
$game = Join-Path $PSScriptRoot "$Name\Build"
$bepinex = Join-Path $root "build\deps\bepinex6"
$out = Join-Path $root "build\deps\interop-$($Name.ToLower())"
if (-not (Test-Path "$game\$Name.exe")) { throw "build the fixture first: fixtures\build.ps1 -Name $Name" }
if (-not (Test-Path $bepinex)) { throw "run .\build.ps1 once to download BepInEx 6" }

$scratch = Join-Path ([IO.Path]::GetTempPath()) "parallax-interop-$Name"
if (Test-Path $scratch) { Remove-Item -Recurse -Force $scratch }
Copy-Item $game $scratch -Recurse
Copy-Item "$bepinex\*" $scratch -Recurse
$log = Join-Path $scratch "BepInEx\LogOutput.log"

$p = Start-Process "$scratch\$Name.exe" -PassThru -ArgumentList "-batchmode -nographics"
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
try {
    while (-not ((Test-Path $log) -and (Select-String -Path $log -Pattern "Chainloader initialized" -Quiet))) {
        if ($p.HasExited) { throw "$Name exited before BepInEx finished, see $log" }
        if ((Get-Date) -gt $deadline) { throw "timed out waiting for interop generation, see $log" }
        Start-Sleep -Seconds 2
    }
} finally {
    if (-not $p.HasExited) { $p.Kill(); $p.WaitForExit() }
}

if (Test-Path $out) { Remove-Item -Recurse -Force $out }
Copy-Item "$scratch\BepInEx\interop" $out -Recurse
Remove-Item -Recurse -Force $scratch
Write-Host "interop assemblies: $out ($((Get-ChildItem $out -Filter *.dll).Count) dlls)"
