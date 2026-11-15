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
void shutdown();
}  // namespace bridge
