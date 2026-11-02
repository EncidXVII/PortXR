# Testing Parallax

Thanks for trying it. This build plays flat Unity games in VR. Expect rough edges: what breaks and where is exactly
what we need.

## Before you start

- **Single-player, offline games only.** Parallax injects a mod into the game. The app refuses games with
  EasyAntiCheat or BattlEye, but do not use it in any online or multiplayer game: you could be banned.
- A PC VR headset with an OpenXR runtime: SteamVR, Virtual Desktop, Quest Link or similar.
- Windows may warn that `Parallax.exe` is from an unknown publisher (the build is not signed yet): **More info → Run
  anyway**. Some antivirus tools flag `winhttp.dll`, the mod loader's (BepInEx) entry point inside game folders.

## Set up

1. Unzip anywhere and keep the folder together (`Parallax.exe` needs the `payload` folders next to it).
2. Start your headset runtime, then run `Parallax.exe`. It lists the Unity games it finds (Steam, Epic, GOG, itch and
   the usual game folders). Missing one? **Add game +** and pick its folder. **Not installed** lists the Unity games
   in your Steam library that are not on this PC: click one and Steam installs it.
   A game with a problem only it has may be fixable with a script: see `SCRIPTING.md`, and send us any script that helped.
3. Pick a game and press **Play in VR**. If Steam asks which version to start (DX11 or DX12), either works.
4. In VR, **double-tap the left menu button** (or press F11) for the VR menu: display (Stereo, Parallax, Theater), recenter, world scale,
   controls, head aim.

**Remove VR** on a game's page puts its folder back exactly as it was.

## What to try

Play each game for 10 to 15 minutes: the main menu, the first level, a cutscene if there is one, a pause menu.

- Is the 3D view right: both eyes agree, nothing doubled, the world the right size?
- Can you read and click the game's menus with the laser pointer?
- Do the controls work (gamepad or keyboard & mouse, whichever the game got)?
- In first-person games: does aiming with your head feel right? In third-person games: does the right stick turn
  the camera?
- Anything that flickers, shows in one eye only, or looks wrong compared to the flat game.

Also try **Display → Parallax** in the VR menu: the game in 3D inside a big screen, played with its own camera
controls. Does it feel deep without straining your eyes? Is the **3D depth** default right?

If the 3D view is broken, **Theater** in the VR menu shows the flat game on a big screen. Say whether that worked.
In Theater or Parallax, open the VR menu and use the screen's handles: drag the bar under it to move it, a corner to resize it, and the
right stick. Was it easy to put where you wanted?

## Reporting

For each game, send:

1. The game's name and what you saw (a phone photo through the lens helps).
2. Its logs: select the game in `Parallax.exe` → **Export logs**. That saves one zip to your desktop.
3. Your headset and runtime, for example "Quest 3, Virtual Desktop".

Works perfectly? Tell us that too: knowing what works is as useful as knowing what breaks.
