# Compatibility

Tiers: **A** stereo + UI + controls, no blocker. **B** playable with a quirk or profile. **C** theater only. **Fail** does not run.
Harness tiers are guesses from automated runs under the SteamVR null driver; B always needs a human.

| Game | Unity | Backend | Tier | Source | Notes |
|---|---|---|---|---|---|
| Fixture F1 | 2022.3 | Mono, D3D11, built-in | A | harness gates 1-10 | all assertions pass |
| Fixture F0 | 2018.4 | Mono (.NET 3.5), D3D11, built-in | A | harness gates | old runtime + native UI capture |
| Fixture F2 | 2022.3 | Mono, D3D11, URP + Bloom | A | harness gates | post-processing reaches the eyes |
| Fixture F3 | 2022.3 | IL2CPP, D3D11, built-in | A | harness gates | before-render hook and desktop mirror stripped: LateUpdate fallback, no mirror |
| Lorn's Lure | 2019.4 | Mono | A (guess) | harness smoke | stereo frames, main camera picked |
| Phoenix Wright: Ace Attorney Trilogy | 2017.4 | Mono (.NET 3.5), 32-bit | Fail | harness smoke | 32-bit: no OpenXR runtime supports it; the app now refuses it |

Only offline single-player games are run by the harness. Multiplayer, anti-cheat and leaderboard games are not tested.
