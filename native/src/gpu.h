#pragma once
#include "common.h"
#include <dxgi1_4.h>

// Drawn over an eye image. vignette: 0 = none, up to 1 = strongest. beam: segment from (x0, y0) to (x1, y1) in the
// image's UV (top-left origin), beamWidth in UV of the image height; 0 draws no beam.
struct Overlay {
    float vignette = 0;
    float beam[4] = {};
    float beamWidth = 0;
    // Watch face: centre (u, v), radius in UV of the image height (0 draws none), lit 0..1; and how much it shows.
    float watch[4] = {};
    float watchAlpha = 1;
    // Near-black pixels made see-through (UI panel whose alpha the game filled in).
    float key = 0;
    float keep[4] = {};  // UV rect left unkeyed
    // Aim reticle: centre (u, v) and radius in UV of the image height; 0 draws none.
    float aim[4] = {};
    // UV rect (u0, v0, u1, v1) of the drawn image left transparent; all zero leaves none.
    float hole[4] = {};
    // UV rect of the source, as displayed, drawn over the whole target; all zero draws the whole source.
    float crop[4] = {};
    // Theme accent (sRGB) for the beam, reticle and lit watch.
    float accent[3] = {0.776f, 1.f, 0.102f};
    // Copy the drawn image for an opacity measurement, read back without stalling on a later draw.
    bool probe = false;
};

// Everything that touches the game's graphics device. One backend per API; all calls come from Unity's render
// thread except present(), which runs on whichever thread the game presents from.
class Gpu {
public:
    virtual ~Gpu() = default;
    // False while the backend still waits for something it needs (D3D12: the game's command queue).
    virtual bool ready() = 0;
    // Checks the adapter against the runtime and creates the session.
    virtual bool create_session(XrSession* session) = 0;
    virtual bool bind_images(XrSwapchain swapchain, int64_t format) = 0;
    virtual void forget_images(XrSwapchain swapchain) = 0;
    virtual bool size(void* texture, int& w, int& h) = 0;
    // Draws a Unity texture into an acquired swapchain image; a non-null capture path writes it out as BMP.
    // Eye layers only: comfort vignette and the laser beam drawn over the image.
    virtual void draw(void* texture, XrSwapchain swapchain, uint32_t index, bool flipY, const Overlay& overlay,
                      const wchar_t* capture) = 0;
    // Unity < 2019.1 UI fallback: grab the backbuffer still bound at end of frame.
    virtual void capture_backbuffer() = 0;
    // Texture holding the last backbuffer grab (end-of-frame event or Present), or null.
    virtual void* backbuffer_copy() = 0;
    // Eye texture Present draws into the game window; null leaves the window alone.
    virtual void set_mirror(void* texture) = 0;
    virtual void present(IDXGISwapChain* swapchain) = 0;
    // Share of opaque pixels in the latest probed image that finished reading back, else -1; each result once.
    virtual float opaque_fraction() = 0;
    // A texture of the core's own (the VR menu) filled from CPU pixels: RGBA8, sRGB-encoded, premultiplied, rows of
    // w * 4 bytes. `texture` is the one returned last time (null at first); a new one comes back when the size
    // changes. Drawn from like any other texture; render thread only.
    virtual void* upload(void* texture, const void* rgba, int w, int h) = 0;
};

// A zero runtime LUID (SteamVR null driver) accepts any adapter; a mismatch is logged.
bool adapter_matches(LUID game, LUID runtime);
Gpu* gpu_create_d3d11(ID3D11Device* device);
Gpu* gpu_create_d3d12(ID3D12Device* device);
// Hooks IDXGISwapChain::Present/Present1 through a throwaway swapchain made on the game's device (or queue).
bool gpu_hook_present(IUnknown* deviceOrQueue);
void gpu_set_present_target(Gpu* gpu);
// Engines the core drives from Present (Unreal): called after the backend's own Present work, outside its lock.
extern void (*gpu_present_observer)(IDXGISwapChain*);
// D3D11: grab the swapchain's image at every Present into backbuffer_copy() (D3D12 always does). No vsync: the headset
// paces frames, and a 60 Hz desktop would halve them.
extern std::atomic<bool> gpu_grab_present, gpu_no_vsync;
// Hooks ID3D12CommandQueue::ExecuteCommandLists so Present can learn which queue the game renders on.
bool gpu_hook_queue(ID3D12CommandQueue* probe);

extern const char kBlitShader[];
extern const size_t kBlitShaderSize;
struct BlitParams {
    float flip, decode, encode, vignette, scaleX, scaleY, offsetX, offsetY;
    float beam[4], beamWidth, aspect, dstLinear, key;
    float watch[4];
    float keep[4];
    float aim[4];
    float hole[4];
    float watchLook[4];  // x: the watch's opacity; yzw: the theme accent (sRGB)
};
void gpu_overlay(BlitParams& p, const Overlay& o, float aspect);
// Format to sample `f` with; `linear` reports whether sampled values are linear light (sRGB views decode in hardware,
// float targets hold linear values). Typeless 8-bit sources are read raw.
DXGI_FORMAT gpu_view_format(DXGI_FORMAT f, bool& linear);
bool gpu_format_linear(DXGI_FORMAT f);
BlitParams gpu_params(bool flipY, bool srcLinear, bool dstLinear);
// Crop that fills a window of aspect `screen` with the middle of an eye image of aspect `eye`.
void gpu_mirror_crop(BlitParams& p, float eye, float screen);
// Share of pixels with alpha over one half, on a sparse grid; -1 for formats it cannot read.
float gpu_opaque_fraction(const uint8_t* data, UINT w, UINT h, UINT pitch, DXGI_FORMAT format);
void gpu_write_bmp(const wchar_t* path, const uint8_t* data, UINT w, UINT h, UINT pitch, DXGI_FORMAT format);
