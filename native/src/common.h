#pragma once
#include <windows.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <atomic>
#include <cstdint>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define PRX_API extern "C" __declspec(dllexport)

// Structs below are mirrored field-for-field in managed Native.cs.
struct PrxPose { float px, py, pz, qx, qy, qz, qw; };
struct PrxFov { float left, right, up, down; };

struct PrxLayer {
    void* texture;  // ID3D11Texture2D* or ID3D12Resource* from Texture.GetNativeTexturePtr
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
    int32_t mirror;  // draw the left eye into the game window at Present
    float quadRadius;  // >0 (only when PrxStats.curvedSupported): curved screen, quadPose is the cylinder's centre
    float vignette;    // comfort vignette on the eye layers, 0..1
    int32_t beam;      // draw the laser beam from beamFrom to beamTo (tracking space, metres) into the eye layers
    float beamFrom[3], beamTo[3];
    // Wrist watch: a round face at watchAt (tracking space) on the back of the left hand, watchRadius metres. It fades
    // in (watchAlpha 0..1) as the player looks at it, like DesktopVR's clock; lit (watchLit 0..1, eased) while glanced
    // at, when the menu is a trigger pull away.
    int32_t watch;
    float watchLit;
    float watchAt[3], watchRadius, watchAlpha;
    // UI panel black: 0 auto (made see-through once the panel is found fully opaque, e.g. a full-screen effect on a
    // UI camera wrote alpha 1; until `scene` changes), 1 always see-through, 2 never.
    int32_t uiKey, scene;
    // Part of the panel never keyed (the VR menu, whose dark background must stay solid): UV from the top-left,
    // x0 y0 x1 y1; an empty rect keys everything.
    float keepRect[4];
    // Parallax mode: each eye's view of the game through the screen, shown on that eye's own quad at quadPose
    // (quadWidth x quadHeight). The eye layers then carry only the beam and the watch, over the screen.
    int32_t window;
    PrxLayer windowLayer[2];
    // Aim reticle per eye: centre in image UV (top-left origin) and radius in UV of the image height; 0 draws none.
    float aimReticle[2][3];
    // The game's crosshair: the middle of the flat UI panel (aimPatch: half width and half height in panel UV) is left
    // out of it, and with aimPatchShown drawn on its own quad at aimPose (aimSize metres), where the camera aims.
    float aimPatch[2];
    int32_t aimPatchShown;
    PrxPose aimPose;
    float aimSize[2];
    // Theme accent (sRGB 0..1) for the beam, the aim reticle and the lit watch.
    float accent[3];
    // Theater and Parallax: the room around the screen instead of black, where the runtime shares its passthrough.
    int32_t passthrough;
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
    // Keyboard + mouse targets per controller input (PrxBind order). A scan code, 0x100 | code for extended keys,
    // BindMouse + 0..2 for the left, right and middle mouse button, or 0 for nothing.
    int32_t binds[12];
    // Snap turning: the right stick no longer turns by itself; turnX does (mouse pixels this frame in keyboard +
    // mouse mode, right-stick X -1..1 in gamepad mode). The managed side closes the loop on the camera's yaw.
    int32_t snap;
    float turnX;
    // Gesture targets for the first 10 inputs (A to right trigger), same codes as binds: double press, held past
    // half a second, and pressed while the same hand's grip is held. 0 leaves that gesture off.
    int32_t twice[10], held[10], combo[10];
    // Head aim: the right stick no longer looks up or down by itself either; turnY does (mouse pixels, up positive, or
    // right-stick Y). The managed side steers the game camera toward where the head looks.
    int32_t aim;
    float turnY;
    // 0 keeps the laser pointer's trigger off the OS mouse button: the VR menu or the game's UI (reached through its
    // EventSystem) takes the click. The OS cursor follows the pointer only while the game window is in front.
    int32_t pointerClick;
};

enum PrxBind { BindA, BindB, BindX, BindY, BindLGrip, BindRGrip, BindLStick, BindRStick, BindLTrigger, BindRTrigger,
               BindMenuTap, BindMenuHold, BindCount };
constexpr int32_t BindMouse = 0x1000;

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
    uint64_t curvedSubmitted;
    int32_t curvedSupported;
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
    X(xrEnumerateViewConfigurationViews) X(xrGetD3D11GraphicsRequirementsKHR) X(xrGetD3D12GraphicsRequirementsKHR) \
    X(xrCreateSession) \
    X(xrDestroySession) X(xrBeginSession) X(xrEndSession) X(xrRequestExitSession) X(xrPollEvent) \
    X(xrCreateReferenceSpace) X(xrDestroySpace) X(xrLocateViews) X(xrLocateSpace) X(xrWaitFrame) \
    X(xrBeginFrame) X(xrEndFrame) X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) \
    X(xrDestroySwapchain) X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) \
    X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) X(xrStringToPath) X(xrCreateActionSet) \
    X(xrCreateAction) X(xrSuggestInteractionProfileBindings) X(xrAttachSessionActionSets) \
    X(xrSyncActions) X(xrGetActionStateBoolean) X(xrGetActionStateFloat) X(xrGetActionStateVector2f) \
    X(xrCreateActionSpace) X(xrApplyHapticFeedback) X(xrStopHapticFeedback) X(xrResultToString) \
    X(xrEnumerateEnvironmentBlendModes)

#define XR_DECLARE(name) extern PFN_##name name;
XR_FUNCS(XR_DECLARE)
#undef XR_DECLARE

void prx_log(const char* fmt, ...);
bool xr_ok(XrResult r, const char* what);

// input.cpp
bool input_create_actions();
bool input_attach_session();
void input_shutdown();
