// Copyright (C) 2026 EncidXVII
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
// Per-game Lua scripts in the core, for the engines without their own script host (Godot, Unreal; Unity's plugin has
// one in C#, with the same files, callbacks and `portxr` table: SCRIPTING.md). Scripts live in
// %APPDATA%\PortXR\scripts\<game exe name>\*.lua, reload within two seconds of a change, and are sandboxed: plain Lua
// (string, math, table, utf8) and the `portxr` table, no files, programs or network. One that fails is switched off,
// with one log line, until its file changes.
#include <string>

struct lua_State;

namespace script {
// The engine's own calls, added to the `portxr` table at the top of the stack (Godot: its nodes). Turns the host on;
// without it tick() does nothing (Unity runs its own).
void enable(void (*engineCalls)(lua_State* L));
// Once a game frame: reloads changed files, then on_scene and on_camera when the names change, then on_frame(dt).
void tick(const std::string& scene, const std::string& camera);
// What scripts ask of the core: the eyes moved (metres in the player's frame: x right, y up, z forward), and the part
// of the game's UI that moves with the crosshair (fractions of the screen; 0 keeps the default).
extern float eyeOffset[3];
extern float crosshairArea[2];
// For engine calls: a Lua error ("no node at Player/Gun"), which stops the script.
[[noreturn]] void fail(lua_State* L, const char* fmt, ...);
}  // namespace script
