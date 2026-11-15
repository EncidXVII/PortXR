#include "view.h"
#include "settings.h"

using namespace mathx;

namespace {
constexpr float kUiDrop = 0.15f;  // the panel's centre sits this far below eye level
V3 g_originPos;
Q g_originYaw;
bool g_recenterPending = true;
float g_bodyYaw, g_lastHeading;
bool g_bodyKnown;
V3 g_camPos;
}  // namespace

namespace view {
void recenter() { g_recenterPending = true; }

float scale() { return settings::getf("View", "WorldScale", 1.f, 0.5f, 3.f); }

void update(bool hasCamera, const Pose& cam, const Pose views[2], Pose eyes[2]) {
    if (g_recenterPending) {
        g_originPos = (views[0].p + views[1].p) * 0.5f;
        g_originYaw = yaw(heading(views[0].q));
        g_recenterPending = false;
    }
    if (hasCamera) {
        float h = heading(cam.q);
        if (!g_bodyKnown) g_bodyYaw = h;
        else g_bodyYaw += wrap(h - g_lastHeading);
        g_bodyKnown = true;
        g_lastHeading = h;
        g_camPos = cam.p;
    }
    Q body = yaw(g_bodyYaw), unYaw = g_originYaw.inverse();
    float s = scale();
    V3 lift{0, settings::getf("View", "Height", 0.f, -1.f, 1.f), 0};
    for (int e = 0; e < 2; ++e) {
        V3 rel = unYaw * (views[e].p - g_originPos);
        eyes[e].p = g_camPos + body * (rel * s) + lift;
        eyes[e].q = body * unYaw * views[e].q;
    }
}

Pose panel(float& width) {
    width = settings::getf("View", "UiWidth", 1.6f, 0.5f, 4.f);
    float distance = settings::getf("View", "UiDistance", 1.2f, 0.5f, 3.f);
    return {g_originPos + g_originYaw * V3{0, -kUiDrop, -distance}, g_originYaw};
}
}  // namespace view
