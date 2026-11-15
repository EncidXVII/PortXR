// Parallax's VR core as a Godot 4.3+ GDExtension. The game loads it at runtime from the ParallaxVR folder beside its
// exe (GDExtensionManager.load_extension from our autoload): no change to the game's pack. Godot renders; the core
// owns the OpenXR session, as it does in Unity games. Direct3D 12 only (the core has no Vulkan backend yet).
#include "bridge.h"

#include <gdextension_interface.h>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace {
// The native resource (ID3D12Resource* under Direct3D 12) behind a RenderingServer texture. Godot's Direct3D 12 driver
// answers DRIVER_RESOURCE_TEXTURE with its own bookkeeping struct; the resource itself is the TEXTURE_VIEW answer.
void* native(const RID& texture) {
    RenderingServer* rs = RenderingServer::get_singleton();
    RenderingDevice* rd = rs->get_rendering_device();
    if (!rd || !texture.is_valid()) return nullptr;
    RID rdTexture = rs->texture_get_rd_texture(texture);
    if (!rdTexture.is_valid()) return nullptr;
    return (void*)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE_VIEW, rdTexture, 0);
}

bridge::Pose pose(const Transform3D& t) {
    Quaternion q = t.basis.get_rotation_quaternion();
    return {{(float)t.origin.x, (float)t.origin.y, (float)t.origin.z}, {(float)q.x, (float)q.y, (float)q.z, (float)q.w}};
}
}  // namespace

class ParallaxCore : public RefCounted {
    GDCLASS(ParallaxCore, RefCounted)

protected:
    static void _bind_methods() {
        ClassDB::bind_method(D_METHOD("start", "app"), &ParallaxCore::start);
        ClassDB::bind_method(D_METHOD("attach", "texture"), &ParallaxCore::attach);
        ClassDB::bind_method(D_METHOD("state"), &ParallaxCore::state);
        ClassDB::bind_method(D_METHOD("begin"), &ParallaxCore::begin);
        ClassDB::bind_method(D_METHOD("locate"), &ParallaxCore::locate);
        ClassDB::bind_method(D_METHOD("submit_quad", "texture", "pose", "size", "flip_y"), &ParallaxCore::submit_quad);
        ClassDB::bind_method(D_METHOD("submit_eyes", "left", "right", "flip_y"), &ParallaxCore::submit_eyes);
        ClassDB::bind_method(D_METHOD("native_texture", "texture"), &ParallaxCore::native_texture);
        ClassDB::bind_method(D_METHOD("poll_log"), &ParallaxCore::poll_log);
        ClassDB::bind_method(D_METHOD("open_settings", "path"), &ParallaxCore::open_settings);
        ClassDB::bind_method(D_METHOD("setting", "section", "key", "fallback"), &ParallaxCore::setting);
        ClassDB::bind_method(D_METHOD("set_setting", "section", "key", "value"), &ParallaxCore::set_setting);
        ClassDB::bind_method(D_METHOD("recenter"), &ParallaxCore::recenter);
        ClassDB::bind_method(D_METHOD("frame", "game"), &ParallaxCore::frame);
        ClassDB::bind_method(D_METHOD("set_window", "hwnd"), &ParallaxCore::set_window);
        ClassDB::bind_method(D_METHOD("pad"), &ParallaxCore::pad);
        ClassDB::bind_method(D_METHOD("test_controllers", "controllers"), &ParallaxCore::test_controllers);
        ClassDB::bind_method(D_METHOD("test_point", "u", "v", "screen"), &ParallaxCore::test_point, DEFVAL(false));
        ClassDB::bind_method(D_METHOD("set_menu_host", "host"), &ParallaxCore::set_menu_host);
        ClassDB::bind_method(D_METHOD("toggle_menu"), &ParallaxCore::toggle_menu);
        ClassDB::bind_method(D_METHOD("capture", "dir"), &ParallaxCore::capture);
        ClassDB::bind_method(D_METHOD("submit_view", "left", "right", "ui", "ui_aspect"), &ParallaxCore::submit_view);
    }

    bridge::Pose eyes_[2]{};
    bridge::Fov fovs_[2]{};
    bool located_ = false;

public:
    // {ok, width, height}: the headset's recommended eye size.
    Dictionary start(const String& app) {
        int w = 0, h = 0;
        bool ok = bridge::start(app.utf8().get_data(), w, h);
        Dictionary d;
        d["ok"] = ok;
        d["width"] = w;
        d["height"] = h;
        return d;
    }

    bool attach(const RID& texture) {
        void* res = native(texture);
        if (!res) return false;
        bridge::attach(res);
        return true;
    }

    int state() { return bridge::state(); }

    void begin() { bridge::begin(); }

    // [left, right]: each {transform: Transform3D (tracking space), fov: [left, right, up, down] radians}; empty
    // while the headset has no pose.
    Array locate() {
        Array out;
        located_ = bridge::locate(eyes_, fovs_);
        if (!located_) return out;
        for (int e = 0; e < 2; ++e) {
            Dictionary d;
            const bridge::Pose& p = eyes_[e];
            d["transform"] = Transform3D(Basis(Quaternion(p.q[0], p.q[1], p.q[2], p.q[3])), Vector3(p.p[0], p.p[1], p.p[2]));
            Array fov;
            fov.append(fovs_[e].left);
            fov.append(fovs_[e].right);
            fov.append(fovs_[e].up);
            fov.append(fovs_[e].down);
            d["fov"] = fov;
            out.append(d);
        }
        return out;
    }

    bool submit_quad(const RID& texture, const Transform3D& at, const Vector2& size, bool flipY) {
        void* res = native(texture);
        if (!res) return false;
        bridge::submit_quad(res, pose(at), (float)size.x, (float)size.y, flipY);
        return true;
    }

    // The eye images rendered at the poses the last locate() returned.
    bool submit_eyes(const RID& left, const RID& right, bool flipY) {
        void *l = native(left), *r = native(right);
        if (!l || !r || !located_) return false;
        bridge::submit_eyes(l, r, eyes_, fovs_, flipY);
        return true;
    }

    int64_t native_texture(const RID& texture) { return (int64_t)native(texture); }

    String poll_log() { return String::utf8(bridge::poll_log().c_str()); }

    // --- The shared behaviour: settings and the view, computed by the core for every engine ---

    void open_settings(const String& path) { bridge::settings_open(std::wstring((const wchar_t*)path.utf16().get_data())); }
    String setting(const String& section, const String& key, const String& fallback) {
        return String::utf8(bridge::setting(section.utf8().get_data(), key.utf8().get_data(), fallback.utf8().get_data()).c_str());
    }
    void set_setting(const String& section, const String& key, const String& value) {
        bridge::set_setting(section.utf8().get_data(), key.utf8().get_data(), value.utf8().get_data());
    }
    void recenter() { bridge::recenter(); }

    // One frame of the shared behaviour. game: {camera: Transform3D (absent: none), camera_key, cursor_free,
    // pad_game, carried, menu_open}. Returns {eyes: [{transform, fov}, {..}], aiming, menu_toggle}; empty while the
    // headset has no pose.
    Dictionary frame(const Dictionary& game) {
        bridge::Game g;
        g.hasCamera = game.has("camera");
        if (g.hasCamera) g.camera = pose(game["camera"]);
        g.cameraKey = String(game.get("camera_key", "")).utf8().get_data();
        g.cursorFree = game.get("cursor_free", false);
        g.padGame = game.get("pad_game", false);
        g.carried = game.get("carried", false);
        g.menuOpen = game.get("menu_open", false);
        g.uiAspect = (float)(double)game.get("ui_aspect", 16.0 / 9.0);
        g.fovY = (float)(double)game.get("fov_y", 1.0);
        g.nearClip = (float)(double)game.get("near", 0.05);
        g.focus = (float)(double)game.get("focus", 0.0);
        bridge::Frame f;
        Dictionary out;
        if (!bridge::frame(g, f)) return out;
        Array eyes;
        for (int e = 0; e < 2; ++e) {
            Dictionary d;
            const bridge::Pose& p = f.eyes[e];
            d["transform"] = Transform3D(Basis(Quaternion(p.q[0], p.q[1], p.q[2], p.q[3])), Vector3(p.p[0], p.p[1], p.p[2]));
            Array fov;
            fov.append(f.fovs[e].left);
            fov.append(f.fovs[e].right);
            fov.append(f.fovs[e].up);
            fov.append(f.fovs[e].down);
            d["fov"] = fov;
            eyes.append(d);
        }
        out["eyes"] = eyes;
        out["aiming"] = f.aiming;
        out["menu_toggle"] = (f.events & 1) != 0;
        out["menu_open"] = f.menuOpen;
        out["mode"] = f.mode;
        out["handles"] = f.handles;
        if (f.mode == 1) {
            Array window;
            for (int e = 0; e < 2; ++e) {
                Dictionary d;
                d["offset"] = f.windowOffset[e];
                Array fov;
                fov.append(f.windowFov[e].left);
                fov.append(f.windowFov[e].right);
                fov.append(f.windowFov[e].up);
                fov.append(f.windowFov[e].down);
                d["fov"] = fov;
                window.append(d);
            }
            out["window"] = window;
        }
        if (f.quit) out["quit"] = true;
        if (f.renderScale) out["render_scale"] = true;
        if (f.cameraPicked) out["camera_picked"] = String::utf8(f.camera.c_str());
        if (f.pointer) {
            out["pointer"] = Vector2(f.pointerU, f.pointerV);
            out["pointer_down"] = f.pointerDown;
        }
        return out;
    }

    // Tests: {lx, ly, rx, ry, lt, rt, buttons} instead of the controllers; an empty dictionary restores them.
    void test_controllers(const Dictionary& c) {
        bridge::test_controllers(!c.is_empty(), c.get("lx", 0.0), c.get("ly", 0.0), c.get("rx", 0.0), c.get("ry", 0.0),
                                 c.get("lt", 0.0), c.get("rt", 0.0), (uint32_t)(int64_t)c.get("buttons", 0));
    }

    void test_point(float u, float v, bool screen) { bridge::test_point(u, v, screen); }

    void toggle_menu() { bridge::toggle_menu(); }
    void capture(const String& dir) { bridge::capture(std::wstring((const wchar_t*)dir.utf16().get_data())); }

    // What the game offers the VR menu: {product, game_dir, cameras: [names], current_camera, eye_width, eye_height}.
    void set_menu_host(const Dictionary& h) {
        bridge::MenuHost m;
        m.product = String(h.get("product", "")).utf8().get_data();
        m.gameDir = std::wstring((const wchar_t*)String(h.get("game_dir", "")).utf16().get_data());
        Array cams = h.get("cameras", Array());
        for (int i = 0; i < cams.size(); ++i) m.cameras.push_back(String(cams[i]).utf8().get_data());
        m.currentCamera = String(h.get("current_camera", "")).utf8().get_data();
        m.eyeWidth = (int)(int64_t)h.get("eye_width", 0);
        m.eyeHeight = (int)(int64_t)h.get("eye_height", 0);
        bridge::set_menu_host(m);
    }

    void set_window(int64_t hwnd) { bridge::set_window((void*)hwnd); }

    // The emulated gamepad ({buttons: XInput bits, lt, rt 0..1, lx, ly, rx, ry -1..1}); empty while gamepad mode is off.
    Dictionary pad() {
        bridge::Pad p{};
        Dictionary d;
        if (!bridge::pad(p)) return d;
        d["buttons"] = (int64_t)p.buttons;
        d["lt"] = p.leftTrigger / 255.0;
        d["rt"] = p.rightTrigger / 255.0;
        d["lx"] = p.lx / 32767.0;
        d["ly"] = p.ly / 32767.0;
        d["rx"] = p.rx / 32767.0;
        d["ry"] = p.ry / 32767.0;
        return d;
    }

    bool submit_view(const RID& left, const RID& right, const RID& ui, float uiAspect) {
        void *l = native(left), *r = native(right), *u = ui.is_valid() ? native(ui) : nullptr;
        if (!l || !r) return false;
        bridge::submit_view(l, r, u, uiAspect, false);
        return true;
    }
};

namespace {
void initialize(ModuleInitializationLevel level) {
    if (level == MODULE_INITIALIZATION_LEVEL_SCENE) GDREGISTER_CLASS(ParallaxCore);
}
void uninitialize(ModuleInitializationLevel) {}
}  // namespace

extern "C" GDExtensionBool GDE_EXPORT parallax_godot_init(GDExtensionInterfaceGetProcAddress getProcAddress,
                                                          GDExtensionClassLibraryPtr library,
                                                          GDExtensionInitialization* init) {
    GDExtensionBinding::InitObject object(getProcAddress, library, init);
    object.register_initializer(initialize);
    object.register_terminator(uninitialize);
    object.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return object.init();
}
