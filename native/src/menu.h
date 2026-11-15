#pragma once
// The VR menu, shared by every engine: the Unity plugin's Menu.cs (tabs, rows, look and motion) drawn by the core on a
// CPU canvas and shown as its own headset layer. It reads and writes the same settings files as the app; what only an
// engine can do (pick a camera, hide objects, quit) comes back to the adapter as requests.
#include <cstdint>
#include <string>
#include <vector>

namespace menu {
// What the engine adapter offers the menu.
struct Host {
    std::string product;                 // the game's name (Now playing)
    std::wstring gameDir;                // the game's folder (Library: which entry is this game)
    std::vector<std::string> cameras;    // Game tab: cameras the view can follow (empty: no camera picker)
    std::string currentCamera;
    bool hideObjects = false;            // Game tab: the Hide objects tool
    int hidden = 0;
    bool aimExtras = false;              // Game tab: Crosshair and Weapon in hand
    bool uiBlack = false;                // View tab: UI black (engines whose UI alpha needs keying)
    int eyeWidth = 0, eyeHeight = 0;     // Diagnostics
};

// What the adapter should do this frame.
struct Requests {
    bool quit = false;           // Quit game (and, with `next`, Parallax.exe starts that game after)
    bool recenter = false;
    bool hideObjects = false, showHidden = false;
    bool renderScale = false;    // Resolution changed: make the eye targets again
    bool cameraPicked = false;
    std::string camera;          // "" is automatic
};

void set_host(const Host& host);
bool is_open();
void toggle();
// The first-run card instead of the tabs.
void show_welcome();
// One frame: the pointer on the menu (u, v from the top-left; pointing false when off it), the right trigger.
Requests frame(bool pointing, float u, float v, float trigger, float dt);
// The menu's pixels (premultiplied RGBA8), w x h; `changed` when they differ from the last call's.
const uint8_t* pixels(int& w, int& h, bool& changed);
// Width / height of the menu image (its quad's shape).
float aspect();
}  // namespace menu
