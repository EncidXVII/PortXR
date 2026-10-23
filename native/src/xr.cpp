#include "common.h"
#include <wrl/client.h>
#include <algorithm>
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
enum Event { EvInit = 1, EvBegin = 2, EvSubmit = 3, EvShutdown = 4 };
constexpr int kSlots = 4;

struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    int width = 0, height = 0;
    std::vector<ID3D11Texture2D*> images;
    std::vector<ComPtr<ID3D11RenderTargetView>> views;
};

HMODULE g_loader;
PFN_xrGetInstanceProcAddr g_getProc;
ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_ctx;
std::atomic<void*> g_initTexture;
DXGI_FORMAT g_format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
bool g_formatSrgb = true;
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
uint64_t g_frames, g_submitted, g_quadSubmitted, g_stereoSubmitted;

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
    return xrCreateSession && xrGetD3D11GraphicsRequirementsKHR && xrWaitFrame;
}

bool fail(const char* msg) {
    prx_log("%s", msg);
    g_xr.state = StateFailed;
    return false;
}

void destroy_swapchain(Swapchain& sc) {
    sc.views.clear();
    sc.images.clear();
    if (sc.handle) xrDestroySwapchain(sc.handle);
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
    uint32_t count = 0;
    xrEnumerateSwapchainImages(sc.handle, 0, &count, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(sc.handle, count, &count, (XrSwapchainImageBaseHeader*)imgs.data());
    for (auto& img : imgs) {
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = g_format;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11RenderTargetView> rtv;
        if (FAILED(g_device->CreateRenderTargetView(img.texture, &rd, &rtv))) {
            prx_log("swapchain RTV failed");
            destroy_swapchain(sc);
            return false;
        }
        sc.images.push_back(img.texture);
        sc.views.push_back(rtv);
    }
    sc.width = w;
    sc.height = h;
    prx_log("swapchain %dx%d, %u images", w, h, count);
    return true;
}

bool copy_layer(Swapchain& sc, const PrxLayer& layer, const std::wstring& capturePath) {
    auto* tex = (ID3D11Texture2D*)layer.texture;
    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);
    if (!ensure_swapchain(sc, (int)desc.Width, (int)desc.Height)) return false;
    uint32_t index;
    if (!xr_ok(xrAcquireSwapchainImage(sc.handle, nullptr, &index), "xrAcquireSwapchainImage")) return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    if (xr_ok(xrWaitSwapchainImage(sc.handle, &wi), "xrWaitSwapchainImage")) {
        blit_draw(g_ctx.Get(), tex, sc.views[index].Get(), sc.width, sc.height, layer.flipY != 0, g_formatSrgb);
        if (!capturePath.empty()) blit_save_bmp(g_ctx.Get(), sc.images[index], capturePath.c_str());
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
            g_formatSrgb = f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            return;
        }
    }
    if (!formats.empty()) g_format = (DXGI_FORMAT)formats[0];
}

void do_init() {
    void* texture = g_initTexture.exchange(nullptr);
    if (g_xr.state != StateInstance || !texture) return;
    ((ID3D11Resource*)texture)->GetDevice(&g_device);
    g_device->GetImmediateContext(&g_ctx);

    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!xr_ok(xrGetD3D11GraphicsRequirementsKHR(g_xr.instance, g_xr.system, &req), "graphics requirements"))
        return (void)fail("no D3D11 support in runtime");
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC ad{};
    if (SUCCEEDED(g_device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter))) adapter->GetDesc(&ad);
    // A zero LUID (SteamVR null driver) means the runtime accepts any adapter.
    bool anyAdapter = req.adapterLuid.HighPart == 0 && req.adapterLuid.LowPart == 0;
    if (!anyAdapter && memcmp(&ad.AdapterLuid, &req.adapterLuid, sizeof(LUID)) != 0) {
        prx_log("game adapter %ls luid %08lx:%08lx, runtime wants %08lx:%08lx", ad.Description, ad.AdapterLuid.HighPart,
                ad.AdapterLuid.LowPart, req.adapterLuid.HighPart, req.adapterLuid.LowPart);
        return (void)fail("game renders on a different GPU than the headset runtime");
    }

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = g_device.Get();
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO, &binding, 0, g_xr.system};
    if (!xr_ok(xrCreateSession(g_xr.instance, &sci, &g_xr.session), "xrCreateSession"))
        return (void)fail("session creation failed");
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr, XR_REFERENCE_SPACE_TYPE_LOCAL,
                                  {{0, 0, 0, 1}, {0, 0, 0}}};
    if (!xr_ok(xrCreateReferenceSpace(g_xr.session, &rs, &g_xr.local), "local space"))
        return (void)fail("reference space failed");
    pick_format();
    if (!input_attach_session()) prx_log("controller input unavailable");
    if (!blit_init(g_device.Get())) return (void)fail("blit init failed");
    prx_log("session created, swapchain format %d", (int)g_format);
    g_xr.state = StateSession;
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

void do_submit(int slot) {
    if (!g_frameBegun) return;
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
    const XrCompositionLayerBaseHeader* layers[2];
    uint32_t count = 0;
    bool stereo = false, hasQuad = false;
    if (g_shouldRender) {
        if (f.stereo && f.eye[0].texture && f.eye[1].texture &&
            copy_layer(g_eyes[0], f.eye[0], path(L"left.bmp")) && copy_layer(g_eyes[1], f.eye[1], path(L"right.bmp"))) {
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
        if (f.quad && f.quadLayer.texture && copy_layer(g_quad, f.quadLayer, path(L"quad.bmp"))) {
            quad.layerFlags = f.quadAlpha ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
            quad.space = g_xr.local;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage = {g_quad.handle, {{0, 0}, {g_quad.width, g_quad.height}}, 0};
            quad.pose = to_xr(f.quadPose);
            quad.size = {f.quadWidth, f.quadHeight};
            layers[count++] = (XrCompositionLayerBaseHeader*)&quad;
            hasQuad = true;
        }
    }
    end_frame(layers, count);
    std::lock_guard lock(g_statsMutex);
    g_stereoSubmitted += stereo;
    g_quadSubmitted += hasQuad;
}

void do_shutdown() {
    if (g_xr.state == StateNone) return;
    g_xr.state = StateNone;
    g_running = false;
    destroy_swapchain(g_eyes[0]);
    destroy_swapchain(g_eyes[1]);
    destroy_swapchain(g_quad);
    input_shutdown();
    blit_shutdown();
    if (g_xr.local) xrDestroySpace(g_xr.local);
    if (g_xr.session) xrDestroySession(g_xr.session);
    if (g_xr.instance) xrDestroyInstance(g_xr.instance);
    g_xr.local = XR_NULL_HANDLE;
    g_xr.session = XR_NULL_HANDLE;
    g_xr.instance = XR_NULL_HANDLE;
    g_ctx.Reset();
    g_device.Reset();
}

void __stdcall on_render_event(int id) {
    switch (id & 0xff) {
    case EvInit: do_init(); break;
    case EvBegin: do_begin(); break;
    case EvSubmit: do_submit((id >> 8) % kSlots); break;
    case EvShutdown: do_shutdown(); break;
    }
}
}  // namespace

PRX_API int32_t prx_init(const char* appName, int32_t* recommendedWidth, int32_t* recommendedHeight) {
    if (g_xr.state != StateNone) return g_xr.state >= StateInstance;
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
    bool d3d11 = std::any_of(exts.begin(), exts.end(),
                             [](auto& e) { return strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0; });
    if (!d3d11) return fail("runtime lacks XR_KHR_D3D11_enable");

    const char* enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy_s(ici.applicationInfo.applicationName, appName, _TRUNCATE);
    strcpy_s(ici.applicationInfo.engineName, "Parallax");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = 1;
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
    out->stereoSubmitted = g_stereoSubmitted;
}
