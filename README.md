# Parallax: play flat Unity games in VR

[![Latest release](https://img.shields.io/github/v/release/EncidXVII/Parallax?color=c6ff1a&labelColor=0a0a0a)](https://github.com/EncidXVII/Parallax/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/EncidXVII/Parallax/total?color=c6ff1a&labelColor=0a0a0a)](https://github.com/EncidXVII/Parallax/releases)
[![Build](https://img.shields.io/github/actions/workflow/status/EncidXVII/Parallax/build.yml?branch=main&labelColor=0a0a0a)](https://github.com/EncidXVII/Parallax/actions/workflows/build.yml)
[![License: GPL-3.0](https://img.shields.io/github/license/EncidXVII/Parallax?color=c6ff1a&labelColor=0a0a0a)](LICENSE)

**Parallax is a free, open-source VR mod for Unity games on PC.** Pick a game you already own, press **Play in VR**,
and play it in full 6DOF stereo with motion controllers: aim with your head or your hand, the gun in your hand, the
game's UI on a panel you can point at. Games that don't suit 3D play on a big screen instead. It works with any
OpenXR headset: Meta Quest 2, 3 and Pro (Link, Air Link, Virtual Desktop), Valve Index, HTC Vive, Pico, Windows Mixed
Reality and others through SteamVR, on Windows and on Linux through Proton.

**[Download the latest release](https://github.com/EncidXVII/Parallax/releases/latest)** · [Compatibility](COMPAT.md) · [FAQ](#faq) · [Game scripts](SCRIPTING.md) · [Contributing](CONTRIBUTING.md)

<img src="docs/images/vr-menu.png" width="560" alt="The VR menu's Quick tab: Display (Stereo, Parallax, Theater), Recenter, World scale, Turning, Comfort vignette, Controls, Aim and Aim with">

## Highlights

- **One click per game.** A desktop app finds your Unity games on Steam, Epic and GOG, adds VR to one and starts it.
  **Remove VR** puts the game folder back byte for byte.
- **Real VR, not a flat screen.** Stereo 6DOF view at real scale, smooth or snap turning, comfort vignette, recenter.
- **Motion controls without setup.** Controllers act as keyboard & mouse or an Xbox pad; every button can be rebound.
- **Head aim or hand aim.** The game camera follows your head or your right controller; the game's gun moves into
  your hand, and its crosshair sits on what you aim at.
- **Three ways to play.** Stereo (stand in the game), Parallax (the game in 3D inside a big screen, like a 3D
  monitor) and Theater (the flat game on a big screen, curved on Virtual Desktop and Meta).
- **Broad Unity support.** Unity 2018.4 to Unity 6, Mono and IL2CPP, Direct3D 11 and 12, built-in, URP and HDRP, with
  the game's bloom and tonemapping kept.
- **Per-game fixes** in small Lua scripts that anyone can share.
- **Safe by design.** Single-player only: anti-cheat games are refused. No account, no telemetry.

## Requirements

- Windows 10 or 11, 64-bit, or Linux with Steam's Proton (see [Linux](#linux)).
- A PC VR headset and an OpenXR runtime: SteamVR, Virtual Desktop, Meta Quest Link, WiVRn or Monado.
- A 64-bit Unity game that renders with Direct3D 11 or 12 (most Unity games on Windows). See [What works today](#what-works-today).

## Use it in 3 steps

1. Start your headset runtime (SteamVR, Virtual Desktop, Quest Link, anything with OpenXR), then run `Parallax.exe`.
2. Pick a game marked **Installed** and press **Play in VR**. **Settings** (top bar) holds the settings for every game; a game's page can change any of them for that game only, with a link back to the all-games value. After **Play in VR** the page says when the game is in VR (or what to check if it starts flat), and later when you last played it in VR. **Add VR** sets a game up without starting it. Parallax installs itself into the game folder; **Remove VR** puts the folder back byte for byte.
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

Every button except the sticks can be rebound to a key or mouse button, in the VR menu (Controls tab) or in Parallax.exe (**Button layout** on a game's page, or Settings → Buttons for all games). Each button can also press another key on a double press, when held for half a second, or while the same hand's grip is held (grip + button). The menu warns when two bindings press the same key.

**Off** (Controls, VR menu Quick or the app) leaves the game to your own keyboard, mouse or gamepad: the controllers then only work the VR menu and the laser pointer.

**Gamepad** mode (VR menu, Quick) presents the controllers as an Xbox pad instead. Games on Unity's Input System package and games that read XInput see it. Unity 2021+ games on the old input manager read controllers through Windows.Gaming.Input and do not; keep keyboard & mouse there.

Something wrong? Select the game in Parallax.exe and press **Export logs**. It saves one zip to your desktop to attach to a report.

## What works today

- Direct3D 11 games from Unity 2018.4 to Unity 6: Mono (both .NET 3.5 and 4.x runtimes) and IL2CPP games (BepInEx 6), URP with its post-processing.
- Direct3D 12 games as well, with the same features.
- The game's URP, HDRP or Post Processing v2 camera settings are copied to the eyes, so bloom, tonemapping and the rest show up in VR. Temporal AA is swapped for SMAA (it smears in stereo).
- The game window keeps showing the left eye.
- Head aim: in first-person games the game camera turns to where you look, so you aim with your head; the right stick turns your body. Auto (the default) turns it on while you play (cursor hidden), tries it briefly on each new camera, and keeps it off for third-person cameras that orbit the player, where the right stick turns the game camera as on a flat screen; **Aim** On or Off in the VR menu's Quick tab or the app.
- Hand aim (the default): **Aim with: Right hand** (VR menu Quick, or the app) steers the same way from the right controller instead of your head: the game camera points where the controller points while your view stays on your head. The game still walks where its camera faces, so forward follows your hand. **Weapon in hand** (VR menu Game tab) moves the gun and arms an FPS hangs under its camera into your hand while the view is drawn.
- Crosshair: the game's own crosshair sits in the middle of its UI panel, which stays put while the camera follows your head or hand. While aiming, Parallax moves the middle of the game's UI (the crosshair with its grab and use icons, prompts and hit markers) to what the camera aims at, at its depth. **Crosshair** (VR menu Game tab): Game's, Lime for a plain lime crosshair there instead, or Off.
- The game's UI floats on a see-through panel. When a game paints that panel solid (a full-screen effect on its UI camera), Parallax notices and makes the black see-through, dim tints translucent (View, UI black: Auto, See-through or Keep).
- Intro and cutscene videos that a game draws straight onto its camera play on the theater screen, then the view returns to stereo.
- A round watch face sits on the back of your left wrist; it lights up when you glance at it, and a right trigger pull then opens the VR menu.
- A laser beam shows where the right controller points while the pointer is on (VR menu, or whenever the game shows its cursor). The pointer clicks the game's menus through the game itself, so it works with the game window minimized or behind other windows; the VR menu always takes the pointer first while it is open.
- Something in the way of your view, like a helmet or the player's own body? VR menu, Game tab, Hide objects: point at it and pull the trigger. Parallax remembers it for that game; Show hidden brings it back.
- 2D games drawn by a perspective camera (Hollow Knight) are scaled so their play plane sits 2.5 m away: the game's own foreground and background layers show their depth. Orthographic 2D games play on the theater screen.
- Per-game Lua scripts fix what one game needs (camera choice, hidden objects, eye offset, game component fields): see [SCRIPTING.md](SCRIPTING.md).
- Parallax mode shows the game in 3D inside the theater screen, played as on a 3D monitor: the game's own camera steers the view, your head does not (no head aim or turning). The screen's plane sits at what is in the middle of the view (the character, the wall ahead, a 2D game's play plane), nearer things come out of it and farther ones go behind; **3D depth** sets the strength. Cameras drawn over the game (a weapon, a HUD) and the game's UI stay flat on the screen.
- Theater mode shows the flat game on a big screen 3 m away; the laser pointer and the wrist menu work there too. The screen is curved on runtimes with curved layers (Virtual Desktop, Meta), and flat on SteamVR, which has none.
- The Theater and Parallax screen is locked while you play. To move it, open the VR menu and set **Screen** to **Unlocked** in Quick; it locks again when you close the menu. Unlocked, it has handles: faint corner brackets and a bar under the middle, lime under the laser. Drag the bar with the trigger to put the screen anywhere around you (up over a bed, off to one side), drag a corner to resize it; while dragging, the right stick pushes it away or pulls it in (up, down) and sizes it (right, left). **RESET** beside the bar brings back 3 m ahead. Each game keeps its screen. A screen raised or lowered past about 12° shows flat, since the curve wraps around you at eye level.
- If the headset runtime restarts or the session drops, the game continues flat and VR comes back by itself once the runtime is up again.
- IL2CPP games may have stripped engine calls; affected extras switch off with a log line instead of breaking the game.
- The VR menu's Library tab quits the game, or starts another game with VR added without taking the headset off.
- Theater and Parallax can show your room around the screen instead of black (**Around the screen: Your room**), on headset software that shares its passthrough with apps; SteamVR does not.
- **Godot 4 games** get full VR from the engine's own OpenXR: **Add VR** puts an `override.cfg` and one script beside
  the game's exe (Remove VR takes them out again). The headset follows the game's camera, also where the game draws its
  3D in a SubViewport (a retro resolution); the game's 2D UI sits on a panel in front; the controllers act as a gamepad
  inside the game. Play in VR wakes the headset runtime first, since Godot gets no VR from a runtime still starting.
  Not yet: a gun or arms layer drawn in its own viewport, the laser pointer, the VR menu.
- **Games in other engines** (Unreal, Godot 3, anything else) are listed too and play on the Theater screen: Parallax
  shows the game's window in VR (Windows' window capture) and turns the controllers into keyboard and mouse, with a
  laser pointer that clicks wherever the game shows its cursor. Nothing is added to the game or its folder; double-tap
  the left menu button to bring the screen in front. Anti-cheat and online-only games are refused here too.
- Not supported: Vulkan and OpenGL games, 32-bit games (no OpenXR runtime supports them). Anti-cheat games (EasyAntiCheat, BattlEye) are refused on purpose. Mono games built with Unity's code trimming (rare; off by default) are marked as not supported: the mod loader cannot start in them. Multiplayer-only games without anti-cheat, games with their own VR mode and trimmed games have **Add VR anyway**, after a caution.
- See [COMPAT.md](COMPAT.md).

## FAQ

**Can I play my Unity game in VR with Parallax?** Most likely, if it is a 64-bit Windows Unity game that uses
Direct3D 11 or 12 and has no anti-cheat. The app tells you per game, before anything is installed. Tested titles and
known issues are in [COMPAT.md](COMPAT.md); report how yours went with a game report.

**Does it work with Meta Quest 3, Quest 2 or Quest Pro?** Yes, through Quest Link, Air Link, Virtual Desktop or
Steam Link, as a PC VR headset. Parallax runs on the PC; the Quest's standalone mode is not supported.

**Which headsets work?** Any headset with an OpenXR runtime on the PC: SteamVR (Index, Vive, Pico, Windows Mixed
Reality, Bigscreen Beyond), Virtual Desktop, Meta Quest Link, WiVRn and Monado on Linux.

**Is it free?** Yes, free and open source under the GPL-3.0.

**Can I get banned?** Parallax refuses games with EasyAntiCheat or BattlEye and flags multiplayer-only games. Use it
for single-player games only.

**Does it change my game files?** It adds BepInEx and the Parallax plugin to the game folder and records every file.
**Remove VR** restores the folder exactly; nothing else on your PC changes.

**My game starts flat. What now?** Start your VR runtime first (SteamVR, Virtual Desktop or Quest Link) and put the
headset on. The game page names the runtime Parallax will use and says what to check if VR does not start. Still
flat? **Export logs** and open an issue.

**Does it work with Unreal, Godot or other engines?** No, only Unity games.

**How is it different from a game's own VR mode?** Parallax adds VR to games that have none. Games with their own VR
mode are marked **Already VR**; start those in VR from Steam.

## Build

Needs Visual Studio 2022 or later Build Tools (C++ and .NET), CMake 3.24+, .NET SDK 8+, Rust (stable, MSVC toolchain). Downloads (BepInEx, the OpenXR loader) are pinned by SHA-256.

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

Godot VR has its own gate: `python harness\godot.py --secondary` runs fixture G1 (`fixtures/G1`, a Godot 4.5 project:
a room of coloured boxes, a HUD, a splash camera before the player's) with the app's `override.cfg` and checks what
SteamVR's compositor shows the test headset: the world in the player camera's view and the HUD on its panel. It needs
the Godot 4.5 editor (`GODOT`, default `E:\Godot\Godot_v4.5-stable_win64_console.exe`); G1 runs as a project, so no
export templates.

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
