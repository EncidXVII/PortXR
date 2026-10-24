# Third-party notices

Parallax ships or downloads the following components. Each remains under its own license.

## BepInEx 5.4.23.5
Plugin loader copied into the game folder on install. LGPL-2.1. https://github.com/BepInEx/BepInEx
Bundled with it: HarmonyX (MIT), MonoMod (MIT), Mono.Cecil (MIT), Unity Doorstop (LGPL-2.1).

## OpenXR SDK loader 1.1.65
`openxr_loader.dll` and headers. Apache-2.0. Copyright (c) 2017-2025 The Khronos Group Inc.
https://github.com/KhronosGroup/OpenXR-SDK-Source

## MinHook 1.3.4
Statically linked into `parallax_xr.dll`. BSD-2-Clause.

    MinHook - The Minimalistic API Hooking Library for x64/x86
    Copyright (C) 2009-2017 Tsuda Kageyu. All rights reserved.

    Redistribution and use in source and binary forms, with or without modification, are permitted provided that
    the following conditions are met:
    1. Redistributions of source code must retain the above copyright notice, this list of conditions and the
       following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
       following disclaimer in the documentation and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
    WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
    PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
    INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
    OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## Rust crates in Parallax.exe
Statically linked. egui, eframe, egui_extras, egui-winit, winit, glow, glutin (MIT OR Apache-2.0); image, serde, serde_json, sha2, ureq, rustls, zip, dirs, winreg, rfd, windows-sys (MIT OR Apache-2.0, or MIT); ring (ISC and BSD-style); webpki-roots (CDLA-Permissive-2.0). Full list with versions: `app/Cargo.lock`.
