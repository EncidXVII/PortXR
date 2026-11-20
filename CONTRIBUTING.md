# Contributing

Thanks for helping. The most useful contributions, roughly in order:

1. **Game reports.** Tried a game? Open a [game report](https://github.com/EncidXVII/PortXR/issues/new?template=game-report.yml)
   with the zip from **Export logs**, whether it worked or not. Working games go into [COMPAT.md](COMPAT.md).
2. **Per-game scripts.** A short Lua script often fixes one game (camera choice, hidden objects, eye offset). See
   [SCRIPTING.md](SCRIPTING.md); share it in the game's report or as a pull request.
3. **Bug fixes and features.** Open an issue first for anything bigger than a small fix, so we can agree on the
   approach before you spend the time.

## Ground rules

- **Offline single-player games only.** PortXR refuses anti-cheat games on purpose. Changes that weaken those
  checks, or reports and scripts for multiplayer or anti-cheat games, are not accepted.
- **Uninstall stays byte-identical.** Anything the installer adds to a game folder must be recorded in the receipt
  and removed by **Remove VR**. The harness checks this.
- By contributing you agree that your work is licensed under the project's [GPL-3.0](LICENSE).

## Layout

| Folder | What | Language |
| --- | --- | --- |
| `native/` | `portxr_xr.dll`: OpenXR session, D3D11/D3D12 swapchain copies, Present hook, input emulation | C++20 |
| `managed/PortXR.Core/` | BepInEx plugin: eye cameras, UI panel, VR menu, theater and Parallax screen | C# |
| `managed/PortXR.Core.Il2Cpp/` | The same sources built for BepInEx 6 / IL2CPP games | C# |
| `app/` | `PortXR.exe`: library, settings, install and uninstall | Rust (egui) |
| `harness/` | Automated tests against SteamVR's null driver | Python |
| `fixtures/` | Unity test games, generated from `fixtures/shared` | Unity / C# |

[README.md](README.md#how-it-works) has more on how the pieces fit.

## Build

Needs Visual Studio 2022 or later Build Tools (C++ and .NET), CMake 3.24+, .NET SDK 8+ and Rust (stable, MSVC). Then:

```
.\build.ps1
```

`dist\` holds `PortXR.exe` and both payloads. Without the IL2CPP interop assemblies (see below) the build skips the
IL2CPP payload with a warning; that is fine for work on the app or on Mono games. For the app alone:
`cargo run --manifest-path app\Cargo.toml`.

## Test

The harness needs SteamVR installed (it runs the null driver, no headset needed), Python 3 with `pillow` and
`numpy`, and the fixture games, which need the matching Unity editors (`fixtures\build.ps1` lists the versions):

```
.\fixtures\build.ps1 -Name F1
python harness\run.py --fixture F1 --secondary
```

`fixtures\interop.ps1` (after building F3) generates the interop assemblies the IL2CPP plugin compiles against.

Before a pull request:

- `cargo clippy --manifest-path app\Cargo.toml` shows no warnings.
- `.\build.ps1` builds without errors.
- For plugin or native changes, run at least F1 (Mono) and F3 (IL2CPP), and the fixture that matches what you
  changed (F4 or F7 for D3D12, F2/F5/F10/F11 for post-processing). Say in the pull request which ones you ran.
  If you cannot run the harness, say so; a maintainer will.

## Code style

- Match the code around you: naming, density, idiom.
- Comments explain *why* (a game quirk, a stripped IL2CPP call, a threshold's reason), not what the next line does.
- The C# sources compile for both Mono (.NET 3.5 profile, Unity 2018.4+) and IL2CPP. On IL2CPP, engine calls the game
  never used can be stripped, so avoid `GUI.Label` and `GUILayout` (draw with `GUIStyle.Draw`), the `GUIStyle` copy
  constructor, and `Vector3.Angle` (use `Xr.Angle`); keep `ref` parameters off methods of `MonoBehaviour` classes.
  When an extra feature needs a call that may be missing, guard it so it switches off with a log line instead of
  breaking the game.
- User-facing text is plain and short, in the words the app already uses (**Add VR**, **Remove VR**, **Play in VR**).

## Rights in what you contribute

PortXR is the work of [EncidXVII](https://github.com/EncidXVII), published under GPL-3.0-only with the terms in [NOTICE](NOTICE). So that it stays one
work with one owner who can answer for all of it:

- You keep the copyright in what you write.
- By opening a pull request you license your contribution to the project under GPL-3.0-only, and you give EncidXVII a
  perpetual, worldwide, royalty-free right to use, change and relicense it as part of PortXR (for example to move to a
  later license version or to publish in a store that needs other terms).
- You confirm that you wrote it or have the right to submit it, as in the
  [Developer Certificate of Origin 1.1](https://developercertificate.org/): add `Signed-off-by: Your Name <email>`
  to your commits (`git commit -s`).
- Contributors are named in the changelog and the release notes.
