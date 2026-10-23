#pragma once
#include <windows.h>
#include <d3d11_1.h>
#include <atomic>
#include <cstdint>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define PRX_API extern "C" __declspec(dllexport)

// Structs below are mirrored field-for-field in managed Native.cs.
struct PrxPose { float px, py, pz, qx, qy, qz, qw; };
struct PrxFov { float left, right, up, down; };

struct PrxLayer {
    void* texture;  // ID3D11Texture2D* from Texture.GetNativeTexturePtr
    int32_t flipY;
};

struct PrxFrame {
    int32_t stereo;
    PrxLayer eye[2];
    PrxPose eyePose[2];
    PrxFov eyeFov[2];
    int32_t quad;
    int32_t quadFromBackbuffer;  // use the texture grabbed by the capture event instead of quadLayer.texture
    PrxLayer quadLayer;
    PrxPose quadPose;
    float quadWidth, quadHeight;
    int32_t quadAlpha;
};

enum PrxButton : uint32_t {
    BtnA = 1, BtnB = 2, BtnX = 4, BtnY = 8, BtnLStick = 16, BtnRStick = 32, BtnMenu = 64,
};

struct PrxControllers {
    uint32_t buttons;
    float lx, ly, rx, ry;
    float lTrigger, rTrigger, lGrip, rGrip;
    PrxPose aim[2];
    int32_t aimValid[2];
};

struct PrxInputConfig {
    int32_t mode;  // 0 virtual gamepad, 1 keyboard + mouse
    int32_t neutral;
    int32_t pointerActive;
    int32_t pointerX, pointerY;  // client pixels, top-left origin
    float mouseSpeed;            // pixels per second at full right-stick deflection
};

enum PrxEvent : uint32_t { EvMenuToggle = 1 };

struct PrxStats {
    int32_t state;
    int32_t focused;
    double periodMs;
    double p95Ms;
    uint64_t frames;
    uint64_t submitted;
    uint64_t quadSubmitted;
    uint64_t stereoSubmitted;
};

enum PrxState : int32_t { StateFailed = -1, StateNone = 0, StateInstance = 1, StateSession = 2, StateRunning = 3 };

// Shared between xr.cpp and input.cpp.
struct XrGlobals {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE;
    std::atomic<int32_t> state = StateNone;
    std::atomic<int32_t> focused = 0;
    std::atomic<int64_t> lastPredicted = 0;  // XrTime of the most recent xrWaitFrame
    std::atomic<int64_t> period = 11111111;  // ns
};
extern XrGlobals g_xr;

#define XR_FUNCS(X) \
    X(xrEnumerateInstanceExtensionProperties) X(xrCreateInstance) X(xrDestroyInstance) X(xrGetSystem) \
    X(xrEnumerateViewConfigurationViews) X(xrGetD3D11GraphicsRequirementsKHR) X(xrCreateSession) \
    X(xrDestroySession) X(xrBeginSession) X(xrEndSession) X(xrRequestExitSession) X(xrPollEvent) \
    X(xrCreateReferenceSpace) X(xrDestroySpace) X(xrLocateViews) X(xrLocateSpace) X(xrWaitFrame) \
    X(xrBeginFrame) X(xrEndFrame) X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) \
    X(xrDestroySwapchain) X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) \
    X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) X(xrStringToPath) X(xrCreateActionSet) \
    X(xrCreateAction) X(xrSuggestInteractionProfileBindings) X(xrAttachSessionActionSets) \
    X(xrSyncActions) X(xrGetActionStateBoolean) X(xrGetActionStateFloat) X(xrGetActionStateVector2f) \
    X(xrCreateActionSpace) X(xrApplyHapticFeedback) X(xrStopHapticFeedback) X(xrResultToString)

#define XR_DECLARE(name) extern PFN_##name name;
XR_FUNCS(XR_DECLARE)
#undef XR_DECLARE

void prx_log(const char* fmt, ...);
bool xr_ok(XrResult r, const char* what);

// input.cpp
bool input_create_actions();
bool input_attach_session();
void input_shutdown();

// blit.cpp
bool blit_init(ID3D11Device* device);
void blit_shutdown();
// Draws src into dst (full viewport). dstSrgb: dst view encodes on write.
void blit_draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, ID3D11RenderTargetView* dst, int w, int h,
               bool flipY, bool dstSrgb);
// Writes a 32-bit top-down BMP of tex (test capture).
void blit_save_bmp(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const wchar_t* path);
