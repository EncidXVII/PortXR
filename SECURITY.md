# Security

Please report security issues privately through GitHub: the repository's **Security** tab, **Report a
vulnerability**, or by email to [mykola@everyvr.net](mailto:mykola@everyvr.net). Do not open a public issue for them. Expect a first answer within a week.

Relevant areas include:

- The installer and uninstaller, which write into game folders and the Proton prefix (`app/src/installer.rs`).
- Per-game Lua scripts (`managed/PortXR.Core/Scripts.cs`), imported from files and run inside the game.
- The update check and the Unity game list download (`app/src/ui.rs`, `app/src/games.rs`).

Only the latest release gets fixes. Releases are built from tagged commits by `release.ps1`, and each comes with a
`SHA256SUMS.txt`; check the zip against it before running a build someone sent you.
