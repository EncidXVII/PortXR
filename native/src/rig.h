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
};

void recenter();
// Tests: controller input to use instead of the headset's (null: the real controllers).
void set_test_controllers(const PrxControllers* c);
// Tests: the fake right hand at the head, pointing at (u, v) on the UI panel; u < 0 stops pointing.
void set_test_point(float u, float v);
// views: this frame's eye poses in tracking space (prx_locate). Runs input (prx_input_update) and places the eyes.
Result frame(const GameFrame& game, const PrxPose views[2]);
// The UI panel in tracking space; width in metres (height follows the UI's aspect).
Pose panel(float& width);
float scale();
}  // namespace rig
