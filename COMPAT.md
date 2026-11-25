# Compatibility

Tiers: **A** stereo + UI + controls, no blocker. **B** playable with a quirk or profile. **C** theater only. **Fail** does not run.
Fixture rows come from the automatic checks that run on every build. Game rows come from game reports and app checks.
Every fixture row also passes gate 12: SteamVR is killed mid-game, the game goes flat and VR comes back on its own.

| Game | Unity | Backend | Tier | Source | Notes |
|---|---|---|---|---|---|
| Fixture F1 | 2022.3 | Mono, D3D11, built-in | A | harness gates 1-10 | all assertions pass |
| Fixture F0 | 2018.4 | Mono (.NET 3.5), D3D11, built-in | A | harness gates | old runtime + native UI capture |
| Fixture F2 | 2022.3 | Mono, D3D11, URP + Bloom | A | harness gates | post-processing reaches the eyes |
| Fixture F3 | 2022.3 | IL2CPP, D3D11, built-in | A | harness gates | before-render hook and Graphics.Blit stripped: LateUpdate fallback, native mirror |
| Fixture F4 | 2022.3 | Mono, D3D12, built-in | A | harness gates | UI grab and mirror at Present |
| Fixture F5 | 2022.3 | IL2CPP, D3D11, URP + Bloom | A | harness gates | post-processing copied through IL2CPP reflection |
| Fixture F6 | 2022.3 | Mono, D3D11, built-in, Input System | A | harness gates | Gamepad mode reaches the Input System |
| Fixture F7 | 6.3 | IL2CPP, D3D12, URP | A | harness gates | onBeforeRender stripped: eyes placed in LateUpdate |
| Fixture F8 | 6.3 | Mono, D3D11, built-in | A | harness gates | |
| Fixture F9 | 2022.3 | Mono with code trimming | Fail | app detection | Mod loader cannot start (trimmed engine and class libraries); refused with a clear message |
| Fixture F10 | 2022.3 | Mono, D3D11, HDRP + Bloom | A | harness gates | HDRP camera settings reach the eyes |
| Fixture F11 | 2022.3 | Mono, D3D11, built-in + Post Processing v2 | A | harness gates | PPv2 layer copied to the eyes |
| Phoenix Wright: Ace Attorney Trilogy | 2017.4 | Mono (.NET 3.5), 32-bit | Fail | app check | 32-bit: PortXR's parts are 64-bit only; the app now refuses it |

PortXR is for offline single-player games. It refuses games with anti-cheat.
