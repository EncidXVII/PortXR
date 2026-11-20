// Copyright (C) 2026 EncidXVII
// SPDX-License-Identifier: GPL-3.0-only

#include "script.h"
#include "common.h"
#include "settings.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <share.h>
#include <vector>

// Lua is built as C++ (its errors are exceptions, so destructors in our calls run), hence no extern "C" here.
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

PRX_API int32_t prx_key_down(int32_t vk);

namespace script {
float eyeOffset[3];
float crosshairArea[2];
}  // namespace script

namespace {
struct Loaded {
    std::string name;
    lua_State* L = nullptr;
    bool failed = false;
};
void (*g_engine)(lua_State*) = nullptr;
std::vector<Loaded> g_scripts;
std::wstring g_stamp;
ULONGLONG g_checkedAt, g_callAt;
LARGE_INTEGER g_start{}, g_last{};
std::string g_scene = "\x01", g_camera = "\x01";  // never a real name: the first tick reports both

std::wstring folder() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring stem = exe;
    stem = stem.substr(stem.find_last_of(L"\\/") + 1);
    stem = stem.substr(0, stem.rfind(L'.'));
    return prx_appdata(L"scripts\\") + stem;
}

std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

double seconds(const LARGE_INTEGER& since) {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return (double)(now.QuadPart - since.QuadPart) / freq.QuadPart;
}

// A script stuck in a loop would freeze the game: a call that runs over a second is stopped.
void watchdog(lua_State* L, lua_Debug*) {
    if (GetTickCount64() - g_callAt > 1000) luaL_error(L, "took over a second (an endless loop?)");
}

// The setting's section in the game's file, which the app writes in full.
std::string section_of(lua_State* L, const char* key) {
    std::string section = settings::section_of(key);
    if (section.empty()) script::fail(L, "no PortXR setting named %s", key);
    return section;
}

float number(lua_State* L, int i) { return (float)luaL_checknumber(L, i); }

// The shared calls (SCRIPTING.md); the engine adds its own after them.
void shared_calls(lua_State* L) {
    auto add = [&](const char* name, lua_CFunction f) { lua_pushcfunction(L, f), lua_setfield(L, -2, name); };
    add("log", [](lua_State* L) {
        prx_log("[script] %s", luaL_tolstring(L, 1, nullptr));
        return 0;
    });
    add("get", [](lua_State* L) {
        const char* key = luaL_checkstring(L, 1);
        std::string v = settings::get(section_of(L, key).c_str(), key, "");
        lua_pushlstring(L, v.data(), v.size());
        return 1;
    });
    add("set", [](lua_State* L) {
        const char* key = luaL_checkstring(L, 1);
        std::string value = luaL_tolstring(L, 2, nullptr), section = section_of(L, key);
        // Written only on a change: a script setting it every frame would rewrite the file 90 times a second.
        if (settings::get(section.c_str(), key, "") != value) settings::set(section.c_str(), key, value);
        return 0;
    });
    add("time", [](lua_State* L) { return lua_pushnumber(L, seconds(g_start)), 1; });
    add("scene", [](lua_State* L) { return lua_pushstring(L, g_scene.c_str()), 1; });
    add("camera", [](lua_State* L) {
        if (g_camera.empty()) lua_pushnil(L);
        else lua_pushstring(L, g_camera.c_str());
        return 1;
    });
    add("key", [](lua_State* L) { return lua_pushboolean(L, prx_key_down((int)luaL_checkinteger(L, 1)) != 0), 1; });
    add("eye_offset", [](lua_State* L) {
        for (int i = 0; i < 3; ++i) script::eyeOffset[i] = number(L, i + 1);
        return 0;
    });
    add("crosshair_area", [](lua_State* L) {
        for (int i = 0; i < 2; ++i) script::crosshairArea[i] = std::clamp(number(L, i + 1), 0.02f, 1.f);
        return 0;
    });
}

// Plain Lua and the portxr table: no files, programs, network or loading code from strings.
lua_State* sandbox() {
    lua_State* L = luaL_newstate();
    const luaL_Reg libs[] = {{LUA_GNAME, luaopen_base}, {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},
                             {LUA_TABLIBNAME, luaopen_table}, {LUA_UTF8LIBNAME, luaopen_utf8}};
    for (const luaL_Reg& lib : libs) luaL_requiref(L, lib.name, lib.func, 1), lua_pop(L, 1);
    for (const char* unsafe : {"dofile", "loadfile", "load", "require", "collectgarbage"}) lua_pushnil(L), lua_setglobal(L, unsafe);
    lua_newtable(L);
    shared_calls(L);
    if (g_engine) g_engine(L);
    lua_setglobal(L, "portxr");
    lua_sethook(L, watchdog, LUA_MASKCOUNT, 100000);
    return L;
}

void unload() {
    for (Loaded& s : g_scripts) lua_close(s.L);
    g_scripts.clear();
}

void reload() {
    std::wstring dir = folder();
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd;
    std::wstring stamp;
    HANDLE h = FindFirstFileW((dir + L"\\*.lua").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do files.push_back(fd.cFileName);
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(files.begin(), files.end());
    for (const std::wstring& f : files) {
        WIN32_FILE_ATTRIBUTE_DATA a{};
        GetFileAttributesExW((dir + L"\\" + f).c_str(), GetFileExInfoStandard, &a);
        stamp += f + L"|" + std::to_wstring(((ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime) + L"|";
    }
    if (stamp == g_stamp) return;
    g_stamp = stamp;
    unload();
    script::eyeOffset[0] = script::eyeOffset[1] = script::eyeOffset[2] = 0;
    script::crosshairArea[0] = script::crosshairArea[1] = 0;
    g_scene = g_camera = "\x01";
    for (const std::wstring& f : files) {
        std::string text;
        if (FILE* file = _wfsopen((dir + L"\\" + f).c_str(), L"rb", _SH_DENYNO)) {
            char buf[8192];
            for (size_t n; (n = fread(buf, 1, sizeof(buf), file)) > 0;) text.append(buf, n);
            fclose(file);
        }
        Loaded s{narrow(f), sandbox()};
        g_callAt = GetTickCount64();
        // Text only: precompiled chunks can break out of the sandbox.
        if (luaL_loadbufferx(s.L, text.data(), text.size(), ("@" + s.name).c_str(), "t") != LUA_OK || lua_pcall(s.L, 0, 0, 0) != LUA_OK) {
            prx_log("Script %s failed to load: %s", s.name.c_str(), lua_tostring(s.L, -1));
            lua_close(s.L);
            continue;
        }
        prx_log("Script %s loaded", s.name.c_str());
        g_scripts.push_back(s);
    }
}

void call(const char* function, const char* text, double number) {
    for (Loaded& s : g_scripts) {
        if (s.failed) continue;
        if (lua_getglobal(s.L, function) != LUA_TFUNCTION) {
            lua_pop(s.L, 1);
            continue;
        }
        if (text) lua_pushstring(s.L, text);
        else lua_pushnumber(s.L, number);
        g_callAt = GetTickCount64();
        if (lua_pcall(s.L, 1, 0, 0) != LUA_OK) {
            s.failed = true;
            prx_log("Script %s stopped in %s: %s", s.name.c_str(), function, lua_tostring(s.L, -1));
            lua_pop(s.L, 1);
        }
    }
}
}  // namespace

namespace script {
void enable(void (*engineCalls)(lua_State*)) {
    g_engine = engineCalls;
    QueryPerformanceCounter(&g_start);
    g_last = g_start;
    g_checkedAt = 0;
    g_stamp = L"\x01";
}

void tick(const std::string& scene, const std::string& camera) {
    if (!g_start.QuadPart) return;
    ULONGLONG now = GetTickCount64();
    if (now - g_checkedAt >= 2000) g_checkedAt = now, reload();
    double dt = std::min(seconds(g_last), 0.25);
    QueryPerformanceCounter(&g_last);
    if (g_scripts.empty()) return;
    // Both names first, so scene() and camera() answer for this frame inside either callback.
    bool newScene = scene != g_scene, newCamera = camera != g_camera;
    g_scene = scene, g_camera = camera;
    if (newScene) call("on_scene", scene.c_str(), 0);
    if (newCamera && !camera.empty()) call("on_camera", camera.c_str(), 0);
    call("on_frame", nullptr, dt);
}

void fail(lua_State* L, const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    luaL_error(L, "%s", buf);
    std::abort();  // luaL_error throws; never reached
}
}  // namespace script
