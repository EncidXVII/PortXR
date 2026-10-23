# Parallax

Play flat Unity games in VR. Pick a game, press **Play in VR**, and play it with your VR controllers: full stereo 6DOF view, the game's UI on a panel, and a big-screen theater mode for anything that does not work in stereo.

Working name, not final.

## Use it in 3 steps

1. Start your headset runtime (SteamVR, Virtual Desktop, Quest Link, anything with OpenXR), then run `Parallax.exe`.
2. Pick a game marked **Ready** and press **Play in VR**. Parallax installs itself into the game folder; **Uninstall** puts the folder back byte for byte.
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

**Gamepad** mode (VR menu, Controls) presents the controllers as an Xbox pad instead. Only games that read XInput directly see it; Unity 2021+ games using the old input manager read controllers through Windows.Gaming.Input and will not.

## What works today

- Unity Mono games (Unity 2018.1+ with the .NET 4.x runtime), Direct3D 11, built-in render pipeline. URP renders but without its post-processing.
- Not yet: IL2CPP, Direct3D 12, .NET 3.5 runtime games, HDRP specifics. Anti-cheat games (EasyAntiCheat, BattlEye) are refused on purpose.
- See [COMPAT.md](COMPAT.md).

## Build

Needs Visual Studio 2022 Build Tools (C++ and .NET), CMake 3.24+, .NET SDK 8+.

```
.\build.ps1
```

`dist\` then holds `Parallax.exe` and the `payload\` it copies into games.

## Test

The harness runs a fixture game against SteamVR's null driver (no headset needed), drives it through a UDP test channel, and asserts eye order, orientation, scale, tracking, timing, UI, input, theater, menu, stability and a byte-identical uninstall. SteamVR must be closed; its settings are restored afterwards.

```
pip install pillow numpy
python harness\run.py --soak 600
```

Fixture F1 is generated from `fixtures/F1` with Unity 2022.3 (`-executeMethod FixtureBuild.Build`).

## How it works

- `native/` (C++20, `parallax_xr.dll`): OpenXR session and frame loop on Unity's render thread, D3D11 copy into OpenXR swapchains, controller actions, haptics, input emulation.
- `managed/` (BepInEx 5 plugin): picks the game camera, renders two eye cameras each frame, captures the game's UI from the backbuffer onto a panel, draws the VR menu.
- `app/` (`Parallax.exe`): library, install/uninstall with a receipt (`.vrinstall.json`), launch.

## License

GPL-3.0. Third-party components: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
