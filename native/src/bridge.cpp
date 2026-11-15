#include "bridge.h"
#include "common.h"
#include "settings.h"
#include "rig.h"
#include <Xinput.h>

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
}

namespace {
enum Event { EvInit = 1, EvBegin = 2, EvSubmit = 3, EvShutdown = 4 };
void event(int id) { ((void(__stdcall*)(int))prx_render_event_func())(id); }
int g_slot;
PrxPose g_views[2];
PrxFov g_fovs[2];
bool g_located;
float g_accent[3];
bool g_accentRead;
float g_vignette;
rig::Result g_rig;
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
    if (settings::refresh() || !g_accentRead) settings::accent(g_accent), g_accentRead = true;
    g_located = prx_locate(g_views, g_fovs) != 0;
    if (!g_located) return false;
    rig::GameFrame g;
    g.hasCamera = game.hasCamera;
    g.camera = {{game.camera.p[0], game.camera.p[1], game.camera.p[2]}, {game.camera.q[0], game.camera.q[1], game.camera.q[2], game.camera.q[3]}};
    g.cameraKey = game.cameraKey;
    g.cursorFree = game.cursorFree;
    g.padGame = game.padGame;
    g.carried = game.carried;
    g.menuOpen = game.menuOpen;
    g.uiAspect = game.uiAspect;
    g.pointerAuto = settings::get("Controls", "Pointer", "Auto") != "MenuOnly";
    rig::Result r = rig::frame(g, g_views);
    g_rig = r;
    for (int e = 0; e < 2; ++e) {
        out.eyes[e] = {{r.eyes[e].p.x, r.eyes[e].p.y, r.eyes[e].p.z}, {r.eyes[e].q.x, r.eyes[e].q.y, r.eyes[e].q.z, r.eyes[e].q.w}};
        out.fovs[e] = {g_fovs[e].left, g_fovs[e].right, g_fovs[e].up, g_fovs[e].down};
    }
    out.aiming = r.aiming;
    out.events = r.events;
    out.pointer = r.pointer.active;
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

void test_point(float u, float v) { rig::set_test_point(u, v); }

void set_window(void* hwnd) { prx_set_game_window((HWND)hwnd); }

bool pad(Pad& out) {
    XINPUT_GAMEPAD gp{};
    if (!prx_pad_state(&gp)) return false;
    out = {gp.wButtons, gp.bLeftTrigger, gp.bRightTrigger, gp.sThumbLX, gp.sThumbLY, gp.sThumbRX, gp.sThumbRY};
    return true;
}

void submit_view(void* left, void* right, void* ui, float uiAspect, bool flipY) {
    if (!g_located) return;
    PrxFrame f{};
    f.stereo = 1;
    f.eye[0] = {left, flipY ? 1 : 0};
    f.eye[1] = {right, flipY ? 1 : 0};
    for (int e = 0; e < 2; ++e) {
        f.eyePose[e] = g_views[e];
        f.eyeFov[e] = g_fovs[e];
    }
    if (ui) {
        float width = 1;
        rig::Pose at = rig::panel(width);
        f.quad = 1;
        f.quadAlpha = 1;
        f.uiKey = 2;  // the engine gives the UI with its real alpha: nothing to key
        f.quadLayer = {ui, flipY ? 1 : 0};
        f.quadPose = {at.p.x, at.p.y, at.p.z, at.q.x, at.q.y, at.q.z, at.q.w};
        f.quadWidth = width;
        f.quadHeight = width / (uiAspect > 0 ? uiAspect : 16.f / 9.f);
    }
    for (int i = 0; i < 3; ++i) f.accent[i] = g_accent[i];
    f.vignette = g_vignette;
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
    prx_set_frame(g_slot, &f);
    event(EvSubmit | (g_slot << 8));
    g_slot = (g_slot + 1) % 4;
}
}  // namespace bridge
