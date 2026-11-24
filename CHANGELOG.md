# Changelog

## Unreleased

Linux:
- No more pasting `WINEDLLOVERRIDES` into Steam's launch options: **Play in VR** sets the game's Proton prefix to
  load PortXR from the game folder (`"winhttp"` or `"dwmapi"` as `native,builtin` under
  `HKCU\Software\Wine\DllOverrides` in its `pfx/user.reg`, the original file kept beside it), and **Remove VR** and
  `--uninstall` take it out again. A running game's prefix is never edited: the page says to close the game first,
  or to start a new game once so Proton sets its prefix up.
- Launch options are now asked for only with WiVRn or Monado, and hold only
  `PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 %command%`, which Steam's Linux Runtime needs to pass those runtimes
  to a game. This applies to Godot games too, which asked for it with every runtime.
- Heroic and Lutris games (Experimental): Epic, GOG and Amazon games installed through Heroic, and Windows games in
  Lutris, are listed, with the Wine prefix each runs in. **Play in VR** starts them through their launcher
  (`heroic://launch/...`, `lutris:rungame/...`). When the launcher runs a game with plain Wine, which gives games no
  OpenXR, the page says to pick a Proton-based runner such as GE-Proton.
- The VR menu's Library tab switches games under Proton (Experimental): each game's prefix gets the list of games
  with VR in Wine's paths, and the next game starts through Steam (`steam://rungameid/...`), Heroic or Lutris as the
  current one quits. The list gains a fourth field, the link; older readers ignore it, and Windows still switches
  through PortXR.exe.
- The in-game VR menu now gets the all-games defaults and the theme: the app keeps them in step with each game's
  Proton prefix, where the menu reads and changes them.
- With WiVRn or Monado, a game's page says when xrizer or OpenComposite is missing as the OpenVR runtime (Proton
  gives games OpenXR only through one), instead of "Starts in VR".
- The OpenXR runtime is found where the OpenXR loader looks for it: the file for this CPU
  (`active_runtime.x86_64.json`), XDG_CONFIG_DIRS and /etc. With none, the page names SteamVR, WiVRn and Monado
  instead of Windows-only apps.
- Linux builds of Unreal and Godot games are listed as **Linux build**, with the step to force Proton, instead of
  being left out.
- Flatpak Steam's libraries are found where the sandbox keeps them, and Snap Steam is found.

## 0.6.1 - 2026-11-24

- About has its own tab in the top bar, beside Library and Settings, instead of being the last section of
  Settings.

## 0.6.0 - 2026-11-23

### The game catalogue in the app
- Each game's page shows how well the game plays with PortXR, from the game catalogue on everyvr.net: its best
  rating and display mode beside the game's state, with every mode tried and the number of reports on hover. A
  click opens the game's catalogue page.
- **Get script** beside Play installs the catalogue's script or settings for the game, after checking each file's
  SHA-256 against the catalogue. When the entry is updated, the button offers the update.
- **How did it run?** (with the troubleshooting links) opens the catalogue's report form with the game, engine and
  PortXR version filled in. No files or code needed: "it just works" is the most useful report.
- Library covers show the catalogue's rating for the mode the game plays in.
- The catalogue is downloaded whole and kept for offline starts, so nothing about your library leaves the PC.

### Also
- About lists the maker's profiles as GitHub, X, Threads, with X at its new address (x.com/EncidXVII).
- Linux packaging: a Nix flake (`nix run github:EncidXVII/PortXR`), and an Arch Linux PKGBUILD in `packaging/aur`,
  both built from the release files.

## 0.5.0 - 2026-11-23 (first public release)

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
  game runs untouched. UE 5.5 and newer,
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
- Steam apps that aren't games (VTube Studio, desktop overlays) are no longer listed. Unreal games whose game exe sits deeper
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
- Unreal before 5.5 (VHOLUME, BPM: Bullets Per Minute): menus and loading screens no longer stay on the UI panel over
  the levels after them. The engine draws its UI into the game window without clearing it; the window is now cleared
  after every frame while VR runs. Compiled, not yet played.
- Unity games that draw the world into a texture and show it across their own UI (Lethal Company): that picture no
  longer lies over the panel as a dark sheet. The hidden camera draws elsewhere and the game's texture is cleared while
  the eyes render. Compiled, not yet played.
- Games that had VR added while PortXR was still called Parallax kept the old plugin beside the new one after an
  update. The old one loaded first and took the headset, so those games went on running the old build (the log said
  "xrCreateInstance failed" and "VR stays off" for PortXR). An update now takes out plugin files an earlier build
  installed and this one no longer ships, and the old plugin's settings, the ones in use, become PortXR's. The app
  does it for every game the next time it lists them. Tested on files; not yet played.
- A game's page says when the game still holds an older PortXR plugin. Every library scan and every Play in VR
  already bring each game's plugin up to the build that is running (file by file, against what it ships); a game
  that was running at the time kept its old files without a word.
- Head aim on Auto starts by itself in first-person Unity games that keep the camera apart from the player and move it
  onto the body every frame (Peripeteia). It did so only where the camera is a child of the body; elsewhere aim stayed
  off until the player had turned on the spot for a while. The body is the character controller or moving rigidbody
  that stays under the camera as it travels. Compiled, not yet played.
- Head and hand aim no longer switch off and on in the middle of a run (VHOLUME). The check that tells a first-person
  camera from one that orbits the player counted turns made while the player still slid or fell after the stick was
  let go, and read a first-person camera as orbiting 1 m off. It now counts only turns where the camera moved along
  the turn, and goes by the median of the last 41. Compiled, not yet played.
- The view no longer shifts when the hand or the aim turns the game camera. A first-person camera that turns about a
  point behind itself (VHOLUME: 11 cm) moves as it turns, and the eyes hung on it: every sway of the aiming hand moved
  the whole world a little. The eyes hang on that pivot instead, set off along the body's forward. Compiled, not yet
  played.
- 3D screen in Unreal first-person games (BPM: Bullets Per Minute): the hands and weapon no longer show double. The
  screen's plane stayed 3 m off, so arms 40 cm from the eyes stood out of it by six eye-widths and more. The plane
  now comes to three times the distance of what the game camera carries: the world goes behind the screen and the
  arms come out by two eye-widths at most. Needs the game's objects (not yet in VHOLUME). Compiled, not yet played.
- VR menu in Full VR: the world dims while the menu is open. The menu is drawn over the game whatever stands nearer
  than it, and a wall or weapon at arm's length behind a menu 1.2 m off made it hard to look at and to point at.
  The pointer also no longer crawls for a finger resting on the trigger (its steadying for a click started at 15% of
  the pull, now 35%). Compiled, not yet played.
- Wrist watch: the right trigger opens the VR menu only while the right hand's beam is on the watch. The watch turned
  toward the eyes was enough before, so a left hand held up while the right one fired opened the menu mid-game. The
  beam shows whenever the watch does. Compiled, not yet played.
- Godot games that run their physics on a thread of their own no longer crash once aiming starts (Explosiovania:
  Jolt with `run_on_separate_thread`; it went down in the first fight). The crosshair's ray, and the 3D screen's
  focus rays, asked for the physics space from the render frame. There the engine answers null ("Space state is
  inaccessible right now"), and an exported game runs scripts without null checks: the next call was a read of
  address 0. The rays are now cast in the physics step, where the space is handed out, and a null one is left
  alone. Reproduced and checked in the Godot 4.5 editor without a headset; not yet played.
- Unreal: the search for the engine's function caller (ProcessEvent) reads further into each candidate and knows one
  more way the engine tests a function's flags; when it still finds none (VHOLUME), the log says where it looked.
  Compiled, not yet played.
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

## 0.2.0 test builds

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
