#include "gpu.h"
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

XrGlobals g_xr;
#define XR_DEFINE(name) PFN_##name name = nullptr;
XR_FUNCS(XR_DEFINE)
#undef XR_DEFINE

namespace {
enum Event { EvInit = 1, EvBegin = 2, EvSubmit = 3, EvShutdown = 4, EvCaptureBackbuffer = 5 };
constexpr int kSlots = 4;

struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    int width = 0, height = 0;
};

HMODULE g_loader;
PFN_xrGetInstanceProcAddr g_getProc;
int g_api;  // 11 or 12, from the plugin
bool g_cylinder;  // runtime supports curved layers (theater screen)
Gpu* g_gpu;
std::atomic<void*> g_initTexture;
DXGI_FORMAT g_format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
// g_patch: the UI panel's middle on its own, for its layer at the aim point. Every layer shows a whole swapchain
// image: some runtimes draw sub-rectangles of a shared image wrong (Virtual Desktop showed them solid white).
Swapchain g_eyes[2], g_quad, g_patch, g_window[2];

// Overlays drawn by the core on the CPU (0: the VR menu, 1: the screen's handles): uploaded to a texture of our own
// when their pixels change, shown over everything at a pose in tracking space; curved around the pose (a cylinder's
// centre) when a radius is given and the runtime has curved layers.
constexpr int kOverlays = 2;
struct Overlay2 {
    Swapchain swapchain;
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    uint64_t version = 0, uploaded = 0;
    bool shown = false;
    PrxPose pose{};
    float size[2]{}, radius = 0;
    void* texture = nullptr;
};
std::mutex g_overlayMutex;
Overlay2 g_overlays[kOverlays];
// Transparent eye images of our own, for frames whose eyes carry only the beam and the watch (Theater).
void* g_clearEye;
int g_eyeSize[2];  // the headset's recommended eye image

std::mutex g_slotMutex;
PrxFrame g_slots[kSlots];

// Render thread only.
bool g_running, g_frameBegun, g_shouldRender;
XrTime g_displayTime;

std::mutex g_statsMutex;
double g_intervals[256];
int g_intervalCount;
LARGE_INTEGER g_lastWait;
uint64_t g_frames, g_submitted, g_quadSubmitted, g_stereoSubmitted, g_curvedSubmitted;

std::mutex g_captureMutex;
std::wstring g_captureDir;

template <class T>
T* load(const char* name) {
    PFN_xrVoidFunction fn = nullptr;
    g_getProc(g_xr.instance, name, &fn);
    return (T*)fn;
}

bool load_functions() {
#define XR_LOAD(name) name = load<std::remove_pointer_t<PFN_##name>>(#name);
    XR_FUNCS(XR_LOAD)
#undef XR_LOAD
    return xrCreateSession && xrWaitFrame &&
           (g_api == 12 ? xrGetD3D12GraphicsRequirementsKHR != nullptr : xrGetD3D11GraphicsRequirementsKHR != nullptr);
}

bool fail(const char* msg) {
    prx_log("%s", msg);
    g_xr.state = StateFailed;
    return false;
}

void destroy_swapchain(Swapchain& sc) {
    if (sc.handle) {
        g_gpu->forget_images(sc.handle);
        xrDestroySwapchain(sc.handle);
    }
    sc = {};
}

bool ensure_swapchain(Swapchain& sc, int w, int h) {
    if (sc.handle && sc.width == w && sc.height == h) return true;
    destroy_swapchain(sc);
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = g_format;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (!xr_ok(xrCreateSwapchain(g_xr.session, &ci, &sc.handle), "xrCreateSwapchain")) return false;
    if (!g_gpu->bind_images(sc.handle, g_format)) {
        destroy_swapchain(sc);
        return false;
    }
    sc.width = w;
    sc.height = h;
    prx_log("swapchain %dx%d", w, h);
    return true;
}

// The beam segment as seen by one eye, in that eye's image UV; false when it is entirely behind the eye.
// A point as seen by one eye: image UV and distance in front; false behind the eye.
bool point_uv(const PrxFrame& f, int e, const float w[3], float uv[2], float& depth) {
    const PrxPose& v = f.eyePose[e];
    float px = w[0] - v.px, py = w[1] - v.py, pz = w[2] - v.pz;
    float qx = -v.qx, qy = -v.qy, qz = -v.qz, qw = v.qw;
    float tx = 2 * (qy * pz - qz * py), ty = 2 * (qz * px - qx * pz), tz = 2 * (qx * py - qy * px);
    float r0 = px + qw * tx + (qy * tz - qz * ty), r1 = py + qw * ty + (qz * tx - qx * tz), r2 = pz + qw * tz + (qx * ty - qy * tx);
    if (r2 > -0.02f) return false;
    const PrxFov& fov = f.eyeFov[e];
    float l = std::tan(fov.left), r = std::tan(fov.right), u = std::tan(fov.up), d = std::tan(fov.down);
    depth = -r2;
    uv[0] = (r0 / depth - l) / (r - l);
    uv[1] = (u - r1 / depth) / (u - d);
    return true;
}

// The watch face for one eye: centre UV and radius in UV of the image height.
bool watch_uv(const PrxFrame& f, int e, float out[4]) {
    float depth;
    if (!point_uv(f, e, f.watchAt, out, depth)) return false;
    out[2] = f.watchRadius / depth / (std::tan(f.eyeFov[e].up) - std::tan(f.eyeFov[e].down));
    out[3] = f.watchLit;
    return true;
}

// UI panel keying (PrxFrame::uiKey). Auto latches after two probes in a row find the panel over 97% opaque: the game
// filled in the alpha, so the panel would hide the world. A new scene measures again, and so does Auto after the
// plugin moved the head camera off screen (3): the cleared screen usually leaves the UI's own alpha, so keying stops,
// but a pipeline pass that still paints the screen (a sun drawn after the clear) latches it again.
bool g_uiKeyed = false;
int g_opaqueProbes = 0, g_keyScene = 0, g_lastUiKey = 0;
uint64_t g_panelFrames = 0;

bool ui_key(const PrxFrame& f) {
    bool fresh = f.uiKey == 3 && g_lastUiKey != 3;
    g_lastUiKey = f.uiKey;
    if (!f.quadAlpha || f.uiKey == 2) return false;
    if (f.uiKey == 1) return true;
    if (fresh) {
        g_uiKeyed = false;
        g_opaqueProbes = 0;
        prx_log("UI panel drawn on a cleared screen; measuring its alpha again");
    }
    if (f.scene != g_keyScene) {
        g_keyScene = f.scene;
        g_uiKeyed = false;
        g_opaqueProbes = 0;
    }
    float opaque = g_gpu->opaque_fraction();
    if (opaque >= 0 && !g_uiKeyed) {
        g_opaqueProbes = opaque > 0.97f ? g_opaqueProbes + 1 : 0;
        if (g_opaqueProbes >= 2) {
            g_uiKeyed = true;
            prx_log("UI panel is opaque (%.0f%%); its black is made see-through", opaque * 100);
        }
    }
    return g_uiKeyed;
}

bool beam_uv(const PrxFrame& f, int e, float out[4]) {
    const PrxPose& v = f.eyePose[e];
    // Rotate by the inverse of the eye orientation: q* (p - eye) q.
    auto local = [&](const float* w, float r[3]) {
        float px = w[0] - v.px, py = w[1] - v.py, pz = w[2] - v.pz;
        float qx = -v.qx, qy = -v.qy, qz = -v.qz, qw = v.qw;
        float tx = 2 * (qy * pz - qz * py), ty = 2 * (qz * px - qx * pz), tz = 2 * (qx * py - qy * px);
        r[0] = px + qw * tx + (qy * tz - qz * ty);
        r[1] = py + qw * ty + (qz * tx - qx * tz);
        r[2] = pz + qw * tz + (qx * ty - qy * tx);
    };
    float a[3], b[3];
    local(f.beamFrom, a);
    local(f.beamTo, b);
    // The eye looks down -Z; clip the segment to a near plane before projecting.
    const float nearZ = -0.02f;
    if (a[2] > nearZ && b[2] > nearZ) return false;
    auto clip = [&](float* p, const float* q) {
        if (p[2] <= nearZ) return;
        float t = (nearZ - q[2]) / (p[2] - q[2]);
        for (int i = 0; i < 3; ++i) p[i] = q[i] + (p[i] - q[i]) * t;
    };
    clip(a, b);
    clip(b, a);
    const PrxFov& fov = f.eyeFov[e];
    float l = std::tan(fov.left), r = std::tan(fov.right), u = std::tan(fov.up), d = std::tan(fov.down);
    auto project = [&](const float* p, float* uv) {
        uv[0] = (p[0] / -p[2] - l) / (r - l);
        uv[1] = (u - p[1] / -p[2]) / (u - d);
    };
    project(a, out);
    project(b, out + 2);
    return true;
}

// width/height: the swapchain's size when not the source's (a crop).
bool copy_layer(Swapchain& sc, const PrxLayer& layer, const std::wstring& capturePath, const Overlay& overlay = {},
                int width = 0, int height = 0) {
    int w, h;
    if (!g_gpu->size(layer.texture, w, h)) return false;
    if (width > 0) w = width, h = height;
    else if (overlay.crop[2] > 0)
        w = (int)(w * (overlay.crop[2] - overlay.crop[0])), h = (int)(h * (overlay.crop[3] - overlay.crop[1]));
    if (!ensure_swapchain(sc, w, h)) return false;
    uint32_t index;
    if (!xr_ok(xrAcquireSwapchainImage(sc.handle, nullptr, &index), "xrAcquireSwapchainImage")) return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    if (xr_ok(xrWaitSwapchainImage(sc.handle, &wi), "xrWaitSwapchainImage")) {
        g_gpu->draw(layer.texture, sc.handle, index, layer.flipY != 0, overlay,
                    capturePath.empty() ? nullptr : capturePath.c_str());
    }
    xrReleaseSwapchainImage(sc.handle, nullptr);
    return true;
}

XrPosef to_xr(const PrxPose& p) { return {{p.qx, p.qy, p.qz, p.qw}, {p.px, p.py, p.pz}}; }

void pick_format() {
    uint32_t count = 0;
    xrEnumerateSwapchainFormats(g_xr.session, 0, &count, nullptr);
    std::vector<int64_t> formats(count);
    xrEnumerateSwapchainFormats(g_xr.session, count, &count, formats.data());
    const DXGI_FORMAT preferred[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                     DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM};
    for (auto f : preferred) {
        if (std::find(formats.begin(), formats.end(), (int64_t)f) != formats.end()) {
            g_format = f;
            return;
        }
    }
    if (!formats.empty()) g_format = (DXGI_FORMAT)formats[0];
}

// Session creation waits for the backend: on D3D12 the game's queue is only known after its first Present.
void create_session() {
    if (g_xr.state != StateInstance || !g_gpu || !g_gpu->ready()) return;
    prx_log("creating session");
    if (!g_gpu->create_session(&g_xr.session)) return (void)fail("session creation failed");
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr, XR_REFERENCE_SPACE_TYPE_LOCAL,
                                  {{0, 0, 0, 1}, {0, 0, 0}}};
    if (!xr_ok(xrCreateReferenceSpace(g_xr.session, &rs, &g_xr.local), "local space"))
        return (void)fail("reference space failed");
    pick_format();
    if (!input_attach_session()) prx_log("controller input unavailable");
    prx_log("session created, swapchain format %d", (int)g_format);
    g_xr.state = StateSession;
}

void do_init() {
    void* texture = g_initTexture.exchange(nullptr);
    if (g_xr.state != StateInstance || !texture || g_gpu) return;
    prx_log("init on Direct3D %d", g_api);
    if (g_api == 12) {
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> probe;
        D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
        if (FAILED(((ID3D12Resource*)texture)->GetDevice(IID_PPV_ARGS(&device))) ||
            FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&probe))) || !gpu_hook_queue(probe.Get()) ||
            !gpu_hook_present(probe.Get()))
            return (void)fail("D3D12 setup failed");
        g_gpu = gpu_create_d3d12(device.Get());
    } else {
        ComPtr<ID3D11Device> device;
        ((ID3D11Resource*)texture)->GetDevice(&device);
        // Only the native mirror needs Present on D3D11, so a failed hook is not fatal.
        gpu_hook_present(device.Get());
        g_gpu = gpu_create_d3d11(device.Get());
    }
    if (!g_gpu) return (void)fail("blit init failed");
    gpu_set_present_target(g_gpu);
    create_session();
}

void poll_events() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_xr.instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto state = ((XrEventDataSessionStateChanged&)ev).state;
            prx_log("session state %d", (int)state);
            g_xr.focused = state == XR_SESSION_STATE_FOCUSED;
            if (state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
                if (xr_ok(xrBeginSession(g_xr.session, &bi), "xrBeginSession")) {
                    g_running = true;
                    g_xr.state = StateRunning;
                }
            } else if (state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(g_xr.session);
                g_running = false;
                g_xr.state = StateSession;
            } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
                g_running = false;
                g_xr.state = StateFailed;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            g_running = false;
            g_xr.state = StateFailed;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// Passthrough around the Theater and Parallax screen (PrxFrame::passthrough): the runtime's alpha-blend mode where it
// offers one, else Meta's passthrough layer (XR_FB_passthrough). SteamVR has neither.
bool g_alphaBlend, g_fbPassthrough;
XrEnvironmentBlendMode g_blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
struct {
    XrPassthroughFB feature = XR_NULL_HANDLE;
    XrPassthroughLayerFB layer = XR_NULL_HANDLE;
    bool running = false, failed = false;
    PFN_xrCreatePassthroughFB create;
    PFN_xrDestroyPassthroughFB destroy;
    PFN_xrPassthroughStartFB start;
    PFN_xrPassthroughPauseFB pause;
    PFN_xrCreatePassthroughLayerFB createLayer;
    PFN_xrDestroyPassthroughLayerFB destroyLayer;
    PFN_xrPassthroughLayerResumeFB resumeLayer;
    PFN_xrPassthroughLayerPauseFB pauseLayer;
} g_pt;

void destroy_passthrough() {
    if (g_pt.layer) g_pt.destroyLayer(g_pt.layer);
    if (g_pt.feature) g_pt.destroy(g_pt.feature);
    g_pt.layer = XR_NULL_HANDLE;
    g_pt.feature = XR_NULL_HANDLE;
    g_pt.running = g_pt.failed = false;
}

#define PT_LOAD(field, name) g_pt.field = load<std::remove_pointer_t<PFN_##name>>(#name)

// Sets this frame's blend mode; returns the passthrough layer to draw under everything, or null.
const XrCompositionLayerBaseHeader* passthrough_layer(bool wanted) {
    static XrCompositionLayerPassthroughFB layer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    g_blend = wanted && g_alphaBlend ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if (g_alphaBlend || !g_fbPassthrough || g_pt.failed) return nullptr;
    if (wanted && !g_pt.layer) {
        PT_LOAD(create, xrCreatePassthroughFB);
        PT_LOAD(destroy, xrDestroyPassthroughFB);
        PT_LOAD(start, xrPassthroughStartFB);
        PT_LOAD(pause, xrPassthroughPauseFB);
        PT_LOAD(createLayer, xrCreatePassthroughLayerFB);
        PT_LOAD(destroyLayer, xrDestroyPassthroughLayerFB);
        PT_LOAD(resumeLayer, xrPassthroughLayerResumeFB);
        PT_LOAD(pauseLayer, xrPassthroughLayerPauseFB);
        XrPassthroughCreateInfoFB ci{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB, nullptr, XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB};
        bool ok = g_pt.create && g_pt.destroy && g_pt.start && g_pt.pause && g_pt.createLayer && g_pt.destroyLayer &&
                  g_pt.resumeLayer && g_pt.pauseLayer &&
                  xr_ok(g_pt.create(g_xr.session, &ci, &g_pt.feature), "xrCreatePassthroughFB");
        XrPassthroughLayerCreateInfoFB li{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB, nullptr, g_pt.feature,
                                          XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB,
                                          XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB};
        ok = ok && xr_ok(g_pt.createLayer(g_xr.session, &li, &g_pt.layer), "xrCreatePassthroughLayerFB");
        if (!ok) {
            if (g_pt.destroy) destroy_passthrough();
            g_pt.failed = true;
            prx_log("passthrough unavailable; the screen stays on black");
            return nullptr;
        }
        g_pt.running = true;
        prx_log("passthrough on (XR_FB_passthrough)");
    }
    if (!g_pt.layer) return nullptr;
    if (wanted != g_pt.running) {
        if (wanted) {
            g_pt.start(g_pt.feature);
            g_pt.resumeLayer(g_pt.layer);
        } else {
            g_pt.pauseLayer(g_pt.layer);
            g_pt.pause(g_pt.feature);
        }
        g_pt.running = wanted;
    }
    if (!wanted) return nullptr;
    layer.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer.space = XR_NULL_HANDLE;
    layer.layerHandle = g_pt.layer;
    return (const XrCompositionLayerBaseHeader*)&layer;
}

void end_frame(const XrCompositionLayerBaseHeader* const* layers, uint32_t count) {
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO, nullptr, g_displayTime, g_blend, count, layers};
    xr_ok(xrEndFrame(g_xr.session, &ei), "xrEndFrame");
    g_frameBegun = false;
    std::lock_guard lock(g_statsMutex);
    ++g_submitted;
}

void do_begin() {
    create_session();
    if (g_xr.state < StateSession) return;
    poll_events();
    if (g_frameBegun) end_frame(nullptr, 0);
    if (!g_running) return;
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (!xr_ok(xrWaitFrame(g_xr.session, nullptr, &fs), "xrWaitFrame")) return;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    {
        std::lock_guard lock(g_statsMutex);
        if (g_lastWait.QuadPart)
            g_intervals[g_intervalCount++ % 256] = 1000.0 * (now.QuadPart - g_lastWait.QuadPart) / freq.QuadPart;
        g_lastWait = now;
        ++g_frames;
    }
    g_xr.period = fs.predictedDisplayPeriod;
    g_xr.lastPredicted = fs.predictedDisplayTime;
    if (!xr_ok(xrBeginFrame(g_xr.session, nullptr), "xrBeginFrame")) return;
    g_frameBegun = true;
    g_displayTime = fs.predictedDisplayTime;
    g_shouldRender = fs.shouldRender;
}

// Fallback for engines without ScreenCapture.CaptureScreenshotIntoRenderTexture (Unity < 2019.1): at end of frame
// Unity still has the backbuffer bound, holding the game UI and IMGUI.
void do_capture_backbuffer() {
    if (g_gpu) g_gpu->capture_backbuffer();
}

void do_submit(int slot) {
    if (!g_frameBegun) {
        if (g_gpu) g_gpu->set_mirror(nullptr);
        return;
    }
    PrxFrame f;
    {
        std::lock_guard lock(g_slotMutex);
        f = g_slots[slot];
    }
    std::wstring dir;
    {
        std::lock_guard lock(g_captureMutex);
        dir.swap(g_captureDir);
    }
    auto path = [&](const wchar_t* name) { return dir.empty() ? std::wstring() : dir + L"\\" + name; };

    XrCompositionLayerProjectionView views[2]{};
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerCylinderKHR cylinder{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
    XrCompositionLayerQuad window[2]{{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}};
    // The flat panel's middle on its own at the aim point: see PrxFrame::aimPatch.
    XrCompositionLayerQuad patch{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad overlayQuad[kOverlays]{{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}};
    XrCompositionLayerCylinderKHR overlayCurve[kOverlays]{{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR}, {XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR}};
    const XrCompositionLayerBaseHeader* layers[10];
    // Eyes without images of their own (Theater): transparent ones, for the beam and the watch.
    if (f.stereo && (!f.eye[0].texture || !f.eye[1].texture) && g_gpu) {
        if (!g_clearEye && g_eyeSize[0] > 0) {
            std::vector<uint8_t> zero((size_t)g_eyeSize[0] * g_eyeSize[1] * 4, 0);
            g_clearEye = g_gpu->upload(nullptr, zero.data(), g_eyeSize[0], g_eyeSize[1]);
        }
        f.eye[0] = f.eye[1] = {g_clearEye, 0};
        if (!g_clearEye) f.stereo = 0;
    }
    const XrCompositionLayerBaseHeader *projLayer = nullptr, *uiLayer = nullptr, *patchLayer = nullptr;
    uint32_t count = 0;
    bool stereo = false, hasQuad = false, curved = false;
    if (g_shouldRender) {
        if (auto* room = passthrough_layer(f.passthrough != 0)) layers[count++] = room;
        Overlay over[2];
        for (int e = 0; e < 2; ++e) {
            over[e].vignette = f.vignette;
            std::copy(f.eyeCrop[e], f.eyeCrop[e] + 4, over[e].crop);
            // A frame from an older managed side leaves it zero: keep lime.
            if (f.accent[0] + f.accent[1] + f.accent[2] > 0)
                for (int i = 0; i < 3; ++i) over[e].accent[i] = f.accent[i];
            // About 4 mm wide at arm's length on a typical 100 degree view.
            if (f.beam && beam_uv(f, e, over[e].beam)) over[e].beamWidth = 0.004f;
            if (f.watch) {
                watch_uv(f, e, over[e].watch);
                over[e].watchAlpha = f.watchAlpha;
            }
            for (int i = 0; i < 3; ++i) over[e].aim[i] = f.aimReticle[e][i];
        }
        if (f.window && f.windowLayer[0].texture && f.windowLayer[1].texture &&
            copy_layer(g_window[0], f.windowLayer[0], path(L"windowL.bmp")) &&
            copy_layer(g_window[1], f.windowLayer[1], path(L"windowR.bmp"))) {
            for (int e = 0; e < 2; ++e) {
                window[e].space = g_xr.local;
                window[e].eyeVisibility = e == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
                window[e].subImage = {g_window[e].handle, {{0, 0}, {g_window[e].width, g_window[e].height}}, 0};
                window[e].pose = to_xr(f.quadPose);
                window[e].size = {f.quadWidth, f.quadHeight};
                layers[count++] = (XrCompositionLayerBaseHeader*)&window[e];
            }
        }
        if (f.stereo && f.eye[0].texture && f.eye[1].texture &&
            copy_layer(g_eyes[0], f.eye[0], path(L"left.bmp"), over[0]) &&
            copy_layer(g_eyes[1], f.eye[1], path(L"right.bmp"), over[1])) {
            for (int e = 0; e < 2; ++e) {
                views[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                views[e].pose = to_xr(f.eyePose[e]);
                views[e].fov = {f.eyeFov[e].left, f.eyeFov[e].right, f.eyeFov[e].up, f.eyeFov[e].down};
                views[e].subImage = {g_eyes[e].handle, {{0, 0}, {g_eyes[e].width, g_eyes[e].height}}, 0};
            }
            proj.space = g_xr.local;
            proj.viewCount = 2;
            proj.views = views;
            projLayer = (XrCompositionLayerBaseHeader*)&proj;
            stereo = true;
        }
        PrxLayer quadSource = f.quadFromBackbuffer ? PrxLayer{g_gpu->backbuffer_copy(), 0} : f.quadLayer;
        Overlay panel;
        // 1: the game filled the panel opaque, so its black is keyed out; -1: see-through, colour without alpha dropped.
        panel.key = ui_key(f) ? 1.f : f.quadAlpha ? -1.f : 0.f;
        for (int i = 0; i < 4; ++i) panel.keep[i] = f.keepRect[i];
        // Measured unkeyed, about twice a second, until it latches.
        panel.probe = f.quadAlpha && (f.uiKey == 0 || f.uiKey == 3) && !g_uiKeyed && (g_panelFrames++ % 45) == 0;
        // While aiming the middle leaves the flat panel (a hole) and is copied to its own layer.
        bool split = f.aimPatch[0] > 0 && f.quadRadius <= 0;
        float middle[4] = {0.5f - f.aimPatch[0], 0.5f - f.aimPatch[1], 0.5f + f.aimPatch[0], 0.5f + f.aimPatch[1]};
        if (split) std::copy(middle, middle + 4, panel.hole);
        if (f.quad && quadSource.texture && copy_layer(g_quad, quadSource, path(L"quad.bmp"), panel)) {
            XrCompositionLayerFlags flags = f.quadAlpha ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
            XrSwapchainSubImage image{g_quad.handle, {{0, 0}, {g_quad.width, g_quad.height}}, 0};
            XrPosef pose = to_xr(f.quadPose);
            if (f.quadRadius > 0) {
                cylinder.layerFlags = flags;
                cylinder.space = g_xr.local;
                cylinder.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                cylinder.subImage = image;
                cylinder.pose = pose;
                cylinder.radius = f.quadRadius;
                cylinder.centralAngle = f.quadWidth / f.quadRadius;
                cylinder.aspectRatio = f.quadWidth / f.quadHeight;
                uiLayer = (XrCompositionLayerBaseHeader*)&cylinder;
                curved = true;
            } else {
                Overlay middlePart;
                middlePart.key = panel.key;
                std::copy(middle, middle + 4, middlePart.crop);
                int pw = std::max(1, (int)(2 * f.aimPatch[0] * g_quad.width)), ph = std::max(1, (int)(2 * f.aimPatch[1] * g_quad.height));
                if (split && f.aimPatchShown && copy_layer(g_patch, quadSource, path(L"patch.bmp"), middlePart, pw, ph)) {
                    patch.layerFlags = flags;
                    patch.space = g_xr.local;
                    patch.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    patch.subImage = {g_patch.handle, {{0, 0}, {g_patch.width, g_patch.height}}, 0};
                    patch.pose = to_xr(f.aimPose);
                    patch.size = {f.aimSize[0], f.aimSize[1]};
                    patchLayer = (XrCompositionLayerBaseHeader*)&patch;
                }
                quad.layerFlags = flags;
                quad.space = g_xr.local;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage = image;
                quad.pose = pose;
                quad.size = {f.quadWidth, f.quadHeight};
                uiLayer = (XrCompositionLayerBaseHeader*)&quad;
            }
            hasQuad = true;
        }
    }
    // Theater and Parallax: the eyes are transparent but for the beam and the watch, so they go over the screen.
    bool overlayEyes = f.window || !f.quadAlpha;
    if (overlayEyes) proj.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    const XrCompositionLayerBaseHeader* order[2] = {projLayer, uiLayer};
    if (overlayEyes) std::swap(order[0], order[1]);
    for (auto* l : order)
        if (l) layers[count++] = l;
    if (patchLayer) layers[count++] = patchLayer;
    // The core's overlays over everything, with their own alpha: the screen's handles, then the VR menu on top.
    if (g_shouldRender) {
        std::lock_guard lock(g_overlayMutex);
        for (int i = kOverlays - 1; i >= 0; --i) {
            Overlay2& o = g_overlays[i];
            if (!o.shown || o.pixels.empty()) continue;
            if (o.uploaded != o.version || !o.texture) {
                o.texture = g_gpu->upload(o.texture, o.pixels.data(), o.w, o.h);
                o.uploaded = o.version;
            }
            if (!o.texture || !copy_layer(o.swapchain, {o.texture, 0}, path(i == 0 ? L"menu.bmp" : L"handles.bmp"))) continue;
            XrSwapchainSubImage image{o.swapchain.handle, {{0, 0}, {o.swapchain.width, o.swapchain.height}}, 0};
            if (o.radius > 0 && g_cylinder) {
                auto& c = overlayCurve[i];
                c.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                c.space = g_xr.local;
                c.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                c.subImage = image;
                c.pose = to_xr(o.pose);
                c.radius = o.radius;
                c.centralAngle = o.size[0] / o.radius;
                c.aspectRatio = o.size[0] / o.size[1];
                layers[count++] = (XrCompositionLayerBaseHeader*)&c;
            } else {
                auto& q = overlayQuad[i];
                q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                q.space = g_xr.local;
                q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                q.subImage = image;
                q.pose = to_xr(o.pose);
                q.size = {o.size[0], o.size[1]};
                layers[count++] = (XrCompositionLayerBaseHeader*)&q;
            }
        }
    }
    g_gpu->set_mirror(!f.mirror ? nullptr : f.window ? f.windowLayer[0].texture : stereo ? f.eye[0].texture : nullptr);
    end_frame(layers, count);
    std::lock_guard lock(g_statsMutex);
    g_stereoSubmitted += stereo && f.quadAlpha && !f.window;
    g_quadSubmitted += hasQuad;
    g_curvedSubmitted += curved;
}

void do_shutdown() {
    if (g_xr.state == StateNone) return;
    // Failed until the end, so the plugin cannot start a new instance while this one is torn down.
    g_xr.state = StateFailed;
    g_running = false;
    g_frameBegun = false;
    gpu_set_present_target(nullptr);
    if (g_gpu) {
        destroy_swapchain(g_eyes[0]);
        destroy_swapchain(g_eyes[1]);
        destroy_swapchain(g_quad);
        destroy_swapchain(g_patch);
        destroy_swapchain(g_window[0]);
        destroy_swapchain(g_window[1]);
        for (auto& o : g_overlays) {
            destroy_swapchain(o.swapchain);
            o.texture = nullptr;
            o.uploaded = 0;
        }
        g_clearEye = nullptr;
    }
    input_shutdown();
    if (g_pt.destroy) destroy_passthrough();
    g_alphaBlend = g_fbPassthrough = false;
    g_blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if (g_xr.local) xrDestroySpace(g_xr.local);
    if (g_xr.session) xrDestroySession(g_xr.session);
    if (g_xr.instance) xrDestroyInstance(g_xr.instance);
    g_xr.local = XR_NULL_HANDLE;
    g_xr.session = XR_NULL_HANDLE;
    g_xr.instance = XR_NULL_HANDLE;
    delete g_gpu;
    g_gpu = nullptr;
    g_xr.state = StateNone;
}

void __stdcall on_render_event(int id) {
    switch (id & 0xff) {
    case EvInit: do_init(); break;
    case EvBegin: do_begin(); break;
    case EvSubmit: do_submit((id >> 8) % kSlots); break;
    case EvShutdown: do_shutdown(); break;
    case EvCaptureBackbuffer: do_capture_backbuffer(); break;
    }
}
}  // namespace

PRX_API int32_t prx_init(const char* appName, int32_t api, int32_t* recommendedWidth, int32_t* recommendedHeight) {
    if (g_xr.state != StateNone) return g_xr.state >= StateInstance;
    g_api = api;
    const char* extension = api == 12 ? XR_KHR_D3D12_ENABLE_EXTENSION_NAME : XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
    wchar_t path[MAX_PATH];
    HMODULE self;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&prx_init, &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    wcscpy_s(wcsrchr(path, L'\\') + 1, MAX_PATH - (wcsrchr(path, L'\\') + 1 - path), L"openxr_loader.dll");
    g_loader = LoadLibraryW(path);
    if (!g_loader) return fail("openxr_loader.dll not found next to the plugin");
    g_getProc = (PFN_xrGetInstanceProcAddr)GetProcAddress(g_loader, "xrGetInstanceProcAddr");
    if (!g_getProc) return fail("loader has no xrGetInstanceProcAddr");
    xrEnumerateInstanceExtensionProperties =
        load<std::remove_pointer_t<PFN_xrEnumerateInstanceExtensionProperties>>("xrEnumerateInstanceExtensionProperties");
    xrCreateInstance = load<std::remove_pointer_t<PFN_xrCreateInstance>>("xrCreateInstance");

    uint32_t count = 0;
    if (!xr_ok(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), "extensions"))
        return fail("no OpenXR runtime (start SteamVR or your headset app)");
    std::vector<XrExtensionProperties> exts(count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, exts.data());
    bool supported = std::any_of(exts.begin(), exts.end(),
                                 [&](auto& e) { return strcmp(e.extensionName, extension) == 0; });
    if (!supported) {
        prx_log("runtime lacks %s", extension);
        return fail("the headset runtime does not support this graphics API");
    }

    g_cylinder = std::any_of(exts.begin(), exts.end(), [](auto& e) {
        return strcmp(e.extensionName, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME) == 0;
    });
    if (!g_cylinder) prx_log("runtime lacks curved layers; the theater screen is flat");
    g_fbPassthrough = std::any_of(exts.begin(), exts.end(), [](auto& e) {
        return strcmp(e.extensionName, XR_FB_PASSTHROUGH_EXTENSION_NAME) == 0;
    });
    std::vector<const char*> enabled{extension};
    if (g_cylinder) enabled.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
    if (g_fbPassthrough) enabled.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy_s(ici.applicationInfo.applicationName, appName, _TRUNCATE);
    strcpy_s(ici.applicationInfo.engineName, "Parallax");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = (uint32_t)enabled.size();
    ici.enabledExtensionNames = enabled.data();
    if (!xr_ok(xrCreateInstance(&ici, &g_xr.instance), "xrCreateInstance")) return fail("instance creation failed");
    if (!load_functions()) return fail("runtime is missing core functions");

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO, nullptr, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    if (!xr_ok(xrGetSystem(g_xr.instance, &sgi, &g_xr.system), "xrGetSystem")) return fail("no headset found");
    XrViewConfigurationView vc[2]{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    if (!xr_ok(xrEnumerateViewConfigurationViews(g_xr.instance, g_xr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                 2, &count, vc), "view configuration"))
        return fail("no stereo view configuration");
    uint32_t modes = 0;
    XrEnvironmentBlendMode blend[8];
    if (xr_ok(xrEnumerateEnvironmentBlendModes(g_xr.instance, g_xr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 8,
                                               &modes, blend), "blend modes"))
        g_alphaBlend = std::find(blend, blend + modes, XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) != blend + modes;
    if (g_alphaBlend || g_fbPassthrough)
        prx_log("passthrough available (%s)", g_alphaBlend ? "alpha blend" : "XR_FB_passthrough");
    *recommendedWidth = (int32_t)vc[0].recommendedImageRectWidth;
    g_eyeSize[0] = (int)vc[0].recommendedImageRectWidth, g_eyeSize[1] = (int)vc[0].recommendedImageRectHeight;
    *recommendedHeight = (int32_t)vc[0].recommendedImageRectHeight;
    if (!input_create_actions()) prx_log("controller actions unavailable");
    g_xr.state = StateInstance;
    prx_log("OpenXR instance ready, eye %dx%d", *recommendedWidth, *recommendedHeight);
    return 1;
}

PRX_API int32_t prx_state() { return g_xr.state; }

// Whether Auto keying found the UI panel opaque in this scene.
PRX_API int32_t prx_ui_keyed() { return g_uiKeyed ? 1 : 0; }

PRX_API int32_t prx_passthrough() { return g_alphaBlend || g_fbPassthrough ? 1 : 0; }

PRX_API void* prx_render_event_func() { return (void*)&on_render_event; }

PRX_API void prx_set_init_texture(void* texture) { g_initTexture = texture; }

PRX_API void prx_set_frame(int32_t slot, const PrxFrame* frame) {
    std::lock_guard lock(g_slotMutex);
    g_slots[slot % kSlots] = *frame;
}

// The VR menu's image (RGBA8, sRGB-encoded, premultiplied) at `pose` (tracking space), widthM x heightM metres; null
// pixels hide it. Pass pixels only when they changed: each call is uploaded again.
PRX_API void prx_set_overlay(int32_t index, const uint8_t* rgba, int32_t w, int32_t h, const PrxPose* pose, float widthM,
                             float heightM, float radius) {
    if (index < 0 || index >= kOverlays) return;
    std::lock_guard lock(g_overlayMutex);
    Overlay2& o = g_overlays[index];
    o.shown = pose != nullptr;
    if (pose) {
        o.pose = *pose;
        o.size[0] = widthM, o.size[1] = heightM;
        o.radius = radius;
    }
    if (rgba && w > 0 && h > 0) {
        o.pixels.assign(rgba, rgba + (size_t)w * h * 4);
        o.w = w, o.h = h;
        ++o.version;
    }
}

PRX_API void prx_set_menu(const uint8_t* rgba, int32_t w, int32_t h, const PrxPose* pose, float widthM, float heightM) {
    prx_set_overlay(0, rgba, w, h, pose, widthM, heightM, 0);
}

PRX_API void prx_capture(const wchar_t* dir) {
    std::lock_guard lock(g_captureMutex);
    g_captureDir = dir;
}

// Views are predicted one period past the last xrWaitFrame: the render thread may still be a frame behind.
PRX_API int32_t prx_locate(PrxPose* poses, PrxFov* fovs) {
    if (g_xr.state != StateRunning || !g_xr.lastPredicted) return 0;
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                        g_xr.lastPredicted + g_xr.period, g_xr.local};
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t count = 0;
    if (XR_FAILED(xrLocateViews(g_xr.session, &li, &vs, 2, &count, views)) ||
        !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
        return 0;
    for (int e = 0; e < 2; ++e) {
        auto& p = views[e].pose;
        poses[e] = {p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z,
                    p.orientation.w};
        fovs[e] = {views[e].fov.angleLeft, views[e].fov.angleRight, views[e].fov.angleUp, views[e].fov.angleDown};
    }
    return 1;
}

PRX_API void prx_get_stats(PrxStats* out) {
    std::lock_guard lock(g_statsMutex);
    out->state = g_xr.state;
    out->focused = g_xr.focused;
    out->periodMs = g_xr.period / 1e6;
    int n = std::min(g_intervalCount, 256);
    std::vector<double> sorted(g_intervals, g_intervals + n);
    std::sort(sorted.begin(), sorted.end());
    out->p95Ms = n ? sorted[(size_t)(n * 0.95)] : 0;
    out->frames = g_frames;
    out->submitted = g_submitted;
    out->quadSubmitted = g_quadSubmitted;
    out->curvedSubmitted = g_curvedSubmitted;
    out->curvedSupported = g_cylinder;
    out->stereoSubmitted = g_stereoSubmitted;
}
