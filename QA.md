# QA: build 5461d1d, plus one app fix found in this pass

Date: 2026-10-30. Windows 11, the real Steam library (139 Unity games: 28 on this PC, 111 more owned on Steam).
The app ran on the second screen. Clicks went through Windows UI Automation, so the mouse never moved. Installs
used the F1 test game (`fixtures\F1\Build`); every change to the folder and to the app's settings was undone after.

## Results

| Area | Check | Result |
| --- | --- | --- |
| Start | First run, no saved library: skeleton cards, then the real grid | Pass |
| Start | With a saved library: games show at once, the lime line sweeps while the scan refreshes | Pass |
| Start | Opening on a game folder (`Parallax.exe <folder>`): its page in 1.6 s | Pass |
| Start | A folder opened that way stays in the library after the background scan | **Failed, fixed** (finding 1) |
| Tabs | All 139 = VR added 20 + Installed 4 + Not installed 75 + Unsupported 40 | Pass |
| Labels | One status label per card in the tabs' words; **Already VR** grey on XSOverlay with its note | Pass |
| Accessibility | Search box, sort and store menus have names: "Search games", "Order: Recently played", "Store: All stores" | Pass |
| Search | "knight" narrows the grid; "zzzz" shows the empty state with **Show all games** | Pass |
| Game pages | VR added (Frogun), blocked (VRChat, EasyAntiCheat), Already VR (XSOverlay): the right buttons and text | Pass |
| Install | **Install**, then **Uninstall → Confirm**: receipt written, folder byte-identical after | Pass |
| Install | Install from the command line while the page is open: it shows Uninstall within 2 s, no input needed | Pass |
| Install | Uninstall right after the running game is killed: succeeds, folder byte-identical | Pass |
| Anti-cheat | Install refused with an EasyAntiCheat folder present | Pass |
| Anti-cheat | Installed game that gains EasyAntiCheat: the next update refuses and removes Parallax | Pass |
| Anti-cheat | Game started directly with EasyAntiCheat present: the plugin logs the refusal and sets `enabled = false` | Pass |
| Settings | All-games page: **Display → Theater** then **Stereo** is written to `defaults.cfg` | Pass |
| Settings | A game's own setting chosen before install goes into the game on install | Pass |
| Settings | Changed while installed: written to the game's config; kept for next time after uninstall | Pass |
| Export logs | Writes a zip to the desktop with the game's logs (`Player.log`, `game.txt` here, as F1 wasn't installed) | Pass |
| Library | **Remove from library** forgets the folder and leaves it alone | Pass |
| VR screen | Move & resize (test gate 19): drag, corner, stick, B to finish, saved per game, lime frame drawn | Pass on all 12 test games |
| Regression | All 12 test games on 5461d1d: 11 pass every check | Pass, F0 flaky (finding 2) |

## Findings

1. **Fixed: a game folder opened from a shortcut fell out of the library.** The background scan started before the
   folder was saved to the library list, so its result lacked the game; going back to the library hid it until
   the next scan. The folder is now saved first. Retested: the game stays (All 140 with it).
2. **F0 test game is flaky.** Some runs start with the simulated headset's tracking off, and then the stereo
   marker, tracking and Parallax window checks fail together; the next run passes all 22. Not caused by recent
   changes (the previous build fails the same way). Not yet diagnosed.
3. **Minor: `Parallax.exe uninstall` on a game without Parallax says "The system cannot find the file
   specified".** Only the command line (used by the test harness) shows it; the app hides Uninstall then.
4. **Minor: the Already VR note says "This game" for apps too** (XSOverlay is an overlay tool).
5. **Not reproduced: two stray characters ("[]") once appeared in the search box after UI Automation typed into
   it.** Likely the automation, not the app; noted in case a player sees it.

## Not covered

- **Play in VR from the app:** it launches games on the main screen; the test harness covers the in-game side.
- **Move & resize by hand in a headset:** the test gate drives it with simulated controllers only.
- **Installing through Steam** from a Not installed card: it opens Steam's install dialog.
- **Add game, Import script:** both need a file dialog.
- **Play waiting for a Steam update:** needs a pending update on a real game.
- **Linux:** no Linux machine with a headset; the Linux code only passed a compile check.

# QA: VR side

Same day, test games F1 (Mono) and F3 (IL2CPP) on the simulated headset. A script opened every VR surface and
captured the eye image and the UI panel or theater screen as the plugin hands them to the runtime. The simulated
headset's own window was covered by SteamVR's status window, so the compositor's combined view was not captured.

| Area | Check | F1 | F3 |
| --- | --- | --- | --- |
| Welcome card | Three steps, Recenter, world scale, Start playing | Pass | Pass, plain font |
| Menu tabs | Quick, View, Controls, Game (camera previews) | Pass | Pass |
| Menu, theater and Parallax | Screen row (Move & resize, Reset) shows only there; 3D depth only in Parallax | Pass | Pass |
| Laser | Beam in the eye image, pointer dot on the panel | Pass | Pass |
| Hover | The button under the laser is clearly marked | **Fixed** (finding 7) | **Fixed** |
| Options row | "Keyboard & mouse" readable, row inside the column | **Fixed** (finding 8) | **Fixed** |
| Wrist glance | Watch drawn and lit when the back of the wrist faces the eyes | Pass | **Failed, fixed** (finding 6) |
| Theater | Game frame on the screen | Pass | Pass |
| Move & resize | Lime frame, corner marks, controls panel | Pass | Pass |
| Parallax | Window image framed as the flat view | Pass | Pass |

## VR findings

6. **Fixed, serious: on IL2CPP games that strip `Math.Acos`, raising the left wrist froze VR.** The wrist glance
   check called `Vector3.Angle`, which throws there, every frame the hand was within 70 cm of the face: frames
   stopped (1388 → 1388) until the hand moved away. The same call had switched off 2D-scene scaling in those games.
   Both now use the plugin's own math. A new test gate (20 wrist glance) checks the watch lights and frames keep
   coming; it passes on F1 and F3.
7. **Fixed: hover was a 1 px line along the top of the button**, the rest covered by the next button's edge.
   The button under the laser now gets a lighter fill and a 2 px ring drawn over everything.
8. **Fixed: "KEYBOARD & MOUSE" was cramped against its neighbours.** Option buttons were sized by letter count;
   they are now as wide as their text plus shared padding, and the row keeps the column's right edge.
9. **Minor: on IL2CPP games the menu uses the game's default font** instead of the display and monospace fonts
   (they don't load there). Readable, less polished.
10. **Small text at low game resolutions.** The menu scales with the game's resolution; at the test games' 540p the
    labels are about 9 px tall. At 1080p and up they are twice that. Worth checking in a headset on a game
    running at a low resolution.

## VR not covered

- **The combined view in a headset:** layer order, curve and depth as the compositor shows them.
- **Comfort:** Parallax depth, the theater distance, motion with the vignette.
- **Real controllers:** laser jitter, haptics, and grabbing the screen by hand.
