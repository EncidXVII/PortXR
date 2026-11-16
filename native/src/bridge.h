#pragma once
// The VR core as plain functions for engine adapters that should not see Windows, Direct3D or OpenXR headers (the
// Godot extension). Poses are OpenXR tracking space: metres, Y up, -Z forward; quaternions x y z w.
#include <cstdint>
#include <string>
#include <vector>


namespace bridge {
struct Pose { float p[3], q[4]; };
struct Fov { float left, right, up, down; };

// Creates the OpenXR instance for a Direct3D 12 game; false with a reason in the log when there is no runtime.
bool start(const char* app, int& eyeWidth, int& eyeHeight);
// Hands the core the game's device through one of its textures (ID3D12Resource*); the session follows.
void attach(void* texture);
int state();  // PrxState: -1 failed, 0 none, 1 instance, 2 session, 3 running
// Start of a game frame: waits for the headset's frame (pacing the game) and begins it.
void begin();
bool locate(Pose eyes[2], Fov fovs[2]);
// End of a game frame: one flat layer (the spike's screen) at `pose`, width x height metres.
void submit_quad(void* texture, const Pose& pose, float width, float height, bool flipY);
// Eye images rendered at the poses from locate().
void submit_eyes(void* left, void* right, const Pose eyes[2], const Fov fovs[2], bool flipY);
std::string poll_log();

// --- The shared behaviour (view.h, settings.h) -------------------------------------------------------------------
// This game's settings file; the all-games defaults come from %APPDATA%\Parallax\defaults.cfg.
void settings_open(const std::wstring& gameFile);
std::string setting(const char* section, const char* key, const char* fallback);
void set_setting(const char* section, const char* key, const std::string& value);
void recenter();
// What only the engine knows about this frame.
struct Game {
    bool hasCamera = false;
    bool noTheater = false;   // no VR screen: Full VR there instead (Unreal)
    Pose camera{};          // the game camera in the world (right-handed, Y up, -Z forward)
    std::string cameraKey;  // scene and camera name
    bool cursorFree = false, padGame = false, carried = false, menuOpen = false;
    float uiAspect = 16.f / 9.f;  // the game's UI, width / height
    float fovY = 1.f, nearClip = 0.05f;  // the game camera's vertical field of view (radians) and near plane
    float focus = 0;                     // Parallax: distance to what sits at the middle of the view (0: unknown)
    float up[3] = {0, 1, 0};             // the player's up (a body under the game's own gravity), world space
    float sceneScale = 1;                // 2D scenes: the player scaled so the play plane sits 2.5 m off
    float eyeOffset[3] = {};             // a game script's nudge to the eyes, metres in tracking space
    bool worldPointer = false;           // the hide-objects tool has the laser (into the game world)
    float worldReach = 3;                // where its beam ends, metres along it
    int screenW = 0, screenH = 0;        // the window's client size, when the laser should move the OS cursor
    bool pointerClick = false;           // the laser's trigger presses the OS mouse button
    bool crosshair = false;              // the core draws the crosshair while aiming (engines without their own)
    float aimDistance = 0;               // game units from the camera to what it aims at (0: unknown, 40 m)
    bool tests = false;                  // a test run drives the menu: no welcome card
};
struct Frame {
    Pose eyes[2];  // where to render the eyes, in the game world
    Fov fovs[2];
    bool aiming = false;
    uint32_t events = 0;  // 1: the VR menu was asked for (double tap on the menu button, or the wrist watch)
    // The laser pointer on the UI panel: the adapter moves the game's cursor to (u, v) (top-left origin) and clicks
    // with `down`, through the engine's own input. Not while the VR menu has it.
    bool pointer = false, pointerDown = false;
    float pointerU = 0, pointerV = 0;
    // The VR menu (the core's): whether it is open, and what it asks of the engine this frame.
    bool menuOpen = false;
    bool quit = false, renderScale = false, cameraPicked = false, hideObjects = false, showHidden = false;
    std::string camera;
    // Display: 0 Stereo (render `eyes`), 1 Parallax (render the window eyes: from the game camera, `windowOffset` game
    // units along its right, with `windowFov`), 2 Theater (render nothing: submit the game's flat picture as the UI).
    int mode = 0;
    bool handles = false;  // the screen's handles are shown (menu open, screen unlocked)
    float windowOffset[2] = {};
    Fov windowFov[2] = {};
    Pose views[2];  // the headset's eyes in tracking space
    // For tools the engine runs itself (hiding objects, the weapon in the hand): the right controller's aim in the game
    // world (while rightValid), its trigger and B button, whether hand aim steers the camera, game units per metre.
    bool rightValid = false, handAiming = false, bDown = false;
    Pose right{};
    float rightTrigger = 0, scale = 1;
};
// After frame(): the rig's own parts of a frame's layers (eye poses, where the UI or screen goes, beam, watch,
// vignette, theme accent, passthrough), for adapters that fill the textures themselves (Unity).
void layers(void* prxFrame, float uiAspect);
// Tests: the head moved to (x, y, z) in tracking space, turned yaw degrees clockwise; off restores it.
void test_head(bool on, float x, float y, float z, float yawDeg);
// What the engine offers the VR menu: the game's name and folder, the cameras the view can follow.
struct MenuHost {
    std::string product;
    std::wstring gameDir;
    std::vector<std::string> cameras;
    std::string currentCamera;
    bool hideObjects = false, aimExtras = false, handHeld = false, uiBlack = false, noTheater = false;
    int hidden = 0, eyeWidth = 0, eyeHeight = 0;
};
void set_menu_host(const MenuHost& host);
void toggle_menu();
// Tests: write the next frame's layers (left.bmp, right.bmp, quad.bmp, menu.bmp) into `dir`.
void capture(const std::wstring& dir);
// After begin(): locates the headset, runs the controllers (aim, snap turning, keyboard & mouse or gamepad) and places
// the eyes. False while the headset has no pose.
bool frame(const Game& game, Frame& out);
// The game's window, for the keyboard & mouse emulation (SendInput while it is in front).
void set_window(void* hwnd);
// The emulated gamepad this frame (XINPUT_GAMEPAD layout), for engines that read pads themselves; false when the
// gamepad mode is off.
struct Pad { uint16_t buttons; uint8_t leftTrigger, rightTrigger; int16_t lx, ly, rx, ry; };
bool pad(Pad& out);
// Tests: sticks (lx ly rx ry), triggers and buttons (PrxButton bits) instead of the controllers; off restores them.
void test_controllers(bool on, float lx, float ly, float rx, float ry, float lt, float rt, uint32_t buttons);
// Tests: the fake right hand points at (u, v) on the UI panel (u < 0: not pointing).
void test_point(float u, float v, bool screen = false);
// End of frame: the eyes rendered at those poses, and the game's UI (straight alpha, uiAspect = width / height) on the
// panel in front.
void submit_view(void* left, void* right, void* ui, float uiAspect, bool flipY);
void shutdown();
}  // namespace bridge
