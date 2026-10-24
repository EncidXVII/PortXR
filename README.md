# Parallax

Play flat Unity games in VR. Pick a game, press **Play in VR**, and play it with your VR controllers: full stereo 6DOF view, the game's UI on a panel, and a big-screen theater mode for anything that does not work in stereo.

Working name, not final.

## Use it in 3 steps

1. Start your headset runtime (SteamVR, Virtual Desktop, Quest Link, anything with OpenXR), then run `Parallax.exe`.
2. Pick a game marked **Ready**, adjust its settings if you like, and press **Play in VR**. Parallax installs itself into the game folder; **Uninstall** puts the folder back byte for byte.
3. In game: **double-tap the left menu button** (or press F11) for the VR menu: theater mode, recenter, height, world scale, controls, camera choice.

## Controls (default: keyboard & mouse emulation)

| Controller | Game sees |
|---|---|
| Left stick | W A S D |
| Right stick | Mouse look |
| Right / left trigger | Left / right mouse button |
| A, B, X, Y | Space, Left Ctrl, E, R |
| Left / right grip | Left Shift, Q |
| Left menu: tap / hold / double-tap | Esc / Tab / VR menu |
| Point at the UI panel + trigger | Mouse cursor + click (when the game shows a cursor) |

**Gamepad** mode (VR menu, Controls) presents the controllers as an Xbox pad instead. Games on Unity's Input System package and games that read XInput see it. Unity 2021+ games on the old input manager read controllers through Windows.Gaming.Input and do not; keep keyboard & mouse there.

Something wrong? Select the game in Parallax.exe and press **Export logs**. It saves one zip to your desktop to attach to a report.

## What works today

- Direct3D 11 games from Unity 2018.4 to Unity 6: Mono (both .NET 3.5 and 4.x runtimes) and IL2CPP games (BepInEx 6), URP with its post-processing.
- Direct3D 12 games as well, with the same features.
- The game's URP, HDRP or Post Processing v2 camera settings are copied to the eyes, so bloom, tonemapping and the rest show up in VR. Temporal AA is swapped for SMAA (it smears in stereo).
- The game window keeps showing the left eye.
- Theater mode shows the flat game on a big screen 3 m away; the laser pointer works on it. The screen is curved on runtimes with curved layers (Virtual Desktop, Meta), and flat on SteamVR, which has none.
- If the headset runtime restarts or the session drops, the game continues flat and VR comes back by itself once the runtime is up again.
- IL2CPP games may have stripped engine calls; affected extras switch off with a log line instead of breaking the game.
- Not supported: Vulkan and OpenGL games, 32-bit games (no OpenXR runtime supports them). Anti-cheat games (EasyAntiCheat, BattlEye) are refused on purpose.
- See [COMPAT.md](COMPAT.md).

## Build

Needs Visual Studio 2022 Build Tools (C++ and .NET), CMake 3.24+, .NET SDK 8+, Rust (stable, MSVC toolchain).

```
.\build.ps1
```

`dist\` then holds `Parallax.exe` and the payloads it copies into games (`payload\` for Mono, `payload-il2cpp\` for IL2CPP). The IL2CPP payload needs interop assemblies generated from fixture F3 (see Test); without them the build warns and skips it.

`.\release.ps1` builds and packs `release\Parallax-<version>.zip` with `SHA256SUMS.txt`. CI (`.github/workflows/build.yml`) runs `release.ps1 -MonoOnly` on every push, because the runner has no Unity to generate the interop assemblies.

## Test

The harness runs a fixture game against SteamVR's null driver (no headset needed), drives it through a UDP test channel, and asserts eye order, orientation, scale, tracking, timing, UI, input, theater, menu, stability and a byte-identical uninstall. SteamVR must be closed; its settings are restored afterwards.

```
pip install pillow numpy
python harness\run.py --fixture F1 --soak 600
```

Test windows open on a secondary monitor when there is one (`--primary` to override). The input gate takes over the real mouse and keyboard for a few seconds, so it only runs with `--input`.

Fixtures are generated from `fixtures/shared` with `fixtures\build.ps1 -Name F1`, then tested with `python harness\run.py --fixture F1`. They need Unity under `E:\Unity\Editor\<version>` (`-Editors` to change).

| Fixture | Unity | Backend | Graphics | Pipeline |
|---|---|---|---|---|
| F0 | 2018.4 | Mono, .NET 3.5 | D3D11 | built-in |
| F1 | 2022.3 | Mono | D3D11 | built-in |
| F2 | 2022.3 | Mono | D3D11 | URP + Bloom |
| F3 | 2022.3 | IL2CPP | D3D11 | built-in |
| F4 | 2022.3 | Mono | D3D12 | built-in |
| F5 | 2022.3 | IL2CPP | D3D11 | URP + Bloom |
| F6 | 2022.3 | Mono, Input System package | D3D11 | built-in |
| F7 | 6.3 | IL2CPP | D3D12 | URP + Bloom |
| F8 | 6.3 | Mono | D3D11 | built-in |

The IL2CPP plugin compiles against interop assemblies BepInEx 6 generates on a game's first launch. After building F3, `fixtures\interop.ps1` runs a scratch copy once without a window and fills `build\deps\interop-f3`.

## How it works

- `native/` (C++20, `parallax_xr.dll`): OpenXR session and frame loop on Unity's render thread, D3D11 and D3D12 copies into OpenXR swapchains, a Present hook for the desktop mirror (and the D3D12 UI grab), controller actions, haptics, input emulation.
- `managed/` (BepInEx 5 plugin for Mono, same sources built for BepInEx 6 / IL2CPP): picks the game camera, renders two eye cameras each frame, captures the game's UI from the backbuffer onto a panel, draws the VR menu.
- `app/` (Rust + egui, one `Parallax.exe`): library grid from Steam libraries and manual adds, per-game settings (kept in `%APPDATA%\Parallax` across uninstalls), install/uninstall with a receipt (`.vrinstall.json`), launch, export logs, update check against GitHub releases.

## License

GPL-3.0. Third-party components: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
