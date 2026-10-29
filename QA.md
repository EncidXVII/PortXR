# QA: desktop app, build 8418c70, fixes retested after it

Date: 2026-10-29. Windows 11, the real Steam library (139 Unity games: 28 on this PC, 111 more owned on Steam).
The app ran on the second screen. Clicks went through Windows UI Automation, so the mouse never moved. Installs
used the F1 test game (`fixtures\F1\Build`).

## Results

| Area | Check | Result |
| --- | --- | --- |
| Start | First run, no saved library: skeleton cards, then the real grid | Pass |
| Start | With a saved library: games show at once, the lime line sweeps while the scan refreshes | Pass |
| Tabs | All 139 = VR added 20 + Installed 4 + Not installed 75 + Unsupported 40 (after finding 4 moved games with their own VR to Unsupported) | Pass |
| Tabs | Each tab shows only its own cards; every card has one status label in the tab's words | Pass |
| Labels | VR added solid lime, Installed lime dot, Not installed grey, blocked red dot with the reason, 32-bit and trimmed grey dot | Pass |
| Search | "knight" narrows the grid; "zzzz" shows the empty state with **Show all games** | Pass |
| Game page | VR added (Frogun): Play in VR, Uninstall, Open folder, Export logs, Open log | Pass |
| Game page | Blocked (VRChat): EasyAntiCheat label, only Open folder, red refusal text | Pass |
| Game page | 32-bit (Phoenix Wright): no Play or Install | Pass |
| Install | **Install** from the game page writes the receipt; **Uninstall → Confirm** leaves the folder byte-identical | Pass |
| Library | **Remove from library** forgets an added folder and leaves the folder itself alone | Pass |
| Anti-cheat | Steam data blocks Rust (EasyAntiCheat) and Unturned (BattlEye) before install | Pass |
| Anti-cheat | Install refused with an EasyAntiCheat folder present | Pass |
| Anti-cheat | Installed game that gains EasyAntiCheat: the next update refuses, removes Parallax, folder byte-identical | Pass |
| Anti-cheat | Game started directly with EasyAntiCheat present: the plugin logs the refusal and sets `enabled = false` in `doorstop_config.ini` | Pass |
| Blocking | Apps with no player modes on Steam (XSOverlay) are no longer called "Multiplayer only" | Pass |
| Regression | Test games F1 (Mono) and F3 (IL2CPP): 21 of 21 checks each | Pass |

## Findings, all fixed

1. **The search box and the sort, store and key menus had no accessible name.** Fixed: they now read "Search
   games", "Order: Recently played", "Store: …" and "<button>: <key>". Egui's menus still can't be opened through UI
   Automation; keyboard and mouse work.
2. **Opening the app on a game folder ran a full library scan before the window drew.** Fixed: the page opens from
   the saved library (1.6 s from launch to the game page), the scan runs behind it, and its result waits until the
   library is shown again, so an open page never loses its place.
3. **A game page didn't redraw when its folder changed outside the app.** Fixed: the page redraws every second. After
   an install from the command line, it showed Uninstall within 2 s without any input.
4. **Resonite was allowed.** Fixed by a new rule: games Steam lists with VR support of their own (categories 31, 53,
   54) are **Already VR**, unsupported with a grey label and a note to play them in VR from Steam. Install refuses
   them. On this library: HUMANITY, Resonite and XSOverlay on the PC, and 18 owned games, all with real VR modes
   (Boneworks, Demeo, The Forest, Tabletop Simulator and others).
5. **Uninstalling right after a game closed could fail on locked files.** Fixed: removal retries for up to 3 s.
   Uninstall right after killing the running test game now succeeds.

Retested after the fixes: test games F1 (Mono) and F3 (IL2CPP) pass 21 of 21 checks each; the app builds for Windows
and type-checks for Linux.

## Not covered

- **Play in VR from the app:** it launches games on the main screen. The test harness covers the in-game side.
- **Installing a game through Steam** from a Not installed card: that opens Steam's install dialog.
- **Add game, Import script:** both need a file dialog.
- **Play waiting for a Steam update:** needs a pending update on a real game.
- **Linux:** no Linux machine with a headset. The Linux build only passed a compile check.
- **Settings page and Export logs:** opened, but values weren't changed.
