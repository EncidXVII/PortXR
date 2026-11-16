// Parallax inside Unreal Engine 4.22 to 5.6 games, loaded as dwmapi.dll from beside the game's Shipping exe (every
// dwmapi export forwards to the system copy).
//
// How a game runs with it, in the order of this file's sections:
// 1. Reading the engine: the debug stereo device Unreal ships (FFakeStereoRendering, switched on by -emulatestereo) is
//    found in the engine's code, with its slots and the console variables stereo needs.
// 2. Eyes, render target, scene texture: the device's functions are replaced, so the engine renders both eyes where the
//    core's rig places them, side by side into one texture at the headset's resolution, off the window; the window keeps
//    the game's UI alone (the UI panel).
// 3. Present: the core submits the eyes and the UI, then waits for the headset's next frame.
// 4. Startup: the game's command line waits for a headset check. With a headset the engine starts with the stereo device;
//    without one the game runs untouched.
// Tests: a ParallaxVR\ue-test file beside the exe runs without a headset (Direct3D 11) and writes what the engine renders.
#include "unreal_dwmapi.h"

#include "bridge.h"
#include "script.h"
#include "uobject.h"
#include "common.h"
#include "gpu.h"
#include "mathx.h"
#include <MinHook.h>
#include <openxr/openxr.h>
#include <intrin.h>
#include <psapi.h>
#include <share.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

extern "C" {
int32_t prx_init(const char* appName, int32_t api, int32_t* recommendedWidth, int32_t* recommendedHeight);
int32_t prx_state();
void* prx_render_event_func();
void prx_set_init_texture(void* texture);
void prx_set_frame(int32_t slot, const PrxFrame* frame);
int32_t prx_install_hooks();
}

namespace {
enum Event { EvInit = 1, EvBegin = 2, EvSubmit = 3 };
void event(int id) { ((void(__stdcall*)(int))prx_render_event_func())(id); }

std::wstring g_dir;   // the game's Shipping exe folder (ours too)
DWORD g_mainThread;
std::wstring g_test;  // tests: run without a headset, write what the engine renders into this folder
FILE* g_log;

void note(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char line[512];
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (!g_log) return prx_log("%s", line);
    fprintf(g_log, "%s\n", line);  // straight to the file: the core's own lines come through drain_log
    fflush(g_log);
}

void drain_log() {
    if (!g_test.empty()) return;  // tests write their lines straight to the file
    std::string lines = bridge::poll_log();
    if (lines.empty() || !g_log) return;
    fputs(lines.c_str(), g_log);
    fflush(g_log);
}

// --- 1. Reading the engine ----------------------------------------------------------------------------------------------

struct Range {
    uint8_t *begin, *end;
};

// The exe (monolithic builds) and *Engine* DLLs (modular builds).
std::vector<Range> engine_code() {
    std::vector<Range> out;
    HMODULE mods[1024];
    DWORD bytes = 0;
    K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &bytes);
    HMODULE exe = GetModuleHandleW(nullptr);
    for (DWORD i = 0; i < bytes / sizeof(HMODULE); ++i) {
        wchar_t name[MAX_PATH];
        GetModuleBaseNameW(GetCurrentProcess(), mods[i], name, MAX_PATH);
        if (mods[i] != exe && !wcsstr(name, L"Engine")) continue;
        MODULEINFO mi{};
        K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi));
        out.push_back({(uint8_t*)mi.lpBaseOfDll, (uint8_t*)mi.lpBaseOfDll + mi.SizeOfImage});
    }
    return out;
}

bool in(const Range& r, const void* p) { return p >= r.begin && p < r.end; }

// Through jump thunks (incremental-link and import jumps) to the code itself.
const uint8_t* follow(const uint8_t* p) {
    for (int i = 0; i < 4; ++i) {
        if (p[0] == 0xE9) p = p + 5 + *(int32_t*)(p + 1);
        else if (p[0] == 0xFF && p[1] == 0x25) p = *(const uint8_t**)(p + 6 + *(int32_t*)(p + 2));
        else break;
    }
    return p;
}

// Start of the function holding `p`, from the x64 unwind tables (chained entries lead back to the primary one).
const uint8_t* function_start(const uint8_t* p) {
    DWORD64 base = 0;
    auto* f = RtlLookupFunctionEntry((DWORD64)p, &base, nullptr);
    if (!f) return nullptr;
    for (int i = 0; i < 16; ++i) {
        auto* info = (const uint8_t*)(base + f->UnwindInfoAddress);
        if (!((info[0] >> 3) & UNW_FLAG_CHAININFO)) break;
        f = (PRUNTIME_FUNCTION)(info + 4 + ((info[2] + 1) & ~1) * 2);
    }
    return (const uint8_t*)(base + f->BeginAddress);
}

// What a rip-relative lea (48/4C 8D, mod 00, rm 101) points at.
const uint8_t* lea_target(const uint8_t* p) {
    if ((p[0] != 0x48 && p[0] != 0x4C) || p[1] != 0x8D || (p[2] & 0xC7) != 0x05) return nullptr;
    return p + 7 + *(int32_t*)(p + 3);
}

void* read_ptr(const void* p) {
    __try {
        return *(void* const*)p;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool writable(const void* p) {
    MEMORY_BASIC_INFORMATION mi{};
    return VirtualQuery(p, &mi, sizeof(mi)) && mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY));
}

void* patch(void** slot, void* fn) {
    DWORD old;
    VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
    void* original = *slot;
    *slot = fn;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return original;
}

// The vtable FFakeStereoRendering's constructor installs: the first lea rax after the start of a function naming one of
// the stereo emulation cvars, recognised by IsStereoEnabled (mov al, 1) in slot 0 or 1.
void** find_vtable() {
    for (const Range& r : engine_code()) {
        for (const wchar_t* cvar : {L"r.StereoEmulationHeight", L"r.StereoEmulationFOV"}) {
            size_t len = wcslen(cvar) * 2;
            for (uint8_t* s = r.begin; s + len < r.end; ++s) {
                if (memcmp(s, cvar, len) != 0) continue;
                // ponytail: one linear pass over the image per string hit; under a second, once per start.
                for (uint8_t* p = r.begin; p + 7 < r.end; ++p) {
                    if (lea_target(p) != s) continue;
                    const uint8_t* fn = function_start(p);
                    for (const uint8_t* q = fn; q && q < fn + 0x100; ++q) {
                        if (q[0] != 0x48 || q[1] != 0x8D || q[2] != 0x05) continue;
                        auto** vt = (void**)lea_target(q);
                        if (!in(r, vt)) continue;
                        for (int slot : {0, 1})
                            if (in(r, vt[slot]) && follow((uint8_t*)vt[slot])[0] == 0xB0 && follow((uint8_t*)vt[slot])[1] == 0x01)
                                return vt;
                    }
                }
            }
        }
    }
    return nullptr;
}

// SSE instructions in a function's first bytes: CalculateStereoViewOffset is the first slot with plenty (rotator and
// vector maths); `doubles` tells UE5's large-world coordinates (scalar double ops outnumber single).
int sse_ops(const uint8_t* p, bool* doubles = nullptr) {
    static const uint8_t ops[] = {0x10, 0x11, 0x28, 0x29, 0x2A, 0x2C, 0x2D, 0x2E, 0x2F, 0x51, 0x54, 0x57, 0x58, 0x59, 0x5A,
                                  0x5C, 0x5E, 0x5F, 0x5D, 0xC6};
    p = follow(p);
    int n = 0, sd = 0, ss = 0;
    for (int i = 0; i < 0x180 && !(p[i] == 0xCC && p[i + 1] == 0xCC); ++i) {
        if (p[i] != 0x0F || !memchr(ops, p[i + 1], sizeof(ops))) continue;
        ++n;
        if (i > 0 && p[i - 1] == 0xF2) ++sd;
        if (i > 0 && p[i - 1] == 0xF3) ++ss;
    }
    if (doubles) *doubles = sd > ss;
    return n;
}

bool g_doubles;  // UE5: FVector, FRotator and FMatrix hold doubles

// The device's slots this adapter replaces. IStereoRendering keeps their order from UE 4.22 to 5.6: AdjustViewRect three
// before the view offset; after it the projection, InitCanvasFromView and RenderTexture_RenderThread; then
// GetRenderTargetManager, the first slot that isn't an empty function (UE 5.6 added one in between).
struct Slots {
    int stereo = -1, offset = -1, renderTargets = -1;
};

Slots find_slots(void** vt) {
    Slots s;
    s.stereo = follow((uint8_t*)vt[0])[0] == 0xB0 ? 0 : 1;
    for (int i = s.stereo + 1; i < 30 && s.offset < 0; ++i)
        if (sse_ops((uint8_t*)vt[i]) >= 10) s.offset = i;
    if (s.offset < 0) return s;
    sse_ops((uint8_t*)vt[s.offset], &g_doubles);
    auto empty = [&](int i) { uint8_t b = follow((uint8_t*)vt[i])[0]; return b == 0xC3 || b == 0xC2; };
    s.renderTargets = s.offset + 4;
    while (s.renderTargets < s.offset + 7 && empty(s.renderTargets)) ++s.renderTargets;
    return s;
}

// Console variables Unreal must run with for stereo: occlusion culling that hides things in one eye off,
// HDR and blurred UI off, and the UI kept out of the eyes (drawn over the window instead: the UI panel). Set in memory,
// where their registration (inlined into the engine's startup code) keeps the value: either a plain variable the
// console variable points at (lea r8 before the name), or the TConsoleVariableData pointer it stores after it (the second
// mov [rip], rax; UE 5.4 keeps only the object, the first). Zero is the same bits for int and float.
struct CVar {
    const wchar_t* name;
    uint32_t defaultBits;      // its default (1, or 1.0f): how UE 5.4's value pair is recognised in the object
    uint32_t* direct = nullptr;
    void** data = nullptr;     // the stored TConsoleVariableData* (UE 4 to 5.3)
    void** object = nullptr;   // the stored IConsoleVariable* (UE 5.4, which keeps no data pointer)
    uint32_t* pair = nullptr;  // ShadowedValue[2] (game and render thread), once found
};
CVar g_cvars[] = {{L"Slate.DrawToVRRenderTarget", 0x3F800000}, {L"r.HZBOcclusion", 1}, {L"r.InstanceCulling.OcclusionCull", 1},
                  {L"r.HDR.UI.CompositeMode", 1}, {L"Slate.AllowBackgroundBlurWidgets", 1}};

void find_cvars() {
    for (const Range& r : engine_code()) {
        for (CVar& c : g_cvars) {
            size_t len = (wcslen(c.name) + 1) * 2;
            for (uint8_t* s = r.begin; s + len < r.end && !c.direct && !c.object; ++s) {
                if (memcmp(s, c.name, len) != 0) continue;
                for (uint8_t* p = r.begin + 0x40; p + 0x80 < r.end && !c.direct && !c.object; ++p) {
                    if (p[0] != 0x48 || p[2] != 0x15 || lea_target(p) != s) continue;  // lea rdx, [name]
                    for (uint8_t* q = p - 0x30; q < p; ++q)
                        if (q[0] == 0x4C && q[1] == 0x8D && q[2] == 0x05 && writable(lea_target(q))) c.direct = (uint32_t*)lea_target(q);
                    int movs = 0;
                    for (uint8_t* q = p; q < p + 0x60 && !c.direct && movs < 2; ++q) {
                        if (q[0] != 0x48 || q[1] != 0x89 || q[2] != 0x05 || !writable(q + 7 + *(int32_t*)(q + 3))) continue;
                        (++movs == 1 ? c.object : c.data) = (void**)(q + 7 + *(int32_t*)(q + 3));
                    }
                }
            }
        }
    }
    for (const CVar& c : g_cvars) note("Unreal: %ls %s", c.name, c.direct ? "variable" : c.data ? "data" : c.object ? "object" : "not found");
}

// UE 5.4: the value pair sits in the console variable object, two equal words holding the default.
uint32_t* find_pair(uint8_t* object, uint32_t bits) {
    for (int off = 0x10; off < 0x100; off += 4) {
        auto* v = (uint32_t*)(object + off);
        if (read_ptr(v) && v[0] == bits && v[1] == bits) return v;
    }
    return nullptr;
}

void zero_cvars() {
    for (CVar& c : g_cvars) {
        if (!c.pair) {
            if (c.direct) c.pair = c.direct;
            else if (void* data = c.data ? *c.data : nullptr) c.pair = (uint32_t*)data;
            else if (void* obj = c.object ? *c.object : nullptr) c.pair = find_pair((uint8_t*)obj, c.defaultBits);
            if (!c.pair) continue;
            note("Unreal: %ls set to 0", c.name);
        }
        c.pair[0] = 0;
        if (!c.direct) c.pair[1] = 0;
    }
}

// --- 2. Eyes --------------------------------------------------------------------------------------------------------------
// Unreal: X forward, Y right, Z up, centimetres (world to metres), rotators in degrees (pitch, yaw, roll).
// OpenXR (the core): X right, Y up, -Z forward, metres.

using mathx::Q;
using mathx::V3;
constexpr double kDeg = 3.14159265358979 / 180;

V3 to_xr(const double ue[3], double scale) { return {(float)(ue[1] / scale), (float)(ue[2] / scale), (float)(-ue[0] / scale)}; }
void to_ue(const V3& v, double scale, double ue[3]) { ue[0] = -v.z * scale, ue[1] = v.x * scale, ue[2] = v.y * scale; }

Q rotator_to_q(const double r[3]) {
    double sp = std::sin(r[0] * kDeg), cp = std::cos(r[0] * kDeg), sy = std::sin(r[1] * kDeg), cy = std::cos(r[1] * kDeg),
           sr = std::sin(r[2] * kDeg), cr = std::cos(r[2] * kDeg);
    // Unreal's rotation matrix rows (forward, right, up), turned into OpenXR's axes: columns right, up, back.
    double f[3] = {cp * cy, cp * sy, sp}, rt[3] = {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp},
           u[3] = {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp};
    V3 X = to_xr(rt, 1), Y = to_xr(u, 1), Z = to_xr(f, 1) * -1;
    float t = X.x + Y.y + Z.z;
    Q q;
    if (t > 0) {
        float s = std::sqrt(t + 1) * 2;
        q = {(Y.z - Z.y) / s, (Z.x - X.z) / s, (X.y - Y.x) / s, s / 4};
    } else if (X.x > Y.y && X.x > Z.z) {
        float s = std::sqrt(1 + X.x - Y.y - Z.z) * 2;
        q = {s / 4, (Y.x + X.y) / s, (Z.x + X.z) / s, (Y.z - Z.y) / s};
    } else if (Y.y > Z.z) {
        float s = std::sqrt(1 + Y.y - X.x - Z.z) * 2;
        q = {(Y.x + X.y) / s, s / 4, (Z.y + Y.z) / s, (Z.x - X.z) / s};
    } else {
        float s = std::sqrt(1 + Z.z - X.x - Y.y) * 2;
        q = {(Z.x + X.z) / s, (Z.y + Y.z) / s, s / 4, (X.y - Y.x) / s};
    }
    return q;
}

void q_to_rotator(const Q& q, double r[3]) {
    double f[3], rt[3], u[3];
    to_ue(q * V3{0, 0, -1}, 1, f);
    to_ue(q * V3{1, 0, 0}, 1, rt);
    to_ue(q * V3{0, 1, 0}, 1, u);
    double yaw = std::atan2(f[1], f[0]);
    r[0] = std::atan2(f[2], std::hypot(f[0], f[1])) / kDeg;
    r[1] = yaw / kDeg;
    // Roll: the actual axes against the right axis of the same heading and pitch without roll.
    double sy[2] = {-std::sin(yaw), std::cos(yaw)};
    r[2] = std::atan2(u[0] * sy[0] + u[1] * sy[1], rt[0] * sy[0] + rt[1] * sy[1]) / kDeg;
}

void read3(void* p, double out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = g_doubles ? ((double*)p)[i] : ((float*)p)[i];
}
void write3(void* p, const double v[3]) {
    for (int i = 0; i < 3; ++i) g_doubles ? (void)(((double*)p)[i] = v[i]) : (void)(((float*)p)[i] = (float)v[i]);
}

using OffsetFn = void (*)(void*, int32_t, void*, float, void*);
using ProjectionFn = void* (*)(void*, void*, int32_t);
OffsetFn g_offset;
ProjectionFn g_projection;
std::atomic<bool> g_vr;      // the headset runs (or a test): eyes and projection are ours

std::mutex g_frameMutex;     // bridge::frame from the game thread (stereo) or Present (no 3D view)
bool g_framed;               // a stereo view ran since the last Present
bool g_located;
bridge::Frame g_out;
double g_eyes[2][2][3];      // per eye: location, rotation (Unreal)
float g_near = 0.1f;         // metres
double g_scale = 100;        // Unreal units per metre
HWND g_window;
int g_eyeW = 1440, g_eyeH = 1600;  // the headset's recommended eye size
std::atomic<int> g_calls[4];  // tests: view offsets, projections, scene textures handed over, view rects

// Unreal 4 numbers its stereo passes 1 and 2 (0: the full, mono pass); Unreal 5 numbers views from 0.
int eye_of(int32_t index) {
    static std::atomic<bool> fromOne;
    if (index == 2) fromOne = true;
    if (index < 0) return -1;
    return fromOne ? index - 1 : index % 2;
}

// The game's objects (section 2, below): what the tools found and keep.
struct Box {
    double c[3], e[3];  // centre, half size (Unreal units)
};
struct Tools {
    int frames = 0;
    std::string scene, camera, autoCamera;  // the world's name; the view's camera (picked, or the player's own)
    std::vector<std::string> cameras;      // the world's cameras, for the menu's picker
    bool hiding = false, triggerWas = false;
    float reach = 3;                       // metres along the laser to what it would hide
    std::vector<ue::Obj*> hid, shown;      // hidden here; drawn primitives (while hiding)
    std::unordered_map<ue::Obj*, Box> bounds;
    size_t cursor = 0;
    std::vector<std::pair<ue::Obj*, std::vector<double>>> held;  // moved to the hand: their relative locations
    ULONGLONG listedAt = 0, rehiddenAt = 0;
} g_tools;
bridge::MenuHost g_host;

bridge::Game game_now() {
    bridge::Game g;
    CURSORINFO ci{sizeof(ci)};
    g.cursorFree = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING);
    g.padGame = true;  // Unreal games read XInput pads
    g.noTheater = true;
    g.carried = true;
    RECT rc{};
    if (g_window && GetClientRect(g_window, &rc) && rc.bottom > 0) {
        g.screenW = rc.right, g.screenH = rc.bottom;
        g.uiAspect = (float)rc.right / rc.bottom;
        g.fovY = 2 * std::atan(1 / g.uiAspect);  // the stereo device's 90 degrees across: the game's own isn't kept
    }
    g.nearClip = g_near;
    g.pointerClick = true;
    g.crosshair = true;
    g.scene = g_tools.scene, g.cameraName = g_tools.camera;
    g.worldPointer = g_tools.hiding, g.worldReach = g_tools.reach;
    return g;
}

void apply(const bridge::Frame& f) {
    g_out = f;
    if (f.quit && g_window) PostMessageW(g_window, WM_CLOSE, 0, 0);
}

// Tests: eyes 6.4 cm apart around the game camera, a 100 degree square view.
bool test_frame(const double loc[3], const double rot[3], bridge::Frame& f) {
    Q q = rotator_to_q(rot);
    V3 p = to_xr(loc, g_scale);
    for (int e = 0; e < 2; ++e) {
        V3 eye = p + q * V3{e ? 0.032f : -0.032f, 0, 0};
        f.eyes[e] = {{eye.x, eye.y, eye.z}, {q.x, q.y, q.z, q.w}};
        f.fovs[e] = {-0.87f, 0.87f, 0.87f, -0.87f};
    }
    return true;
}

// --- 2. The game's objects (uobject.h): the VR menu's game camera, hide objects and weapon in hand ---------------------

bool flag(const ue::Obj* o, const char* property) {
    double v = 0;
    return ue::get_numbers(o, property, &v, 1) && v != 0;
}
bool drawn(const ue::Obj* c) { return flag(c, "bVisible") && !flag(c, "bHiddenInGame"); }

bool in_world(const ue::Obj* o, const ue::Obj* world) {
    for (int i = 0; o && i < 8; ++i, o = ue::outer(o))
        if (o == world) return true;
    return false;
}

void set_hidden(ue::Obj* o, bool hidden) {
    ue::Invoke(o, "SetHiddenInGame").set("NewHidden", {hidden ? 1. : 0.}).set("bPropagateToChildren", {1}).run();
}

std::vector<std::string> hidden_paths() {
    std::vector<std::string> out;
    std::string all = bridge::setting("Game", "Hidden", "");
    for (size_t a = 0, b; a < all.size(); a = b + 1) {
        b = all.find(';', a);
        if (b == std::string::npos) b = all.size();
        if (b > a) out.push_back(all.substr(a, b - a));
    }
    return out;
}

// Every two seconds: the world's cameras for the picker, the primitives the laser can hide, hidden ones hidden again
// (levels load, games show things again); the menu told.
void list_objects(ue::Obj* world) {
    Tools& t = g_tools;
    static ue::Obj *cameraClass = ue::find_class("CameraComponent"), *primClass = ue::find_class("PrimitiveComponent");
    t.cameras.clear();
    t.autoCamera.clear();
    ue::Obj* pawn = ue::pawn();
    for (ue::Obj* c : ue::objects_of(cameraClass)) {
        if (!in_world(c, world)) continue;
        t.cameras.push_back(ue::path(c));
        if (t.autoCamera.empty() && pawn && ue::outer(c) == pawn) t.autoCamera = t.cameras.back();
    }
    if (t.cameras != g_host.cameras) {
        std::string all;
        for (const std::string& c : t.cameras) all += (all.empty() ? "" : ", ") + c;
        note("Unreal: cameras in %s: %s", ue::name(world).c_str(), all.empty() ? "none" : all.c_str());
    }
    t.shown.clear();
    if (t.hiding)
        for (ue::Obj* c : ue::objects_of(primClass))
            if (in_world(c, world) && drawn(c)) t.shown.push_back(c);
    std::vector<std::string> paths = hidden_paths();
    for (const std::string& p : paths)
        if (ue::Obj* o = ue::find(p); o && drawn(o)) {
            set_hidden(o, true);
            if (std::find(t.hid.begin(), t.hid.end(), o) == t.hid.end()) t.hid.push_back(o);
        }
    g_host.hideObjects = g_host.handHeld = true;
    g_host.cameras = t.cameras;
    g_host.currentCamera = t.camera;
    g_host.hidden = (int)paths.size();
    bridge::set_menu_host(g_host);
}

// Before the rig, on the game thread: object access once the game runs, the scene's name, and the picked camera's
// view in place of the game's.
void tools_view(double loc[3], double rot[3]) {
    Tools& t = g_tools;
    if (++t.frames < 120 || !ue::init()) return;
    ue::Obj* world = ue::world();
    t.scene = world ? ue::name(world) : "";
    ULONGLONG now = GetTickCount64();
    if (now - t.listedAt > 2000 && world) t.listedAt = now, list_objects(world);
    std::string picked = bridge::setting("Game", "CameraName", "");
    static ue::Obj* cameraClass = ue::find_class("CameraComponent");
    ue::Obj* cam = picked.empty() ? nullptr : ue::find(picked);
    ue::Invoke l(cam, "K2_GetComponentLocation"), r(cam, "K2_GetComponentRotation");
    if (cam && ue::is_a(cam, cameraClass) && l.run() && r.run()) {
        l.get("ReturnValue", loc, 3), r.get("ReturnValue", rot, 3);
        t.camera = picked;
    } else {
        t.camera = t.autoCamera;
    }
}

V3 hand_forward(const bridge::Pose& p) { return Q{p.q[0], p.q[1], p.q[2], p.q[3]} * V3{0, 0, -1}; }

// The ray from the right hand against the drawn primitives' bounds (a few hundred measured a frame); the trigger
// hides the nearest (kept per game by path), B or the VR menu puts the tool away.
bool ray_box(const double o[3], const double d[3], const Box& b, double& hit) {
    double enter = -1e30, leave = 1e30;
    for (int i = 0; i < 3; ++i) {
        double lo = b.c[i] - b.e[i], hi = b.c[i] + b.e[i];
        if (std::fabs(d[i]) < 1e-9) {
            if (o[i] < lo || o[i] > hi) return false;
            continue;
        }
        double t1 = (lo - o[i]) / d[i], t2 = (hi - o[i]) / d[i];
        enter = std::max(enter, std::min(t1, t2)), leave = std::min(leave, std::max(t1, t2));
    }
    if (enter > leave || enter <= 0) return false;  // missed, behind, or the hand is inside it
    return hit = enter, true;
}

void aim_hider(const bridge::Frame& f) {
    Tools& t = g_tools;
    if (f.menuOpen || f.bDown) return void(t.hiding = false);
    if (!f.rightValid) return;
    ue::Obj* lib = ue::statics("KismetSystemLibrary");
    for (int k = 0; k < 300 && !t.shown.empty(); ++k) {
        t.cursor %= t.shown.size();
        ue::Obj* c = t.shown[t.cursor++];
        ue::Invoke b(lib, "GetComponentBounds");
        Box box{};
        if (ue::alive(c) && b.set("Component", c).run() && b.get("Origin", box.c, 3) == 3 && b.get("BoxExtent", box.e, 3) == 3)
            t.bounds[c] = box;
    }
    double o[3], d[3], best = 1e30;
    to_ue({f.right.p[0], f.right.p[1], f.right.p[2]}, g_scale, o);
    V3 fwd = hand_forward(f.right);
    d[0] = -fwd.z, d[1] = fwd.x, d[2] = fwd.y;
    ue::Obj* target = nullptr;
    for (ue::Obj* c : t.shown) {
        auto it = t.bounds.find(c);
        double hit;
        if (it != t.bounds.end() && ray_box(o, d, it->second, hit) && hit < best) best = hit, target = c;
    }
    t.reach = target ? (float)(best / g_scale / std::max(f.scale, 1e-4f)) : 3.f;
    bool trigger = f.rightTrigger > 0.6f;
    if (trigger && !t.triggerWas && target && ue::alive(target)) {
        set_hidden(target, true);
        t.hid.push_back(target);
        std::string all = bridge::setting("Game", "Hidden", ""), p = ue::path(target);
        bridge::set_setting("Game", "Hidden", all.empty() ? p : all + ";" + p);
        t.shown.erase(std::find(t.shown.begin(), t.shown.end(), target));
        g_host.hidden = (int)hidden_paths().size();
        bridge::set_menu_host(g_host);
        note("Unreal: hid %s", p.c_str());
    }
    t.triggerWas = trigger;
}

// Weapon in hand: what the view's camera carries (the arms, the weapon) moved to the right hand while hand aiming,
// put back before the next frame.
void hold(const bridge::Frame& f) {
    Tools& t = g_tools;
    for (auto& [c, rel] : t.held)
        if (ue::alive(c)) ue::Invoke(c, "K2_SetRelativeLocation").set("NewLocation", {rel[0], rel[1], rel[2]}).set("bTeleport", {1}).run();
    t.held.clear();
    std::string on = bridge::setting("Controls", "WeaponInHand", "true");
    if (!f.aiming || !f.handAiming || !f.rightValid || (on != "true" && on != "True")) return;
    ue::Obj* cam = t.camera.empty() ? nullptr : ue::find(t.camera);
    std::vector<ue::Obj*> carried;
    for (ue::Obj* c : ue::get_objects(cam, "AttachChildren"))
        if (ue::alive(c) && drawn(c)) carried.push_back(c);
    if (carried.empty()) return;
    double middle[3] = {}, at[3];
    std::vector<std::vector<double>> where;
    for (ue::Obj* c : carried) {
        ue::Invoke l(c, "K2_GetComponentLocation");
        std::vector<double> p(3);
        l.run(), l.get("ReturnValue", p.data(), 3);
        for (int i = 0; i < 3; ++i) middle[i] += p[i] / carried.size();
        where.push_back(p);
    }
    V3 hand = V3{f.right.p[0], f.right.p[1], f.right.p[2]} + hand_forward(f.right) * (0.08f * f.scale);
    to_ue(hand, g_scale, at);
    for (size_t k = 0; k < carried.size(); ++k) {
        std::vector<double> rel(3);
        if (ue::get_numbers(carried[k], "RelativeLocation", rel.data(), 3) != 3) continue;
        t.held.push_back({carried[k], rel});
        const std::vector<double>& p = where[k];
        ue::Invoke(carried[k], "K2_SetWorldLocation")
            .set("NewLocation", {p[0] + at[0] - middle[0], p[1] + at[1] - middle[1], p[2] + at[2] - middle[2]})
            .set("bTeleport", {1})
            .run();
    }
}

// After the rig: the menu's requests and the tools.
void tools_frame(const bridge::Frame& f) {
    Tools& t = g_tools;
    if (t.frames < 120 || !ue::init()) return;
    hold(f);
    if (f.showHidden) {
        for (ue::Obj* o : t.hid)
            if (ue::alive(o)) set_hidden(o, false);
        t.hid.clear();
        bridge::set_setting("Game", "Hidden", "");
        g_host.hidden = 0;
        bridge::set_menu_host(g_host);
    }
    if (f.hideObjects) t.hiding = t.triggerWas = true, t.listedAt = 0;  // the trigger that chose the tool goes up first
    if (t.hiding) aim_hider(f);
    if (!t.hiding) t.shown.clear(), t.bounds.clear();
}

// Tests: the objects the tools start from, once, 600 frames in.
void probe_objects() {
    note("Unreal objects: the view runs on the %s thread", GetCurrentThreadId() == g_mainThread ? "main" : "NOT main");
    if (!ue::init()) return;
    auto show = [](const char* what, ue::Obj* o) {
        note("Unreal objects: %s = %s (%s)", what, o ? ue::path(o).c_str() : "null", o ? ue::name(ue::class_of(o)).c_str() : "-");
    };
    show("world", ue::world());
    show("controller", ue::player_controller());
    show("pawn", ue::pawn());
    show("camera manager", ue::camera_manager());
}

void ui_target();  // section 2, render target

// The engine asks for the left eye first: that call runs the rig with the game camera and places both eyes.
void offset_hook(void* self, int32_t index, void* rotation, float worldToMeters, void* location) {
    ++g_calls[0];
    ue::set_game_thread();
    static int probeFrames;
    if (!g_test.empty() && index >= 0 && eye_of(index) == 0 && ++probeFrames == 600) probe_objects();
    int e = eye_of(index);
    if (e < 0 || e > 1 || !g_vr) return g_offset(self, index, rotation, worldToMeters, location);
    double scale = g_scale = worldToMeters > 0 ? worldToMeters : 100;
    if (e == 0) {
        double loc[3], rot[3];
        read3(location, loc);
        read3(rotation, rot);
        ui_target();
        std::lock_guard lock(g_frameMutex);
        bridge::Frame f;
        if (!g_test.empty()) {
            g_located = test_frame(loc, rot, f);
        } else {
            tools_view(loc, rot);
            bridge::Game g = game_now();
            g.hasCamera = true;
            V3 p = to_xr(loc, scale);
            Q q = rotator_to_q(rot);
            g.camera = {{p.x, p.y, p.z}, {q.x, q.y, q.z, q.w}};
            g_located = bridge::frame(g, f);
            tools_frame(f);
        }
        g_framed = true;
        if (g_located) {
            apply(f);
            for (int i = 0; i < 2; ++i) {
                // 3D screen: the window's eyes, from the game camera along its right, looking where it looks.
                if (f.mode == 1) {
                    to_ue(to_xr(loc, scale) + rotator_to_q(rot) * V3{f.windowOffset[i], 0, 0}, scale, g_eyes[i][0]);
                    std::copy(rot, rot + 3, g_eyes[i][1]);
                    continue;
                }
                const bridge::Pose& eye = f.eyes[i];
                to_ue({eye.p[0], eye.p[1], eye.p[2]}, scale, g_eyes[i][0]);
                q_to_rotator({eye.q[0], eye.q[1], eye.q[2], eye.q[3]}, g_eyes[i][1]);
            }
        }
    }
    if (!g_located) return g_offset(self, index, rotation, worldToMeters, location);
    write3(location, g_eyes[e][0]);
    write3(rotation, g_eyes[e][1]);
}

// The headset's asymmetric frustum (3D screen: the window's) in Unreal's reversed-Z, infinite-far form, near plane kept
// from the game.
void* projection_hook(void* self, void* out, int32_t index) {
    ++g_calls[1];
    g_projection(self, out, index);
    int e = eye_of(index);
    if (e < 0 || e > 1 || !g_vr || !g_located) return out;
    double nearZ = g_doubles ? ((double*)out)[14] : ((float*)out)[14];
    g_near = (float)(nearZ / g_scale);
    const bridge::Fov& fov = g_out.mode == 1 ? g_out.windowFov[e] : g_out.fovs[e];
    double l = std::tan(fov.left), r = std::tan(fov.right), u = std::tan(fov.up), d = std::tan(fov.down);
    double m[16] = {2 / (r - l), 0, 0, 0, 0, 2 / (u - d), 0, 0, -(r + l) / (r - l), -(u + d) / (u - d), 0, 1, 0, 0, nearZ, 0};
    for (int i = 0; i < 16; ++i) g_doubles ? (void)(((double*)out)[i] = m[i]) : (void)(((float*)out)[i] = (float)m[i]);
    return out;
}

// --- 2. Render target: both eyes side by side at the headset's resolution, off the window ---------------------------------

struct Layout {
    uint32_t eyeW = 0, eyeH = 0;
    bool operator!=(const Layout& o) const { return eyeW != o.eyeW || eyeH != o.eyeH; }
};
Layout g_have;  // as the engine has it
bool g_uiInEyes;  // UE 5.5+: Slate draws the UI into the viewport's texture (redirected: ui_target)

Layout layout_wanted() {
    float scale = bridge::render_scale();
    return {(uint32_t)(g_eyeW * scale), (uint32_t)(g_eyeH * scale)};
}

using AdjustFn = void (*)(void*, int32_t, int32_t*, int32_t*, uint32_t*, uint32_t*);
AdjustFn g_adjust;

// Each eye's place in our render target (the device would halve the window's size: UE 5.4 then draws both eyes into
// a window-sized corner of it).
void adjust_hook(void* self, int32_t index, int32_t* x, int32_t* y, uint32_t* w, uint32_t* h) {
    ++g_calls[3];
    int e = eye_of(index);
    Layout l = g_have;
    if (e < 0 || e > 1 || !l.eyeW) return g_adjust(self, index, x, y, w, h);
    *x = e * (int32_t)l.eyeW, *y = 0, *w = l.eyeW, *h = l.eyeH;
}

// Our IStereoRenderTargetManager: a vtable of plain functions (the object is only its vtable pointer).
void* g_rtmTable[32];
struct {
    void** vtable = g_rtmTable;
} g_rtm;

uintptr_t g_sizeReturn;  // where the engine asked for our size: it asks for the buffered frames right after

// UE 5.5+: the engine asks for these on the render thread as it draws the UI, after it set its Slate handle back to the
// eye texture for the frame: the handle goes to the UI's texture again then (ui_target).
void on_render_ask();
uint64_t rtm_zero(void*) { return on_render_ask(), 0; }  // every other slot: false, nothing, PF_Unknown, "allocate it yourself"
uint64_t rtm_one(void*) { return on_render_ask(), 1; }   // ShouldUseSeparateRenderTarget
// Slots 5 and 6: GetNumberOfBufferedFrames is 5 before UE 4.26 and 6 after (NeedReAllocateShadingRateTexture came in
// before it, and AllocateRenderTargetTexture follows it). The call right after CalculateRenderTargetSize is that one
// (told by its return address): 1 frame; anything else answers false.
uint64_t rtm_slot5_6(void*) { return (uintptr_t)_ReturnAddress() - g_sizeReturn < 0x40 ? 1 : 0; }
void rtm_size(void*, void*, uint32_t* x, uint32_t* y) {
    g_sizeReturn = (uintptr_t)_ReturnAddress();
    g_have = layout_wanted();
    *x = g_have.eyeW * 2, *y = g_have.eyeH;
}
bool rtm_realloc(void*, void*) { return layout_wanted() != g_have; }
void* rtm_hook(void*) { return on_render_ask(), &g_rtm; }

// --- 2. Scene texture ---------------------------------------------------------------------------------------------------
// It reaches the stereo device every frame in RenderTexture_RenderThread (which would copy it into the window; skipped
// while VR runs, so the window keeps only the UI). Its native resource sits a few pointers into Unreal's RHI texture:
// found once by its vtable (one of the device's textures') and size.

std::vector<void*> g_nativeVtables;
std::atomic<bool> g_nativeReady;
std::atomic<void*> g_scene;  // ID3D11Texture2D* / ID3D12Resource*
uint32_t g_sceneW, g_sceneH;
void* g_lastRhi;
int g_api;

// The vtables Direct3D gives its textures on this device: the swapchain's and one of our own.
void collect_native_vtables(IUnknown* back) {
    g_nativeVtables.push_back(*(void**)back);
    if (g_api == 11) {
        ComPtr<ID3D11Device> device;
        ((ID3D11Texture2D*)back)->GetDevice(&device);
        D3D11_TEXTURE2D_DESC d{4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
        ComPtr<ID3D11Texture2D> t;
        if (device && SUCCEEDED(device->CreateTexture2D(&d, nullptr, &t))) g_nativeVtables.push_back(*(void**)t.Get());
    } else {
        ComPtr<ID3D12Device> device;
        ((ID3D12Resource*)back)->GetDevice(IID_PPV_ARGS(&device));
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                              D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
        ComPtr<ID3D12Resource> t;
        if (device && SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON,
                                                                nullptr, IID_PPV_ARGS(&t))))
            g_nativeVtables.push_back(*(void**)t.Get());
    }
}

bool native_size(void* p, uint32_t& w, uint32_t& h) {
    if (g_api == 11) {
        D3D11_TEXTURE2D_DESC d;
        ((ID3D11Texture2D*)p)->GetDesc(&d);
        w = d.Width, h = d.Height;
    } else {
        auto d = ((ID3D12Resource*)p)->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return false;
        w = (uint32_t)d.Width, h = d.Height;
    }
    return true;
}

// Breadth-first through the RHI texture's pointers to the native texture of our render target's size.
void* find_native(void* rhi) {
    std::vector<void*> level{rhi}, seen;
    for (int depth = 0; depth < 4 && !level.empty(); ++depth) {
        std::vector<void*> next;
        for (void* obj : level) {
            for (int off = 0; off < 0x200; off += 8) {
                void* p = read_ptr((uint8_t*)obj + off);
                if ((uintptr_t)p < 0x10000 || (uintptr_t)p & 7) continue;
                void* vt = read_ptr(p);
                uint32_t w, h;
                if (std::find(g_nativeVtables.begin(), g_nativeVtables.end(), vt) != g_nativeVtables.end() &&
                    native_size(p, w, h) && w + 16 >= g_have.eyeW * 2 && w <= g_have.eyeW * 2 + 16 && h + 16 >= g_have.eyeH && h <= g_have.eyeH + 16) {
                    g_sceneW = w, g_sceneH = h;
                    return p;
                }
                if (next.size() < 2048 && std::find(seen.begin(), seen.end(), p) == seen.end()) next.push_back(p), seen.push_back(p);
            }
        }
        level.swap(next);
    }
    return nullptr;
}

// The chain of objects from an engine object to a native texture of size w x h (breadth-first, `span` bytes each).
std::vector<std::pair<void*, int>> native_chain(void* start, uint32_t want_w, uint32_t want_h, int span = 0x200) {
    struct Node { void* p; int parent, off; };
    std::vector<Node> nodes{{start, -1, 0}};
    std::vector<void*> seen{start};
    size_t begin = 0;
    for (int depth = 0; depth < 4; ++depth) {
        size_t end = nodes.size();
        for (size_t i = begin; i < end; ++i)
            for (int off = 0; off < span; off += 8) {
                void* p = read_ptr((uint8_t*)nodes[i].p + off);
                if ((uintptr_t)p < 0x10000 || (uintptr_t)p & 7) continue;
                uint32_t w, h;
                if (std::find(g_nativeVtables.begin(), g_nativeVtables.end(), read_ptr(p)) != g_nativeVtables.end() &&
                    native_size(p, w, h) && w == want_w && h == want_h) {
                    std::vector<std::pair<void*, int>> out{{p, off}};
                    for (int k = (int)i; k >= 0; k = nodes[k].parent) out.insert(out.begin(), {nodes[k].p, nodes[k].off});
                    return out;
                }
                if (nodes.size() < 8192 && std::find(seen.begin(), seen.end(), p) == seen.end())
                    nodes.push_back({p, (int)i, off}), seen.push_back(p);
            }
        begin = end;
    }
    return {};
}


// UE 5.5+: Slate draws the game's UI into the viewport's own texture (its handle on it), laid out at the window's
// size and stretched over the whole texture. Its handles are pointed at a texture of the window's size instead: a
// render target the engine makes for us (kept from garbage collection; a map change still takes it, and another is
// made), cleared each frame.
ue::Obj* g_uiTarget;                 // game thread
uint32_t g_uiW, g_uiH;
std::atomic<void*> g_uiRhi, g_uiNative;  // its engine and native textures
void* g_viewport;                    // the game viewport (FSceneViewport), found from its client

void ui_target() {
    static int frames;
    if (!g_uiInEyes || ++frames < 120 || !ue::init() || !g_window) return;
    ue::Obj* world = ue::world();
    ue::Obj* lib = ue::statics("KismetRenderingLibrary");
    if (!world || !lib) return;
    RECT rc{};
    GetClientRect(g_window, &rc);
    if (rc.right < 64 || rc.bottom < 64) return;
    // A new window size: a new target (the old one stays, unused).
    if (!g_uiTarget || !ue::alive(g_uiTarget) || g_uiW != (uint32_t)rc.right || g_uiH != (uint32_t)rc.bottom) {
        // Made in the map: a new map takes it, and a new one is made (Slate's handles keep the old texture alive until
        // they move on).
        ue::Invoke make(lib, "CreateRenderTarget2D");
        make.set("WorldContextObject", world).set("Width", {(double)rc.right}).set("Height", {(double)rc.bottom});
        make.set("Format", {2}).set("ClearColor", {0, 0, 0, 0});  // RTF_RGBA8
        ue::Obj* t = make.run() ? make.get("ReturnValue") : nullptr;
        if (g_uiTarget) note("Unreal: the UI's texture went (%s)", ue::alive(g_uiTarget) ? "window resized" : "collected");
        if (!t || !ue::keep(t)) return;
        g_uiTarget = t, g_uiW = rc.right, g_uiH = rc.bottom, g_uiRhi = g_uiNative = nullptr;
        note("Unreal: the game's UI gets its own %ux%u texture", g_uiW, g_uiH);
    }
    if (!g_uiRhi) {
        auto chain = native_chain(g_uiTarget, g_uiW, g_uiH, 0x400);  // the object, its resource, the RHI texture, native
        if (chain.size() < 3) return;
        g_uiNative = chain.back().first;
        g_uiRhi = chain[chain.size() - 2].first;
    }
    ue::Invoke(lib, "ClearRenderTarget2D").set("WorldContextObject", world).set("TextureRenderTarget", g_uiTarget)
        .set("ClearColor", {0, 0, 0, 0}).run();
    if (!g_viewport) {
        ue::Obj* client = ue::get_object(ue::engine(), "GameViewport");
        // FViewport starts as an FRenderTarget: its vtable, then its texture. The client points at it early on.
        for (int off = 0; client && off < 0x400 && !g_viewport; off += 8) {
            void* v = read_ptr(client + off);
            if ((uintptr_t)v >= 0x10000 && !((uintptr_t)v & 7) && read_ptr((uint8_t*)v + 8) == g_lastRhi && g_lastRhi) g_viewport = v;
        }
    }
}

int32_t* refs(void* rhi) { return (int32_t*)((uint8_t*)rhi + 8); }  // FRHIResource's count (and flags above bit 29)

// Render thread: the viewport's Slate handles (an object whose second word is the eye texture, not a texture itself:
// one pointer on from the viewport, or in a short array there) moved to the UI's texture, the counts kept right.
void redirect_ui(void* eye) {
    void* ui = g_uiRhi;
    uint8_t* vp = (uint8_t*)g_viewport;
    if (!ui || !vp || !eye) return;
    // Slate's handles share one class (FSlateRenderTargetRHI): the first found sets it.
    static void* handleClass;
    auto take = [&](uint8_t* h) {
        if ((uintptr_t)h < 0x10000 || (uintptr_t)h & 7 || read_ptr(h + 8) != eye || read_ptr(h) == read_ptr(eye)) return;
        if (handleClass && read_ptr(h) != handleClass) return;
        handleClass = read_ptr(h);
        _InterlockedIncrement((volatile long*)refs(ui));
        _InterlockedDecrement((volatile long*)refs(eye));  // the viewport keeps its own counts on it
        *(void**)(h + 8) = ui;
        static std::vector<uint8_t*> told;
        if (std::find(told.begin(), told.end(), h) == told.end())
            told.push_back(h), note("Unreal: Slate draws the UI into its own texture (handle %p; counts: UI %08x, eye %08x)", h, *refs(ui), *refs(eye));
    };
    for (int off = 0x10; off < 0x1000; off += 8) {
        uint8_t* p = (uint8_t*)read_ptr(vp + off);
        if ((uintptr_t)p < 0x10000 || (uintptr_t)p & 7) continue;
        take(p);
        int n = (int)(uintptr_t)read_ptr(vp + off + 8);
        if (n >= 1 && n <= 4)
            for (int i = 0; i < n; ++i) take((uint8_t*)read_ptr(p + i * 8));
    }
}

std::atomic<DWORD> g_renderThread;  // where RenderTexture_RenderThread runs

void on_render_ask() {
    if (g_uiInEyes && g_vr && GetCurrentThreadId() == g_renderThread) redirect_ui(g_lastRhi);
}

using RenderTextureFn = void (*)(void*, void*, void*, void*, void*);
RenderTextureFn g_renderTexture;

void render_texture_hook(void* self, void* cmd, void* backbuffer, void* src, void* windowSize) {
    ++g_calls[2];
    g_renderThread = GetCurrentThreadId();
    // Tests: the textures' counts stay put while the handles move every frame.
    if (!g_test.empty() && g_uiRhi && (g_calls[2] == 3000 || g_calls[2] == 6000))
        note("Unreal test: counts at frame %d: UI %08x, eye %08x", g_calls[2].load(), *refs(g_uiRhi), *refs(g_lastRhi));
    // The headset not running yet: the device's own copy of both eyes into the window, so the game stays visible.
    if (!g_vr) return g_renderTexture(self, cmd, backbuffer, src, windowSize);
    if (!src || !g_nativeReady) return;  // the device's texture vtables come with the first Present
    if (src != g_lastRhi && src != g_uiRhi) {
        if (void* native = find_native(src)) {
            g_lastRhi = src, g_scene = native;
            note("Unreal: scene texture %p -> %p (%ux%u)", src, native, g_sceneW, g_sceneH);
        }
    }
    if (g_uiInEyes) redirect_ui(g_lastRhi);
}

// --- 3. Present: start the core, submit the frame, wait for the headset ------------------------------------------------

int g_slot, g_presents;
bool g_started, g_dead;

void start(IDXGISwapChain* sc) {
    g_started = true;
    ComPtr<IUnknown> back;
    ComPtr<ID3D12Resource> r12;
    ComPtr<ID3D11Texture2D> r11;
    if (SUCCEEDED(sc->GetBuffer(0, IID_PPV_ARGS(&r12)))) back = r12, g_api = 12;
    else if (SUCCEEDED(sc->GetBuffer(0, IID_PPV_ARGS(&r11)))) back = r11, g_api = 11;
    else return (void)(g_dead = true);
    collect_native_vtables(back.Get());
    g_nativeReady = true;
    DXGI_SWAP_CHAIN_DESC desc{};
    sc->GetDesc(&desc);
    g_window = desc.OutputWindow;
    note("Unreal: Direct3D %d, window %ux%u", g_api, desc.BufferDesc.Width, desc.BufferDesc.Height);
    if (!g_test.empty()) return (void)(g_vr = true);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring name = wcsrchr(exe, L'\\') + 1;
    name = name.substr(0, name.find(L"-Win64-Shipping"));
    std::string app(WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, app.data(), (int)app.size(), nullptr, nullptr);
    app.pop_back();
    if (!prx_init(app.c_str(), g_api, &g_eyeW, &g_eyeH)) return (void)(g_dead = true);
    bridge::set_window(g_window);
    bridge::MenuHost host;
    host.product = app;
    host.gameDir = g_dir;
    host.eyeWidth = g_eyeW, host.eyeHeight = g_eyeH;
    host.noTheater = true;
    host.aimExtras = true;
    bridge::set_menu_host(host);
    gpu_grab_present = true;
    gpu_no_vsync = true;
    prx_set_init_texture(back.Get());
    event(EvInit);
}

// Tests: the scene texture and the window, as the engine left them, every 1200 frames (Direct3D 11 only).
void test_capture(IDXGISwapChain* sc) {
    if (g_api != 11 || ++g_presents % 1200 != 600) return;
    std::wstring n = std::to_wstring(g_presents / 1200);
    note("Unreal test: %d view offsets, %d projections, %d scene hand-overs, %d view rects", g_calls[0].load(),
         g_calls[1].load(), g_calls[2].load(), g_calls[3].load());
    ComPtr<ID3D11Texture2D> back;
    sc->GetBuffer(0, IID_PPV_ARGS(&back));
    ComPtr<ID3D11Device> device;
    back->GetDevice(&device);
    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    auto save = [&](ID3D11Texture2D* src, std::wstring name) {
        if (!src) return;
        D3D11_TEXTURE2D_DESC d;
        src->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING, d.BindFlags = 0, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ, d.MiscFlags = 0;
        d.MipLevels = d.ArraySize = 1;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &staging))) return;
        ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, src, 0, nullptr);
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
        gpu_write_bmp((g_test + L"\\" + name).c_str(), (const uint8_t*)m.pData, d.Width, d.Height, m.RowPitch, d.Format);
        ctx->Unmap(staging.Get(), 0);
        note("Unreal test: wrote %ls (%ux%u, format %d)", name.c_str(), d.Width, d.Height, (int)d.Format);
    };
    save((ID3D11Texture2D*)g_scene.load(), L"scene" + n + L".bmp");
    save(back.Get(), L"window" + n + L".bmp");
    save((ID3D11Texture2D*)g_uiNative.load(), L"ui" + n + L".bmp");
}

void on_present(IDXGISwapChain* sc) {
    if (!g_started) start(sc);
    if (g_vr) zero_cvars();  // every frame: the game's own settings may set them again
    RECT rc{};
    if (g_window) GetClientRect(g_window, &rc);
    if (!g_test.empty()) return test_capture(sc);
    drain_log();
    if (g_dead || prx_state() < StateInstance) return;
    {
        std::lock_guard lock(g_frameMutex);
        bool stereo = g_framed;
        g_framed = false;
        // No 3D view this frame (a loading screen, the headset not yet running): the rig still runs, for the menu and
        // the controls.
        if (!stereo) {
            bridge::Frame f;
            g_located = bridge::frame(game_now(), f);
            if (g_located) apply(f);
        }
        g_vr = g_located;
        if (g_located) {
            PrxFrame f{};
            bridge::layers(&f, rc.bottom > 0 ? (float)rc.right / rc.bottom : 16.f / 9.f);
            void* scene = g_scene;
            Layout l = g_have;
            float tw = (float)g_sceneW, th = (float)g_sceneH;
            // The UI panel: the window, holding the game's UI alone (its black keyed out); UE 5.5+: the UI's own texture.
            f.quad = f.quadAlpha = f.quadFromBackbuffer = 1;
            if (void* ui = g_uiNative) f.quadFromBackbuffer = 0, f.quadLayer = {ui, 0};
            // The eyes from the scene texture, side by side: Full VR's, or 3D screen's on the screen under the UI (the
            // core's own transparent eyes then carry the beam and the watch). Without a 3D view this frame only those.
            if (stereo && scene) {
                f.stereo = 1;
                if (g_out.mode == 1) f.window = 1, f.windowLayer[0] = f.windowLayer[1] = {scene, 0};
                else f.eye[0] = f.eye[1] = {scene, 0};
                for (int e = 0; e < 2; ++e) {
                    float crop[4] = {e * l.eyeW / tw, 0, (e + 1) * l.eyeW / tw, l.eyeH / th};
                    memcpy(f.eyeCrop[e], crop, sizeof(crop));
                }
            } else {
                f.stereo = f.beam || f.watch;
            }
            prx_set_frame(g_slot, &f);
            event(EvSubmit | (g_slot << 8));
            g_slot = (g_slot + 1) % 4;
        }
    }
    event(EvBegin);
}

// --- 4. Startup -----------------------------------------------------------------------------------------------------------

std::wstring g_cmd;           // the game's command line with our switches
LPWSTR(WINAPI* g_getCmd)();   // the system's GetCommandLineW
HANDLE g_scanned, g_decided;  // boot has looked for the stereo device; the command line has its answer
std::atomic<bool> g_deviceOk, g_stereo, g_answered;

// A headset answers now: an OpenXR instance (no graphics needed) and its head-mounted system, then gone again.
bool headset() {
    HMODULE m = LoadLibraryW((g_dir + L"\\openxr_loader.dll").c_str());
    auto gpa = m ? (PFN_xrGetInstanceProcAddr)GetProcAddress(m, "xrGetInstanceProcAddr") : nullptr;
    PFN_xrCreateInstance create = nullptr;
    if (!gpa || XR_FAILED(gpa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&create)))
        return note("Unreal: no OpenXR loader or runtime (%s)", m ? "no xrCreateInstance" : "openxr_loader.dll"), false;
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ici.applicationInfo.applicationName, "Parallax");
    strcpy_s(ici.applicationInfo.engineName, "Parallax");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    XrInstance instance;
    if (XrResult res = create(&ici, &instance); XR_FAILED(res)) return note("Unreal: OpenXR instance: error %d", (int)res), false;
    PFN_xrGetSystem getSystem = nullptr;
    PFN_xrDestroyInstance destroy = nullptr;
    gpa(instance, "xrGetSystem", (PFN_xrVoidFunction*)&getSystem);
    gpa(instance, "xrDestroyInstance", (PFN_xrVoidFunction*)&destroy);
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO, nullptr, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    XrSystemId system;
    XrResult res = getSystem ? getSystem(instance, &sgi, &system) : XR_ERROR_FUNCTION_UNSUPPORTED;
    bool ok = XR_SUCCEEDED(res);
    if (!ok) note("Unreal: OpenXR headset: error %d", (int)res);
    if (destroy) destroy(instance);
    return ok;
}

// IsStereoEnabled stays the device's own (always true): with the device present and stereo off, UE 5.0's scene renderer
// reads view -1 and crashes. Until the headset runs, the replaced functions pass through to the device's own.
void install(void** vt, const Slots& s) {
    g_offset = (OffsetFn)patch(&vt[s.offset], (void*)&offset_hook);
    g_projection = (ProjectionFn)patch(&vt[s.offset + 1], (void*)&projection_hook);
    g_adjust = (AdjustFn)patch(&vt[s.offset - 3], (void*)&adjust_hook);
    g_renderTexture = (RenderTextureFn)patch(&vt[s.offset + 3], (void*)&render_texture_hook);
    for (auto& f : g_rtmTable) f = (void*)&rtm_zero;
    g_rtmTable[0] = (void*)&rtm_one;
    g_rtmTable[2] = (void*)&rtm_size;
    g_rtmTable[3] = (void*)&rtm_realloc;
    g_rtmTable[5] = g_rtmTable[6] = (void*)&rtm_slot5_6;
    patch(&vt[s.renderTargets], (void*)&rtm_hook);
}

DWORD WINAPI boot(void*) {
    std::wstring dir = g_dir + L"\\ParallaxVR";
    CreateDirectoryW(dir.c_str(), nullptr);
    g_log = _wfsopen(((g_test.empty() ? dir : g_test) + L"\\parallax.log").c_str(), L"w", _SH_DENYNO);
    bridge::settings_open(dir + L"\\parallax.cfg");
    ULONGLONG t0 = GetTickCount64();
    void** vt = find_vtable();
    Slots s = vt ? find_slots(vt) : Slots{};
    g_deviceOk = s.offset >= 0;
    note("Unreal: engine read in %llu ms", GetTickCount64() - t0);
    SetEvent(g_scanned);
    if (!g_deviceOk) {
        note(vt ? "Unreal: stereo device found, its view offset not; VR stays off" : "Unreal: no FFakeStereoRendering in this game; VR stays off");
        return drain_log(), 0;
    }
    WaitForSingleObject(g_decided, INFINITE);
    if (!g_stereo) return note("Unreal: no headset; the game runs as it is"), drain_log(), 0;
    script::enable(ue::script_calls);  // game scripts: the shared calls and the game's objects
    note("Unreal: stereo vtable %p, IsStereoEnabled %d, view offset %d, %s precision, render targets %d", (void*)vt, s.stereo,
         s.offset, g_doubles ? "double" : "single", s.renderTargets);
    install(vt, s);
    find_cvars();
    // UE 5.5+ has no Slate.DrawToVRRenderTarget: its UI is redirected instead (ui_target).
    g_uiInEyes = !g_cvars[0].direct && !g_cvars[0].data && !g_cvars[0].object;
    if (g_test.empty()) prx_install_hooks();
    // Present on every swapchain, through one of our own (the vtable is DXGI's, shared by D3D11 and D3D12 games).
    ComPtr<ID3D11Device> probe;
    gpu_present_observer = &on_present;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &probe, nullptr,
                                 nullptr)) ||
        !gpu_hook_present(probe.Get()))
        note("Unreal: no Present hook; VR stays off");
    drain_log();
    return 0;
}

// This thread holds the loader lock (the PEB's, x64 offset 0x110) while DLLs initialise; boot can't run until it's free.
bool under_loader_lock() {
    auto* lock = *(CRITICAL_SECTION**)(__readgsqword(0x60) + 0x110);
    return (DWORD)(uintptr_t)lock->OwningThread == GetCurrentThreadId();
}

// The headset check in a process of its own (rundll32 on this DLL: ParallaxProbe): SteamVR's runtime can't start inside
// a game that hasn't reached its main yet, and takes about 20 s from cold.
bool headset_apart() {
    std::wstring cmd = L"rundll32.exe \"" + g_dir + L"\\dwmapi.dll\",ParallaxProbe";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return note("Unreal: the headset check didn't start (%lu)", GetLastError()), false;
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

// Stereo when boot found the stereo device and a headset answers, so a game started without one runs untouched.
void answer() {
    if (WaitForSingleObject(g_scanned, 20000) == WAIT_OBJECT_0 && g_deviceOk) {
        ULONGLONG t0 = GetTickCount64();
        g_stereo = !g_test.empty() || headset_apart();
        note("Unreal: headset check: %s (%llu ms)", g_stereo ? "found" : "none", GetTickCount64() - t0);
    }
    g_answered = true;
    SetEvent(g_decided);
}

// The engine's first read (the CRT's, before WinMain) gets the answer; reads while DLLs still load get the plain line.
LPWSTR WINAPI command_line() {
    static std::once_flag once;
    if (!g_answered && !under_loader_lock()) g_mainThread = GetCurrentThreadId(), std::call_once(once, answer);
    return g_answered && g_stereo ? g_cmd.data() : g_getCmd();
}
}  // namespace

void unreal_note(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char line[512];
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    note("%s", line);
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(self);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    g_dir = path;
    g_dir.resize(g_dir.rfind(L'\\'));
    // Loaded by rundll32 for the headset check (ParallaxProbe), not by the game beside it: nothing to set up.
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (_wcsnicmp(exe, g_dir.c_str(), g_dir.size()) != 0 || exe[g_dir.size()] != L'\\') return TRUE;
    if (GetFileAttributesW((g_dir + L"\\ParallaxVR\\ue-test").c_str()) != INVALID_FILE_ATTRIBUTES) g_test = g_dir + L"\\ParallaxVR";
    // The engine reads its command line once, early in WinMain: the stereo device, and the console variables that must
    // already hold at startup (HZB occlusion crashes the scene renderer's constructor in stereo, UE 5.0). find_cvars
    // keeps them, and those registered too late for this (Slate's), in memory afterwards.
    g_cmd = std::wstring(GetCommandLineW()) + L" -emulatestereo -ini:Engine:[SystemSettings]:r.EnableStereoEmulation=1,[SystemSettings]:r.HZBOcclusion=0,"
            L"[SystemSettings]:r.InstanceCulling.OcclusionCull=0,[SystemSettings]:r.HDR.UI.CompositeMode=0,"
            L"[SystemSettings]:Slate.AllowBackgroundBlurWidgets=0" + std::wstring(g_test.empty() ? L"" : L" -dx11");
    g_scanned = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_decided = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (MH_Initialize() == MH_OK && MH_CreateHookApi(L"kernelbase", "GetCommandLineW", (void*)&command_line, (void**)&g_getCmd) == MH_OK)
        MH_EnableHook(MH_ALL_HOOKS);
    if (HANDLE t = CreateThread(nullptr, 0, boot, nullptr, 0, nullptr)) CloseHandle(t);
    return TRUE;
}

// rundll32 "<dir>\dwmapi.dll",ParallaxProbe: exits 0 when a headset answers (see headset_apart).
extern "C" __declspec(dllexport) void CALLBACK ParallaxProbe(HWND, HINSTANCE, LPSTR, int) { ExitProcess(headset() ? 0 : 1); }
