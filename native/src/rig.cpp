#include "rig.h"
#include "settings.h"
#include <algorithm>
#include <cmath>
#include <map>

extern "C" PRX_API uint32_t prx_input_update(const PrxInputConfig* cfg, const PrxControllers* test, PrxControllers* out);

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
PrxControllers g_test{};

float clamp01(float v) { return std::clamp(v, 0.f, 1.f); }
float sign(float v) { return v < 0 ? -1.f : 1.f; }
float delta_angle(float from, float to) { return wrap((to - from) * kRad) * kDeg; }
V3 forward(const Q& q) { return q * V3{0, 0, -1}; }
// Clockwise heading and pitch in degrees, as the player sees them.
float heading_cw(const V3& f) { return std::atan2(f.x, -f.z) * kDeg; }
float pitch_deg(const V3& f) { return std::asin(std::clamp(f.y, -1.f, 1.f)) * kDeg; }
Q body() { return yaw(-g_bodyYaw * kRad); }
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
    float h = heading_cw(forward(cam.q));
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
    if (aim == "Off" || mode == 2 || !g.hasCamera || !g_bodyKnown || g.menuOpen) return false;
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
    Q aimQ = hand ? Q{g_controllers.aim[1].qx, g_controllers.aim[1].qy, g_controllers.aim[1].qz, g_controllers.aim[1].qw} : q_of(g_lastViews[0]);
    V3 look = g_originYaw.inverse() * forward(aimQ);
    float wantYaw = g_bodyYaw + heading_cw(look);
    float wantPitch = std::clamp(pitch_deg(look), -80.f, 80.f);
    V3 f = forward(g.camera.q);
    float yaw = heading_cw(f), pitch = pitch_deg(f);
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
    float yaw = heading_cw(forward(cam.q)), rx = g_controllers.rx;
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
    }
    if (game.hasCamera) {
        track_body(game.camera);
        sample_orbit(game.camera);
    }

    // Controllers: the same emulation as the Unity plugin, configured from the settings.
    int mode = input_mode(game.padGame);
    PrxInputConfig cfg{};
    cfg.mode = mode;
    cfg.neutral = game.menuOpen ? 1 : 0;
    cfg.mouseSpeed = settings::getf("Controls", "MouseSpeed", 1200.f, 100.f, 6000.f);
    cfg.pointerClick = 1;
    g_aiming = aim_wanted(game, mode);
    if (g_aiming) {
        cfg.snap = 1;
        cfg.aim = 1;
        head_aim(game, mode == 0);
        cfg.turnX = g_aimPushX;
        cfg.turnY = g_aimPushY;
    } else if (setting("Comfort", "Turning", "Smooth") != "Smooth" && mode != 2 && game.hasCamera && !game.menuOpen &&
               (g_snapping || (g_orbitSamples >= 10 && orbit_radius() < 0.45f))) {
        // Snap turns are for a camera that turns about itself; a third-person camera keeps the right stick as is.
        cfg.snap = 1;
        cfg.turnX = snap_turn(game.camera, mode == 0);
    }
    bindings(cfg);
    Result r;
    r.events = prx_input_update(&cfg, g_testOn ? &g_test : nullptr, &g_controllers);
    r.controllers = g_controllers;
    r.aiming = g_aiming;
    if (game.hasCamera) g_camPos = smoothed(game.camera.p);
    update_vignette(game.hasCamera);
    r.vignette = g_vignette;

    // The eyes: the headset's movement since recentring, turned with the body, scaled, from the game camera.
    Q b = body(), unYaw = g_originYaw.inverse();
    float s = scale();
    V3 lift{0, settings::getf("View", "Height", 0.f, -1.f, 1.f), 0};
    for (int e = 0; e < 2; ++e) {
        V3 rel = unYaw * (v_of(views[e]) - g_originPos);
        r.eyes[e].p = g_camPos + b * (rel * s) + lift;
        r.eyes[e].q = b * unYaw * q_of(views[e]);
    }
    return r;
}

Pose panel(float& width) {
    width = settings::getf("View", "UiWidth", 1.6f, 0.5f, 4.f);
    float distance = settings::getf("View", "UiDistance", 1.2f, 0.5f, 3.f);
    return {g_originPos + g_originYaw * V3{0, -kUiDrop, -distance}, g_originYaw};
}
}  // namespace rig
