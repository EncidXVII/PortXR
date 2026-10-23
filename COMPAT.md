# Compatibility

Tiers: **A** stereo + UI + controls, no blocker. **B** playable with a quirk or profile. **C** theater only. **Fail** does not run.
Harness tiers are guesses from automated runs under the SteamVR null driver; B always needs a human.

| Game | Unity | Backend | Tier | Source | Notes |
|---|---|---|---|---|---|
| Fixture F1 | 2022.3 | Mono, D3D11, built-in | A | harness gates 1-10 | all assertions pass |
| Lorn's Lure | 2019.4 | Mono | A (guess) | harness smoke | stereo frames, main camera picked |
| Phoenix Wright: Ace Attorney Trilogy | 2017.4 | Mono (.NET 3.5) | Fail | harness smoke | plugin needs the .NET 4.x runtime |

Only offline single-player games are run by the harness. Multiplayer, anti-cheat and leaderboard games are not tested.
