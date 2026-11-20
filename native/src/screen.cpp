// Copyright (C) 2026 EncidXVII
// SPDX-License-Identifier: GPL-3.0-only

// PortXR Theater: the Theater screen for games PortXR cannot go inside (any engine, any store). It captures the
// game's window with Windows Graphics Capture and shows it on the curved screen through the same VR core the plugin
// uses; the controllers drive the keyboard and mouse, and a laser pointer clicks where it points while the game shows
// its cursor. Nothing is added to the game or its folder.
//
//   PortXRTheater.exe --dir <game folder> [--wait <seconds>] [--capture <dir> [--capture-frame <n>]]
//   PortXRTheater.exe --wake   (start the headset runtime and wait until it is ready, then exit)
//
// Waits for a window from a process started from <game folder>, follows it while the game runs (also when the game
// makes a new window), and exits once no such window is left. Log: %APPDATA%\PortXR\theater.log.
#include <unknwn.h>
#include "common.h"
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <cmath>
#include <cstdio>
#include <share.h>
#include <cstring>
#include <cwchar>
#include <vector>
#include <algorithm>
#include <string>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wd3d = winrt::Windows::Graphics::DirectX::Direct3D11;

extern "C" {
__declspec(dllimport) int32_t prx_init(const char* appName, int32_t api, int32_t* w, int32_t* h);
__declspec(dllimport) int32_t prx_state();
__declspec(dllimport) int32_t prx_passthrough();
__declspec(dllimport) void* prx_render_event_func();
__declspec(dllimport) void prx_set_init_texture(void* texture);
__declspec(dllimport) void prx_set_frame(int32_t slot, const PrxFrame* frame);
__declspec(dllimport) int32_t prx_locate(PrxPose* poses, PrxFov* fovs);
__declspec(dllimport) uint32_t prx_input_update(const PrxInputConfig* cfg, const PrxControllers* test, PrxControllers* out);
__declspec(dllimport) void prx_set_game_window(HWND hwnd);
__declspec(dllimport) void prx_haptic_pulse(int32_t hand, float amplitude, float seconds);
__declspec(dllimport) int32_t prx_poll_log(char* out, int32_t capacity);
__declspec(dllimport) void prx_capture(const wchar_t* dir);
__declspec(dllimport) void prx_get_stats(PrxStats* out);
}

namespace {
enum Event { EvInit = 1, EvBegin = 2, EvSubmit = 3, EvShutdown = 4 };
using RenderEvent = void(__stdcall*)(int);
RenderEvent g_event;

FILE* g_log;
void log(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}
void drain_core_log() {
    char line[1024];
    while (prx_poll_log(line, sizeof(line)) > 0) log("core: %s", line);
}

std::wstring appdata(const wchar_t* name) {
    CreateDirectoryW(prx_appdata(L"").c_str(), nullptr);
    return prx_appdata(name);
}

std::string read_file(const std::wstring& path) {
    std::string s;
    if (FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO)) {
        char buf[4096];
        for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) s.append(buf, n);
        fclose(f);
    }
    return s;
}

// "Key = value" in the [section] of the app's settings for all games (the same file the plugin reads).
std::string setting(const std::string& text, const char* section, const char* key) {
    std::string want = std::string("[") + section + "]";
    bool in = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty() && line[0] == '[') {
            in = line == want;
            continue;
        }
        size_t eq = line.find('=');
        if (!in || eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        while (!k.empty() && k.back() == ' ') k.pop_back();
        if (k != key) continue;
        size_t v = line.find_first_not_of(' ', eq + 1);
        return v == std::string::npos ? std::string() : line.substr(v);
    }
    return {};
}

float setting_f(const std::string& text, const char* section, const char* key, float fallback, float lo, float hi) {
    std::string v = setting(text, section, key);
    if (v.empty()) return fallback;
    float f = (float)atof(v.c_str());
    return f < lo ? lo : f > hi ? hi : f;
}

// The game's window: the largest visible top-level window of a process whose exe is in the game folder (also a
// launcher's child, e.g. Unreal's <name>-Win64-Shipping.exe two folders down).
std::wstring g_dir;
HWND g_best;
long long g_bestArea;

bool under_dir(const std::wstring& exe) {
    return exe.size() > g_dir.size() && _wcsnicmp(exe.c_str(), g_dir.c_str(), g_dir.size()) == 0 &&
           exe[g_dir.size()] == L'\\';
}

BOOL CALLBACK consider(HWND hwnd, LPARAM) {
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    if (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return TRUE;
    RECT r;
    GetClientRect(hwnd, &r);
    long long area = (long long)r.right * r.bottom;
    if (r.right < 320 || r.bottom < 200 || area <= g_bestArea) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return TRUE;
    wchar_t exe[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    bool ours = QueryFullProcessImageNameW(p, 0, exe, &n) && under_dir(std::wstring(exe, n));
    CloseHandle(p);
    if (ours) g_best = hwnd, g_bestArea = area;
    return TRUE;
}

HWND find_game_window() {
    g_best = nullptr;
    g_bestArea = 0;
    EnumWindows(consider, 0);
    return g_best;
}

// Rotation by `yaw` radians about +Y (OpenXR: Y up, -Z forward).
struct Quat { float x, y, z, w; };
Quat yaw_quat(float yaw) { return {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)}; }
void rotate(const Quat& q, const float v[3], float out[3]) {
    // v' = v + 2w(q x v) + 2 q x (q x v)
    float tx = 2 * (q.y * v[2] - q.z * v[1]), ty = 2 * (q.z * v[0] - q.x * v[2]), tz = 2 * (q.x * v[1] - q.y * v[0]);
    out[0] = v[0] + q.w * tx + (q.y * tz - q.z * ty);
    out[1] = v[1] + q.w * ty + (q.z * tx - q.x * tz);
    out[2] = v[2] + q.w * tz + (q.x * ty - q.y * tx);
}
Quat pose_quat(const PrxPose& p) { return {p.qx, p.qy, p.qz, p.qw}; }

// Theater screen: a curved section (arc length width) of a cylinder of radius `distance` around the head, centred
// straight ahead at eye height when placed. Flat where the runtime has no curved layers.
struct Screen {
    float center[3] = {0, 0, 0};
    float yaw = 0;
    float distance = 3, width = 3.2f, height = 1.8f;
    bool placed = false;
};

void place(Screen& s, const PrxPose eyes[2]) {
    for (int i = 0; i < 3; ++i) s.center[i] = 0.5f * ((&eyes[0].px)[i] + (&eyes[1].px)[i]);
    float fwd[3], minus_z[3] = {0, 0, -1};
    rotate(pose_quat(eyes[0]), minus_z, fwd);
    s.yaw = std::atan2(-fwd[0], -fwd[2]);
    s.placed = true;
}

// Aim ray (tracking space) to screen UV, top-left origin; false for a miss. The player stands inside the cylinder,
// so the hit is where the ray leaves it.
bool hit_screen(const Screen& s, bool curved, const PrxPose& aim, float uv[2], float point[3]) {
    Quat inv = yaw_quat(-s.yaw), rot = yaw_quat(s.yaw);
    float minus_z[3] = {0, 0, -1}, dw[3], d[3], rel[3] = {aim.px - s.center[0], aim.py - s.center[1], aim.pz - s.center[2]}, p[3];
    rotate(pose_quat(aim), minus_z, dw);
    rotate(inv, dw, d);
    rotate(inv, rel, p);
    float h[3];
    if (curved) {
        float R = s.distance, a = d[0] * d[0] + d[2] * d[2], b = 2 * (p[0] * d[0] + p[2] * d[2]),
              c = p[0] * p[0] + p[2] * p[2] - R * R, disc = b * b - 4 * a * c;
        if (a < 1e-6f || disc < 0) return false;
        float t = (-b + std::sqrt(disc)) / (2 * a);
        if (t <= 0) return false;
        for (int i = 0; i < 3; ++i) h[i] = p[i] + d[i] * t;
        uv[0] = std::atan2(h[0], -h[2]) * R / s.width + 0.5f;
    } else {
        if (d[2] > -1e-4f) return false;
        float t = (-s.distance - p[2]) / d[2];
        if (t <= 0) return false;
        for (int i = 0; i < 3; ++i) h[i] = p[i] + d[i] * t;
        uv[0] = h[0] / s.width + 0.5f;
    }
    uv[1] = 0.5f - h[1] / s.height;
    float world[3];
    rotate(rot, h, world);
    for (int i = 0; i < 3; ++i) point[i] = s.center[i] + world[i];
    return uv[0] >= 0 && uv[0] <= 1 && uv[1] >= 0 && uv[1] <= 1;
}

// Windows Graphics Capture of one window into `latest`, cropped to its client area.
struct Capture {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    wd3d::IDirect3DDevice rtDevice{nullptr};
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    winrt::Windows::Graphics::SizeInt32 poolSize{};
    HWND hwnd = nullptr;
    ComPtr<ID3D11Texture2D> latest;
    int width = 0, height = 0;
    bool fresh = false;

    void stop() {
        if (session) session.Close();
        if (pool) pool.Close();
        session = nullptr, pool = nullptr, item = nullptr, hwnd = nullptr;
    }

    bool start(HWND w) {
        stop();
        try {
            auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            winrt::check_hresult(interop->CreateForWindow(w, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
            poolSize = item.Size();
            pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
                rtDevice, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, poolSize);
            session = pool.CreateCaptureSession(item);
            session.IsCursorCaptureEnabled(true);
            // Windows 11 draws a yellow frame around captured windows unless the app may leave it out.
            try {
                session.IsBorderRequired(false);
            } catch (...) {
            }
            session.StartCapture();
            hwnd = w;
            return true;
        } catch (const winrt::hresult_error& e) {
            log("capture failed: %ls", e.message().c_str());
            stop();
            return false;
        }
    }

    // Copies the newest frame, if any, into `latest`.
    void update() {
        if (!pool) return;
        wgc::Direct3D11CaptureFrame frame{nullptr};
        for (auto f = pool.TryGetNextFrame(); f; f = pool.TryGetNextFrame()) frame = f;
        if (!frame) return;
        auto size = frame.ContentSize();
        if (size.Width != poolSize.Width || size.Height != poolSize.Height) {
            poolSize = size;
            pool.Recreate(rtDevice, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        }
        ComPtr<ID3D11Texture2D> src;
        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        if (FAILED(access->GetInterface(IID_PPV_ARGS(&src)))) return;
        // The capture holds the whole window (title bar and borders of a windowed game too): keep the client area.
        RECT frameRect{}, client{};
        DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frameRect, sizeof(frameRect));
        GetClientRect(hwnd, &client);
        POINT origin{0, 0};
        ClientToScreen(hwnd, &origin);
        LONG left = std::max(0L, origin.x - frameRect.left), top = std::max(0L, origin.y - frameRect.top);
        LONG w = std::min<LONG>(client.right, size.Width - left), h = std::min<LONG>(client.bottom, size.Height - top);
        if (w < 16 || h < 16) left = top = 0, w = size.Width, h = size.Height;
        if (!latest || w != width || h != height) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = (UINT)w, td.Height = (UINT)h, td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            latest.Reset();
            if (FAILED(device->CreateTexture2D(&td, nullptr, &latest))) return;
            width = w, height = h;
            log("game picture %dx%d", w, h);
        }
        D3D11_BOX box{(UINT)left, (UINT)top, 0, (UINT)(left + w), (UINT)(top + h), 1};
        ctx->CopySubresourceRegion(latest.Get(), 0, 0, 0, 0, src.Get(), 0, &box);
        fresh = true;
    }
};

ComPtr<ID3D11Texture2D> clear_texture(ID3D11Device* device, int w, int h) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)w, td.Height = (UINT)h, td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<uint32_t> zero((size_t)w * h, 0);
    D3D11_SUBRESOURCE_DATA data{zero.data(), (UINT)w * 4, 0};
    ComPtr<ID3D11Texture2D> t;
    device->CreateTexture2D(&td, &data, &t);
    return t;
}

void accent(float out[3]) {
    static const char* names[] = {"Lime", "Mint", "Aqua", "Sky", "Violet", "Pink", "Amber", "White"};
    static const int rgbs[] = {0xC6FF1A, 0x5CF2A0, 0x3DF2E0, 0x5CC8FF, 0xB49AFF, 0xFF7ADB, 0xFFB627, 0xF5F5F7};
    std::string t = read_file(appdata(L"theme.txt"));
    int rgb = rgbs[0];
    for (int i = 0; i < 8; ++i)
        if (t.find(names[i]) == 0) rgb = rgbs[i];
    out[0] = ((rgb >> 16) & 255) / 255.f, out[1] = ((rgb >> 8) & 255) / 255.f, out[2] = (rgb & 255) / 255.f;
}

// Plugin defaults (Bindings.cs): A Space, B LeftCtrl, X E, Y R, left grip LeftShift, right grip Q, left stick C,
// left trigger right mouse, right trigger left mouse, menu tap Esc, menu hold Tab.
PrxInputConfig input_config(const std::string& cfg) {
    PrxInputConfig c{};
    c.mode = 1;
    c.mouseSpeed = setting_f(cfg, "Controls", "MouseSpeed", 1200, 100, 6000);
    const int32_t binds[BindCount] = {0x39, 0x1D, 0x12, 0x13, 0x2A, 0x10, 0x2E, 0, BindMouse + 1, BindMouse, 0x01, 0x0F};
    std::memcpy(c.binds, binds, sizeof(binds));
    c.pointerClick = 1;
    return c;
}

// The OS cursor is how the game says it wants pointing: games aiming with the mouse hide it.
bool cursor_wanted() {
    CURSORINFO ci{sizeof(ci)};
    return GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING);
}
// --wake: starts the headset runtime and waits until it runs an app, then lets go. A game that opens VR itself (Godot's
// own OpenXR) right after a cold SteamVR start otherwise gets no GPU from it and falls back to flat.
int wake() {
    g_log = _wfsopen(appdata(L"wake.log").c_str(), L"w", _SH_DENYNO);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
    int32_t w = 0, h = 0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                                 nullptr, &ctx)) ||
        !prx_init("PortXR", 11, &w, &h)) {
        drain_core_log();
        log("no VR runtime to wake");
        return 1;
    }
    g_event = (RenderEvent)prx_render_event_func();
    auto tex = clear_texture(device.Get(), 64, 64);
    prx_set_init_texture(tex.Get());
    g_event(EvInit);
    ULONGLONG until = GetTickCount64() + 60000;
    while (prx_state() != StateRunning && prx_state() != StateFailed && GetTickCount64() < until) {
        g_event(EvBegin);
        Sleep(20);
    }
    bool ready = prx_state() == StateRunning;
    log("runtime %s", ready ? "ready" : "not ready");
    g_event(EvShutdown);
    drain_core_log();
    return ready ? 0 : 1;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && !wcscmp(argv[1], L"--wake")) return wake();
    double waitSeconds = 600;
    std::wstring captureDir;
    int captureFrame = 180;
    for (int i = 1; i + 1 < argc; ++i) {
        if (!wcscmp(argv[i], L"--dir")) g_dir = argv[++i];
        else if (!wcscmp(argv[i], L"--wait")) waitSeconds = _wtof(argv[++i]);
        else if (!wcscmp(argv[i], L"--capture")) captureDir = argv[++i];  // tests: the layers as BMP, once
        else if (!wcscmp(argv[i], L"--capture-frame")) captureFrame = _wtoi(argv[++i]);
    }
    while (!g_dir.empty() && (g_dir.back() == L'\\' || g_dir.back() == L'/')) g_dir.pop_back();
    if (g_dir.empty()) return 2;
    g_log = _wfsopen(appdata(L"theater.log").c_str(), L"w", _SH_DENYNO);
    log("PortXR Theater for %ls", g_dir.c_str());

    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\PortXRTheater");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        log("already running");
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    if (!wgc::GraphicsCaptureSession::IsSupported()) {
        log("this Windows cannot capture windows (Windows 10 1903 or later needed)");
        return 1;
    }

    HWND hwnd = nullptr;
    for (ULONGLONG until = GetTickCount64() + (ULONGLONG)(waitSeconds * 1000); !hwnd && GetTickCount64() < until; Sleep(500))
        hwnd = find_game_window();
    if (!hwnd) {
        log("no game window appeared");
        return 1;
    }
    wchar_t title[256] = L"";
    GetWindowTextW(hwnd, title, 256);
    log("game window: %ls", title);

    Capture cap;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &level, 1,
                                 D3D11_SDK_VERSION, &cap.device, nullptr, &cap.ctx))) {
        log("no Direct3D 11.1 device");
        return 1;
    }
    {
        ComPtr<IDXGIDevice> dxgi;
        cap.device.As(&dxgi);
        winrt::com_ptr<::IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()));
        cap.rtDevice = inspectable.as<wd3d::IDirect3DDevice>();
    }

    int32_t eyeW = 0, eyeH = 0;
    if (!prx_init("PortXR Theater", 11, &eyeW, &eyeH)) {
        drain_core_log();
        log("no VR runtime");
        return 1;
    }
    g_event = (RenderEvent)prx_render_event_func();
    // The eye layers stay transparent but for the laser beam the core draws into them, over the screen.
    auto eyes = clear_texture(cap.device.Get(), eyeW, eyeH);
    prx_set_init_texture(eyes.Get());
    g_event(EvInit);
    drain_core_log();
    cap.start(hwnd);
    prx_set_game_window(hwnd);

    std::string cfgText = read_file(appdata(L"defaults.cfg"));
    Screen screen;
    screen.distance = setting_f(cfgText, "View", "ScreenDistance", 3, 1, 20);
    screen.width = setting_f(cfgText, "View", "ScreenWidth", 3.2f, 0.5f, 30);
    bool room = setting(cfgText, "View", "Background") == "Passthrough";
    PrxInputConfig input = input_config(cfgText);
    float accentRgb[3];
    accent(accentRgb);
    ULONGLONG lostAt = 0, settingsAt = GetTickCount64();
    bool pointing = false, captured = false;
    int slot = 0, shown = 0;
    PrxStats stats{};
    prx_get_stats(&stats);
    bool curved = stats.curvedSupported != 0;
    log("screen %.1f m wide at %.1f m, %s", screen.width, screen.distance, curved ? "curved" : "flat");

    for (;;) {
        // The game may swap its window (launcher to game, a mode change): follow the newest; stop once none is left.
        if (!IsWindow(cap.hwnd) || !IsWindowVisible(cap.hwnd)) {
            if (!lostAt) lostAt = GetTickCount64();
            if (HWND next = find_game_window()) {
                GetWindowTextW(next, title, 256);
                log("game window now: %ls", title);
                cap.start(next);
                prx_set_game_window(next);
                lostAt = 0;
            } else if (GetTickCount64() - lostAt > 20000) {
                log("game closed");
                break;
            }
        } else {
            lostAt = 0;
        }
        if (GetTickCount64() - settingsAt > 3000) {
            settingsAt = GetTickCount64();
            accent(accentRgb);
        }

        int state = prx_state();
        if (state == StateFailed) {
            drain_core_log();
            log("VR runtime failed");
            break;
        }
        g_event(EvBegin);
        if (state != StateRunning) {
            drain_core_log();
            Sleep(20);
            continue;
        }
        cap.update();

        PrxPose eyePose[2];
        PrxFov eyeFov[2];
        bool tracked = prx_locate(eyePose, eyeFov) != 0;
        if (tracked && !screen.placed) place(screen, eyePose);

        PrxFrame f{};
        f.accent[0] = accentRgb[0], f.accent[1] = accentRgb[1], f.accent[2] = accentRgb[2];
        f.passthrough = room && prx_passthrough();
        f.uiKey = 2;
        PrxControllers c{};
        float uv[2] = {-1, -1}, point[3] = {};
        bool onScreen = false;
        // The pointer found on the previous frame moves the cursor and takes the right trigger as a click.
        input.pointerActive = pointing ? 1 : 0;
        uint32_t events = prx_input_update(&input, nullptr, &c);
        if (events & EvMenuToggle) {
            // Double tap on the left menu button: the screen comes back in front.
            if (tracked) place(screen, eyePose);
            prx_haptic_pulse(0, 0.3f, 0.05f);
        }
        if (screen.placed && c.aimValid[1]) onScreen = hit_screen(screen, curved, c.aim[1], uv, point);
        bool wanted = onScreen && cursor_wanted() && cap.width > 0;
        if (wanted && !pointing) prx_haptic_pulse(1, 0.25f, 0.02f);
        pointing = wanted;
        if (pointing) {
            RECT client;
            GetClientRect(cap.hwnd, &client);
            input.pointerX = (int)(uv[0] * client.right);
            input.pointerY = (int)(uv[1] * client.bottom);
        }

        if (cap.latest && screen.placed) {
            screen.height = screen.width * cap.height / std::max(1, cap.width);
            Quat q = yaw_quat(screen.yaw);
            f.quad = 1;
            f.quadLayer = {cap.latest.Get(), 0};
            f.quadWidth = screen.width;
            f.quadHeight = screen.height;
            f.quadRadius = curved ? screen.distance : 0;
            if (curved) {
                f.quadPose = {screen.center[0], screen.center[1], screen.center[2], q.x, q.y, q.z, q.w};
            } else {
                float ahead[3] = {0, 0, -screen.distance}, at[3];
                rotate(q, ahead, at);
                f.quadPose = {screen.center[0] + at[0], screen.center[1] + at[1], screen.center[2] + at[2], q.x, q.y, q.z, q.w};
            }
        }
        if (tracked) {
            f.stereo = 1;
            f.eye[0] = f.eye[1] = {eyes.Get(), 0};
            f.eyePose[0] = eyePose[0], f.eyePose[1] = eyePose[1];
            f.eyeFov[0] = eyeFov[0], f.eyeFov[1] = eyeFov[1];
            if (c.aimValid[1] && (pointing || onScreen)) {
                f.beam = 1;
                f.beamFrom[0] = c.aim[1].px, f.beamFrom[1] = c.aim[1].py, f.beamFrom[2] = c.aim[1].pz;
                for (int i = 0; i < 3; ++i) f.beamTo[i] = point[i];
            }
        }
        if (!captureDir.empty() && cap.fresh && !captured && ++shown == captureFrame) {
            prx_capture(captureDir.c_str());
            captured = true;
        }
        prx_set_frame(slot, &f);
        g_event(EvSubmit | (slot << 8));
        slot = (slot + 1) % 4;
        drain_core_log();
    }
    cap.stop();
    g_event(EvShutdown);
    drain_core_log();
    if (single) CloseHandle(single);
    return 0;
}
