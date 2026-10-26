# Delivery workflow

Every build that leaves this machine is a zip made by `release.ps1` from a committed tree, so any report traces
back to one commit.

## Test builds (to testers)

1. **Commit** everything. `release.ps1` refuses a dirty tree.
2. **Regress**: every fixture must pass every gate:
   ```
   foreach ($f in "F0","F1","F2","F3","F4","F5","F6","F7","F8","F10","F11") { python harness\run.py --fixture $f --secondary }
   ```
   For a change to a real-game behaviour, also check that game offline (single-player titles only).
3. **Pack**: `.\release.ps1 -Test` gives `release\Parallax-<version>-test-<commit>.zip` and `SHA256SUMS.txt`. The zip
   holds `Parallax.exe`, both payloads, the licence notices and `TESTING.md` (what to try, how to report).
4. **Send** the zip and its SHA-256, and say which games to focus on. Note the commit in the tracker so reports map to it.
5. **Collect**: testers send a description, the game's **Export logs** zip and their headset and runtime. The log
   zip names the plugin version; the commit is in the zip's file name.
6. **Fix and resend**: a new test build from the new commit. Testers unzip it over the old folder (or anywhere) and
   start `Parallax.exe`, which updates the plugin in every game it installed into. **Play** updates it too.

## Releases (public)

1. Bump the version in `managed\Parallax.Core\Plugin.cs` (`BepInPlugin`) and `app\Cargo.toml`. They must match:
   `release.ps1` checks.
2. Update `COMPAT.md` with the games verified for this version. Then commit, regress and run `.\release.ps1`, which
   gives `release\Parallax-<version>.zip`.
3. Tag `v<version>` and publish a GitHub release with the zip and `SHA256SUMS.txt`. The app's update check reads
   GitHub releases. Publishing is outward-facing: only on an explicit go-ahead.

## Rules

- No build from a dirty tree, and none with a failing fixture gate.
- Testers get test builds only. Nothing is published without an explicit go-ahead.
- Real-game checks on this machine use offline single-player titles only, unattended (no launcher clicks).
