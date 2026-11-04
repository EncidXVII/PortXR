# Changelog

## 0.2.0 (first public release)

### Games
- Unity 2018.4 to Unity 6, Mono (.NET 3.5 and 4.x) and IL2CPP, Direct3D 11 and 12.
- URP, HDRP and Post Processing v2 camera settings carried to the eyes; Temporal AA swapped for SMAA.
- Refuses 32-bit games, anti-cheat games (EasyAntiCheat, BattlEye) and Mono games built with code trimming, each
  with the reason.

### In VR
- Stereo 6DOF view, the game's UI on a see-through panel, head aim in first-person games.
- Hand aim (the right controller steers the game camera) and a crosshair at the depth of what you aim at.
- **Parallax** display mode: the game in 3D inside a screen, like a 3D monitor, with adjustable depth.
- **Theater** mode: the flat game on a big, curved screen. Intro and cutscene videos play there automatically.
- Screen handles while the VR menu is open: drag the bar to move the screen, the corners to resize it; kept per game.
- VR menu (double-tap the left menu button, the wrist watch, or F11): display, depth, recenter, world scale, comfort
  (snap or smooth turning, vignette), height, resolution, bindings, camera choice with previews, frame-time graph.
- Controllers as keyboard & mouse or as an Xbox pad, with rebinding, double-press, hold and grip-chord bindings.
- Laser pointer for the game's menus; Hide objects for helmets and bodies in the way.
- Per-game Lua scripts.
- Recovers when the headset runtime restarts.

### App
- Library from Steam, Epic, GOG and added folders, with cover art, search, filters and a Not installed shelf.
- **Play in VR**, **Add VR** and **Remove VR** (the game folder goes back byte for byte).
- Settings for all games, with per-game changes kept across uninstalls.
- Export logs, update check; the version, the guide and "Report a problem" at the end of Settings.
- Play in VR follows the game until VR starts, names the active VR runtime, and warns when none is set up.
- Linux build for games run through Proton.
