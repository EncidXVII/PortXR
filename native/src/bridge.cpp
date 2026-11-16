#include "bridge.h"
#include "common.h"
#include "settings.h"
#include "rig.h"
#include "menu.h"
#include "canvas.h"
#include "script.h"
#include <Xinput.h>
#include <algorithm>

extern "C" {
PRX_API int32_t prx_init(const char* appName, int32_t api, int32_t* recommendedWidth, int32_t* recommendedHeight);
PRX_API int32_t prx_state();
PRX_API void* prx_render_event_func();
PRX_API void prx_set_init_texture(void* texture);
PRX_API void prx_set_frame(int32_t slot, const PrxFrame* frame);
PRX_API int32_t prx_locate(PrxPose* poses, PrxFov* fovs);
PRX_API int32_t prx_poll_log(char* out, int32_t capacity);
PRX_API void prx_set_game_window(HWND hwnd);
PRX_API int32_t prx_pad_state(XINPUT_GAMEPAD* out);
PRX_API int32_t prx_key_down(int32_t vk);
PRX_API void prx_capture(const wchar_t* dir);
PRX_API void prx_set_menu(const uint8_t* rgba, int32_t w, int32_t h, const PrxPose* pose, float widthM, float heightM);
PRX_API void prx_set_overlay(int32_t index, const uint8_t* rgba, int32_t w, int32_t h, const PrxPose* pose, float widthM,
                             float heightM, float radius);
PRX_API int32_t prx_passthrough();
PRX_API void prx_get_stats(PrxStats* out);
}

namespace {
enum Event { EvInit = 1, EvBegin = 2, EvSubmit = 3, EvShutdown = 4 };
void event(int id) { ((void(__stdcall*)(int))prx_render_event_func())(id); }
int g_slot;
PrxPose g_views[2];
PrxFov g_fovs[2];
bool g_located;
float g_accent[3];
ULONGLONG g_accentAt;
float g_vignette;
rig::Result g_rig;
LARGE_INTEGER g_menuTick{};
canvas::Canvas g_handlesCanvas;
int g_handlesDrawn = -1;  // what the handles canvas shows: handle, grab and size, packed

PrxPose prx_pose(const rig::Pose& p) { return {p.p.x, p.p.y, p.p.z, p.q.x, p.q.y, p.q.z, p.q.w}; }

// The plugin's DrawHandles: faint rounded corner brackets (lime while resizing), a pill bar under the middle, a quiet
// RESET beside it, and while dragging the distance and width with the stick's controls above the bar.
void draw_handles(const rig::Result& r, float aspect) {
    int ch = 720, cw = (int)(ch * aspect);
    g_handlesCanvas.resize(cw, ch);
    float W = (float)cw, H = (float)ch, t = std::max(3.f, H / 160), corner = H * 0.12f, arm = corner * 0.6f, inset = H * 0.02f;
    canvas::Color faint{0.961f, 0.961f, 0.969f, 0.4f}, shade{0.043f, 0.043f, 0.047f, 0.55f};
    float accentRgb[3];
    settings::accent(accentRgb);
    canvas::Color lime{accentRgb[0], accentRgb[1], accentRgb[2], 1};
    bool sizing = r.handle == 2 || r.grab == 2;
    float k = sizing ? 2.f : 1.f;
    for (float x : {inset, W - inset})
        for (float y : {inset, H - inset}) {
            canvas::Color c = sizing ? lime : faint;
            g_handlesCanvas.round({x < W / 2 ? x : x - arm, y < H / 2 ? y : y - t * k, arm, t * k}, c, t * k / 2);
            g_handlesCanvas.round({x < W / 2 ? x : x - t * k, y < H / 2 ? y : y - arm, t * k, arm}, c, t * k / 2);
        }
    canvas::Rect bar{W / 2 - H * 0.16f, H * 0.935f, H * 0.32f, H * 0.065f};
    g_handlesCanvas.round(bar, shade, bar.h / 2);
    g_handlesCanvas.round({bar.x + bar.w * 0.2f, bar.cy() - t, bar.w * 0.6f, t * 2}, r.handle == 1 || r.grab == 1 ? lime : faint, t);
    canvas::Rect reset{bar.x + bar.w + H * 0.02f, bar.y, H * 0.16f, bar.h};
    g_handlesCanvas.round(reset, r.handle == 3 ? lime : shade, reset.h * 0.3f);
    g_handlesCanvas.text(reset, "RESET", canvas::Bold, H / 40, r.handle == 3 ? canvas::rgb(0x0B0B0C) : faint, canvas::Center);
    if (!r.grab) return;
    canvas::Rect tip{W / 2 - H * 0.5f, bar.y - H * 0.1f, H, H * 0.08f};
    g_handlesCanvas.round(tip, {0.043f, 0.043f, 0.047f, 0.85f}, tip.h * 0.25f);
    char line[96];
    snprintf(line, sizeof(line), "%.1f m away, %.1f m wide", r.screenDistance, r.screenWidth);
    g_handlesCanvas.text({tip.x, tip.y, tip.w, tip.h / 2}, line, canvas::Body, H / 40, {1, 1, 1, 1}, canvas::Center);
    g_handlesCanvas.text({tip.x, tip.cy(), tip.w, tip.h / 2}, "Right stick: up/down distance, left/right size", canvas::Body, H / 40,
                         {1, 1, 1, 1}, canvas::Center);
}
bool g_menuKeyDown, g_welcomed;
bool g_testHead;
float g_testHeadAt[4];
int g_lastMode;
rig::Screen g_lastScreen;
float g_aspect = 16.f / 9.f;
// The crosshair the core draws (Game::crosshair): while aiming, the game camera and how far it aims.
bool g_aimCross;
rig::Pose g_aimCam;
float g_aimDist;
PrxPose pose(const bridge::Pose& p) { return {p.p[0], p.p[1], p.p[2], p.q[0], p.q[1], p.q[2], p.q[3]}; }
void submit(PrxFrame& f) {
    f.accent[0] = 0.776f, f.accent[1] = 1.f, f.accent[2] = 0.102f;
    prx_set_frame(g_slot, &f);
    event(EvSubmit | (g_slot << 8));
    g_slot = (g_slot + 1) % 4;
}
}  // namespace

namespace bridge {
bool start(const char* app, int& eyeWidth, int& eyeHeight) {
    int32_t w = 0, h = 0;
    bool ok = prx_init(app, 12, &w, &h) != 0;
    eyeWidth = w, eyeHeight = h;
    return ok;
}

void attach(void* texture) {
    prx_set_init_texture(texture);
    event(EvInit);
}

int state() { return prx_state(); }

void begin() { event(EvBegin); }

bool locate(Pose eyes[2], Fov fovs[2]) {
    PrxPose p[2];
    PrxFov f[2];
    if (!prx_locate(p, f)) return false;
    for (int e = 0; e < 2; ++e) {
        eyes[e] = {{p[e].px, p[e].py, p[e].pz}, {p[e].qx, p[e].qy, p[e].qz, p[e].qw}};
        fovs[e] = {f[e].left, f[e].right, f[e].up, f[e].down};
    }
    return true;
}

void submit_quad(void* texture, const Pose& at, float width, float height, bool flipY) {
    PrxFrame f{};
    f.quad = 1;
    f.quadLayer = {texture, flipY ? 1 : 0};
    f.quadPose = pose(at);
    f.quadWidth = width;
    f.quadHeight = height;
    submit(f);
}

void submit_eyes(void* left, void* right, const Pose eyes[2], const Fov fovs[2], bool flipY) {
    PrxFrame f{};
    f.stereo = 1;
    f.eye[0] = {left, flipY ? 1 : 0};
    f.eye[1] = {right, flipY ? 1 : 0};
    for (int e = 0; e < 2; ++e) {
        f.eyePose[e] = pose(eyes[e]);
        f.eyeFov[e] = {fovs[e].left, fovs[e].right, fovs[e].up, fovs[e].down};
    }
    submit(f);
}

std::string poll_log() {
    std::string out;
    char line[1024];
    while (prx_poll_log(line, sizeof(line)) > 0) out += std::string(line) + "\n";
    return out;
}

void shutdown() { event(EvShutdown); }

void settings_open(const std::wstring& gameFile) { settings::open(gameFile, L""); }
std::string setting(const char* section, const char* key, const char* fallback) { return settings::get(section, key, fallback); }
void set_setting(const char* section, const char* key, const std::string& value) { settings::set(section, key, value); }
void recenter() { rig::recenter(); }

bool frame(const Game& game, Frame& out) {
    // The theme (one file for every game and the app) is looked at once a second; a settings change shows at once.
    ULONGLONG nowMs = GetTickCount64();
    if (settings::refresh() || nowMs - g_accentAt >= 1000) settings::accent(g_accent), g_accentAt = nowMs;
    script::tick(game.scene, game.cameraName);
    g_located = prx_locate(g_views, g_fovs) != 0;
    if (!g_located) return false;
    if (g_testHead) {
        // The head where the test puts it: the eyes keep their places around it, turned to its heading.
        mathx::V3 center{(g_views[0].px + g_views[1].px) / 2, (g_views[0].py + g_views[1].py) / 2, (g_views[0].pz + g_views[1].pz) / 2};
        mathx::Q undo = mathx::Q{g_views[0].qx, g_views[0].qy, g_views[0].qz, g_views[0].qw}.inverse();
        mathx::Q turn = mathx::yaw(-g_testHeadAt[3] * mathx::kPi / 180);
        mathx::V3 head{g_testHeadAt[0], g_testHeadAt[1], g_testHeadAt[2]};
        for (auto& v : g_views) {
            mathx::V3 p = head + turn * (undo * (mathx::V3{v.px, v.py, v.pz} - center));
            mathx::Q q = turn * undo * mathx::Q{v.qx, v.qy, v.qz, v.qw};
            v = {p.x, p.y, p.z, q.x, q.y, q.z, q.w};
        }
    }
    rig::GameFrame g;
    g.hasCamera = game.hasCamera;
    g.noTheater = game.noTheater;
    g.camera = {{game.camera.p[0], game.camera.p[1], game.camera.p[2]}, {game.camera.q[0], game.camera.q[1], game.camera.q[2], game.camera.q[3]}};
    g.cameraKey = game.cameraKey;
    g.cursorFree = game.cursorFree;
    g.padGame = game.padGame;
    g.carried = game.carried;
    g.menuOpen = menu::is_open();
    g.uiAspect = game.uiAspect;
    g.fovY = game.fovY;
    g.nearClip = game.nearClip;
    g.focus = game.focus;
    g.screenUnlocked = menu::screen_unlocked();
    // The menu stands where the game's picture is (the UI panel, or the middle of the Theater and Parallax screen), a
    // little in front, as tall as Menu.cs draws it on the screen.
    float aspect = game.uiAspect > 0 ? game.uiAspect : 16.f / 9.f;
    float pw = 1;
    rig::Pose at = rig::panel(pw);
    float ph = pw / aspect;
    if (g_lastMode != 0) {
        at = g_lastScreen.center;
        if (g_lastScreen.radius > 0) at.p = g_lastScreen.pose.p + g_lastScreen.pose.q * mathx::V3{0, 0, -g_lastScreen.radius};
        ph = g_lastScreen.height;
    }
    g.menuHeight = ph * 840.f / 1080.f;
    g.menuWidth = g.menuHeight * menu::aspect();
    g.menuPose = {at.p + at.q * mathx::V3{0, 0, g_lastMode != 0 ? 0.05f : 0.02f}, at.q};
    g.pointerAuto = settings::get("Controls", "Pointer", "Auto") != "MenuOnly";
    g.up = {game.up[0], game.up[1], game.up[2]};
    g.sceneScale = game.sceneScale;
    // The engine's nudge (Unity's scripts), and the core's scripts' (player frame: z forward, tracking -Z).
    g.eyeOffset = {game.eyeOffset[0] + script::eyeOffset[0], game.eyeOffset[1] + script::eyeOffset[1], game.eyeOffset[2] - script::eyeOffset[2]};
    g.worldPointer = game.worldPointer;
    g.worldReach = game.worldReach;
    g.screenW = game.screenW, g.screenH = game.screenH;
    g.pointerClick = game.pointerClick;
    rig::Result r = rig::frame(g, g_views);
    g_rig = r;
    g_aimCross = game.crosshair && game.hasCamera && r.aiming;
    g_aimCam = g.camera;
    g_aimDist = game.aimDistance;
    g_lastMode = r.mode;
    g_lastScreen = r.screen;
    g_aspect = aspect;
    out.mode = r.mode;
    out.handles = r.handles;
    for (int e = 0; e < 2; ++e) {
        out.windowOffset[e] = r.windowOffset[e];
        out.windowFov[e] = {r.windowFov[e].left, r.windowFov[e].right, r.windowFov[e].up, r.windowFov[e].down};
    }
    // The screen's handles, redrawn when what they show changes.
    if (r.handles && r.mode != 0) {
        int state = r.handle | (r.grab << 4) | ((int)(r.screenDistance * 10) << 8) | ((int)(r.screenWidth * 10) << 16);
        bool changed = state != g_handlesDrawn;
        if (changed) draw_handles(r, aspect), g_handlesDrawn = state;
        PrxPose pose = prx_pose(r.screen.radius > 0 ? r.screen.pose : r.screen.center);
        // A hair in front of the screen; the curved one a little inside its cylinder.
        if (r.screen.radius <= 0) {
            mathx::V3 n = r.screen.center.q * mathx::V3{0, 0, 0.01f};
            pose.px += n.x, pose.py += n.y, pose.pz += n.z;
        }
        prx_set_overlay(1, changed ? g_handlesCanvas.pixels() : nullptr, g_handlesCanvas.width(), g_handlesCanvas.height(), &pose,
                        r.screen.width, r.screen.height, r.screen.radius > 0 ? r.screen.radius - 0.01f : 0);
    } else {
        g_handlesDrawn = -1;
        prx_set_overlay(1, nullptr, 0, 0, nullptr, 0, 0, 0);
    }
    for (int e = 0; e < 2; ++e) {
        out.eyes[e] = {{r.eyes[e].p.x, r.eyes[e].p.y, r.eyes[e].p.z}, {r.eyes[e].q.x, r.eyes[e].q.y, r.eyes[e].q.z, r.eyes[e].q.w}};
        out.fovs[e] = {g_fovs[e].left, g_fovs[e].right, g_fovs[e].up, g_fovs[e].down};
        out.views[e] = {{g_views[e].px, g_views[e].py, g_views[e].pz}, {g_views[e].qx, g_views[e].qy, g_views[e].qz, g_views[e].qw}};
    }
    out.aiming = r.aiming;
    out.events = r.events;
    out.rightValid = r.controllers.aimValid[1] != 0;
    if (out.rightValid) {
        // Tracking space to the game world, as the eyes were placed (rig::Result::origin).
        const PrxPose& a = r.controllers.aim[1];
        mathx::Q undo = r.origin.q.inverse();
        mathx::V3 w = r.base + r.body * (undo * (mathx::V3{a.px, a.py, a.pz} - r.origin.p) * r.scale);
        mathx::Q q = r.body * undo * mathx::Q{a.qx, a.qy, a.qz, a.qw};
        out.right = {{w.x, w.y, w.z}, {q.x, q.y, q.z, q.w}};
    }
    out.handAiming = r.handAiming;
    out.rightTrigger = r.controllers.rTrigger;
    out.bDown = (r.controllers.buttons & BtnB) != 0;
    out.scale = r.scale;
    // The VR menu: a double tap on the menu button or the watch (from the rig), or F11 with the game in front.
    LARGE_INTEGER tick, freq;
    QueryPerformanceCounter(&tick);
    QueryPerformanceFrequency(&freq);
    float dt = g_menuTick.QuadPart ? std::min(0.1f, (float)(tick.QuadPart - g_menuTick.QuadPart) / freq.QuadPart) : 1 / 90.f;
    g_menuTick = tick;
    bool key = prx_key_down((int)settings::getf("Controls", "MenuKey", 0x7A, 1, 255)) != 0;
    if ((r.events & EvMenuToggle) || (key && !g_menuKeyDown)) menu::toggle();
    g_menuKeyDown = key;
    // The welcome card the first time VR runs in a game.
    if (!g_welcomed && !game.tests && settings::getb("Menu", "FirstRun", true)) menu::show_welcome();
    g_welcomed = true;
    menu::Requests req = menu::frame(r.pointer.onMenu, r.pointer.u, r.pointer.v, r.controllers.rTrigger, dt);
    if (req.recenter) rig::recenter();
    if (menu::is_open()) {
        int w = 0, h = 0;
        bool changed = false;
        const uint8_t* px = menu::pixels(w, h, changed);
        PrxPose pose{g.menuPose.p.x, g.menuPose.p.y, g.menuPose.p.z, g.menuPose.q.x, g.menuPose.q.y, g.menuPose.q.z, g.menuPose.q.w};
        prx_set_menu(changed ? px : nullptr, w, h, &pose, g.menuWidth, g.menuHeight);
    } else {
        prx_set_menu(nullptr, 0, 0, nullptr, 0, 0);
    }
    out.menuOpen = menu::is_open();
    out.quit = req.quit;
    out.renderScale = req.renderScale;
    out.cameraPicked = req.cameraPicked;
    out.camera = req.camera;
    out.hideObjects = req.hideObjects;
    out.showHidden = req.showHidden;
    out.pointer = r.pointer.active && !r.pointer.onMenu;
    out.pointerDown = r.pointer.down;
    out.pointerU = r.pointer.u;
    out.pointerV = r.pointer.v;
    g_vignette = r.vignette;
    return true;
}

void test_controllers(bool on, float lx, float ly, float rx, float ry, float lt, float rt, uint32_t buttons) {
    if (!on) return rig::set_test_controllers(nullptr);
    PrxControllers c{};
    c.lx = lx, c.ly = ly, c.rx = rx, c.ry = ry, c.lTrigger = lt, c.rTrigger = rt, c.buttons = buttons;
    rig::set_test_controllers(&c);
}

void test_point(float u, float v, bool screen) { rig::set_test_point(u, v, screen); }

void toggle_menu() { menu::toggle(); }
void capture(const std::wstring& dir) { prx_capture(dir.c_str()); }

void set_menu_host(const MenuHost& h) {
    menu::Host m;
    m.product = h.product;
    m.gameDir = h.gameDir;
    m.cameras = h.cameras;
    m.currentCamera = h.currentCamera;
    m.hideObjects = h.hideObjects;
    m.aimExtras = h.aimExtras;
    m.handHeld = h.handHeld;
    m.uiBlack = h.uiBlack;
    m.noTheater = h.noTheater;
    m.hidden = h.hidden;
    m.eyeWidth = h.eyeWidth;
    m.eyeHeight = h.eyeHeight;
    menu::set_host(m);
}

void set_window(void* hwnd) { prx_set_game_window((HWND)hwnd); }

bool pad(Pad& out) {
    XINPUT_GAMEPAD gp{};
    if (!prx_pad_state(&gp)) return false;
    out = {gp.wButtons, gp.bLeftTrigger, gp.bRightTrigger, gp.sThumbLX, gp.sThumbLY, gp.sThumbRX, gp.sThumbRY};
    return true;
}

// While aiming, where the game camera aims (what it hits, else 40 m), seen from each eye: Parallax's reticle there, or
// the UI panel's middle (the game's own crosshair) moved there, as the Unity plugin does.
void crosshair(PrxFrame& f, bool game) {
    using mathx::V3;
    const rig::Result& r = g_rig;
    float dist = g_aimDist > 0 ? std::max(g_aimDist, 0.3f) : 40.f * r.scale;
    V3 world = g_aimCam.p + g_aimCam.q * V3{0, 0, -dist};
    V3 at = r.origin.p + r.origin.q * (r.body.inverse() * (world - r.base) * (1 / std::max(1e-4f, r.scale)));
    if (game) {
        V3 head = (V3{g_views[0].px, g_views[0].py, g_views[0].pz} + V3{g_views[1].px, g_views[1].py, g_views[1].pz}) * 0.5f;
        V3 to = at - head;
        float d = std::max(1e-4f, to.length());
        to = to * (1 / d);
        // Same size to the eye as on the panel, facing the head, level.
        float grow = d / std::max(0.1f, (V3{f.quadPose.px, f.quadPose.py, f.quadPose.pz} - head).length());
        float pitch = std::asin(std::clamp(to.y, -1.f, 1.f)) / 2;
        mathx::Q q = mathx::yaw(std::atan2(-to.x, -to.z)) * mathx::Q{std::sin(pitch), 0, 0, std::cos(pitch)};
        // Half the middle of the UI that moves with it; a script's crosshair_area widens it.
        f.aimPatch[0] = script::crosshairArea[0] > 0 ? script::crosshairArea[0] / 2 : 0.09f;
        f.aimPatch[1] = script::crosshairArea[1] > 0 ? script::crosshairArea[1] / 2 : 0.08f;
        f.aimPatchShown = 1;
        f.aimPose = {at.x, at.y, at.z, q.x, q.y, q.z, q.w};
        f.aimSize[0] = 2 * f.aimPatch[0] * f.quadWidth * grow;
        f.aimSize[1] = 2 * f.aimPatch[1] * f.quadHeight * grow;
        return;
    }
    for (int e = 0; e < 2; ++e) {
        const PrxPose& v = g_views[e];
        V3 p = mathx::Q{v.qx, v.qy, v.qz, v.qw}.inverse() * (at - V3{v.px, v.py, v.pz});
        if (p.z > -0.01f) continue;
        float l = std::tan(g_fovs[e].left), rt = std::tan(g_fovs[e].right), u = std::tan(g_fovs[e].up), dn = std::tan(g_fovs[e].down);
        f.aimReticle[e][0] = (p.x / -p.z - l) / (rt - l);
        f.aimReticle[e][1] = (u - p.y / -p.z) / (u - dn);
        f.aimReticle[e][2] = std::tan(0.45f * mathx::kPi / 180) / (u - dn);
    }
}

void layers(void* prxFrame, float uiAspect) {
    PrxFrame& f = *(PrxFrame*)prxFrame;
    int mode = g_rig.mode;
    for (int e = 0; e < 2; ++e) {
        f.eyePose[e] = g_views[e];
        f.eyeFov[e] = g_fovs[e];
    }
    f.passthrough = mode != rig::Stereo && settings::get("View", "Background", "Dark") == "Passthrough" && prx_passthrough() ? 1 : 0;
    if (mode != rig::Stereo) {
        // The game's picture (Theater) or its UI over the window (Parallax) on the screen, curved where it fits.
        const rig::Screen& sc = g_rig.screen;
        f.quadPose = prx_pose(mode == rig::Parallax ? sc.center : (sc.radius > 0 ? sc.pose : sc.center));
        f.quadRadius = mode == rig::Theater ? sc.radius : 0;
        f.quadWidth = sc.width;
        f.quadHeight = sc.height;
    } else {
        float width = 1;
        rig::Pose at = rig::panel(width);
        f.quadPose = prx_pose(at);
        f.quadRadius = 0;
        f.quadWidth = width;
        f.quadHeight = width / (uiAspect > 0 ? uiAspect : 16.f / 9.f);
    }
    for (int i = 0; i < 3; ++i) f.accent[i] = g_accent[i];
    f.vignette = g_vignette;
    std::string cross = settings::get("Controls", "Crosshair", "Game");
    if (mode == rig::Stereo && g_aimCross && cross != "Off") crosshair(f, cross == "Game");
    if (g_rig.beam) {
        f.beam = 1;
        f.beamFrom[0] = g_rig.beamFrom.x, f.beamFrom[1] = g_rig.beamFrom.y, f.beamFrom[2] = g_rig.beamFrom.z;
        f.beamTo[0] = g_rig.beamTo.x, f.beamTo[1] = g_rig.beamTo.y, f.beamTo[2] = g_rig.beamTo.z;
    }
    if (g_rig.watch.shown) {
        f.watch = 1;
        f.watchLit = g_rig.watch.lit;
        f.watchAlpha = g_rig.watch.alpha;
        f.watchAt[0] = g_rig.watch.at.x, f.watchAt[1] = g_rig.watch.at.y, f.watchAt[2] = g_rig.watch.at.z;
        f.watchRadius = 0.022f;
    }
}

// Auto resolution, as Rig.cs: every 4 s (a fresh 256-frame timing window) two slow windows in a row take the eyes down
// 10%; a drop that doesn't help means the game is CPU-bound, so it is undone and scaling stops; headroom raises it again.
// The first 10 s of a session (shader compiles, streaming) don't count.
namespace {
int g_autoTenths = 10, g_slowChecks, g_calmChecks;
bool g_cpuBound, g_justDropped;
double g_p95BeforeDrop;
ULONGLONG g_nextScaleCheck;
}  // namespace

float render_scale() {
    float scale = settings::getf("View", "RenderScale", 1, 0.5f, 2);
    if (!settings::getb("View", "AutoRenderScale", true)) return scale;
    PrxStats s{};
    prx_get_stats(&s);
    ULONGLONG now = GetTickCount64();
    if (s.state != StateRunning) {
        g_nextScaleCheck = now + 10000;
    } else if (!g_cpuBound && now >= g_nextScaleCheck) {
        g_nextScaleCheck = now + 4000;
        if (g_justDropped) {
            g_justDropped = false;
            if (s.p95Ms > g_p95BeforeDrop * 0.95) {
                g_cpuBound = true;
                g_autoTenths = std::min(10, g_autoTenths + 1);
                prx_log("Auto resolution: a lower one did not help (the game is CPU-bound); back to %d0%%", g_autoTenths);
                return scale * g_autoTenths / 10;
            }
        }
        g_slowChecks = s.p95Ms > s.periodMs * 1.2 ? g_slowChecks + 1 : 0;
        if (g_slowChecks >= 2 && g_autoTenths > 5) {
            g_slowChecks = g_calmChecks = 0;
            g_p95BeforeDrop = s.p95Ms;
            g_justDropped = true;
            prx_log("Auto resolution: %d0%% (frames take %.1f ms of %.1f)", --g_autoTenths, s.p95Ms, s.periodMs);
        } else if (s.p95Ms < s.periodMs * 1.05 && g_autoTenths < 10 && ++g_calmChecks >= 3) {
            g_calmChecks = 0;
            prx_log("Auto resolution: %d0%%", ++g_autoTenths);
        }
    }
    return scale * g_autoTenths / 10;
}

void test_head(bool on, float x, float y, float z, float yawDeg) {
    g_testHead = on;
    g_testHeadAt[0] = x, g_testHeadAt[1] = y, g_testHeadAt[2] = z, g_testHeadAt[3] = yawDeg;
}

void submit_view(void* left, void* right, void* ui, float uiAspect, bool flipY) {
    if (!g_located) return;
    PrxFrame f{};
    int mode = g_rig.mode;
    layers(&f, uiAspect);
    // Stereo: the eyes. Theater and Parallax: transparent eyes of the core's own, for the beam and the watch over the
    // screen; Parallax adds the window eyes on the screen.
    f.stereo = mode == rig::Stereo || g_rig.beam || g_rig.watch.shown || mode == rig::Parallax ? 1 : 0;
    if (mode == rig::Stereo) {
        f.eye[0] = {left, flipY ? 1 : 0};
        f.eye[1] = {right, flipY ? 1 : 0};
    }
    if (mode == rig::Parallax && left && right) {
        f.window = 1;
        f.windowLayer[0] = {left, flipY ? 1 : 0};
        f.windowLayer[1] = {right, flipY ? 1 : 0};
    }
    if (ui) {
        f.quad = 1;
        f.quadAlpha = mode != rig::Theater ? 1 : 0;
        f.uiKey = 2;  // the engine gives the UI with its real alpha: nothing to key
        f.quadLayer = {ui, flipY ? 1 : 0};
    }
    prx_set_frame(g_slot, &f);
    event(EvSubmit | (g_slot << 8));
    g_slot = (g_slot + 1) % 4;
}
}  // namespace bridge

// --- The bridge for the Unity plugin (C#): the same frame as every engine, flat structs at the edge ---------------
// Poses in OpenXR handedness (the plugin mirrors Z at its side).
struct PrxGame {
    int32_t hasCamera;
    PrxPose camera;
    const char* cameraKey;
    int32_t cursorFree, padGame, carried, tests;
    float uiAspect, fovY, nearClip, focus;
    float up[3], sceneScale, eyeOffset[3];
    int32_t worldPointer;
    float worldReach;
    int32_t screenW, screenH, pointerClick;
};
struct PrxRig {
    PrxPose eyes[2], views[2];
    PrxFov fovs[2];
    int32_t mode, aiming, events, menuOpen;
    int32_t pointer, pointerDown;
    float pointerU, pointerV;
    int32_t quit, renderScale, cameraPicked, hideObjects, showHidden;
    float windowOffset[2];
    PrxFov windowFov[2];
    int32_t handAiming;
    PrxPose handAim;
    PrxPose origin;  // tracking to world: base + body * (origin.q^-1 * (p - origin.p) * scale)
    float body[4], base[3], scale;
    PrxControllers controllers;
    int32_t handles, handle, grab;
    float screen[4];  // yaw, pitch (degrees), distance, width (metres)
    float vignette, focus, bodyYaw, orbitRadius;
    int32_t smoothing, glancing, watch, beam;
};

extern "C" {
PRX_API int32_t prx_bridge_frame(const PrxGame* in, PrxRig* out) {
    bridge::Game g;
    g.hasCamera = in->hasCamera != 0;
    g.camera = {{in->camera.px, in->camera.py, in->camera.pz}, {in->camera.qx, in->camera.qy, in->camera.qz, in->camera.qw}};
    g.cameraKey = in->cameraKey ? in->cameraKey : "";
    g.cursorFree = in->cursorFree != 0, g.padGame = in->padGame != 0, g.carried = in->carried != 0, g.tests = in->tests != 0;
    g.uiAspect = in->uiAspect, g.fovY = in->fovY, g.nearClip = in->nearClip, g.focus = in->focus;
    for (int i = 0; i < 3; ++i) g.up[i] = in->up[i], g.eyeOffset[i] = in->eyeOffset[i];
    g.sceneScale = in->sceneScale;
    g.worldPointer = in->worldPointer != 0, g.worldReach = in->worldReach;
    g.screenW = in->screenW, g.screenH = in->screenH, g.pointerClick = in->pointerClick != 0;
    bridge::Frame f;
    *out = {};
    if (!bridge::frame(g, f)) return 0;
    const rig::Result& r = g_rig;
    for (int e = 0; e < 2; ++e) {
        out->eyes[e] = {f.eyes[e].p[0], f.eyes[e].p[1], f.eyes[e].p[2], f.eyes[e].q[0], f.eyes[e].q[1], f.eyes[e].q[2], f.eyes[e].q[3]};
        out->views[e] = g_views[e];
        out->fovs[e] = g_fovs[e];
        out->windowOffset[e] = r.windowOffset[e];
        out->windowFov[e] = r.windowFov[e];
    }
    out->mode = f.mode, out->aiming = f.aiming, out->events = f.events, out->menuOpen = f.menuOpen;
    out->pointer = f.pointer, out->pointerDown = f.pointerDown, out->pointerU = f.pointerU, out->pointerV = f.pointerV;
    out->quit = f.quit, out->renderScale = f.renderScale, out->cameraPicked = f.cameraPicked;
    out->hideObjects = f.hideObjects, out->showHidden = f.showHidden;
    out->handAiming = r.handAiming, out->handAim = r.handAim;
    out->origin = prx_pose(r.origin);
    out->body[0] = r.body.x, out->body[1] = r.body.y, out->body[2] = r.body.z, out->body[3] = r.body.w;
    out->base[0] = r.base.x, out->base[1] = r.base.y, out->base[2] = r.base.z;
    out->scale = r.scale;
    out->controllers = r.controllers;
    out->handles = r.handles, out->handle = r.handle, out->grab = r.grab;
    out->screen[0] = r.screenYaw, out->screen[1] = r.screenPitch, out->screen[2] = r.screenDistance, out->screen[3] = r.screenWidth;
    out->vignette = r.vignette, out->focus = r.focus, out->bodyYaw = r.bodyYaw, out->orbitRadius = r.orbitRadius;
    out->smoothing = r.stepSmoothing, out->glancing = r.glancing, out->watch = r.watch.shown, out->beam = r.beam;
    return 1;
}

PRX_API void prx_bridge_layers(PrxFrame* f, float uiAspect) { bridge::layers(f, uiAspect); }
PRX_API void prx_bridge_recenter() { rig::recenter(); }

// Tests: controllers instead of the headset's (null: the real ones), the laser forced onto the UI at (u, v) (u < 0:
// not), the raised wrist counted as looked at, the head moved (x, y, z, clockwise yaw degrees; null: off).
PRX_API void prx_bridge_test(const PrxControllers* c, float u, float v, int32_t wrist, const float* head) {
    rig::set_test_controllers(c);
    rig::set_test_point(u, v, false);
    rig::set_test_wrist(wrist != 0);
    bridge::test_head(head != nullptr, head ? head[0] : 0, head ? head[1] : 0, head ? head[2] : 0, head ? head[3] : 0);
}
}
