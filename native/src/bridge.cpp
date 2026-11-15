#include "bridge.h"
#include "common.h"

extern "C" {
PRX_API int32_t prx_init(const char* appName, int32_t api, int32_t* recommendedWidth, int32_t* recommendedHeight);
PRX_API int32_t prx_state();
PRX_API void* prx_render_event_func();
PRX_API void prx_set_init_texture(void* texture);
PRX_API void prx_set_frame(int32_t slot, const PrxFrame* frame);
PRX_API int32_t prx_locate(PrxPose* poses, PrxFov* fovs);
PRX_API int32_t prx_poll_log(char* out, int32_t capacity);
}

namespace {
enum Event { EvInit = 1, EvBegin = 2, EvSubmit = 3, EvShutdown = 4 };
void event(int id) { ((void(__stdcall*)(int))prx_render_event_func())(id); }
int g_slot;
PrxPose pose(const bridge::Pose& p) { return {p.p[0], p.p[1], p.p[2], p.q[0], p.q[1], p.q[2], p.q[3]}; }
void submit(PrxFrame& f) {
    f.accent[0] = 0.776f, f.accent[1] = 1.f, f.accent[2] = 0.102f;
    f.uiKey = 2;
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
}  // namespace bridge
