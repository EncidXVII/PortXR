#pragma once
// The VR core as plain functions for engine adapters that should not see Windows, Direct3D or OpenXR headers (the
// Godot extension). Poses are OpenXR tracking space: metres, Y up, -Z forward; quaternions x y z w.
#include <cstdint>
#include <string>


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
    Pose camera{};          // the game camera in the world (right-handed, Y up, -Z forward)
    std::string cameraKey;  // scene and camera name
    bool cursorFree = false, padGame = false, carried = false, menuOpen = false;
};
struct Frame {
    Pose eyes[2];  // where to render the eyes, in the game world
    Fov fovs[2];
    bool aiming = false;
    uint32_t events = 0;  // 1: the VR menu button was double-tapped
};
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
// End of frame: the eyes rendered at those poses, and the game's UI (straight alpha, uiAspect = width / height) on the
// panel in front.
void submit_view(void* left, void* right, void* ui, float uiAspect, bool flipY);
void shutdown();
}  // namespace bridge
