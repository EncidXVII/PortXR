# Builds a fixture player: copies fixtures\shared into the Unity project, then Configure + Build in batchmode.
#   F0  Unity 2018.4, built-in pipeline, Mono on the .NET 3.5 runtime (old games; no CaptureScreenshotIntoRenderTexture)
#   F1  Unity 2022.3, built-in pipeline, Mono
#   F2  Unity 2022.3, URP with Bloom, Mono
#   F3  Unity 2022.3, built-in pipeline, IL2CPP (its interop assemblies are what the IL2CPP plugin compiles against)
#   F4  Unity 2022.3, built-in pipeline, Mono, Direct3D 12
param([Parameter(Mandatory)][ValidateSet("F0", "F1", "F2", "F3", "F4")][string]$Name, [string]$Editors = "E:\Unity\Editor")
$fixtures = @{
    F0 = @{ Unity = "2018.4.36f1"; Args = "-runtime legacy"; Urp = $false }
    F1 = @{ Unity = "2022.3.62f3"; Args = ""; Urp = $false }
    F2 = @{ Unity = "2022.3.62f3"; Args = ""; Urp = $true }
    F3 = @{ Unity = "2022.3.62f3"; Args = "-backend il2cpp"; Urp = $false }
    F4 = @{ Unity = "2022.3.62f3"; Args = "-gfx d3d12"; Urp = $false }
}
$f = $fixtures[$Name]
$project = Join-Path $PSScriptRoot $Name
$shared = Join-Path $PSScriptRoot "shared"
$target = Join-Path $project "Assets\Fixture"
if (Test-Path $target) { Remove-Item -Recurse -Force $target }
New-Item -ItemType Directory -Force "$target\Editor" | Out-Null
Copy-Item "$shared\*.cs" $target
Copy-Item "$shared\Editor\FixtureBuild.cs" "$target\Editor"
if ($f.Urp) { Copy-Item "$shared\Editor\FixtureUrp.cs" "$target\Editor" }

$unity = Join-Path $Editors "$($f.Unity)\Editor\Unity.exe"
foreach ($method in "Configure", "Build") {
    $log = Join-Path $project "Logs\$method.log"
    New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
    $p = Start-Process $unity -PassThru -ArgumentList "-batchmode -quit -nographics -projectPath `"$project`" -executeMethod FixtureBuild.$method -logFile `"$log`" -fixture $Name $($f.Args)"
    # WaitForExit, not -Wait: Unity 2018 leaves its compiler server running and -Wait waits for the whole tree.
    $p.WaitForExit()
    if ($p.ExitCode) { throw "$Name $method failed ($($p.ExitCode)), see $log" }
}
Write-Host "$Name built: $project\Build\$Name.exe"
