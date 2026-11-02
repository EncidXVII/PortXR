# Parallax

Play flat Unity games in VR. Pick a game, press **Play in VR**, and play it with your VR controllers: full stereo 6DOF view, the game's UI on a panel, a Parallax mode that shows the game in 3D inside a big screen, and a flat theater mode for anything that does not work in 3D.

**[Download the latest release](https://github.com/EncidXVII/Parallax/releases/latest)** · [Compatibility](COMPAT.md) · [Game scripts](SCRIPTING.md) · [Contributing](CONTRIBUTING.md)

## Requirements

- Windows 10 or 11, 64-bit, or Linux with Steam's Proton (see [Linux](#linux)).
- A PC VR headset and an OpenXR runtime: SteamVR, Virtual Desktop, Meta Quest Link, WiVRn or Monado.
- A 64-bit Unity game that renders with Direct3D 11 or 12 (most Unity games on Windows). See [What works today](#what-works-today).

## Use it in 3 steps

1. Start your headset runtime (SteamVR, Virtual Desktop, Quest Link, anything with OpenXR), then run `Parallax.exe`.
2. Pick a game marked **Installed** and press **Play in VR**. **Settings** (top bar) holds the settings for every game; a game's page can change any of them for that game only, with a link back to the all-games value. **Add VR** sets a game up without starting it. Parallax installs itself into the game folder; **Remove VR** puts the folder back byte for byte.
3. In game: **double-tap the left menu button**, look at the back of your left hand and pull the right trigger, or press F11 for the VR menu: display (Stereo, Parallax, Theater), 3D depth, recenter, world scale, comfort (smooth or snap turning by 15/30/45°, a vignette that darkens the edges of your view while you move), height, resolution, button bindings, camera choice with live previews, and a frame-time graph. The first time VR starts in a game, a short welcome card walks through recenter, world scale and the controls.

## Linux

Parallax runs Windows Unity games through Proton, Steam's Windows layer: unpack the Linux build and run `./Parallax`.

- **Headset:** WiVRn (standalone headsets) or Monado (wired) work best; SteamVR for Linux works too.
- **Launch options:** Proton only loads Parallax with these in the game's Steam launch options (Properties → General). The game page shows them with a **Copy launch options** button until Steam has saved them:
  `WINEDLLOVERRIDES="winhttp=n,b" PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 %command%`
- **Linux versions:** where a game has its own Linux build, Steam installs that one, and the library marks it **Linux build**. Force Proton for it (Properties → Compatibility), let Steam download the Windows version, and it is ready.
- **Steam only:** games from other stores run outside Steam's Proton; add their folder and start them from their launcher (Heroic, Lutris) with `WINEDLLOVERRIDES="winhttp=n,b"` set.

## Controls

**Auto** (the default) presents the controllers as an Xbox pad in games that ship a gamepad input library (Unity's Input System, Rewired, InControl, XInput wrappers), and as keyboard & mouse everywhere else. The keyboard & mouse layout:

| Controller | Game sees |
|---|---|
| Left stick | W A S D |
| Right stick | Mouse look |
| Right / left trigger | Left / right mouse button |
| A, B, X, Y | Space, Left Ctrl, E, R |
| Left / right grip | Left Shift, Q |
| Left stick click | C |
| Left menu: tap / hold / double-tap | Esc / Tab / VR menu |
| Point at the UI panel + trigger | Mouse cursor + click (when the game shows a cursor) |

Every button except the sticks can be rebound to a key or mouse button, in the VR menu (Controls tab) or in Parallax.exe (Buttons, for all games or one). Each button can also press another key on a double press, when held for half a second, or while the same hand's grip is held (grip + button). The menu warns when two bindings press the same key.

**Off** (Controls, VR menu Quick or the app) leaves the game to your own keyboard, mouse or gamepad: the controllers then only work the VR menu and the laser pointer.

**Gamepad** mode (VR menu, Quick) presents the controllers as an Xbox pad instead. Games on Unity's Input System package and games that read XInput see it. Unity 2021+ games on the old input manager read controllers through Windows.Gaming.Input and do not; keep keyboard & mouse there.

Something wrong? Select the game in Parallax.exe and press **Export logs**. It saves one zip to your desktop to attach to a report.

## What works today

- Direct3D 11 games from Unity 2018.4 to Unity 6: Mono (both .NET 3.5 and 4.x runtimes) and IL2CPP games (BepInEx 6), URP with its post-processing.
- Direct3D 12 games as well, with the same features.
- The game's URP, HDRP or Post Processing v2 camera settings are copied to the eyes, so bloom, tonemapping and the rest show up in VR. Temporal AA is swapped for SMAA (it smears in stereo).
- The game window keeps showing the left eye.
- Head aim: in first-person games the game camera turns to where you look, so you aim with your head; the right stick turns your body. Auto (the default) turns it on while you play (cursor hidden), tries it briefly on each new camera, and keeps it off for third-person cameras that orbit the player, where the right stick turns the game camera as on a flat screen; On or Off in Quick, Controls.
- The game's UI floats on a see-through panel. When a game paints that panel solid (a full-screen effect on its UI camera), Parallax notices and makes the black see-through, dim tints translucent (View, UI black: Auto, See-through or Keep).
- Intro and cutscene videos that a game draws straight onto its camera play on the theater screen, then the view returns to stereo.
- A round watch face sits on the back of your left wrist; it lights up when you glance at it, and a right trigger pull then opens the VR menu.
- A laser beam shows where the right controller points while the pointer is on (VR menu, or whenever the game shows its cursor). The pointer clicks the game's menus through the game itself, so it works with the game window minimized or behind other windows; the VR menu always takes the pointer first while it is open.
- Something in the way of your view, like a helmet or the player's own body? VR menu, Game tab, Hide objects: point at it and pull the trigger. Parallax remembers it for that game; Show hidden brings it back.
- 2D games drawn by a perspective camera (Hollow Knight) are scaled so their play plane sits 2.5 m away: the game's own foreground and background layers show their depth. Orthographic 2D games play on the theater screen.
- Per-game Lua scripts fix what one game needs (camera choice, hidden objects, eye offset, game component fields): see [SCRIPTING.md](SCRIPTING.md).
- Parallax mode shows the game in 3D inside the theater screen, played as on a 3D monitor: the game's own camera steers the view, your head does not (no head aim or turning). The screen's plane sits at what is in the middle of the view (the character, the wall ahead, a 2D game's play plane), nearer things come out of it and farther ones go behind; **3D depth** sets the strength. Cameras drawn over the game (a weapon, a HUD) and the game's UI stay flat on the screen.
- Theater mode shows the flat game on a big screen 3 m away; the laser pointer and the wrist menu work there too. The screen is curved on runtimes with curved layers (Virtual Desktop, Meta), and flat on SteamVR, which has none.
- The Theater and Parallax screen has handles while the VR menu is open: faint corner brackets and a bar under the middle, lime under the laser. Drag the bar with the trigger to put the screen anywhere around you (up over a bed, off to one side), drag a corner to resize it; while dragging, the right stick pushes it away or pulls it in (up, down) and sizes it (right, left). **RESET** beside the bar brings back 3 m ahead. Each game keeps its screen. A screen raised or lowered past about 12° shows flat, since the curve wraps around you at eye level.
- If the headset runtime restarts or the session drops, the game continues flat and VR comes back by itself once the runtime is up again.
- IL2CPP games may have stripped engine calls; affected extras switch off with a log line instead of breaking the game.
- Not supported: Vulkan and OpenGL games, 32-bit games (no OpenXR runtime supports them). Anti-cheat games (EasyAntiCheat, BattlEye) are refused on purpose. Mono games built with Unity's code trimming (rare; off by default) are marked as not supported: the mod loader cannot start in them.
- See [COMPAT.md](COMPAT.md).

## Build

Needs Visual Studio 2022 Build Tools (C++ and .NET), CMake 3.24+, .NET SDK 8+, Rust (stable, MSVC toolchain).

```
.\build.ps1
```

`dist\` then holds `Parallax.exe` and the payloads it copies into games (`payload\` for Mono, `payload-il2cpp\` for IL2CPP). The IL2CPP payload needs interop assemblies generated from fixture F3 (see Test); without them the build warns and skips it.

`.\release.ps1` builds and packs `release\Parallax-<version>.zip` with `SHA256SUMS.txt`. CI (`.github/workflows/build.yml`) runs `release.ps1 -MonoOnly` on every push, because the runner has no Unity to generate the interop assemblies.

## Test

The harness runs a fixture game against SteamVR's null driver (no headset needed), drives it through a UDP test channel, and asserts eye order, orientation, scale, tracking, timing, UI, input, theater, Parallax window, menu, stability and a byte-identical uninstall. SteamVR must be closed; its settings are restored afterwards.

```
pip install pillow numpy
python harness\run.py --fixture F1 --soak 600
```

Test windows open on the main screen; `--secondary` moves them to a second monitor. The input gate takes over the real mouse and keyboard for a few seconds, so it only runs with `--input`.

Fixtures are generated from `fixtures/shared` with `fixtures\build.ps1 -Name F1`, then tested with `python harness\run.py --fixture F1`. They need the matching Unity editor, by default under `E:\Unity\Editor\<version>` (`-Editors` to change).

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
| F9 | 2022.3 | Mono, code trimming on | D3D11 | built-in (detection only: the app refuses it) |
| F10 | 2022.3 | Mono | D3D11 | HDRP + Bloom |
| F11 | 2022.3 | Mono | D3D11 | built-in + Post Processing v2 (Bloom) |

The IL2CPP plugin compiles against interop assemblies BepInEx 6 generates on a game's first launch. After building F3, `fixtures\interop.ps1` runs a scratch copy once without a window and fills `build\deps\interop-f3`.

## How it works

- `native/` (C++20, `parallax_xr.dll`): OpenXR session and frame loop on Unity's render thread, D3D11 and D3D12 copies into OpenXR swapchains, a Present hook for the desktop mirror (and the D3D12 UI grab), controller actions, haptics, input emulation.
- `managed/` (BepInEx 5 plugin for Mono, same sources built for BepInEx 6 / IL2CPP): picks the game camera, renders two eye cameras each frame, captures the game's UI from the backbuffer onto a panel, draws the VR menu.
- `app/` (Rust + egui, one `Parallax.exe`): library grid from Steam, Epic, GOG and manual adds (plus a "Not installed" shelf of the Steam library's Unity games, matched against Wikidata's list of Unity games, that installs through Steam and flags multiplayer-only and 32-bit games first), per-game script import with search, status and store filters and a recently-played order, settings for all games with per-game changes (kept in `%APPDATA%\Parallax` across uninstalls; a game follows a changed all-games value unless it was set differently for that game, in the app or its VR menu), install/uninstall with a receipt (`.vrinstall.json`), launch, export logs, update check against GitHub releases. `Parallax.exe <gameDir>` opens straight to that game's page.

## Safety and privacy

- **Single-player only.** Parallax refuses games with EasyAntiCheat or BattlEye and flags multiplayer-only games. Modding an online game can break its terms of service and get an account banned; do not use Parallax for that.
- **Reversible.** Adding VR copies BepInEx and the plugin into the game folder and records every file in `.vrinstall.json`; **Remove VR** puts the folder back byte for byte. Steam's "Verify integrity of game files" leaves the added files in place; Remove VR is the way out.
- **Network.** The app asks GitHub for the latest release (it shows a link; it never downloads or runs an update by itself) and Wikidata for its list of Unity games, for the Not installed shelf. Nothing else leaves your PC: no account, no telemetry. Logs are only collected when you press **Export logs**, into a zip on your desktop.

Parallax is an independent project, not affiliated with or endorsed by Unity Technologies, Valve, Meta, or any game's developer or publisher. Game names are trademarks of their owners.

## Contributing

Bug reports, game reports and per-game scripts are welcome: see [CONTRIBUTING.md](CONTRIBUTING.md). Security issues: [SECURITY.md](SECURITY.md). Changes per version: [CHANGELOG.md](CHANGELOG.md).

## License

GPL-3.0. Third-party components: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
