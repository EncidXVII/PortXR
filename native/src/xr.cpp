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
Swapchain g_eyes[2], g_quad;

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
    out[3] = f.watchLit ? 1.f : 0.f;
    return true;
}

// UI panel keying (PrxFrame::uiKey). Auto latches after two probes in a row find the panel over 97% opaque: the game
// filled in the alpha, so the panel would hide the world. A new scene measures again.
bool g_uiKeyed = false;
int g_opaqueProbes = 0, g_keyScene = 0;
uint64_t g_panelFrames = 0;

bool ui_key(const PrxFrame& f) {
    if (!f.quadAlpha || f.uiKey == 2) return false;
    if (f.uiKey == 1) return true;
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

bool copy_layer(Swapchain& sc, const PrxLayer& layer, const std::wstring& capturePath, const Overlay& overlay = {}) {
    int w, h;
    if (!g_gpu->size(layer.texture, w, h) || !ensure_swapchain(sc, w, h)) return false;
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

void end_frame(const XrCompositionLayerBaseHeader* const* layers, uint32_t count) {
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO, nullptr, g_displayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, count, layers};
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
    const XrCompositionLayerBaseHeader* layers[2];
    uint32_t count = 0;
    bool stereo = false, hasQuad = false, curved = false;
    if (g_shouldRender) {
        Overlay over[2];
        for (int e = 0; e < 2; ++e) {
            over[e].vignette = f.vignette;
            // About 4 mm wide at arm's length on a typical 100 degree view.
            if (f.beam && beam_uv(f, e, over[e].beam)) over[e].beamWidth = 0.004f;
            if (f.watch) watch_uv(f, e, over[e].watch);
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
            layers[count++] = (XrCompositionLayerBaseHeader*)&proj;
            stereo = true;
        }
        PrxLayer quadSource = f.quadFromBackbuffer ? PrxLayer{g_gpu->backbuffer_copy(), 0} : f.quadLayer;
        Overlay panel;
        panel.key = ui_key(f) ? 1.f : 0.f;
        for (int i = 0; i < 4; ++i) panel.keep[i] = f.keepRect[i];
        // Measured unkeyed, about twice a second, until it latches.
        panel.probe = f.quadAlpha && f.uiKey == 0 && !g_uiKeyed && (g_panelFrames++ % 45) == 0;
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
                layers[count++] = (XrCompositionLayerBaseHeader*)&cylinder;
                curved = true;
            } else {
                quad.layerFlags = flags;
                quad.space = g_xr.local;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage = image;
                quad.pose = pose;
                quad.size = {f.quadWidth, f.quadHeight};
                layers[count++] = (XrCompositionLayerBaseHeader*)&quad;
            }
            hasQuad = true;
        }
    }
    g_gpu->set_mirror(stereo && f.mirror ? f.eye[0].texture : nullptr);
    end_frame(layers, count);
    std::lock_guard lock(g_statsMutex);
    g_stereoSubmitted += stereo;
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
    }
    input_shutdown();
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
    const char* enabled[] = {extension, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy_s(ici.applicationInfo.applicationName, appName, _TRUNCATE);
    strcpy_s(ici.applicationInfo.engineName, "Parallax");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = g_cylinder ? 2 : 1;
    ici.enabledExtensionNames = enabled;
    if (!xr_ok(xrCreateInstance(&ici, &g_xr.instance), "xrCreateInstance")) return fail("instance creation failed");
    if (!load_functions()) return fail("runtime is missing core functions");

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO, nullptr, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    if (!xr_ok(xrGetSystem(g_xr.instance, &sgi, &g_xr.system), "xrGetSystem")) return fail("no headset found");
    XrViewConfigurationView vc[2]{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    if (!xr_ok(xrEnumerateViewConfigurationViews(g_xr.instance, g_xr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                 2, &count, vc), "view configuration"))
        return fail("no stereo view configuration");
    *recommendedWidth = (int32_t)vc[0].recommendedImageRectWidth;
    *recommendedHeight = (int32_t)vc[0].recommendedImageRectHeight;
    if (!input_create_actions()) prx_log("controller actions unavailable");
    g_xr.state = StateInstance;
    prx_log("OpenXR instance ready, eye %dx%d", *recommendedWidth, *recommendedHeight);
    return 1;
}

PRX_API int32_t prx_state() { return g_xr.state; }

PRX_API void* prx_render_event_func() { return (void*)&on_render_event; }

PRX_API void prx_set_init_texture(void* texture) { g_initTexture = texture; }

PRX_API void prx_set_frame(int32_t slot, const PrxFrame* frame) {
    std::lock_guard lock(g_slotMutex);
    g_slots[slot % kSlots] = *frame;
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
