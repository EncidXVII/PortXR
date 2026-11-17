#pragma once
// The player in the game world, engine-neutral: the Unity plugin's Rig behaviour moved into the core so every engine
// adapter shares it. Each frame the adapter reports the game camera and what only the engine knows; the rig recentres,
// turns the body with the game camera's heading, runs head or hand aim and snap turning through the input emulation,
// drives the comfort vignette, and places the eyes in the game world. The UI panel stays put in the room.
//
// Conventions: right-handed, Y up, -Z forward (OpenXR's and Godot's; the Unity plugin converts at its edge). Angles the
// player sees (headings, snap and aim) run clockwise from above, in degrees, as the plugin's did.
#include "common.h"
#include "mathx.h"
#include <string>

namespace rig {
struct Pose {
    mathx::V3 p;
    mathx::Q q;
};

struct GameFrame {
    bool hasCamera = false;
    bool noTheater = false;  // always renders both eyes: VR screen is the 3D screen without depth (Unreal)
    Pose camera;              // the game camera in the world
    std::string cameraKey;    // scene and camera name: first-person verdicts are kept per key
    bool cursorFree = false;  // the game shows a free cursor (a menu): no head aim
    bool padGame = false;     // Controls Auto picks the gamepad (the game reads one)
    bool carried = false;     // the camera rides on a player body (first person until its turns say otherwise)
    bool menuOpen = false;    // the VR menu takes the controllers
    float uiAspect = 16.f / 9.f;  // the game's UI (width / height): the panel's shape
    bool pointerAuto = true;      // Laser pointer: with the game's cursor (Auto) or the menu only
    Pose menuPose;                // the open VR menu in tracking space (its quad), and its size in metres
    float menuWidth = 0, menuHeight = 0;
    bool screenUnlocked = false;  // the menu's Screen: Unlocked (Theater and Parallax): the handles show
    float fovY = 1.f;             // the game camera's vertical field of view, radians (Parallax's window)
    float nearClip = 0.05f;
    float focus = 0;              // what sits at the middle of the view, metres along the camera (0: unknown)
    mathx::V3 up{0, 1, 0};        // the player's up: world up, or a body's own where the game applies its own gravity
    float sceneScale = 1;         // 2D scenes: the player scaled up so the play plane sits 2.5 m off (Parallax's focus too)
    mathx::V3 eyeOffset;          // a game script's nudge to the eyes, metres in tracking space
    bool worldPointer = false;    // the hide-objects tool: the laser goes into the game world and game input pauses
    float worldReach = 3;         // ... ending this far along it (metres in tracking space: what it would hide)
    int screenW = 0, screenH = 0; // the game window's client size: the laser moves the OS cursor there (0: it does not)
    bool pointerClick = false;    // the laser's trigger presses the OS mouse button (the engine's own UI did not take it)
};

enum Mode { Stereo = 0, Parallax = 1, Theater = 2 };

// The Theater and Parallax screen in tracking space. Curved where the runtime has curved layers and the screen sits near
// eye level: then `pose` is the cylinder's centre and `radius` its radius.
struct Screen {
    Pose pose;
    float width = 3.2f, height = 1.8f, radius = 0;
    Pose center;  // the middle of the screen's face (flat: the same as pose)
};

// The laser pointer on the UI panel: where (UV, top-left origin, eased against hand tremor) and the trigger.
struct Pointer {
    bool active = false;  // on the panel while wanted: the adapter moves the game's cursor there
    float u = 0, v = 0;
    bool down = false;
    bool entered = false;  // just arrived on the panel this frame
    bool onMenu = false;   // on the open VR menu instead (u, v on the menu)
};

struct Watch {
    bool shown = false;
    mathx::V3 at;  // tracking space
    float alpha = 0, lit = 0;
};

struct Result {
    Pose eyes[2];      // in the game world
    bool aiming = false;
    float vignette = 0;
    uint32_t events = 0;  // PrxEvent bits (EvMenuToggle: a double tap on the menu button, or a wrist glance and trigger)
    PrxControllers controllers{};
    Pointer pointer;
    bool beam = false;
    mathx::V3 beamFrom, beamTo;  // tracking space
    Watch watch;
    int mode = Stereo;
    Screen screen;
    // Parallax: each eye's window camera, from the game camera: an offset along its right (game units) and its frustum
    // (tangents of left, right, up, down).
    float windowOffset[2] = {};
    PrxFov windowFov[2] = {};
    // Screen handles (with the screen unlocked): shown, the one under the laser (1 bar, 2 corner, 3 reset), the drag.
    bool handles = false;
    int handle = 0, grab = 0;
    float screenYaw = 0, screenPitch = 0, screenDistance = 3, screenWidth = 3.2f;
    // Hand aim: the right controller steers the game camera; its newest valid pose (tracking space).
    bool handAiming = false;
    PrxPose handAim{};
    // Tracking space to the game world, as the eyes were placed: base + body * (origin.q^-1 * (p - origin.p) * scale).
    Pose origin;
    mathx::Q body;
    mathx::V3 base;
    float scale = 1;
    // For tests and diagnostics.
    float bodyYaw = 0, orbitRadius = 0, focus = 0;
    bool stepSmoothing = false, glancing = false;
};

void recenter();
// Tests: controller input to use instead of the headset's (null: the real controllers).
void set_test_controllers(const PrxControllers* c);
// Tests: the fake right hand at the head, pointing at (u, v) on the UI panel; u < 0 stops pointing.
void set_test_point(float u, float v, bool screen = false);
// Tests: the null headset never looks at the raised wrist; this counts it as looked at.
void set_test_wrist(bool on);
// views: this frame's eye poses in tracking space (prx_locate). Runs input (prx_input_update) and places the eyes.
Result frame(const GameFrame& game, const PrxPose views[2]);
// The UI panel in tracking space; width in metres (height follows the UI's aspect).
Pose panel(float& width);
float scale();
}  // namespace rig
