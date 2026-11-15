#include "rig.h"
#include "settings.h"
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <map>

extern "C" PRX_API uint32_t prx_input_update(const PrxInputConfig* cfg, const PrxControllers* test, PrxControllers* out);
extern "C" PRX_API void prx_haptic_pulse(int32_t hand, float amplitude, float seconds);
extern "C" PRX_API void prx_get_stats(PrxStats* out);

using namespace mathx;

namespace {
constexpr float kDeg = 180.f / kPi, kRad = kPi / 180.f;
constexpr float kUiDrop = 0.15f, kSmoothTurnDegPerSec = 120.f;

// Room anchor, body and the last frame's inputs.
V3 g_originPos;
Q g_originYaw;
bool g_recenterPending = true;
float g_bodyYaw, g_lastHeading;  // degrees, clockwise
bool g_bodyKnown, g_aiming;
V3 g_camPos;
PrxControllers g_controllers{};
PrxPose g_lastViews[2]{};
LARGE_INTEGER g_lastTick{};
float g_dt = 1 / 90.f, g_now;

// Snap turning: closed loop on the game camera's heading.
bool g_snapArmed = true, g_snapping;
float g_snapTarget, g_snapUntil, g_snapLastYaw, g_snapLastPush, g_degPerPixel;
// Head aim: last game camera angles, pushes, learned degrees per pixel (signed: games may invert Y), stick signs.
float g_aimLastYaw, g_aimLastPitch, g_aimPushX, g_aimPushY, g_aimDppX, g_aimDppY;
float g_padSignX = 1, g_padSignY = 1;
int g_padWrongY, g_aimWrongX, g_aimWrongY;
float g_probeIgnored, g_aimSuspendedUntil;
int g_ignoredTimes;
// Orbit check: how far in front of the camera its turning pivot sits (first person turns about itself).
V3 g_orbitLastPos, g_orbitLastFwd;
float g_orbitNum, g_orbitDen;
int g_orbitSamples;
std::string g_orbitKey;
std::map<std::string, bool> g_firstPerson;
// Comfort vignette.
float g_vignette, g_vignetteLastYaw;
V3 g_vignetteLastPos;
// Step smoothing: a camera moved at the physics rate judders at the headset's; follow a line one step behind.
V3 g_stepFrom, g_stepTo;
float g_stepAt, g_stepLen = 0.02f, g_lastMoveAt;
int g_stepMoved, g_stepStill, g_stepFrames;
bool g_stepSmoothing;
bool g_testOn;
// The Theater and Parallax screen: a direction from the recenter point (degrees right, up), a distance and a width,
// kept per game in the settings; `live` while a handle drags it (these lead, the settings follow on release).
float g_screenYaw, g_screenPitch, g_screenDistance = 3, g_screenWidth = 3.2f;
bool g_screenLive;
int g_handle, g_grab;
bool g_handlePull;
Q g_grabFrom;
float g_grabWidth, g_grabReach;
float g_focus = 3;
// Laser pointer and wrist watch.
float g_pointerU, g_pointerV;
bool g_pointerOn, g_pointerDown;
bool g_watchLooked, g_glancing, g_glanceTrigger;
float g_watchHeld, g_watchFade, g_watchGlow;
PrxControllers g_test{};
bool g_testWrist;
// The player's ground: world up, or a player body's own up where the game applies its own gravity (small planets),
// carried along without twist so headings stay continuous.
Q g_upFrame;
float g_sceneScale = 1;
bool g_handAiming;
PrxPose g_handAim{};

float clamp01(float v) { return std::clamp(v, 0.f, 1.f); }
float sign(float v) { return v < 0 ? -1.f : 1.f; }
float delta_angle(float from, float to) { return wrap((to - from) * kRad) * kDeg; }
V3 forward(const Q& q) { return q * V3{0, 0, -1}; }
// Clockwise heading and pitch in degrees, as the player sees them.
float heading_cw(const V3& f) { return std::atan2(f.x, -f.z) * kDeg; }
float pitch_deg(const V3& f) { return std::asin(std::clamp(f.y, -1.f, 1.f)) * kDeg; }
Q body() { return g_upFrame * yaw(-g_bodyYaw * kRad); }
// A camera's heading on the player's ground, stable when it looks straight up or down: the forward's level part vanishes
// there and the up vector's takes over.
float cam_heading(const Q& q) {
    Q un = g_upFrame.inverse();
    V3 f = un * forward(q), u = un * (q * V3{0, 1, 0});
    return heading_cw(f - u * f.y);
}
float cam_pitch(const Q& q) { return pitch_deg(g_upFrame.inverse() * forward(q)); }
Q q_of(const PrxPose& p) { return {p.qx, p.qy, p.qz, p.qw}; }
V3 v_of(const PrxPose& p) { return {p.px, p.py, p.pz}; }

std::string setting(const char* section, const char* key, const char* fallback) { return settings::get(section, key, fallback); }

float snap_angle() {
    std::string t = setting("Comfort", "Turning", "Smooth");
    return t == "Snap15" ? 15.f : t == "Snap45" ? 45.f : 30.f;
}

// Keyboard & mouse mode: the [Bindings] names to scan codes (0x100: extended key; 0x1000 + n: mouse button n).
int key_code(const std::string& name) {
    static const std::map<std::string, int> codes{
        {"None", 0}, {"Space", 0x39}, {"Enter", 0x1C}, {"Esc", 0x01}, {"Tab", 0x0F}, {"Backspace", 0x0E}, {"LeftShift", 0x2A},
        {"LeftCtrl", 0x1D}, {"LeftAlt", 0x38}, {"Q", 0x10}, {"E", 0x12}, {"R", 0x13}, {"F", 0x21}, {"G", 0x22}, {"C", 0x2E},
        {"V", 0x2F}, {"Z", 0x2C}, {"X", 0x2D}, {"T", 0x14}, {"I", 0x17}, {"M", 0x32}, {"1", 0x02}, {"2", 0x03}, {"3", 0x04},
        {"4", 0x05}, {"Up", 0x148}, {"Down", 0x150}, {"Left", 0x14B}, {"Right", 0x14D}, {"MouseLeft", 0x1000},
        {"MouseRight", 0x1001}, {"MouseMiddle", 0x1002}};
    auto it = codes.find(name);
    return it == codes.end() ? 0 : it->second;
}

void bindings(PrxInputConfig& cfg) {
    static const char* inputs[] = {"A", "B", "X", "Y", "LeftGrip", "RightGrip", "LeftStickClick", "RightStickClick",
                                   "LeftTrigger", "RightTrigger", "MenuTap", "MenuHold"};
    static const char* defaults[] = {"Space", "LeftCtrl", "E", "R", "LeftShift", "Q", "C", "None", "MouseRight", "MouseLeft", "Esc", "Tab"};
    for (int i = 0; i < BindCount; ++i) cfg.binds[i] = key_code(setting("Bindings", inputs[i], defaults[i]));
    for (int i = 0; i < 10; ++i) {
        cfg.twice[i] = key_code(setting("Bindings", (std::string(inputs[i]) + "Double").c_str(), "None"));
        cfg.held[i] = key_code(setting("Bindings", (std::string(inputs[i]) + "Hold").c_str(), "None"));
        bool grip = i == 4 || i == 5;
        cfg.combo[i] = grip ? 0 : key_code(setting("Bindings", (std::string(inputs[i]) + "Combo").c_str(), "None"));
    }
}

// 0 gamepad, 1 keyboard & mouse, 2 off.
int input_mode(bool padGame) {
    std::string m = setting("Controls", "Mode", "Auto");
    if (m == "Gamepad") return 0;
    if (m == "KeyboardMouse") return 1;
    if (m == "Off") return 2;
    return padGame ? 0 : 1;
}

void track_body(const rig::Pose& cam) {
    float h = cam_heading(cam.q);
    if (!g_bodyKnown) g_bodyYaw = h;
    else if (!g_aiming) g_bodyYaw += delta_angle(g_lastHeading, h);
    g_bodyKnown = true;
    g_lastHeading = h;
}

void sample_orbit(const rig::Pose& cam) {
    V3 p = cam.p, f = forward(cam.q), dc = p - g_orbitLastPos, df = f - g_orbitLastFwd;
    bool primed = g_orbitLastFwd.dot(g_orbitLastFwd) > 0;
    g_orbitLastPos = p;
    g_orbitLastFwd = f;
    // Only turns of at least 0.3 degrees standing still count: a jump says nothing about the pivot, and circle-strafing
    // moves a first-person camera exactly like an orbit.
    bool walking = g_controllers.lx * g_controllers.lx + g_controllers.ly * g_controllers.ly > 0.04f;
    if (!primed || walking || df.dot(df) < 2.7e-5f || dc.dot(dc) > 0.25f) return;
    g_orbitNum = g_orbitNum * 0.99f - dc.dot(df);
    g_orbitDen = g_orbitDen * 0.99f + df.dot(df);
    ++g_orbitSamples;
}

float orbit_radius() { return g_orbitDen > 0 ? g_orbitNum / g_orbitDen : 0; }

// Head aim on Auto: first person, judged per scene and camera from how the camera turns (below 0.45 m from itself;
// flipped only by strong evidence, above 1 m or below 0.2 m); without a verdict, a carried camera that has not ignored
// the pushes twice.
bool aim_wanted(const rig::GameFrame& g, int mode) {
    std::string aim = setting("Controls", "HeadAim", "Auto");
    if (aim == "Off" || mode == 2 || !g.hasCamera || !g_bodyKnown || g.menuOpen || g.worldPointer) return false;
    if (g_now < g_aimSuspendedUntil) return false;
    // A free cursor is a menu: mouse pushes would drag it (stick pushes leave it be when forced on).
    if (g.cursorFree && (aim != "On" || mode != 0)) return false;
    if (aim == "On") return true;
    auto known = g_firstPerson.find(g.cameraKey);
    if (g_orbitSamples >= 40) {
        float r = orbit_radius();
        bool fp = known == g_firstPerson.end() ? r < 0.45f : known->second;
        if (known != g_firstPerson.end()) {
            if (fp && r > 1.f) fp = false;
            else if (!fp && r < 0.2f) fp = true;
        }
        if (known == g_firstPerson.end() || known->second != fp)
            prx_log("head aim (auto): %s turns about %.2f m from itself, %s person", g.cameraKey.c_str(), r, fp ? "first" : "third");
        g_firstPerson[g.cameraKey] = fp;
        return fp;
    }
    if (known != g_firstPerson.end()) return known->second;
    return g.carried && g_ignoredTimes < 2;
}

void learn_pixels(float& dpp, int& wrong, float moved, float push, bool invertible) {
    if (std::fabs(push) < 4 || std::fabs(moved) < 0.05f) return;
    float sample = moved / push;
    if (std::fabs(sample) > 2.f) return;
    if (sign(sample) != (dpp != 0 ? sign(dpp) : 1.f)) {
        wrong = std::fabs(push) >= 20 && std::fabs(moved) >= 1.f ? wrong + 1 : 0;
        if (!invertible || wrong < 5) return;
        dpp = 0;
    }
    wrong = 0;
    dpp = dpp != 0 ? dpp + (sample - dpp) * 0.3f : sample;
}

void learn_stick(float& s, int& wrong, float moved, float push) {
    if (std::fabs(push) < 0.3f || std::fabs(moved) < 0.05f) return;
    if (sign(moved) == sign(push)) {
        wrong = 0;
        return;
    }
    wrong = std::fabs(push) >= 0.5f && std::fabs(moved) >= 1.f ? wrong + 1 : 0;
    if (wrong < 5) return;
    s = -s;
    wrong = 0;
}

float pixel_push(float err, float dpp) {
    if (std::fabs(err) < 0.3f) return 0;
    return dpp != 0 ? std::clamp(err * 0.5f / dpp, -800.f, 800.f) : sign(err) * std::min(std::fabs(err) * 2, 30.f);
}

// Steers the game camera to where the head (or right hand) looks: mouse pixels from a learned rate, or a proportional
// right stick. The right stick turns the body instead, smoothly or in snaps.
void head_aim(const rig::GameFrame& g, bool pad) {
    float rx = g_controllers.rx;
    if (setting("Comfort", "Turning", "Smooth") == "Smooth") g_bodyYaw += rx * std::fabs(rx) * kSmoothTurnDegPerSec * std::min(g_dt, 0.1f);
    else if (std::fabs(rx) < 0.3f) g_snapArmed = true;
    else if (g_snapArmed && std::fabs(rx) > 0.7f) {
        g_snapArmed = false;
        g_bodyYaw += sign(rx) * snap_angle();
    }
    bool hand = setting("Controls", "AimWith", "Hand") == "Hand" && g_controllers.aimValid[1];
    g_handAiming = hand;
    if (hand) g_handAim = g_controllers.aim[1];
    Q aimQ = hand ? Q{g_controllers.aim[1].qx, g_controllers.aim[1].qy, g_controllers.aim[1].qz, g_controllers.aim[1].qw} : q_of(g_lastViews[0]);
    V3 look = g_originYaw.inverse() * forward(aimQ);
    float wantYaw = g_bodyYaw + heading_cw(look);
    float wantPitch = std::clamp(pitch_deg(look), -80.f, 80.f);
    float yaw = cam_heading(g.camera.q), pitch = cam_pitch(g.camera.q);
    float movedX = delta_angle(g_aimLastYaw, yaw), movedY = pitch - g_aimLastPitch;
    float errX = delta_angle(yaw, wantYaw), errY = wantPitch - pitch;
    if (pad) {
        learn_stick(g_padSignY, g_padWrongY, movedY, g_aimPushY);
        g_aimPushX = std::fabs(errX) < 1.f ? 0 : sign(errX) * std::clamp(std::fabs(errX) / 25.f, 0.3f, 1.f) * g_padSignX;
        g_aimPushY = std::fabs(errY) < 1.f ? 0 : sign(errY) * std::clamp(std::fabs(errY) / 25.f, 0.3f, 1.f) * g_padSignY;
    } else {
        learn_pixels(g_aimDppX, g_aimWrongX, movedX, g_aimPushX, false);
        learn_pixels(g_aimDppY, g_aimWrongY, movedY, g_aimPushY, true);
        g_aimPushX = pixel_push(errX, g_aimDppX);
        g_aimPushY = pixel_push(errY, g_aimDppY);
    }
    g_aimLastYaw = yaw;
    g_aimLastPitch = pitch;
    // Pushes the camera ignores for 1.5 s pause aim for 3 s, so it never spins the view endlessly.
    if (std::fabs(errX) > 5.f && std::fabs(movedX) < 0.05f) g_probeIgnored += g_dt;
    else if (std::fabs(movedX) > 0.2f) g_probeIgnored = 0;
    if (g_probeIgnored > 1.5f) {
        g_probeIgnored = 0;
        g_aimSuspendedUntil = g_now + 3.f;
        ++g_ignoredTimes;
        prx_log("head aim: %s ignores the pushes; paused for 3 s", g.cameraKey.c_str());
    }
}

// A flick of the right stick turns the game camera by the snap angle, pushing until its heading arrives.
float snap_turn(const rig::Pose& cam, bool pad) {
    float yaw = cam_heading(cam.q), rx = g_controllers.rx;
    if (!g_snapping) {
        if (std::fabs(rx) < 0.3f) g_snapArmed = true;
        if (!g_snapArmed || std::fabs(rx) < 0.7f) return 0;
        g_snapArmed = false;
        g_snapping = true;
        g_snapTarget = yaw + sign(rx) * snap_angle();
        g_snapUntil = g_now + 0.6f;
        g_snapLastYaw = yaw;
        g_snapLastPush = 0;
    }
    float moved = delta_angle(g_snapLastYaw, yaw);
    if (!pad && std::fabs(g_snapLastPush) >= 4 && std::fabs(moved) > 0.05f && sign(moved) == sign(g_snapLastPush)) {
        float sample = moved / g_snapLastPush;
        g_degPerPixel = g_degPerPixel > 0 ? g_degPerPixel + (sample - g_degPerPixel) * 0.5f : sample;
    }
    g_snapLastYaw = yaw;
    float err = delta_angle(yaw, g_snapTarget);
    if (std::fabs(err) < 1.5f || g_now > g_snapUntil) {
        g_snapping = false;
        return g_snapLastPush = 0;
    }
    // Half the remaining error per frame: the game may apply input a frame late.
    if (pad) g_snapLastPush = sign(err) * std::clamp(std::fabs(err) / 20.f, 0.3f, 1.f);
    else g_snapLastPush = g_degPerPixel > 0 ? std::clamp(err * 0.5f / g_degPerPixel, -600.f, 600.f) : sign(err) * 40.f;
    return g_snapLastPush;
}

void update_vignette(bool hasCamera) {
    std::string level = setting("Comfort", "Vignette", "Off");
    if (level == "Off" || !hasCamera) {
        g_vignette = 0;
        return;
    }
    float dt = std::max(g_dt, 1e-3f);
    float speed = (g_camPos - g_vignetteLastPos).length() / dt, turn = std::fabs(delta_angle(g_vignetteLastYaw, g_bodyYaw)) / dt;
    g_vignetteLastPos = g_camPos;
    g_vignetteLastYaw = g_bodyYaw;
    float stick = std::sqrt(g_controllers.lx * g_controllers.lx + g_controllers.ly * g_controllers.ly);
    float motion = std::max(std::max(stick, g_snapping ? 1.f : 0.f), std::max(speed < 50 ? speed / 3.f : 0.f, turn / 90.f));
    float target = (level == "High" ? 1.f : 0.55f) * clamp01(motion);
    float rate = dt * (target > g_vignette ? 4.f : 1.5f);
    g_vignette = target > g_vignette ? std::min(target, g_vignette + rate) : std::max(target, g_vignette - rate);
}

struct Surface {
    V3 center;  // the middle of the face (flat), or the cylinder's centre (curved)
    Q rot;
    float w, h, radius;  // radius 0: flat
    V3 point(float u, float v) const {
        if (radius <= 0) return center + rot * V3{(u - 0.5f) * w, (0.5f - v) * h, 0};
        float a = (u - 0.5f) * w / radius;
        return center + rot * V3{std::sin(a) * radius, (0.5f - v) * h, -std::cos(a) * radius};
    }
};

bool hit_panel(const V3& from, const V3& dir, const V3& center, const Q& rot, float w, float h, float& u, float& v, V3& at);

// Where a ray meets a surface: UV (top-left origin) and the point. On the curved screen the player stands inside the
// cylinder, so the hit is where the ray leaves it (the plugin's HitCylinder).
bool hit(const Surface& s, const V3& from, const V3& dir, float& u, float& v, V3& at) {
    if (s.radius <= 0) return hit_panel(from, dir, s.center, s.rot, s.w, s.h, u, v, at);
    Q inv = s.rot.inverse();
    V3 p = inv * (from - s.center), d = inv * dir;
    float a = d.x * d.x + d.z * d.z, b = 2 * (p.x * d.x + p.z * d.z), c = p.x * p.x + p.z * p.z - s.radius * s.radius;
    float disc = b * b - 4 * a * c;
    if (a < 1e-6f || disc < 0) return false;
    float t = (-b + std::sqrt(disc)) / (2 * a);
    if (t <= 0) return false;
    V3 h = p + d * t;
    u = std::atan2(h.x, -h.z) * s.radius / s.w + 0.5f;
    v = 0.5f - h.y / s.h;
    at = s.center + s.rot * h;
    return u >= 0 && u <= 1 && v >= 0 && v <= 1;
}

// Where a ray meets the UI panel (tracking space): UV with a top-left origin and the point; false when it misses.
bool hit_panel(const V3& from, const V3& dir, const V3& center, const Q& rot, float w, float h, float& u, float& v, V3& at) {
    V3 n = rot * V3{0, 0, 1};
    float denom = dir.dot(n);
    if (std::fabs(denom) < 1e-4f) return false;
    float t = (center - from).dot(n) / denom;
    if (t <= 0) return false;
    at = from + dir * t;
    V3 local = rot.inverse() * (at - center);
    u = local.x / w + 0.5f;
    v = 0.5f - local.y / h;
    return u >= 0 && u <= 1 && v >= 0 && v <= 1;
}

V3 aim_from(const PrxPose& p) { return {p.px, p.py, p.pz}; }
V3 aim_dir(const PrxPose& p) { return Q{p.qx, p.qy, p.qz, p.qw} * V3{0, 0, -1}; }

// The pointer, as the plugin's UpdatePointer: wanted with the VR menu open, or (Laser pointer: with game cursor) while
// the game shows a free cursor; small moves eased against tremor, more so while the trigger is being pulled.
void update_pointer(const rig::GameFrame& g, rig::Result& r, const Surface& surface) {
    const PrxControllers& c = g_controllers;
    if (g.worldPointer) {
        // The hide-objects tool: the beam goes out into the game world and ends on what it would hide.
        g_pointerOn = g_pointerDown = false;
        r.pointer = {};
        r.beam = c.aimValid[1] != 0;
        r.beamFrom = aim_from(c.aim[1]);
        r.beamTo = r.beamFrom + aim_dir(c.aim[1]) * std::min(g.worldReach, 20.f);
        return;
    }
    bool wanted = g.menuOpen || (g.pointerAuto && g.cursorFree);
    float u = -1, v = -1;
    V3 at, from = aim_from(c.aim[1]), dir = aim_dir(c.aim[1]);
    // With the VR menu open the laser works the menu, not the game's UI.
    Surface target = g.menuOpen ? Surface{g.menuPose.p, g.menuPose.q, g.menuWidth, g.menuHeight, 0} : surface;
    bool on = c.aimValid[1] && hit(target, from, dir, u, v, at);
    if (on && g_pointerOn && wanted) {
        float d = std::sqrt((u - g_pointerU) * (u - g_pointerU) + (v - g_pointerV) * (v - g_pointerV));
        float follow = std::clamp(d * 60.f, c.rTrigger > 0.15f ? 0.04f : 0.15f, 1.f);
        g_pointerU += (u - g_pointerU) * follow;
        g_pointerV += (v - g_pointerV) * follow;
    } else if (on) {
        g_pointerU = u, g_pointerV = v;
    }
    bool active = on && wanted;
    r.pointer.entered = active && !g_pointerOn;
    if (r.pointer.entered) prx_haptic_pulse(1, 0.25f, 0.02f);
    g_pointerOn = active;
    g_pointerDown = active && (g_pointerDown ? c.rTrigger > 0.4f : c.rTrigger > 0.6f);
    r.pointer.active = active;
    r.pointer.onMenu = active && g.menuOpen;
    r.pointer.u = g_pointerU, r.pointer.v = g_pointerV;
    r.pointer.down = g_pointerDown;
    r.beam = wanted && c.aimValid[1];
    if (r.beam) {
        r.beamFrom = from;
        // The beam ends on the panel at the eased point, else a metre out.
        r.beamTo = active ? target.point(g_pointerU, g_pointerV) : from + dir;
    }
}

// The wrist watch, as the plugin's WristGlance (and DesktopVR's clock): the back of the left hand turned toward the
// eyes, close to the face, with the eyes on it. Shows wide, stays wider, held at least 0.3 s so it never blinks at the
// edge. Lit when turned toward the eyes or under the laser; then the right trigger opens the VR menu (returns true).
bool update_watch(const rig::GameFrame& g, rig::Result& r, const PrxPose views[2]) {
    constexpr float ShowFacing = 0.4f, ShowLooking = 0.7f, KeepFacing = 0.15f, KeepLooking = 0.45f, HoldS = 0.3f;
    constexpr float ShowS = 0.08f, HideS = 0.18f, GlowS = 0.1f;
    const PrxControllers& c = g_controllers;
    bool shown = settings::getb("Controls", "WristMenu", true) && c.aimValid[0] && !g.menuOpen;
    bool looked = false, glance = false;
    V3 at;
    if (shown) {
        Q hand{c.aim[0].qx, c.aim[0].qy, c.aim[0].qz, c.aim[0].qw};
        at = aim_from(c.aim[0]) + hand * V3{-0.035f, 0.01f, 0.12f};
        V3 toHead = (v_of(views[0]) + v_of(views[1])) * 0.5f - at;
        float dist = toHead.length();
        if (dist <= 0.7f && dist > 1e-4f) {
            V3 toEye = toHead * (1 / dist);
            float facing = (hand * V3{-1, 0, 0}).dot(toEye);
            float looking = g_testWrist ? 1.f : (q_of(views[0]) * V3{0, 0, -1}).dot(toEye * -1.f);
            looked = g_watchLooked ? facing > KeepFacing && looking > KeepLooking : facing > ShowFacing && looking > ShowLooking;
            glance = looked && facing > 0.6f;
            if (!glance && looked && c.aimValid[1]) {
                V3 from = aim_from(c.aim[1]), dir = aim_dir(c.aim[1]), to = at - from;
                float along = to.dot(dir);
                glance = along > 0 && along < 0.6f && (to - dir * along).length() < 0.06f;
            }
        }
    }
    g_watchHeld += g_dt;
    if (looked != g_watchLooked && (looked || g_watchHeld >= HoldS)) {
        g_watchLooked = looked;
        g_watchHeld = 0;
    }
    float target = shown && g_watchLooked ? 1.f : 0.f;
    g_watchFade = target > g_watchFade ? std::min(target, g_watchFade + g_dt / ShowS) : std::max(target, g_watchFade - g_dt / HideS);
    float glow = glance ? 1.f : 0.f;
    g_watchGlow += (glow - g_watchGlow) * std::min(1.f, g_dt / GlowS);
    if (std::fabs(glow - g_watchGlow) < 0.002f) g_watchGlow = glow;
    r.watch.shown = shown && g_watchFade > 0.004f;
    r.watch.at = at;
    r.watch.alpha = g_watchFade;
    r.watch.lit = g_watchGlow;
    bool trigger = c.rTrigger > 0.6f;
    if (glance && !g_glancing) prx_haptic_pulse(0, 0.2f, 0.02f);
    bool open = glance && trigger && !g_glanceTrigger;
    g_glancing = glance;
    g_glanceTrigger = trigger;
    return open;
}

constexpr float kMinScreen = 0.8f, kMaxScreen = 20.f;
Q pitch_q(float degrees) { return {std::sin(degrees * kRad / 2), 0, 0, std::cos(degrees * kRad / 2)}; }

void screen_from_settings() {
    if (g_screenLive) return;
    g_screenYaw = settings::getf("View", "ScreenYaw", 0, -360, 360);
    g_screenPitch = settings::getf("View", "ScreenPitch", 0, -60, 75);
    g_screenDistance = settings::getf("View", "ScreenDistance", 3, kMinScreen, kMaxScreen);
    g_screenWidth = settings::getf("View", "ScreenWidth", 3.2f, kMinScreen, kMaxScreen);
}

std::string number(float v, float step) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%g", std::round(v / step) * step);
    return buf;
}

void save_screen() {
    settings::set("View", "ScreenYaw", number(g_screenYaw, 0.1f));
    settings::set("View", "ScreenPitch", number(g_screenPitch, 0.1f));
    settings::set("View", "ScreenDistance", number(g_screenDistance, 0.01f));
    settings::set("View", "ScreenWidth", number(g_screenWidth, 0.01f));
}

// The screen as the plugin's Placement and CurveCenter: turned from the recenter point; curved around it at eye level.
bool g_curvedRuntime;
rig::Screen place_screen(bool window, float aspect) {
    screen_from_settings();
    rig::Screen s;
    bool curve = g_curvedRuntime && !window && std::fabs(g_screenPitch) < 12.f;
    Q rot = g_originYaw * yaw(-g_screenYaw * kRad) * pitch_q(curve ? 0 : g_screenPitch);
    s.width = g_screenWidth;
    s.height = g_screenWidth / (aspect > 0 ? aspect : 16.f / 9.f);
    if (curve) {
        float radius = g_screenDistance * std::cos(g_screenPitch * kRad);
        V3 c = g_originPos + V3{0, g_screenDistance * std::sin(g_screenPitch * kRad), 0};
        s.pose = {c, rot};
        s.radius = radius;
        s.center = {c + rot * V3{0, 0, -radius}, rot};
    } else {
        s.pose = s.center = {g_originPos + rot * V3{0, 0, -g_screenDistance}, rot};
    }
    return s;
}

Surface surface_of(const rig::Screen& s) {
    return s.radius > 0 ? Surface{s.pose.p, s.pose.q, s.width, s.height, s.radius} : Surface{s.center.p, s.center.q, s.width, s.height, 0};
}

// Handles on the screen, in its UV (the plugin's Bar, ResetRect and corners, in screen-height units).
int handle_at(float u, float v, float aspect) {
    float x = u * aspect, y = v, cw = aspect;  // x in screen heights
    float barX = cw / 2 - 0.16f, barY = 0.935f, barW = 0.32f, barH = 0.065f;
    if (x >= barX && x < barX + barW && y >= barY && y < barY + barH) return 1;
    float rx = barX + barW + 0.02f;
    if (x >= rx && x < rx + 0.16f && y >= barY && y < barY + barH) return 3;
    float c = 0.12f;
    return (x < c || x > cw - c) && (y < c || y > 1 - c) ? 2 : 0;
}

// How far from the screen's centre the ray crosses the screen's plane; -1 when it does not.
float reach(const V3& from, const V3& dir, const rig::Pose& center) {
    Q inv = center.q.inverse();
    V3 p = inv * (from - center.p), d = inv * dir;
    if (d.z > -1e-4f) return -1;
    V3 h = p + d * (-p.z / d.z);
    return std::sqrt(h.x * h.x + h.y * h.y);
}

// The plugin's UpdateHandles: takes the laser while it is on a handle or dragging one (true). The bar moves the screen
// anywhere around the player, a corner resizes it, RESET brings back 3 m ahead; while dragging, the right stick pushes it
// away or pulls it in (up, down) and sizes it (right, left).
bool update_handles(bool shown, rig::Result& r, const Surface& s, float aspect) {
    const PrxControllers& c = g_controllers;
    bool pull = c.rTrigger > 0.6f, fresh = pull && !g_handlePull;
    g_handlePull = pull;
    r.handles = shown || g_grab != 0;
    if (!r.handles || !c.aimValid[1]) {
        if (g_grab) save_screen();
        g_handle = g_grab = 0;
        g_screenLive = false;
        return false;
    }
    float u = -1, v = -1;
    V3 at, from = aim_from(c.aim[1]), dir = aim_dir(c.aim[1]);
    bool on = hit(s, from, dir, u, v, at);
    int over = on ? handle_at(u, v, aspect) : 0;
    if (!g_grab) {
        if (over != g_handle && over) prx_haptic_pulse(1, 0.2f, 0.015f);
        g_handle = over;
    }
    if (!g_handle) return false;
    r.beam = true;
    r.beamFrom = from;
    rig::Pose center{s.radius > 0 ? s.point(0.5f, 0.5f) : s.center, s.rot};
    if (fresh && !g_grab) {
        prx_haptic_pulse(1, 0.3f, 0.03f);
        if (g_handle == 3) {
            settings::set("View", "ScreenYaw", "0");
            settings::set("View", "ScreenPitch", "0");
            settings::set("View", "ScreenDistance", "3");
            settings::set("View", "ScreenWidth", "3.2");
            g_screenLive = false;
            screen_from_settings();
        } else {
            g_grab = g_handle;
            g_screenLive = true;
            // Moving keeps the screen where it sat relative to the ray; resizing scales with the corner's reach.
            g_grabFrom = look(dir).inverse() * (g_originYaw * yaw(-g_screenYaw * kRad) * pitch_q(g_screenPitch));
            g_grabWidth = g_screenWidth;
            g_grabReach = reach(from, dir, center);
        }
    } else if (!pull && g_grab) {
        g_grab = 0;
        g_screenLive = false;
        save_screen();
    }
    if (g_grab) {
        if (std::fabs(c.ry) > 0.2f) g_screenDistance = std::clamp(g_screenDistance * std::exp(c.ry * g_dt * 1.5f), kMinScreen, kMaxScreen);
        if (std::fabs(c.rx) > 0.2f) g_grabWidth = std::clamp(g_grabWidth * std::exp(c.rx * g_dt * 1.5f), kMinScreen, kMaxScreen);
        if (g_grab == 1) {
            V3 facing = g_originYaw.inverse() * ((look(dir) * g_grabFrom) * V3{0, 0, -1});
            g_screenYaw = heading_cw(facing);
            g_screenPitch = std::clamp(pitch_deg(facing), -60.f, 75.f);
            g_screenWidth = g_grabWidth;
        } else if (g_grabReach > 0.05f) {
            float now = reach(from, dir, center);
            if (now > 0) g_screenWidth = std::clamp(g_grabWidth * now / g_grabReach, kMinScreen, kMaxScreen);
        }
    }
    r.beamTo = on ? s.point(u, v) : from + dir;
    return true;
}

// The plugin's WindowEyes and UpdateFocus: the window into the game sits at the focus distance, as large as the flat
// view there; game units per metre of screen, times the depth setting, space the eyes, never wider than two
// eye-widths at the world scale. The focus (the adapter's rays) stops at 15 m and eases over about 0.7 s.
void window_eyes(const rig::GameFrame& g, rig::Result& r, const PrxPose views[2], float screenH, float aspect) {
    float s = rig::scale() * g_sceneScale, plane = g_sceneScale > 1 ? g_sceneScale * 2.5f : 0;
    float ipd = std::clamp((v_of(views[1]) - v_of(views[0])).length(), 0.05f, 0.08f);
    float nearest = std::max(g.nearClip * 4, 0.5f);
    // A 2D scene's play plane, else what the adapter's rays found.
    float target = plane > 0 ? plane : g.focus > 0 ? g.focus : g_focus;
    target = std::clamp(target, nearest, std::max({nearest, 15.f * s, plane}));
    g_focus += (target - g_focus) * (1 - std::exp(-g_dt * 1.5f));
    float gh = 2 * g_focus * std::tan(g.fovY / 2), gw = gh * aspect;
    float k = std::min(gh / screenH, 2.f * s) * clamp01(settings::getf("View", "Depth", 1, 0, 1));
    for (int e = 0; e < 2; ++e) {
        float x = (e == 0 ? -ipd : ipd) / 2 * k;
        r.windowOffset[e] = x;
        r.windowFov[e] = {std::atan((-gw / 2 - x) / g_focus), std::atan((gw / 2 - x) / g_focus), std::atan(gh / 2 / g_focus),
                          std::atan(-gh / 2 / g_focus)};
    }
}

V3 smoothed(V3 pos) {
    if ((pos - g_stepTo).dot(pos - g_stepTo) > 0) {
        g_stepFrom = (pos - g_stepTo).dot(pos - g_stepTo) > 4.f ? pos : g_stepTo;  // a jump: no line to it
        g_stepLen = std::clamp(g_now - g_stepAt, 0.005f, 0.1f);
        g_stepTo = pos;
        g_stepAt = g_lastMoveAt = g_now;
        ++g_stepMoved;
    } else if (g_now - g_lastMoveAt < 0.1f) ++g_stepStill;
    // Judged every 90 frames: a fifth of the moving frames standing still turns it on, a twentieth off.
    if (++g_stepFrames >= 90) {
        float still = g_stepMoved > 20 ? (float)g_stepStill / (g_stepMoved + g_stepStill) : 0;
        if (still > 0.2f) g_stepSmoothing = true;
        else if (still < 0.05f) g_stepSmoothing = false;
        g_stepFrames = g_stepMoved = g_stepStill = 0;
    }
    if (!g_stepSmoothing) return pos;
    float t = clamp01((g_now - g_stepAt) / g_stepLen);
    return g_stepFrom + (g_stepTo - g_stepFrom) * t;
}
}  // namespace

namespace rig {
void recenter() { g_recenterPending = true; }

float g_testPointU = -1, g_testPointV;
bool g_testPointScreen;

void set_test_point(float u, float v, bool screen) {
    g_testPointU = u, g_testPointV = v, g_testPointScreen = screen;
}

void set_test_wrist(bool on) { g_testWrist = on; }

void set_test_controllers(const PrxControllers* c) {
    g_testOn = c != nullptr;
    if (c) g_test = *c;
}

float scale() { return settings::getf("View", "WorldScale", 1.f, 0.5f, 3.f); }

Result frame(const GameFrame& game, const PrxPose views[2]) {
    LARGE_INTEGER tick, freq;
    QueryPerformanceCounter(&tick);
    QueryPerformanceFrequency(&freq);
    g_dt = g_lastTick.QuadPart ? std::min(0.1f, (float)(tick.QuadPart - g_lastTick.QuadPart) / freq.QuadPart) : 1 / 90.f;
    g_lastTick = tick;
    g_now = (float)((double)tick.QuadPart / freq.QuadPart);
    g_lastViews[0] = views[0], g_lastViews[1] = views[1];
    if (g_recenterPending) {
        g_originPos = (v_of(views[0]) + v_of(views[1])) * 0.5f;
        g_originYaw = yaw(heading(q_of(views[0])));
        g_recenterPending = false;
    }
    if (game.cameraKey != g_orbitKey) {
        // Another camera: its turns are judged afresh.
        g_orbitKey = game.cameraKey;
        g_orbitSamples = 0;
        g_orbitNum = g_orbitDen = 0;
        g_orbitLastFwd = {};
        g_probeIgnored = 0;
        g_ignoredTimes = 0;
    }
    g_sceneScale = std::max(1.f, game.sceneScale);
    g_handAiming = false;
    if (game.hasCamera) {
        float n = game.up.length();
        if (n > 1e-3f) g_upFrame = from_to(g_upFrame * V3{0, 1, 0}, game.up * (1 / n)) * g_upFrame;
        track_body(game.camera);
        sample_orbit(game.camera);
    }

    // Controllers: the same emulation as the Unity plugin, configured from the settings.
    int mode = input_mode(game.padGame);
    PrxInputConfig cfg{};
    cfg.mode = mode;
    // Game input pauses while the VR menu, the hide-objects tool or a screen handle has the controllers.
    cfg.neutral = game.menuOpen || game.worldPointer || g_handle || g_grab ? 1 : 0;
    cfg.mouseSpeed = settings::getf("Controls", "MouseSpeed", 1200.f, 100.f, 6000.f);
    // The pointer's click reaches the game through the adapter (the engine's own UI events), never the OS mouse.
    // Engines whose UI follows the OS mouse (Unity) also get the OS cursor moved under the laser and, unless their own
    // UI events took the click, the OS button; not on the VR menu, which is ours.
    cfg.pointerClick = game.pointerClick && !game.menuOpen ? 1 : 0;
    bool osCursor = g_pointerOn && !game.menuOpen && game.screenW > 0 && game.screenH > 0;
    cfg.pointerX = osCursor ? (int)std::lround(g_pointerU * game.screenW) : -1;
    cfg.pointerY = osCursor ? (int)std::lround(g_pointerV * game.screenH) : -1;
    // While it points at the panel, the right trigger is the pointer's, not the game's.
    cfg.pointerActive = g_pointerOn ? 1 : 0;
    // Display: Parallax needs a game camera; without one (or when asked) the flat game on the Theater screen.
    std::string display = setting("View", "Display", "Stereo");
    int view = display == "Theater" || !game.hasCamera ? rig::Theater : display == "Parallax" ? rig::Parallax : rig::Stereo;
    {
        static bool asked = false;
        if (!asked) {
            PrxStats st{};
            prx_get_stats(&st);
            g_curvedRuntime = st.curvedSupported != 0;
            asked = st.state >= StateSession;
        }
    }
    g_aiming = view == rig::Stereo && aim_wanted(game, mode);
    if (g_aiming) {
        cfg.snap = 1;
        cfg.aim = 1;
        head_aim(game, mode == 0);
        cfg.turnX = g_aimPushX;
        cfg.turnY = g_aimPushY;
    } else if (view == rig::Stereo && setting("Comfort", "Turning", "Smooth") != "Smooth" && mode != 2 && game.hasCamera && !game.menuOpen &&
               (g_snapping || (g_orbitSamples >= 10 && orbit_radius() < 0.45f))) {
        // Snap turns are for a camera that turns about itself; a third-person camera keeps the right stick as is.
        cfg.snap = 1;
        cfg.turnX = snap_turn(game.camera, mode == 0);
    }
    bindings(cfg);
    Result r;
    if (g_testOn && g_testPointU >= 0) {
        float pw = 1;
        Pose p = panel(pw);
        float ph = pw / (game.uiAspect > 0 ? game.uiAspect : 16.f / 9.f);
        if (game.menuOpen && !g_testPointScreen) p = game.menuPose, pw = game.menuWidth, ph = game.menuHeight;
        V3 target = p.p + p.q * V3{(g_testPointU - 0.5f) * pw, (0.5f - g_testPointV) * ph, 0};
        std::string shown = setting("View", "Display", "Stereo");
        if ((!game.menuOpen || g_testPointScreen) && (shown != "Stereo" || !game.hasCamera)) {
            rig::Screen sc = place_screen(shown == "Parallax" && game.hasCamera, game.uiAspect > 0 ? game.uiAspect : 16.f / 9.f);
            target = surface_of(sc).point(g_testPointU, g_testPointV);
        }
        V3 from = (v_of(views[0]) + v_of(views[1])) * 0.5f, d = target - from;
        Q q = look(d * (1 / d.length()));
        g_test.aim[1] = {from.x, from.y, from.z, q.x, q.y, q.z, q.w};
        g_test.aimValid[1] = 1;
    }
    r.events = prx_input_update(&cfg, g_testOn ? &g_test : nullptr, &g_controllers);
    r.controllers = g_controllers;
    r.aiming = g_aiming;
    if (game.hasCamera) g_camPos = smoothed(game.camera.p);
    update_vignette(game.hasCamera && view == rig::Stereo);
    r.vignette = view == rig::Stereo ? g_vignette : 0;
    r.mode = view;
    float aspect = game.uiAspect > 0 ? game.uiAspect : 16.f / 9.f;
    Surface surface;
    if (view == rig::Stereo) {
        float w = 1;
        Pose at = panel(w);
        surface = {at.p, at.q, w, w / aspect, 0};
    } else {
        r.screen = place_screen(view == rig::Parallax, aspect);
        surface = surface_of(r.screen);
        if (view == rig::Parallax && game.hasCamera) window_eyes(game, r, views, r.screen.height, aspect);
    }
    // Handles first: on them the laser is theirs (shown with the menu open and the screen unlocked).
    bool onHandles = view != rig::Stereo && update_handles(game.menuOpen && game.screenUnlocked, r, surface, aspect);
    r.handle = g_handle, r.grab = g_grab, r.screenDistance = g_screenDistance, r.screenWidth = g_screenWidth;
    if (onHandles) {
        g_pointerOn = false;
        r.pointer = {};
    } else {
        update_pointer(game, r, surface);
    }
    if (update_watch(game, r, views)) r.events |= EvMenuToggle;

    // The eyes: the headset's movement since recentring, turned with the body, scaled, from the game camera.
    Q b = body(), unYaw = g_originYaw.inverse();
    float s = scale() * g_sceneScale;
    V3 lift{0, settings::getf("View", "Height", 0.f, -1.f, 1.f), 0};
    for (int e = 0; e < 2; ++e) {
        V3 rel = unYaw * (v_of(views[e]) - g_originPos);
        r.eyes[e].p = g_camPos + b * ((rel + game.eyeOffset) * s) + lift;
        r.eyes[e].q = b * unYaw * q_of(views[e]);
    }
    r.origin = {g_originPos, g_originYaw};
    r.body = b;
    r.base = g_camPos + lift;
    r.scale = s;
    r.handAiming = g_handAiming;
    r.handAim = g_handAim;
    r.screenYaw = g_screenYaw, r.screenPitch = g_screenPitch;
    r.bodyYaw = g_bodyYaw;
    r.orbitRadius = orbit_radius();
    r.focus = g_focus;
    r.stepSmoothing = g_stepSmoothing;
    r.glancing = g_glancing;
    return r;
}

Pose panel(float& width) {
    width = settings::getf("View", "UiWidth", 1.6f, 0.5f, 4.f);
    float distance = settings::getf("View", "UiDistance", 1.2f, 0.5f, 3.f);
    return {g_originPos + g_originYaw * V3{0, -kUiDrop, -distance}, g_originYaw};
}
}  // namespace rig
