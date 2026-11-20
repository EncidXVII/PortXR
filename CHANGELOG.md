# Changelog

## Unreleased

- **Uninstall PortXR**: `Uninstall PortXR.cmd` in its folder (or `PortXR.exe --uninstall`) takes VR out of every game
  it was added to and deletes PortXR's settings. Game scripts stay. Until now each game had to be done by hand.

### One core for every engine
- Unity and Godot games now run the same player rig, VR menu and screen handles from PortXR's native core: aim,
  snap turning, the vignette, the laser pointer, the wrist watch, the Theater and Parallax screen. A fix or feature
  lands in every engine at once; the Unity plugin only reports the game camera and renders the eyes.
- **Add VR** puts Godot 4.3+ games on that core (a `PortXR` folder beside the exe, on Direct3D 12), with the
  laser pointer, VR menu and the game's own settings page in the app, as Unity games have. Games set up the older way
  move over on their next Play. Godot 4.0 to 4.2, and custom builds without Direct3D 12 (Vulkan
  only), keep the engine's own OpenXR instead of being refused.
- **Unreal Engine 4.22 to 5.6 games get full VR** on the same core. **Add VR** puts a `dwmapi.dll` beside the game's
  Shipping exe (Remove VR puts the folder back): the engine renders both eyes itself, the game's UI sits on a panel in
  front, and the controllers act as a gamepad. Full VR, 3D screen and VR screen (the 3D screen's view, flat), head and hand
  aim, the crosshair, laser pointer, watch and VR menu. Play starts the VR runtime first; started without a headset, the
  game runs untouched. Runs on SteamVR's test headset in Direct3D 12; not yet tried on a real one. UE 5.5 and newer,
  which dropped the engine's switch for keeping the UI out of the eyes, draw it into a texture of its own the size of
  the window, so it shows whole on the panel as in older versions.
- **Unreal games' own objects** (UE 4.25 and newer): PortXR now reads the engine's object list, names, classes,
  properties and functions, found and checked in each game rather than per version. With it, Unreal games get the VR
  menu's **Game camera** picker, **Hide objects** and **Weapon in hand** as Unity and Godot games have, and scripts reach
  the game's actors and components: find, hide, move, read or set any property, call any function (SCRIPTING.md).
  `on_scene` gets the map's name and `on_camera` the camera the view follows. Tried on UE 4.26, 4.27, 5.0, 5.4 and 5.5 games.
- **Console commands** for Unreal games: a game's Advanced settings take console variables (`r.MotionBlurQuality 0;
  r.DepthOfFieldQuality 0`), run as each level starts, for the effects that look wrong in VR. Scripts' `call` now passes
  strings and names too, so `KismetSystemLibrary.ExecuteConsoleCommand` and the rest work from scripts.
- The crosshair (Game's or PortXR's) now comes from the core, so Godot and Unreal games have it too. Godot games also
  get **Hide objects** and **Weapon in hand** in the VR menu's Game tab, as Unity games have.
- Godot games get what Unity games had: the body's heading stays steady when the game camera looks straight up or
  down, and the screen handles pause the game's input.
- Godot: the real mouse cursor no longer jumps to the window's corner while the laser points at the game.
- The VR menu repaints about 15 times faster (1.6 ms instead of 24 ms), so opening it no longer costs frames.

- **Game scripts in every engine.** Godot and Unreal games now run the same per-game Lua scripts as Unity games (same
  folder, callbacks and `portxr` calls), from a Lua 5.4 inside the core. Godot scripts also reach the game's nodes:
  find, show and hide, move, read or set any property, call a method. A script stuck in a loop is stopped after a second instead of freezing the game.
  The app's **Scripts** section is on every Full VR game's page.
- **Auto resolution** in Godot and Unreal games, as in Unity's: the eyes drop 10% at a time while the headset misses
  frames, come back with headroom, and stop trying when the game is CPU-bound. It now lives in the core.
- Godot games keep the **Game camera** picked in the VR menu or the app across starts (it applied only until the game
  closed). The VR menu's **Library** tab knows a running Unreal game as the current one (it offered to start it again),
  and its Game tab no longer shows an empty camera picker. The app hides **UI black** for Godot and Unreal games, where
  it doesn't apply.

### Library
- Every game says what you get: **Full VR** (you stand inside the game; lime once VR is set up, so Play goes straight
  in), **VR screen** (the game on a big screen in VR, any engine), or why PortXR can't run it. Tabs: Full VR, VR
  screen, Not installed and Unsupported; All leaves the unsupported games out. A line under the tabs says what the
  words mean.
- **Not installed** shows only Steam library games that would get Full VR; games PortXR can't run (anti-cheat,
  multiplayer only, their own VR mode, 32-bit) are left off it. Any game you install plays on the VR screen anyway.
  It now lists Unreal Engine 4 and 5 games and Godot 4 games too, not only Unity ones.
- Steam apps that aren't games (VTube Studio, XSOverlay) are no longer listed. Unreal games whose game exe sits deeper
  or beside a launcher of the same kind (Marvel Rivals, WARDOGS) are recognised, and the anti-cheat installer or a
  developer console is no longer taken for a game's exe.
- Cover art for games newer Steam clients cache under new names (`library_capsule.jpg`, `library_header.jpg`): about
  160 games in a large library showed only their initial.
- The name in the top bar lines up with the page under it, and the store and order menus stay beside the status tabs
  whenever they fit (they dropped to a row of their own at most window sizes), wrapping under the tabs from the left.
- **Open log** and **Export logs** work for Godot and Unreal games too (`PortXR\portxr.log` beside the exe, where
  script errors land); the exported `game.txt` names the real engine instead of calling every game Unity.
- No more sweeping line under the title each time the window comes back into focus; the list just updates.
- One set of words everywhere: the display modes are now **Full VR**, **3D screen** and **VR screen** (were Stereo,
  Parallax and Theater) in the app, the VR menu and the library. Saved settings carry over.
- **Play in VR** adds VR itself when it is missing; the separate Add VR button is gone, and every Full VR game shows
  the same lime chip.
- Game page: Play and the button layout up top; the engine, folder, logs and **Remove VR** sit in a quiet
  Troubleshooting row at the bottom instead of a red button beside Play.
- Library: your games first, then the Steam games that would get Full VR under their own heading. Games PortXR
  can't run keep their Unsupported tab, out of All. The toolbar fits on one row again.
- Not installed finds about twice as many Steam games: beyond Wikidata's engine list (which knows the engine of
  about one game in six), Steam's own app cache gives the engine away through where a game keeps its cloud saves
  (Unity's AppData\LocalLow, Unreal's Saved\SaveGames, Godot's app_userdata). Nothing extra leaves the PC.
- Fewer risky games offered: anti-cheat now also comes from the community-kept Are We Anti-Cheat Yet? list (Steam
  names it for few games; 7 Days to Die, Gears 5 and Aimlabs got through), built in and refreshed weekly, so it holds
  offline and on a first start too, and from Easy Anti-Cheat and BattlEye
  launcher names. Competitive online games with a token single-player mode (Verdun, Battlerite, Tekken 7's ranked
  play) are marked "Competitive online": Steam's Online PvP plus a competitive tag among the game's top five.
- The VR menu costs the game less: a repaint averages 1.1 to 1.6 ms (was 1.6 to 2.4) and at most 6.5 ms (was 9.1).
  Cover art is shrunk once to its card size (sharper too) instead of resampled every paint, and rounded shapes fill
  their corner rows in one span. The log says what each opening of the menu cost.
- The app uses about a quarter of the memory: covers load only when on screen, and at most 60 stay held (462 MB to
  126 MB private with a 200-game library).
- The library lists only Unity, Unreal and Godot games: games in other engines are no longer listed for the VR screen.
- Unreal 5.5 and newer: the game could crash a minute or so in. The engine's garbage collector took the texture the
  game's UI is drawn into while the game still drew into it; the engine now holds those textures. A new one is made
  only once the window keeps a new size for half a second (dragging an edge made dozens, about 14 MB each at 1440p),
  and a size the window returns to (a fullscreen toggle) uses its texture again. Known: resizing the window during VR
  can still crash the game, about once in 70 size changes in a stress test.
- Unreal games get the VR screen: the game flat on a big screen, as in Unity and Godot games (the engine always draws
  both eyes, so it is the 3D screen's view with no depth). UE 5.5 and newer, Direct3D 11: the desktop window shows the
  left eye during VR instead of the last frame from before. While a loading screen
  shows, the laser and the wrist watch now draw over it.
- Linux: Unreal and Godot 4.3+ games are listed and get full VR through Proton. The Steam launch options gain
  `dwmapi` (`WINEDLLOVERRIDES="winhttp,dwmapi=n,b"`), which loads the Unreal core; the page asks only for what each
  game needs.
- The README and app speak of Unity, Unreal and Godot games, and no longer promise camera previews in the VR menu
  (it lists the game's cameras by name).
- Godot and Unreal games: after Play, the game page now says when the game is in VR (and later when it was last played
  in VR) instead of "No VR yet" after 90 seconds.
- Opening a Godot or other non-Unity game's folder (`PortXR.exe <folder>`) opens its page instead of saying no
  Unity game was found.

### Look and motion
- Themes: Lime, Mint, Aqua, Sky, Violet, Pink, Amber or White as the accent, the same eight as WristXR and
  SweatXR. Pick it in the app (Settings > Look) or the VR menu (View > Theme); one choice for every game and the
  app, and a running game picks up a change from the app within seconds. The laser beam, aim crosshair and lit
  watch follow it too. The crosshair choice "Lime" now reads "PortXR's" (same setting).
- In line with WristXR: neutral graphite greys, filled cells with soft corners instead of outlines, a chosen
  option filled ink (lime stays for the active tab, the main action and the library's status filter). Same screens,
  same controls.
- App: Barlow for body text and Barlow SemiBold for the spaced capital labels (nav, filters, chips), both bundled;
  Anton stays for titles. Text one step bigger, rounded covers, chips and buttons, a white slider knob.
- VR menu: the same tabs and rows, restyled, with the app's Barlow and Anton faces. It is now one menu for every
  engine, drawn by PortXR itself on its own headset layer over the UI panel or screen: Unity games (Mono and IL2CPP
  alike) get rounded corners and the app's fonts, and the game's UI capture no longer carries it. The Game tab lists
  cameras by name.
- Motion, following Windows' "Animation effects": the VR menu fades in, the tab bar and a row's choice slide, hovers
  ease, presses show on trigger-down; in the app the tab bar grows into place and covers light up on hover.

### In VR
- **Aim with** now starts on **Right hand**: in first-person games the game camera follows the right controller,
  your view your head. Head stays one choice away (VR menu Quick, or the app).
- Parallax mode no longer pulls the picture apart a few seconds in: the eyes were spaced by the focus distance, so
  looking down a long street put them metres apart (Peripeteia). They now stay within two eye-widths at the world
  scale, the focus stops at 15 m, and it eases over about 0.7 s.
- UI panel: HUD boxes and crosshairs no longer come out see-through. Once the game camera draws off screen (games
  whose effects painted over the UI, such as HDRP's), the panel is measured again and only keyed if something
  still paints the screen; and translucent UI keeps the opacity it has on the monitor (older Unity UI shaders store
  it squared on a transparent screen; where the colour shows that, the opacity is raised back).
- The screen in Theater and Parallax is locked while you play: its handles show only after **Screen: Unlocked** in
  the VR menu's Quick tab, and it locks again when the menu closes.
- The wrist watch fades in as you look at it, like WristXR's clock, and is out of sight otherwise; glanced at, its
  rim and glyph turn lime as before. It shows at a wide glance and holds once shown, so it never flickers at the
  edge. A grey rim and round-ended bars replace the white rim.
- **Library** tab in the VR menu: **Quit game**, or **Start** another game with VR added (PortXR.exe waits for this
  game to close, updates the plugin and launches the next one). Both ask twice, since unsaved progress is lost.
  Laid out like WristXR's window picker: a Now playing card, then your VR games as a shelf of Steam covers (the
  initial where a game has none, and in IL2CPP games), five at a time.
- **Around the screen: Your room** (Quick tab, Theater and Parallax): passthrough around the screen instead of black, where the
  headset software shares it with apps (alpha-blend passthrough or Meta's passthrough extension); the row only shows
  there. SteamVR has neither.

### App
- **Godot 4 games in full VR**: the game's own OpenXR, switched on by an `override.cfg` and a script beside its exe
  (Add VR; Remove VR restores the folder byte for byte). The headset follows the game's camera, the 2D UI is a panel
  in front, the controllers are a gamepad inside the game. Play in VR wakes the headset runtime first
  (PortXRTheater.exe --wake). Godot 3 games play on the Theater screen.
- **Any game on the Theater screen**: games in other engines (Unreal, Godot and the rest, from Steam, Epic, GOG or a
  folder you add) now show in the library with a **Theater** badge. Play in VR starts the game as usual and shows its
  window on the Theater screen (PortXRTheater.exe, beside the app); the controllers work as keyboard and mouse, and
  the laser pointer clicks where the game shows its cursor. Nothing is written to the game folder. Steam tools and
  apps are left out; anti-cheat and online-only games stay refused.
- **Add VR anyway** for games PortXR would leave alone but could run in: multiplayer-only games without
  anti-cheat, games with their own VR mode, and Mono games built with code trimming. The page says what to expect
  first and asks again. Anti-cheat, 32-bit and Linux-build games stay refused.
- Settings: **Around the screen** (Dark or Your room) for Theater and Parallax.

### Tests
- Godot gate (`harness/godot.py`): fixture G1, a Godot 4.5 project, in VR through the app's override.cfg; the
  compositor's view of the test headset must show the world from the player camera and the HUD panel.
- Theater host: a plain (non-Unity) window and a Godot 4.5 game captured onto the Theater screen on the SteamVR test
  headset, colours exact.
- Gate 6c: a 50% white square in every fixture's UI must reach the VR panel at about 50% opacity, also where the
  game camera had to move off screen (F5, F7).
- The VR menu captures cover all six tabs and the first-run card.
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
