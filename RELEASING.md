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
3. **Pack**: `.\release.ps1 -Test` gives `release\PortXR-<version>-test-<commit>.zip` and `SHA256SUMS.txt`. The zip
   holds `PortXR.exe`, both payloads, the licence notices and `TESTING.md` (what to try, how to report).
4. **Send** the zip and its SHA-256, and say which games to focus on. Note the commit in the tracker so reports map to it.
5. **Collect**: testers send a description, the game's **Export logs** zip and their headset and runtime. The log
   zip names the plugin version; the commit is in the zip's file name.
6. **Fix and resend**: a new test build from the new commit. Testers unzip it over the old folder (or anywhere) and
   start `PortXR.exe`, which updates the plugin in every game it installed into. **Play in VR** updates it too.

## Releases (public)

1. Bump the version in `managed\PortXR.Core\Plugin.cs` (`BepInPlugin`) and `app\Cargo.toml`. They must match:
   `release.ps1` checks.
2. Update `COMPAT.md` with the games verified for this version. Then commit, regress and run `.\release.ps1`, which
   gives `release\PortXR-<version>.zip`.
3. Tag `v<version>` and publish a GitHub release with the zip and `SHA256SUMS.txt`, its notes taken from this
   version's `CHANGELOG.md` section. The app's update check reads GitHub releases. Publishing is outward-facing: only
   on an explicit go-ahead.
   ```
   git tag -a v0.6.1 -m "PortXR 0.6.1"; git push origin main v0.6.1
   gh release create v0.6.1 release\PortXR-0.6.1.zip release\SHA256SUMS.txt --title "PortXR 0.6.1" --notes-file notes.md
   ```
4. In `flake.nix`, set `version` and the Linux tarball's `hash` (its SHA-256 in SRI form: `nix hash convert
   --hash-algo sha256 <hex>`), then push: the Nix workflow builds it.

## First publish (once)

1. Create the repository `EncidXVII/PortXR` (public, no template files: they are here already), then
   `git remote add origin https://github.com/EncidXVII/PortXR.git` and push `main`. The app's links (Guide, Report a
   problem, update check, scripting guide) and the README badges point there.
2. **About** (the gear beside it on the repository page). GitHub search ranks the name, description, topics and
   README; topic pages list repositories by stars.
   - Description: `Free, open-source VR mod for Unity games: play flat PC games in 6DOF VR with motion controls, head
     and hand aim. Meta Quest, Index, Vive, Pico, any OpenXR headset.`
   - Website: leave empty, or the latest release page.
   - Topics (20 at most): `vr`, `virtual-reality`, `vr-mod`, `flat2vr`, `openxr`, `steamvr`, `unity`, `unity3d`,
     `bepinex`, `il2cpp`, `mod`, `modding`, `game-mod`, `meta-quest`, `oculus-quest`, `valve-index`, `6dof`,
     `motion-controls`, `rust`, `egui`.
   - Tick "Releases" and untick "Packages" and "Deployments" so the sidebar shows only what matters.
3. **Settings → General → Social preview**: upload `docs/images/social-preview.png` (the card shown when the repository
   link is shared). Features: Issues on, Discussions on (categories: Q&A, Game reports, Show and tell), Wiki off.
4. **Security**: turn on Private vulnerability reporting (`SECURITY.md` sends reports there) and Dependabot alerts.
5. **Branch protection** on `main`: require the `build` check before merging.
6. Check the first CI run is green, then make the release as above: notes from the CHANGELOG section with the
   headset clip at the top, the zip and `SHA256SUMS.txt` attached. Releases feed the Downloads badge and the app's
   update check.
7. Open and pin an issue "Game reports: tell us what works", linking the game report template, so visitors see the
   list growing; label a few small issues `good first issue` (GitHub lists those to new contributors).
8. Pin the repository on your GitHub profile.

## Launch (each post is outward-facing: only on an explicit go-ahead)

- **Record a 30-60 s clip in the headset first**: hand aim in an FPS, the VR menu, Theater. A clip moves far more people
  than text; put it (as a GIF or MP4, under 10 MB, which GitHub plays inline) at the top of the README and the release
  notes, and use it in every post.
- Within GitHub: open pull requests adding PortXR to curated lists people browse (for example `awesome-vr`,
  `awesome-openxr`, `awesome-unity`), each following that list's own rules.
- Outside it, where the stars that drive GitHub Trending come from: r/virtualreality, r/SteamVR, r/oculus,
  r/OculusQuest (PC VR flair), r/VRGaming, the Flat2VR community Discord, and a Show HN on Hacker News for the
  technical side. Each with the clip,
  a one-line pitch, the tested games, and the single-player-only rule up front.
- Answer early comments quickly and turn game reports into COMPAT.md rows: a list of games that work is what people
  search for ("<game> VR mod").

## Pictures

The README's and the banner's pictures come from the real UI:

- `docs/images/vr-menu.png`: `python harness/menu_shots.py OUT_DIR`, then copy `vr-menu-0.png` (the Quick tab).
- `docs/images/library.png`: run `PortXR.exe` with `PORTXR_SHOT=docs\images\library.png` and
  `PORTXR_SHOT_SIZE=1120x1040` set. It shows your own library, saves the picture once the covers have loaded and
  closes; in this mode it changes nothing in any game folder.
- `docs/images/social-preview.png`: `app\assets\brand\render-og.ps1` from the repository root.
