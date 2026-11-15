#pragma once
// Where the player's eyes go in the game world, and where the UI panel stands in the room: the engine-neutral part of
// the Unity plugin's Rig (TrackBody, the eye placement in BeforeRender, Placement), shared by every engine adapter.
//
// The room (tracking space) is anchored at the recenter point. The body turns with the game camera's heading, so the
// world turns around the player as the game's camera turns; the headset's movement since recentring is added on top,
// scaled by World scale, and lifted by Height. The UI panel stays put in the room, ahead of the recenter point.
#include "mathx.h"

namespace view {
struct Pose {
    mathx::V3 p;
    mathx::Q q;
};

// The next update anchors the room at the headset (also done on the first update).
void recenter();
// views: this frame's eye poses in tracking space (from prx_locate). cam: the game camera in the world (right-handed,
// Y up, -Z forward); without one the eyes stay where they were last placed. Writes the eyes' world poses.
void update(bool hasCamera, const Pose& cam, const Pose views[2], Pose eyes[2]);
// The UI panel in tracking space, and its width in metres (its height follows the game's UI aspect).
Pose panel(float& width);
float scale();
}  // namespace view
