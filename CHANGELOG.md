# Changelog

## Unreleased

### Look and motion
- Themes: Lime, Mint, Aqua, Sky, Violet, Pink, Amber or White as the accent, the same eight as DesktopVR and
  FitnessVR. Pick it in the app (Settings > Look) or the VR menu (View > Theme); one choice for every game and the
  app, and a running game picks up a change from the app within seconds. The laser beam, aim crosshair and lit
  watch follow it too. The crosshair choice "Lime" now reads "Parallax's" (same setting).
- In line with DesktopVR: neutral graphite greys, filled cells with soft corners instead of outlines, a chosen
  option filled ink (lime stays for the active tab, the main action and the library's status filter). Same screens,
  same controls.
- App: Barlow for body text and Barlow SemiBold for the spaced capital labels (nav, filters, chips), both bundled;
  Anton stays for titles. Text one step bigger, rounded covers, chips and buttons, a white slider knob.
- VR menu: the same tabs and rows, restyled; Barlow where installed, else Segoe UI, for labels and tabs too.
  Rounded corners in Mono games; IL2CPP games keep square cells (their engine build has no way to make the texture).
- Motion, following Windows' "Animation effects": the VR menu fades in, the tab bar and a row's choice slide, hovers
  ease, presses show on trigger-down; in the app the tab bar grows into place and covers light up on hover.

### In VR
- The screen in Theater and Parallax is locked while you play: its handles show only after **Screen: Unlocked** in
  the VR menu's Quick tab, and it locks again when the menu closes.
- The wrist watch fades in as you look at it, like DesktopVR's clock, and is out of sight otherwise; glanced at, its
  rim and glyph turn lime as before. It shows at a wide glance and holds once shown, so it never flickers at the
  edge. A grey rim and round-ended bars replace the white rim.
- **Library** tab in the VR menu: **Quit game**, or **Start** another game with VR added (Parallax.exe waits for this
  game to close, updates the plugin and launches the next one). Both ask twice, since unsaved progress is lost.
- **Around the screen: Your room** (Quick tab, Theater and Parallax): passthrough around the screen instead of black, where the
  headset software shares it with apps (alpha-blend passthrough or Meta's passthrough extension); the row only shows
  there. SteamVR has neither.

### App
- **Add VR anyway** for games Parallax would leave alone but could run in: multiplayer-only games without
  anti-cheat, games with their own VR mode, and Mono games built with code trimming. The page says what to expect
  first and asks again. Anti-cheat, 32-bit and Linux-build games stay refused.
- Settings: **Around the screen** (Dark or Your room) for Theater and Parallax.

### Tests
- Test sessions turn off SteamVR's startup overlay apps and leftover desktop windows (no "already running" dialogs,
  no Settings window over the test headset); the player's settings come back after.

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
