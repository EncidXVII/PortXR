#include "menu.h"
#include "canvas.h"
#include "common.h"
#include "settings.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>

extern "C" {
PRX_API void prx_get_stats(PrxStats* out);
PRX_API void prx_haptic_pulse(int32_t hand, float amplitude, float seconds);
PRX_API int32_t prx_passthrough();
}

using canvas::Color;
using canvas::Rect;

namespace {
// Look and sizes as Menu.cs (px at the 1080 reference height).
constexpr float W = 1100, H = 840, Row = 64, Pad = 48;
constexpr float PanelRadius = 24, CellRadius = 14, InnerRadius = 12;
constexpr float ParagraphW = 760; // about 70 characters of Small text: a readable line
constexpr float Display = 60, Body = 28, Small = 24;
constexpr float Fast = 0.16f, Slow = 0.28f;
const Color Bg = canvas::rgb(0x0B0B0C), Surface = canvas::rgb(0x1D1D20), Line = canvas::rgb(0x2A2A2E);
const Color HoverFill = canvas::rgb(0x2A2A2E), PressFill = canvas::rgb(0x36363B);
const Color Ink = canvas::rgb(0xF5F5F7), Ink2 = canvas::rgb(0xA8A8AF), Ink3 = canvas::rgb(0x88888F);
const Color OnLime = canvas::rgb(0x0B0B0C), Danger = canvas::rgb(0xFF5A3C), DangerInk = canvas::rgb(0xFF8A6E);
const char* kTabs[] = {"Quick", "View", "Controls", "Game", "Library", "Diagnostics"};
enum { QuickTab, ViewTab, ControlsTab, GameTab, LibraryTab, DiagnosticsTab };
const char* kThemes[] = {"Lime", "Mint", "Aqua", "Sky", "Violet", "Pink", "Amber", "White"};
const int kThemeRgb[] = {0xC6FF1A, 0x5CF2A0, 0x3DF2E0, 0x5CC8FF, 0xB49AFF, 0xFF7ADB, 0xFFB627, 0xF5F5F7};

struct Style {
    canvas::Face face;
    float size;
    Color color;
    canvas::Align align;
    bool wrap = false;
};

// Bindings (Bindings.cs): inputs, hands, key names as stored and as shown.
const char* kInputs[] = {"A", "B", "X", "Y", "LeftGrip", "RightGrip", "LeftStickClick", "RightStickClick", "LeftTrigger", "RightTrigger", "MenuTap", "MenuHold"};
const char* kLabels[] = {"A", "B", "X", "Y", "Grip", "Grip", "Stick click", "Stick click", "Trigger", "Trigger", "Menu tap", "Menu hold"};
const char* kDefaults[] = {"Space", "LeftCtrl", "E", "R", "LeftShift", "Q", "C", "None", "MouseRight", "MouseLeft", "Esc", "Tab"};
const int kLeftHand[] = {2, 3, 4, 8, 6, 10, 11}, kRightHand[] = {0, 1, 5, 9, 7};
const char* kKeys[] = {"None", "Space", "Enter", "Esc", "Tab", "Backspace", "LeftShift", "LeftCtrl", "LeftAlt", "Q", "E", "R", "F", "G", "C", "V", "Z",
                       "X", "T", "I", "M", "1", "2", "3", "4", "Up", "Down", "Left", "Right", "MouseLeft", "MouseRight", "MouseMiddle"};
const char* kGestures[] = {"Press", "Double press", "Hold", "Grip +"};
const char* kSuffix[] = {"", "Double", "Hold", "Combo"};

std::string shown_key(const std::string& k) {
    static const std::map<std::string, std::string> names{{"LeftShift", "Left Shift"}, {"LeftCtrl", "Left Ctrl"}, {"LeftAlt", "Left Alt"},
                                                          {"MouseLeft", "Left click"}, {"MouseRight", "Right click"},
                                                          {"MouseMiddle", "Middle click"}, {"Up", "Up arrow"}, {"Down", "Down arrow"},
                                                          {"Left", "Left arrow"}, {"Right", "Right arrow"}};
    auto it = names.find(k);
    return it == names.end() ? k : it->second;
}

bool applies(int gesture, int input) { return gesture == 0 || (input < 10 && !(gesture == 3 && (input == 4 || input == 5))); }

std::string upper(const std::string& s) {
    std::string out = s;
    for (auto& c : out)
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    return out;
}

std::string fmt(const char* f, double v) {
    char buf[64];
    snprintf(buf, sizeof(buf), f, v);
    return buf;
}

bool motion_on() {
    static int on = -1;
    if (on < 0) {
        BOOL v = TRUE;
        on = !SystemParametersInfoW(0x1042, 0, &v, 0) || v ? 1 : 0;  // SPI_GETCLIENTAREAANIMATION
    }
    return on == 1;
}

// MenuLayout.cs: rows and columns that nest; a measuring pass, then the drawing pass.
struct Layout {
    struct Group {
        bool row;
        float x, y, w, cursor = 0, cross = 0, fixed = 0;
        int index, flex = 0;
    };
    struct Measure {
        float fixed = 0, cross = 0, main = 0;
        int flex = 0;
    };
    std::vector<Measure> measures;
    std::vector<Group> stack;
    int nextIndex = 0;
    bool measuring = false;
    Rect last;

    void run(float width, const std::function<void()>& body) {
        measuring = true;
        pass(width, body);
        measuring = false;
        pass(width, body);
    }
    void pass(float width, const std::function<void()>& body) {
        nextIndex = 0;
        stack.clear();
        push(false, 0, 0, width);
        body();
        stack.clear();
    }
    void push(bool row, float x, float y, float w) {
        Group g{row, x, y, w};
        g.index = nextIndex++;
        if (measuring) {
            if ((int)measures.size() <= g.index) measures.resize(g.index + 1);
            measures[g.index] = {};
        }
        stack.push_back(g);
    }
    Measure of(int index) const { return index < (int)measures.size() ? measures[index] : Measure{}; }
    bool in_row() const { return stack.back().row; }
    Rect take(float w, float h) {
        Group& g = stack.back();
        Rect r;
        if (g.row) {
            Measure m = of(g.index);
            float width = w > 0 ? w : m.flex > 0 ? std::max(0.f, (g.w - m.fixed) / m.flex) : 0;
            if (w > 0) g.fixed += w;
            else g.flex++;
            r = {g.x + g.cursor, g.y, width, h};
            g.cursor += width;
            g.cross = std::max(g.cross, h);
        } else {
            float width = w > 0 ? w : g.w;
            r = {g.x, g.y + g.cursor, width, h};
            g.cursor += h;
            g.fixed += h;
            g.cross = std::max(g.cross, width);
        }
        last = r;
        return r;
    }
    void gap(float px) {
        stack.back().cursor += px;
        stack.back().fixed += px;
    }
    void flex() {
        if (in_row()) take(0, 0);
    }
    void begin_row() {
        Group p = stack.back();
        float w = p.row ? of(nextIndex).main : p.w;
        push(true, p.row ? p.x + p.cursor : p.x, p.row ? p.y : p.y + p.cursor, w);
    }
    void begin_column(float width = 0) {
        Group p = stack.back();
        float w = width > 0 ? width : p.row ? of(nextIndex).cross : p.w;
        push(false, p.row ? p.x + p.cursor : p.x, p.row ? p.y : p.y + p.cursor, w);
    }
    void end() {
        Group g = stack.back();
        stack.pop_back();
        if (measuring) measures[g.index] = {g.fixed, g.cross, g.cursor, g.flex};
        Group& p = stack.back();
        last = g.row ? (p.row ? take(g.cursor, g.cross) : take(std::max(g.w, g.cursor), g.cross)) : take(g.w, g.cursor);
    }
};

struct Entry {
    std::string name;
    std::wstring dir;
    std::wstring cover;
};

std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring folder(std::wstring p) {
    wchar_t full[MAX_PATH * 2];
    if (GetFullPathNameW(p.c_str(), MAX_PATH * 2, full, nullptr)) p = full;
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    for (auto& c : p) c = (wchar_t)towlower(c);
    return p;
}

std::wstring appdata(const wchar_t* name) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    return std::wstring(buf, n) + L"\\Parallax\\" + name;
}

class Menu {
public:
    menu::Host host;
    bool open = false, welcome = false;
    bool screenUnlocked = false;
    int tab = QuickTab;
    menu::Requests req;

    void show(int page) {
        welcome = false;
        select(page);
    }
    void unlock(bool on) {
        screenUnlocked = on;
        dirty = true;
    }

    void toggle() {
        open = !open;
        picking = -1;
        confirming = NoConfirm;
        press = false;
        if (open) {
            openedAt = now;
            if (tab == LibraryTab) load_library();
        } else {
            welcome = false;
            screenUnlocked = false;
        }
        prx_haptic_pulse(0, 0.4f, 0.03f);
        dirty = true;
    }

    menu::Requests frame(bool pointing_, float u, float v, float trigger, float dt) {
        req = {};
        now += dt;
        frameMs[historyAt] = dt * 1000;
        historyAt = (historyAt + 1) % History;
        if (!open) return req;
        if (settings::refresh()) dirty = true;
        accent = theme_color(theme_index());
        pointing = pointing_;
        mx = u * W, my = v * H;
        bool held = triggerDown ? trigger > 0.4f : trigger > 0.6f;
        if (pointing && held && !triggerDown) press = true;
        triggerDown = held;
        this->dt = dt;
        // Logic: clicks and hover, no pixels.
        painting = false;
        animating = false;
        float hoverBefore = hovered;
        hovered = 0;
        body();
        bool pressed = press;
        press = false;
        if (hovered != 0 && hovered != lastHovered) prx_haptic_pulse(1, 0.15f, 0.01f);
        lastHovered = hovered;
        // Paint when something shows differently: a click, a new hover, an easing in progress, a new setting, the
        // diagnostics graph (ten times a second), the fade-in.
        bool diag = tab == DiagnosticsTab && now - paintedAt > 0.1f;
        if (dirty || pressed || hovered != hoverBefore || animating || diag || now - openedAt < Fast * 2 || triggerDown != lastTrigger) {
            painting = true;
            canvas.resize((int)W, (int)H);
            canvas.fade = motion_on() ? std::clamp((now - openedAt) / Fast, 0.f, 1.f) : 1.f;
            canvas.fade = 1 - (1 - canvas.fade) * (1 - canvas.fade);
            canvas.round({0, 0, W, H}, Bg, PanelRadius);
            hovered = 0;
            // Cleared first: drawing may ask for another paint (a cover still to decode).
            dirty = false;
            body();
            painting = false;
            version++;
            paintedAt = now;
        }
        lastTrigger = triggerDown;
        return req;
    }

    canvas::Canvas canvas;
    uint64_t version = 0;

private:
    static constexpr int NoConfirm = -2, QuitConfirm = -1, LibraryCards = 5, History = 120;
    static constexpr float CardGap = 16, NowHeight = 148, ThumbWidth = 64, ThumbW = 304, ThumbH = 171;
    Layout ui;
    float now = 0, dt = 0, openedAt = -10, paintedAt = 0;
    bool painting = false, animating = false, dirty = true, pointing = false, triggerDown = false, press = false, lastTrigger = false;
    float mx = -1, my = -1, hovered = 0, lastHovered = 0;
    Rect hoverRect;
    float barX = -1, barW = 0;
    std::map<std::string, std::pair<float, float>> slides;
    std::map<int, float> hovers;
    int picking = -1, gesture = 0, confirming = NoConfirm, libPage = 0, camPage = 0;
    Color accent = canvas::rgb(0xC6FF1A);
    float frameMs[History] = {};
    int historyAt = 0;
    std::wstring appPath;
    bool haveCurrent = false;
    Entry current;
    std::vector<Entry> others;
    std::map<std::wstring, canvas::Image> covers;
    bool coverLoaded = false;

    // Styles (Menu.cs EnsureStyles): cells carry only the text; their rounded fill is drawn by Cell.
    Style cell() const { return {canvas::Body, 23, Ink, canvas::Center}; }
    Style cellOn() const { return {canvas::Bold, 23, OnLime, canvas::Center}; }
    Style cellQuiet() const { return {canvas::Body, 23, Ink2, canvas::Center}; }
    Style quiet() const { return {canvas::Body, 22, Ink2, canvas::Center}; }
    Style sign() const { return {canvas::Body, 30, Ink, canvas::Center}; }
    Style label() const { return {canvas::Body, Body, Ink, canvas::Left, true}; }
    Style muted() const { return {canvas::Body, Small, Ink2, canvas::Left, true}; }
    Style warn() const { return {canvas::Body, Small, DangerInk, canvas::Left, true}; }
    Style value() const { return {canvas::Body, Body, Ink, canvas::Center}; }
    Style data() const { return {canvas::Mono, Body * 0.85f, Ink, canvas::Left}; }
    Style display() const { return {canvas::Display, Display, Ink, canvas::Left}; }
    Style eyebrow() const { return {canvas::Bold, Small, Ink3, canvas::Left}; }
    Style eyebrowWord() const { return {canvas::Bold, Small, accent, canvas::Left}; }
    Style tabStyle() const { return {canvas::Body, 26, Ink2, canvas::Left}; }
    Style tabOn() const { return {canvas::Bold, 26, Ink, canvas::Left}; }
    Style monogram() const { return {canvas::Display, 96, Ink3, canvas::TopLeft}; }
    Style monogramSmall() const { return {canvas::Display, 44, Ink3, canvas::Center}; }
    Style cardName() const { return {canvas::Body, Small, Ink2, canvas::TopLeft}; }
    Style cardTitle() const { return {canvas::Body, Small, Ink, canvas::TopLeft}; }
    Style centredLabel() const { return {canvas::Body, Body, Ink, canvas::Center}; }
    Style centredMuted() const { return {canvas::Body, Small, Ink2, canvas::Center}; }

    static int theme_index() {
        std::wstring path = appdata(L"theme.txt");
        static std::string last;
        static ULONGLONG at = 0;
        if (GetTickCount64() - at > 1000) {
            at = GetTickCount64();
            last.clear();
            if (FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO)) {
                char buf[64] = {};
                fread(buf, 1, 63, f);
                fclose(f);
                last = buf;
            }
        }
        for (int i = 0; i < 8; ++i)
            if (_strnicmp(last.c_str(), kThemes[i], strlen(kThemes[i])) == 0) return i;
        return 0;
    }
    static Color theme_color(int i) { return canvas::rgb(kThemeRgb[i]); }
    Color accent_pressed() const { return {accent.r * 0.86f, accent.g * 0.86f, accent.b * 0.86f, 1}; }

    // --- Drawing and hit testing, as Menu.cs's helpers --------------------------------------------------------------
    bool paint() const { return painting && !ui.measuring; }
    bool over(const Rect& r) const { return pointing && r.contains(mx, my); }
    void round(const Rect& r, Color c, float radius) {
        if (paint()) canvas.round(r, c, radius, 0);
    }
    void ring(const Rect& r, Color c, float radius) {
        if (paint()) canvas.round(r, c, radius, 1);
    }
    void mask(const Rect& r, Color c, float radius) {
        if (paint()) canvas.round(r, c, radius, 2);
    }
    void fill(const Rect& r, Color c) {
        if (paint()) canvas.fill(r, c);
    }
    void text(const Rect& r, const std::string& t, const Style& s) {
        if (paint()) canvas.text(r, t, s.face, s.size, s.color, s.align, s.wrap);
    }
    float natural(const std::string& t, const Style& s) { return canvas.measure(t, s.face, s.size) + 4; }
    void label(const std::string& t, const Style& s, float width, float height = Row) {
        text(ui.take(width > 0 ? width : ui.in_row() ? natural(t, s) : 0, height), t, s);
    }
    void hover(const Rect& r) {
        if (over(r)) hovered = r.x * 7919.f + r.y + 1;
    }
    bool clicked(const Rect& r) const { return !painting && !ui.measuring && over(r) && press; }
    bool pressed(const Rect& r) const { return pointing && triggerDown && r.contains(mx, my); }
    float follow(float v, float target, float tau) {
        if (!motion_on() || tau <= 0) return target;
        v += (target - v) * std::min(1.f, dt / tau);
        if (std::fabs(target - v) < 0.002f) return target;
        animating = true;
        return v;
    }
    float eased(const Rect& r, bool on) {
        int key = (int)(r.x * 7919.f + r.y);
        float h = hovers[key];
        if (!ui.measuring && !painting) h = follow(h, on ? 1.f : 0.f, Fast / 4);
        else if (h != (on ? 1.f : 0.f)) animating = true;
        if (!painting) hovers[key] = h;
        if (hovers.size() > 512) hovers.clear();
        return h;
    }

    bool cell_(const Rect& r, const std::string& shown, const Style& style, Color fill_, bool on, bool lime = false) {
        bool o = over(r);
        float h = eased(r, o && !on);
        if (paint()) {
            Color c = lime ? (pressed(r) ? accent_pressed() : accent) : on ? fill_ : pressed(r) ? PressFill : canvas::mix(fill_, HoverFill, h);
            round(r, c, CellRadius);
            text(r, shown, style);
            if (o) hoverRect = r;
        }
        hover(r);
        bool c = clicked(r);
        if (c) {
            prx_haptic_pulse(1, 0.35f, 0.02f);
            dirty = true;
        }
        return c;
    }
    bool button(const std::string& t, bool on, float width, bool dataText = false, const Style* look = nullptr) {
        std::string shown = dataText ? t : upper(t);
        Style style = look ? *look : on ? cellOn() : cell();
        Rect r = ui.take(width > 0 ? width : natural(shown, style) + 48, Row);
        return cell_(r, shown, style, on ? Ink : Surface, on);
    }
    bool quiet_button(const std::string& t, float width) {
        Rect row = ui.take(width, Row);
        Rect r{row.x, row.y + 6, row.w, row.h - 12};
        return cell_(r, upper(t), quiet(), Surface, false);
    }
    bool primary(const std::string& t, float width) {
        Rect r = ui.take(width, Row);
        Style s = cellOn();
        return cell_(r, upper(t), s, accent, false, true);
    }

    int segment(const std::string& id, const std::vector<std::string>& labels, const std::vector<float>& widths, int selected) {
        float total = 0;
        for (float w : widths) total += w;
        Rect track = ui.take(total + 8, Row);
        round(track, Surface, CellRadius + 2);
        int clickedAt = -1;
        float x = track.x + 4;
        std::vector<Rect> cells;
        for (size_t i = 0; i < labels.size(); ++i) {
            cells.push_back({x, track.y + 4, widths[i], track.h - 8});
            x += widths[i];
        }
        if (!ui.measuring && selected >= 0) {
            std::pair<float, float> to{cells[selected].x, cells[selected].w};
            auto it = slides.find(id);
            std::pair<float, float> at = it == slides.end() ? to : it->second;
            if (!painting) at = {follow(at.first, to.first, Slow / 4), follow(at.second, to.second, Slow / 4)};
            else if (at != to) animating = true;
            slides[id] = at;
            round({at.first, track.y + 4, at.second, track.h - 8}, Ink, InnerRadius);
        }
        for (size_t i = 0; i < labels.size(); ++i) {
            Rect r = cells[i];
            bool on = (int)i == selected, o = over(r);
            float h = eased(r, o && !on);
            if (paint()) {
                if (!on && pressed(r)) round(r, PressFill, InnerRadius);
                else if (!on && h > 0.01f) round(r, {HoverFill.r, HoverFill.g, HoverFill.b, h}, InnerRadius);
                text(r, upper(labels[i]), on ? cellOn() : cellQuiet());
                if (o && !on) hoverRect = r;
            }
            hover(r);
            if (clicked(r) && !on) {
                clickedAt = (int)i;
                prx_haptic_pulse(1, 0.35f, 0.02f);
                dirty = true;
            }
        }
        return clickedAt;
    }

    // A setting with choices in the 600 px value column (Options in Menu.cs).
    void options(const char* name, const char* section, const char* key, const std::vector<std::string>& values,
                 const std::vector<std::string>& labels, const char* fallback) {
        ui.begin_row();
        label(name, label(), 360);
        // As even as the labels allow: every option gets the same width unless its label needs more.
        std::vector<float> widths;
        float sum = 0;
        for (auto& l : labels) widths.push_back(natural(upper(l), cell())), sum += widths.back();
        float pad = sum + 32 * labels.size() <= 592 ? 32.f : 12.f;
        for (auto& w : widths) w += pad;
        // Options wider than an even share keep their width; the rest split what is left (until that settles).
        float even = 592.f / labels.size();
        for (size_t pass = 0; pass <= labels.size(); ++pass) {
            float wide = 0;
            size_t n = 0;
            for (float w : widths)
                if (w > even) wide += w, ++n;
            if (n == labels.size()) break;
            float next = (592 - wide) / (labels.size() - n);
            if (next == even) break;
            even = next;
        }
        for (auto& w : widths) w = std::max(w, even);
        std::string v = settings::get(section, key, fallback);
        int current = -1;
        for (size_t i = 0; i < values.size(); ++i)
            if (values[i] == v) current = (int)i;
        int picked = segment(name, labels, widths, current);
        if (picked >= 0) settings::set(section, key, values[picked]);
        ui.end();
        ui.gap(12);
    }

    void choice(const char* name, const char* off, const char* on, bool& v) {
        ui.begin_row();
        label(name, label(), 360);
        int picked = segment(name, {off, on}, {296, 296}, v ? 1 : 0);
        if (picked >= 0) v = picked == 1;
        ui.end();
        ui.gap(12);
    }

    void toggle_row(const char* name, const char* section, const char* key, bool fallback) {
        bool on = settings::getb(section, key, fallback), was = on;
        choice(name, "Off", "On", on);
        if (on != was) settings::set(section, key, on ? "true" : "false");
    }

    void stepper(const char* name, const char* section, const char* key, float fallback, float step, float lo, float hi, const char* format,
                 bool* changed = nullptr) {
        ui.begin_row();
        label(name, label(), 360);
        stepper_controls(section, key, fallback, step, lo, hi, format, changed);
        ui.end();
        ui.gap(12);
    }

    void stepper_controls(const char* section, const char* key, float fallback, float step, float lo, float hi, const char* format,
                          bool* changed = nullptr) {
        float v = settings::getf(section, key, fallback, lo, hi);
        Style s = sign();
        auto store = [&](float x) {
            x = std::round(std::clamp(x, lo, hi) * 100) / 100;
            settings::set(section, key, fmt("%g", x));
            if (changed) *changed = true;
        };
        if (button("\xE2\x88\x92", false, 96, false, &s)) store(v - step);
        std::string shown = fmt(format, v);
        // Zero without a sign ("0.00 m", not "+0.00 m").
        if (std::fabs(v) < 0.005f) {
            std::string unsigned_ = format;
            size_t plus = unsigned_.find("%+");
            if (plus != std::string::npos) unsigned_.erase(plus + 1, 1);
            shown = fmt(unsigned_.c_str(), 0.0);
        }
        label(shown, value(), 200);
        if (button("+", false, 96, false, &s)) store(v + step);
    }

    void eyebrow_row(const char* number, const std::string& word) {
        ui.begin_row();
        label(std::string(number) + " // ", eyebrow(), 0, 32);
        label(word, eyebrowWord(), 0, 32);
        ui.end();
    }

    // --- The menu ----------------------------------------------------------------------------------------------------
    void body() {
        ui.run(W - 2 * Pad, [&] {
            // The content starts 48 px in and 32 px down (Menu.cs's matrix offset).
            ui.stack.back().x = Pad;
            ui.stack.back().y = 32;
            hoverRect = {};
            if (welcome) draw_welcome();
            else draw_tabs();
            // In a headset a hairline is lost: the control under the laser gets a 2 px ring.
            if (hoverRect.w > 0 && paint()) ring(hoverRect, Ink, CellRadius);
        });
    }

    void draw_tabs() {
        ui.begin_row();
        Rect active;
        for (int i = 0; i < 6; ++i) {
            bool on = i == tab;
            std::string word = upper(kTabs[i]);
            Style s = on ? tabOn() : tabStyle();
            Rect r = ui.take(natural(word, s), Row);
            if (!on && over(r)) s.color = Ink;
            text(r, word, s);
            if (clicked(r)) select(i);
            if (on) active = r;
            hover(r);
            ui.gap(20);
        }
        ui.flex();
        if (tab <= GameTab && quiet_button("Reset", 128)) reset_tab();
        ui.gap(8);
        if (quiet_button("Close", 128)) toggle();
        ui.gap(2);
        ui.end();
        Rect rule = ui.take(0, 1);
        fill(rule, Line);
        if (!ui.measuring && active.w > 0) {
            if (barX < 0) barX = active.x, barW = active.w;
            if (!painting) {
                barX = follow(barX, active.x, Slow / 4);
                barW = follow(barW, active.w, Slow / 4);
            } else if (barX != active.x || barW != active.w) animating = true;
            round({barX, rule.y - 3, barW, 4}, accent, 2);
        }
        ui.gap(24);
        switch (tab) {
            case QuickTab: draw_quick(); break;
            case ViewTab: draw_view(); break;
            case ControlsTab: draw_controls(); break;
            case GameTab: draw_game(); break;
            case LibraryTab: draw_library(); break;
            default: draw_diagnostics(); break;
        }
    }

    void select(int i) {
        tab = i;
        picking = -1;
        camPage = 0;
        confirming = NoConfirm;
        dirty = true;
        if (i == LibraryTab) load_library();
    }

    void draw_welcome() {
        label("YOU ARE IN VR.", display(), 0, 72);
        ui.gap(8);
        label("Three quick checks, then play. The menu is always a double-tap away.", muted(), 0, 48);
        ui.gap(24);
        step("01", "Stand or sit the way you will play, then set your view straight ahead.");
        ui.begin_row();
        ui.gap(72);
        if (button("Recenter", false, 300)) req.recenter = true;
        ui.end();
        ui.gap(16);
        step("02", "Doors and people should look life-size. Too small or too big? Adjust the world scale.");
        ui.begin_row();
        ui.gap(72);
        stepper_controls("View", "WorldScale", 1, 0.1f, 0.5f, 3, "%.1f\xC3\x97");
        ui.end();
        ui.gap(16);
        step("03", "Left stick moves, right stick looks, triggers click. Double-tap the left menu button for this menu.");
        ui.gap(24);
        ui.begin_row();
        ui.gap(72);
        if (primary("Start playing", 360)) {
            settings::set("Menu", "FirstRun", "false");
            toggle();
        }
        ui.gap(8);
        if (button("Show all settings", false, 360)) {
            settings::set("Menu", "FirstRun", "false");
            welcome = false;
            dirty = true;
        }
        ui.end();
    }

    void step(const char* number, const std::string& t) {
        ui.begin_row();
        float h = natural(t, label()) > W - 2 * Pad - 72 ? 80 : 48;
        label(number, eyebrowWord(), 72, h);
        text(ui.take(0, h), t, label());
        ui.end();
        ui.gap(8);
    }

    void reset_tab() {
        std::vector<std::pair<const char*, const char*>> keys;
        if (tab == QuickTab)
            keys = {{"View", "Display"}, {"View", "Depth"}, {"View", "Background"}, {"View", "ScreenYaw"}, {"View", "ScreenPitch"},
                    {"View", "ScreenDistance"}, {"View", "ScreenWidth"}, {"View", "WorldScale"}, {"Comfort", "Turning"},
                    {"Comfort", "Vignette"}, {"Controls", "Mode"}, {"Controls", "HeadAim"}, {"Controls", "AimWith"}};
        else if (tab == ViewTab)
            keys = {{"View", "Height"}, {"View", "RenderScale"}, {"View", "AutoRenderScale"}, {"View", "UiDistance"}, {"View", "UiWidth"},
                    {"Controls", "Pointer"}, {"Controls", "WristMenu"}, {"View", "UiBlack"}};
        else if (tab == GameTab) keys = {{"Game", "CameraName"}, {"Controls", "Crosshair"}, {"Controls", "WeaponInHand"}};
        static std::vector<std::string> bindingKeys;
        if (tab == ControlsTab) {
            bindingKeys.clear();
            for (int g = 0; g < 4; ++g)
                for (int i = 0; i < 12; ++i)
                    if (applies(g, i)) bindingKeys.push_back(std::string(kInputs[i]) + kSuffix[g]);
            for (auto& k : bindingKeys) keys.push_back({"Bindings", k.c_str()});
        }
        // Back to the player's all-games values, else the defaults (an empty value here reads as the default).
        std::map<std::string, std::string> all;
        if (FILE* f = _wfsopen(appdata(L"defaults.cfg").c_str(), L"rb", _SH_DENYNO)) {
            std::string text, section;
            char buf[4096];
            for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
            fclose(f);
            size_t pos = 0;
            while (pos < text.size()) {
                size_t end = text.find('\n', pos);
                if (end == std::string::npos) end = text.size();
                std::string l = text.substr(pos, end - pos);
                pos = end + 1;
                while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
                size_t a = l.find_first_not_of(' ');
                if (a == std::string::npos) continue;
                l = l.substr(a);
                if (l[0] == '[' && l.back() == ']') section = l.substr(1, l.size() - 2);
                else if (l[0] != '#' && l.find('=') != std::string::npos) {
                    std::string k = l.substr(0, l.find('=')), v = l.substr(l.find('=') + 1);
                    while (!k.empty() && k.back() == ' ') k.pop_back();
                    size_t b = v.find_first_not_of(' ');
                    all[section + "/" + k] = b == std::string::npos ? "" : v.substr(b);
                }
            }
        }
        for (auto& [section, key] : keys) {
            auto it = all.find(std::string(section) + "/" + key);
            std::string v = it != all.end() ? it->second : "";
            if (it == all.end() && std::string(section) == "Bindings") {
                v = "None";
                for (int i = 0; i < 12; ++i)
                    if (key == std::string(kInputs[i])) v = kDefaults[i];
            }
            settings::set(section, key, v);
        }
        picking = -1;
        if (tab == ViewTab) req.renderScale = true;
    }

    void draw_quick() {
        options("Display", "View", "Display", {"Stereo", "Parallax", "Theater"}, {"Full VR", "3D screen", "VR screen"}, "Stereo");
        std::string display = settings::get("View", "Display", "Stereo");
        if (display == "Parallax") stepper("3D depth", "View", "Depth", 1, 0.25f, 0, 1, "%.2f\xC3\x97");
        if (display != "Stereo") {
            bool unlocked = screenUnlocked;
            choice("Screen", "Locked", "Unlocked", unlocked);
            if (unlocked != screenUnlocked) screenUnlocked = unlocked, dirty = true;
            ui.begin_row();
            ui.gap(360);
            label(unlocked ? "Point at its corners to resize it, at the bar under it to move it."
                           : "Unlock to move or resize it.",
                  muted(), 0, 40);
            ui.end();
            ui.gap(12);
            if (prx_passthrough())
                options("Around the screen", "View", "Background", {"Dark", "Passthrough"}, {"Dark", "Your room"}, "Dark");
        }
        ui.begin_row();
        label("Position", label(), 360);
        if (button("Recenter", false, 0)) req.recenter = true;
        ui.end();
        ui.gap(12);
        std::vector<std::string> modes{"Auto", "Gamepad", "KeyboardMouse", "Off"}, modeLabels{"Auto", "Gamepad", "Keyboard & mouse", "Off"};
        if (display != "Stereo") {
            options("Controls", "Controls", "Mode", modes, modeLabels, "Auto");
            return;
        }
        stepper("World scale", "View", "WorldScale", 1, 0.1f, 0.5f, 3, "%.1f\xC3\x97");
        options("Turning", "Comfort", "Turning", {"Smooth", "Snap15", "Snap30", "Snap45"}, {"Smooth", "Snap 15\xC2\xB0", "Snap 30\xC2\xB0", "Snap 45\xC2\xB0"}, "Smooth");
        options("Comfort vignette", "Comfort", "Vignette", {"Off", "Low", "High"}, {"Off", "Low", "High"}, "Off");
        options("Controls", "Controls", "Mode", modes, modeLabels, "Auto");
        options("Aim", "Controls", "HeadAim", {"Auto", "On", "Off"}, {"Auto", "On", "Off"}, "Auto");
        if (settings::get("Controls", "HeadAim", "Auto") != "Off")
            options("Aim with", "Controls", "AimWith", {"Head", "Hand"}, {"Head", "Right hand"}, "Hand");
    }

    void draw_view() {
        stepper("Height", "View", "Height", 0, 0.05f, -1, 1, "%+.2f m");
        bool changed = false;
        stepper("Resolution", "View", "RenderScale", 1, 0.1f, 0.5f, 2, "%.1f\xC3\x97", &changed);
        if (changed) req.renderScale = true;
        toggle_row("Auto resolution", "View", "AutoRenderScale", true);
        stepper("UI distance", "View", "UiDistance", 1.2f, 0.1f, 0.5f, 3, "%.1f m");
        stepper("UI width", "View", "UiWidth", 1.6f, 0.1f, 0.5f, 4, "%.1f m");
        bool menuOnly = settings::get("Controls", "Pointer", "Auto") == "MenuOnly", was = menuOnly;
        choice("Laser pointer", "With game cursor", "Menu only", menuOnly);
        if (menuOnly != was) settings::set("Controls", "Pointer", menuOnly ? "MenuOnly" : "Auto");
        toggle_row("Wrist glance menu", "Controls", "WristMenu", true);
        if (host.uiBlack)
            options("UI black", "View", "UiBlack", {"Auto", "SeeThrough", "Keep"}, {"Auto", "See-through", "Keep"}, "Auto");
        theme_row();
    }

    // Accent colour for every game and the app: one dot per theme, the chosen one ringed and named.
    void theme_row() {
        ui.begin_row();
        int current = theme_index();
        label("Theme", label(), 360);
        for (int i = 0; i < 8; ++i) {
            Rect r = ui.take(Row + 6, Row);
            Rect cellRect{r.x + 7, r.y + 4, Row - 8, Row - 8};
            Rect dot{cellRect.x + 9, cellRect.y + 9, cellRect.w - 18, cellRect.h - 18};
            bool o = over(cellRect);
            float h = eased(cellRect, o);
            if (paint()) {
                if (i == current) ring(cellRect, Ink, cellRect.w / 2);
                else if (h > 0.01f) ring(cellRect, Ink3, cellRect.w / 2);
                round(dot, theme_color(i), dot.w / 2);
            }
            hover(cellRect);
            if (clicked(cellRect) && i != current) {
                std::wstring path = appdata(L"theme.txt");
                if (FILE* f = _wfopen(path.c_str(), L"wb")) {
                    fputs(kThemes[i], f);
                    fclose(f);
                }
                prx_haptic_pulse(1, 0.35f, 0.02f);
                dirty = true;
            }
        }
        ui.gap(12);
        label(kThemes[current], muted(), 0);
        ui.end();
        ui.gap(12);
    }

    std::string input_now() {
        return settings::get("Controls", "Mode", "Auto");
    }

    void draw_controls() {
        std::string mode = input_now();
        if (mode == "Off") {
            label("Controls are off: the controllers only work this menu and the laser pointer. Play with your own keyboard, "
                  "mouse or gamepad. Switch Controls in Quick to use the controllers in the game.", muted(), ParagraphW, 96);
            return;
        }
        if (mode == "Gamepad") {
            label("Gamepad mode: the controllers act as an Xbox pad. Sticks, triggers, A B X Y, grips as shoulder buttons, stick "
                  "clicks; menu tap is Start. Switch to Keyboard & mouse in Quick to rebind.", muted(), ParagraphW, 96);
            return;
        }
        if (picking >= 0) {
            draw_key_picker();
            return;
        }
        ui.begin_row();
        float each = (W - 2 * Pad - 8) / 4;
        int picked = segment("gesture", {kGestures[0], kGestures[1], kGestures[2], kGestures[3]}, {each, each, each, each}, gesture);
        if (picked >= 0) gesture = picked;
        ui.end();
        label(gesture == 0   ? "Left stick moves (W A S D), right stick looks. Pick a button to change it."
              : gesture == 1 ? "Two quick presses. The first press still does its own key."
              : gesture == 2 ? "Held for half a second. A short press then taps its own key on release."
                             : "Pressed while the same hand's grip is held. The grip's own key lets go meanwhile.",
              muted(), 0);
        ui.begin_row();
        binding_column("01", "LEFT HAND", kLeftHand, 7);
        ui.gap(40);
        binding_column("02", "RIGHT HAND", kRightHand, 5);
        ui.end();
        std::string conflict = binding_conflict();
        if (!conflict.empty()) label(conflict, warn(), 0);
    }

    std::string binding(int g, int i) {
        return settings::get("Bindings", (std::string(kInputs[i]) + kSuffix[g]).c_str(), g == 0 ? kDefaults[i] : "None");
    }

    std::string binding_name(int g, int i) {
        return std::string(g == 3 ? "Grip + " : "") + kLabels[i] + (g == 1 ? " twice" : g == 2 ? " held" : "");
    }

    std::string binding_conflict() {
        std::map<std::string, std::string> seen;
        for (int g = 0; g < 4; ++g)
            for (int i = 0; i < 12; ++i) {
                if (!applies(g, i)) continue;
                std::string v = binding(g, i);
                if (v == "None") continue;
                bool left = std::find(std::begin(kLeftHand), std::end(kLeftHand), i) != std::end(kLeftHand);
                std::string where = std::string(left ? "Left " : "Right ") + binding_name(g, i);
                auto it = seen.find(v);
                if (it != seen.end()) return v + " is on both " + it->second + " and " + where + ".";
                seen[v] = where;
            }
        return {};
    }

    void binding_column(const char* number, const char* name, const int* inputs, int count) {
        ui.begin_column();
        eyebrow_row(number, name);
        for (int k = 0; k < count; ++k) {
            int i = inputs[k];
            if (!applies(gesture, i)) continue;
            ui.begin_row();
            label(binding_name(gesture, i), label(), 260);
            if (button(shown_key(binding(gesture, i)), false, 220)) picking = i, dirty = true;
            ui.end();
        }
        ui.end();
    }

    void draw_key_picker() {
        ui.begin_row();
        bool left = std::find(std::begin(kLeftHand), std::end(kLeftHand), picking) != std::end(kLeftHand);
        label(std::string(left ? "Left " : "Right ") + binding_name(gesture, picking) + " presses:", label(), 0);
        ui.flex();
        if (button("Cancel", false, 160)) picking = -1, dirty = true;
        ui.end();
        ui.gap(8);
        std::string current = binding(gesture, picking);
        const int columns = 6, n = (int)(sizeof(kKeys) / sizeof(kKeys[0]));
        for (int start = 0; start < n && picking >= 0; start += columns) {
            ui.begin_row();
            for (int j = start; j < std::min(start + columns, n); ++j) {
                if (button(shown_key(kKeys[j]), current == kKeys[j], 160)) {
                    settings::set("Bindings", (std::string(kInputs[picking]) + kSuffix[gesture]).c_str(), kKeys[j]);
                    picking = -1;
                    break;
                }
                ui.gap(4);
            }
            ui.end();
            ui.gap(4);
        }
    }

    void draw_game() {
        if (host.hideObjects) {
            ui.begin_row();
            label("In your view? Point at it, pull the trigger. B stops.", muted(), 680);
            ui.flex();
            if (host.hidden > 0 && button(host.hidden == 1 ? "Show 1 hidden" : "Show " + std::to_string(host.hidden) + " hidden", false, 260))
                req.showHidden = true;
            ui.gap(4);
            if (button("Hide objects", false, 260)) {
                toggle();
                req.hideObjects = true;
            }
            ui.end();
            ui.gap(16);
        }
        if (host.cameras.empty()) return aim_rows();
        label("Game camera", label(), 0);
        std::string chosen = settings::get("Game", "CameraName", "");
        bool automatic = chosen.empty();
        const int columns = 3;
        int pages = std::max(1, ((int)host.cameras.size() + columns - 1) / columns);
        camPage %= pages;
        ui.begin_row();
        if (button("Automatic", automatic, ThumbW, true)) pick_camera("");
        ui.flex();
        if (pages > 1 && button("More cameras " + std::to_string(camPage + 1) + "/" + std::to_string(pages), false, 360))
            camPage = (camPage + 1) % pages, dirty = true;
        ui.end();
        ui.gap(16);
        ui.begin_row();
        for (int j = camPage * columns; j < std::min((camPage + 1) * columns, (int)host.cameras.size()); ++j) {
            const std::string& cam = host.cameras[j];
            if (button(cam, !automatic && cam == chosen, ThumbW, true)) pick_camera(cam);
            ui.gap(16);
        }
        ui.end();
        ui.gap(24);
        aim_rows();
    }

    void aim_rows() {
        if (!host.aimExtras || settings::get("Controls", "HeadAim", "Auto") == "Off") return;
        options("Crosshair", "Controls", "Crosshair", {"Game", "Lime", "Off"}, {"Game's", "Parallax's", "Off"}, "Game");
        if (host.handHeld && settings::get("Controls", "AimWith", "Hand") == "Hand") toggle_row("Weapon in hand", "Controls", "WeaponInHand", true);
    }

    void pick_camera(const std::string& name) {
        settings::set("Game", "CameraName", name);
        req.cameraPicked = true;
        req.camera = name;
    }

    // --- Library: quit, or start another game with VR added (through Parallax.exe), as Menu.cs ----------------------
    void load_library() {
        appPath.clear();
        haveCurrent = false;
        others.clear();
        libPage = 0;
        covers.clear();
        std::string text;
        if (FILE* f = _wfsopen(appdata(L"vr-games.txt").c_str(), L"rb", _SH_DENYNO)) {
            char buf[4096];
            for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
            fclose(f);
        }
        std::wstring here = folder(host.gameDir);
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(pos, end - pos);
            pos = end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::vector<std::string> f;
            size_t a = 0;
            for (size_t b; (b = line.find('\t', a)) != std::string::npos; a = b + 1) f.push_back(line.substr(a, b - a));
            f.push_back(line.substr(a));
            if (f.size() < 2 || f[0].empty()) continue;
            if (f[0] == "app") {
                std::wstring p = wide(f[1]);
                if (p.size() > 4 && _wcsicmp(p.c_str() + p.size() - 4, L".exe") == 0 && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES)
                    appPath = p;
                continue;
            }
            Entry e{f[0], wide(f[1]), f.size() > 2 ? wide(f[2]) : L""};
            // The running game's folder is the listed one, or inside it (Unreal runs from <Project>\Binaries\Win64).
            std::wstring listed = folder(e.dir);
            if (here == listed || here.rfind(listed + L"\\", 0) == 0) current = e, haveCurrent = true;
            else others.push_back(e);
        }
    }

    const canvas::Image* cover(const std::wstring& path) {
        if (path.empty()) return nullptr;
        auto it = covers.find(path);
        if (it != covers.end()) return it->second.w > 0 ? &it->second : nullptr;
        // One cover decoded per frame, so opening the tab never stalls the headset.
        if (coverLoaded) {
            dirty = true;  // the next frame decodes the next one
            return nullptr;
        }
        coverLoaded = true;
        canvas::Image img;
        canvas::load_image(path, img);
        auto& stored = covers[path] = std::move(img);
        dirty = true;
        return stored.w > 0 ? &stored : nullptr;
    }

    void cover_art(const Rect& r, const std::wstring& path, const std::string& name, Color behind, const Style& initial, float scrim = 0) {
        if (!paint()) return;
        const canvas::Image* tex = cover(path);
        if (tex) {
            fill(r, Surface);
            canvas.image(r, *tex);
        } else {
            round(r, HoverFill, CellRadius);
            Rect at{r.x + r.w * 0.1f, r.y + r.w * 0.06f, r.w, r.w * 0.6f};
            std::string initial_ = name.empty() ? "?" : upper(name.substr(0, 1));
            text(initial.align == canvas::Center ? r : at, initial_, initial);
        }
        if (scrim > 0) fill(r, {0, 0, 0, scrim});
        if (tex || scrim > 0) mask(r, behind, CellRadius);
    }

    void draw_library() {
        if (!painting) coverLoaded = false;
        draw_now_playing();
        ui.gap(32);
        int pages = std::max(1, ((int)others.size() + LibraryCards - 1) / LibraryCards);
        libPage %= pages;
        ui.begin_row();
        label("Your VR games", muted(), 0);
        ui.flex();
        if (pages > 1) {
            Style s = sign();
            if (button("\xE2\x80\xB9", false, 64, false, &s)) libPage = (libPage + pages - 1) % pages, confirming = NoConfirm;
            label(std::to_string(libPage + 1) + " / " + std::to_string(pages), value(), 96);
            if (button("\xE2\x80\xBA", false, 64, false, &s)) libPage = (libPage + 1) % pages, confirming = NoConfirm;
        }
        ui.end();
        ui.gap(12);
        float w = (W - 2 * Pad - (LibraryCards - 1) * CardGap) / LibraryCards;
        Rect shelf = ui.take(0, w * 1.5f + 52);
        if (appPath.empty() || others.empty()) {
            round(shelf, Surface, CellRadius);
            bool noApp = appPath.empty();
            text({shelf.x, shelf.cy() - 40, shelf.w, 40}, noApp ? "Switching games needs Parallax.exe" : "No other games with VR yet", centredLabel());
            text({shelf.x, shelf.cy() + 4, shelf.w, 36},
                 noApp ? "Open it once on this PC, then your VR games show up here." : "Add VR to another game in Parallax.exe and it shows up here.",
                 centredMuted());
            return;
        }
        for (int n = 0; n < LibraryCards; ++n) {
            int i = libPage * LibraryCards + n;
            if (i >= (int)others.size()) break;
            game_card({shelf.x + n * (w + CardGap), shelf.y, w, shelf.h}, i);
        }
    }

    void draw_now_playing() {
        Rect card = ui.take(0, NowHeight);
        bool asking = confirming == QuitConfirm;
        round(card, Surface, CellRadius);
        Rect thumb{card.x + 16, card.y + 16, ThumbWidth, NowHeight - 32};
        std::string name = haveCurrent ? current.name : host.product;
        cover_art(thumb, haveCurrent ? current.cover : L"", name, Surface, monogramSmall());
        float tx = thumb.x + thumb.w + 24, tw = card.x + card.w - 16 - 240 - 24 - tx;
        // Two lines at rest sit in the card's middle; the Quit warning adds a third under them.
        float top = asking ? card.y + 22 : card.y + 40;
        text({tx, top, tw, 28}, "NOW PLAYING", eyebrowWord());
        Style l = label();
        l.wrap = false;
        text({tx, top + 32, tw, 40}, canvas.fit(name, l.face, l.size, tw), l);
        // The warning belongs to Quit, so it shows once Quit is pressed; at rest it read as the game's description.
        if (asking) {
            Style note = warn();
            note.wrap = false;
            text({tx, card.y + 98, tw, 28}, "Press Quit now to close it. Unsaved progress is lost.", note);
        }
        Rect quit{card.x + card.w - 16 - 240, card.cy() - Row / 2, 240, Row};
        bool pressedQuit = asking ? cell_(quit, "QUIT NOW", cellOn(), Danger, true) : cell_(quit, "QUIT GAME", cell(), HoverFill, false);
        if (!pressedQuit) return;
        if (asking) leave(L"");
        else confirming = QuitConfirm;
    }

    void game_card(const Rect& r, int i) {
        const Entry& e = others[i];
        Rect art{r.x, r.y, r.w, r.w * 1.5f};
        bool o = over(r), asking = confirming == i;
        float h = eased(art, o);
        if (paint()) {
            cover_art(art, e.cover, e.name, Bg, monogram(), asking ? 0.7f : pressed(r) ? 0.25f : 0);
            auto pill = [&](float cy, const char* t, Color fill_) {
                Rect p{art.cx() - 64, cy - 22, 128, 44};
                round(p, {fill_.r, fill_.g, fill_.b, asking ? 1 : h}, 22);
                Style s = cellOn();
                s.color.a = asking ? 1 : h;
                text(p, t, s);
            };
            if (asking) {
                text({art.x, art.cy() - 52, art.w, 36}, "Quit and start?", centredLabel());
                pill(art.cy(), "START", Danger);
            } else if (h > 0.01f) pill(art.y + art.h - 40, "START", accent);
            if (o || asking) ring(art, asking ? Danger : accent, CellRadius);
            Style s = o ? cardTitle() : cardName();
            text({r.x + 4, art.y + art.h + 12, r.w - 8, 36}, canvas.fit(e.name, s.face, s.size, r.w - 8), s);
        }
        hover(r);
        if (!clicked(r)) return;
        prx_haptic_pulse(1, 0.35f, 0.02f);
        dirty = true;
        if (asking) leave(e.dir);
        else confirming = i;
    }

    // Quits the game; with `next`, Parallax.exe starts that game folder once this process has ended.
    void leave(const std::wstring& next) {
        confirming = NoConfirm;
        if (!next.empty()) {
            std::wstring dir = next;
            while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
            std::wstring cmd = L"\"" + appPath + L"\" play \"" + dir + L"\" " + std::to_wstring(GetCurrentProcessId());
            STARTUPINFOW si{sizeof(si)};
            PROCESS_INFORMATION pi{};
            if (!CreateProcessW(appPath.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                prx_log("menu: could not start Parallax.exe to switch games (%lu)", GetLastError());
                return;
            }
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            prx_log("menu: switching to another game");
        } else {
            prx_log("menu: quit");
        }
        req.quit = true;
    }

    // --- Diagnostics ---------------------------------------------------------------------------------------------------
    void draw_diagnostics() {
        PrxStats s{};
        prx_get_stats(&s);
        draw_graph((float)s.periodMs);
        ui.gap(16);
        static const char* states[] = {"Failed", "None", "Instance", "Session", "Running"};
        info_row("Headset", std::string(states[std::clamp(s.state + 1, 0, 4)]) + ", " + fmt("%.0f Hz", 1000.0 / std::max(0.001, s.periodMs)));
        info_row("Frame time p95", fmt("%.1f ms", s.p95Ms) + " of " + fmt("%.1f ms", s.periodMs));
        info_row("Eye resolution", std::to_string(host.eyeWidth) + " x " + std::to_string(host.eyeHeight));
        if (s.state == StateRunning && s.p95Ms > s.periodMs * 1.1)
            warning("The game misses the headset's frame rate.", "Lower resolution", [&] {
                float v = settings::getf("View", "RenderScale", 1, 0.5f, 2);
                settings::set("View", "RenderScale", fmt("%g", std::max(0.5, std::round((v - 0.1) * 100) / 100)));
                req.renderScale = true;
            });
        std::string conflict = binding_conflict();
        if (!conflict.empty()) warning(conflict, "Fix bindings", [&] { select(ControlsTab); });
    }

    void draw_graph(float budgetMs) {
        ui.begin_row();
        label("FRAME TIMES", eyebrow(), 0, 32);
        ui.flex();
        label(fmt("Line: headset budget %.1f ms", budgetMs), muted(), 0, 32);
        ui.end();
        ui.gap(8);
        Rect rect = ui.take(0, 160);
        if (!paint()) return;
        round(rect, Surface, 16);
        rect = {rect.x + 12, rect.y + 8, rect.w - 24, rect.h - 16};
        float top = std::max(budgetMs * 1.5f, 1.f), bar = rect.w / History;
        for (int i = 0; i < History; ++i) {
            float ms = frameMs[(historyAt + i) % History], h = std::min(ms / top, 1.f) * rect.h;
            fill({rect.x + i * bar, rect.y + rect.h - h, std::max(1.f, bar - 2), h}, ms > budgetMs * 1.1f ? Danger : accent);
        }
        fill({rect.x, rect.y + rect.h - budgetMs / top * rect.h, rect.w, 1}, Ink2);
    }

    void warning(const std::string& t, const char* fix, const std::function<void()>& action) {
        ui.begin_row();
        label(t, warn(), 0);
        ui.flex();
        if (button(fix, false, 260)) action();
        ui.end();
    }

    void info_row(const char* name, const std::string& t) {
        ui.begin_row();
        label(name, muted(), 360, 48);
        text(ui.take(0, 48), t, data());
        ui.end();
    }
};

Menu g_menu;
int g_lastVersion = -1;
}  // namespace

namespace menu {
void set_host(const Host& host) { g_menu.host = host; }
bool is_open() { return g_menu.open; }
bool screen_unlocked() { return g_menu.open && g_menu.screenUnlocked; }
void toggle() { g_menu.toggle(); }

void show_welcome() {
    if (!g_menu.open) g_menu.toggle();
    g_menu.welcome = true;
}

Requests frame(bool pointing, float u, float v, float trigger, float dt) { return g_menu.frame(pointing, u, v, trigger, dt); }

const uint8_t* pixels(int& w, int& h, bool& changed) {
    w = g_menu.canvas.width();
    h = g_menu.canvas.height();
    changed = (int)g_menu.version != g_lastVersion;
    g_lastVersion = (int)g_menu.version;
    return w > 0 ? g_menu.canvas.pixels() : nullptr;
}

float aspect() { return W / H; }
}  // namespace menu

// --- The menu's entry points for the Unity plugin (C#): what Unity offers it, and the tests' tabs ------------------
struct PrxMenuHost {
    const char* product;
    const wchar_t* gameDir;
    const char* cameras;  // names, one per line
    const char* currentCamera;
    int32_t hideObjects, hidden, aimExtras, uiBlack, eyeWidth, eyeHeight;
};

extern "C" {
PRX_API void prx_menu_settings(const wchar_t* gameFile) { settings::open(gameFile, L""); }

PRX_API void prx_menu_host(const PrxMenuHost* h) {
    menu::Host m;
    m.product = h->product ? h->product : "";
    m.gameDir = h->gameDir ? h->gameDir : L"";
    for (const char* s = h->cameras; s && *s;) {
        const char* e = strchr(s, '\n');
        m.cameras.emplace_back(s, e ? e - s : strlen(s));
        s = e ? e + 1 : s + strlen(s);
    }
    m.currentCamera = h->currentCamera ? h->currentCamera : "";
    m.hideObjects = h->hideObjects, m.hidden = h->hidden, m.aimExtras = m.handHeld = h->aimExtras, m.uiBlack = h->uiBlack;
    m.eyeWidth = h->eyeWidth, m.eyeHeight = h->eyeHeight;
    menu::set_host(m);
}

// -2 toggles, -1 shows the welcome card, 0..5 opens that tab; unlock: the Screen row (tests), -1 leaves it.
PRX_API void prx_menu_show(int32_t page, int32_t unlock) {
    if (page == -2) menu::toggle();
    else if (page == -1) menu::show_welcome();
    else if (page >= 0) {
        if (!g_menu.open) g_menu.toggle();
        g_menu.show(page);
    }
    if (unlock >= 0) g_menu.unlock(unlock != 0);
}

}
