#include "common.h"
#include "gesture.h"
#include <MinHook.h>
#include <Xinput.h>
#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace {
XrActionSet g_set;
XrAction g_stick, g_trigger, g_grip, g_primary, g_secondary, g_thumb, g_menu, g_aim, g_haptic;
XrPath g_hands[2];
XrSpace g_aimSpace[2];
bool g_attached;

std::mutex g_padMutex;
XINPUT_STATE g_pad{};
bool g_padActive;
WORD g_vibration[2];
bool g_hapticOn[2];

// Menu button: tap -> Start (delayed so a double tap can claim it), double tap -> VR menu, hold -> Back.
constexpr ULONGLONG kDoubleTapMs = 300, kHoldMs = 700, kPulseMs = 120;
bool g_menuDown, g_menuConsumed, g_menuHeld, g_tapPending;
ULONGLONG g_menuPressAt, g_menuReleaseAt, g_startUntil;

bool g_gripDown[2];
Gestures g_gestures;
float g_mouseCarry[2];
LARGE_INTEGER g_lastUpdate;
bool g_pointerDown;
int g_lastPointer[2] = {-1, -1};
HWND g_window;
bool g_windowSet;  // the Theater host names the game window; the plugin finds its own

XrPath path(const char* s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(g_xr.instance, s, &p);
    return p;
}

XrAction make_action(const char* name, const char* label, XrActionType type) {
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    strcpy_s(ci.actionName, name);
    strcpy_s(ci.localizedActionName, label);
    ci.actionType = type;
    ci.countSubactionPaths = 2;
    ci.subactionPaths = g_hands;
    XrAction a = XR_NULL_HANDLE;
    xr_ok(xrCreateAction(g_set, &ci, &a), name);
    return a;
}

struct Binding {
    XrAction* action;
    const char* path;
};

void suggest(const char* profile, std::initializer_list<Binding> bindings) {
    std::vector<XrActionSuggestedBinding> list;
    for (auto& b : bindings) list.push_back({*b.action, path(b.path)});
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = path(profile);
    sb.countSuggestedBindings = (uint32_t)list.size();
    sb.suggestedBindings = list.data();
    xr_ok(xrSuggestInteractionProfileBindings(g_xr.instance, &sb), profile);
}

bool get_bool(XrAction a, int hand) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, a, g_hands[hand]};
    XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(g_xr.session, &gi, &s)) && s.isActive && s.currentState;
}

float get_float(XrAction a, int hand) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, a, g_hands[hand]};
    XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(xrGetActionStateFloat(g_xr.session, &gi, &s)) && s.isActive ? s.currentState : 0.f;
}

XrVector2f get_vec2(XrAction a, int hand) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, a, g_hands[hand]};
    XrActionStateVector2f s{XR_TYPE_ACTION_STATE_VECTOR2F};
    return XR_SUCCEEDED(xrGetActionStateVector2f(g_xr.session, &gi, &s)) && s.isActive ? s.currentState
                                                                                      : XrVector2f{0, 0};
}

void read_actions(PrxControllers& c) {
    XrActiveActionSet active{g_set, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
    if (XR_FAILED(xrSyncActions(g_xr.session, &si))) return;
    auto l = get_vec2(g_stick, 0), r = get_vec2(g_stick, 1);
    c.lx = l.x, c.ly = l.y, c.rx = r.x, c.ry = r.y;
    c.lTrigger = get_float(g_trigger, 0), c.rTrigger = get_float(g_trigger, 1);
    c.lGrip = get_float(g_grip, 0), c.rGrip = get_float(g_grip, 1);
    c.buttons = (get_bool(g_primary, 1) ? BtnA : 0) | (get_bool(g_secondary, 1) ? BtnB : 0) |
                (get_bool(g_primary, 0) ? BtnX : 0) | (get_bool(g_secondary, 0) ? BtnY : 0) |
                (get_bool(g_thumb, 0) ? BtnLStick : 0) | (get_bool(g_thumb, 1) ? BtnRStick : 0) |
                (get_bool(g_menu, 0) ? BtnMenu : 0);
    for (int h = 0; h < 2; ++h) {
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(g_aimSpace[h], g_xr.local, g_xr.lastPredicted + g_xr.period, &loc))) {
            auto& p = loc.pose;
            c.aim[h] = {p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z,
                        p.orientation.w};
            c.aimValid[h] = (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
                            (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
        }
    }
}

uint32_t update_menu_button(bool down, ULONGLONG now) {
    uint32_t events = 0;
    if (down && !g_menuDown) {
        if (g_tapPending && now - g_menuReleaseAt < kDoubleTapMs) {
            g_tapPending = false;
            g_menuConsumed = true;
            events |= EvMenuToggle;
        } else {
            g_menuPressAt = now;
        }
    } else if (down && !g_menuConsumed && !g_menuHeld && now - g_menuPressAt >= kHoldMs) {
        g_menuHeld = true;
    } else if (!down && g_menuDown) {
        if (!g_menuConsumed && !g_menuHeld) {
            g_tapPending = true;
            g_menuReleaseAt = now;
        }
        g_menuConsumed = g_menuHeld = false;
    }
    if (g_tapPending && now - g_menuReleaseAt >= kDoubleTapMs) {
        g_tapPending = false;
        g_startUntil = now + kPulseMs;
    }
    g_menuDown = down;
    return events;
}

bool hysteresis(bool& state, float v) {
    state = state ? v > 0.4f : v > 0.6f;
    return state;
}

SHORT axis(float v) { return (SHORT)(std::fmax(-1.f, std::fmin(1.f, v)) * 32767); }

BOOL CALLBACK find_window(HWND hwnd, LPARAM) {
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    char cls[32];
    if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && GetClassNameA(hwnd, cls, sizeof(cls)) &&
        strcmp(cls, "UnityWndClass") == 0) {
        g_window = hwnd;
        return FALSE;
    }
    return TRUE;
}

HWND game_window() {
    if (!g_window || !IsWindow(g_window)) {
        g_window = nullptr;
        if (!g_windowSet) EnumWindows(find_window, 0);
    }
    return g_window;
}

bool game_in_front(HWND hwnd) { return hwnd && GetForegroundWindow() == hwnd && !IsIconic(hwnd); }

// What the emulation currently holds down: scan codes (0x100 | code = extended) and the three mouse buttons.
std::bitset<0x200> g_keysSent;
bool g_mouseSent[3];

void send_key(int code, bool down) {
    INPUT in{INPUT_KEYBOARD};
    in.ki.wScan = (WORD)(code & 0xFF);
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (code & 0x100 ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof(in));
    g_keysSent[code] = down;
}

void send_mouse_button(int button, bool down) {
    static const DWORD flags[3][2] = {{MOUSEEVENTF_LEFTUP, MOUSEEVENTF_LEFTDOWN},
                                      {MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_RIGHTDOWN},
                                      {MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_MIDDLEDOWN}};
    if (g_mouseSent[button] == down) return;
    INPUT in{INPUT_MOUSE};
    in.mi.dwFlags = flags[button][down];
    SendInput(1, &in, sizeof(in));
    g_mouseSent[button] = down;
}

void update_keyboard_mouse(const PrxControllers& c, const PrxInputConfig& cfg, bool startTap, double dt) {
    HWND hwnd = game_window();
    bool on = cfg.mode == 1 && !cfg.neutral && hwnd && GetForegroundWindow() == hwnd;
    std::bitset<0x200> keys;
    bool mouse[3] = {};
    if (on) {
        keys[0x11] = c.ly > 0.5f, keys[0x1F] = c.ly < -0.5f, keys[0x1E] = c.lx < -0.5f, keys[0x20] = c.lx > 0.5f;
        // While the laser pointer is on the game window, the right trigger clicks there instead of its binding.
        const bool pressed[kGestureInputs] = {
            (c.buttons & BtnA) != 0,     (c.buttons & BtnB) != 0,     (c.buttons & BtnX) != 0,
            (c.buttons & BtnY) != 0,     g_gripDown[0],               g_gripDown[1],
            (c.buttons & BtnLStick) != 0, (c.buttons & BtnRStick) != 0, c.lTrigger > 0.5f,
            c.rTrigger > 0.5f && !cfg.pointerActive};
        auto hold = [&](int code) {
            if (code >= BindMouse && code < BindMouse + 3) mouse[code - BindMouse] = true;
            else if (code > 0 && code < 0x200) keys[code] = true;
        };
        GestureBinds gb;
        std::memcpy(gb.press, cfg.binds, sizeof(gb.press));
        std::memcpy(gb.twice, cfg.twice, sizeof(gb.twice));
        std::memcpy(gb.held, cfg.held, sizeof(gb.held));
        std::memcpy(gb.combo, cfg.combo, sizeof(gb.combo));
        g_gestures.update(pressed, gb, GetTickCount64(), hold);
        if (startTap) hold(cfg.binds[BindMenuTap]);
        if (g_menuHeld) hold(cfg.binds[BindMenuHold]);
        float speed = cfg.mouseSpeed * (float)dt;
        g_mouseCarry[0] += cfg.snap ? cfg.turnX : c.rx * std::fabs(c.rx) * speed;
        g_mouseCarry[1] -= cfg.aim ? cfg.turnY : c.ry * std::fabs(c.ry) * speed;
        int dx = (int)g_mouseCarry[0], dy = (int)g_mouseCarry[1];
        g_mouseCarry[0] -= dx, g_mouseCarry[1] -= dy;
        if (dx || dy) {
            INPUT in{INPUT_MOUSE};
            in.mi.dx = dx;
            in.mi.dy = dy;
            in.mi.dwFlags = MOUSEEVENTF_MOVE;
            SendInput(1, &in, sizeof(in));
        }
    }
    if (cfg.pointerActive && cfg.pointerClick && g_pointerDown && game_in_front(hwnd)) mouse[0] = true;
    for (int k = 1; k < 0x200; ++k)
        if (keys[k] != g_keysSent[k]) send_key(k, keys[k]);
    for (int b = 0; b < 3; ++b) send_mouse_button(b, mouse[b]);
}

void update_pointer(const PrxControllers& c, const PrxInputConfig& cfg) {
    bool down = cfg.pointerActive && hysteresis(g_pointerDown, c.rTrigger);
    g_pointerDown = down;
    HWND hwnd = game_window();
    // Behind other windows or minimized, the OS cursor would point (and click) at whatever else is there.
    if (!cfg.pointerActive || !game_in_front(hwnd)) {
        g_lastPointer[0] = -1;
        return;
    }
    // -1: the engine takes the pointer through its own events (or it is on the VR menu); the OS cursor stays.
    if (cfg.pointerX < 0 || (cfg.pointerX == g_lastPointer[0] && cfg.pointerY == g_lastPointer[1])) return;
    POINT p{cfg.pointerX, cfg.pointerY};
    ClientToScreen(hwnd, &p);
    SetCursorPos(p.x, p.y);
    g_lastPointer[0] = cfg.pointerX, g_lastPointer[1] = cfg.pointerY;
}

void update_pad(const PrxControllers& c, const PrxInputConfig& cfg, bool start) {
    XINPUT_GAMEPAD gp{};
    if (!cfg.neutral) {
        gp.sThumbLX = axis(c.lx), gp.sThumbLY = axis(c.ly), gp.sThumbRX = axis(cfg.snap ? cfg.turnX : c.rx), gp.sThumbRY = axis(cfg.aim ? cfg.turnY : c.ry);
        gp.bLeftTrigger = (BYTE)(c.lTrigger * 255);
        gp.bRightTrigger = cfg.pointerActive ? 0 : (BYTE)(c.rTrigger * 255);
        WORD b = 0;
        if (c.buttons & BtnA) b |= XINPUT_GAMEPAD_A;
        if (c.buttons & BtnB) b |= XINPUT_GAMEPAD_B;
        if (c.buttons & BtnX) b |= XINPUT_GAMEPAD_X;
        if (c.buttons & BtnY) b |= XINPUT_GAMEPAD_Y;
        if (c.buttons & BtnLStick) b |= XINPUT_GAMEPAD_LEFT_THUMB;
        if (c.buttons & BtnRStick) b |= XINPUT_GAMEPAD_RIGHT_THUMB;
        if (g_gripDown[0]) b |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        if (g_gripDown[1]) b |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        if (start) b |= XINPUT_GAMEPAD_START;
        if (g_menuHeld) b |= XINPUT_GAMEPAD_BACK;
        gp.wButtons = b;
    }
    std::lock_guard lock(g_padMutex);
    g_padActive = cfg.mode == 0;
    if (memcmp(&gp, &g_pad.Gamepad, sizeof(gp)) != 0) {
        g_pad.Gamepad = gp;
        ++g_pad.dwPacketNumber;
    }
}

void haptic(int hand, float amplitude, XrDuration duration) {
    if (!g_attached || g_xr.state != StateRunning) return;
    XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO, nullptr, g_haptic, g_hands[hand]};
    if (amplitude <= 0.01f) {
        xrStopHapticFeedback(g_xr.session, &hi);
        return;
    }
    XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION, nullptr, duration, XR_FREQUENCY_UNSPECIFIED, amplitude};
    xrApplyHapticFeedback(g_xr.session, &hi, (XrHapticBaseHeader*)&v);
}

void update_rumble() {
    WORD vib[2];
    {
        std::lock_guard lock(g_padMutex);
        vib[0] = g_vibration[0], vib[1] = g_vibration[1];
    }
    for (int h = 0; h < 2; ++h) {
        float amp = vib[h] / 65535.f;
        if (amp > 0.01f) {
            haptic(h, amp, 100'000'000);
            g_hapticOn[h] = true;
        } else if (g_hapticOn[h]) {
            haptic(h, 0, 0);
            g_hapticOn[h] = false;
        }
    }
}

// XInput hooks. One detour instance per hooked export so each keeps its own trampoline.
using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using SetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
using GetCapsFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
constexpr int kMaxHooks = 8;
GetStateFn g_origGetState[kMaxHooks];
SetStateFn g_origSetState[kMaxHooks];
GetCapsFn g_origGetCaps[kMaxHooks];

template <int N>
DWORD WINAPI hook_get_state(DWORD user, XINPUT_STATE* state) {
    {
        std::lock_guard lock(g_padMutex);
        if (user == 0 && g_padActive && state) {
            *state = g_pad;
            return ERROR_SUCCESS;
        }
    }
    return g_origGetState[N](user, state);
}

template <int N>
DWORD WINAPI hook_set_state(DWORD user, XINPUT_VIBRATION* vibration) {
    {
        std::lock_guard lock(g_padMutex);
        if (user == 0 && g_padActive && vibration) {
            g_vibration[0] = vibration->wLeftMotorSpeed;
            g_vibration[1] = vibration->wRightMotorSpeed;
            return ERROR_SUCCESS;
        }
    }
    return g_origSetState[N](user, vibration);
}

template <int N>
DWORD WINAPI hook_get_caps(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps) {
    {
        std::lock_guard lock(g_padMutex);
        if (user == 0 && g_padActive && caps) {
            *caps = {};
            caps->Type = XINPUT_DEVTYPE_GAMEPAD;
            caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
            caps->Gamepad = {0xF3FF, 255, 255, 32767, 32767, 32767, 32767};
            caps->Vibration = {65535, 65535};
            return ERROR_SUCCESS;
        }
    }
    return g_origGetCaps[N](user, flags, caps);
}

template <size_t... I>
constexpr auto make_table(std::index_sequence<I...>) {
    struct Row {
        GetStateFn get;
        SetStateFn set;
        GetCapsFn caps;
    };
    return std::array<Row, sizeof...(I)>{Row{&hook_get_state<I>, &hook_set_state<I>, &hook_get_caps<I>}...};
}
const auto g_hookTable = make_table(std::make_index_sequence<kMaxHooks>{});

template <class Fn>
bool hook(void* target, Fn detour, Fn* original, std::vector<void*>& done) {
    if (!target || std::find(done.begin(), done.end(), target) != done.end()) return false;
    done.push_back(target);
    return MH_CreateHook(target, (void*)detour, (void**)original) == MH_OK;
}
}  // namespace

bool input_create_actions() {
    g_hands[0] = path("/user/hand/left");
    g_hands[1] = path("/user/hand/right");
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(si.actionSetName, "gameplay");
    strcpy_s(si.localizedActionSetName, "Gameplay");
    if (!xr_ok(xrCreateActionSet(g_xr.instance, &si, &g_set), "xrCreateActionSet")) return false;
    g_stick = make_action("stick", "Move / turn", XR_ACTION_TYPE_VECTOR2F_INPUT);
    g_trigger = make_action("trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT);
    g_grip = make_action("grip", "Grip", XR_ACTION_TYPE_FLOAT_INPUT);
    g_primary = make_action("primary", "A / X", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_secondary = make_action("secondary", "B / Y", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_thumb = make_action("thumb", "Stick click", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_menu = make_action("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_aim = make_action("aim", "Pointer", XR_ACTION_TYPE_POSE_INPUT);
    g_haptic = make_action("haptic", "Rumble", XR_ACTION_TYPE_VIBRATION_OUTPUT);

    suggest("/interaction_profiles/oculus/touch_controller",
            {{&g_stick, "/user/hand/left/input/thumbstick"}, {&g_stick, "/user/hand/right/input/thumbstick"},
             {&g_trigger, "/user/hand/left/input/trigger/value"}, {&g_trigger, "/user/hand/right/input/trigger/value"},
             {&g_grip, "/user/hand/left/input/squeeze/value"}, {&g_grip, "/user/hand/right/input/squeeze/value"},
             {&g_primary, "/user/hand/left/input/x/click"}, {&g_primary, "/user/hand/right/input/a/click"},
             {&g_secondary, "/user/hand/left/input/y/click"}, {&g_secondary, "/user/hand/right/input/b/click"},
             {&g_thumb, "/user/hand/left/input/thumbstick/click"}, {&g_thumb, "/user/hand/right/input/thumbstick/click"},
             {&g_menu, "/user/hand/left/input/menu/click"},
             {&g_aim, "/user/hand/left/input/aim/pose"}, {&g_aim, "/user/hand/right/input/aim/pose"},
             {&g_haptic, "/user/hand/left/output/haptic"}, {&g_haptic, "/user/hand/right/output/haptic"}});
    // Index has no menu button for apps; the left trackpad press stands in.
    suggest("/interaction_profiles/valve/index_controller",
            {{&g_stick, "/user/hand/left/input/thumbstick"}, {&g_stick, "/user/hand/right/input/thumbstick"},
             {&g_trigger, "/user/hand/left/input/trigger/value"}, {&g_trigger, "/user/hand/right/input/trigger/value"},
             {&g_grip, "/user/hand/left/input/squeeze/value"}, {&g_grip, "/user/hand/right/input/squeeze/value"},
             {&g_primary, "/user/hand/left/input/a/click"}, {&g_primary, "/user/hand/right/input/a/click"},
             {&g_secondary, "/user/hand/left/input/b/click"}, {&g_secondary, "/user/hand/right/input/b/click"},
             {&g_thumb, "/user/hand/left/input/thumbstick/click"}, {&g_thumb, "/user/hand/right/input/thumbstick/click"},
             {&g_menu, "/user/hand/left/input/trackpad/force"},
             {&g_aim, "/user/hand/left/input/aim/pose"}, {&g_aim, "/user/hand/right/input/aim/pose"},
             {&g_haptic, "/user/hand/left/output/haptic"}, {&g_haptic, "/user/hand/right/output/haptic"}});
    suggest("/interaction_profiles/htc/vive_controller",
            {{&g_stick, "/user/hand/left/input/trackpad"}, {&g_stick, "/user/hand/right/input/trackpad"},
             {&g_trigger, "/user/hand/left/input/trigger/value"}, {&g_trigger, "/user/hand/right/input/trigger/value"},
             {&g_grip, "/user/hand/left/input/squeeze/click"}, {&g_grip, "/user/hand/right/input/squeeze/click"},
             {&g_primary, "/user/hand/right/input/trackpad/click"}, {&g_secondary, "/user/hand/right/input/menu/click"},
             {&g_thumb, "/user/hand/left/input/trackpad/click"}, {&g_menu, "/user/hand/left/input/menu/click"},
             {&g_aim, "/user/hand/left/input/aim/pose"}, {&g_aim, "/user/hand/right/input/aim/pose"},
             {&g_haptic, "/user/hand/left/output/haptic"}, {&g_haptic, "/user/hand/right/output/haptic"}});
    suggest("/interaction_profiles/microsoft/motion_controller",
            {{&g_stick, "/user/hand/left/input/thumbstick"}, {&g_stick, "/user/hand/right/input/thumbstick"},
             {&g_trigger, "/user/hand/left/input/trigger/value"}, {&g_trigger, "/user/hand/right/input/trigger/value"},
             {&g_grip, "/user/hand/left/input/squeeze/click"}, {&g_grip, "/user/hand/right/input/squeeze/click"},
             {&g_primary, "/user/hand/left/input/trackpad/click"}, {&g_primary, "/user/hand/right/input/trackpad/click"},
             {&g_secondary, "/user/hand/right/input/menu/click"},
             {&g_thumb, "/user/hand/left/input/thumbstick/click"}, {&g_thumb, "/user/hand/right/input/thumbstick/click"},
             {&g_menu, "/user/hand/left/input/menu/click"},
             {&g_aim, "/user/hand/left/input/aim/pose"}, {&g_aim, "/user/hand/right/input/aim/pose"},
             {&g_haptic, "/user/hand/left/output/haptic"}, {&g_haptic, "/user/hand/right/output/haptic"}});
    suggest("/interaction_profiles/khr/simple_controller",
            {{&g_trigger, "/user/hand/left/input/select/click"}, {&g_trigger, "/user/hand/right/input/select/click"},
             {&g_menu, "/user/hand/left/input/menu/click"},
             {&g_aim, "/user/hand/left/input/aim/pose"}, {&g_aim, "/user/hand/right/input/aim/pose"},
             {&g_haptic, "/user/hand/left/output/haptic"}, {&g_haptic, "/user/hand/right/output/haptic"}});
    return true;
}

bool input_attach_session() {
    if (!g_set) return false;
    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1, &g_set};
    if (!xr_ok(xrAttachSessionActionSets(g_xr.session, &ai), "xrAttachSessionActionSets")) return false;
    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo ci{XR_TYPE_ACTION_SPACE_CREATE_INFO, nullptr, g_aim, g_hands[h], {{0, 0, 0, 1}, {0, 0, 0}}};
        xr_ok(xrCreateActionSpace(g_xr.session, &ci, &g_aimSpace[h]), "aim space");
    }
    g_attached = true;
    return true;
}

void input_shutdown() {
    g_attached = false;
    for (auto& s : g_aimSpace) {
        if (s) xrDestroySpace(s);
        s = XR_NULL_HANDLE;
    }
    std::lock_guard lock(g_padMutex);
    g_padActive = false;
}

PRX_API int32_t prx_install_hooks() {
    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return 0;
    std::vector<void*> done;
    int slot = 0;
    for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", "xinput1_2.dll", "xinput1_1.dll"}) {
        HMODULE m = LoadLibraryA(dll);
        if (!m || slot >= kMaxHooks) continue;
        const auto& row = g_hookTable[slot];
        bool any = hook(GetProcAddress(m, "XInputGetState"), row.get, &g_origGetState[slot], done);
        any |= hook(GetProcAddress(m, "XInputSetState"), row.set, &g_origSetState[slot], done);
        any |= hook(GetProcAddress(m, "XInputGetCapabilities"), row.caps, &g_origGetCaps[slot], done);
        if (any) ++slot;
        // Ordinal 100 is the undocumented GetStateEx (adds the guide button); same signature as GetState.
        if (slot < kMaxHooks && hook(GetProcAddress(m, (LPCSTR)100), g_hookTable[slot].get, &g_origGetState[slot], done))
            ++slot;
    }
    MH_EnableHook(MH_ALL_HOOKS);
    prx_log("xinput hooks: %d", slot);
    return slot;
}

PRX_API uint32_t prx_input_update(const PrxInputConfig* cfg, const PrxControllers* test, PrxControllers* out) {
    PrxControllers c{};
    if (test) c = *test;
    else if (g_attached && g_xr.state == StateRunning && g_xr.focused) read_actions(c);
    *out = c;

    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    double dt = g_lastUpdate.QuadPart ? std::fmin(0.1, (double)(now.QuadPart - g_lastUpdate.QuadPart) / freq.QuadPart) : 0;
    g_lastUpdate = now;

    ULONGLONG ms = GetTickCount64();
    uint32_t events = update_menu_button((c.buttons & BtnMenu) != 0, ms);
    bool start = ms < g_startUntil;
    hysteresis(g_gripDown[0], c.lGrip);
    hysteresis(g_gripDown[1], c.rGrip);
    update_pointer(c, *cfg);
    update_pad(c, *cfg, start);
    update_keyboard_mouse(c, *cfg, start, dt);
    update_rumble();
    return events;
}

// The emulated pad for games that read Unity's Input System instead of XInput; 0 while Gamepad mode is off.
PRX_API int32_t prx_pad_state(XINPUT_GAMEPAD* out) {
    std::lock_guard lock(g_padMutex);
    *out = g_pad.Gamepad;
    return g_padActive;
}

// A key held while this process owns the foreground window. In native code because some games' Mono maps user32
// to a path inside the game (Outer Wilds), so a managed DllImport of it throws.
PRX_API int32_t prx_key_down(int32_t vk) {
    if (!(GetAsyncKeyState(vk) & 0x8000)) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// The Theater host, which runs beside the game rather than in it: the window keys, mouse and pointer go to.
PRX_API void prx_set_game_window(HWND hwnd) {
    g_window = hwnd;
    g_windowSet = true;
}

PRX_API int32_t prx_game_in_front() { return game_in_front(game_window()); }

PRX_API void prx_haptic_pulse(int32_t hand, float amplitude, float seconds) {
    haptic(hand & 1, amplitude, (XrDuration)(seconds * 1e9));
}
