#pragma once
#include "common.h"
#include <dxgi1_4.h>

// Drawn over an eye image. vignette: 0 = none, up to 1 = strongest. beam: segment from (x0, y0) to (x1, y1) in the
// image's UV (top-left origin), beamWidth in UV of the image height; 0 draws no beam.
struct Overlay {
    float vignette = 0;
    float beam[4] = {};
    float beamWidth = 0;
    // Watch face: centre (u, v), radius in UV of the image height (0 draws none), lit 0 or 1.
    float watch[4] = {};
    // Near-black pixels made see-through (UI panel whose alpha the game filled in).
    float key = 0;
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
};

// A zero runtime LUID (SteamVR null driver) accepts any adapter; a mismatch is logged.
bool adapter_matches(LUID game, LUID runtime);
Gpu* gpu_create_d3d11(ID3D11Device* device);
Gpu* gpu_create_d3d12(ID3D12Device* device);
// Hooks IDXGISwapChain::Present/Present1 through a throwaway swapchain made on the game's device (or queue).
bool gpu_hook_present(IUnknown* deviceOrQueue);
void gpu_set_present_target(Gpu* gpu);
// Hooks ID3D12CommandQueue::ExecuteCommandLists so Present can learn which queue the game renders on.
bool gpu_hook_queue(ID3D12CommandQueue* probe);

extern const char kBlitShader[];
extern const size_t kBlitShaderSize;
struct BlitParams {
    float flip, decode, encode, vignette, scaleX, scaleY, offsetX, offsetY;
    float beam[4], beamWidth, aspect, dstLinear, key;
    float watch[4];
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
